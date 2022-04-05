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

#include <string>

#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "absl/random/random.h"
#include "absl/strings/numbers.h"
#include "tcmalloc/internal/linked_list.h"
#include "tcmalloc/testing/testutil.h"

namespace tcmalloc {
namespace {

using tcmalloc_internal::SLL_Pop;
using tcmalloc_internal::SLL_Push;

struct PeakStats {
  size_t backing;
  size_t application;
};

PeakStats GetPeakStats() {
  // TODO(ckennelly): Parse this with protobuf directly
  PeakStats ret;
  const std::string buf = GetStatsInPbTxt();
  constexpr absl::string_view backing_needle = "peak_backed: ";
  constexpr absl::string_view application_needle = "peak_application_demand: ";

  auto parse = [](absl::string_view buf, absl::string_view needle) {
    auto pos = buf.find(needle);
    EXPECT_NE(pos, absl::string_view::npos);
    pos += needle.size();
    auto stop = buf.find(' ', pos);
    if (stop != absl::string_view::npos) {
      stop -= pos;
    }
    size_t ret;
    EXPECT_TRUE(absl::SimpleAtoi(buf.substr(pos, stop), &ret))
        << buf.substr(pos, stop);
    return ret;
  };

  ret.backing = parse(buf, backing_needle);
  ret.application = parse(buf, application_needle);

  return ret;
}

TEST(RealizedFragmentation, Accuracy) {
#ifndef NDEBUG
  GTEST_SKIP() << "Skipping test under debug build for performance";
#endif

  const PeakStats starting = GetPeakStats();
  // We have allocated at least once up to this point.
  ASSERT_GT(starting.backing, 0);

  // Since application data is sampled, allow wider error bars.
  constexpr double kBackingTolerance = 0.20;
  constexpr double kApplicationTolerance = 0.25;
  absl::BitGen rng;

  // Allocate many 2MB allocations, as to trigger a new high water mark, then
  // deallocate.
  constexpr size_t kLargeTarget = 1 << 29;
  constexpr size_t kLargeSize = 2 << 20;
  void* large_list = nullptr;

  for (size_t total = 0; total < kLargeTarget; total += kLargeSize) {
    SLL_Push(&large_list, ::operator new(kLargeSize));
  }

  const PeakStats peak0 = GetPeakStats();

  EXPECT_NEAR(peak0.backing, starting.backing + kLargeTarget,
              (starting.backing + kLargeTarget) * kBackingTolerance);
  EXPECT_NEAR(peak0.application, starting.application + kLargeTarget,
              (starting.application + kLargeTarget) * kApplicationTolerance);

  while (large_list != nullptr) {
    void* object = SLL_Pop(&large_list);
    sized_delete(object, kLargeSize);
  }

  // Allocate many small alocations, as to trigger another high water mark.
  // Deallocate half of these allocations, but fragmentation should remain high.
  constexpr size_t kSmallTarget = kLargeTarget * 2;
  constexpr size_t kSmallSize = 1024;
  void* small_list_keep = nullptr;
  int kept = 0;
  void* small_list_free = nullptr;
  int freed = 0;

  for (size_t total = 0; total < kSmallTarget; total += kSmallSize) {
    void* object = ::operator new(kSmallSize);
    if (absl::Bernoulli(rng, 0.5)) {
      SLL_Push(&small_list_keep, object);
      kept++;
    } else {
      SLL_Push(&small_list_free, object);
      freed++;
    }
  }

  const PeakStats peak1 = GetPeakStats();

  EXPECT_NEAR(peak1.backing, starting.backing + kSmallTarget,
              (starting.backing + kSmallTarget) * kBackingTolerance);
  EXPECT_NEAR(peak1.application, starting.application + kSmallTarget,
              (starting.application + kSmallTarget) * kApplicationTolerance);

  while (small_list_free != nullptr) {
    void* object = SLL_Pop(&small_list_free);
    sized_delete(object, kSmallSize);
  }

  // Allocate many 2MB allocations, as to trigger another high water mark.
  // Fragmentation should continue to be high due to partial spans from the
  // previous round.
  for (size_t total = 0; total < 2 * kLargeTarget; total += kLargeSize) {
    SLL_Push(&large_list, ::operator new(kLargeSize));
  }

  const PeakStats peak2 = GetPeakStats();

  const double expected_backing =
      starting.backing + kSmallTarget + 2 * kLargeTarget;
  const double expected_application =
      starting.backing + kSmallSize * kept + 2 * kLargeTarget;

  EXPECT_NEAR(peak2.backing, expected_backing,
              expected_backing * kBackingTolerance);
  EXPECT_NEAR(peak2.application, expected_application,
              expected_application * kApplicationTolerance);

  while (large_list != nullptr) {
    void* object = SLL_Pop(&large_list);
    sized_delete(object, kLargeSize);
  }

  while (small_list_keep != nullptr) {
    void* object = SLL_Pop(&small_list_keep);
    sized_delete(object, kSmallSize);
  }
}

}  // namespace
}  // namespace tcmalloc
