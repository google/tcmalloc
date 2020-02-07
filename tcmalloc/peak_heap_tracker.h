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

#ifndef TCMALLOC_PEAK_HEAP_TRACKER_H_
#define TCMALLOC_PEAK_HEAP_TRACKER_H_

#include "absl/base/thread_annotations.h"
#include "tcmalloc/common.h"
#include "tcmalloc/internal/atomic_stats_counter.h"
#include "tcmalloc/internal/logging.h"
#include "tcmalloc/malloc_extension.h"

namespace tcmalloc {

class PeakHeapTracker {
 public:
  // Constructor should do nothing since we rely on explicit Init()
  // call, which may or may not be called before the constructor runs.
  PeakHeapTracker() {}

  // Explicit Init is required because constructor for our single static
  // instance may not have run by the time it is used
  void Init() {
    peak_sampled_span_stacks_ = nullptr;
    peak_sampled_heap_size_.Clear();
  }

  // Possibly save high-water-mark allocation stack traces for peak-heap
  // profile. Should be called immediately after sampling an allocation. If
  // the heap has grown by a sufficient amount since the last high-water-mark,
  // it will save a copy of the sample profile.
  void MaybeSaveSample() LOCKS_EXCLUDED(pageheap_lock);

  // Return the saved high-water-mark heap profile, if any.
  std::unique_ptr<tcmalloc_internal::ProfileBase> DumpSample() const
      LOCKS_EXCLUDED(pageheap_lock);

 private:
  // Linked list of stack traces from sampled allocations saved (from
  // sampled_objects_ above) when we allocate memory from the system. The
  // linked list pointer is stored in StackTrace::stack[kMaxStackDepth-1].
  StackTrace* peak_sampled_span_stacks_;

  // Sampled heap size last time peak_sampled_span_stacks_ was saved. Only
  // written under pageheap_lock; may be read without it.
  tcmalloc_internal::StatsCounter peak_sampled_heap_size_;

  bool IsNewPeak();
};

}  // namespace tcmalloc

#endif  // TCMALLOC_PEAK_HEAP_TRACKER_H_
