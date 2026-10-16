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

#include "tcmalloc/internal/cpu_cache_resize_policy.h"

#include <stddef.h>
#include <stdint.h>

#include <array>

#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "absl/types/span.h"

namespace tcmalloc {
namespace tcmalloc_internal {
namespace {

using ::testing::ElementsAre;

constexpr size_t kNumClasses = 8;
using Policy = MissCountResizePolicy<kNumClasses>;
using Density = MissDensityResizePolicy<kNumClasses>;
using States = std::array<SizeClassResizeState, kNumClasses>;

// A size class whose shrink score is 0 regardless of its freelist state, so
// that the clock never refuses to shrink it.
constexpr uint32_t kLargeObject = 128 << 10;

States MakeStates() {
  States states{};
  for (size_t size_class = 1; size_class < kNumClasses; ++size_class) {
    SizeClassResizeState& state = states[size_class];
    state.object_size = kLargeObject;
    state.batch_length = 2;
    state.capacity = 10;
    state.max_capacity = 100;
    state.length = 0;
  }
  return states;
}

std::array<int32_t, kNumClasses> Deltas(const States& states) {
  std::array<int32_t, kNumClasses> deltas;
  for (size_t i = 0; i < kNumClasses; ++i) deltas[i] = states[i].delta;
  return deltas;
}

std::array<uint16_t, kNumClasses> Ticks(const States& states) {
  std::array<uint16_t, kNumClasses> ticks;
  for (size_t i = 0; i < kNumClasses; ++i) ticks[i] = states[i].quiescent_ticks;
  return ticks;
}

std::array<bool, kNumClasses> FastPathGrowth(const States& states) {
  std::array<bool, kNumClasses> allowed;
  for (size_t i = 0; i < kNumClasses; ++i) {
    allowed[i] = states[i].allow_fast_path_growth;
  }
  return allowed;
}

// Interval over which the misses of the density states were counted.
constexpr double kInterval = 2.0;
// Capacity of every density state, in objects of 1 KiB, so that a class's
// density is misses / (kInterval * kDensityCapacity) misses per second per
// KiB and kMissesAtTarget misses put it exactly at the target density.
constexpr size_t kDensityCapacity = 20;
constexpr size_t kMissesAtTarget =
    Density::kTargetMissDensity * kInterval * kDensityCapacity;
static_assert(kMissesAtTarget == 4);

// Every class holds 20 objects of 1 KiB, two batches being 8, and has taken
// half the target density's misses: inside the hysteresis band, where it is
// neither grown nor shrunk but may be shrunk to fund a class with at least
// four times its density.
States MakeDensityStates() {
  States states{};
  for (size_t size_class = 1; size_class < kNumClasses; ++size_class) {
    SizeClassResizeState& state = states[size_class];
    state.object_size = 1024;
    state.batch_length = 4;
    state.capacity = kDensityCapacity;
    state.max_capacity = 256;
    state.misses = kMissesAtTarget / 2;
  }
  return states;
}

TEST(ShrinkScoreTest, SmallObjects) {
  // 2 for small objects, +1 when the freelist is full, +1 when it is at one
  // batch and shrinking it would disable transfer cache batching.
  EXPECT_EQ(ShrinkScore(/*object_size=*/8, /*length=*/0, /*capacity=*/10,
                        /*batch_length=*/4),
            2);
  EXPECT_EQ(ShrinkScore(8, 10, 10, 4), 3);
  EXPECT_EQ(ShrinkScore(8, 5, 10, 4), 3);
  EXPECT_EQ(ShrinkScore(144, 4, 4, 4), 4);
  EXPECT_EQ(ShrinkScore(8, 8, 10, 4), 2);
}

TEST(ShrinkScoreTest, MediumObjects) {
  EXPECT_EQ(ShrinkScore(145, 0, 10, 4), 1);
  EXPECT_EQ(ShrinkScore(1024, 10, 10, 4), 2);
  EXPECT_EQ(ShrinkScore(1024, 4, 10, 4), 2);
  EXPECT_EQ(ShrinkScore(1024, 4, 4, 4), 3);
}

TEST(ShrinkScoreTest, LargeObjects) {
  EXPECT_EQ(ShrinkScore(1025, 0, 10, 4), 0);
  EXPECT_EQ(ShrinkScore(1025, 10, 10, 4), 1);
  EXPECT_EQ(ShrinkScore(64 << 10, 10, 10, 4), 1);
  EXPECT_EQ(ShrinkScore((64 << 10) + 1, 10, 10, 4), 0);
  EXPECT_EQ(ShrinkScore(kLargeObject, 10, 10, 4), 0);
}

TEST(MissCountResizePolicyTest, NoMissesNoChange) {
  States states = MakeStates();
  size_t next_steal = 3;
  const SizeClassResizePlan plan = Policy::Plan(
      absl::MakeSpan(states), /*available_bytes=*/1000, next_steal);
  EXPECT_EQ(plan.available_bytes, 1000);
  EXPECT_EQ(plan.num_resizes, 0);
  EXPECT_EQ(next_steal, 3);
  EXPECT_THAT(Deltas(states), ElementsAre(0, 0, 0, 0, 0, 0, 0, 0));
  EXPECT_THAT(Ticks(states), ElementsAre(0, 0, 0, 0, 0, 0, 0, 0));
}

TEST(MissCountResizePolicyTest, NeverGatesFastPathGrowth) {
  States states = MakeStates();
  for (SizeClassResizeState& state : states) {
    state.allow_fast_path_growth = false;
  }
  states[3].capacity_misses = 10;
  size_t next_steal = 1;
  (void)Policy::Plan(absl::MakeSpan(states), /*available_bytes=*/0, next_steal);
  EXPECT_THAT(FastPathGrowth(states),
              ElementsAre(true, true, true, true, true, true, true, true));
}

TEST(MissCountResizePolicyTest, GrowsFromUnallocatedBytes) {
  States states = MakeStates();
  states[3].capacity_misses = 10;
  size_t next_steal = 1;
  // Enough unallocated bytes for a batch: nothing is stolen, and the class
  // takes all of the unallocated bytes it can use, not just a batch.
  const SizeClassResizePlan plan =
      Policy::Plan(absl::MakeSpan(states),
                   /*available_bytes=*/5 * kLargeObject + 1, next_steal);
  EXPECT_EQ(plan.num_resizes, 1);
  EXPECT_EQ(plan.available_bytes, 1);
  EXPECT_EQ(next_steal, 1);
  EXPECT_THAT(Deltas(states), ElementsAre(0, 0, 0, 5, 0, 0, 0, 0));
  EXPECT_THAT(Ticks(states), ElementsAre(0, 0, 0, 0, 0, 0, 0, 0));
}

TEST(MissCountResizePolicyTest, GrowthIsCappedAtMaxCapacity) {
  States states = MakeStates();
  states[3].capacity_misses = 10;
  states[3].max_capacity = 12;
  size_t next_steal = 1;
  const SizeClassResizePlan plan = Policy::Plan(
      absl::MakeSpan(states), /*available_bytes=*/5 * kLargeObject, next_steal);
  EXPECT_EQ(plan.num_resizes, 1);
  EXPECT_EQ(plan.available_bytes, 3 * kLargeObject);
  EXPECT_THAT(Deltas(states), ElementsAre(0, 0, 0, 2, 0, 0, 0, 0));
}

TEST(MissCountResizePolicyTest, StealsWhenUnallocatedBytesRunShort) {
  States states = MakeStates();
  states[1].object_size = 16;
  states[1].batch_length = 8;
  states[1].capacity = 4;
  states[1].capacity_misses = 10;
  size_t next_steal = 1;
  // Class 1 wants a batch (128 bytes).  The scan starts at class 1 itself,
  // which is a growth candidate and is skipped; class 2 releases one object,
  // which more than covers the need, and the scan stops there.  The class
  // then grows by as much as the released bytes allow, up to its maximum.
  const SizeClassResizePlan plan =
      Policy::Plan(absl::MakeSpan(states), /*available_bytes=*/0, next_steal);
  EXPECT_EQ(plan.num_resizes, 1);
  EXPECT_EQ(next_steal, 2);
  EXPECT_THAT(Deltas(states), ElementsAre(0, 96, -1, 0, 0, 0, 0, 0));
  EXPECT_THAT(Ticks(states), ElementsAre(0, 0, 1, 0, 0, 0, 0, 0));
  EXPECT_EQ(plan.available_bytes, kLargeObject - 96 * 16);
}

TEST(MissCountResizePolicyTest, StealsSeveralObjectsForOneBatch) {
  States states = MakeStates();
  states[1].capacity_misses = 10;
  size_t next_steal = 5;
  // Every object is worth the same, so a batch of two costs two objects: the
  // scan starts at class 5, shrinks it and class 6, and leaves the hand on 6.
  const SizeClassResizePlan plan =
      Policy::Plan(absl::MakeSpan(states), /*available_bytes=*/0, next_steal);
  EXPECT_EQ(plan.num_resizes, 1);
  EXPECT_EQ(plan.available_bytes, 0);
  EXPECT_EQ(next_steal, 6);
  EXPECT_THAT(Deltas(states), ElementsAre(0, 2, 0, 0, 0, -1, -1, 0));
  EXPECT_THAT(Ticks(states), ElementsAre(0, 0, 0, 0, 0, 1, 1, 0));
}

TEST(MissCountResizePolicyTest, ClockRefusesRecentlyGrownClasses) {
  States states = MakeStates();
  states[1].capacity_misses = 10;
  // Small objects need two ticks before they are shrunk (score 2 when the
  // freelist is neither full nor at one batch).
  for (size_t size_class = 2; size_class < kNumClasses; ++size_class) {
    states[size_class].object_size = 64;
    states[size_class].batch_length = 32;
  }
  states[2].quiescent_ticks = 0;
  states[3].quiescent_ticks = 1;
  states[4].quiescent_ticks = 2;
  states[5].quiescent_ticks = 2;
  size_t next_steal = 2;
  // A batch of class 1 costs 2 * kLargeObject bytes = 4096 objects of 64
  // bytes; the scan visits every class once, ticking all of them, and shrinks
  // only the two whose ticks had reached the score.  The hand ends up past the
  // last class visited, which wrapped around to class 1.
  const SizeClassResizePlan plan =
      Policy::Plan(absl::MakeSpan(states), /*available_bytes=*/0, next_steal);
  EXPECT_EQ(plan.num_resizes, 1);
  EXPECT_EQ(plan.available_bytes, 2 * 64);
  EXPECT_EQ(next_steal, 2);
  EXPECT_THAT(Deltas(states), ElementsAre(0, 0, 0, 0, -1, -1, 0, 0));
  EXPECT_THAT(Ticks(states), ElementsAre(0, 0, 1, 2, 3, 3, 1, 1));
}

TEST(MissCountResizePolicyTest, ScanCoversEveryClassOnceAndWraps) {
  States states = MakeStates();
  states[1].capacity_misses = 10;
  states[1].batch_length = 20;
  for (size_t size_class = 2; size_class < kNumClasses; ++size_class) {
    states[size_class].capacity = 1;
  }
  size_t next_steal = kNumClasses;
  // Twenty objects are wanted but each class can give up one: the scan wraps
  // from past the last class to class 1, visits classes 2 to 7 once, and the
  // candidate grows by what was released.
  const SizeClassResizePlan plan =
      Policy::Plan(absl::MakeSpan(states), /*available_bytes=*/0, next_steal);
  EXPECT_EQ(plan.num_resizes, 1);
  EXPECT_EQ(plan.available_bytes, 0);
  EXPECT_EQ(next_steal, kNumClasses);
  EXPECT_THAT(Deltas(states), ElementsAre(0, 6, -1, -1, -1, -1, -1, -1));
}

TEST(MissCountResizePolicyTest, EmptyClassesAreNotTicked) {
  States states = MakeStates();
  states[1].capacity_misses = 10;
  states[2].capacity = 0;
  states[3].capacity = 0;
  size_t next_steal = 2;
  const SizeClassResizePlan plan =
      Policy::Plan(absl::MakeSpan(states), /*available_bytes=*/0, next_steal);
  EXPECT_EQ(plan.num_resizes, 1);
  EXPECT_EQ(next_steal, 5);
  EXPECT_THAT(Deltas(states), ElementsAre(0, 2, 0, 0, -1, -1, 0, 0));
  EXPECT_THAT(Ticks(states), ElementsAre(0, 0, 0, 0, 1, 1, 0, 0));
}

TEST(MissCountResizePolicyTest, FullFreelistsEvict) {
  States states = MakeStates();
  states[1].capacity_misses = 10;
  states[1].batch_length = 3;
  states[1].max_capacity = states[1].capacity + 3;
  states[1].object_size = 1024;
  states[2].length = 10;
  states[2].object_size = 2048;  // Score 1 while full, 0 afterwards.
  states[2].quiescent_ticks = 1;
  size_t next_steal = 2;
  // Shrinking the full class 2 evicts an object and releases 2048 bytes;
  // 3 * 1024 are wanted, so the scan continues to class 3.
  const SizeClassResizePlan plan =
      Policy::Plan(absl::MakeSpan(states), /*available_bytes=*/0, next_steal);
  EXPECT_EQ(plan.num_resizes, 1);
  EXPECT_THAT(Deltas(states), ElementsAre(0, 3, -1, -1, 0, 0, 0, 0));
  EXPECT_EQ(plan.available_bytes, 2048 + kLargeObject - 3 * 1024);
  EXPECT_EQ(next_steal, 3);
}

TEST(MissCountResizePolicyTest, GrowthCandidatesAreNotShrunk) {
  States states = MakeStates();
  // Five candidates fill the window; class 6 misses too but is outside it
  // and may be shrunk, class 7 has no misses.
  for (size_t size_class = 1; size_class <= 6; ++size_class) {
    states[size_class].capacity_misses = 100 - size_class;
  }
  size_t next_steal = 1;
  // Each candidate wants two objects.  With no unallocated bytes, the only
  // sources are classes 6 and 7, one object each per visit.
  const SizeClassResizePlan plan =
      Policy::Plan(absl::MakeSpan(states), /*available_bytes=*/0, next_steal);
  EXPECT_EQ(plan.num_resizes, 5);
  EXPECT_EQ(plan.available_bytes, 0);
  EXPECT_THAT(Deltas(states), ElementsAre(0, 2, 2, 2, 2, 2, -5, -5));
}

TEST(MissCountResizePolicyTest, WindowExpandsPastClassesAtMaxCapacity) {
  States states = MakeStates();
  for (size_t size_class = 1; size_class <= 6; ++size_class) {
    states[size_class].capacity_misses = 100 - size_class;
    // Room for exactly one batch, so that no class takes more than its share
    // of the unallocated bytes.
    states[size_class].max_capacity = states[size_class].capacity + 2;
  }
  // The top candidate cannot grow, so the sixth class is considered too.
  states[1].capacity = states[1].max_capacity;
  size_t next_steal = 7;
  const SizeClassResizePlan plan =
      Policy::Plan(absl::MakeSpan(states),
                   /*available_bytes=*/10 * kLargeObject, next_steal);
  EXPECT_EQ(plan.num_resizes, 5);
  EXPECT_EQ(plan.available_bytes, 0);
  EXPECT_EQ(next_steal, 7);
  EXPECT_THAT(Deltas(states), ElementsAre(0, 0, 2, 2, 2, 2, 2, 0));
}

TEST(MissCountResizePolicyTest, TiesPreferSmallerClasses) {
  States states = MakeStates();
  states[5].capacity_misses = 10;
  states[3].capacity_misses = 10;
  size_t next_steal = 1;
  // Unallocated bytes for a single batch: class 3 is served first and takes
  // them all; class 5 then steals from the scan.
  const SizeClassResizePlan plan = Policy::Plan(
      absl::MakeSpan(states), /*available_bytes=*/2 * kLargeObject, next_steal);
  EXPECT_EQ(plan.num_resizes, 2);
  EXPECT_THAT(Deltas(states), ElementsAre(0, -1, -1, 2, 0, 2, 0, 0));
  EXPECT_EQ(next_steal, 2);
}

TEST(MissCountResizePolicyTest, TicksWrapAround) {
  States states = MakeStates();
  states[1].capacity_misses = 10;
  states[2].quiescent_ticks = kMaxQuiescentTicks;
  states[3].quiescent_ticks = kMaxQuiescentTicks;
  size_t next_steal = 2;
  const SizeClassResizePlan plan =
      Policy::Plan(absl::MakeSpan(states), /*available_bytes=*/0, next_steal);
  EXPECT_EQ(plan.num_resizes, 1);
  EXPECT_THAT(Deltas(states), ElementsAre(0, 2, -1, -1, 0, 0, 0, 0));
  EXPECT_THAT(Ticks(states), ElementsAre(0, 0, 0, 0, 0, 0, 0, 0));
}

TEST(MissDensityResizePolicyTest, HoldsInsideHysteresisBand) {
  States states = MakeDensityStates();
  states[1].misses = kMissesAtTarget / 4;  // Exactly the shrink density.
  states[2].misses = kMissesAtTarget - 1;
  const SizeClassResizePlan plan = Density::Plan(
      absl::MakeSpan(states), /*available_bytes=*/1000, kInterval);
  EXPECT_EQ(plan.available_bytes, 1000);
  EXPECT_EQ(plan.num_resizes, 0);
  EXPECT_THAT(Deltas(states), ElementsAre(0, 0, 0, 0, 0, 0, 0, 0));
  EXPECT_THAT(FastPathGrowth(states), ElementsAre(true, false, false, false,
                                                  false, false, false, false));
}

TEST(MissDensityResizePolicyTest, NoMissesHalvesToTwoBatches) {
  States states = MakeDensityStates();
  states[1].misses = 0;
  states[2].misses = 0;
  states[2].capacity = 12;  // Halving would go below two batches.
  states[3].misses = 0;
  states[3].capacity = 8;  // At two batches already.
  states[4].misses = 0;
  states[4].capacity = 6;  // Below two batches: left alone.
  const SizeClassResizePlan plan =
      Density::Plan(absl::MakeSpan(states), /*available_bytes=*/0, kInterval);
  EXPECT_THAT(Deltas(states), ElementsAre(0, -10, -4, 0, 0, 0, 0, 0));
  EXPECT_EQ(plan.available_bytes, 14 * 1024);
  EXPECT_EQ(plan.num_resizes, 2);
}

TEST(MissDensityResizePolicyTest, LowDensityHalves) {
  States states = MakeDensityStates();
  // One miss over 100 KiB: a twentieth of the target density.
  states[1].capacity = 100;
  states[1].misses = 1;
  const SizeClassResizePlan plan =
      Density::Plan(absl::MakeSpan(states), /*available_bytes=*/0, kInterval);
  EXPECT_THAT(Deltas(states), ElementsAre(0, -50, 0, 0, 0, 0, 0, 0));
  EXPECT_EQ(plan.available_bytes, 50 * 1024);
  EXPECT_FALSE(states[1].allow_fast_path_growth);
}

TEST(MissDensityResizePolicyTest, GrowsTowardTargetDensity) {
  States states = MakeDensityStates();
  // Five times the target: the target capacity, 20 * sqrt(5) = 44.7, is
  // clamped to double the capacity.
  states[1].misses = 5 * kMissesAtTarget;
  // Twice the target: 20 * sqrt(2) = 28.3, rounded to whole batches.
  states[2].misses = 2 * kMissesAtTarget;
  // 1.25 times the target: 20 * sqrt(1.25) = 22.4, rounded up to a batch.
  states[3].misses = 5 * kMissesAtTarget / 4;
  // Exactly the target: already the right size.
  states[4].misses = kMissesAtTarget;
  const SizeClassResizePlan plan = Density::Plan(
      absl::MakeSpan(states), /*available_bytes=*/100 * 1024, kInterval);
  EXPECT_THAT(Deltas(states), ElementsAre(0, 20, 8, 4, 0, 0, 0, 0));
  EXPECT_EQ(plan.available_bytes, 68 * 1024);
  EXPECT_EQ(plan.num_resizes, 3);
  EXPECT_THAT(FastPathGrowth(states),
              ElementsAre(true, true, true, true, true, false, false, false));
}

TEST(MissDensityResizePolicyTest, GrowthIsCappedAtMaxCapacity) {
  States states = MakeDensityStates();
  states[1].misses = 5 * kMissesAtTarget;
  states[1].max_capacity = 30;
  const SizeClassResizePlan plan = Density::Plan(
      absl::MakeSpan(states), /*available_bytes=*/100 * 1024, kInterval);
  EXPECT_THAT(Deltas(states), ElementsAre(0, 10, 0, 0, 0, 0, 0, 0));
  EXPECT_EQ(plan.available_bytes, 90 * 1024);
}

TEST(MissDensityResizePolicyTest, EmptyClassWithMissesIsFundedFirst) {
  States states = MakeDensityStates();
  for (size_t size_class = 1; size_class < kNumClasses; ++size_class) {
    states[size_class].misses = kMissesAtTarget - 1;  // Not victims.
  }
  states[1].capacity = 0;
  states[1].misses = 1;
  states[2].misses = 2 * kMissesAtTarget;  // Wants 8 more objects.
  // Two batches for the empty class take all of the unallocated bytes.
  const SizeClassResizePlan plan = Density::Plan(
      absl::MakeSpan(states), /*available_bytes=*/8 * 1024, kInterval);
  EXPECT_THAT(Deltas(states), ElementsAre(0, 8, 0, 0, 0, 0, 0, 0));
  EXPECT_EQ(plan.available_bytes, 0);
  EXPECT_EQ(plan.num_resizes, 1);
  EXPECT_TRUE(states[1].allow_fast_path_growth);
}

TEST(MissDensityResizePolicyTest, EmptyClassWithoutMissesIsLeftAlone) {
  States states = MakeDensityStates();
  states[1].capacity = 0;
  states[1].misses = 0;
  const SizeClassResizePlan plan = Density::Plan(
      absl::MakeSpan(states), /*available_bytes=*/8 * 1024, kInterval);
  EXPECT_THAT(Deltas(states), ElementsAre(0, 0, 0, 0, 0, 0, 0, 0));
  EXPECT_EQ(plan.available_bytes, 8 * 1024);
  EXPECT_TRUE(states[1].allow_fast_path_growth);
}

TEST(MissDensityResizePolicyTest, UnallocatedBytesAreUsedBeforeVictims) {
  States states = MakeDensityStates();
  states[1].misses = 5 * kMissesAtTarget;  // Wants 20 more objects.
  const SizeClassResizePlan plan = Density::Plan(
      absl::MakeSpan(states), /*available_bytes=*/20 * 1024, kInterval);
  EXPECT_THAT(Deltas(states), ElementsAre(0, 20, 0, 0, 0, 0, 0, 0));
  EXPECT_EQ(plan.available_bytes, 0);
  EXPECT_EQ(plan.num_resizes, 1);
}

TEST(MissDensityResizePolicyTest, PartialFundingGrowsWhatItCan) {
  States states = MakeDensityStates();
  for (size_t size_class = 1; size_class < kNumClasses; ++size_class) {
    states[size_class].misses = kMissesAtTarget - 1;  // Not victims.
  }
  states[1].misses = 2 * kMissesAtTarget;  // Wants 8 more objects.
  const SizeClassResizePlan plan = Density::Plan(
      absl::MakeSpan(states), /*available_bytes=*/5 * 1024 + 100, kInterval);
  EXPECT_THAT(Deltas(states), ElementsAre(0, 5, 0, 0, 0, 0, 0, 0));
  EXPECT_EQ(plan.available_bytes, 100);
}

TEST(MissDensityResizePolicyTest, VictimsFundGrowthLowestDensityFirst) {
  States states = MakeDensityStates();
  // Ten times the target over 40 KiB: wants 40 more objects.
  states[1].capacity = 40;
  states[1].misses = 10 * kMissesAtTarget * 2;
  // No misses: halves on its own, then is a victim down to two batches.
  states[2].misses = 0;
  const SizeClassResizePlan plan =
      Density::Plan(absl::MakeSpan(states), /*available_bytes=*/0, kInterval);
  // Class 2 releases 10 KiB by halving and 2 KiB more as the lowest density
  // victim; the remaining victims, highest size class first among equals,
  // each give up to half their capacity until the grower is funded exactly.
  EXPECT_THAT(Deltas(states), ElementsAre(0, 40, -12, 0, 0, -8, -10, -10));
  EXPECT_EQ(plan.available_bytes, 0);
  EXPECT_EQ(plan.num_resizes, 6);
}

TEST(MissDensityResizePolicyTest, VictimsNeedAQuarterOfTheGrowersDensity) {
  States states = MakeDensityStates();
  states[1].misses = 2 * kMissesAtTarget;  // Wants 8 more objects.
  for (size_t size_class = 2; size_class < kNumClasses; ++size_class) {
    states[size_class].misses = kMissesAtTarget - 1;  // More than a quarter.
  }
  SizeClassResizePlan plan =
      Density::Plan(absl::MakeSpan(states), /*available_bytes=*/0, kInterval);
  EXPECT_THAT(Deltas(states), ElementsAre(0, 0, 0, 0, 0, 0, 0, 0));
  EXPECT_EQ(plan.available_bytes, 0);
  EXPECT_EQ(plan.num_resizes, 0);

  states[7].misses = kMissesAtTarget / 2;  // Exactly a quarter.
  plan =
      Density::Plan(absl::MakeSpan(states), /*available_bytes=*/0, kInterval);
  EXPECT_THAT(Deltas(states), ElementsAre(0, 8, 0, 0, 0, 0, 0, -8));
  EXPECT_EQ(plan.available_bytes, 0);
  EXPECT_EQ(plan.num_resizes, 2);
}

TEST(MissDensityResizePolicyTest, ClassesWithoutCapacityAreIgnored) {
  States states = MakeDensityStates();
  states[1].max_capacity = 0;
  states[1].capacity = 0;
  states[1].misses = 100;
  states[2].misses = 0;
  const SizeClassResizePlan plan =
      Density::Plan(absl::MakeSpan(states), /*available_bytes=*/0, kInterval);
  EXPECT_THAT(Deltas(states), ElementsAre(0, 0, -10, 0, 0, 0, 0, 0));
  EXPECT_EQ(plan.available_bytes, 10 * 1024);
}

}  // namespace
}  // namespace tcmalloc_internal
}  // namespace tcmalloc
