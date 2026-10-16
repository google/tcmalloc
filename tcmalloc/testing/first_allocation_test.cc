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
#include <stdlib.h>

#include <new>
#include <optional>

#include "gtest/gtest.h"
#include "absl/base/attributes.h"
#include "tcmalloc/internal/config.h"
#include "tcmalloc/malloc_extension.h"
#include "tcmalloc/static_vars.h"

namespace tcmalloc::tcmalloc_internal {
namespace {

// Not the smallest size class, so a wrong size class is visible in
// GetAllocatedSize.
constexpr size_t kSize = 1000;

ABSL_CONST_INIT std::optional<size_t> first_allocation_size;
ABSL_CONST_INIT bool active_after_first_allocation = false;
ABSL_CONST_INIT bool active_when_second_thread_saw_inited = false;

void* SecondThread(void*) {
  // Allocate as soon as initialization is published.
  while (!tc_globals.IsInited()) {
  }
  active_when_second_thread_saw_inited = tc_globals.CpuCacheActive();
  ::operator delete(::operator new(kSize), kSize);
  return nullptr;
}

__attribute__((constructor(101))) void FirstAllocations() {
  if (kSanitizerPresent) {
    // TCMalloc is not the allocator, so the second thread would never see it
    // initialized.
    return;
  }

  pthread_t second_thread;
  if (pthread_create(&second_thread, nullptr, SecondThread, nullptr) != 0) {
    abort();
  }
  void* ptr = ::operator new(kSize);
  active_after_first_allocation = tc_globals.CpuCacheActive();
  first_allocation_size = MallocExtension::GetAllocatedSize(ptr);
  ::operator delete(ptr, kSize);
  pthread_join(second_thread, nullptr);
}

TEST(FirstAllocationTest, UsesInitializedSizeClass) {
  if (kSanitizerPresent) {
    GTEST_SKIP() << "TCMalloc is not the allocator";
  }
  ASSERT_TRUE(first_allocation_size.has_value());
  EXPECT_EQ(*first_allocation_size,
            MallocExtension::GetEstimatedAllocatedSize(kSize));
}

TEST(FirstAllocationTest, InitializedImpliesActivated) {
  if (kSanitizerPresent) {
    GTEST_SKIP() << "TCMalloc is not the allocator";
  }
  // A thread that sees TCMalloc initialized must not start on a per-thread
  // cache while the first allocation is still activating per-CPU caches.
  EXPECT_EQ(active_when_second_thread_saw_inited,
            active_after_first_allocation);
}

}  // namespace
}  // namespace tcmalloc::tcmalloc_internal
