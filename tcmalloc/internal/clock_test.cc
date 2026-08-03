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

#include "tcmalloc/internal/clock.h"

#include <stdint.h>

#include "gtest/gtest.h"
#include "absl/time/time.h"

namespace tcmalloc {
namespace tcmalloc_internal {
namespace {

class Cycles32Test : public ::testing::Test {
 protected:
  void SetUp() override {
    g_ticks_ = 0;
    g_freq_ = 1e9;  // 1 GHz -> 1 ns per cycle.
  }

  void TearDown() override { g_ticks_ = 0; }

  static int64_t MockNow() { return g_ticks_; }
  static double MockFreq() { return g_freq_; }

  Clock mock_clock_{.now = MockNow, .freq = MockFreq};

  static inline int64_t g_ticks_ = 0;
  static inline double g_freq_ = 1e9;
};

TEST_F(Cycles32Test, Uninitialized) {
  Cycles32 c;
  EXPECT_EQ(c.raw(), 0);
  EXPECT_EQ(c.AsDuration(mock_clock_), absl::InfiniteDuration());
}

TEST_F(Cycles32Test, UpdateAndAsDuration) {
  g_ticks_ = (100LL << Cycles32::kShift);

  Cycles32 c;
  c.Update(mock_clock_);
  EXPECT_EQ(c.raw(), 100);

  g_ticks_ = (250LL << Cycles32::kShift);
  EXPECT_EQ(c.AsDuration(mock_clock_),
            absl::Nanoseconds(150LL << Cycles32::kShift));
}

TEST_F(Cycles32Test, WraparoundModularSubtraction) {
  Cycles32 c(0xFFFFFFF0u);

  g_ticks_ = (10LL << Cycles32::kShift);
  EXPECT_EQ(c.AsDuration(mock_clock_),
            absl::Nanoseconds(26LL << Cycles32::kShift));
}

TEST_F(Cycles32Test, TimeAfterOrEqual) {
  Cycles32 c10(10);
  Cycles32 c20(20);
  EXPECT_TRUE(c10.TimeAfterOrEqual(c10));
  EXPECT_TRUE(c20.TimeAfterOrEqual(c10));
  EXPECT_FALSE(c10.TimeAfterOrEqual(c20));

  Cycles32 c_before(0xFFFFFFF0u);
  Cycles32 c_after(10);
  EXPECT_TRUE(c_after.TimeAfterOrEqual(c_before));
  EXPECT_FALSE(c_before.TimeAfterOrEqual(c_after));
}

TEST_F(Cycles32Test, SentinelAliasingAtTickZero) {
  g_ticks_ = 0;  // Tick 0 right-shifts to 0.
  Cycles32 c;
  c.Update(mock_clock_);
  EXPECT_EQ(c.raw(),
            1);  // Clamped to 1 to reserve 0 as uninitialized sentinel.
  EXPECT_NE(c.AsDuration(mock_clock_), absl::InfiniteDuration());
}

TEST_F(Cycles32Test, ExactHalfEpochSignInversion) {
  Cycles32 base(0);
  Cycles32 max_forward(0x7FFFFFFFu);
  Cycles32 sign_flip(0x80000000u);

  EXPECT_TRUE(max_forward.TimeAfterOrEqual(base));
  EXPECT_FALSE(sign_flip.TimeAfterOrEqual(base));

  Cycles32 w_base(10);
  Cycles32 w_forward(10 + 0x7FFFFFFFu);
  Cycles32 w_flip(10 + 0x80000000u);
  EXPECT_TRUE(w_forward.TimeAfterOrEqual(w_base));
  EXPECT_FALSE(w_flip.TimeAfterOrEqual(w_base));
}

TEST_F(Cycles32Test, MaximumElapsedDurationExactness) {
  Cycles32 c(1);
  g_ticks_ = (0xFFFFFFFFLL << Cycles32::kShift);
  // 0xFFFFFFFFu - 1 = 0xFFFFFFFEu ticks elapsed.
  EXPECT_EQ(c.AsDuration(mock_clock_),
            absl::Nanoseconds(0xFFFFFFFELL << Cycles32::kShift));
}

TEST_F(Cycles32Test, ZeroElapsedDuration) {
  g_ticks_ = (500LL << Cycles32::kShift);
  Cycles32 c;
  c.Update(mock_clock_);
  EXPECT_EQ(c.AsDuration(mock_clock_), absl::ZeroDuration());
}

TEST_F(Cycles32Test, SnapshotImmuneToDrift) {
  g_ticks_ = (100LL << Cycles32::kShift);
  Cycles32 c;
  c.Update(mock_clock_);

  g_ticks_ = (200LL << Cycles32::kShift);
  auto snap = mock_clock_.GetSnapshot();

  // Advance clock after snapshot was taken.
  g_ticks_ = (500LL << Cycles32::kShift);

  // AsDuration(snap) must use the snapshot (100 ticks elapsed), not g_ticks_
  // (400 ticks).
  EXPECT_EQ(c.AsDuration(snap), absl::Nanoseconds(100LL << Cycles32::kShift));
}

TEST_F(Cycles32Test, ResetAndBoolOperator) {
  Cycles32 c;
  EXPECT_FALSE(static_cast<bool>(c));
  EXPECT_EQ(c.AsDuration(mock_clock_), absl::InfiniteDuration());

  c.Update(mock_clock_);
  EXPECT_TRUE(static_cast<bool>(c));
  EXPECT_NE(c.AsDuration(mock_clock_), absl::InfiniteDuration());

  c.Reset();
  EXPECT_FALSE(static_cast<bool>(c));
  EXPECT_EQ(c.AsDuration(mock_clock_), absl::InfiniteDuration());
}

}  // namespace
}  // namespace tcmalloc_internal
}  // namespace tcmalloc
