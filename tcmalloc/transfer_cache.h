// Copyright 2019 The TCMalloc Authors
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

#ifndef TCMALLOC_TRANSFER_CACHE_H_
#define TCMALLOC_TRANSFER_CACHE_H_

#include <stddef.h>
#include <stdint.h>

#include <atomic>
#include <utility>

#include "absl/base/const_init.h"
#include "absl/base/internal/spinlock.h"
#include "absl/base/macros.h"
#include "absl/base/thread_annotations.h"
#include "absl/types/span.h"
#include "tcmalloc/central_freelist.h"
#include "tcmalloc/common.h"

namespace tcmalloc {

#ifndef TCMALLOC_SMALL_BUT_SLOW

class TransferCaches;

struct alignas(8) SizeInfo {
  int32_t used;
  int32_t capacity;
};

// TransferCache is used to cache transfers of
// sizemap.num_objects_to_move(size_class) back and forth between
// thread caches and the central cache for a given size class.
class TransferCache {
 public:
  constexpr TransferCache(TransferCaches *owner)
      : owner_(owner),
        lock_(absl::kConstInit, absl::base_internal::SCHEDULE_KERNEL_ONLY),
        max_capacity_(0),
        slot_info_{},
        slots_(nullptr),
        freelist_do_not_access_directly_(),
        arbitrary_transfer_(false) {}
  TransferCache(const TransferCache &) = delete;
  TransferCache &operator=(const TransferCache &) = delete;

  void Init(size_t cl) ABSL_EXCLUSIVE_LOCKS_REQUIRED(pageheap_lock);

  // These methods all do internal locking.

  // Insert the specified batch into the transfer cache.  N is the number of
  // elements in the range.  RemoveRange() is the opposite operation.
  void InsertRange(absl::Span<void *> batch, int N) ABSL_LOCKS_EXCLUDED(lock_);

  // Returns the actual number of fetched elements and stores elements in the
  // batch.
  int RemoveRange(void **batch, int N) ABSL_LOCKS_EXCLUDED(lock_);

  // Returns the number of free objects in the central cache.
  size_t central_length() { return freelist().length(); }

  // Returns the number of free objects in the transfer cache.
  size_t tc_length();

  // Returns the memory overhead (internal fragmentation) attributable
  // to the freelist.  This is memory lost when the size of elements
  // in a freelist doesn't exactly divide the page-size (an 8192-byte
  // page full of 5-byte objects would have 2 bytes memory overhead).
  size_t OverheadBytes() { return freelist().OverheadBytes(); }

  SizeInfo GetSlotInfo() const {
    return slot_info_.load(std::memory_order_relaxed);
  }

 private:
  friend TransferCaches;

  // REQUIRES: lock is held.
  // Tries to make room for a batch.  If the cache is full it will try to expand
  // it at the cost of some other cache size.  Return false if there is no
  // space.
  bool MakeCacheSpace(int N) ABSL_EXCLUSIVE_LOCKS_REQUIRED(lock_);

  // REQUIRES: lock_ is *not* held.
  // Tries to shrink the Cache.  Return false if it failed to shrink the cache.
  // Decreases cache_slots_ on success.
  bool ShrinkCache() ABSL_LOCKS_EXCLUDED(lock_);

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

  TransferCaches *const owner_;

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

class TransferCaches {
  template <size_t... Idx>
  constexpr TransferCaches(std::index_sequence<Idx...> i)
      : cache_{((void)Idx, TransferCache(this))...}, next_to_evict_(1) {
    static_assert(sizeof...(Idx) == kNumClasses);
  }

 public:
  constexpr TransferCaches()
      : TransferCaches(std::make_index_sequence<kNumClasses>{}) {}

  TransferCaches(const TransferCaches &) = delete;
  TransferCaches &operator=(const TransferCaches &) = delete;

  void Init() EXCLUSIVE_LOCKS_REQUIRED(pageheap_lock) {
    for (int i = 0; i < kNumClasses; ++i) {
      cache_[i].Init(i);
    }
  }

  void InsertRange(int size_class, absl::Span<void *> batch, int n) {
    cache_[size_class].InsertRange(batch, n);
  }

  int RemoveRange(int size_class, void **batch, int n) {
    return cache_[size_class].RemoveRange(batch, n);
  }

  size_t central_length(int size_class) {
    return cache_[size_class].central_length();
  }
  size_t tc_length(int size_class) { return cache_[size_class].tc_length(); }
  size_t OverheadBytes(int size_class) {
    return cache_[size_class].OverheadBytes();
  }

 private:
  friend TransferCache;
  int DetermineSizeClassToEvict();
  bool ShrinkCache(int size_class) { return cache_[size_class].ShrinkCache(); }

  TransferCache cache_[kNumClasses];
  std::atomic<int32_t> next_to_evict_;
} ABSL_CACHELINE_ALIGNED;

#else

// For the small memory model, the transfer cache is not used.
class TransferCaches {
 public:
  constexpr TransferCaches() : freelist_() {}
  TransferCaches(const TransferCaches &) = delete;
  TransferCaches &operator=(const TransferCaches &) = delete;

  void Init() EXCLUSIVE_LOCKS_REQUIRED(pageheap_lock) {
    for (int i = 0; i < kNumClasses; ++i) {
      freelist_[i].Init(i);
    }
  }

  void InsertRange(int size_class, absl::Span<void *> batch, int n) {
    freelist_[size_class].InsertRange(batch.data(), n);
  }

  int RemoveRange(int size_class, void **batch, int n) {
    return freelist_[size_class].RemoveRange(batch, n);
  }

  size_t central_length(int size_class) {
    return freelist_[size_class].length();
  }

  size_t tc_length(int size_class) { return 0; }

  size_t OverheadBytes(int size_class) {
    return freelist_[size_class].OverheadBytes();
  }

 private:
  CentralFreeList freelist_[kNumClasses];
} ABSL_CACHELINE_ALIGNED;

#endif
}  // namespace tcmalloc

#endif  // TCMALLOC_TRANSFER_CACHE_H_
