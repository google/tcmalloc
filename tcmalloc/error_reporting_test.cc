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

#include "tcmalloc/error_reporting.h"

#include <sys/mman.h>

#include <cstddef>
#include <new>
#include <optional>

#include "gtest/gtest.h"
#include "tcmalloc/internal/page_size.h"
#include "tcmalloc/internal/sampled_allocation.h"
#include "tcmalloc/static_vars.h"

namespace tcmalloc::tcmalloc_internal {
namespace {

TEST(ErrorReportingTest, MismatchedDeleteCorruptedStackDepth) {
  const size_t kAllocSize = 440;
  const size_t kDeleteSize = 40;
  const size_t page_size = GetPageSize();
  ASSERT_GE(page_size, sizeof(SampledAllocation));

  void* pages = mmap(nullptr, 2 * page_size, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  ASSERT_NE(pages, MAP_FAILED);
  ASSERT_EQ(
      mprotect(static_cast<char*>(pages) + page_size, page_size, PROT_NONE), 0);

  auto* alloc =
      new (static_cast<char*>(pages) + page_size - sizeof(SampledAllocation))
          SampledAllocation();
  alloc->sampled_stack.requested_size = kAllocSize;
  alloc->sampled_stack.allocated_size = kAllocSize;
  alloc->sampled_stack.depth = 1586;

  EXPECT_DEATH(
      {
        ReportMismatchedDelete(tc_globals, nullptr, *alloc, kDeleteSize,
                               kAllocSize, std::nullopt);
      },
      "alloc\\.sampled_stack\\.depth <= kMaxStackDepth \\(1586 <= 64\\) "
      "Corrupted SampledAllocation");

  alloc->~SampledAllocation();
  ASSERT_EQ(munmap(pages, 2 * page_size), 0);
}

}  // namespace
}  // namespace tcmalloc::tcmalloc_internal
