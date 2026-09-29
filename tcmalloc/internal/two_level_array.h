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

#ifndef TCMALLOC_INTERNAL_TWO_LEVEL_ARRAY_H_
#define TCMALLOC_INTERNAL_TWO_LEVEL_ARRAY_H_

#include <stddef.h>
#include <stdint.h>

#include <algorithm>
#include <new>
#include <utility>

#include "absl/base/attributes.h"
#include "absl/base/optimization.h"
#include "absl/numeric/bits.h"
#include "tcmalloc/internal/config.h"
#include "tcmalloc/internal/logging.h"

GOOGLE_MALLOC_SECTION_BEGIN
namespace tcmalloc {
namespace tcmalloc_internal {

// TwoLevelArray provides a virtually indexed 2-level array.
//
// It contains 32 first-level buckets covering exponentially larger second-level
// blocks (leafs), filled from the end of the array to allow computing bucket
// index with a single count-leading-zeros instruction:
//   - Bucket 31 covers 1 element (2^0, index 0).
//   - Bucket 30 covers 2 elements (2^1, indices 1..2).
//   - Bucket 29 covers 4 elements (2^2, indices 3..6).
//   - ...
//   - Bucket 0 covers 2^31 elements.
// Total capacity across all 32 buckets is 2^32 - 1 elements.
//
// If configured, the first initial buckets (covering indices 0 to 2^N - 2) are
// allocated together in a single batch when the array first needs storage.
// Buckets store offsetted pointers (block - start_index) so that
// buckets_[Bucket(index)][index] directly indexes the underlying block with
// no offset computation.
//
// Second-level leaf blocks are allocated lazily upon first access via the
// provided Allocator (e.g. Arena allocator), and are never freed.
template <typename T, typename Allocator>
class TwoLevelArray {
 public:
  constexpr TwoLevelArray() = default;
  explicit TwoLevelArray(Allocator& alloc) : alloc_(alloc) {}
  ~TwoLevelArray() = default;
  TwoLevelArray(const TwoLevelArray&) = delete;
  TwoLevelArray& operator=(const TwoLevelArray&) = delete;

  // Initializes the array and allocated memory for the given number of buckets
  // (number of allocated elements is 2^initial_buckets - 1).
  //
  // Must be called before any other method.
  void Init(size_t initial_buckets) {
    TC_CHECK_EQ(buckets_[kNumBuckets - 1], nullptr);
    TC_CHECK_GT(initial_buckets, 0);
    TC_CHECK_LT(initial_buckets, kNumBuckets);
    init_size_ = (1ul << initial_buckets) - 1;
    // Pre-allocating memory here allows fast paths in operator[] and push_back,
    // since we won't need to check for bucket being nullptr for indexes below
    // init_size_.
    AllocateBucket(kNumBuckets - 1);
  }

  size_t size() const { return size_; }
  bool empty() const { return size_ == 0; }

  ABSL_ATTRIBUTE_ALWAYS_INLINE T& operator[](size_t index) {
    TC_ASSERT_LT(index, size_);
    if (ABSL_PREDICT_TRUE(index < init_size_)) {
      // This is strictly an optimization (can be removed). If initial capacity
      // is chosen properly, we can avoid dynamic bucket index in most cases.
      return buckets_[kNumBuckets - 1][index];
    }
    return buckets_[Bucket(index)][index];
  }

  ABSL_ATTRIBUTE_ALWAYS_INLINE const T& operator[](size_t index) const {
    return const_cast<TwoLevelArray&>(*this)[index];
  }

  ABSL_ATTRIBUTE_ALWAYS_INLINE void push_back(const T& val) {
    if (ABSL_PREDICT_TRUE(size_ < init_size_)) {
      // This is strictly an optimization (can be removed). If initial capacity
      // is chosen properly, we can avoid dynamic bucket index and nullptr
      // check in most cases.
      buckets_[kNumBuckets - 1][size_] = val;
    } else {
      const int b = Bucket(size_);
      if (ABSL_PREDICT_FALSE(buckets_[b] == nullptr)) {
        AllocateBucket(b);
      }
      buckets_[b][size_] = val;
    }
    ++size_;
  }

  ABSL_ATTRIBUTE_ALWAYS_INLINE void pop_back() {
    TC_ASSERT_GT(size_, 0);
    --size_;
  }

  template <typename Fn>
  void ForEach(Fn&& fn) {
    ForEachImpl(std::forward<Fn>(fn), size_);
  }

  // Like ForEach, but only iterates up to the given index.
  // This enables lock-free iteration in SampleRecorder.
  // REQUIRES: n <= size().
  template <typename Fn>
  void ForEachUpTo(uint32_t n, Fn&& fn) {
    ForEachImpl(std::forward<Fn>(fn), n);
  }

 private:
  static constexpr size_t kNumBuckets = 32;

  static constexpr ABSL_ATTRIBUTE_ALWAYS_INLINE int Bucket(size_t index) {
    return absl::countl_zero(static_cast<uint32_t>(index + 1));
  }

  template <typename Fn>
  void ForEachImpl(Fn&& fn, uint32_t size) {
    uint32_t count = std::min<uint32_t>(size, init_size_);
    T* ptr = buckets_[kNumBuckets - 1];
    T* end = ptr + count;
    for (; ptr < end; ++ptr) {
      fn(*ptr);
    }
    for (int b = static_cast<int>(kNumBuckets - InitBucketCount() - 1); b >= 0;
         --b) {
      const uint32_t k = (kNumBuckets - 1) - b;
      const uint32_t num_elements = 1u << k;
      const uint32_t start_index = num_elements - 1;
      if (start_index >= size) {
        break;
      }
      uint32_t count = std::min<uint32_t>(size - start_index, num_elements);
      T* ptr = buckets_[b] + start_index;
      T* end = ptr + count;
      for (; ptr < end; ++ptr) {
        fn(*ptr);
      }
    }
  }

  ABSL_ATTRIBUTE_NOINLINE void AllocateBucket(int b) {
    TC_CHECK_GT(init_size_, 0);
    TC_ASSERT_GE(b, 0);
    TC_ASSERT_LT(b, static_cast<int>(kNumBuckets));
    if (b >= static_cast<int>(kNumBuckets - InitBucketCount())) {
      //  Allocate the initial buckets (covering indices 0..2^N-2) together in a
      //  single memory block. Since the initial buckets are stored contiguously
      //  in this block starting from index 0, and each bucket's start index
      //  matches its offset in the block, the offsetted pointer
      //  (block + start_index) - start_index is simply block. We initialize all
      //  initial buckets to point to block directly.
      T* block = Alloc(init_size_);
      for (size_t i = kNumBuckets - InitBucketCount(); i < kNumBuckets; ++i) {
        buckets_[i] = block;
      }
      return;
    }

    const size_t k = (kNumBuckets - 1) - b;
    const size_t num_elements = 1ul << k;
    const size_t start_index = num_elements - 1;
    T* block = Alloc(num_elements);
    // Store an offsetted pointer (block - start_index) into buckets_[b] so that
    // buckets_[b][index] accesses block[index - start_index] directly without
    // any runtime offset adjustment or masking in operator[].
    buckets_[b] = block - start_index;
  }

  T* Alloc(size_t n) {
    // Align storage on sizeof(T) if it's a power of two, so that elements
    // don't cross cache lines.
    constexpr size_t S = sizeof(T);
    constexpr size_t A = std::max(alignof(T), S & (S - 1) ? 1 : S);
    return static_cast<T*>(alloc_(n * S, std::align_val_t(A)));
  }

  uint32_t InitBucketCount() const {
    return kNumBuckets - Bucket(init_size_) - 1;
  }

  // We place buckets_ first, followed by size_ to keep frequently accessed
  // data together (the end of the buckets_ is hot, while the beginning
  // is likely unused).
  T* buckets_[kNumBuckets] = {nullptr};
  uint32_t size_ = 0;
  uint32_t init_size_ = 0;
  ABSL_ATTRIBUTE_NO_UNIQUE_ADDRESS Allocator alloc_;
};

}  // namespace tcmalloc_internal
}  // namespace tcmalloc
GOOGLE_MALLOC_SECTION_END

#endif  // TCMALLOC_INTERNAL_TWO_LEVEL_ARRAY_H_
