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

#include "tcmalloc/internal/timeseries_tracker.h"

#include <cstddef>
#include <cstdint>
#include <vector>

#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "absl/base/internal/cycleclock.h"
#include "absl/time/clock.h"
#include "absl/time/time.h"
#include "tcmalloc/internal/clock.h"

using ::testing::ElementsAre;

namespace tcmalloc {
namespace tcmalloc_internal {
namespace {

class TimeSeriesTrackerTest : public testing::Test {
 public:
  struct TestEntry {
    static TestEntry Nil() { return TestEntry(); }

    void Report(int n) { values_.push_back(n); }

    bool empty() const { return values_.empty(); }

    std::vector<int> values_;
  };

 protected:
  void Advance(absl::Duration d) {
    clock_ += absl::ToDoubleSeconds(d) * GetFakeClockFrequency();
  }

  // Covers a minimum 2-second window on a 8 slots time series.
  static constexpr absl::Duration kEpochLength = absl::Milliseconds(250);
  static constexpr size_t kSlots = 8;
  // Helper struct whose sole purpose is to call ResetClock() upon construction.
  struct ClockResetter {
    ClockResetter() { ResetClock(); }
  };

  // This ensures ResetClock() is called before the tracker_ inits, so it has
  // the same initial state (e.g., first epoch)
  ClockResetter clock_resetter_;
  TimeSeriesTracker<TestEntry, int, kSlots> tracker_{
      Clock{.now = FakeClock, .freq = GetFakeClockFrequency}, kEpochLength};

 private:
  static int64_t FakeClock() { return clock_; }

  static void ResetClock() { clock_ = 0; }

  static double GetFakeClockFrequency() {
    return absl::ToDoubleNanoseconds(absl::Seconds(2));
  }

  static int64_t clock_;
};

int64_t TimeSeriesTrackerTest::clock_{0};

// Test that frequency conversion in the cycle clock works correctly
TEST(TimeSeriesTest, CycleClock) {
  TimeSeriesTracker<TimeSeriesTrackerTest::TestEntry, int, 100> tracker{
      Clock{absl::base_internal::CycleClock::Now,
            absl::base_internal::CycleClock::Frequency},
      absl::Milliseconds(100)};  // 100ms epochs

  tracker.Report(1);
  absl::SleepFor(absl::Milliseconds(100));
  tracker.Report(2);

  // Iterate through entries.
  int num_timestamps = 0;
  int offset_1, offset_2, epoch_delta_2;
  tracker.Iter([&](size_t offset, size_t epoch_delta,
                   const TimeSeriesTrackerTest::TestEntry& e) {
    ASSERT_LT(num_timestamps, 2);
    if (num_timestamps == 0) {
      offset_1 = offset;
      EXPECT_THAT(e.values_, ElementsAre(1));
    } else {
      offset_2 = offset;
      epoch_delta_2 = epoch_delta;
      EXPECT_THAT(e.values_, ElementsAre(2));
    }
    num_timestamps++;
  });

  // If we are near an epoch boundary, the gap between the two records might be
  // two epochs.
  EXPECT_GE(epoch_delta_2, 1);
  EXPECT_LE(epoch_delta_2, 2);
  // The slot always advanced by one regardless the amount of time passed.
  EXPECT_EQ(offset_2 - offset_1, 1);
}

TEST_F(TimeSeriesTrackerTest, Works) {
  // Advances 8 epochs, report.
  Advance(absl::Seconds(2));
  tracker_.Report(1);
  // Staying in the same epoch, report.
  Advance(absl::Nanoseconds(1));
  tracker_.Report(2);
  // Advances 2 epochs, report.
  Advance(absl::Seconds(0.5));
  tracker_.Report(4);

  // Iterate through entries.
  int num_timestamps = 0;
  int offset_1, offset_2, epoch_delta_2;
  tracker_.Iter([&](size_t offset, size_t epoch_delta, const TestEntry& e) {
    ASSERT_LT(num_timestamps, 2);
    if (num_timestamps == 0) {
      offset_1 = offset;
      EXPECT_THAT(e.values_, ElementsAre(1, 2));
    } else {
      offset_2 = offset;
      epoch_delta_2 = epoch_delta;
      EXPECT_THAT(e.values_, ElementsAre(4));
    }
    num_timestamps++;
  });

  EXPECT_EQ(num_timestamps, 2);
  EXPECT_EQ(epoch_delta_2, 2);
  EXPECT_EQ(offset_2 - offset_1, 1);

  // Advances 3 epochs, report.
  Advance(absl::Seconds(0.75));
  tracker_.Report(8);

  // Advances 2 epochs, report.
  Advance(absl::Seconds(0.5));
  tracker_.Report(16);

  // Iterate backwards using interval length that enough to covert the last two
  // records.
  num_timestamps = 0;
  tracker_.IterBackwards(
      [&](size_t offset, size_t epoch_delta, const TestEntry& e) {
        EXPECT_EQ(num_timestamps, offset);
        if (num_timestamps == 0) {
          EXPECT_EQ(epoch_delta, 2);
          EXPECT_THAT(e.values_, ElementsAre(16));
        } else {
          EXPECT_EQ(epoch_delta, 3);
          EXPECT_THAT(e.values_, ElementsAre(8));
        }
        num_timestamps++;
      },
      // Checks if the traversal function can correctly convert time to epochs
      // by giving an interval that is slightly wider than 2 epochs.
      absl::Seconds(0.6));

  EXPECT_EQ(num_timestamps, 2);
  auto recent_record = tracker_.GetMostRecentRecord();
  // The record was reported 2 epochs ago.
  EXPECT_EQ(recent_record.epoch_taken, 2);
  EXPECT_THAT(recent_record.data.values_, ElementsAre(8));

  // Advances 16 epochs. All 4 records are still there as the clock is updated
  // in a constant manner.
  Advance(absl::Seconds(4));
  tracker_.UpdateTimeBase();
  num_timestamps = 0;
  tracker_.Iter([&](size_t offset, size_t epoch_delta, const TestEntry& e) {
    num_timestamps++;
  });
  EXPECT_EQ(num_timestamps, 4);
  recent_record = tracker_.GetMostRecentRecord();
  // The record was reported 16 epochs ago.
  EXPECT_EQ(recent_record.epoch_taken, 16);
  EXPECT_THAT(recent_record.data.values_, ElementsAre(16));

  // Add a new record and show that all previous 4 records are still there.
  // These 5 reports were collected in 5.75s, which is longer than
  // (epoch_length_ 0.25s * kSlots 8).
  tracker_.Report(1234);
  std::vector<std::vector<int>> all_values;
  tracker_.Iter([&](size_t offset, size_t epoch_delta, const TestEntry& e) {
    all_values.push_back(e.values_);
  });

  EXPECT_THAT(all_values,
              ElementsAre(ElementsAre(1, 2), ElementsAre(4), ElementsAre(8),
                          ElementsAre(16), ElementsAre(1234)));
}

// Tests IterBackwards() functions correctly by covering all edge cases.
TEST_F(TimeSeriesTrackerTest, IterBackwards) {
  tracker_.Report(1);
  // Checks if we can reach the first record (i.e., the init functions are
  // working correctly).
  tracker_.IterBackwards(
      [&](size_t offset, size_t epoch_delta, const TestEntry& e) {
        EXPECT_EQ(epoch_delta, 1);
        EXPECT_THAT(e.values_, ElementsAre(1));
      },
      absl::Seconds(0.1));
  // Advances 2 epochs, report.
  Advance(absl::Seconds(0.5));
  tracker_.Report(4);
  // Advances 3 epochs, report.
  Advance(absl::Seconds(0.75));
  tracker_.Report(8);
  // Advances 2 epochs, report.
  Advance(absl::Seconds(0.5));
  tracker_.Report(16);
  // Advances 16 epochs. All 4 records are still there as the clock is updated
  // in a constant manner.
  Advance(absl::Seconds(4));
  tracker_.UpdateTimeBase();
  // Traverses all record if the interval is infinite. There are 5 valid data
  // points in total.
  int num_timestamps = 0;
  tracker_.IterBackwards([&](size_t offset, size_t epoch_delta,
                             const TestEntry& e) { num_timestamps++; },
                         absl::InfiniteDuration());
  EXPECT_EQ(num_timestamps, 5);

  // The traversal is capped by the valid points event though we request a large
  // interval.
  num_timestamps = 0;
  tracker_.IterBackwards([&](size_t offset, size_t epoch_delta,
                             const TestEntry& e) { num_timestamps++; },
                         absl::Hours(1));
  EXPECT_EQ(num_timestamps, 5);

  // Checks if the data are overwritten correctly if we keep adding new records.
  for (int i = 0; i < kSlots; i++) {
    Advance(absl::Seconds(0.25));
    tracker_.Report(1234);
  }
  num_timestamps = 0;
  tracker_.IterBackwards(
      [&](size_t offset, size_t epoch_delta, const TestEntry& e) {
        EXPECT_EQ(epoch_delta, 1);
        EXPECT_THAT(e.values_, ElementsAre(1234));
        num_timestamps++;
      },
      absl::InfiniteDuration());
  EXPECT_EQ(num_timestamps, kSlots);

  // No traversal is taken if the interval is zero.
  tracker_.IterBackwards(
      [&](size_t offset, size_t epoch_delta, const TestEntry& e) {
        // This lambda body should not be reached.
        FAIL() << "IterBackwards callback was unexpectedly invoked with "
                  "ZeroDuration.";
      },
      absl::ZeroDuration());
}

// Tests the time series works as expected when no valid record exists .
TEST_F(TimeSeriesTrackerTest, NoValidRecord) {
  tracker_.IterBackwards([&](size_t offset, size_t epoch_delta,
                             const TestEntry& e) { EXPECT_TRUE(e.empty()); },
                         absl::InfiniteDuration());
  auto num_timestamps = 0;
  tracker_.Iter([&](size_t offset, size_t epoch_delta, const TestEntry& e) {
    num_timestamps++;
  });
  EXPECT_EQ(num_timestamps, 0);

  auto recent_record = tracker_.GetMostRecentRecord();
  // Nothing but the placeholder added by Init.
  EXPECT_EQ(recent_record.epoch_taken, 1);
  EXPECT_TRUE(recent_record.data.empty());
}

}  // namespace
}  // namespace tcmalloc_internal
}  // namespace tcmalloc
