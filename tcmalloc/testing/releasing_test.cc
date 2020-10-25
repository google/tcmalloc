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
//
// This tests the memory accounting when releasing memory to the OS.  As this
// requires careful memory accounting, we avoid allocating at critical times and
// avoid Google Test/background threads.

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <cstdio>
#include <limits>
#include <vector>

#include "absl/random/random.h"
#include "absl/strings/string_view.h"
#include "absl/types/optional.h"
#include "benchmark/benchmark.h"
#include "tcmalloc/common.h"
#include "tcmalloc/internal/logging.h"
#include "tcmalloc/internal/memory_stats.h"
#include "tcmalloc/malloc_extension.h"

namespace {

int64_t GetRSS() {
  tcmalloc::tcmalloc_internal::MemoryStats stats;
  CHECK_CONDITION(tcmalloc::tcmalloc_internal::GetMemoryStats(&stats));
  return stats.rss;
}

int64_t UnmappedBytes() {
  absl::optional<size_t> value = tcmalloc::MallocExtension::GetNumericProperty(
      "tcmalloc.pageheap_unmapped_bytes");
  CHECK_CONDITION(value.has_value());
  return *value;
}

}  // namespace

int main() {
  int ret = mlockall(MCL_CURRENT | MCL_FUTURE);
  if (ret != 0) {
    const bool kSoftFail = true;

    if (kSoftFail) {
      // Determine if we should be able to mlock memory due to our limits.
      struct rlimit lock_limit;
      if (getrlimit(RLIMIT_MEMLOCK, &lock_limit) != 0) {
        tcmalloc::Crash(tcmalloc::kCrash, __FILE__, __LINE__,
                        "getrlimit failed: errno", errno);
      }

      if (lock_limit.rlim_cur != RLIM_INFINITY && errno == ENOMEM) {
        tcmalloc::Log(tcmalloc::kLog, __FILE__, __LINE__,
                      "mlockall failed: errno", errno, " mlock limit ",
                      lock_limit.rlim_cur);
        return 0;
      }
    }
    tcmalloc::Crash(tcmalloc::kCrash, __FILE__, __LINE__,
                    "mlockall failed: errno", errno);
  }

  const int kSmallAllocations = 1000;
  const size_t kSmallSize = 256 * 1024;
  const int kLargeAllocations = 1000;
  const size_t kLargeSize = 4 * 1024 * 1024;

  std::vector<void*> ptrs;
  ptrs.reserve(kSmallAllocations + kLargeAllocations);

  absl::BitGen rng;
  for (int i = 0; i < kSmallAllocations; i++) {
    size_t size = absl::LogUniform<size_t>(rng, 0, kSmallSize);
    void* ptr = ::operator new(size);
    memset(ptr, 0xCD, size);
    ::benchmark::DoNotOptimize(ptr);
    ptrs.push_back(ptr);
  }

  for (int i = 0; i < kLargeAllocations; i++) {
    size_t size = absl::LogUniform<size_t>(rng, kLargeSize / 2, kLargeSize);
    void* ptr = ::operator new(size);
    memset(ptr, 0xCD, size);
    ::benchmark::DoNotOptimize(ptr);
    ptrs.push_back(ptr);
  }

  int64_t before, after, before_unmapped, after_unmapped;
  // Release all of the memory that we can.  Verify that RSS change corresponds
  // to what the release logic did.

  before = GetRSS();
  before_unmapped = UnmappedBytes();

  // Clean up.
  for (void* ptr : ptrs) {
    ::operator delete(ptr);
  }

  // Try to release memory TCMalloc thinks it does not need.
  tcmalloc::MallocExtension::ReleaseMemoryToSystem(0);
  after = GetRSS();
  after_unmapped = UnmappedBytes();

  int64_t unmapped_diff = after_unmapped - before_unmapped;
  int64_t memusage_diff = before - after;
  if (unmapped_diff < 0) {
    tcmalloc::Crash(tcmalloc::kCrash, __FILE__, __LINE__, "Memory was mapped.");
  } else if (unmapped_diff % tcmalloc::kHugePageSize != 0) {
    tcmalloc::Crash(tcmalloc::kCrash, __FILE__, __LINE__,
                    "Non-hugepage size for unmapped memory: ", unmapped_diff);
  }

  // Try to release all unused memory.

  tcmalloc::MallocExtension::ReleaseMemoryToSystem(
      std::numeric_limits<size_t>::max());
  after = GetRSS();
  after_unmapped = UnmappedBytes();

  unmapped_diff = after_unmapped - before_unmapped;
  memusage_diff = before - after;
  const double kTolerance = 5e-4;

  tcmalloc::Log(tcmalloc::kLog, __FILE__, __LINE__, "Unmapped Memory [Before]",
                before_unmapped);
  tcmalloc::Log(tcmalloc::kLog, __FILE__, __LINE__, "Unmapped Memory [After ]",
                after_unmapped);
  tcmalloc::Log(tcmalloc::kLog, __FILE__, __LINE__, "Unmapped Memory [Diff  ]",
                after_unmapped - before_unmapped);
  tcmalloc::Log(tcmalloc::kLog, __FILE__, __LINE__, "Memory Usage [Before]",
                before);
  tcmalloc::Log(tcmalloc::kLog, __FILE__, __LINE__, "Memory Usage [After ]",
                after);
  tcmalloc::Log(tcmalloc::kLog, __FILE__, __LINE__, "Memory Usage [Diff  ]",
                before - after);

  if (unmapped_diff == 0) {
    tcmalloc::Crash(tcmalloc::kCrash, __FILE__, __LINE__,
                    "No memory was unmapped.");
  }

  if (unmapped_diff * (1. + kTolerance) < memusage_diff ||
      unmapped_diff * (1. - kTolerance) > memusage_diff) {
    tcmalloc::Crash(tcmalloc::kCrash, __FILE__, __LINE__,
                    "(after_unmapped - before_unmapped) != (before - after)",
                    after_unmapped - before_unmapped, before - after);
  }

  printf("PASS\n");
  return 0;
}
