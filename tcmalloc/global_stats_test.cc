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

#include <stddef.h>

#include <new>
#include <thread>  // NOLINT(build/c++11)

#include "gtest/gtest.h"
#include "tcmalloc/internal/parameter_accessors.h"
#include "tcmalloc/internal/percpu.h"
#include "tcmalloc/internal_malloc_extension.h"
#include "tcmalloc/malloc_extension.h"
#include "tcmalloc/testing/testutil.h"

namespace tcmalloc {
namespace {

TEST(GlobalStatsTest, ExtractStatsDoesNotFlushThreadCacheUnderPageheapLock) {
  if (!tcmalloc_internal::subtle::percpu::IsFast()) {
    GTEST_SKIP() << "Per-CPU caches are not supported";
  }
  ASSERT_FALSE(MallocExtension::PerCpuCachesActive());
  ScopedNeverSample never_sample;

  // Static::SlowInitIfNecessary() calls subtle::percpu::IsFast() on the initial
  // thread, so spawn a worker thread whose rseq state starts uninitialized
  // (IsFastNoInit() == false).
  std::thread worker([]() {
    ASSERT_FALSE(tcmalloc_internal::subtle::percpu::IsFastNoInit());

    // Fill the 256 KiB transfer cache (capacity 4, 1 object per span) and
    // leave a 5th object in this thread's ThreadCache so that if
    // MarkThreadIdle() later flushes this ThreadCache, the 5th object overflows
    // the transfer cache to the page heap and acquires pageheap_lock.
    constexpr size_t kSize = 256 * 1024;
    constexpr int kNumObjs = 5;
    void* ptrs[kNumObjs];
    for (int i = 0; i < kNumObjs; ++i) {
      ptrs[i] = ::operator new(kSize);
    }
    for (int i = 0; i < kNumObjs; ++i) {
      ::operator delete(ptrs[i], kSize);
    }
    ASSERT_FALSE(tcmalloc_internal::subtle::percpu::IsFastNoInit());

    // Activate the CPU cache from a separate helper thread so CpuCacheActive()
    // becomes true globally while IsFastNoInit() remains false on this worker.
    TCMalloc_Internal_SetPerCpuCachesEnabledNoBuildRequirement(true);
    std::thread([]() { TCMalloc_Internal_ForceCpuCacheActivation(); }).join();
    ASSERT_TRUE(MallocExtension::PerCpuCachesActive());
    ASSERT_FALSE(tcmalloc_internal::subtle::percpu::IsFastNoInit());

    // GetNumericProperty calls ExtractTCMallocStats, which takes the pageheap
    // lock. We need to ensure we don't self-deadlock.
    EXPECT_TRUE(
        MallocExtension::GetNumericProperty("generic.physical_memory_used")
            .has_value());
  });
  worker.join();
}

}  // namespace
}  // namespace tcmalloc
