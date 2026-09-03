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

#include "gtest/gtest.h"

namespace tcmalloc {
namespace {

TEST(AllocAtLeastTest, Basic) {
  alloc_result_t result = alloc_at_least(127);
  EXPECT_NE(result.ptr, nullptr);
  EXPECT_GE(result.size, 127);
  std::free(result.ptr);
}

TEST(AllocAtLeastTest, SubPointerAlignments) {
  for (size_t align : {size_t{1}, size_t{2}, size_t{4}}) {
    errno = 0;
    alloc_result_t result = aligned_alloc_at_least(align, 64);
    EXPECT_NE(result.ptr, nullptr) << "errno=" << errno;
    EXPECT_GE(result.size, 64);
    if (result.ptr != nullptr) {
      EXPECT_EQ(reinterpret_cast<uintptr_t>(result.ptr) % align, 0);
      std::free(result.ptr);
    }
  }
}

}  // namespace
}  // namespace tcmalloc
