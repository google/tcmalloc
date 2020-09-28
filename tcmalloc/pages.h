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

#ifndef TCMALLOC_PAGES_H_
#define TCMALLOC_PAGES_H_

#include "tcmalloc/common.h"
#include "tcmalloc/internal/logging.h"

namespace tcmalloc {

// Type that can hold the length of a run of pages
using Length = uintptr_t;

// A single aligned page.
class PageId {
 public:
  constexpr PageId() : pn_(0) {}
  constexpr PageId(const PageId& p) = default;
  constexpr PageId& operator=(const PageId& p) = default;

  constexpr explicit PageId(uintptr_t pn) : pn_(pn) {}

  void* start_addr() const {
    return reinterpret_cast<void*>(pn_ << kPageShift);
  }

  uintptr_t start_uintptr() const { return pn_ << kPageShift; }

  size_t index() const { return pn_; }

  constexpr PageId& operator+=(Length rhs) {
    pn_ += rhs;
    return *this;
  }

  constexpr PageId& operator-=(Length rhs) {
    ASSERT(pn_ >= rhs);
    pn_ -= rhs;
    return *this;
  }

 private:
  friend constexpr bool operator<(PageId lhs, PageId rhs);
  friend constexpr bool operator>(PageId lhs, PageId rhs);
  friend constexpr bool operator<=(PageId lhs, PageId rhs);
  friend constexpr bool operator>=(PageId lhs, PageId rhs);
  friend constexpr bool operator==(PageId lhs, PageId rhs);
  friend constexpr bool operator!=(PageId lhs, PageId rhs);
  friend constexpr Length operator-(PageId lhs, PageId rhs);

  uintptr_t pn_;
};

// Convert byte size into pages.  This won't overflow, but may return
// an unreasonably large value if bytes is huge enough.
inline constexpr Length BytesToLengthCeil(size_t bytes) {
  return (bytes >> kPageShift) + ((bytes & (kPageSize - 1)) > 0 ? 1 : 0);
}

inline constexpr Length BytesToLengthFloor(size_t bytes) {
  return bytes >> kPageShift;
}

inline constexpr Length kMaxValidPages =
    BytesToLengthFloor(~static_cast<Length>(0));
// For all span-lengths < kMaxPages we keep an exact-size list.
inline constexpr Length kMaxPages = Length(1 << (20 - kPageShift));

inline PageId& operator++(PageId& p) {  // NOLINT(runtime/references)
  return p += 1;
}

inline constexpr bool operator<(PageId lhs, PageId rhs) {
  return lhs.pn_ < rhs.pn_;
}

inline constexpr bool operator>(PageId lhs, PageId rhs) {
  return lhs.pn_ > rhs.pn_;
}

inline constexpr bool operator<=(PageId lhs, PageId rhs) {
  return lhs.pn_ <= rhs.pn_;
}

inline constexpr bool operator>=(PageId lhs, PageId rhs) {
  return lhs.pn_ >= rhs.pn_;
}

inline constexpr bool operator==(PageId lhs, PageId rhs) {
  return lhs.pn_ == rhs.pn_;
}

inline constexpr bool operator!=(PageId lhs, PageId rhs) {
  return lhs.pn_ != rhs.pn_;
}

inline constexpr PageId operator+(PageId lhs, Length rhs) { return lhs += rhs; }

inline constexpr PageId operator+(Length lhs, PageId rhs) { return rhs += lhs; }

inline constexpr PageId operator-(PageId lhs, Length rhs) { return lhs -= rhs; }

inline constexpr Length operator-(PageId lhs, PageId rhs) {
  ASSERT(lhs.pn_ >= rhs.pn_);
  return Length(lhs.pn_ - rhs.pn_);
}

inline PageId PageIdContaining(const void* p) {
  return PageId(reinterpret_cast<uintptr_t>(p) >> kPageShift);
}

}  // namespace tcmalloc

#endif  // TCMALLOC_PAGES_H_
