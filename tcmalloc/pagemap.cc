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

#include "tcmalloc/pagemap.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include "absl/base/optimization.h"
#include "tcmalloc/arena.h"
#include "tcmalloc/common.h"
#include "tcmalloc/internal/config.h"
#include "tcmalloc/internal/logging.h"
#include "tcmalloc/internal/sampled_allocation.h"
#include "tcmalloc/malloc_tracing_extension.h"
#include "tcmalloc/pages.h"
#include "tcmalloc/span.h"
#include "tcmalloc/static_vars.h"

GOOGLE_MALLOC_SECTION_BEGIN
namespace tcmalloc {
namespace tcmalloc_internal {

void* PageMapMetaDataAlloc(size_t bytes) {
  return tc_globals.arena().Alloc(ArenaAlloc::kPageMap, bytes);
}

template <int BITS, PagemapAllocator Allocator>
GOOGLE_MALLOC_SECTION int PageMap<BITS, Allocator>::GetAllocatedSpans(
    std::vector<tcmalloc::malloc_tracing_extension::AllocatedAddressRanges::
                    SpanDetails>& allocated_spans) {
  PageHeapSpinLockHolder l;
  int allocated_span_count = 0;
  for (std::optional<PageId> p = PageId{0}; p.has_value();
       p = get_next_set_page(p.value())) {
    PageId page_id = p.value();
    std::optional<SpanMeta> meta = GetSpanMetaNullable(page_id);
    if (!meta.has_value()) {
      continue;
    }
    if (meta->is_small) {
      if (!meta->is_in_cfl && meta->heap_index_or_page_offset != 0) {
        continue;
      }
      Length n = Length(tc_globals.sizemap().class_to_pages(meta->size_class));
      size_t object_size = tc_globals.sizemap().class_to_size(meta->size_class);
      if (allocated_spans.capacity() > allocated_spans.size()) {
        allocated_spans.push_back(
            {page_id.start_uintptr(), n.in_bytes(), object_size});
      }
      ++allocated_span_count;
      p = page_id + n - Length(1);
    } else {
      Length n = BytesToLengthCeil(GetLargeSize(page_id, *meta));
      // As documented, GetAllocatedSpans wants to avoid allocating more
      // memory for the output vector while holding the pageheap_lock. So, we
      // stop adding more entries after we reach its existing capacity. Note
      // that the count returned will still be the total number of allocated
      // Spans.
      if (allocated_spans.capacity() > allocated_spans.size()) {
        allocated_spans.push_back({page_id.start_uintptr(), n.in_bytes(), 0});
      }
      ++allocated_span_count;
      p = page_id + n - Length(1);
    }
  }
  return allocated_span_count;
}

template <int BITS, PagemapAllocator Allocator>
GOOGLE_MALLOC_SECTION void PageMap<BITS, Allocator>::RegisterSampledSpan(
    PageId p, SampledAllocation* s) {
  std::atomic<SpanMeta>* meta_ptr = GetSpanMeta(p);
  SpanMeta meta = meta_ptr->load(std::memory_order_relaxed);
  TC_ASSERT_EQ(meta.is_small1, 0);
  meta.is_sampled = 1;
  meta.num_pages_or_sampled_index = s->sampled_index;
  TC_CHECK_EQ(meta.num_pages_or_sampled_index, s->sampled_index);
  meta_ptr->store(meta, std::memory_order_relaxed);
  TC_ASSERT_EQ(sizeclass(p), 0);
}

template <int BITS, PagemapAllocator Allocator>
GOOGLE_MALLOC_SECTION SampledAllocation*
PageMap<BITS, Allocator>::UnregisterSampledSpan(PageId p) {
  TC_ASSERT_EQ(sizeclass(p), 0);
  std::atomic<SpanMeta>* meta_ptr = GetSpanMeta(p);
  SpanMeta meta = meta_ptr->load(std::memory_order_relaxed);
  TC_ASSERT(meta.is_sampled);
  SampledAllocation* s = tc_globals.sampled_allocation_recorder().Map(
      meta.num_pages_or_sampled_index);
  meta.is_sampled = 0;
  meta.num_pages_or_sampled_index = 0;
  meta_ptr->store(meta, std::memory_order_relaxed);
  return s;
}

template <int BITS, PagemapAllocator Allocator>
GOOGLE_MALLOC_SECTION size_t
PageMap<BITS, Allocator>::GetLargeSize(PageId p, SpanMeta meta) const {
  if (meta.is_sampled) {
    return tc_globals.sampled_allocation_recorder()
        .Map(meta.num_pages_or_sampled_index)
        ->sampled_stack.allocated_size;
  }
  Length n = meta.num_pages_or_sampled_index == 0
                 ? Length(reinterpret_cast<uintptr_t>(GetHugepage(p)))
                 : Length(meta.num_pages_or_sampled_index);
  return n.in_bytes();
}

template <int BITS, PagemapAllocator Allocator>
GOOGLE_MALLOC_SECTION size_t
PageMap<BITS, Allocator>::GetLargeSize(PageId p) const {
  std::optional<SpanMeta> meta = GetSpanMetaNullable(p);
  if (ABSL_PREDICT_FALSE(!meta.has_value() || meta->is_small)) {
    return 0;
  }
  return GetLargeSize(p, *meta);
}

template class PageMap<kAddressBits - kPageShift, PageMapMetaDataAlloc>;

}  // namespace tcmalloc_internal
}  // namespace tcmalloc
GOOGLE_MALLOC_SECTION_END
