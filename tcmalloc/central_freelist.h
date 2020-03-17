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

#ifndef TCMALLOC_CENTRAL_FREELIST_H_
#define TCMALLOC_CENTRAL_FREELIST_H_

#include <stddef.h>

#include "absl/base/internal/spinlock.h"
#include "absl/base/macros.h"
#include "absl/base/thread_annotations.h"
#include "tcmalloc/internal/atomic_stats_counter.h"
#include "tcmalloc/span.h"

namespace tcmalloc {

// Data kept per size-class in central cache.
class CentralFreeList {
 public:
  // A CentralFreeList may be used before its constructor runs.
  // So we prevent lock_'s constructor from doing anything to the lock_ state.
  CentralFreeList()
      : lock_(absl::base_internal::kLinkerInitialized),
        counter_(absl::base_internal::kLinkerInitialized),
        num_spans_(absl::base_internal::kLinkerInitialized) {}

  void Init(size_t cl) ABSL_LOCKS_EXCLUDED(lock_);

  // These methods all do internal locking.

  // Insert batch[0..N-1] into the central freelist.
  // REQUIRES: N > 0 && N <= kMaxObjectsToMove.
  void InsertRange(void** batch, int N) ABSL_LOCKS_EXCLUDED(lock_);

  // Fill a prefix of batch[0..N-1] with up to N elements removed from central
  // freelist.  Return the number of elements removed.
  int RemoveRange(void** batch, int N) ABSL_LOCKS_EXCLUDED(lock_);

  // Returns the number of free objects in cache.
  size_t length() { return static_cast<size_t>(counter_.value()); }

  // Returns the memory overhead (internal fragmentation) attributable
  // to the freelist.  This is memory lost when the size of elements
  // in a freelist doesn't exactly divide the page-size (an 8192-byte
  // page full of 5-byte objects would have 2 bytes memory overhead).
  size_t OverheadBytes();

  // My size class.
  size_t size_class() const {
    return size_class_;
  }

 private:
  // Release an object to spans.
  // Returns object's span if it become completely free.
  Span* ReleaseToSpans(void* object, Span* span)
      ABSL_EXCLUSIVE_LOCKS_REQUIRED(lock_);

  // Populate cache by fetching from the page heap.
  // May temporarily release lock_.
  void Populate() ABSL_EXCLUSIVE_LOCKS_REQUIRED(lock_);

  // This lock protects all the mutable data members.
  absl::base_internal::SpinLock lock_;

  size_t size_class_;  // My size class (immutable after Init())
  size_t object_size_;
  size_t objects_per_span_;

  // Following are kept as a StatsCounter so that they can read without
  // acquiring a lock. Updates to these variables are guarded by lock_ so writes
  // are performed using LossyAdd for speed, the lock still guarantees accuracy.

  // Num free objects in cache entry
  tcmalloc_internal::StatsCounter counter_;
  // Num spans in empty_ plus nonempty_
  tcmalloc_internal::StatsCounter num_spans_;

  SpanList nonempty_
      ABSL_GUARDED_BY(lock_);  // Dummy header for non-empty spans

  CentralFreeList(const CentralFreeList&) = delete;
  CentralFreeList& operator=(const CentralFreeList&) = delete;
};

}  // namespace tcmalloc

#endif  // TCMALLOC_CENTRAL_FREELIST_H_
