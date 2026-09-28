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

#include "tcmalloc/internal/two_level_array.h"

#include <stddef.h>

#include <new>
#include <vector>

#include "gtest/gtest.h"

namespace tcmalloc {
namespace tcmalloc_internal {
namespace {

struct NewAllocator {
  std::vector<size_t>* allocations = nullptr;
  void* operator()(size_t bytes, std::align_val_t alignment) const {
    if (allocations != nullptr) {
      allocations->push_back(bytes);
    }
    return ::operator new(bytes, alignment);
  }
};

TEST(TwoLevelArrayTest, BatchInitialBucketsAllocation) {
  std::vector<size_t> allocations;
  NewAllocator alloc{&allocations};
  TwoLevelArray<int, NewAllocator> arr(alloc);
  EXPECT_TRUE(allocations.empty());

  // Allocate all first 5 buckets in a single batch (31 elements).
  arr.Init(5);
  ASSERT_EQ(allocations.size(), 1);
  EXPECT_EQ(allocations[0], 31 * sizeof(int));

  // Pushing elements 0..30 should NOT trigger any additional allocations.
  for (int i = 0; i <= 30; ++i) {
    arr.push_back(i);
    EXPECT_EQ(allocations.size(), 1);
  }

  // Pushing element 31 (bucket 26) triggers allocation of 32 elements.
  arr.push_back(31);
  ASSERT_EQ(allocations.size(), 2);
  EXPECT_EQ(allocations[1], 32 * sizeof(int));

  // Pushing elements 32..62 should NOT trigger allocations.
  for (int i = 32; i <= 62; ++i) {
    arr.push_back(i);
    EXPECT_EQ(allocations.size(), 2);
  }

  // Pushing element 63 (bucket 25) triggers allocation of 64 elements.
  arr.push_back(63);
  ASSERT_EQ(allocations.size(), 3);
  EXPECT_EQ(allocations[2], 64 * sizeof(int));

  // Verify all elements are correct.
  for (int i = 0; i <= 63; ++i) {
    EXPECT_EQ(arr[i], i);
  }
}

TEST(TwoLevelArrayTest, SingleInitialBucket) {
  std::vector<size_t> allocations;
  NewAllocator alloc{&allocations};
  TwoLevelArray<int, NewAllocator> arr(alloc);
  arr.Init(1);
  ASSERT_EQ(allocations.size(), 1);
  EXPECT_EQ(allocations[0], 1 * sizeof(int));

  arr.push_back(0);
  EXPECT_EQ(allocations.size(), 1);
}

TEST(TwoLevelArrayTest, CustomInitialBuckets) {
  std::vector<size_t> allocations;
  NewAllocator alloc{&allocations};
  TwoLevelArray<int, NewAllocator> arr(alloc);
  arr.Init(3);

  // 3 buckets cover 2^3 - 1 = 7 elements.
  ASSERT_EQ(allocations.size(), 1);
  EXPECT_EQ(allocations[0], 7 * sizeof(int));

  for (int i = 0; i <= 6; ++i) {
    arr.push_back(i);
    EXPECT_EQ(allocations.size(), 1);
  }

  // Pushing element 7 triggers next bucket (8 elements).
  arr.push_back(7);
  ASSERT_EQ(allocations.size(), 2);
  EXPECT_EQ(allocations[1], 8 * sizeof(int));
}

TEST(TwoLevelArrayTest, DoubleInitFails) {
  TwoLevelArray<int, NewAllocator> arr;
  arr.Init(5);
  EXPECT_DEATH(arr.Init(5), "");
}

TEST(TwoLevelArrayTest, InitTooLargeFails) {
  TwoLevelArray<int, NewAllocator> arr;
  EXPECT_DEATH(arr.Init(32), "");
}

TEST(TwoLevelArrayTest, InitZeroFails) {
  TwoLevelArray<int, NewAllocator> arr;
  EXPECT_DEATH(arr.Init(0), "");
}

TEST(TwoLevelArrayTest, PushWithoutInitFails) {
  TwoLevelArray<int, NewAllocator> arr;
  EXPECT_DEATH(arr.push_back(0), "");
}

TEST(TwoLevelArrayTest, PushAndPopBack) {
  TwoLevelArray<int, NewAllocator> arr;
  arr.Init(5);
  const int N = 1000;
  for (int i = 0; i < N; ++i) {
    arr.push_back(i * 10);
    EXPECT_EQ(arr[i], i * 10);
  }
  EXPECT_EQ(arr.size(), N);

  // Modify in-place.
  for (int i = 0; i < N; ++i) {
    arr[i] = i * 20;
  }
  for (int i = 0; i < N; ++i) {
    EXPECT_EQ(arr[i], i * 20);
  }

  // Pop back half of elements.
  for (int i = N - 1; i >= N / 2; --i) {
    EXPECT_EQ(arr[arr.size() - 1], i * 20);
    arr.pop_back();
  }
  EXPECT_EQ(arr.size(), N / 2);

  // Re-push.
  for (int i = N / 2; i < N; ++i) {
    arr.push_back(i * 30);
  }
  EXPECT_EQ(arr.size(), N);
  for (int i = 0; i < N / 2; ++i) {
    EXPECT_EQ(arr[i], i * 20);
  }
  for (int i = N / 2; i < N; ++i) {
    EXPECT_EQ(arr[i], i * 30);
  }
}

TEST(TwoLevelArrayTest, PointerElements) {
  TwoLevelArray<int*, NewAllocator> arr;
  arr.Init(5);
  std::vector<int> vals = {1, 2, 3, 4, 5, 6, 7, 8};
  for (int& v : vals) {
    arr.push_back(&v);
  }
  EXPECT_EQ(arr.size(), vals.size());
  for (size_t i = 0; i < vals.size(); ++i) {
    EXPECT_EQ(*arr[i], vals[i]);
  }
}

TEST(TwoLevelArrayTest, ForEach) {
  TwoLevelArray<int, NewAllocator> arr;
  arr.Init(5);

  arr.ForEach([&](int val) { FAIL(); });
  arr.ForEachUpTo(0, [&](int val) { FAIL(); });

  const int N = 1000;
  for (int i = 0; i < N; ++i) {
    arr.push_back(i);
  }

  std::vector<int> collected;
  arr.ForEach([&](int val) { collected.push_back(val); });
  ASSERT_EQ(collected.size(), N);
  for (int i = 0; i < N; ++i) {
    EXPECT_EQ(collected[i], i);
  }

  arr.ForEachUpTo(0, [&](int val) { FAIL(); });

  size_t num_calls = 0;
  arr.ForEachUpTo(1, [&](int val) { EXPECT_EQ(collected[num_calls++], val); });
  EXPECT_EQ(num_calls, 1);

  num_calls = 0;
  arr.ForEachUpTo(N / 2,
                  [&](int val) { EXPECT_EQ(collected[num_calls++], val); });
  EXPECT_EQ(num_calls, N / 2);

  num_calls = 0;
  arr.ForEachUpTo(N, [&](int val) { EXPECT_EQ(collected[num_calls++], val); });
  EXPECT_EQ(num_calls, N);

  arr.ForEach([](int& val) { val *= 2; });
  for (int i = 0; i < N; ++i) {
    EXPECT_EQ(arr[i], i * 2);
  }
}

}  // namespace
}  // namespace tcmalloc_internal
}  // namespace tcmalloc
