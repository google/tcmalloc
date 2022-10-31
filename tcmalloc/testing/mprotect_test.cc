// Copyright 2022 The TCMalloc Authors
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

#include <limits>
#include <new>

#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "tcmalloc/experiment.h"
#include "tcmalloc/experiment_config.h"
#include "tcmalloc/internal/config.h"
#include "tcmalloc/malloc_extension.h"
#include "tcmalloc/testing/testutil.h"

namespace tcmalloc {
namespace {

TEST(MprotectExperiment, UseAfterFreeDeathTest) {
  ASSERT_TRUE(IsExperimentActive(
      Experiment::TEST_ONLY_TCMALLOC_MPROTECT_RELEASED_MEMORY));

  // Allocate a large buffer to ensure we work directly with the page heap,
  // rather than any intermediate caches.
  constexpr size_t kSize = tcmalloc_internal::kHugePageSize;
  void* ptr = ::operator new(kSize);

  sized_delete(ptr, kSize);

  MallocExtension::ReleaseMemoryToSystem(std::numeric_limits<size_t>::max());

  EXPECT_DEATH(
      {
        // Read after free.
        uint32_t x;
        memcpy(&x, ptr, sizeof(x));
        benchmark::DoNotOptimize(x);
      },
      "");

  EXPECT_DEATH(
      {
        // Write after free.
        uint32_t x = 5;
        memcpy(ptr, &x, sizeof(x));
        benchmark::DoNotOptimize(ptr);
      },
      "");
}

}  // namespace
}  // namespace tcmalloc
