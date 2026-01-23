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
#include <cstddef>
#include <cstdlib>
#include <string>

#include "gtest/gtest.h"
#include "tcmalloc/internal/memory_tag.h"
#include "tcmalloc/internal/parameter_accessors.h"
#include "tcmalloc/testing/testutil.h"

namespace tcmalloc {
namespace {

using ::tcmalloc::tcmalloc_internal::GetMemoryTag;
using ::tcmalloc::tcmalloc_internal::MemoryTag;

extern "C" {
void* __alloc_token_0_malloc(size_t) noexcept;
void* __alloc_token_1_malloc(size_t) noexcept;
}

TEST(Partition, AssignedCorrectly) {
  ScopedNeverSample never_sample;
  void* ptr_0 = __alloc_token_0_malloc(8);
  void* ptr_1 = __alloc_token_1_malloc(8);

  EXPECT_EQ(GetMemoryTag(ptr_0), MemoryTag::kNormalP0);
  if (TCMalloc_Internal_GetHeapPartitioning()) {
    ASSERT_NE(MemoryTag::kNormalP0, MemoryTag::kNormalP1);
    EXPECT_EQ(GetMemoryTag(ptr_1), MemoryTag::kNormalP1);
    char* heap_partitioning_env = std::getenv("TCMALLOC_HEAP_PARTITIONING");
    std::string heap_partitioning_env_str =
        heap_partitioning_env == nullptr ? "true" : heap_partitioning_env;
    EXPECT_NE(heap_partitioning_env, "false");
    EXPECT_NE(heap_partitioning_env, "0");
  } else {
    EXPECT_EQ(GetMemoryTag(ptr_1), MemoryTag::kNormal);
  }

  free(ptr_0);
  free(ptr_1);
}

}  // namespace
}  // namespace tcmalloc
