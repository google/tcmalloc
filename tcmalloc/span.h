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
//
// A Span is a contiguous run of pages.

#ifndef TCMALLOC_SPAN_H_
#define TCMALLOC_SPAN_H_

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <cassert>
#include <cstddef>

#include "tcmalloc/common.h"
#include "tcmalloc/internal/config.h"
#include "tcmalloc/internal/logging.h"
#include "tcmalloc/pages.h"

GOOGLE_MALLOC_SECTION_BEGIN
namespace tcmalloc {
namespace tcmalloc_internal {

// Metadata for a span, stored inline in the pagemap.
union SpanMeta {
  // Covers up to 512M pages, or 4TB with 8K pages.
  static constexpr size_t kNumPagesBits = 29;
  static constexpr size_t kMaxNumPages = 1ul << kNumPagesBits;

  // Covers up to 2M spans in CFL nonempty list.
  static constexpr size_t kFullHeapIndexBits = 21;
#ifdef NDEBUG
  static constexpr size_t kHeapIndexBits = kFullHeapIndexBits;
#else
  // For testing of heap index overflows.
  static constexpr size_t kHeapIndexBits = 5;
#endif

  static_assert(sizeof(CompactSizeClass) == 1);

  // Small object span metadata.
  struct {
    uint32_t size_class : 8;
    // Truncated index in CFL's nonempty_ array (if is_in_cfl == 1).
    // Otherwise, it's page offset from the start of the span.
    // Truncating the index is OK since we have a way to find the non-truncated
    // index in the very unlikely case the index was truncated.
    // In debug mode we use very few bits for the index, to test overflows.
    uint32_t heap_index_or_page_offset : kHeapIndexBits;
#ifndef NDEBUG
    uint32_t unused : kFullHeapIndexBits - kHeapIndexBits;
#endif
    uint32_t is_in_cfl : 1;   // span is in CFL nonempty list
    uint32_t is_donated : 1;  // donated from page heap
    uint32_t is_small : 1;    // = 1 for small objects
  };
  // Large object span metadata.
  struct {
    // If is_sampled == 0, number of pages in the span. If the span is larger
    // than kMaxNumPages, this field is 0 and the size is stored in the huge
    // page metadata.
    // If is_sampled == 1, the sampled allocation index.
    uint32_t num_pages_or_sampled_index : kNumPagesBits;
    uint32_t is_sampled : 1;   // sampled object
    uint32_t is_donated1 : 1;  // donated from page heap
    uint32_t is_small1 : 1;    // = 0 for large objects
  };

  static constexpr SpanMeta Freed() {
    SpanMeta m{};
    m.is_donated1 = 1;
    return m;
  }

  constexpr bool IsValid() const {
    return is_small || num_pages_or_sampled_index > 0 || is_sampled;
  }

  constexpr bool IsFreed() const {
    return !is_small1 && num_pages_or_sampled_index == 0 && !is_sampled &&
           is_donated1;
  }

  PageId SmallSpanStart(PageId p) const {
    TC_ASSERT(is_small);
    return p - Length(is_in_cfl ? 0 : heap_index_or_page_offset);
  }
};

static_assert(sizeof(SpanMeta) == 4);

// Denominator for bitmap scaling factor. The idea is that instead of dividing
// by N we multiply by M = kBitmapScalingDenominator / N and round the resulting
// value.
inline constexpr size_t kBitmapScalingDenominator = 1 << 30;

enum AccessDensityPrediction {
  // Predict that the span would be sparsely-accessed.
  kSparse = 0,
  // Predict that the span would be densely-accessed.
  kDense = 1,
  kPredictionCounts
};

struct SpanAllocInfo {
  size_t objects_per_span;
  AccessDensityPrediction density;
};

[[nodiscard]] inline uint32_t CalcReciprocal(size_t size) {
  TC_ASSERT_GT(size, 0);
  return kBitmapScalingDenominator / size;
}

[[nodiscard]] ABSL_ATTRIBUTE_ALWAYS_INLINE inline uint32_t OffsetToIdx(
    uintptr_t offset, uint32_t reciprocal) {
  return static_cast<uint32_t>(
      (offset * reciprocal + kBitmapScalingDenominator / 2) /
      kBitmapScalingDenominator);
}

}  // namespace tcmalloc_internal
}  // namespace tcmalloc
GOOGLE_MALLOC_SECTION_END

#endif  // TCMALLOC_SPAN_H_
