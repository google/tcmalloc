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

#include <sys/mman.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include "absl/base/thread_annotations.h"
#include "tcmalloc/arena.h"
#include "tcmalloc/common.h"
#include "tcmalloc/internal/config.h"
#include "tcmalloc/internal/logging.h"
#include "tcmalloc/internal/util.h"
#include "tcmalloc/pages.h"
#include "tcmalloc/span.h"
#include "tcmalloc/static_vars.h"

GOOGLE_MALLOC_SECTION_BEGIN
namespace tcmalloc {
namespace tcmalloc_internal {

template <int BITS, PagemapAllocator Allocator>
GOOGLE_MALLOC_SECTION int PageMap<BITS, Allocator>::GetAllocatedSpans(
    std::vector<tcmalloc::malloc_tracing_extension::AllocatedAddressRanges::
                    SpanDetails>& allocated_spans) {
  PageHeapSpinLockHolder l;
  int allocated_span_count = 0;
  for (std::optional<PageId> p = PageId{0}; p.has_value();
       p = get_next_set_page(p.value())) {
    Span* s = GetDescriptor(p.value());
    if (s == nullptr || s == &tc_globals.invalid_span()) {
      continue;
    }
    // Free'd up Span that's not yet removed from PageMap.
    if (p.value() < s->first_page() || s->last_page() < p.value()) continue;
    CompactSizeClass size_class = sizeclass(p.value());
    TC_ASSERT_EQ(s->first_page(), p.value());
    // As documented, GetAllocatedSpans wants to avoid allocating more memory
    // for the output vector while holding the pageheap_lock. So, we stop
    // adding more entries after we reach its existing capacity. Note that the
    // count returned will still be the total number of allocated Spans.
    if (allocated_spans.capacity() > allocated_spans.size()) {
      allocated_spans.push_back(
          {s->first_page().start_uintptr(), s->bytes_in_span(),
           tc_globals.sizemap().class_to_size(size_class)});
    }
    ++allocated_span_count;
    p = s->last_page();
  }
  return allocated_span_count;
}

template class PageMap<kAddressBits - kPageShift, MetaDataAlloc>;

void* MetaDataAlloc(size_t bytes) {
  return tc_globals.arena().Alloc(ArenaAlloc::kPageMap, bytes);
}

}  // namespace tcmalloc_internal
}  // namespace tcmalloc
GOOGLE_MALLOC_SECTION_END
