// Copyright 2022 The TCMalloc Authors
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

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <new>
#include <vector>

#include "fuzztest/fuzztest.h"
#include "absl/types/span.h"
#include "tcmalloc/common.h"
#include "tcmalloc/internal/logging.h"
#include "tcmalloc/pages.h"
#include "tcmalloc/span.h"

namespace tcmalloc::tcmalloc_internal {
namespace {

void FuzzSpan(const std::string& s) {
  // Extract fuzz input into 6 integers.
  //
  // TODO(b/271282540): Strongly type input.
  size_t state[6];
  if (s.size() != sizeof(state)) {
    return;
  }
  memcpy(state, s.data(), sizeof(state));

  const size_t object_size = state[0];
  const size_t num_pages = state[1];
  const size_t num_to_move = state[2];
  const uint32_t max_span_cache_size =
      std::clamp(state[3] & 0xF, Span::kCacheSize, Span::kLargeCacheSize);

  // Use a larger cache array only when we are using a maximum allowed cache
  // size.
  const uint32_t max_span_cache_array_size =
      max_span_cache_size == Span::kLargeCacheSize ? Span::kLargeCacheArraySize
                                                   : max_span_cache_size;
  const uint64_t alloc_time = state[5];

  if (!SizeMap::IsValidSizeClass(object_size, num_pages, num_to_move)) {
    // Invalid size class configuration, but ValidSizeClass detected that.
    return;
  }

  const auto pages = Length(num_pages);
  const size_t objects_per_span = pages.in_bytes() / object_size;
  const size_t span_size = Span::CalcSizeOf(max_span_cache_array_size);
  const uint32_t size_reciprocal = Span::CalcReciprocal(object_size);

  const size_t initial_objects_at_build =
      std::min(objects_per_span, state[3] >> 4);

  void* mem;
  int res = posix_memalign(&mem, kPageSize, pages.in_bytes());
  TC_CHECK_EQ(res, 0);

  void* buf = ::operator new(span_size, std::align_val_t(alignof(Span)));
  Span* span = new (buf) Span(Range(PageIdContaining(mem), pages));

  std::vector<void*> ptrs;
  ptrs.resize(initial_objects_at_build);

  TC_CHECK_EQ(
      span->BuildFreelist(object_size, objects_per_span, absl::MakeSpan(ptrs),
                          max_span_cache_size, alloc_time),
      initial_objects_at_build);
  TC_CHECK_EQ(span->Allocated(), initial_objects_at_build);

  ptrs.reserve(objects_per_span);
  while (ptrs.size() < objects_per_span) {
    size_t want = std::min(num_to_move, objects_per_span - ptrs.size());
    TC_CHECK_GT(want, 0);
    void* batch[kMaxObjectsToMove];
    TC_CHECK(!span->FreelistEmpty(object_size));
    size_t n = span->FreelistPopBatch(absl::MakeSpan(batch, want), object_size);

    TC_CHECK_GT(n, 0);
    TC_CHECK_LE(n, want);
    TC_CHECK_LE(n, kMaxObjectsToMove);
    ptrs.insert(ptrs.end(), batch, batch + n);
  }

  TC_CHECK(span->FreelistEmpty(object_size));
  TC_CHECK_EQ(ptrs.size(), objects_per_span);
  TC_CHECK_EQ(ptrs.size(), span->Allocated());

  for (size_t i = 0, popped = ptrs.size(); i < popped; ++i) {
    bool ok = span->FreelistPush(ptrs[i], object_size, size_reciprocal,
                                 max_span_cache_size);
    TC_CHECK_EQ(ok, i != popped - 1);
    // If the freelist becomes full, then the span does not actually push the
    // element onto the freelist.
    //
    // For single object spans, the freelist always stays "empty" as a result.
    TC_CHECK(popped == 1 || !span->FreelistEmpty(object_size));
  }

  if (!span->UseBitmapForSize(object_size) &&
      max_span_cache_size == Span::kLargeCacheSize) {
    TC_CHECK_EQ(span->AllocTime(object_size, max_span_cache_size), alloc_time);
  } else {
    TC_CHECK_EQ(span->AllocTime(object_size, max_span_cache_size), 0);
  }

  free(mem);
  ::operator delete(buf, std::align_val_t(alignof(Span)));
}

FUZZ_TEST(SpanTest, FuzzSpan)
    ;

}  // namespace
}  // namespace tcmalloc::tcmalloc_internal
