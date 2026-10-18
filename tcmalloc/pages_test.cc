// Copyright 2023 The TCMalloc Authors
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

#include "tcmalloc/pages.h"

#include "benchmark/benchmark.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "tcmalloc/huge_pages.h"

namespace tcmalloc::tcmalloc_internal {
namespace {

TEST(LengthDeathTest, UnderflowChecks) {
  const Length kBig = Length(5);
  const Length kSmall = Length(1);

  Length tmp = kSmall;
  EXPECT_DEBUG_DEATH(tmp -= kBig, "n_.*n_");
  EXPECT_DEBUG_DEATH(benchmark::DoNotOptimize(kSmall - kBig), "n_.*n_");
}

TEST(HugeRangeTest, EmptyRangeDoesNotIntersect) {
  const HugePage p = HugePage{10};
  const HugeRange non_empty = HugeRange::Make(p, NHugePages(4));
  const HugeRange empty_at_start = HugeRange::Make(p, NHugePages(0));
  const HugeRange empty_inside = HugeRange::Make(HugePage{12}, NHugePages(0));

  EXPECT_FALSE(non_empty.intersects(empty_at_start));
  EXPECT_FALSE(empty_at_start.intersects(non_empty));
  EXPECT_FALSE(non_empty.intersects(empty_inside));
  EXPECT_FALSE(empty_inside.intersects(non_empty));
  EXPECT_FALSE(empty_at_start.intersects(empty_at_start));
}

TEST(HugeRangeTest, MaxPageNumberBoundary) {
  const HugePage last_page = HugePage{HugePage::kMaxPageNumber};
  const HugeRange top_range = HugeRange::Make(last_page, NHugePages(1));
  const HugeRange prev_range =
      HugeRange::Make(HugePage{HugePage::kMaxPageNumber - 1}, NHugePages(1));
  const HugeRange top_two =
      HugeRange::Make(HugePage{HugePage::kMaxPageNumber - 1}, NHugePages(2));
  const HugeRange zero_range = HugeRange::Make(HugePage{0}, NHugePages(1));

  EXPECT_EQ(top_range.byte_len(), kHugePageSize);
  EXPECT_TRUE(top_range.contains(last_page));
  EXPECT_TRUE(top_range.contains(top_range));
  EXPECT_TRUE(top_two.contains(top_range));
  EXPECT_FALSE(top_range.contains(top_two));
  EXPECT_FALSE(top_range.contains(HugeRange{last_page, NHugePages(2)}));
  EXPECT_TRUE(prev_range.precedes(top_range));
  EXPECT_FALSE(top_range.precedes(zero_range));
}

}  // namespace
}  // namespace tcmalloc::tcmalloc_internal
