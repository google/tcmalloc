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

#include "tcmalloc/internal/constant_divisor.h"

#include <algorithm>
#include <cstdint>
#include <limits>

#include "gtest/gtest.h"
#include "absl/random/bit_gen_ref.h"
#include "absl/random/distributions.h"
#include "absl/random/random.h"

namespace tcmalloc::tcmalloc_internal {
namespace {

TEST(ConstantDivisorUint8Test, Exhaustive) {
  for (int d = 1; d < 256; ++d) {
    const ConstantDivisor<uint8_t> divisor(static_cast<uint8_t>(d));
    for (int n = 0; n < 256; ++n) {
      const uint8_t num = static_cast<uint8_t>(n);
      ASSERT_EQ(divisor.div(num), num / d) << "n=" << n << " d=" << d;
      ASSERT_EQ(num / divisor, num / d) << "n=" << n << " d=" << d;
    }
  }
}

TEST(ConstantDivisorTest, ConstexprEvaluation) {
  constexpr ConstantDivisor<uint64_t> d64(1000);
  static_assert(25000 / d64 == 25);
  static_assert(d64.div(999) == 0);

  constexpr ConstantDivisor<uint32_t> d32(1);
  static_assert(UINT32_MAX / d32 == UINT32_MAX);
}

template <typename T>
class ConstantDivisorTypedTest : public ::testing::Test {
 protected:
  static constexpr T MaxNumerator() {
    if constexpr (sizeof(T) == sizeof(uint64_t)) {
      return (T{1} << 63) - 1;
    } else {
      return std::numeric_limits<T>::max();
    }
  }
};

using UnsignedTypes = ::testing::Types<uint8_t, uint16_t, uint32_t, uint64_t>;
TYPED_TEST_SUITE(ConstantDivisorTypedTest, UnsignedTypes);

TYPED_TEST(ConstantDivisorTypedTest, SimpleAndPowersOfTwo) {
  using T = TypeParam;
  const T max_n = this->MaxNumerator();

  for (int k = 0; k < static_cast<int>(sizeof(T) * 8); ++k) {
    const T d = T{1} << k;
    const ConstantDivisor<T> div_pow2(d);
    EXPECT_EQ(T{0} / div_pow2, 0);
    EXPECT_EQ(T{1} / div_pow2, T{1} / d);
    if (d <= max_n) {
      EXPECT_EQ(d / div_pow2, 1);
    }
    if (d - 1 <= max_n) {
      EXPECT_EQ(static_cast<T>(d - 1) / div_pow2, 0);
    }
    if (d <= max_n - 1) {
      EXPECT_EQ(static_cast<T>(d + 1) / div_pow2, static_cast<T>(d + 1) / d);
    }
    EXPECT_EQ(max_n / div_pow2, max_n / d);
  }
}

TYPED_TEST(ConstantDivisorTypedTest, BoundaryCases) {
  using T = TypeParam;
  const T max_n = this->MaxNumerator();
  const T max_d = std::numeric_limits<T>::max();

  const ConstantDivisor<T> div_max(max_d);
  EXPECT_EQ(T{0} / div_max, 0);
  EXPECT_EQ(max_n / div_max, max_n / max_d);

  const ConstantDivisor<T> div_max_minus_1(max_d - 1);
  EXPECT_EQ(max_n / div_max_minus_1, max_n / (max_d - 1));

  for (T d : {T{1}, T{2}, T{3}, T{5}, T{7}, T{10}, T{21}, T{99}, T{255}}) {
    const ConstantDivisor<T> divisor(d);
    for (T n : {T{0}, T{1}, T{2}, static_cast<T>(d - 1), d,
                static_cast<T>(std::min<uint64_t>(max_n, uint64_t{d} + 1)),
                static_cast<T>(max_n - 1), max_n}) {
      EXPECT_EQ(n / divisor, n / d)
          << "n=" << uint64_t{n} << " d=" << uint64_t{d};
    }
  }
}

template <typename T>
T ChooseBiasedValue(absl::BitGenRef gen, T max_val) {
  return absl::Uniform<T>(gen, 0, max_val) >>
         absl::Uniform<unsigned>(gen, 0, 8 * sizeof(T));
}

TYPED_TEST(ConstantDivisorTypedTest, RandomCases) {
  using T = TypeParam;
  const T max_n = this->MaxNumerator();
  const T max_d = std::numeric_limits<T>::max();
  absl::BitGen gen;
  for (int i = 0; i < 50000; ++i) {
    const T d = std::max<T>(1, ChooseBiasedValue<T>(gen, max_d));
    const T n = ChooseBiasedValue<T>(gen, max_n);
    const ConstantDivisor<T> divisor(d);
    ASSERT_EQ(n / divisor, n / d)
        << "n=" << uint64_t{n} << " d=" << uint64_t{d};
  }
}

}  // namespace
}  // namespace tcmalloc::tcmalloc_internal
