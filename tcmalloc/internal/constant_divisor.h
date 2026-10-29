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
#ifndef TCMALLOC_INTERNAL_CONSTANT_DIVISOR_H_
#define TCMALLOC_INTERNAL_CONSTANT_DIVISOR_H_

#include <cstdint>
#include <type_traits>

#include "absl/numeric/bits.h"
#include "absl/numeric/int128.h"
#include "tcmalloc/internal/config.h"
#include "tcmalloc/internal/logging.h"

GOOGLE_MALLOC_SECTION_BEGIN
namespace tcmalloc {
namespace tcmalloc_internal {

// Precomputes a reciprocal multiplier and shift for repeated unsigned integer
// division by a runtime-invariant divisor.
//
// Based on
// https://ridiculousfish.com/blog/posts/labor-of-division-episode-i.html and
// https://lemire.me/blog/2019/02/20/more-fun-with-fast-remainders-when-the-divisor-is-a-constant/.
//
// For 64-bit unsigned types, we use one fewer bit of precision than necessary
// for arbitrary 64-bit unsigned numerators so that the precomputed multiplier
// always fits in a single uint64_t without an extra overflow-handling addition
// and shift; numerators must therefore be < 2^63. For 8-, 16-, and 32-bit
// unsigned types, the full [0, max()] numerator range is supported.
template <typename T>
class ConstantDivisor {
  static_assert(std::is_unsigned_v<T> && sizeof(T) <= sizeof(uint64_t),
                "ConstantDivisor requires an unsigned integer type <= 64 bits");

 public:
  using value_type = T;

  constexpr ConstantDivisor() = default;

  explicit constexpr ConstantDivisor(T d) {
    TC_ASSERT_GT(d, 0);
    const int l = absl::bit_width(static_cast<T>(d - 1));
    shift_ = static_cast<uint8_t>(kNumeratorBits + l);
    mul_ = static_cast<MulType>(((WideType{1} << shift_) - 1) / d) + 1;
  }

  [[nodiscard]] constexpr T div(T n) const {
    TC_ASSERT_GT(mul_, 0);
    if constexpr (sizeof(T) == sizeof(uint64_t)) {
      TC_ASSERT_LT(n, T{1} << 63);
    }
    return static_cast<T>((static_cast<WideType>(mul_) * n) >> shift_);
  }

  [[nodiscard]] friend constexpr T operator/(T n, const ConstantDivisor& d) {
    return d.div(n);
  }

 private:
  static constexpr int kNumeratorBits =
      sizeof(T) == sizeof(uint64_t) ? 63 : sizeof(T) * 8;
  using MulType =
      std::conditional_t<sizeof(T) <= sizeof(uint16_t), uint32_t, uint64_t>;
  using WideType = std::conditional_t<sizeof(T) <= sizeof(uint16_t), uint64_t,
                                      absl::uint128>;

  MulType mul_ = 0;
  uint8_t shift_ = 0;
};

}  // namespace tcmalloc_internal
}  // namespace tcmalloc
GOOGLE_MALLOC_SECTION_END

#endif  // TCMALLOC_INTERNAL_CONSTANT_DIVISOR_H_
