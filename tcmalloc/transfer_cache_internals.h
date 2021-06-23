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

#include <limits>

#include "tcmalloc/internal/config.h"

#ifdef __x86_64__
#include <emmintrin.h>
#include <xmmintrin.h>
#endif

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <utility>

#include "absl/base/attributes.h"
#include "absl/base/casts.h"
#include "absl/base/const_init.h"
#include "absl/base/internal/spinlock.h"
#include "absl/base/macros.h"
#include "absl/base/optimization.h"
#include "absl/base/thread_annotations.h"
#include "absl/synchronization/internal/futex.h"
#include "absl/types/optional.h"
#include "absl/types/span.h"
#include "tcmalloc/central_freelist.h"
#include "tcmalloc/common.h"
#include "tcmalloc/experiment.h"
#include "tcmalloc/internal/logging.h"
#include "tcmalloc/tracking.h"
#include "tcmalloc/transfer_cache_stats.h"

GOOGLE_MALLOC_SECTION_BEGIN
namespace tcmalloc::tcmalloc_internal::internal_transfer_cache {

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
        insert_hits_(0),
        remove_hits_(0),
        insert_misses_(0),
        remove_misses_(0),
        slot_info_{},
        slots_(nullptr),
        freelist_do_not_access_directly_() {}

  TransferCache(const TransferCache &) = delete;
  TransferCache &operator=(const TransferCache &) = delete;

  struct Capacity {
    int capacity;
    int max_capacity;
  };

  // Compute initial and max capacity that we should configure this cache for.
  static Capacity CapacityNeeded(size_t cl) {
    // We need at least 2 slots to store list head and tail.
    static_assert(kMinObjectsToMove >= 2);

    const size_t bytes = Manager::class_to_size(cl);
    if (cl <= 0 || bytes <= 0) return {0, 0};

    // Limit the maximum size of the cache based on the size class.  If this
    // is not done, large size class objects will consume a lot of memory if
    // they just sit in the transfer cache.
    const size_t objs_to_move = Manager::num_objects_to_move(cl);
    ASSERT(objs_to_move > 0);

    // Starting point for the maximum number of entries in the transfer cache.
    // This actual maximum for a given size class may be lower than this
    // maximum value.
    int max_capacity = kMaxCapacityInBatches * objs_to_move;
    // A transfer cache freelist can have anywhere from 0 to
    // max_capacity_ slots to put link list chains into.
    int capacity = kInitialCapacityInBatches * objs_to_move;

    // Limit each size class cache to at most 1MB of objects or one entry,
    // whichever is greater. Total transfer cache memory used across all
    // size classes then can't be greater than approximately
    // 1MB * kMaxNumTransferEntries.
    max_capacity = std::min<int>(
        max_capacity,
        std::max<int>(objs_to_move,
                      (1024 * 1024) / (bytes * objs_to_move) * objs_to_move));
    capacity = std::min(capacity, max_capacity);

    if (IsExperimentActive(Experiment::TEST_ONLY_TCMALLOC_16X_TRANSFER_CACHE) ||
        IsExperimentActive(Experiment::TCMALLOC_16X_TRANSFER_CACHE_REAL)) {
      capacity *= 16;
      max_capacity *= 16;
    }
    return {capacity, max_capacity};
  }

  // We require the pageheap_lock with some templates, but not in tests, so the
  // thread safety analysis breaks pretty hard here.
  void Init(size_t cl) ABSL_NO_THREAD_SAFETY_ANALYSIS {
    freelist().Init(cl);
    const Capacity needed = CapacityNeeded(cl);

    absl::base_internal::SpinLockHolder h(&lock_);
    max_capacity_ = needed.max_capacity;
    slots_ = max_capacity_ != 0 ? reinterpret_cast<void **>(owner_->Alloc(
                                      max_capacity_ * sizeof(void *)))
                                : nullptr;
    SetSlotInfo({0, needed.capacity});
  }

  static constexpr bool IsFlexible() { return false; }

  // These methods all do internal locking.

  // Insert the specified batch into the transfer cache.  N is the number of
  // elements in the range.  RemoveRange() is the opposite operation.
  void InsertRange(absl::Span<void *> batch) ABSL_LOCKS_EXCLUDED(lock_) {
    const int N = batch.size();
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
        insert_hits_++;
        return;
      }
    }
    insert_misses_.fetch_add(1, std::memory_order_relaxed);
    tracking::Report(kTCInsertMiss, size_class(), 1);
    freelist().InsertRange(batch);
  }

  // Returns the actual number of fetched elements and stores elements in the
  // batch.
  ABSL_MUST_USE_RESULT int RemoveRange(void **batch, int N)
      ABSL_LOCKS_EXCLUDED(lock_) {
    ASSERT(N > 0);
    const int B = Manager::num_objects_to_move(size_class());
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
        remove_hits_++;
        return N;
      }
    }
    remove_misses_.fetch_add(1, std::memory_order_relaxed);
    tracking::Report(kTCRemoveMiss, size_class(), 1);
    return freelist().RemoveRange(batch, N);
  }

  // Returns the number of free objects in the central cache.
  size_t central_length() const { return freelist().length(); }

  // Returns the number of free objects in the transfer cache.
  size_t tc_length() const {
    return static_cast<size_t>(slot_info_.load(std::memory_order_relaxed).used);
  }

  // Returns the number of spans allocated and deallocated from the CFL
  SpanStats GetSpanStats() const { return freelist().GetSpanStats(); }

  // Returns the number of transfer cache insert/remove hits/misses.
  TransferCacheStats GetHitRateStats() ABSL_LOCKS_EXCLUDED(lock_) {
    TransferCacheStats stats;
    {
      absl::base_internal::SpinLockHolder h(&lock_);
      stats.insert_hits = insert_hits_;
      stats.remove_hits = remove_hits_;
    }
    stats.insert_misses = insert_misses_;
    stats.remove_misses = remove_misses_;
    return stats;
  }

  // Returns the memory overhead (internal fragmentation) attributable
  // to the freelist.  This is memory lost when the size of elements
  // in a freelist doesn't exactly divide the page-size (an 8192-byte
  // page full of 5-byte objects would have 2 bytes memory overhead).
  size_t OverheadBytes() const { return freelist().OverheadBytes(); }

  SizeInfo GetSlotInfo() const {
    return slot_info_.load(std::memory_order_relaxed);
  }

  // REQUIRES: lock is held.
  // Tries to make room for N elements. If the cache is full it will try to
  // expand it at the cost of some other cache size.  Return false if there is
  // no space.
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

  bool HasSpareCapacity() const {
    int n = Manager::num_objects_to_move(size_class());
    auto info = GetSlotInfo();
    return info.capacity - info.used >= n;
  }

  // Takes lock_ and invokes MakeCacheSpace() on this cache.  Returns true if it
  // succeeded at growing the cache by a batch size.
  bool GrowCache() ABSL_LOCKS_EXCLUDED(lock_) {
    absl::base_internal::SpinLockHolder h(&lock_);
    return MakeCacheSpace(Manager::num_objects_to_move(size_class()));
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
      if (info.capacity < N) return false;

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
    freelist().InsertRange({to_free, static_cast<uint64_t>(num_to_free)});
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

  size_t insert_hits_ ABSL_GUARDED_BY(lock_);
  size_t remove_hits_ ABSL_GUARDED_BY(lock_);
  std::atomic<size_t> insert_misses_;
  std::atomic<size_t> remove_misses_;

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
} ABSL_CACHELINE_ALIGNED;

}  // namespace tcmalloc::tcmalloc_internal::internal_transfer_cache
GOOGLE_MALLOC_SECTION_END

#endif  // TCMALLOC_TRANSFER_CACHE_INTERNAL_H_
