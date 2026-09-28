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

#include <atomic>
#include <cstddef>
#include <cstdint>

#include "absl/base/attributes.h"
#include "absl/base/optimization.h"
#include "absl/base/thread_annotations.h"
#include "absl/types/span.h"
#include "tcmalloc/arena.h"
#include "tcmalloc/common.h"
#include "tcmalloc/error_reporting.h"
#include "tcmalloc/internal/central_freelist_hooks.h"
#include "tcmalloc/internal/config.h"
#include "tcmalloc/internal/hook_list.h"
#include "tcmalloc/internal/logging.h"
#include "tcmalloc/internal/memory_tag.h"
#include "tcmalloc/internal/prefetch.h"
#include "tcmalloc/page_allocator_interface.h"
#include "tcmalloc/pagemap.h"
#include "tcmalloc/pages.h"
#include "tcmalloc/span.h"
#include "tcmalloc/static_vars.h"

GOOGLE_MALLOC_SECTION_BEGIN
namespace tcmalloc {
namespace tcmalloc_internal {
namespace central_freelist_internal {

static MemoryTag MemoryTagFromSizeClass(size_t size_class) {
  if (IsColdSizeClass(size_class)) {
    return MemoryTag::kCold;
  }
  if (tc_globals.active_partitions() == 1) {
    return MemoryTag::kNormal;
  }
  return MultiNormalTag(size_class / kNumBaseClasses);
}

static AccessDensityPrediction AccessDensity(int objects_per_span) {
  // Use number of objects per span as a proxy for estimating access density of
  // the span. If number of objects per span is higher than
  // kFewObjectsAllocMaxLimit threshold, we assume that the span would be
  // long-lived.
  return objects_per_span > kFewObjectsAllocMaxLimit
             ? AccessDensityPrediction::kDense
             : AccessDensityPrediction::kSparse;
}

size_t StaticForwarder::class_to_size(int size_class) {
  return tc_globals.sizemap().class_to_size(size_class);
}

Length StaticForwarder::class_to_pages(int size_class) {
  return Length(tc_globals.sizemap().class_to_pages(size_class));
}
void StaticForwarder::MapObjectsToMeta(absl::Span<void*> batch,
                                       std::atomic<SpanMeta>** metas) {
  // Prefetch SpanMeta objects to reduce cache misses inside of critical
  // section.
  for (size_t i = 0; i < batch.size(); ++i) {
    metas[i] = tc_globals.pagemap().GetSpanMeta(PageIdContaining(batch[i]));
    PrefetchW(metas[i]);
  }
}

std::atomic<SpanMeta>* StaticForwarder::GetSpanMeta(PageId p) {
  return tc_globals.pagemap().GetSpanMeta(p);
}

void* StaticForwarder::ArenaAlloc(size_t bytes, std::align_val_t alignment) {
  return tc_globals.arena().Alloc(ArenaAlloc::kCentralFreeListArray, bytes,
                                  alignment);
}

void* absl_nullable StaticForwarder::AllocateSpan(int size_class,
                                                  size_t objects_per_span,
                                                  Length pages_per_span) {
  const MemoryTag tag = MemoryTagFromSizeClass(size_class);
  const AccessDensityPrediction density = AccessDensity(objects_per_span);

  SpanAllocInfo span_alloc_info = {.objects_per_span = objects_per_span,
                                   .density = density};
  TC_ASSERT(density == AccessDensityPrediction::kSparse ||
            (density == AccessDensityPrediction::kDense &&
             pages_per_span == Length(1)));
  auto res =
      tc_globals.page_allocator().New(pages_per_span, span_alloc_info, tag);
  if (ABSL_PREDICT_FALSE(!res)) {
    return nullptr;
  }
  TC_ASSERT_EQ(tag, GetMemoryTag(res.r.start_addr()));
  TC_ASSERT_EQ(res.r.n, pages_per_span);

  tc_globals.pagemap().RegisterSmallSpan(res.r.p, pages_per_span, size_class,
                                         res.donated);
  return res.r.start_addr();
}

static void ReturnAllocsToPageHeap(
    MemoryTag tag,
    absl::Span<PageAllocatorInterface::AllocationState> free_allocs,
    SpanAllocInfo span_alloc_info) ABSL_LOCKS_EXCLUDED(pageheap_lock) {
  PageHeapSpinLockHolder l;
  for (const auto& alloc : free_allocs) {
    tc_globals.page_allocator().Delete(alloc, tag, span_alloc_info);
  }
}

void StaticForwarder::DeallocateSpans(
    size_t objects_per_span, Length pages_per_span,
    absl::Span<void*> free_spans, absl::Span<std::atomic<SpanMeta>*> pmetas) {
  TC_ASSERT_NE(free_spans.size(), 0);
  TC_ASSERT_LE(free_spans.size(), kMaxObjectsToMove);
  TC_ASSERT_EQ(free_spans.size(), pmetas.size());
  const MemoryTag tag = GetMemoryTag(free_spans[0]);
  PageAllocatorInterface::AllocationState allocs[kMaxObjectsToMove];
  // Unregister size class doesn't require holding any locks.
  for (int i = 0, n = free_spans.size(); i < n; ++i) {
    void* ptr = free_spans[i];
    TC_ASSERT_EQ(GetMemoryTag(ptr), tag);
    TC_ASSERT(!IsSampledMemory(ptr));
    const PageId p = PageIdContaining(ptr);

    // Before taking pageheap_lock, prefetch the PageTrackers these spans are
    // on.
    void* pt = tc_globals.pagemap().GetHugepage(p);
    // Prefetch for writing, as we will issue stores to the PageTracker
    // instance.
    PrefetchW(pt);
    PrefetchW(reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(pt) +
                                      ABSL_CACHELINE_SIZE));
    allocs[i].r = Range(p, pages_per_span);
    allocs[i].donated = pmetas[i]->load(std::memory_order_relaxed).is_donated;
    tc_globals.pagemap().UnregisterSmallSpan(p, pages_per_span);
  }

  const AccessDensityPrediction density = AccessDensity(objects_per_span);
  SpanAllocInfo span_alloc_info = {.objects_per_span = objects_per_span,
                                   .density = density};
  ReturnAllocsToPageHeap(tag, absl::MakeSpan(allocs, free_spans.size()),
                         span_alloc_info);
}

ABSL_ATTRIBUTE_NOINLINE void StaticForwarder::InvokeInsertRangeHookSlow(
    size_t size_class, absl::Span<void*> batch) {
  central_freelist_insert_range_hooks.Invoke(size_class, batch);
}

ABSL_ATTRIBUTE_NOINLINE void StaticForwarder::InvokeRemoveRangeHookSlow(
    size_t size_class, absl::Span<void*> batch) {
  central_freelist_remove_range_hooks.Invoke(size_class, batch);
}

}  // namespace central_freelist_internal
}  // namespace tcmalloc_internal
}  // namespace tcmalloc
GOOGLE_MALLOC_SECTION_END
