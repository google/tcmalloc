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

#include <stdint.h>

#include <atomic>
#include <thread>  // NOLINT(build/c++11)
#include <vector>

#include "benchmark/benchmark.h"
#include "gtest/gtest.h"
#include "absl/random/random.h"
#include "absl/synchronization/notification.h"
#include "absl/time/clock.h"
#include "absl/time/time.h"
#include "tcmalloc/cpu_cache.h"
#include "tcmalloc/internal/percpu.h"
#include "tcmalloc/internal/sysinfo.h"
#include "tcmalloc/internal_malloc_extension.h"
#include "tcmalloc/static_vars.h"
#include "tcmalloc/thread_cache.h"

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

  // Trigger initialization of the CpuCache, confirming it was not initialized
  // at the start of the test and is afterwards.  Activate from several threads
  // at once: only one of them may call CpuCache::Activate.
  EXPECT_FALSE(tc_globals.CpuCacheActive());
  ASSERT_NE(&TCMalloc_Internal_ForceCpuCacheActivation, nullptr);
  Parameters::set_per_cpu_caches(true);

  constexpr int kActivators = 4;
  std::atomic<bool> go(false);
  std::vector<std::thread> activators;
  activators.reserve(kActivators);
  for (int i = 0; i < kActivators; ++i) {
    activators.emplace_back([&]() {
      while (!go.load(std::memory_order_acquire)) {
      }
      TCMalloc_Internal_ForceCpuCacheActivation();
      EXPECT_TRUE(tc_globals.CpuCacheActive());
    });
  }
  go.store(true, std::memory_order_release);
  for (auto& activator : activators) {
    activator.join();
  }
  EXPECT_TRUE(tc_globals.CpuCacheActive());

  absl::SleepFor(absl::Milliseconds(100));

  done.Notify();
  t.join();
}

TEST(CpuCacheActivateTest, ThreadCacheConcurrentStatsAndResize) {
  absl::Notification ready;
  std::atomic<bool> stop{false};

  std::thread worker([&]() {
    ThreadCache* tc = ThreadCache::GetCache();
    void* p = tc->Allocate(1);
    ready.Notify();
    while (!stop.load(std::memory_order_relaxed)) {
      tc->Deallocate(p, 1);
      p = tc->Allocate(1);
    }
    tc->Deallocate(p, 1);
  });

  ready.WaitForNotification();
  const size_t orig_size = ThreadCache::overall_thread_cache_size();
  for (int i = 0; i < 500; ++i) {
    uint64_t total_bytes = 0;
    uint64_t class_count[kNumClasses] = {0};
    (void)ThreadCache::GetStats(&total_bytes, class_count);
    ThreadCache::set_overall_thread_cache_size(kMinThreadCacheSize * 2);
    ThreadCache::set_overall_thread_cache_size(orig_size);
  }
  stop.store(true, std::memory_order_relaxed);
  worker.join();
}

}  // namespace
}  // namespace tcmalloc_internal
}  // namespace tcmalloc
