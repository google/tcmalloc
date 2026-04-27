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

#ifndef TCMALLOC_INTERNAL_BYTES_H_
#define TCMALLOC_INTERNAL_BYTES_H_

#include <cstddef>
#include <limits>
#include <string>

#include "absl/base/attributes.h"
#include "absl/strings/numbers.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/string_view.h"
#include "tcmalloc/internal/config.h"
#include "tcmalloc/internal/logging.h"
#include "tcmalloc/internal/optimization.h"

GOOGLE_MALLOC_SECTION_BEGIN
namespace tcmalloc {
namespace tcmalloc_internal {

class ABSL_ATTRIBUTE_TRIVIAL_ABI Bytes {
 public:
  constexpr Bytes() : n_(0) {}
  explicit constexpr Bytes(size_t n) : n_(n) {}

  constexpr Bytes(const Bytes&) = default;
  constexpr Bytes& operator=(const Bytes&) = default;

  constexpr size_t raw_num() const { return n_; }

  static constexpr Bytes min() { return Bytes(0); }
  static constexpr Bytes max() {
    return Bytes(std::numeric_limits<size_t>::max());
  }

  constexpr Bytes& operator+=(Bytes rhs) {
    TC_ASSERT_LE(rhs.n_, std::numeric_limits<size_t>::max() - n_);
    n_ += rhs.n_;
    return *this;
  }

  constexpr Bytes& operator-=(Bytes rhs) {
    TC_ASSERT_GE(n_, rhs.n_);
    n_ -= rhs.n_;
    return *this;
  }

  constexpr Bytes& operator*=(size_t rhs) {
    if (rhs != 0) {
      TC_ASSERT_LE(n_, std::numeric_limits<size_t>::max() / rhs);
    }
    n_ *= rhs;
    return *this;
  }

  constexpr Bytes& operator/=(size_t rhs) {
    TC_ASSERT_NE(rhs, 0u);
    n_ /= rhs;
    return *this;
  }

  constexpr Bytes& operator%=(Bytes rhs) {
    TC_ASSERT_NE(rhs.n_, 0u);
    n_ %= rhs.n_;
    return *this;
  }

  friend constexpr bool operator<(Bytes lhs, Bytes rhs) {
    return lhs.n_ < rhs.n_;
  }
  friend constexpr bool operator>(Bytes lhs, Bytes rhs) {
    return lhs.n_ > rhs.n_;
  }
  friend constexpr bool operator<=(Bytes lhs, Bytes rhs) {
    return lhs.n_ <= rhs.n_;
  }
  friend constexpr bool operator>=(Bytes lhs, Bytes rhs) {
    return lhs.n_ >= rhs.n_;
  }
  friend constexpr bool operator==(Bytes lhs, Bytes rhs) {
    return lhs.n_ == rhs.n_;
  }
  friend constexpr bool operator!=(Bytes lhs, Bytes rhs) {
    return lhs.n_ != rhs.n_;
  }

  template <typename Sink>
  friend void AbslStringify(Sink& sink, const Bytes& v) {
    absl::Format(&sink, "%zu", v.raw_num());
  }

 private:
  size_t n_;
};

inline bool AbslParseFlag(absl::string_view text, Bytes* b,
                          std::string* /* error */) {
  size_t n;
  if (!absl::SimpleAtoi(text, &n)) {
    return false;
  }
  *b = Bytes(n);
  return true;
}

inline std::string AbslUnparseFlag(Bytes b) {
  return absl::StrCat(b.raw_num());
}

inline Bytes& operator++(Bytes& b) { return b += Bytes(1); }
inline Bytes& operator--(Bytes& b) { return b -= Bytes(1); }

[[nodiscard]]
TCMALLOC_ATTRIBUTE_CONST inline constexpr Bytes operator+(Bytes lhs,
                                                          Bytes rhs) {
  return lhs += rhs;
}

[[nodiscard]]
TCMALLOC_ATTRIBUTE_CONST inline constexpr Bytes operator-(Bytes lhs,
                                                          Bytes rhs) {
  return lhs -= rhs;
}

[[nodiscard]]
TCMALLOC_ATTRIBUTE_CONST inline constexpr Bytes operator*(Bytes lhs,
                                                          size_t rhs) {
  return lhs *= rhs;
}

[[nodiscard]]
TCMALLOC_ATTRIBUTE_CONST inline constexpr Bytes operator*(size_t lhs,
                                                          Bytes rhs) {
  return rhs *= lhs;
}

[[nodiscard]]
TCMALLOC_ATTRIBUTE_CONST inline constexpr size_t operator/(Bytes lhs,
                                                           Bytes rhs) {
  TC_ASSERT_NE(rhs.raw_num(), 0u);
  return lhs.raw_num() / rhs.raw_num();
}

[[nodiscard]]
TCMALLOC_ATTRIBUTE_CONST inline constexpr Bytes operator/(Bytes lhs,
                                                          size_t rhs) {
  TC_ASSERT_NE(rhs, 0u);
  return lhs /= rhs;
}

[[nodiscard]]
TCMALLOC_ATTRIBUTE_CONST inline constexpr Bytes operator%(Bytes lhs,
                                                          Bytes rhs) {
  TC_ASSERT_NE(rhs.raw_num(), 0u);
  return lhs %= rhs;
}

}  // namespace tcmalloc_internal
}  // namespace tcmalloc
GOOGLE_MALLOC_SECTION_END

#endif  // TCMALLOC_INTERNAL_BYTES_H_
