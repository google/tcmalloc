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

#include "tcmalloc/internal/top_k_buffer.h"

#include <algorithm>
#include <functional>
#include <vector>

#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "absl/random/random.h"

namespace tcmalloc::tcmalloc_internal {
namespace {

using ::testing::ElementsAre;
using ::testing::UnorderedElementsAre;

TEST(TopKBufferTest, EmptyAndUnderLimit) {
  TopKBuffer<int, 4, std::less<int>> buffer;
  EXPECT_EQ(buffer.limit(), 4);
  EXPECT_EQ(buffer.size(), 0);
  buffer.Finish();
  EXPECT_EQ(buffer.size(), 0);
  EXPECT_EQ(buffer.begin(), buffer.end());

  buffer.Push(30);
  buffer.Push(10);
  buffer.Push(20);
  buffer.Finish();
  EXPECT_EQ(buffer.size(), 3);
  EXPECT_EQ(buffer[0], 30);
  const auto& const_buffer = buffer;
  EXPECT_EQ(const_buffer[1], 10);
  EXPECT_THAT(std::vector<int>(buffer.begin(), buffer.end()),
              UnorderedElementsAre(10, 20, 30));
}

TEST(TopKBufferTest, CompactsAndFiltersWithCutoff) {
  TopKBuffer<int, 8, std::less<int>> buffer(3);
  EXPECT_EQ(buffer.limit(), 3);

  for (int x : {9, 4, 7, 1, 8, 2, 6, 3, 5, 0}) {
    buffer.Push(x);
  }
  buffer.Finish();

  const TopKBuffer<int, 8, std::less<int>>& const_buffer = buffer;
  std::vector<int> result(const_buffer.begin(), const_buffer.end());
  std::sort(result.begin(), result.end());
  EXPECT_THAT(result, ElementsAre(0, 1, 2));
}

TEST(TopKBufferTest, DuplicateValuesAroundCutoff) {
  TopKBuffer<int, 4, std::greater<int>> buffer(4);
  for (int x : {5, 5, 5, 5, 5, 5, 1, 2, 5, 9, 5}) {
    buffer.Push(x);
  }
  buffer.Finish();

  std::vector<int> result(buffer.begin(), buffer.end());
  std::sort(result.begin(), result.end(), std::greater<int>{});
  EXPECT_THAT(result, ElementsAre(9, 5, 5, 5));
}

TEST(TopKBufferTest, MatchesReferenceSortAcrossRandomInputs) {
  absl::BitGen rng;
  for (int limit : {1, 2, 5, 16}) {
    for (int n : {0, 1, 3, 10, 32, 100}) {
      TopKBuffer<int, 16, std::less<int>> buffer(limit);
      std::vector<int> expected;
      expected.reserve(n);
      for (int i = 0; i < n; ++i) {
        int val = absl::Uniform<int>(rng, 0, 50);
        buffer.Push(val);
        expected.push_back(val);
      }
      buffer.Finish();

      std::sort(expected.begin(), expected.end());
      if (static_cast<int>(expected.size()) > limit) {
        expected.resize(limit);
      }

      std::vector<int> actual(buffer.begin(), buffer.end());
      std::sort(actual.begin(), actual.end());
      EXPECT_EQ(actual, expected);
    }
  }
}

}  // namespace
}  // namespace tcmalloc::tcmalloc_internal
