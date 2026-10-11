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
//
// Exercises the first allocation in the process, which initializes TCMalloc.
// It is made from a priority-101 constructor: after libc is initialized
// (unlike .preinit_array, which is too early for TCMalloc's initialization),
// but before default-priority constructors such as TCMalloc's own.

#include <stddef.h>

#include <new>
#include <optional>

#include "gtest/gtest.h"
#include "absl/base/attributes.h"
#include "tcmalloc/internal/config.h"
#include "tcmalloc/malloc_extension.h"

namespace tcmalloc::tcmalloc_internal {
namespace {

// Not the smallest size class, so a wrong size class is visible in
// GetAllocatedSize.
constexpr size_t kSize = 1000;

ABSL_CONST_INIT std::optional<size_t> first_allocation_size;

__attribute__((constructor(101))) void FirstAllocation() {
  void* ptr = ::operator new(kSize);
  first_allocation_size = MallocExtension::GetAllocatedSize(ptr);
  ::operator delete(ptr, kSize);
}

TEST(FirstAllocationTest, UsesInitializedSizeClass) {
  if (kSanitizerPresent) {
    GTEST_SKIP() << "TCMalloc is not the allocator";
  }
  ASSERT_TRUE(first_allocation_size.has_value());
  EXPECT_EQ(*first_allocation_size,
            MallocExtension::GetEstimatedAllocatedSize(kSize));
}

}  // namespace
}  // namespace tcmalloc::tcmalloc_internal
