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
#include "tcmalloc/internal/parameter_accessors.h"
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

extern "C" {

void TCMalloc_Internal_SnapshotCentralFreeLists(
    tcmalloc::tcmalloc_internal::CentralFreeListSnapshot* absl_nonnull
        snapshot) {
  using tcmalloc::tcmalloc_internal::CentralFreeListSpanSnapshot;
  using tcmalloc::tcmalloc_internal::CompactSizeClass;
  using tcmalloc::tcmalloc_internal::kAlignment;
  using tcmalloc::tcmalloc_internal::kNumClasses;
  using tcmalloc::tcmalloc_internal::kPageSize;
  using tcmalloc::tcmalloc_internal::Length;
  using tcmalloc::tcmalloc_internal::PageHeapSpinLockHolder;
  using tcmalloc::tcmalloc_internal::PageId;
  using tcmalloc::tcmalloc_internal::PageMeta;
  using tcmalloc::tcmalloc_internal::Span;
  using tcmalloc::tcmalloc_internal::tc_globals;

  tc_globals.InitIfNecessary();

  struct Candidate {
    PageId first_page;
    CompactSizeClass size_class;
    bool visited;
  };

  size_t span_cap = 0;
  {
    PageHeapSpinLockHolder l;
    const size_t total = tc_globals.span_allocator().stats().total;
    span_cap = total + total / 4 + 1024;
  }
  size_t bitmap_cap = std::max<size_t>(span_cap * 4, 4096);

  std::vector<Candidate> candidates;
  snapshot->class_seqs.resize(kNumClasses);
  static constexpr size_t kMaxSpanObjects =
      kPageSize / static_cast<size_t>(kAlignment);
  static constexpr size_t kMaxSpanBitmapWords = (kMaxSpanObjects + 63) / 64;
  uint64_t span_bitmap[kMaxSpanBitmapWords];

  for (int attempt = 0; attempt < 4; ++attempt) {
    candidates.reserve(span_cap);
    snapshot->spans.reserve(span_cap);
    snapshot->allocated_bitmaps.reserve(bitmap_cap);

    size_t needed_spans = 0;
    size_t needed_bitmap_words = 0;
    bool overflow = false;

    {
      PageHeapSpinLockHolder l;
      candidates.clear();
      snapshot->spans.clear();
      snapshot->allocated_bitmaps.clear();

      for (std::optional<PageId> p = PageId{0}; p.has_value();
           p = tc_globals.pagemap().get_next_set_page(*p)) {
        const PageMeta meta = tc_globals.pagemap().GetDescriptor(*p);
        if (!meta.valid()) {
          continue;
        }
        const CompactSizeClass sc = meta.sizeclass();
        Length num_pages;
        if (sc != 0) {
          Span* s = meta.span();
          if (*p != s->first_page()) {
            continue;
          }
          num_pages = tc_globals.sizemap().class_to_pages(sc);
          ++needed_spans;
          if (candidates.size() < candidates.capacity()) {
            candidates.push_back({*p, sc, false});
          } else {
            overflow = true;
          }
        } else {
          num_pages = meta.size();
        }
        p = *p + num_pages - Length(1);
      }

      if (!overflow) {
        std::sort(candidates.begin(), candidates.end(),
                  [](const Candidate& a, const Candidate& b) {
                    if (a.size_class != b.size_class) {
                      return a.size_class < b.size_class;
                    }
                    return a.first_page < b.first_page;
                  });

        size_t cand_pos = 0;
        for (size_t sc = 0; sc < kNumClasses; ++sc) {
          const size_t cand_start = cand_pos;
          while (cand_pos < candidates.size() &&
                 candidates[cand_pos].size_class == sc) {
            ++cand_pos;
          }
          const size_t cand_end = cand_pos;
          if (sc == 0 || tc_globals.sizemap().class_to_size(sc) == 0) {
            snapshot->class_seqs[sc] = 0;
            continue;
          }
          auto& cfl = tc_globals.transfer_cache().central_freelist(sc);
          const uint32_t cfl_objs_per_span = cfl.objects_per_span();
          snapshot->class_seqs[sc] = cfl.ForEachNonemptySpan(
              [&](const Span& span, size_t object_size,
                  uint32_t objects_per_span, int32_t bitmap_offset) {
                const PageId first = span.first_page();
                auto it = std::lower_bound(candidates.begin() + cand_start,
                                           candidates.begin() + cand_end, first,
                                           [](const Candidate& c, PageId p) {
                                             return c.first_page < p;
                                           });
                if (it != candidates.begin() + cand_end &&
                    it->first_page == first) {
                  it->visited = true;
                }
                const size_t words = (objects_per_span + 63) / 64;
                TC_ASSERT_LE(words, kMaxSpanBitmapWords);
                needed_bitmap_words += words;
                ++needed_spans;
                if (snapshot->spans.size() == snapshot->spans.capacity() ||
                    snapshot->allocated_bitmaps.size() + words >
                        snapshot->allocated_bitmaps.capacity()) {
                  overflow = true;
                  return;
                }
                for (size_t w = 0; w < words; ++w) {
                  span_bitmap[w] = ~uint64_t{0};
                }
                if (objects_per_span % 64 != 0) {
                  span_bitmap[words - 1] =
                      (uint64_t{1} << (objects_per_span % 64)) - 1;
                }
                span.ForEachFreeObject(object_size, objects_per_span,
                                       bitmap_offset, [&](size_t idx) {
                                         span_bitmap[idx / 64] &=
                                             ~(uint64_t{1} << (idx % 64));
                                       });
                const uint32_t word_offset =
                    static_cast<uint32_t>(snapshot->allocated_bitmaps.size());
                for (size_t w = 0; w < words; ++w) {
                  snapshot->allocated_bitmaps.push_back(span_bitmap[w]);
                }
                snapshot->spans.push_back(CentralFreeListSpanSnapshot{
                    .start_address = first.start_uintptr(),
                    .size_class = static_cast<uint32_t>(sc),
                    .objects_per_span = objects_per_span,
                    .allocated_objects = span.Allocated(),
                    .bitmap_word_offset = word_offset,
                });
              },
              [&]() {
                for (size_t idx = cand_start; idx < cand_end; ++idx) {
                  if (candidates[idx].visited) {
                    continue;
                  }
                  const PageId first = candidates[idx].first_page;
                  const PageMeta meta =
                      tc_globals.pagemap().GetDescriptor(first);
                  if (!meta.valid() || meta.sizeclass() != sc) {
                    continue;
                  }
                  Span* s = meta.span();
                  if (s->nonempty_index() != Span::kFullSpan) {
                    continue;
                  }
                  ++needed_spans;
                  if (snapshot->spans.size() == snapshot->spans.capacity()) {
                    overflow = true;
                    continue;
                  }
                  snapshot->spans.push_back(CentralFreeListSpanSnapshot{
                      .start_address = first.start_uintptr(),
                      .size_class = static_cast<uint32_t>(sc),
                      .objects_per_span = cfl_objs_per_span,
                      .allocated_objects = cfl_objs_per_span,
                      .bitmap_word_offset = UINT32_MAX,
                  });
                }
              });
        }
      }
    }

    if (!overflow) {
      return;
    }
    span_cap = std::max(span_cap * 2, needed_spans + needed_spans / 4 + 1024);
    bitmap_cap = std::max(bitmap_cap * 2,
                          needed_bitmap_words + needed_bitmap_words / 4 + 4096);
  }
}

}  // extern "C"

GOOGLE_MALLOC_SECTION_END
