// Copyright 2020 The TCMalloc Authors
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

#include <stddef.h>

#include <limits>
#include <new>
#include <tuple>

#include "gtest/gtest.h"
#include "tcmalloc/malloc_extension.h"

namespace tcmalloc {
namespace {

class HotColdNewInvalidSizeTest
    : public testing::TestWithParam<std::tuple<hot_cold_t, bool>> {};

TEST_P(HotColdNewInvalidSizeTest, InvalidSizeFails) {
#if defined(ABSL_HAVE_ADDRESS_SANITIZER) || \
    defined(ABSL_HAVE_MEMORY_SANITIZER) || defined(ABSL_HAVE_THREAD_SANITIZER)
  GTEST_SKIP() << "skipping large allocation tests on sanitizers";
#endif
  constexpr size_t kBadSize = std::numeric_limits<size_t>::max();
  const auto [hot_cold, is_array] = GetParam();
  if (is_array) {
    EXPECT_DEATH((void)::operator new[](kBadSize, hot_cold), ".*");
  } else {
    EXPECT_DEATH((void)::operator new(kBadSize, hot_cold), ".*");
  }
}

// The invalid size below is rejected before the hot/cold threshold matters, so
// only test the two extremes (cold and hot) across scalar and array new.
INSTANTIATE_TEST_SUITE_P(HotColdNew, HotColdNewInvalidSizeTest,
                         testing::Combine(testing::Values(hot_cold_t{0},
                                                          hot_cold_t{255}),
                                          testing::Bool()));

TEST(HotColdNew, InvalidSizeNothrow) {
  constexpr size_t kBadSize = std::numeric_limits<size_t>::max();
  EXPECT_EQ(::operator new(kBadSize, std::nothrow, hot_cold_t{0}), nullptr);
  EXPECT_EQ(::operator new(kBadSize, std::nothrow, hot_cold_t{128}), nullptr);
  EXPECT_EQ(::operator new(kBadSize, std::nothrow, hot_cold_t{255}), nullptr);
  EXPECT_EQ(::operator new[](kBadSize, std::nothrow, hot_cold_t{0}), nullptr);
  EXPECT_EQ(::operator new[](kBadSize, std::nothrow, hot_cold_t{128}), nullptr);
  EXPECT_EQ(::operator new[](kBadSize, std::nothrow, hot_cold_t{255}), nullptr);
}

}  // namespace
}  // namespace tcmalloc
