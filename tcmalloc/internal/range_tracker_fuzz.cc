// Copyright 2026 The TCMalloc Authors
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

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "fuzztest/fuzztest.h"
#include "absl/types/span.h"
#include "tcmalloc/internal/range_tracker.h"

namespace tcmalloc::tcmalloc_internal {
namespace {

void FuzzBitmapPopBatch(const std::array<bool, 128>& bits_to_set,
                        size_t limit) {
  constexpr size_t N = 128;
  Bitmap<N> map1;
  Bitmap<N> map2;

  for (size_t i = 0; i < N; ++i) {
    if (bits_to_set[i]) {
      map1.SetBit(i);
      map2.SetBit(i);
    }
  }

  std::vector<size_t> offsets1, offsets2;
  size_t popped1 =
      map1.PopBatch([&](size_t v) { offsets1.push_back(v); }, limit);
  EXPECT_EQ(offsets1.size(), popped1);

  while (!map2.IsZero() && offsets2.size() < limit) {
    size_t offset = map2.FindSet(0);
    offsets2.push_back(offset);
    map2.ClearBit(offset);
  }

  EXPECT_THAT(offsets1, testing::Eq(offsets2));

  for (size_t i = 0; i < N; ++i) {
    EXPECT_EQ(map1.GetBit(i), map2.GetBit(i));
  }
  EXPECT_EQ(map1.IsZero(), map2.IsZero());
}

FUZZ_TEST(BitmapFuzzTest, FuzzBitmapPopBatch)
    .WithDomains(fuzztest::Arbitrary<std::array<bool, 128>>(),
                 fuzztest::InRange<size_t>(0, 128));

void FuzzBitmapCountBits(const std::array<bool, 253>& bits_to_set, size_t start,
                         size_t length) {
  constexpr size_t N = 253;
  if (start > N || start + length > N) {
    return;
  }

  Bitmap<N> map;
  for (size_t i = 0; i < N; ++i) {
    if (bits_to_set[i]) {
      map.SetBit(i);
    }
  }

  size_t expected = 0;
  for (size_t j = 0; j < length; j++) {
    size_t idx = start + j;
    if (bits_to_set[idx]) {
      expected++;
    }
  }

  EXPECT_EQ(expected, map.CountBits(start, length));
}

FUZZ_TEST(BitmapFuzzTest, FuzzBitmapCountBits)
    .WithDomains(fuzztest::Arbitrary<std::array<bool, 253>>(),
                 fuzztest::InRange<size_t>(0, 253),
                 fuzztest::InRange<size_t>(0, 253));

}  // namespace
}  // namespace tcmalloc::tcmalloc_internal
