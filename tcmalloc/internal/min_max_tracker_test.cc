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

#include "tcmalloc/internal/min_max_tracker.h"

#include <stdint.h>

#include "gtest/gtest.h"
#include "absl/time/time.h"
#include "tcmalloc/internal/clock.h"
#include "tcmalloc/internal/hardware_pages.h"

namespace tcmalloc {
namespace tcmalloc_internal {
namespace {

class MinMaxTrackerTest : public testing::Test {
 protected:
  void Advance(absl::Duration d) {
    clock_ += absl::ToDoubleSeconds(d) * GetFakeClockFrequency();
  }

  static int64_t FakeClock() { return clock_; }

  static double GetFakeClockFrequency() {
    return absl::ToDoubleNanoseconds(absl::Seconds(2));
  }

 private:
  static int64_t clock_;
};

int64_t MinMaxTrackerTest::clock_{0};

TEST_F(MinMaxTrackerTest, Works) {
  const absl::Duration kDuration = absl::Seconds(2);
  MinMaxTracker<HardwareLength> tracker{
      Clock{.now = FakeClock, .freq = GetFakeClockFrequency}, kDuration};

  tracker.Report(NHardwarePages(0));
  EXPECT_EQ(NHardwarePages(0), tracker.MaxOverTime(kDuration));
  EXPECT_EQ(NHardwarePages(0), tracker.MinOverTime(kDuration));

  tracker.Report(NHardwarePages(10));
  EXPECT_EQ(NHardwarePages(10), tracker.MaxOverTime(kDuration));
  EXPECT_EQ(NHardwarePages(0), tracker.MinOverTime(kDuration));

  tracker.Report(NHardwarePages(5));
  EXPECT_EQ(NHardwarePages(10), tracker.MaxOverTime(kDuration));
  EXPECT_EQ(NHardwarePages(0), tracker.MinOverTime(kDuration));

  tracker.Report(NHardwarePages(100));
  EXPECT_EQ(NHardwarePages(100), tracker.MaxOverTime(kDuration));
  EXPECT_EQ(NHardwarePages(0), tracker.MinOverTime(kDuration));

  // Some tests for advancing time
  Advance(kDuration / 3);
  tracker.Report(NHardwarePages(2));
  EXPECT_EQ(NHardwarePages(2), tracker.MaxOverTime(absl::Nanoseconds(1)));
  EXPECT_EQ(NHardwarePages(100), tracker.MaxOverTime(kDuration / 2));
  EXPECT_EQ(NHardwarePages(100), tracker.MaxOverTime(kDuration));
  EXPECT_EQ(NHardwarePages(2), tracker.MinOverTime(absl::Nanoseconds(1)));
  EXPECT_EQ(NHardwarePages(0), tracker.MinOverTime(kDuration / 2));
  EXPECT_EQ(NHardwarePages(0), tracker.MinOverTime(kDuration));

  Advance(kDuration / 3);
  tracker.Report(NHardwarePages(5));
  EXPECT_EQ(NHardwarePages(5), tracker.MaxOverTime(absl::Nanoseconds(1)));
  EXPECT_EQ(NHardwarePages(5), tracker.MaxOverTime(kDuration / 2));
  EXPECT_EQ(NHardwarePages(100), tracker.MaxOverTime(kDuration));
  EXPECT_EQ(NHardwarePages(5), tracker.MinOverTime(absl::Nanoseconds(1)));
  EXPECT_EQ(NHardwarePages(2), tracker.MinOverTime(kDuration / 2));
  EXPECT_EQ(NHardwarePages(0), tracker.MinOverTime(kDuration));

  // This should annihilate everything.
  Advance(kDuration * 2);
  tracker.Report(NHardwarePages(1));
  EXPECT_EQ(NHardwarePages(1), tracker.MaxOverTime(absl::Nanoseconds(1)));
  EXPECT_EQ(NHardwarePages(1), tracker.MinOverTime(absl::Nanoseconds(1)));
  EXPECT_EQ(NHardwarePages(1), tracker.MaxOverTime(kDuration));
  EXPECT_EQ(NHardwarePages(1), tracker.MinOverTime(kDuration));
}

}  // namespace
}  // namespace tcmalloc_internal
}  // namespace tcmalloc
