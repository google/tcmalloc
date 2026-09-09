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

void PageMap::RegisterSizeClass(Span* span, size_t sc) {
  const PageId first = span->first_page();
  const PageId last = span->last_page();
  TC_ASSERT_EQ(GetDescriptor(first), span);
  for (PageId p = first; p <= last; ++p) {
    map_.set_with_sizeclass(p.index(), span, sc);
  }
}

void PageMap::UnregisterSizeClass(Span* span) {
  const PageId first = span->first_page();
  const PageId last = span->last_page();
  TC_ASSERT_EQ(GetDescriptor(first), span);
  for (PageId p = first; p <= last; ++p) {
    map_.clear_sizeclass(p.index());
  }
}

int PageMap::GetAllocatedSpans(
    std::vector<tcmalloc::malloc_tracing_extension::AllocatedAddressRanges::
                    SpanDetails>& allocated_spans) {
  PageHeapSpinLockHolder l;
  int allocated_span_count = 0;
  for (std::optional<uintptr_t> i = 0; i.has_value();
       i = map_.get_next_set_page(i.value())) {
    PageId page_id = PageId{i.value()};
    Span* s = GetDescriptor(page_id);
    if (s == nullptr || s == &tc_globals.invalid_span()) {
      continue;
    }
    // Free'd up Span that's not yet removed from PageMap.
    if (page_id < s->first_page() || s->last_page() < page_id) continue;
    CompactSizeClass size_class = sizeclass(page_id);
    TC_ASSERT_EQ(s->first_page().index(), i);
    // As documented, GetAllocatedSpans wants to avoid allocating more memory
    // for the output vector while holding the pageheap_lock. So, we stop
    // adding more entries after we reach its existing capacity. Note that the
    // count returned will still be the total number of allocated Spans.
    if (allocated_spans.capacity() > allocated_spans.size()) {
      allocated_spans.push_back({s->first_page().start_uintptr(),
                                 s->bytes_in_span(),
                                 Static::sizemap().class_to_size(size_class)});
    }
    ++allocated_span_count;
    i = s->last_page().index();
  }
  return allocated_span_count;
}

void* MetaDataAlloc(size_t bytes) { return tc_globals.arena().Alloc(bytes); }

}  // namespace tcmalloc_internal
}  // namespace tcmalloc
GOOGLE_MALLOC_SECTION_END
