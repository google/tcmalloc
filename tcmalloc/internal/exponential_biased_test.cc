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

#include "tcmalloc/internal/exponential_biased.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "gtest/gtest.h"
#include "tcmalloc/testing/testutil.h"

namespace tcmalloc {
namespace tcmalloc_internal {
namespace {

// Testing that NextRandom + GetRandom generates uniform random numbers.
// Applies the Anderson-Darling test for uniformity.
void TestNextRandom(size_t n) {
  ExponentialBiased::State x{1};
  constexpr double kMaxRandomValue =
      static_cast<double>(uint64_t{1} << ExponentialBiased::kRandomBits);
  // Initialize
  for (int i = 1; i <= 20; i++) {  // 20 mimics sampler.Init()
    x = ExponentialBiased::NextRandom(x);
  }
  std::vector<uint32_t> int_random_sample(n);
  // Collect samples
  for (size_t i = 0; i < n; i++) {
    int_random_sample[i] = ExponentialBiased::GetRandom(x);
    x = ExponentialBiased::NextRandom(x);
  }
  // First sort them...
  std::sort(int_random_sample.begin(), int_random_sample.end());
  std::vector<double> random_sample(n);
  // Convert them to uniform randoms (in the range [0,1])
  for (size_t i = 0; i < n; i++) {
    random_sample[i] =
        static_cast<double>(int_random_sample[i]) / kMaxRandomValue;
  }
  // Now compute the Anderson-Darling statistic
  double ad_pvalue = AndersonDarlingTest(random_sample);
  EXPECT_GT(std::min(ad_pvalue, 1 - ad_pvalue), 0.0001)
      << "prng is not uniform: n = " << n << " p = " << ad_pvalue;
}

TEST(ExponentialBiased, TestNextRandom_MultipleValues) {
  TestNextRandom(10);  // Check short-range correlation
  TestNextRandom(100);
  TestNextRandom(1000);
  TestNextRandom(10000);  // Make sure there's no systematic error
}

// Test that high bits above kStateBits in a seed do not affect outputs.
TEST(ExponentialBiased, NextRand_range) {
  constexpr uint64_t kOne = 1;
  ExponentialBiased::State x{kOne << 55};
  ExponentialBiased::State y{0};
  for (int i = 1; i <= (1 << 22); i++) {
    x = ExponentialBiased::NextRandom(x);
    y = ExponentialBiased::NextRandom(y);
    ASSERT_EQ(ExponentialBiased::GetRandom(x), ExponentialBiased::GetRandom(y));
  }
}

// Tests certain arithmetic operations to make sure they compute what we
// expect them to (for testing across different platforms).
TEST(ExponentialBiased, arithmetic_1) {
  ExponentialBiased::State rnd{1};
  uint64_t raw = 1;
  constexpr uint64_t kModMask =
      (uint64_t{1} << ExponentialBiased::kStateBits) - 1;
  for (int i = 1; i <= (1 << 27); i++) {
    rnd = ExponentialBiased::NextRandom(rnd);
    raw = (UINT64_C(0x5DEECE66D) * raw + 0xB) & kModMask;
    const uint32_t r = ExponentialBiased::GetRandom(rnd);
    ASSERT_EQ(r, static_cast<uint32_t>(raw >> 16));
    double q = (r >> (ExponentialBiased::kRandomBits - 26)) + 1.0;
    ASSERT_GE(q, 0) << r;
  }
  // Test some potentially out of bounds seed values for rnd.
  for (int i = 1; i < 64; i++) {
    ExponentialBiased::State seed{uint64_t{1} << i};
    const uint32_t r = ExponentialBiased::GetRandom(seed);
    double q = (r >> (ExponentialBiased::kRandomBits - 26)) + 1.0;
    ASSERT_GE(q, 0) << " r=" << r << " i=" << i;
  }
}

TEST(ExponentialBiased, RandomWrapperMatchesDirectState) {
  Random rng(42);
  ExponentialBiased::State state{42};
  for (int i = 0; i < 100; ++i) {
    state = ExponentialBiased::NextRandom(state);
    EXPECT_EQ(rng.Next(), ExponentialBiased::GetRandom(state));
  }
  rng.Reset(42);
  state = ExponentialBiased::NextRandom(ExponentialBiased::State{42});
  EXPECT_EQ(rng.Next(), ExponentialBiased::GetRandom(state));
}

TEST(ExponentialBiased, CoinFlip) {
  // Ensure that the low bits contain good randomness and can be as a coin flip.
  for (uint64_t seed = 0; seed < 100; seed++) {
    ExponentialBiased::State rnd{seed};
    int even = 0;
    constexpr int kIters = 100;
    for (int i = 0; i < 2 * kIters; i++) {
      rnd = ExponentialBiased::NextRandom(rnd);
      // Check that it works even if we look at every second value
      // (i.e. if the rand is used twice per some operation).
      // This fails without GetRandom, which caused issues for guarded page
      // allocator sampling (left-right-alignment decision).
      if (i % 2) {
        even += static_cast<int>(ExponentialBiased::GetRandom(rnd) % 2);
      }
    }
    EXPECT_GT(even, kIters / 10) << seed;
    EXPECT_LT(even, kIters / 10 * 9) << seed;
  }
}
}  // namespace
}  // namespace tcmalloc_internal
}  // namespace tcmalloc
