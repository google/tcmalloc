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

#include <stddef.h>
#include <stdint.h>

#include <algorithm>
#include <atomic>
#include <utility>

#include "absl/base/attributes.h"
#include "absl/base/const_init.h"
#include "absl/base/internal/spinlock.h"
#include "absl/base/macros.h"
#include "absl/base/thread_annotations.h"
#include "absl/types/span.h"
#include "tcmalloc/central_freelist.h"
#include "tcmalloc/common.h"
#include "tcmalloc/experiment.h"
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
  constexpr TransferCache(TransferCacheManager *owner)
      : owner_(owner),
        lock_(absl::kConstInit, absl::base_internal::SCHEDULE_KERNEL_ONLY),
        max_capacity_(0),
        slot_info_{},
        slots_(nullptr),
        freelist_do_not_access_directly_(),
        arbitrary_transfer_(false) {}
  TransferCache(const TransferCache &) = delete;
  TransferCache &operator=(const TransferCache &) = delete;

  void Init(size_t cl) EXCLUSIVE_LOCKS_REQUIRED(pageheap_lock) {
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
      size_t bytes = TransferCacheManager::class_to_size(cl);
      size_t objs_to_move = TransferCacheManager::num_objects_to_move(cl);
      ASSERT(objs_to_move > 0 && bytes > 0);

      // Starting point for the maximum number of entries in the transfer cache.
      // This actual maximum for a given size class may be lower than this
      // maximum value.
      max_capacity_ = 64 * objs_to_move;
      // A transfer cache freelist can have anywhere from 0 to
      // max_capacity_ slots to put link list chains into.
      info.capacity = 16 * objs_to_move;

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
          TransferCacheManager::Alloc(max_capacity_ * sizeof(void *)));
    }
    SetSlotInfo(info);
  }

  // These methods all do internal locking.

  // Insert the specified batch into the transfer cache.  N is the number of
  // elements in the range.  RemoveRange() is the opposite operation.
  void InsertRange(absl::Span<void *> batch, int N) ABSL_LOCKS_EXCLUDED(lock_) {
    const int B = TransferCacheManager::num_objects_to_move(size_class());
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
    const int B = TransferCacheManager::num_objects_to_move(size_class());
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

  // REQUIRES: lock_ is *not* held.
  // Tries to shrink the Cache.  Return false if it failed to shrink the cache.
  // Decreases cache_slots_ on success.
  bool ShrinkCache() ABSL_LOCKS_EXCLUDED(lock_) {
    int N = TransferCacheManager::num_objects_to_move(size_class());

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

  TransferCacheManager *const owner_;

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

  // This is a thin wrapper for the CentralFreeList.  It is intended to ensure
  // that we are not holding lock_ when we access it.
  ABSL_ATTRIBUTE_ALWAYS_INLINE CentralFreeList &freelist()
      ABSL_LOCKS_EXCLUDED(lock_) {
    return freelist_do_not_access_directly_;
  }

  size_t size_class() const {
    return freelist_do_not_access_directly_.size_class();
  }

  CentralFreeList freelist_do_not_access_directly_;

  // Cached value of IsExperimentActive(Experiment::TCMALLOC_ARBITRARY_TRANSFER)
  bool arbitrary_transfer_;
} ABSL_CACHELINE_ALIGNED;

}  // namespace tcmalloc::internal_transfer_cache

#endif  // TCMALLOC_TRANSFER_CACHE_INTERNAL_H_
