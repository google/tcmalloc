// Copyright 2023 The TCMalloc Authors
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

#include <stdio.h>

#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "gtest/gtest.h"
#include "absl/strings/str_split.h"
#include "absl/time/clock.h"
#include "absl/time/time.h"
#include "tcmalloc/internal/sysinfo.h"
#include "tcmalloc/malloc_extension.h"
#include "tcmalloc/testing/test_allocator_harness.h"
#include "tcmalloc/testing/testutil.h"
#include "tcmalloc/testing/thread_manager.h"

namespace tcmalloc {
namespace {

// Returns the total capacity of the per-CPU caches: drains every cache so that
// its capacity is all unallocated, then sums the used and unallocated bytes
// from the "cpu N: U bytes (M MiB) with A bytes unallocated" lines of
// GetStats().  Without the drain, freelist slots that are allocated but empty
// are counted by neither figure.
size_t CpuCacheBytes() {
  for (int cpu = 0, n = tcmalloc_internal::NumCPUs(); cpu < n; ++cpu) {
    MallocExtension::ReleaseCpuMemory(cpu);
  }
  size_t sum = 0;
  const std::vector<std::string> lines =
      absl::StrSplit(MallocExtension::GetStats(), '\n');
  for (const std::string& line : lines) {
    int cpu;
    size_t used;
    size_t unallocated;
    if (sscanf(line.c_str(),
               "cpu %d: %zu bytes (%*f MiB) with %zu bytes unallocated", &cpu,
               &used, &unallocated) == 3) {
      sum += used + unallocated;
    }
  }
  return sum;
}

TEST(CpuCacheSizeTest, Resizing) {
  if (!MallocExtension::PerCpuCachesActive()) {
    GTEST_SKIP() << "Test requires working per-CPU caches";
  }

  const int cpus = tcmalloc_internal::NumCPUs();
  const int original_size = MallocExtension::GetMaxPerCpuCacheSize();

  // Run shuffles, size class resizes, slab resizes, and drains at a high rate
  // so that they overlap with the resizes below.
  ScopedBackgroundProcessSleepInterval sleep_interval(absl::Milliseconds(1));

  constexpr int kThreads = 10;
  AllocatorHarness harness(kThreads);

  // Changes the limit while the harness threads allocate and background
  // actions run, then returns the per-CPU cache capacity once both stop.
  // GetStats() reads slab headers without synchronizing against slab resizes,
  // so background actions must be idle while measuring.
  auto resize_under_load = [&](std::optional<int> new_size) {
    std::thread background([] { MallocExtension::ProcessBackgroundActions(); });
    ThreadManager mgr;
    mgr.Start(kThreads, [&](int thread_id) { harness.Run(thread_id); });
    absl::SleepFor(absl::Milliseconds(50));
    if (new_size.has_value()) {
      MallocExtension::SetMaxPerCpuCacheSize(*new_size);
    }
    absl::SleepFor(absl::Milliseconds(100));
    mgr.Stop();

    ScopedBackgroundProcessActionsEnabled background_process_enabled(
        /*value=*/false);
    background.join();
    return CpuCacheBytes();
  };

  const size_t original_bytes = resize_under_load(std::nullopt);
  EXPECT_GE(original_bytes, 0.9 * cpus * original_size);
  EXPECT_LE(original_bytes, 1.1 * cpus * original_size);

  constexpr int kCacheMultiplier = 3;
  const size_t larger_bytes =
      resize_under_load(kCacheMultiplier * original_size);
  EXPECT_GE(larger_bytes, 0.9 * kCacheMultiplier * cpus * original_size);
  EXPECT_LE(larger_bytes, 1.1 * kCacheMultiplier * cpus * original_size);

  constexpr int kCacheDivisor = 2;
  const size_t smaller_bytes = resize_under_load(original_size / kCacheDivisor);
  EXPECT_GE(smaller_bytes, 0.9 * cpus * original_size / kCacheDivisor);
  EXPECT_LE(smaller_bytes, 1.1 * cpus * original_size / kCacheDivisor);

  EXPECT_EQ(resize_under_load(-1), 0);
  EXPECT_EQ(MallocExtension::GetMaxPerCpuCacheSize(), 0);

  const size_t restored_bytes = resize_under_load(original_size);
  EXPECT_EQ(MallocExtension::GetMaxPerCpuCacheSize(), original_size);
  EXPECT_GE(restored_bytes, 0.9 * cpus * original_size);
  EXPECT_LE(restored_bytes, 1.1 * cpus * original_size);
}

}  // namespace
}  // namespace tcmalloc
