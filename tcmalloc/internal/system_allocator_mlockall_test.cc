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

#include <errno.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <unistd.h>

#include <optional>

#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "absl/time/clock.h"
#include "absl/time/time.h"
#include "tcmalloc/internal/memory_tag.h"
#include "tcmalloc/internal/numa.h"
#include "tcmalloc/internal/pageflags.h"
#include "tcmalloc/internal/system_allocator.h"

namespace tcmalloc {
namespace tcmalloc_internal {
namespace {

using ::testing::Eq;
using ::testing::Field;
using ::testing::Optional;

struct MlockallParams {
  const char* mlockall_unlock_future;
  bool expect_locked;
};

class SystemAllocatorMlockallTest
    : public ::testing::TestWithParam<MlockallParams> {};

TEST_P(SystemAllocatorMlockallTest, Mlockall) {
#if ABSL_HAVE_ADDRESS_SANITIZER || ABSL_HAVE_MEMORY_SANITIZER || \
    ABSL_HAVE_THREAD_SANITIZER
  GTEST_SKIP() << "Skipped under sanitizers.";
#endif

  GTEST_SKIP() << "pageflags not commonly available";

  if (GetParam().mlockall_unlock_future) {
    ASSERT_EQ(setenv("TCMALLOC_MLOCKALL_UNLOCK_FUTURE",
                     GetParam().mlockall_unlock_future, 1),
              0);
  } else {
    ASSERT_EQ(unsetenv("TCMALLOC_MLOCKALL_UNLOCK_FUTURE"), 0);
  }

  if (mlockall(MCL_CURRENT | MCL_FUTURE) != 0) {
    // If we can't mlockall, we can't test. This is likely due to permissions.
    GTEST_SKIP() << "Failed to mlockall, errno=" << errno;
  }
  // Any allocations from here on should be mlocked by default by the kernel.
  // However, TCMalloc should munlock memory returned by SystemAllocator if
  // TCMALLOC_MLOCKALL_UNLOCK_FUTURE=1.

  NumaTopology<2> topology;
  SystemAllocator<NumaTopology<2>, 1> allocator(topology, 4 << 20);

  PageFlags page_flags;

  AddressRange result = allocator.Allocate(1024, 1, MemoryTag::kNormal);
  ASSERT_NE(result.ptr, nullptr);
  ASSERT_GE(result.bytes, 1024);

  // Touch the memory to ensure it is faulted in.
  for (size_t i = 0; i < result.bytes; i += getpagesize()) {
    static_cast<volatile char*>(result.ptr)[i] = 0;
  }

  const size_t expected_locked = GetParam().expect_locked ? result.bytes : 0;

  std::optional<PageStats> stats;
  if (GetParam().expect_locked) {
    // Wait until the kernel has had time to propagate flags.
    absl::Time start = absl::Now();
    do {
      stats = page_flags.Get(result.ptr, result.bytes);
      ASSERT_TRUE(stats.has_value());
      if (stats->bytes_locked == expected_locked) {
        break;
      }
      absl::SleepFor(absl::Seconds(1));
    } while (absl::Now() - start < absl::Seconds(60));
  } else {
    stats = page_flags.Get(result.ptr, result.bytes);
  }

  EXPECT_THAT(stats,
              Optional(Field(&PageStats::bytes_locked, Eq(expected_locked))));

  ASSERT_TRUE(allocator.Release(result.ptr, result.bytes).success);

  ASSERT_EQ(munlockall(), 0);
  ASSERT_EQ(unsetenv("TCMALLOC_MLOCKALL_UNLOCK_FUTURE"), 0);
}

INSTANTIATE_TEST_SUITE_P(SystemAllocatorMlockallTest,
                         SystemAllocatorMlockallTest,
                         ::testing::Values(MlockallParams{"0", true},
                                           MlockallParams{"1", false},
                                           MlockallParams{nullptr, true}));

}  // namespace
}  // namespace tcmalloc_internal
}  // namespace tcmalloc
