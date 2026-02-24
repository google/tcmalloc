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

#include "tcmalloc/huge_page_subrelease.h"

#include <stddef.h>
#include <stdio.h>

#include <cstdint>
#include <string>

#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "absl/strings/string_view.h"
#include "absl/time/time.h"
#include "tcmalloc/huge_pages.h"
#include "tcmalloc/internal/clock.h"
#include "tcmalloc/internal/logging.h"
#include "tcmalloc/pages.h"

using tcmalloc::tcmalloc_internal::Length;
using testing::StrEq;

#include "tcmalloc/testing/testutil.h"

namespace tcmalloc {
namespace tcmalloc_internal {
namespace {

class StatsTrackerTest : public testing::Test {
 private:
  static int64_t clock_;
  static int64_t FakeClock() { return clock_; }
  static void ResetClock() { clock_ = 0; }
  static double GetFakeClockFrequency() {
    return absl::ToDoubleNanoseconds(absl::Seconds(2));
  }

  // Helper struct whose sole purpose is to call ResetClock() upon construction.
  struct ClockResetter {
    ClockResetter() { ResetClock(); }
  };

  // This ensures ResetClock() is called before the constructor runs
  // (where tracker_ inits), so its time series has the same initial state
  // (e.g., first epoch)
  ClockResetter clock_resetter_;

 protected:
  static constexpr absl::Duration kWindow = absl::Minutes(10);

  // Epoch length: 0.5 min (i.e., 10-min window in 20 slots). The tracker can
  // hold records longer than 10 mins, and we expect it to account the epoch
  // coverage correctly.
  using StatsTrackerType = SubreleaseStatsTracker<20>;
  StatsTrackerType tracker_{
      Clock{.now = FakeClock, .freq = GetFakeClockFrequency}, kWindow,
      /*summary_interval=*/absl::Minutes(5)};

  void Advance(absl::Duration d) {
    clock_ += static_cast<int64_t>(absl::ToDoubleSeconds(d) *
                                   GetFakeClockFrequency());
  }

  // Generates four data points for the tracker that represent "interesting"
  // points (i.e., min/max pages demand, min/max hugepages).
  void GenerateInterestingPoints(Length num_pages, HugeLength num_hugepages,
                                 Length num_free_pages);

  // Generates a data point with a particular amount of demand pages, while
  // ignoring the specific number of hugepages.
  void GenerateDemandPoint(Length num_pages, Length num_free_pages);
};

int64_t StatsTrackerTest::clock_{0};

void StatsTrackerTest::GenerateInterestingPoints(Length num_pages,
                                                 HugeLength num_hugepages,
                                                 Length num_free_pages) {
  for (int i = 0; i <= 1; ++i) {
    for (int j = 0; j <= 1; ++j) {
      StatsTrackerType::SubreleaseStats stats;
      stats.num_pages = num_pages + Length((i == 0) ? 4 : 8 * j);
      stats.free_pages = num_free_pages + Length(10 * i + j);
      stats.unmapped_pages = Length(10);
      stats.used_pages_in_subreleased_huge_pages = num_pages;
      stats.huge_pages[StatsTrackerType::kRegular] =
          num_hugepages + ((i == 1) ? NHugePages(4) : NHugePages(8) * j);
      stats.huge_pages[StatsTrackerType::kDonated] = num_hugepages;
      stats.huge_pages[StatsTrackerType::kPartialReleased] = NHugePages(i);
      stats.huge_pages[StatsTrackerType::kReleased] = NHugePages(j);
      tracker_.Report(stats);
    }
  }
}

void StatsTrackerTest::GenerateDemandPoint(Length num_pages,
                                           Length num_free_pages) {
  HugeLength hp = NHugePages(1);
  StatsTrackerType::SubreleaseStats stats;
  stats.num_pages = num_pages;
  stats.free_pages = num_free_pages;
  stats.unmapped_pages = Length(0);
  stats.used_pages_in_subreleased_huge_pages = Length(0);
  stats.huge_pages[StatsTrackerType::kRegular] = hp;
  stats.huge_pages[StatsTrackerType::kDonated] = hp;
  stats.huge_pages[StatsTrackerType::kPartialReleased] = hp;
  stats.huge_pages[StatsTrackerType::kReleased] = hp;
  tracker_.Report(stats);
}

// Tests that the tracker aggregates all data correctly. The output is tested by
// comparing the text output of the tracker. While this is a bit verbose, it is
// much cleaner than extracting and comparing all data manually.
TEST_F(StatsTrackerTest, Works) {
  // Epoch 1.
  GenerateInterestingPoints(Length(1), NHugePages(1), Length(1));
  // Epoch 11.
  Advance(absl::Minutes(5));
  GenerateInterestingPoints(Length(100), NHugePages(5), Length(200));
  // Epoch 13.
  Advance(absl::Minutes(1));
  GenerateInterestingPoints(Length(200), NHugePages(10), Length(100));

  // Test text output (time series summary).
  {
    std::string buffer = PrintToString(1024 * 1024, [&](Printer& printer) {
      tracker_.Print(printer, "StatsTracker");
    });

    EXPECT_THAT(buffer, StrEq(R"(StatsTracker: time series over 5 min interval

StatsTracker: realized fragmentation: 0.8 MiB
StatsTracker: minimum free pages: 110 (100 backed)
StatsTracker: at peak demand: 208 pages (and 111 free, 10 unmapped)
StatsTracker: at peak demand: 26 hps (14 regular, 10 donated, 1 partial, 1 released)

StatsTracker: Since the start of the execution, 0 subreleases (0 pages) were skipped due to either recent (0s) peaks, or the sum of short-term (0s) fluctuations and long-term (0s) trends.
StatsTracker: 0.0000% of decisions confirmed correct, 0 pending (0.0000% of pages, 0 pending), as per anticipated 300s realized fragmentation.
StatsTracker: Subrelease stats last 10 min: total 0 pages subreleased (0 pages from partial allocs), 0 hugepages broken
)"));
  }
}

TEST_F(StatsTrackerTest, InvalidDurations) {
  // These should not crash.
  tracker_.min_free_pages(absl::InfiniteDuration());
  tracker_.min_free_pages(kWindow + absl::Seconds(1));
  tracker_.min_free_pages(-(kWindow + absl::Seconds(1)));
  tracker_.min_free_pages(-absl::InfiniteDuration());
}

TEST_F(StatsTrackerTest, ComputeRecentPeaks) {
  // Epoch 1.
  GenerateDemandPoint(Length(3000), Length(1000));
  // Epoch 4.
  Advance(absl::Minutes(1.5));
  GenerateDemandPoint(Length(1500), Length(0));
  // Epoch 6.
  Advance(absl::Minutes(1));
  GenerateDemandPoint(Length(100), Length(2000));
  // Epoch 8.
  Advance(absl::Minutes(1));
  GenerateDemandPoint(Length(500), Length(3000));

  Length peak = tracker_.GetRecentPeak(absl::Minutes(3));
  EXPECT_EQ(peak, Length(1500));
  Length peak2 = tracker_.GetRecentPeak(absl::Minutes(5));
  EXPECT_EQ(peak2, Length(3000));
  // Epoch 16.
  Advance(absl::Minutes(4));
  GenerateDemandPoint(Length(200), Length(3000));

  Length peak3 = tracker_.GetRecentPeak(absl::Minutes(4));
  EXPECT_EQ(peak3, Length(200));

  Advance(absl::Minutes(5));
  GenerateDemandPoint(Length(150), Length(3000));

  Length peak4 = tracker_.GetRecentPeak(absl::Minutes(5));
  EXPECT_EQ(peak4, Length(150));
}

TEST_F(StatsTrackerTest, ComputeRecentDemand) {
  // Generates max and min demand in each epoch to create short-term demand
  // fluctuations.
  // Epoch 1.
  GenerateDemandPoint(Length(1500), Length(2000));
  GenerateDemandPoint(Length(3000), Length(1000));
  // Epoch 4.
  Advance(absl::Minutes(1.5));
  GenerateDemandPoint(Length(500), Length(1000));
  GenerateDemandPoint(Length(1500), Length(0));
  // Epoch 6.
  Advance(absl::Minutes(1));
  GenerateDemandPoint(Length(50), Length(1000));
  GenerateDemandPoint(Length(100), Length(2000));
  // Epoch 8.
  Advance(absl::Minutes(1));
  GenerateDemandPoint(Length(100), Length(2000));
  GenerateDemandPoint(Length(300), Length(3000));

  Length short_long_peak_pages =
      tracker_.GetRecentDemand(absl::Minutes(2), absl::Minutes(3));
  EXPECT_EQ(short_long_peak_pages, Length(700));
  Length short_long_peak_pages2 =
      tracker_.GetRecentDemand(absl::Minutes(5), absl::Minutes(5));
  EXPECT_EQ(short_long_peak_pages2, Length(3000));
  // Epoch 16.
  Advance(absl::Minutes(4));
  GenerateDemandPoint(Length(150), Length(500));
  GenerateDemandPoint(Length(200), Length(3000));

  Length short_long_peak_pages3 =
      tracker_.GetRecentDemand(absl::Minutes(1), absl::ZeroDuration());
  EXPECT_EQ(short_long_peak_pages3, Length(50));
  // Epoch 36.
  Advance(absl::Minutes(5));
  GenerateDemandPoint(Length(100), Length(700));
  GenerateDemandPoint(Length(150), Length(800));

  Length short_long_peak_pages4 =
      tracker_.GetRecentDemand(absl::ZeroDuration(), absl::Minutes(5));
  EXPECT_EQ(short_long_peak_pages4, Length(100));
  // The short_interval needs to be shorter or equal to the long_interval when
  // they are both set.  We cap short_interval to long_interval when this is not
  // the case.
  EXPECT_EQ(tracker_.GetRecentDemand(absl::Minutes(2), absl::Minutes(1)),
            tracker_.GetRecentDemand(absl::Minutes(1), absl::Minutes(1)));
}

TEST_F(StatsTrackerTest, ComputeRecentDemandAndCappedToPeak) {
  // Generates max and min demand in each epoch to create short-term demand
  // fluctuations.
  GenerateDemandPoint(Length(50), Length(2000));
  GenerateDemandPoint(Length(3000), Length(1000));
  Advance(absl::Minutes(2));
  GenerateDemandPoint(Length(1500), Length(0));
  Advance(absl::Minutes(1));
  GenerateDemandPoint(Length(50), Length(1000));
  GenerateDemandPoint(Length(100), Length(2000));
  // The calculated demand is 2500 (maximum demand diff) + 1500 (max
  // min_demand), but capped by the peak observed in the time series.
  Length demand_1 =
      tracker_.GetRecentDemand(absl::Minutes(5), absl::Minutes(5));
  EXPECT_EQ(demand_1, Length(3000));
  // Capped by the peak observed in 2 mins.
  Length demand_2 = tracker_.GetRecentDemand(absl::Minutes(5), absl::Minutes(5),
                                             absl::Minutes(2));
  EXPECT_EQ(demand_2, Length(1500));
}

// Tests that we can compute the realized fragmentation correctly.
TEST_F(StatsTrackerTest, ComputeRealizedFragmentation) {
  GenerateDemandPoint(Length(50), Length(500));
  Advance(absl::Minutes(2));
  GenerateDemandPoint(Length(3000), Length(1000));
  Advance(absl::Minutes(1));
  GenerateDemandPoint(Length(1500), Length(2000));

  Length fragmentation_1 = tracker_.RealizedFragmentation();
  EXPECT_EQ(fragmentation_1, Length(500));

  Advance(absl::Minutes(30));
  GenerateDemandPoint(Length(1500), Length(2000));

  Length fragmentation_2 = tracker_.RealizedFragmentation();
  EXPECT_EQ(fragmentation_2, Length(2000));
}

TEST_F(StatsTrackerTest, TrackCorrectSubreleaseDecisions) {
  // First peak (large)
  GenerateDemandPoint(Length(1000), Length(1000));

  // Incorrect subrelease: Subrelease to 1000
  Advance(absl::Minutes(1));
  GenerateDemandPoint(Length(100), Length(1000));
  tracker_.ReportSkippedSubreleasePages(Length(900), Length(1000));

  // Second peak (small)
  Advance(absl::Minutes(1));
  GenerateDemandPoint(Length(500), Length(1000));

  EXPECT_EQ(tracker_.total_skipped().pages, Length(900));
  EXPECT_EQ(tracker_.total_skipped().count, 1);
  EXPECT_EQ(tracker_.correctly_skipped().pages, Length(0));
  EXPECT_EQ(tracker_.correctly_skipped().count, 0);
  EXPECT_EQ(tracker_.pending_skipped().pages, Length(900));
  EXPECT_EQ(tracker_.pending_skipped().count, 1);

  // Correct subrelease: Subrelease to 500
  Advance(absl::Minutes(1));
  GenerateDemandPoint(Length(500), Length(100));
  tracker_.ReportSkippedSubreleasePages(Length(50), Length(550));
  GenerateDemandPoint(Length(500), Length(50));
  tracker_.ReportSkippedSubreleasePages(Length(50), Length(500));
  GenerateDemandPoint(Length(500), Length(0));

  EXPECT_EQ(tracker_.total_skipped().pages, Length(1000));
  EXPECT_EQ(tracker_.total_skipped().count, 3);
  EXPECT_EQ(tracker_.correctly_skipped().pages, Length(0));
  EXPECT_EQ(tracker_.correctly_skipped().count, 0);
  EXPECT_EQ(tracker_.pending_skipped().pages, Length(1000));
  EXPECT_EQ(tracker_.pending_skipped().count, 3);

  // Third peak (large, too late for first peak)
  Advance(absl::Minutes(4));
  GenerateDemandPoint(Length(1100), Length(1000));

  EXPECT_EQ(tracker_.total_skipped().pages, Length(1000));
  EXPECT_EQ(tracker_.total_skipped().count, 3);
  EXPECT_EQ(tracker_.correctly_skipped().pages, Length(100));
  EXPECT_EQ(tracker_.correctly_skipped().count, 2);
  EXPECT_EQ(tracker_.pending_skipped().pages, Length(0));
  EXPECT_EQ(tracker_.pending_skipped().count, 0);
}

}  // namespace
}  // namespace tcmalloc_internal
}  // namespace tcmalloc
