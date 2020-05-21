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

#include "absl/base/const_init.h"
#include "absl/base/internal/spinlock.h"
#include "absl/base/macros.h"
#include "absl/base/thread_annotations.h"
#include "absl/types/span.h"
#include "tcmalloc/central_freelist.h"
#include "tcmalloc/common.h"

namespace tcmalloc {

#ifndef TCMALLOC_SMALL_BUT_SLOW

struct alignas(8) SizeInfo {
  int32_t used;
  int32_t capacity;
};

// TransferCache is used to cache transfers of
// sizemap.num_objects_to_move(size_class) back and forth between
// thread caches and the central cache for a given size class.
class TransferCache {
 public:
  constexpr TransferCache()
      : lock_(absl::kConstInit, absl::base_internal::SCHEDULE_KERNEL_ONLY),
        max_capacity_(0),
        slot_info_{},
        slots_(nullptr),
        freelist_(),
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
  size_t central_length() { return freelist_.length(); }

  // Returns the number of free objects in the transfer cache.
  size_t tc_length();

  // Returns the memory overhead (internal fragmentation) attributable
  // to the freelist.  This is memory lost when the size of elements
  // in a freelist doesn't exactly divide the page-size (an 8192-byte
  // page full of 5-byte objects would have 2 bytes memory overhead).
  size_t OverheadBytes() {
    return freelist_.OverheadBytes();
  }

  SizeInfo GetSlotInfo() const {
    return slot_info_.load(std::memory_order_relaxed);
  }

 private:
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

  CentralFreeList freelist_;

  // Cached value of IsExperimentActive(Experiment::TCMALLOC_ARBITRARY_TRANSFER)
  bool arbitrary_transfer_;
} ABSL_CACHELINE_ALIGNED;

#else

// For the small memory model, the transfer cache is not used.
class TransferCache {
 public:
  constexpr TransferCache() : freelist_() {}
  TransferCache(const TransferCache &) = delete;
  TransferCache &operator=(const TransferCache &) = delete;

  void Init(size_t cl) EXCLUSIVE_LOCKS_REQUIRED(pageheap_lock) {
    freelist_.Init(cl);
  }

  void InsertRange(absl::Span<void *> batch, int N) {
    freelist_.InsertRange(batch.data(), N);
  }

  int RemoveRange(void **batch, int N) {
    return freelist_.RemoveRange(batch, N);
  }

  size_t central_length() { return freelist_.length(); }

  size_t tc_length() { return 0; }

  size_t OverheadBytes() { return freelist_.OverheadBytes(); }

 private:
  CentralFreeList freelist_;
} ABSL_CACHELINE_ALIGNED;

#endif
}  // namespace tcmalloc

#endif  // TCMALLOC_TRANSFER_CACHE_H_
