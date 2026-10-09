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

#include <algorithm>
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
    PageMeta meta = GetDescriptor(p.value());
    if (!meta.valid()) {
      continue;
    }
    CompactSizeClass size_class = meta.sizeclass();
    Length num_pages;
    if (size_class != 0) {
      Span* s = meta.span();
      if (p.value() != s->first_page()) continue;
      num_pages = tc_globals.sizemap().class_to_pages(size_class);
    } else {
      num_pages = meta.size();
    }
    // As documented, GetAllocatedSpans wants to avoid allocating more memory
    // for the output vector while holding the pageheap_lock. So, we stop
    // adding more entries after we reach its existing capacity. Note that the
    // count returned will still be the total number of allocated Spans.
    if (allocated_spans.capacity() > allocated_spans.size()) {
      allocated_spans.push_back(
          {p->start_uintptr(), num_pages.in_bytes(),
           tc_globals.sizemap().class_to_size(size_class)});
    }
    ++allocated_span_count;
    p = *p + num_pages - Length(1);
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
