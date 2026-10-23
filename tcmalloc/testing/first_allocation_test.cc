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
// Exercises the first allocation in the process, which initializes TCMalloc,
// while a second thread races to allocate as soon as initialization is
// published.  The allocations are made from a priority-101 constructor: after
// libc is initialized (unlike .preinit_array, which is too early for TCMalloc's
// initialization), but before default-priority constructors such as TCMalloc's
// own.

#include <pthread.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#include <new>
#include <optional>

#include "gtest/gtest.h"
#include "absl/base/attributes.h"
#include "tcmalloc/common.h"
#include "tcmalloc/internal/config.h"
#include "tcmalloc/internal/environment.h"
#include "tcmalloc/internal/parameter_accessors.h"
#include "tcmalloc/malloc_extension.h"
#include "tcmalloc/parameters.h"
#include "tcmalloc/static_vars.h"
#include "tcmalloc/testing/rseq_util.h"

namespace tcmalloc::tcmalloc_internal {
namespace {

#ifdef FIRST_ALLOCATION_LARGE
constexpr size_t kSize = 2 * kMaxSize;
#else
// Not the smallest size class, so a wrong size class is visible in the
// returned size.
constexpr size_t kSize = 1000;
#endif

ABSL_CONST_INIT std::optional<size_t> first_allocation_size;
ABSL_CONST_INIT bool active_after_first_allocation = false;
ABSL_CONST_INIT bool active_when_second_thread_saw_inited = false;

void* SecondThread(void*) {
  // Allocate as soon as initialization is published.
  while (!tc_globals.IsInited()) {
  }
  active_when_second_thread_saw_inited = tc_globals.CpuCacheActive();
  sized_ptr_t res = __size_returning_new(kSize);
  ::operator delete(res.p, res.n);
  return nullptr;
}

__attribute__((constructor(101))) void FirstAllocations() {
  if (kSanitizerPresent) {
    // TCMalloc is not the allocator, so the second thread would never see it
    // initialized.
    return;
  }

  // Keep the first allocations unsampled: a guarded sample returns the
  // requested size rather than the size class.
  const int64_t sampling_interval = Parameters::profile_sampling_interval();
  TCMalloc_Internal_SetProfileSamplingInterval(0);

  pthread_t second_thread;
  if (pthread_create(&second_thread, nullptr, SecondThread, nullptr) != 0) {
    abort();
  }
  sized_ptr_t res = __size_returning_new(kSize);
  active_after_first_allocation = tc_globals.CpuCacheActive();
  first_allocation_size = res.n;
  ::operator delete(res.p, res.n);
  pthread_join(second_thread, nullptr);

  TCMalloc_Internal_SetProfileSamplingInterval(sampling_interval);
}

// Per-CPU caches are expected on the first allocation unless the build is
// per-thread, rseq is unavailable, or TCMALLOC_DEFER_PER_CPU_CACHES=1 defers
// activation.
bool ExpectActiveOnFirstAllocation() {
#if defined(TCMALLOC_DEPRECATED_PERTHREAD)
  return false;
#else
  const char* defer = thread_safe_getenv("TCMALLOC_DEFER_PER_CPU_CACHES");
  const bool deferred = defer != nullptr && defer[0] == '1' && defer[1] == '\0';
  return testing::ExpectWorkingRseq() && !deferred;
#endif
}

TEST(FirstAllocationTest, UsesInitializedSizeClass) {
  if (kSanitizerPresent) {
    GTEST_SKIP() << "TCMalloc is not the allocator";
  }
  ASSERT_TRUE(first_allocation_size.has_value());
  EXPECT_EQ(*first_allocation_size,
            MallocExtension::GetEstimatedAllocatedSize(kSize));
}

TEST(FirstAllocationTest, ActivatesPerCpuCaches) {
  if (kSanitizerPresent) {
    GTEST_SKIP() << "TCMalloc is not the allocator";
  }
  const bool expected = ExpectActiveOnFirstAllocation();
  EXPECT_EQ(active_after_first_allocation, expected);
  // A thread that sees TCMalloc initialized must not start on a per-thread
  // cache while the first allocation is still activating per-CPU caches.
  EXPECT_EQ(active_when_second_thread_saw_inited, expected);
}

}  // namespace
}  // namespace tcmalloc::tcmalloc_internal
