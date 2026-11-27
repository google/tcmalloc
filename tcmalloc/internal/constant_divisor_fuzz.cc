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

#include <cstdint>
#include <limits>

#include "gtest/gtest.h"
#include "fuzztest/fuzztest.h"
#include "tcmalloc/internal/constant_divisor.h"

namespace tcmalloc::tcmalloc_internal {
namespace {

void DivideUint64MatchesHardware(uint64_t n, uint64_t d) {
  const ConstantDivisor<uint64_t> divisor(d);
  EXPECT_EQ(divisor.div(n), n / d);
  EXPECT_EQ(n / divisor, n / d);
  EXPECT_EQ(divisor.mod(n), n % d);
  EXPECT_EQ(n % divisor, n % d);
}
FUZZ_TEST(ConstantDivisorFuzzTest, DivideUint64MatchesHardware)
    .WithDomains(fuzztest::InRange<uint64_t>(0, (uint64_t{1} << 63) - 1),
                 fuzztest::NonZero<uint64_t>());

void DivideUint32MatchesHardware(uint32_t n, uint32_t d) {
  const ConstantDivisor<uint32_t> divisor(d);
  EXPECT_EQ(divisor.div(n), n / d);
  EXPECT_EQ(n / divisor, n / d);
  EXPECT_EQ(divisor.mod(n), n % d);
  EXPECT_EQ(n % divisor, n % d);
}
FUZZ_TEST(ConstantDivisorFuzzTest, DivideUint32MatchesHardware)
    .WithDomains(fuzztest::Arbitrary<uint32_t>(),
                 fuzztest::NonZero<uint32_t>());

void DivideUint16MatchesHardware(uint16_t n, uint16_t d) {
  const ConstantDivisor<uint16_t> divisor(d);
  EXPECT_EQ(divisor.div(n), n / d);
  EXPECT_EQ(n / divisor, n / d);
  EXPECT_EQ(divisor.mod(n), n % d);
  EXPECT_EQ(n % divisor, n % d);
}
FUZZ_TEST(ConstantDivisorFuzzTest, DivideUint16MatchesHardware)
    .WithDomains(fuzztest::Arbitrary<uint16_t>(),
                 fuzztest::NonZero<uint16_t>());

void PowerOfTwoUint64MatchesHardware(uint64_t n, int shift) {
  const uint64_t d = uint64_t{1} << shift;
  const ConstantPowerOfTwoDivisor<uint64_t> divisor(d);
  EXPECT_EQ(divisor.div(n), n / d);
  EXPECT_EQ(n / divisor, n / d);
  EXPECT_EQ(divisor.mod(n), n % d);
  EXPECT_EQ(n % divisor, n % d);
}
FUZZ_TEST(ConstantDivisorFuzzTest, PowerOfTwoUint64MatchesHardware)
    .WithDomains(fuzztest::Arbitrary<uint64_t>(), fuzztest::InRange(0, 63));

}  // namespace
}  // namespace tcmalloc::tcmalloc_internal
