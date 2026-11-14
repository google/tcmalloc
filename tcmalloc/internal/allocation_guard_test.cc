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

#include "tcmalloc/internal/allocation_guard.h"

#include <new>

#include "gtest/gtest.h"
#include "absl/base/const_init.h"
#include "absl/base/internal/spinlock.h"
#include "tcmalloc/internal/scoped_allow_allocation.h"

namespace tcmalloc::tcmalloc_internal {
namespace {

TEST(AllocationGuard, Noncooperative) {
  absl::base_internal::SpinLock lock(absl::base_internal::SCHEDULE_KERNEL_ONLY);
  AllocationGuardSpinLockHolder h(lock);
}

#if !defined(NDEBUG) && !defined(FUZZING_BUILD_MODE_UNSAFE_FOR_PRODUCTION)
TEST(AllocationGuard, ReentrancyCount) {
  EXPECT_EQ(tcmalloc_reentrancy_count, 0);
  int under_lock = -1;
  int under_nested = -1;
  int after_nested = -1;
  int under_allow = -1;
  int after_allow = -1;
  {
    absl::base_internal::SpinLock lock(
        absl::base_internal::SCHEDULE_KERNEL_ONLY);
    AllocationGuardSpinLockHolder h(lock);
    under_lock = tcmalloc_reentrancy_count;
    {
      AllocationGuard nested;
      under_nested = tcmalloc_reentrancy_count;
    }
    after_nested = tcmalloc_reentrancy_count;
    {
      ScopedAllocationAllow allow;
      under_allow = tcmalloc_reentrancy_count;
      ::operator delete(::operator new(1));
    }
    after_allow = tcmalloc_reentrancy_count;
  }
  EXPECT_EQ(under_lock, 1);
  EXPECT_EQ(under_nested, 2);
  EXPECT_EQ(after_nested, 1);
  EXPECT_EQ(under_allow, 0);
  EXPECT_EQ(after_allow, 1);
  EXPECT_EQ(tcmalloc_reentrancy_count, 0);
}
#endif

TEST(AllocationGuard, CooperativeDeathTest) {
  absl::base_internal::SpinLock lock;

  EXPECT_DEBUG_DEATH(
      { AllocationGuardSpinLockHolder h(lock); }, "SIGABRT received");
}

}  // namespace
}  // namespace tcmalloc::tcmalloc_internal
