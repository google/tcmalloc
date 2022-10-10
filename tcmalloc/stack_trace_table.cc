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

#include "tcmalloc/stack_trace_table.h"

#include <stddef.h>
#include <string.h>

#include <limits>

#include "absl/base/internal/spinlock.h"
#include "tcmalloc/common.h"
#include "tcmalloc/internal/logging.h"
#include "tcmalloc/internal/mincore.h"
#include "tcmalloc/malloc_extension.h"
#include "tcmalloc/page_heap_allocator.h"
#include "tcmalloc/sampler.h"
#include "tcmalloc/static_vars.h"

GOOGLE_MALLOC_SECTION_BEGIN
namespace tcmalloc {
namespace tcmalloc_internal {

StackTraceTable::StackTraceTable(ProfileType type)
    : type_(type), depth_total_(0), all_(nullptr) {}

StackTraceTable::~StackTraceTable() {
  Bucket* cur = all_;
  while (cur != nullptr) {
    Bucket* next = cur->next;
    cur->~Bucket();
    {
      absl::base_internal::SpinLockHolder h(&pageheap_lock);
      tc_globals.bucket_allocator().Delete(cur);
    }
    cur = next;
  }
  all_ = nullptr;
}

void StackTraceTable::AddTrace(double sample_weight, const StackTrace& t) {
  depth_total_ += t.depth;
  Bucket* b;
  {
    // TODO(b/239458966): Avoid the extra copy of stack trace data to sample
    // for cases where Iterate() can be directly based on TCMalloc's internal
    // data structure.
    absl::base_internal::SpinLockHolder h(&pageheap_lock);
    b = tc_globals.bucket_allocator().New();
  }
  b = new (b) Bucket;

  // Report total bytes that are a multiple of the object size.
  size_t allocated_size = t.allocated_size;
  size_t requested_size = t.requested_size;

  uintptr_t bytes = sample_weight * AllocatedBytes(t) + 0.5;
  // We want sum to be a multiple of allocated_size; pick the nearest
  // multiple rather than always rounding up or down.
  //
  // TODO(b/215362992): Revisit this assertion when GWP-ASan guards
  // zero-byte allocations.
  ASSERT(allocated_size > 0);
  // The reported count of samples, with possible rounding up for unsample.
  b->sample.count = (bytes + allocated_size / 2) / allocated_size;
  b->sample.sum = b->sample.count * allocated_size;
  b->sample.requested_size = requested_size;
  b->sample.requested_alignment = t.requested_alignment;
  b->sample.requested_size_returning = t.requested_size_returning;
  b->sample.allocated_size = allocated_size;
  b->sample.access_hint = static_cast<hot_cold_t>(t.access_hint);
  b->sample.access_allocated = t.cold_allocated ? Profile::Sample::Access::Cold
                                                : Profile::Sample::Access::Hot;
  b->sample.depth = t.depth;
  b->sample.allocation_time = t.allocation_time;

  b->sample.span_start_address = t.span_start_address;

  static_assert(kMaxStackDepth <= Profile::Sample::kMaxStackDepth,
                "Profile stack size smaller than internal stack sizes");
  memcpy(b->sample.stack, t.stack,
         sizeof(b->sample.stack[0]) * b->sample.depth);

  b->next = all_;
  all_ = b;
}

void StackTraceTable::Iterate(
    absl::FunctionRef<void(const Profile::Sample&)> func) const {
  Bucket* cur = all_;
  while (cur != nullptr) {
    func(cur->sample);
    cur = cur->next;
  }
}

}  // namespace tcmalloc_internal
}  // namespace tcmalloc
GOOGLE_MALLOC_SECTION_END
