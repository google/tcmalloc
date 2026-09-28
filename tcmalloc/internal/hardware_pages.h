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

#ifndef TCMALLOC_INTERNAL_HARDWARE_PAGES_H_
#define TCMALLOC_INTERNAL_HARDWARE_PAGES_H_

#include <cstddef>
#include <limits>
#include <string>
#include <utility>

#include "absl/base/attributes.h"
#include "absl/strings/numbers.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_format.h"
#include "absl/strings/string_view.h"
#include "tcmalloc/internal/config.h"
#include "tcmalloc/internal/logging.h"
#include "tcmalloc/internal/optimization.h"
#include "tcmalloc/internal/page_size.h"

GOOGLE_MALLOC_SECTION_BEGIN
namespace tcmalloc {
namespace tcmalloc_internal {

// A count of native (hardware/OS) pages, as reported by GetPageSize().  This
// is distinct from Length (TCMalloc pages) and HugeLength (hugepages).
class ABSL_ATTRIBUTE_TRIVIAL_ABI HardwareLength {
 public:
  constexpr HardwareLength() : n_(0) {}
  explicit constexpr HardwareLength(size_t n) : n_(n) {}

  constexpr HardwareLength(const HardwareLength&) = default;
  constexpr HardwareLength& operator=(const HardwareLength&) = default;

  constexpr size_t raw_num() const { return n_; }
  // Scales by the runtime page size, not EXEC_PAGESIZE: the latter is a
  // compile-time upper bound (64 KiB on aarch64 headers) that can differ from
  // the page size of the running kernel.
  size_t in_bytes() const { return n_ * GetPageSize(); }

  static constexpr HardwareLength min() { return HardwareLength(0); }
  static constexpr HardwareLength max() {
    return HardwareLength(std::numeric_limits<size_t>::max());
  }

  constexpr HardwareLength& operator+=(HardwareLength rhs) {
    TC_ASSERT_LE(rhs.n_, std::numeric_limits<size_t>::max() - n_);
    n_ += rhs.n_;
    return *this;
  }

  constexpr HardwareLength& operator-=(HardwareLength rhs) {
    TC_ASSERT_GE(n_, rhs.n_);
    n_ -= rhs.n_;
    return *this;
  }

  constexpr HardwareLength& operator*=(size_t rhs) {
    if (rhs != 0) {
      TC_ASSERT_LE(n_, std::numeric_limits<size_t>::max() / rhs);
    }
    n_ *= rhs;
    return *this;
  }

  constexpr HardwareLength& operator/=(size_t rhs) {
    TC_ASSERT_NE(rhs, 0u);
    n_ /= rhs;
    return *this;
  }

  constexpr HardwareLength& operator%=(HardwareLength rhs) {
    TC_ASSERT_NE(rhs.n_, 0u);
    n_ %= rhs.n_;
    return *this;
  }

  friend constexpr bool operator<(HardwareLength lhs, HardwareLength rhs) {
    return lhs.n_ < rhs.n_;
  }
  friend constexpr bool operator>(HardwareLength lhs, HardwareLength rhs) {
    return lhs.n_ > rhs.n_;
  }
  friend constexpr bool operator<=(HardwareLength lhs, HardwareLength rhs) {
    return lhs.n_ <= rhs.n_;
  }
  friend constexpr bool operator>=(HardwareLength lhs, HardwareLength rhs) {
    return lhs.n_ >= rhs.n_;
  }
  friend constexpr bool operator==(HardwareLength lhs, HardwareLength rhs) {
    return lhs.n_ == rhs.n_;
  }
  friend constexpr bool operator!=(HardwareLength lhs, HardwareLength rhs) {
    return lhs.n_ != rhs.n_;
  }

  template <typename Sink>
  friend void AbslStringify(Sink& sink, const HardwareLength& v) {
    absl::Format(&sink, "%zu", v.raw_num());
  }

  template <typename H>
  friend H AbslHashValue(H h, const HardwareLength& v) {
    return H::combine(std::move(h), v.n_);
  }

 private:
  size_t n_;
};

inline bool AbslParseFlag(absl::string_view text, HardwareLength* l,
                          std::string* /* error */) {
  size_t n;
  if (!absl::SimpleAtoi(text, &n)) {
    return false;
  }
  *l = HardwareLength(n);
  return true;
}

inline std::string AbslUnparseFlag(HardwareLength l) { return absl::StrCat(l); }

inline HardwareLength& operator++(HardwareLength& l) {
  return l += HardwareLength(1);
}
inline HardwareLength operator++(HardwareLength& l, int) {
  HardwareLength tmp = l;
  ++l;
  return tmp;
}
inline HardwareLength& operator--(HardwareLength& l) {
  return l -= HardwareLength(1);
}
inline HardwareLength operator--(HardwareLength& l, int) {
  HardwareLength tmp = l;
  --l;
  return tmp;
}

[[nodiscard]]
TCMALLOC_ATTRIBUTE_CONST inline constexpr HardwareLength operator+(
    HardwareLength lhs, HardwareLength rhs) {
  return lhs += rhs;
}

[[nodiscard]]
TCMALLOC_ATTRIBUTE_CONST inline constexpr HardwareLength operator-(
    HardwareLength lhs, HardwareLength rhs) {
  return lhs -= rhs;
}

[[nodiscard]]
TCMALLOC_ATTRIBUTE_CONST inline constexpr HardwareLength operator*(
    HardwareLength lhs, size_t rhs) {
  return lhs *= rhs;
}

[[nodiscard]]
TCMALLOC_ATTRIBUTE_CONST inline constexpr HardwareLength operator*(
    size_t lhs, HardwareLength rhs) {
  return rhs *= lhs;
}

[[nodiscard]]
TCMALLOC_ATTRIBUTE_CONST inline constexpr size_t operator/(HardwareLength lhs,
                                                           HardwareLength rhs) {
  TC_ASSERT_NE(rhs.raw_num(), 0u);
  return lhs.raw_num() / rhs.raw_num();
}

[[nodiscard]]
TCMALLOC_ATTRIBUTE_CONST inline constexpr HardwareLength operator/(
    HardwareLength lhs, size_t rhs) {
  TC_ASSERT_NE(rhs, 0u);
  return lhs /= rhs;
}

[[nodiscard]]
TCMALLOC_ATTRIBUTE_CONST inline constexpr HardwareLength operator%(
    HardwareLength lhs, HardwareLength rhs) {
  TC_ASSERT_NE(rhs.raw_num(), 0u);
  return lhs %= rhs;
}

TCMALLOC_ATTRIBUTE_CONST
inline constexpr HardwareLength NHardwarePages(size_t n) {
  return HardwareLength(n);
}

}  // namespace tcmalloc_internal
}  // namespace tcmalloc
GOOGLE_MALLOC_SECTION_END

#endif  // TCMALLOC_INTERNAL_HARDWARE_PAGES_H_
