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

void FuzzBitmapPopBatch(const std::vector<bool>& bits_to_set, size_t limit) {
  constexpr size_t N = 128;
  Bitmap<N> map1;
  Bitmap<N> map2;

  for (size_t i = 0; i < bits_to_set.size() && i < N; ++i) {
    if (bits_to_set[i]) {
      map1.SetBit(i);
      map2.SetBit(i);
    }
  }

  uint8_t offsets1[N];
  size_t popped1 = map1.PopBatch(offsets1, limit);

  uint8_t offsets2[N];
  size_t popped2 = 0;
  while (!map2.IsZero() && popped2 < limit) {
    size_t offset = map2.FindSet(0);
    offsets2[popped2++] = offset;
    map2.ClearBit(offset);
  }

  EXPECT_THAT(absl::MakeSpan(offsets1, popped1),
              testing::ElementsAreArray(offsets2, popped2));

  for (size_t i = 0; i < N; ++i) {
    EXPECT_EQ(map1.GetBit(i), map2.GetBit(i));
  }
  EXPECT_EQ(map1.IsZero(), map2.IsZero());
}

FUZZ_TEST(BitmapFuzzTest, FuzzBitmapPopBatch)
    .WithDomains(
        fuzztest::VectorOf(fuzztest::Arbitrary<bool>()).WithMaxSize(128),
        fuzztest::InRange<size_t>(0, 128));

}  // namespace
}  // namespace tcmalloc::tcmalloc_internal
