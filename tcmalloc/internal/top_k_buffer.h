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

#ifndef TCMALLOC_INTERNAL_TOP_K_BUFFER_H_
#define TCMALLOC_INTERNAL_TOP_K_BUFFER_H_

#include <stddef.h>

#include <algorithm>
#include <array>

#include "tcmalloc/internal/config.h"
#include "tcmalloc/internal/logging.h"

GOOGLE_MALLOC_SECTION_BEGIN
namespace tcmalloc {
namespace tcmalloc_internal {

// Retains the top limit_ items according to Compare without maintaining a
// heap on every insertion. Rather than a heap, whose displacement walks
// log2(K) dependent loads per candidate in the worst case, items that rank
// ahead of the cutoff are appended to a 2 * MaxLimit buffer, which is
// compacted to limit_ via std::nth_element whenever it fills and in Finish():
// amortized O(1) per candidate, and nothing per candidate that does not rank
// ahead of the cutoff a compaction established.
template <typename T, size_t MaxLimit, typename Compare>
class TopKBuffer {
 public:
  using value_type = T;
  using iterator = T*;
  using const_iterator = const T*;

  constexpr TopKBuffer() : TopKBuffer(MaxLimit) {}
  constexpr explicit TopKBuffer(int limit) : limit_(limit) {
    TC_ASSERT_GT(limit_, 0);
    TC_ASSERT_LE(static_cast<size_t>(limit_), MaxLimit);
  }

  void Push(const T& item) {
    if (has_cutoff_ && !Compare{}(item, cutoff_)) {
      return;
    }
    items_[size_] = item;
    ++size_;
    if (size_ == 2 * limit_) {
      Compact();
    }
  }

  void Finish() {
    if (size_ > limit_) {
      Compact();
    }
    TC_ASSERT_LE(size_, limit_);
  }

  [[nodiscard]] int size() const { return size_; }
  [[nodiscard]] int limit() const { return limit_; }
  [[nodiscard]] T* begin() { return items_.data(); }
  [[nodiscard]] const T* begin() const { return items_.data(); }
  [[nodiscard]] T* end() { return items_.data() + size_; }
  [[nodiscard]] const T* end() const { return items_.data() + size_; }
  [[nodiscard]] T& operator[](size_t i) {
    TC_ASSERT_LT(i, static_cast<size_t>(size_));
    return items_[i];
  }
  [[nodiscard]] const T& operator[](size_t i) const {
    TC_ASSERT_LT(i, static_cast<size_t>(size_));
    return items_[i];
  }

 private:
  void Compact() {
    TC_ASSERT_GT(size_, limit_);
    auto first = items_.begin();
    auto cut = first + (limit_ - 1);
    std::nth_element(first, cut, first + size_, Compare{});
    cutoff_ = *cut;
    has_cutoff_ = true;
    size_ = limit_;
  }

  int limit_;
  int size_ = 0;
  bool has_cutoff_ = false;
  T cutoff_{};
  std::array<T, 2 * MaxLimit> items_;
};

}  // namespace tcmalloc_internal
}  // namespace tcmalloc
GOOGLE_MALLOC_SECTION_END

#endif  // TCMALLOC_INTERNAL_TOP_K_BUFFER_H_
