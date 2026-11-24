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

namespace tcmalloc::tcmalloc_internal {
namespace {

TEST(LengthDeathTest, UnderflowChecks) {
  const Length kBig = Length(5);
  const Length kSmall = Length(1);

  Length tmp = kSmall;
  EXPECT_DEBUG_DEATH(tmp -= kBig, "n_.*n_");
  EXPECT_DEBUG_DEATH(benchmark::DoNotOptimize(kSmall - kBig), "n_.*n_");
}

TEST(PagesTest, ClassifyReleasedPages) {
  EXPECT_EQ(ClassifyReleasedPages(Length(0), Length(0)), ReleasedPages::kNone);
  EXPECT_EQ(ClassifyReleasedPages(Length(0), Length(5)), ReleasedPages::kNone);
  EXPECT_EQ(ClassifyReleasedPages(Length(1), Length(5)),
            ReleasedPages::kPartial);
  EXPECT_EQ(ClassifyReleasedPages(Length(4), Length(5)),
            ReleasedPages::kPartial);
  EXPECT_EQ(ClassifyReleasedPages(Length(5), Length(5)), ReleasedPages::kAll);
  EXPECT_DEBUG_DEATH(
      benchmark::DoNotOptimize(ClassifyReleasedPages(Length(5), Length(1))),
      "released.*total");
}

}  // namespace
}  // namespace tcmalloc::tcmalloc_internal
