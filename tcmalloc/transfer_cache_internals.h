// Copyright 2020 The TCMalloc Authors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef TCMALLOC_TRANSFER_CACHE_INTERNAL_H_
#define TCMALLOC_TRANSFER_CACHE_INTERNAL_H_

#include <sched.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __x86_64__
#include <emmintrin.h>
#include <xmmintrin.h>
#endif

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <utility>

#include "absl/base/attributes.h"
#include "absl/base/const_init.h"
#include "absl/base/internal/spinlock.h"
#include "absl/base/macros.h"
#include "absl/base/optimization.h"
#include "absl/base/thread_annotations.h"
#include "absl/types/span.h"
#include "tcmalloc/central_freelist.h"
#include "tcmalloc/common.h"
#include "tcmalloc/experiment.h"
#include "tcmalloc/internal/logging.h"
#include "tcmalloc/tracking.h"

namespace tcmalloc::internal_transfer_cache {

struct alignas(8) SizeInfo {
  int32_t used;
  int32_t capacity;
};

// TransferCache is used to cache transfers of
// sizemap.num_objects_to_move(size_class) back and forth between
// thread caches and the central cache for a given size class.
template <typename CentralFreeList, typename TransferCacheManager>
class TransferCache {
 public:
  using Manager = TransferCacheManager;
  using FreeList = CentralFreeList;

  static constexpr int kMaxCapacityInBatches = 64;
  static constexpr int kInitialCapacityInBatches = 16;

  constexpr explicit TransferCache(Manager *owner) : TransferCache(owner, 0) {}

  // C++11 has complex rules for direct initialization of an array of aggregate
  // types that are not copy constructible.  The int parameters allows us to do
  // two distinct things at the same time:
  //  - have an implicit constructor (one arg implicit ctors are dangerous)
  //  - build an array of these in an arg pack expansion without a comma
  //    operator trick
  constexpr TransferCache(Manager *owner, int)
      : owner_(owner),
        lock_(absl::kConstInit, absl::base_internal::SCHEDULE_KERNEL_ONLY),
        max_capacity_(0),
        slot_info_{},
        slots_(nullptr),
        freelist_do_not_access_directly_(),
        arbitrary_transfer_(false) {}

  TransferCache(const TransferCache &) = delete;
  TransferCache &operator=(const TransferCache &) = delete;

  // We require the pageheap_lock with some templates, but not in tests, so the
  // thread safety analysis breaks pretty hard here.
  void Init(size_t cl) ABSL_NO_THREAD_SAFETY_ANALYSIS {
    freelist().Init(cl);
    absl::base_internal::SpinLockHolder h(&lock_);

    // We need at least 2 slots to store list head and tail.
    ASSERT(kMinObjectsToMove >= 2);

    // Cache this value, for performance.
    arbitrary_transfer_ =
        IsExperimentActive(Experiment::TCMALLOC_ARBITRARY_TRANSFER_CACHE);

    slots_ = nullptr;
    max_capacity_ = 0;
    SizeInfo info = {0, 0};

    if (cl > 0) {
      // Limit the maximum size of the cache based on the size class.  If this
      // is not done, large size class objects will consume a lot of memory if
      // they just sit in the transfer cache.
      size_t bytes = Manager::class_to_size(cl);
      size_t objs_to_move = Manager::num_objects_to_move(cl);
      ASSERT(objs_to_move > 0 && bytes > 0);

      // Starting point for the maximum number of entries in the transfer cache.
      // This actual maximum for a given size class may be lower than this
      // maximum value.
      max_capacity_ = kMaxCapacityInBatches * objs_to_move;
      // A transfer cache freelist can have anywhere from 0 to
      // max_capacity_ slots to put link list chains into.
      info.capacity = kInitialCapacityInBatches * objs_to_move;

      // Limit each size class cache to at most 1MB of objects or one entry,
      // whichever is greater. Total transfer cache memory used across all
      // size classes then can't be greater than approximately
      // 1MB * kMaxNumTransferEntries.
      max_capacity_ = std::min<size_t>(
          max_capacity_,
          std::max<size_t>(
              objs_to_move,
              (1024 * 1024) / (bytes * objs_to_move) * objs_to_move));
      info.capacity = std::min(info.capacity, max_capacity_);
      slots_ = reinterpret_cast<void **>(
          owner_->Alloc(max_capacity_ * sizeof(void *)));
    }
    SetSlotInfo(info);
  }

  // These methods all do internal locking.

  // Insert the specified batch into the transfer cache.  N is the number of
  // elements in the range.  RemoveRange() is the opposite operation.
  void InsertRange(absl::Span<void *> batch, int N) ABSL_LOCKS_EXCLUDED(lock_) {
    const int B = Manager::num_objects_to_move(size_class());
    ASSERT(0 < N && N <= B);
    auto info = slot_info_.load(std::memory_order_relaxed);
    if (N == B && info.used + N <= max_capacity_) {
      absl::base_internal::SpinLockHolder h(&lock_);
      if (MakeCacheSpace(N)) {
        // MakeCacheSpace can drop the lock, so refetch
        info = slot_info_.load(std::memory_order_relaxed);
        info.used += N;
        SetSlotInfo(info);

        void **entry = GetSlot(info.used - N);
        memcpy(entry, batch.data(), sizeof(void *) * N);
        tracking::Report(kTCInsertHit, size_class(), 1);
        return;
      }
    } else if (arbitrary_transfer_) {
      absl::base_internal::SpinLockHolder h(&lock_);
      MakeCacheSpace(N);
      // MakeCacheSpace can drop the lock, so refetch
      info = slot_info_.load(std::memory_order_relaxed);
      int unused = info.capacity - info.used;
      if (N < unused) {
        info.used += N;
        SetSlotInfo(info);
        void **entry = GetSlot(info.used - N);
        memcpy(entry, batch.data(), sizeof(void *) * N);
        tracking::Report(kTCInsertHit, size_class(), 1);
        return;
      }
      // We could not fit the entire batch into the transfer cache
      // so send the batch to the freelist and also take some elements from
      // the transfer cache so that we amortise the cost of accessing spans
      // in the freelist. Only do this if caller has sufficient space in
      // batch.
      // First of all fill up the rest of the batch with elements from the
      // transfer cache.
      int extra = B - N;
      if (N > 1 && extra > 0 && info.used > 0 && batch.size() >= B) {
        // Take at most all the objects present
        extra = std::min(extra, info.used);
        ASSERT(extra + N <= kMaxObjectsToMove);
        info.used -= extra;
        SetSlotInfo(info);

        void **entry = GetSlot(info.used);
        memcpy(batch.data() + N, entry, sizeof(void *) * extra);
        N += extra;
#ifndef NDEBUG
        int rest = batch.size() - N - 1;
        if (rest > 0) {
          memset(batch.data() + N, 0x3f, rest * sizeof(void *));
        }
#endif
      }
    }
    tracking::Report(kTCInsertMiss, size_class(), 1);
    freelist().InsertRange(batch.data(), N);
  }

  // Returns the actual number of fetched elements and stores elements in the
  // batch.
  int RemoveRange(void **batch, int N) ABSL_LOCKS_EXCLUDED(lock_) {
    ASSERT(N > 0);
    const int B = Manager::num_objects_to_move(size_class());
    int fetch = 0;
    auto info = slot_info_.load(std::memory_order_relaxed);
    if (N == B && info.used >= N) {
      absl::base_internal::SpinLockHolder h(&lock_);
      // Refetch with the lock
      info = slot_info_.load(std::memory_order_relaxed);
      if (info.used >= N) {
        info.used -= N;
        SetSlotInfo(info);
        void **entry = GetSlot(info.used);
        memcpy(batch, entry, sizeof(void *) * N);
        tracking::Report(kTCRemoveHit, size_class(), 1);
        return N;
      }
    } else if (arbitrary_transfer_ && info.used >= 0) {
      absl::base_internal::SpinLockHolder h(&lock_);
      // Refetch with the lock
      info = slot_info_.load(std::memory_order_relaxed);

      fetch = std::min(N, info.used);
      info.used -= fetch;
      SetSlotInfo(info);
      void **entry = GetSlot(info.used);
      memcpy(batch, entry, sizeof(void *) * fetch);
      tracking::Report(kTCRemoveHit, size_class(), 1);
      if (fetch == N) return N;
    }
    tracking::Report(kTCRemoveMiss, size_class(), 1);
    return freelist().RemoveRange(batch + fetch, N - fetch) + fetch;
  }

  // Returns the number of free objects in the central cache.
  size_t central_length() { return freelist().length(); }

  // Returns the number of free objects in the transfer cache.
  size_t tc_length() {
    return static_cast<size_t>(slot_info_.load(std::memory_order_relaxed).used);
  }

  // Returns the number of spans allocated and deallocated from the CFL
  SpanStats GetSpanStats() const { return freelist().GetSpanStats(); }

  // Returns the memory overhead (internal fragmentation) attributable
  // to the freelist.  This is memory lost when the size of elements
  // in a freelist doesn't exactly divide the page-size (an 8192-byte
  // page full of 5-byte objects would have 2 bytes memory overhead).
  size_t OverheadBytes() { return freelist().OverheadBytes(); }

  SizeInfo GetSlotInfo() const {
    return slot_info_.load(std::memory_order_relaxed);
  }

  // REQUIRES: lock is held.
  // Tries to make room for a batch.  If the cache is full it will try to expand
  // it at the cost of some other cache size.  Return false if there is no
  // space.
  bool MakeCacheSpace(int N) ABSL_EXCLUSIVE_LOCKS_REQUIRED(lock_) {
    auto info = slot_info_.load(std::memory_order_relaxed);
    // Is there room in the cache?
    if (info.used + N <= info.capacity) return true;
    // Check if we can expand this cache?
    if (info.capacity + N > max_capacity_) return false;

    int to_evict = owner_->DetermineSizeClassToEvict();
    if (to_evict == size_class()) return false;

    // Release the held lock before the other instance tries to grab its lock.
    lock_.Unlock();
    bool made_space = owner_->ShrinkCache(to_evict);
    lock_.Lock();

    if (!made_space) return false;

    // Succeeded in evicting, we're going to make our cache larger.  However, we
    // may have dropped and re-acquired the lock, so the cache_size may have
    // changed.  Therefore, check and verify that it is still OK to increase the
    // cache_size.
    info = slot_info_.load(std::memory_order_relaxed);
    if (info.capacity + N > max_capacity_) return false;
    info.capacity += N;
    SetSlotInfo(info);
    return true;
  }

  bool HasSpareCapacity() {
    int n = Manager::num_objects_to_move(size_class());
    auto info = GetSlotInfo();
    return info.capacity - info.used >= n;
  }

  bool GrowCache() {
    CHECK_CONDITION(false && "unused");
    return true;
  }

  // REQUIRES: lock_ is *not* held.
  // Tries to shrink the Cache.  Return false if it failed to shrink the cache.
  // Decreases cache_slots_ on success.
  bool ShrinkCache() ABSL_LOCKS_EXCLUDED(lock_) {
    int N = Manager::num_objects_to_move(size_class());

    void *to_free[kMaxObjectsToMove];
    int num_to_free;
    {
      absl::base_internal::SpinLockHolder h(&lock_);
      auto info = slot_info_.load(std::memory_order_relaxed);
      if (info.capacity == 0) return false;
      if (!arbitrary_transfer_ && info.capacity < N) return false;

      N = std::min(N, info.capacity);
      int unused = info.capacity - info.used;
      if (N <= unused) {
        info.capacity -= N;
        SetSlotInfo(info);
        return true;
      }

      num_to_free = N - unused;
      info.capacity -= N;
      info.used -= num_to_free;
      SetSlotInfo(info);

      // Our internal slot array may get overwritten as soon as we drop the
      // lock, so copy the items to free to an on stack buffer.
      memcpy(to_free, GetSlot(info.used), sizeof(void *) * num_to_free);
    }

    // Access the freelist without holding the lock.
    freelist().InsertRange(to_free, num_to_free);
    return true;
  }

  // This is a thin wrapper for the CentralFreeList.  It is intended to ensure
  // that we are not holding lock_ when we access it.
  ABSL_ATTRIBUTE_ALWAYS_INLINE FreeList &freelist() ABSL_LOCKS_EXCLUDED(lock_) {
    return freelist_do_not_access_directly_;
  }

  // The const version of the wrapper, needed to call stats on
  ABSL_ATTRIBUTE_ALWAYS_INLINE const FreeList &freelist() const
      ABSL_LOCKS_EXCLUDED(lock_) {
    return freelist_do_not_access_directly_;
  }

 private:
  // Returns first object of the i-th slot.
  void **GetSlot(size_t i) ABSL_EXCLUSIVE_LOCKS_REQUIRED(lock_) {
    return slots_ + i;
  }

  void SetSlotInfo(SizeInfo info) {
    ASSERT(0 <= info.used);
    ASSERT(info.used <= info.capacity);
    ASSERT(info.capacity <= max_capacity_);
    slot_info_.store(info, std::memory_order_relaxed);
  }

  Manager *const owner_;

  // This lock protects all the data members.  used_slots_ and cache_slots_
  // may be looked at without holding the lock.
  absl::base_internal::SpinLock lock_;

  // Maximum size of the cache for a given size class. (immutable after Init())
  int32_t max_capacity_;

  // Number of currently used and available cached entries in slots_.  This
  // variable is updated under a lock but can be read without one.
  // INVARIANT: [0 <= slot_info_.used <= slot_info.capacity <= max_cache_slots_]
  std::atomic<SizeInfo> slot_info_;

  // Pointer to array of free objects.  Use GetSlot() to get pointers to
  // entries.
  void **slots_ ABSL_GUARDED_BY(lock_);

  size_t size_class() const {
    return freelist_do_not_access_directly_.size_class();
  }

  FreeList freelist_do_not_access_directly_;

  // Cached value of IsExperimentActive(Experiment::TCMALLOC_ARBITRARY_TRANSFER)
  bool arbitrary_transfer_;
} ABSL_CACHELINE_ALIGNED;

// Lock free transfer cache based on LMAX disruptor pattern.
//
// Use `GetSlot()` to get pointers to entries.
// Pointer to array of `max_capacity_ + 1` free objects which forms a circular
// buffer.
//
// Various offsets have a strict ordering invariant:
//   * tail_committed <= tail <= head_committed <= head (viewed circularly).
//   * Empty when tail_committed == head_committed.
//   * Full when tail_committed - 1 == head_committed.
//
// When there are no active threads,
//   *  `tail_committed == tail`
//   *  `head_committed == head`
//
// In terms of atomic sequencing, only committed variables hold dependencies.
// - `RemoveRange` acquires `head_committed` and releases `tail_committed`
// - `InsertRange` acquires `tail_committed` and releases `head_committed`
//
// For example:
//
// The queue is idle with some data in it and a batch size of 3.
//   +--------------------------------------------------------------------+
//   |  |  |  |xx|xx|xx|xx|xx|xx|xx|xx|xx|xx|xx|  |  |  |  |  |  |  |  |  |
//   +--------------------------------------------------------------------+
//             ^                                ^
//             |                                |
//         tail_committed/tail              head_committed/head
//
// Four threads arrive (near simultaneously). Two are concurrently removing
// batches (c1, c2), and two threads are inserting batches (p1, p2).
//   +--------------------------------------------------------------------+
//   |  |  |  |c1|c1|c1|c2|c2|c2|xx|xx|xx|xx|xx|p1|p1|p1|p2|p2|p2|  |  |  |
//   +--------------------------------------------------------------------+
//             ^        ^        ^              ^        ^        ^
//             |        |        |              |        |        |
//             | c1 commit point |              | p1 commit point |
//         tail_committed        tail       head_committed        head
//
// At this point c2 and p2, cannot commit until c1 or p1 commit respectively.
// Let's say c1 commits:
//   +--------------------------------------------------------------------+
//   |  |  |  |  |  |  |c2|c2|c2|xx|xx|xx|xx|xx|p1|p1|p1|p2|p2|p2|  |  |  |
//   +--------------------------------------------------------------------+
//                      ^        ^              ^        ^        ^
//                      |        |              |        |        |
//                      |        |              | p1 commit point |
//         tail_committed        tail       head_committed        head
//
// Now, c2 can commit its batch:
//   +--------------------------------------------------------------------+
//   |  |  |  |  |  |  |  |  |  |xx|xx|xx|xx|xx|p1|p1|p1|p2|p2|p2|  |  |  |
//   +--------------------------------------------------------------------+
//                               ^              ^        ^        ^
//                               |              |        |        |
//                               |              | p1 commit point |
//                tail_committed/tail       head_committed        head
//
// In parallel, p1 could have completed and committed its batch:
//   +--------------------------------------------------------------------+
//   |  |  |  |  |  |  |  |  |  |xx|xx|xx|xx|xx|xx|xx|xx|p2|p2|p2|  |  |  |
//   +--------------------------------------------------------------------+
//                               ^                       ^        ^
//                               |                       |        |
//                tail_committed/tail       head_committed        head
//
// At which point p2 can commit:
//   +--------------------------------------------------------------------+
//   |  |  |  |  |  |  |  |  |  |xx|xx|xx|xx|xx|xx|xx|xx|xx|xx|xx|  |  |  |
//   +--------------------------------------------------------------------+
//                               ^                                ^
//                               |                                |
//                tail_committed/tail              head_committed/head
template <typename CentralFreeList, typename TransferCacheManager>
class LockFreeTransferCache {
 public:
  using Manager = TransferCacheManager;
  using FreeList = CentralFreeList;
  static constexpr int kMaxCapacityInBatches = 64;
  static constexpr int kInitialCapacityInBatches = 16;

  constexpr explicit LockFreeTransferCache(Manager *owner)
      : LockFreeTransferCache(owner, 0) {}

  // C++11 has complex rules for direct initialization of an array of aggregate
  // types that are not copy constructible.  The int parameters allows us to do
  // two distinct things at the same time:
  //  - have an implicit constructor (one arg implicit ctors are dangerous)
  //  - build an array of these in an arg pack expansion without a comma
  //    operator trick
  constexpr LockFreeTransferCache(Manager *owner, int)
      : owner_(owner),
        slots_(nullptr),
        freelist_(),
        max_capacity_(0),
        capacity_(),
        head_(),
        head_committed_(),
        tail_(),
        tail_committed_() {}

  LockFreeTransferCache(const LockFreeTransferCache &) = delete;
  LockFreeTransferCache &operator=(const LockFreeTransferCache &) = delete;

  // We require the pageheap_lock with some templates, but not in tests, so the
  // thread safety analysis breaks pretty hard here.
  void Init(size_t cl) ABSL_NO_THREAD_SAFETY_ANALYSIS {
    freelist_.Init(cl);

    // We need at least 2 slots to store list head and tail.
    ASSERT(kMinObjectsToMove >= 2);

    slots_ = nullptr;
    max_capacity_ = 0;
    int32_t capacity = 0;

    if (cl > 0) {
      // Limit the maximum size of the cache based on the size class.  If this
      // is not done, large size class objects will consume a lot of memory if
      // they just sit in the transfer cache.
      size_t bytes = Manager::class_to_size(cl);
      batch_size_ = Manager::num_objects_to_move(cl);
      ASSERT(batch_size_ > 0 && bytes > 0);

      // Starting point for the maximum number of entries in the transfer cache.
      // This actual maximum for a given size class may be lower than this
      // maximum value.
      max_capacity_ = kMaxCapacityInBatches * batch_size_;
      // A transfer cache freelist can have anywhere from 0 to
      // max_capacity_ slots to put link list chains into.
      capacity = kInitialCapacityInBatches * batch_size_;

      // Limit each size class cache to at most 1MB of objects or one entry,
      // whichever is greater. Total transfer cache memory used across all
      // size classes then can't be greater than approximately
      // 1MB * kMaxNumTransferEntries.
      max_capacity_ = std::min<size_t>(
          max_capacity_,
          std::max<size_t>(batch_size_, (1024 * 1024) / (bytes * batch_size_) *
                                            batch_size_));
      capacity = std::min(capacity, max_capacity_);
      capacity_.store(capacity, std::memory_order_relaxed);
      slots_ = reinterpret_cast<void **>(
          owner_->Alloc(slots_size() * sizeof(void *)));
    }
  }

  // Insert the specified batch into the transfer cache.  N is the number of
  // elements in the range.  RemoveRange() is the opposite operation.
  void InsertRange(absl::Span<void *> batch, int N) {
    ASSERT(0 < N && N <= batch_size_);
    int32_t new_h;
    int32_t old_h = head_.load(std::memory_order_relaxed);
    do {
      int32_t t = tail_committed_.load(std::memory_order_acquire);
      int32_t s = size_from_pos(old_h, t);
      int32_t c = capacity_.load(std::memory_order_relaxed);

      // We could grow the capacity and then have another thread steal our hard
      // work, so make sure to refetch capacity and see if we still have space
      // after growing.
      while (c - s < N) {
        if (!MakeCacheSpace(N)) {
          tracking::Report(kTCInsertMiss, size_class(), 1);
          freelist_.InsertRange(batch.data(), N);
          return;
        }
        c = capacity_.load(std::memory_order_relaxed);
      }

      new_h = old_h + N;
      if (new_h >= slots_size()) new_h -= slots_size();
    } while (!head_.compare_exchange_weak(
        old_h, new_h, std::memory_order_relaxed, std::memory_order_relaxed));

    tracking::Report(kTCInsertHit, size_class(), 1);
    if (old_h < new_h) {
      ASSERT(new_h - old_h == N);
      void **entry = GetSlot(old_h);
      memcpy(entry, batch.data(), sizeof(void *) * N);
    } else {
      int32_t overhang = slots_size() - old_h;
      ASSERT(overhang + new_h == N);
      void **entry = GetSlot(old_h);
      memcpy(entry, batch.data(), sizeof(void *) * overhang);
      batch.remove_prefix(overhang);
      entry = GetSlot(0);
      memcpy(entry, batch.data(), sizeof(void *) * new_h);
    }

    AdvanceCommitLine(&head_committed_, old_h, new_h);
  }

  // Returns the actual number of fetched elements and stores elements in the
  // batch.
  int RemoveRange(void **batch, int N) {
    ASSERT(N > 0);
    int32_t new_t;
    int32_t old_t = tail_.load(std::memory_order_relaxed);
    do {
      int32_t h = head_committed_.load(std::memory_order_acquire);
      int32_t s = size_from_pos(h, old_t);
      if (s < N) {
        tracking::Report(kTCRemoveMiss, size_class(), 1);
        return freelist_.RemoveRange(batch, N);
      }
      new_t = old_t + N;
      if (new_t >= slots_size()) new_t -= slots_size();
    } while (!tail_.compare_exchange_weak(
        old_t, new_t, std::memory_order_relaxed, std::memory_order_relaxed));

    tracking::Report(kTCRemoveHit, size_class(), 1);
    if (old_t < new_t) {
      ASSERT(new_t - old_t == N);
      void **entry = GetSlot(old_t);
      memcpy(batch, entry, sizeof(void *) * N);
    } else {
      size_t overhang = slots_size() - old_t;
      ASSERT(overhang + new_t == N);

      void **entry = GetSlot(old_t);
      memcpy(batch, entry, sizeof(void *) * overhang);
      batch += overhang;
      entry = GetSlot(0);
      memcpy(batch, entry, sizeof(void *) * new_t);
    }

    AdvanceCommitLine(&tail_committed_, old_t, new_t);
    return N;
  }

  // Returns the number of free objects in the central cache.
  size_t central_length() { return freelist_.length(); }

  // Returns the number of free objects in the transfer cache.
  size_t tc_length() {
    return size_from_pos(head_committed_.load(std::memory_order_relaxed),
                         tail_committed_.load(std::memory_order_relaxed));
  }

  int32_t size_from_pos(int32_t h, int32_t t) {
    int32_t s = h - t;
    if (s < 0) s += slots_size();
    return s;
  }

  // Returns the number of spans allocated and deallocated from the CFL
  SpanStats GetSpanStats() const { return freelist_.GetSpanStats(); }

  // Returns the memory overhead (internal fragmentation) attributable
  // to the freelist.  This is memory lost when the size of elements
  // in a freelist doesn't exactly divide the page-size (an 8192-byte
  // page full of 5-byte objects would have 2 bytes memory overhead).
  size_t OverheadBytes() { return freelist_.OverheadBytes(); }

  // Tries to make room for a batch.  If the cache is full it will try to expand
  // it at the cost of some other cache size.  Return false if there is no
  // space.
  bool MakeCacheSpace(int N) {
    // Check if we can expand this cache?
    int32_t c = capacity_.load(std::memory_order_relaxed);
    if (c + N > max_capacity_) return false;

    int to_evict = owner_->DetermineSizeClassToEvict();
    if (to_evict == size_class()) return false;
    if (!owner_->ShrinkCache(to_evict)) return false;
    if (GrowCache()) return true;

    // At this point, we have successfully taken cache space from someone.  Do
    // not give up until we have given it back to somebody or the entire thing
    // can just start leaking cache capacity.
    while (true) {
      if (++to_evict >= kNumClasses) to_evict = 1;

      // In theory we could unconditionally call to owner_->GrowCache(to_evict);
      // however, that would actually produce different behavior between the
      // prod and the test fake, so we manually do this to avoid jumping through
      // the manager when we "know" we are talking to ourself.
      if (to_evict == size_class()) {
        if (GrowCache()) return true;
      } else if (owner_->GrowCache(to_evict)) {
        return false;
      }
    }
    return true;
  }

  // Tries to grow the cache. Returns false if it failed.
  bool GrowCache() {
    int new_c;
    int old_c = capacity_.load(std::memory_order_relaxed);
    do {
      new_c = old_c + batch_size_;
      if (new_c > max_capacity_) return false;
    } while (!capacity_.compare_exchange_weak(
        old_c, new_c, std::memory_order_relaxed, std::memory_order_relaxed));
    return true;
  }

  // Tries to shrink the Cache.  Return false if it failed.
  bool ShrinkCache() {
    int new_c;
    int32_t old_c = capacity_.load(std::memory_order_relaxed);
    do {
      if (old_c < batch_size_) return false;
      new_c = old_c - batch_size_;
    } while (!capacity_.compare_exchange_weak(
        old_c, new_c, std::memory_order_relaxed, std::memory_order_relaxed));

    // TODO(kfm): decide if we want to do this or not
    // if (tc_length() >= capacity_.load(std::memory_order_relaxed)) {
    //   void *buf[kMaxObjectsToMove];
    //   int i = RemoveRange(buf, N);
    //   freelist().InsertRange(buf, i);
    // }
    return true;
  }

  bool HasSpareCapacity() {
    int32_t c = capacity_.load(std::memory_order_relaxed);
    int32_t s = tc_length();
    return c - s >= batch_size_;
  }

  ABSL_ATTRIBUTE_ALWAYS_INLINE FreeList &freelist() { return freelist_; }
  ABSL_ATTRIBUTE_ALWAYS_INLINE const FreeList &freelist() const {
    return freelist_;
  }

 private:
  // TODO(kfm): this is a simple spin lock, it would be nice if we could
  // report contention here.
  ABSL_ATTRIBUTE_ALWAYS_INLINE void AdvanceCommitLine(
      std::atomic<int32_t> *commit, int32_t from, int32_t to) {
    int32_t temp_pos;
    while (!commit->compare_exchange_weak(temp_pos = from, to,
                                          std::memory_order_release,
                                          std::memory_order_relaxed)) {
#ifdef __x86_64__
      _mm_pause();
#endif
    }
  }

  // Returns first object of the i-th slot.
  void **GetSlot(size_t i) { return slots_ + i; }

  int32_t slots_size() const { return max_capacity_ + 1; }

  size_t size_class() const { return freelist_.size_class(); }

  Manager *const owner_;

  void **slots_;
  FreeList freelist_;

  // Maximum size of the cache for a given size class. (immutable after Init())
  int32_t max_capacity_;
  int32_t batch_size_;

  alignas(ABSL_CACHELINE_SIZE) std::atomic<int32_t> capacity_;
  alignas(ABSL_CACHELINE_SIZE) std::atomic<int32_t> head_;
  alignas(ABSL_CACHELINE_SIZE) std::atomic<int32_t> head_committed_;
  alignas(ABSL_CACHELINE_SIZE) std::atomic<int32_t> tail_;
  alignas(ABSL_CACHELINE_SIZE) std::atomic<int32_t> tail_committed_;
} ABSL_CACHELINE_ALIGNED;

}  // namespace tcmalloc::internal_transfer_cache

#endif  // TCMALLOC_TRANSFER_CACHE_INTERNAL_H_
