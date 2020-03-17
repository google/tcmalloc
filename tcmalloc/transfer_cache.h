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

#include "absl/base/internal/spinlock.h"
#include "absl/base/macros.h"
#include "absl/base/thread_annotations.h"
#include "absl/types/span.h"
#include "tcmalloc/central_freelist.h"
#include "tcmalloc/common.h"

namespace tcmalloc {

#ifndef TCMALLOC_SMALL_BUT_SLOW

// TransferCache is used to cache transfers of
// sizemap.num_objects_to_move(size_class) back and forth between
// thread caches and the central cache for a given size class.
class TransferCache {
 public:
  // A TransferCache may be used before its constructor runs.
  // So we prevent lock_'s constructor from doing anything to the lock_ state.
  TransferCache() : lock_(absl::base_internal::kLinkerInitialized) {}
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

 private:
  // REQUIRES: lock is held.
  // Tries to make room for a batch.  If the cache is full it will try to expand
  // it at the cost of some other cache size.  Return false if there is no
  // space.
  bool MakeCacheSpace(int N) ABSL_EXCLUSIVE_LOCKS_REQUIRED(lock_);

  // REQUIRES: lock_ for locked_size_class is held.
  // Picks a "random" size class to steal slots from.  In reality it just
  // iterates over the sizeclasses but does so without taking a lock.  Returns
  // true on success.
  // May temporarily lock a "random" size class.
  static bool EvictRandomSizeClass(int locked_size_class, bool force);

  // REQUIRES: lock_ is *not* held.
  // Tries to shrink the Cache.  If force is true it will relase objects to
  // spans if it allows it to shrink the cache.  Return false if it failed to
  // shrink the cache.  Decreases cache_slots_ on success.
  // May temporarily take lock_.  If it takes lock_, the locked_size_class
  // lock is released to keep the thread from holding two size class locks
  // concurrently which could lead to a deadlock.
  bool ShrinkCache(int locked_size_class, bool force)
      ABSL_LOCKS_EXCLUDED(lock_);

  // Returns first object of the i-th slot.
  void **GetSlot(size_t i) ABSL_EXCLUSIVE_LOCKS_REQUIRED(lock_) {
    return slots_ + i;
  }

  // This lock protects all the data members.  used_slots_ and cache_slots_
  // may be looked at without holding the lock.
  absl::base_internal::SpinLock lock_;

  // Number of currently used cached entries in tc_slots_.  This variable is
  // updated under a lock but can be read without one.
  std::atomic<int32_t> used_slots_;

  // Pointer to array of free objects.  Use GetSlot() to get pointers to
  // entries.
  void **slots_ ABSL_GUARDED_BY(lock_);

  // The current number of slots for this size class.  This is an adaptive value
  // that is increased if there is lots of traffic on a given size class.  This
  // variable is updated under a lock but can be read without one.
  std::atomic<int32_t> cache_slots_;

  // Maximum size of the cache for a given size class. (immutable after Init())
  int32_t max_cache_slots_;

  CentralFreeList freelist_;

  // Cached value of IsExperimentActive(Experiment::TCMALLOC_ARBITRARY_TRANSFER)
  bool arbitrary_transfer_;
} ABSL_CACHELINE_ALIGNED;

#else

// For the small memory model, the transfer cache is not used.
class TransferCache {
 public:
  TransferCache() {}
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
};

#endif
}  // namespace tcmalloc

#endif  // TCMALLOC_TRANSFER_CACHE_H_
