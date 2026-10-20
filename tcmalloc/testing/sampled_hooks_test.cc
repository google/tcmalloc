// Copyright 2025 The TCMalloc Authors
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
#include <stdint.h>

#include <algorithm>
#include <limits>
#include <new>
#include <optional>
#include <string>
#include <thread>

#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "absl/base/attributes.h"
#include "absl/base/const_init.h"
#include "absl/base/internal/spinlock.h"
#include "absl/base/thread_annotations.h"
#include "absl/time/clock.h"
#include "absl/time/time.h"
#include "absl/types/span.h"
#include "tcmalloc/malloc_extension.h"
#include "tcmalloc/malloc_hook.h"
#include "tcmalloc/testing/testutil.h"

namespace tcmalloc {
namespace {

// If the gunit main function, the TEST_F implementation or any other framework
// starts a thread that allocates/deallocates memory, we don't want it to
// interfere with the tests, so we enable the hooks only in the test thread.
thread_local volatile bool hooks_enabled = false;

size_t num_new_hook_calls;
size_t num_delete_hook_calls;
size_t last_alloc_size;
size_t min_alloc_size;
size_t max_alloc_size;
double last_weight;
double min_weight;
double max_weight;
double unsampled_total_allocated_size;
size_t last_requested_size;
std::optional<std::align_val_t> last_requested_alignment;
const void* last_ptr;
absl::Time last_alloc_time;
uint8_t last_access_hint;
MallocHook::Access last_access_allocated;

void TestSampledNewHook(const MallocHook::SampledAlloc& sampled_alloc) {
  if (!hooks_enabled) {
    return;
  }
  ++num_new_hook_calls;
  last_alloc_size = sampled_alloc.allocated_size;
  min_alloc_size = std::min(min_alloc_size, last_alloc_size);
  max_alloc_size = std::max(max_alloc_size, last_alloc_size);
  last_weight = sampled_alloc.weight;
  // In case last_weight is NaN, which is not supposed to happen, update
  // min_weight and max_weight and make the test fail.
  if (!(last_weight >= min_weight)) {
    min_weight = last_weight;
  }
  if (!(last_weight <= max_weight)) {
    max_weight = last_weight;
  }
  unsampled_total_allocated_size += last_alloc_size * last_weight;
  last_requested_alignment = sampled_alloc.requested_alignment;
  last_requested_size = sampled_alloc.requested_size;
  last_ptr = sampled_alloc.ptr;
  last_alloc_time = sampled_alloc.allocation_time;
  last_access_hint = sampled_alloc.access_hint;
  last_access_allocated = sampled_alloc.access_allocated;
}

void TestSampledDeleteHook(
    const
    MallocHook::SampledAlloc& sampled_alloc) {
  if (hooks_enabled) {
    ++num_delete_hook_calls;
  }
}

class SampledHooksTest : public ::testing::Test {
 protected:

  void SetUp() override {
    hooks_enabled = false;
    num_new_hook_calls = 0;
    num_delete_hook_calls = 0;
    last_alloc_size = std::numeric_limits<size_t>::max();
    min_alloc_size = std::numeric_limits<size_t>::max();
    max_alloc_size = 0;
    last_weight = std::numeric_limits<double>::infinity();
    min_weight = std::numeric_limits<double>::infinity();
    max_weight = -std::numeric_limits<double>::infinity();
    unsampled_total_allocated_size = 0;
    last_access_hint = 0;
    last_access_allocated = MallocHook::Access::Hot;
    ASSERT_TRUE(MallocHook::AddSampledNewHook(&TestSampledNewHook));
    ASSERT_TRUE(MallocHook::AddSampledDeleteHook(&TestSampledDeleteHook));
  }
  void TearDown() override {
    ASSERT_TRUE(MallocHook::RemoveSampledNewHook(&TestSampledNewHook));
    ASSERT_TRUE(MallocHook::RemoveSampledDeleteHook(&TestSampledDeleteHook));
  }
  void* Allocate(size_t bytes) {
    hooks_enabled = true;
    void* p = ::operator new(bytes);
    hooks_enabled = false;
    return p;
  }
  void* AllocateAligned(size_t bytes, size_t alignment) {
    hooks_enabled = true;
    void* p = ::operator new(bytes, std::align_val_t(alignment));
    hooks_enabled = false;
    return p;
  }
  void* AllocateWithHint(size_t bytes, tcmalloc::hot_cold_t hint) {
    hooks_enabled = true;
    void* p = ::operator new(bytes, hint);
    hooks_enabled = false;
    return p;
  }
  void Deallocate(void* p, size_t bytes) {
    hooks_enabled = true;
    sized_delete(p, bytes);
    hooks_enabled = false;
  }
  void DeallocateAligned(void* p, size_t bytes, size_t alignment) {
    hooks_enabled = true;
    sized_aligned_delete(p, bytes, std::align_val_t(alignment));
    hooks_enabled = false;
  }
};

TEST_F(SampledHooksTest, AlwaysSampled) {
  ScopedProfileSamplingInterval i(1);

  absl::Time start = absl::Now();
  void* p = Allocate(34);
  EXPECT_EQ(1, num_new_hook_calls);
  EXPECT_GT(last_alloc_time, start - absl::Seconds(1));
  EXPECT_LT(last_alloc_time, start + absl::Seconds(1));
  EXPECT_EQ(0, num_delete_hook_calls);
  EXPECT_EQ(MallocExtension::GetEstimatedAllocatedSize(34), last_alloc_size);
  EXPECT_EQ(1.0, last_weight);
  EXPECT_EQ(p, last_ptr);

  Deallocate(p, 34);
  EXPECT_EQ(1, num_new_hook_calls);
  EXPECT_EQ(1, num_delete_hook_calls);
  EXPECT_EQ(1.0, last_weight);

  start = absl::Now();
  p = Allocate(57);
  EXPECT_EQ(2, num_new_hook_calls);
  EXPECT_GT(last_alloc_time, start - absl::Seconds(1));
  EXPECT_LT(last_alloc_time, start + absl::Seconds(1));
  EXPECT_EQ(1, num_delete_hook_calls);
  EXPECT_EQ(MallocExtension::GetEstimatedAllocatedSize(57), last_alloc_size);
  EXPECT_EQ(1.0, last_weight);
  EXPECT_EQ(p, last_ptr);

  Deallocate(p, 57);
  EXPECT_EQ(2, num_new_hook_calls);
  EXPECT_EQ(2, num_delete_hook_calls);
  EXPECT_EQ(1.0, last_weight);
}

TEST_F(SampledHooksTest, SometimesSampled) {
  ScopedProfileSamplingInterval s(64 * 1024);
  for (int log_size = 10; log_size < 30; log_size++) {
    const size_t kRequestedSizePerAllocation = 1 << log_size;
    const size_t kTrueSizePerAllocation =
        nallocx(kRequestedSizePerAllocation, 0);
    SCOPED_TRACE(kRequestedSizePerAllocation);

    unsampled_total_allocated_size = 0;
    num_new_hook_calls = 0;
    num_delete_hook_calls = 0;
    min_alloc_size = std::numeric_limits<size_t>::max();
    max_alloc_size = 0;
    min_weight = std::numeric_limits<double>::infinity();
    max_weight = -std::numeric_limits<double>::infinity();

    const size_t kNumAllocations = std::max<size_t>(
        64 * 1024, MallocExtension::GetProfileSamplingInterval() * 8192 /
                       kRequestedSizePerAllocation);
    const size_t kTotalAllocatedSize = kTrueSizePerAllocation * kNumAllocations;
    for (size_t i = 0; i < kNumAllocations; ++i) {
      Deallocate(Allocate(kRequestedSizePerAllocation),
                 kRequestedSizePerAllocation);
    }
    EXPECT_LE(unsampled_total_allocated_size, kTotalAllocatedSize * 1.063);
    EXPECT_GE(unsampled_total_allocated_size,
              kTotalAllocatedSize * (1 - 0.063));
    EXPECT_EQ(num_new_hook_calls, num_delete_hook_calls);
    EXPECT_EQ(kTrueSizePerAllocation, min_alloc_size);
    EXPECT_EQ(kTrueSizePerAllocation, max_alloc_size);
  }
}

TEST_F(SampledHooksTest, NeverSampled) {
  ScopedProfileSamplingInterval i(0);

  Deallocate(Allocate(256 * 1024 * 1024), 256 * 1024 * 1024);
  EXPECT_EQ(0, num_new_hook_calls);
  EXPECT_EQ(0, num_delete_hook_calls);

  ScopedProfileSamplingInterval j(-1);
  Deallocate(Allocate(256 * 1024 * 1024), 256 * 1024 * 1024);
  EXPECT_EQ(0, num_new_hook_calls);
  EXPECT_EQ(0, num_delete_hook_calls);
}

TEST_F(SampledHooksTest, RequestedSizeAndAlignment) {
  ScopedProfileSamplingInterval i(1);

  void* p = Allocate(34);
  EXPECT_EQ(34, last_requested_size);
  EXPECT_EQ(std::nullopt, last_requested_alignment);
  Deallocate(p, 34);

  p = AllocateAligned(24, 16);
  EXPECT_EQ(24, last_requested_size);
  EXPECT_EQ(std::align_val_t{16}, last_requested_alignment);
  DeallocateAligned(p, 24, 16);
}

TEST_F(SampledHooksTest, AccessHintAndAllocated) {
  ScopedProfileSamplingInterval i(1);

  // 1. Cold Hint
  void* p = AllocateWithHint(64, tcmalloc::hot_cold_t{0});
  EXPECT_EQ(0, last_access_hint);
  // The allocator may place cold hints into the Hot partition under some
  // circumstances (e.g. heap partitioning disabled or unsupported).
  EXPECT_THAT(last_access_allocated, testing::AnyOf(MallocHook::Access::Hot,
                                                    MallocHook::Access::Cold));
  Deallocate(p, 64);

  // 2. Hot Hint
  p = AllocateWithHint(64, tcmalloc::hot_cold_t{255});
  EXPECT_EQ(255, last_access_hint);
  EXPECT_EQ(last_access_allocated, MallocHook::Access::Hot);
  Deallocate(p, 64);
}

TEST_F(SampledHooksTest, AlwaysSampledAndGuarded) {
  ScopedProfileSamplingInterval i(1);
  ScopedGuardedSamplingInterval g(0);  // TODO(b/201336703): Guard every sample

  // Allocate some memory until allocs_until_guarded_sample_ reaches 0.
  for (int i = 0; i < 100; i++) {
    void* p = ::operator new[](32);
    ::operator delete[](p);
  }

  void* p = Allocate(34);
  EXPECT_EQ(1, num_new_hook_calls);
  EXPECT_EQ(0, num_delete_hook_calls);
  EXPECT_EQ(MallocExtension::GetEstimatedAllocatedSize(34), last_alloc_size);
  EXPECT_EQ(1.0, last_weight);
  EXPECT_EQ(p, last_ptr);

  Deallocate(p, 34);
  EXPECT_EQ(1, num_new_hook_calls);
  EXPECT_EQ(1, num_delete_hook_calls);
  EXPECT_EQ(1.0, last_weight);
}

ABSL_ATTRIBUTE_NOINLINE void* AllocateFromDistinctStackFrame(size_t depth) {
  if (depth > 0) {
    void* p = AllocateFromDistinctStackFrame(depth - 1);
    benchmark::DoNotOptimize(p);
    return p;
  }
  void* p = ::operator new(64);
  benchmark::DoNotOptimize(p);
  return p;
}

ABSL_CONST_INIT absl::base_internal::SpinLock delete_hook_mu;
bool in_delete_hook ABSL_GUARDED_BY(delete_hook_mu) = false;
bool reuse_done ABSL_GUARDED_BY(delete_hook_mu) = false;
const void* stack_before[64] ABSL_GUARDED_BY(delete_hook_mu);
size_t stack_before_depth ABSL_GUARDED_BY(delete_hook_mu) = 0;
const void* stack_after[64] ABSL_GUARDED_BY(delete_hook_mu);
size_t stack_after_depth ABSL_GUARDED_BY(delete_hook_mu) = 0;
thread_local bool capture_next_delete = false;

void ConcurrentSampledDeleteHook(
    const
    MallocHook::SampledAlloc& sampled_alloc) {
  if (!capture_next_delete) {
    return;
  }
  capture_next_delete = false;
  {
    absl::base_internal::SpinLockHolder lock(delete_hook_mu);
    stack_before_depth = sampled_alloc.stack.size();
    std::copy(sampled_alloc.stack.begin(), sampled_alloc.stack.end(),
              stack_before);
    in_delete_hook = true;
  }
  while (true) {
    {
      absl::base_internal::SpinLockHolder lock(delete_hook_mu);
      if (reuse_done) {
        break;
      }
    }
    std::this_thread::yield();
  }
  {
    absl::base_internal::SpinLockHolder lock(delete_hook_mu);
    stack_after_depth = sampled_alloc.stack.size();
    std::copy(sampled_alloc.stack.begin(), sampled_alloc.stack.end(),
              stack_after);
  }
}

TEST_F(SampledHooksTest, DeleteHookStackNotCorruptedBySampleReuse) {
  ScopedProfileSamplingInterval i(1);
  {
    absl::base_internal::SpinLockHolder lock(delete_hook_mu);
    in_delete_hook = false;
    reuse_done = false;
    stack_before_depth = 0;
    stack_after_depth = 0;
  }

  ASSERT_TRUE(MallocHook::AddSampledDeleteHook(&ConcurrentSampledDeleteHook));
  std::thread worker([] {
    while (true) {
      {
        absl::base_internal::SpinLockHolder lock(delete_hook_mu);
        if (in_delete_hook) {
          break;
        }
      }
      std::this_thread::yield();
    }
    // Trigger a sampled allocation from a deeper stack frame on a separate
    // thread while sampled_alloc.stack is live in the delete hook on the main
    // thread. Because Unregister() has already placed the SampledAllocation on
    // graveyard_.dead, Register() pops and reuses the same SampledAllocation
    // node. sampled_alloc.stack must not alias the recycled node's
    // sampled_stack.stack buffer.
    void* p2 = AllocateFromDistinctStackFrame(8);
    ::operator delete(p2);
    {
      absl::base_internal::SpinLockHolder lock(delete_hook_mu);
      reuse_done = true;
    }
  });

  void* p1 = ::operator new(34);
  capture_next_delete = true;
  sized_delete(p1, 34);
  capture_next_delete = false;
  worker.join();
  ASSERT_TRUE(
      MallocHook::RemoveSampledDeleteHook(&ConcurrentSampledDeleteHook));

  absl::base_internal::SpinLockHolder lock(delete_hook_mu);
  ASSERT_GT(stack_before_depth, 0u);
  ASSERT_EQ(stack_before_depth, stack_after_depth);
  EXPECT_EQ(absl::MakeConstSpan(stack_before, stack_before_depth),
            absl::MakeConstSpan(stack_after, stack_after_depth));
}

}  // namespace
}  // namespace tcmalloc
