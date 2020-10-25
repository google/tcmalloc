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

#include "tcmalloc/internal/bits.h"

#include <cstdint>
#include <limits>
#include <memory>

#include "absl/random/random.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"

namespace tcmalloc {
namespace tcmalloc_internal {
namespace {

TEST(BitsTest, Log2EdgeCases) {
  EXPECT_EQ(-1, Bits::Log2Floor(0));
  EXPECT_EQ(-1, Bits::Log2Ceiling(0));

  for (int i = 0; i < 32; i++) {
    uint32_t n = 1U << i;
    EXPECT_EQ(i, Bits::Log2Floor(n));
    EXPECT_EQ(i, Bits::Log2Ceiling(n));
    if (n > 2) {
      EXPECT_EQ(i - 1, Bits::Log2Floor(n - 1));
      EXPECT_EQ(i, Bits::Log2Floor(n + 1));
      EXPECT_EQ(i, Bits::Log2Ceiling(n - 1));
      EXPECT_EQ(i + 1, Bits::Log2Ceiling(n + 1));
    }
  }
}

TEST(BitsTest, Log2Random) {
  absl::BitGen random;

  const int kNumIterations = 10000;
  for (int i = 0; i < kNumIterations; i++) {
    int maxbit = -1;
    uint32_t n = 0;
    while (!absl::Bernoulli(random, 1.0 / 32)) {
      int bit = absl::Uniform<int32_t>(random, 0, 32);
      n |= (1U << bit);
      maxbit = std::max(bit, maxbit);
    }
    EXPECT_EQ(maxbit, Bits::Log2Floor(n));
  }
}

TEST(BitsTest, IsZeroOrPow2) {
  EXPECT_TRUE(Bits::IsZeroOrPow2(0));
  EXPECT_TRUE(Bits::IsZeroOrPow2(1));
  EXPECT_TRUE(Bits::IsZeroOrPow2(2));
  EXPECT_FALSE(Bits::IsZeroOrPow2(3));
  EXPECT_TRUE(Bits::IsZeroOrPow2(4));
  EXPECT_FALSE(Bits::IsZeroOrPow2(1337));
  EXPECT_TRUE(Bits::IsZeroOrPow2(65536));
  EXPECT_FALSE(Bits::IsZeroOrPow2(std::numeric_limits<uint32_t>::max()));
  EXPECT_TRUE(Bits::IsZeroOrPow2(uint32_t{1} << 31));
}

TEST(BitsTest, RoundUpToPow2) {
  EXPECT_EQ(Bits::RoundUpToPow2(0), 1);
  EXPECT_EQ(Bits::RoundUpToPow2(1), 1);
  EXPECT_EQ(Bits::RoundUpToPow2(2), 2);
  EXPECT_EQ(Bits::RoundUpToPow2(3), 4);
  EXPECT_EQ(Bits::RoundUpToPow2(4), 4);
  EXPECT_EQ(Bits::RoundUpToPow2(1337), 2048);
  EXPECT_EQ(Bits::RoundUpToPow2(65536), 65536);
  EXPECT_EQ(Bits::RoundUpToPow2(65536 - 1337), 65536);
}

}  // namespace
}  // namespace tcmalloc_internal
}  // namespace tcmalloc
