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

#include "tcmalloc/central_freelist.h"

#include <stdint.h>

#include "tcmalloc/internal/linked_list.h"
#include "tcmalloc/internal/logging.h"
#include "tcmalloc/page_heap.h"
#include "tcmalloc/pagemap.h"
#include "tcmalloc/static_vars.h"

namespace tcmalloc {

// Like a constructor and hence we disable thread safety analysis.
void CentralFreeList::Init(size_t cl) ABSL_NO_THREAD_SAFETY_ANALYSIS {
  size_class_ = cl;
  object_size_ = Static::sizemap()->class_to_size(cl);
  pages_per_span_ = Static::sizemap()->class_to_pages(cl);
  objects_per_span_ = pages_per_span_ * kPageSize / (cl ? object_size_ : 1);
}

static Span* MapObjectToSpan(void* object) {
  const PageId p = PageIdContaining(object);
  Span* span = Static::pagemap()->GetExistingDescriptor(p);
  return span;
}

Span* CentralFreeList::ReleaseToSpans(void* object, Span* span) {
  if (span->FreelistEmpty()) {
    nonempty_.prepend(span);
  }

  if (span->FreelistPush(object, object_size_)) {
    return nullptr;
  }
  span->RemoveFromList();  // from nonempty_
  return span;
}

void CentralFreeList::InsertRange(void** batch, int N) {
  CHECK_CONDITION(N > 0 && N <= kMaxObjectsToMove);
  Span* spans[kMaxObjectsToMove];
  // Safe to store free spans into freed up space in span array.
  Span** free_spans = spans;
  int free_count = 0;

  // Prefetch Span objects to reduce cache misses.
  for (int i = 0; i < N; ++i) {
    Span* span = MapObjectToSpan(batch[i]);
    ASSERT(span != nullptr);
    span->Prefetch();
    spans[i] = span;
  }

  // First, release all individual objects into spans under our mutex
  // and collect spans that become completely free.
  {
    absl::base_internal::SpinLockHolder h(&lock_);
    for (int i = 0; i < N; ++i) {
      Span* span = ReleaseToSpans(batch[i], spans[i]);
      if (span) {
        free_spans[free_count] = span;
        free_count++;
      }
    }
    RecordMultiSpansDeallocated(free_count);
    UpdateObjectCounts(N);
  }

  // Then, release all free spans into page heap under its mutex.
  if (free_count) {
    absl::base_internal::SpinLockHolder h(&pageheap_lock);
    for (int i = 0; i < free_count; ++i) {
      ASSERT(!IsTaggedMemory(free_spans[i]->start_address()));
      Static::pagemap()->UnregisterSizeClass(free_spans[i]);
      Static::page_allocator()->Delete(free_spans[i], /*tagged=*/false);
    }
  }
}

int CentralFreeList::RemoveRange(void** batch, int N) {
  ASSERT(N > 0);
  absl::base_internal::SpinLockHolder h(&lock_);
  if (nonempty_.empty()) {
    Populate();
  }

  int result = 0;
  while (result < N && !nonempty_.empty()) {
    Span* span = nonempty_.first();
    int here = span->FreelistPopBatch(batch + result, N - result, object_size_);
    ASSERT(here > 0);
    if (span->FreelistEmpty()) {
      span->RemoveFromList();  // from nonempty_
    }
    result += here;
  }
  UpdateObjectCounts(-result);
  return result;
}

// Fetch memory from the system and add to the central cache freelist.
void CentralFreeList::Populate() ABSL_NO_THREAD_SAFETY_ANALYSIS {
  // Release central list lock while operating on pageheap
  lock_.Unlock();

  Span* span = Static::page_allocator()->New(pages_per_span_, /*tagged=*/false);
  if (span == nullptr) {
    Log(kLog, __FILE__, __LINE__, "tcmalloc: allocation failed",
        pages_per_span_ << kPageShift);
    lock_.Lock();
    return;
  }
  ASSERT(span->num_pages() == pages_per_span_);

  Static::pagemap()->RegisterSizeClass(span, size_class_);
  span->BuildFreelist(object_size_, objects_per_span_);

  // Add span to list of non-empty spans
  lock_.Lock();
  nonempty_.prepend(span);
  RecordSpanAllocated();
}

size_t CentralFreeList::OverheadBytes() {
  if (size_class_ == 0) {  // 0 holds the 0-sized allocations
    return 0;
  }
  const size_t overhead_per_span = (pages_per_span_ * kPageSize) % object_size_;
  return num_spans() * overhead_per_span;
}

SpanStats CentralFreeList::GetSpanStats() const {
  SpanStats stats;
  if (size_class_ == 0) {  // 0 holds the 0-sized allocations
    return stats;
  }
  stats.num_spans_requested = static_cast<size_t>(num_spans_requested_.value());
  stats.num_spans_returned = static_cast<size_t>(num_spans_returned_.value());
  stats.obj_capacity = stats.num_live_spans() * objects_per_span_;
  return stats;
}

}  // namespace tcmalloc
