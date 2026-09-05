// Copyright 2025 The TCMalloc Authors
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

#include "tcmalloc/alloc_at_least.h"

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>

#include "gtest/gtest.h"

namespace {

TEST(AllocAtLeastTest, AlignedAllocAtLeastValidAlignments) {
  constexpr size_t kAlignments[] = {1,  2,  4,  8,
                                    16, 32, 64, alignof(std::max_align_t) * 2};
  constexpr size_t kSizes[] = {0, 1, 3, 5, 7, 15, 31, 63, 127};

  for (size_t align : kAlignments) {
    for (size_t size : kSizes) {
      alloc_result_t res = aligned_alloc_at_least(align, size);
      if (size == 0 && res.ptr == nullptr) {
        // Implementation may return nullptr for size 0.
        continue;
      }
      ASSERT_NE(res.ptr, nullptr) << "align: " << align << ", size: " << size;
      EXPECT_GE(res.size, size) << "align: " << align << ", size: " << size;
      EXPECT_EQ(reinterpret_cast<uintptr_t>(res.ptr) % align, 0)
          << "align: " << align << ", size: " << size;
      if (res.size > 0) {
        std::memset(res.ptr, 0x42, res.size);
      }
      std::free(res.ptr);
    }
  }
}

TEST(AllocAtLeastTest, AlignedAllocAtLeastInvalidAlignments) {
  constexpr size_t kInvalidAlignments[] = {0, 3, 5, 6, 7, 9, 10, 12};
  for (size_t align : kInvalidAlignments) {
    errno = 0;
    alloc_result_t res = aligned_alloc_at_least(align, 64);
    EXPECT_EQ(res.ptr, nullptr) << "align: " << align;
    EXPECT_EQ(res.size, 0) << "align: " << align;
    EXPECT_EQ(errno, EINVAL) << "align: " << align;
  }
}

TEST(AllocAtLeastTest, AlignedAllocAtLeastSizeOverflow) {
  errno = 0;
  alloc_result_t res = aligned_alloc_at_least(8, SIZE_MAX - 1);
  EXPECT_EQ(res.ptr, nullptr);
  EXPECT_EQ(res.size, 0);
  EXPECT_EQ(errno, ENOMEM);
}

TEST(AllocAtLeastTest, AllocAtLeastBasic) {
  alloc_result_t res = alloc_at_least(127);
  ASSERT_NE(res.ptr, nullptr);
  EXPECT_GE(res.size, 127);
  std::memset(res.ptr, 0x42, res.size);
  std::free(res.ptr);

  alloc_result_t zero_res = alloc_at_least(0);
  if (zero_res.ptr != nullptr) {
    std::free(zero_res.ptr);
  }
}

}  // namespace
