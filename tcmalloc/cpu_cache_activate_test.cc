// Copyright 2021 The TCMalloc Authors
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

#include <sched.h>
#include <stddef.h>
#include <stdint.h>

#include <atomic>
#include <thread>  // NOLINT(build/c++11)

#include "benchmark/benchmark.h"
#include "gtest/gtest.h"
#include "absl/random/random.h"
#include "absl/synchronization/notification.h"
#include "absl/time/clock.h"
#include "absl/time/time.h"
#include "tcmalloc/arena.h"
#include "tcmalloc/common.h"
#include "tcmalloc/cpu_cache.h"
#include "tcmalloc/internal/percpu.h"
#include "tcmalloc/internal/sysinfo.h"
#include "tcmalloc/internal_malloc_extension.h"
#include "tcmalloc/static_vars.h"

namespace tcmalloc {
namespace tcmalloc_internal {
namespace {

// This test mutates global state, including triggering the activation of the
// per-CPU caches.  It should not be run along side other tests in the same
// process that may rely on an isolated global instance.
TEST(CpuCacheActivateTest, GlobalInstance) {
  if (!subtle::percpu::IsFast()) {
    return;
  }

  tc_globals.InitIfNecessary();
  auto& cache = tc_globals.cpu_cache();

  absl::Notification done;

  std::thread t([&]() {
    const int num_cpus = NumCPUs();
    absl::BitGen rng;

    while (!done.HasBeenNotified()) {
      const double coin = absl::Uniform(rng, 0., 1.);
      const bool ready = tc_globals.CpuCacheActive();

      if (ready && coin < 0.25) {
        const int cpu = absl::Uniform(rng, 0, num_cpus);
        benchmark::DoNotOptimize(cache.UsedBytes(cpu));
      } else if (ready && coin < 0.5) {
        const int cpu = absl::Uniform(rng, 0, num_cpus);
        benchmark::DoNotOptimize(cache.Capacity(cpu));
      } else if (ready && coin < 0.75) {
        benchmark::DoNotOptimize(cache.TotalUsedBytes());
      } else {
        benchmark::DoNotOptimize(cache.CacheLimit());
      }
    }
  });

  // Trigger initialization of the CpuCache from two concurrent threads,
  // confirming it was not initialized at the start of the test, is initialized
  // afterwards, and executes CpuCache::Activate() at most once.
  EXPECT_FALSE(tc_globals.CpuCacheActive());
  ASSERT_NE(&TCMalloc_Internal_ForceCpuCacheActivation, nullptr);
  Parameters::set_per_cpu_caches(true);

  const size_t before_cpu_cache_arena =
      tc_globals.arena()
          .stats()
          .bytes_allocated_per_type[static_cast<size_t>(ArenaAlloc::kCpuCache)];

  std::atomic<int> activators_ready{0};
  std::atomic<bool> go{false};
  auto activator_fn = [&]() {
    ::operator delete(::operator new(1));
    activators_ready.fetch_add(1, std::memory_order_release);
    while (!go.load(std::memory_order_acquire)) {
    }
    TCMalloc_Internal_ForceCpuCacheActivation();
  };

  std::thread a1(activator_fn);
  std::thread a2(activator_fn);
  while (activators_ready.load(std::memory_order_acquire) < 2) {
    sched_yield();
  }
  {
    PageHeapSpinLockHolder l;
    go.store(true, std::memory_order_release);
    const absl::Time deadline = absl::Now() + absl::Milliseconds(10);
    while (absl::Now() < deadline) {
      sched_yield();
    }
  }
  a1.join();
  a2.join();

  EXPECT_TRUE(tc_globals.CpuCacheActive());

  const size_t after_cpu_cache_arena =
      tc_globals.arena()
          .stats()
          .bytes_allocated_per_type[static_cast<size_t>(ArenaAlloc::kCpuCache)];
  const size_t slab_metadata_bytes =
      tc_globals.cpu_cache().MetadataMemoryUsage().virtual_size;
  ASSERT_GT(after_cpu_cache_arena,
            before_cpu_cache_arena + slab_metadata_bytes);
  const size_t resize_info_arena_bytes =
      after_cpu_cache_arena - before_cpu_cache_arena - slab_metadata_bytes;
  EXPECT_LE(resize_info_arena_bytes,
            52 * kNumClasses * static_cast<size_t>(NumCPUs()) + 1024);

  absl::SleepFor(absl::Milliseconds(100));

  done.Notify();
  t.join();
}

}  // namespace
}  // namespace tcmalloc_internal
}  // namespace tcmalloc
