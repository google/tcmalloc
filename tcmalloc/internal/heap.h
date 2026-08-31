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

#ifndef TCMALLOC_INTERNAL_HEAP_H_
#define TCMALLOC_INTERNAL_HEAP_H_

#include <stddef.h>

#include <utility>

#include "absl/base/attributes.h"
#include "absl/base/optimization.h"
#include "tcmalloc/internal/config.h"
#include "tcmalloc/internal/logging.h"

GOOGLE_MALLOC_SECTION_BEGIN
namespace tcmalloc {
namespace tcmalloc_internal {

// Binary min-heap built on top of an abstract array.
//
// The element type T must provide:
//   - PriorityType;
//   - PriorityType Priority() const;
//   - static bool Compare(Priority a, Priority b);
//   - void SetIndex(size_t index);
//
// The underlying `Array` can be any container providing:
//   - size_t size() const
//   - bool empty() const
//   - arr[size_t] (both non-const and const element access)
//   - arr.push_back(const T&) / arr.push_back(T&&)
//   - arr.pop_back()
//   - arr.Init(Args&&...)
//   - arr.ForEach(Fn&&)
// Currently, TwoLevelArray satisfy these requirements.
//
// We don't use std::push/pop_heap for several reasons:
//  - SetIndex support to keep track of the element's index;
//  - Ability to remove from the middle, and change element's priority;
//  - Ability to push with different priority than element's priority;
//    (this is needed to append/prepend spans in CFL to the group of spans
//     with the same priority);
//  - Always inline of methods and other micro-optimizations in the CFL's
//    critical section.
template <typename T, typename Array>
class Heap {
 public:
  constexpr Heap() = default;
  explicit Heap(Array array) : arr_(std::move(array)) {}

  Heap(const Heap&) = default;
  Heap& operator=(const Heap&) = default;
  Heap(Heap&&) noexcept = default;
  Heap& operator=(Heap&&) noexcept = default;

  using PriorityType = typename T::PriorityType;

  [[nodiscard]] size_t size() const { return arr_.size(); }
  [[nodiscard]] bool empty() const { return arr_.empty(); }

  Array& array() { return arr_; }
  const Array& array() const { return arr_; }

  template <typename... Args>
  void Init(Args&&... args) {
    arr_.Init(std::forward<Args>(args)...);
  }

  ABSL_ATTRIBUTE_ALWAYS_INLINE [[nodiscard]] T& operator[](size_t idx) {
    TC_ASSERT_LT(idx, arr_.size());
    return arr_[idx];
  }

  ABSL_ATTRIBUTE_ALWAYS_INLINE [[nodiscard]] T& Back() {
    TC_ASSERT(!empty());
    return arr_[arr_.size() - 1];
  }

  ABSL_ATTRIBUTE_ALWAYS_INLINE void PopBack() {
    TC_ASSERT(!empty());
    arr_.pop_back();
  }

  // Pushes a new element into the heap with the given priority.
  // Returns the inserted element and its index in the heap.
  // It's caller's responsibility to call SetIndex() on the element.
  // This design is needed for 2 reasons:
  //  - For SpanMeta we must not set non-0 index w/o setting is_in_cfl
  //    (that would lead to memory corruptions, since such SpanMeta
  //     means "middle of a span");
  //  - For CFL uses this leads to more optimal code since the caller
  //    already has SpanMeta pointer (don't need to re-fetch it),
  //    and may need to update other fields in SpanMeta.
  ABSL_ATTRIBUTE_ALWAYS_INLINE [[nodiscard]] std::pair<T*, size_t> Push(
      const T& val, PriorityType prio) {
    const size_t idx = arr_.size();
    arr_.push_back(val);
    return SiftUp(idx, prio);
  }

  ABSL_ATTRIBUTE_ALWAYS_INLINE void Remove(size_t idx) {
    TC_ASSERT_LT(idx, arr_.size());
    size_t last = arr_.size() - 1;
    if (ABSL_PREDICT_FALSE(idx == last)) {
      arr_.pop_back();
      return;
    }
    T& pos = arr_[idx];
    pos = std::move(arr_[last]);
    arr_.pop_back();
    if (idx > 0) {
      auto [ptr, new_idx] = SiftUp(idx, pos.Priority());
      if (new_idx != idx) {
        ptr->SetIndex(new_idx);
        return;
      }
    }
    auto [ptr, new_idx] = SiftDown(idx);
    ptr->SetIndex(new_idx);
  }

  // Adjusts the position of the element at `idx` with the given priority
  // after lowering its priority.
  // It's caller's responsibility to call SetIndex() on the element.
  ABSL_ATTRIBUTE_ALWAYS_INLINE [[nodiscard]] std::pair<T*, size_t> SiftUp(
      size_t idx, PriorityType prio) {
    TC_ASSERT_LT(idx, arr_.size());
    T* ptr = &arr_[idx];
    if (idx == 0) {
      return {ptr, 0};
    }
    size_t next_idx = (idx - 1) / 2;
    T* next_ptr = &arr_[next_idx];
    PriorityType next_prio = next_ptr->Priority();
    if (!T::Compare(prio, next_prio)) {
      return {ptr, idx};
    }
    T val = std::move(*ptr);
    do {
      *ptr = std::move(*next_ptr);
      ptr->SetIndex(idx);
      idx = next_idx;
      ptr = next_ptr;
      if (idx == 0) {
        break;
      }
      next_idx = (idx - 1) / 2;
      next_ptr = &arr_[next_idx];
      next_prio = next_ptr->Priority();
    } while (T::Compare(prio, next_prio));
    *ptr = std::move(val);
    return {ptr, idx};
  }

  // Adjusts the position of the element at `idx` with the given priority
  // after increasing its priority.
  // It's caller's responsibility to call SetIndex() on the element.
  ABSL_ATTRIBUTE_ALWAYS_INLINE [[nodiscard]] std::pair<T*, size_t> SiftDown(
      size_t idx) {
    TC_ASSERT_LT(idx, arr_.size());
    T* ptr = &arr_[idx];
    const size_t size = arr_.size();
    size_t next_idx = 2 * idx + 1;
    if (next_idx >= size) {
      return {ptr, idx};
    }
    T* next_ptr = &arr_[next_idx];
    PriorityType next_prio = next_ptr->Priority();
    if (size_t right_idx = next_idx + 1; right_idx < size) {
      T* right_ptr = &arr_[right_idx];
      PriorityType right_prio = right_ptr->Priority();
      if (T::Compare(right_prio, next_prio)) {
        next_idx = right_idx;
        next_ptr = right_ptr;
        next_prio = right_prio;
      }
    }
    const PriorityType prio = ptr->Priority();
    if (!T::Compare(next_prio, prio)) {
      return {ptr, idx};
    }
    T val = std::move(*ptr);
    do {
      *ptr = std::move(*next_ptr);
      ptr->SetIndex(idx);
      idx = next_idx;
      ptr = next_ptr;
      next_idx = 2 * idx + 1;
      if (next_idx >= size) {
        break;
      }
      next_ptr = &arr_[next_idx];
      next_prio = next_ptr->Priority();
      if (size_t right_idx = next_idx + 1; right_idx < size) {
        T* right_ptr = &arr_[right_idx];
        PriorityType right_prio = right_ptr->Priority();
        if (T::Compare(right_prio, next_prio)) {
          next_idx = right_idx;
          next_ptr = right_ptr;
          next_prio = right_prio;
        }
      }
    } while (T::Compare(next_prio, prio));
    *ptr = std::move(val);
    return {ptr, idx};
  }

  template <typename Fn>
  ABSL_ATTRIBUTE_ALWAYS_INLINE void ForEach(Fn&& fn) {
    arr_.ForEach(std::forward<Fn>(fn));
  }

  template <typename Fn>
  ABSL_ATTRIBUTE_ALWAYS_INLINE void ForEach(Fn&& fn) const {
    arr_.ForEach(std::forward<Fn>(fn));
  }

 private:
  Array arr_;
};

}  // namespace tcmalloc_internal
}  // namespace tcmalloc
GOOGLE_MALLOC_SECTION_END

#endif  // TCMALLOC_INTERNAL_HEAP_H_
