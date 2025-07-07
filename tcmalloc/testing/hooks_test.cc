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

#include <fcntl.h>
#include <malloc.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include <new>
#include <optional>
#include <string>
#include <vector>

#include "benchmark/benchmark.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "absl/debugging/leak_check.h"
#include "tcmalloc/internal/allocation_guard.h"
#include "tcmalloc/internal/logging.h"
#include "tcmalloc/malloc_extension.h"
#include "tcmalloc/malloc_hook.h"
#include "tcmalloc/malloc_hook_invoke.h"
#include "tcmalloc/testing/malloc_hook_recorder.h"
#include "tcmalloc/testing/testutil.h"

namespace tcmalloc {
namespace {

using tcmalloc_testing::MallocHookRecorder;
using testing::Contains;
using testing::ElementsAre;
using testing::HasSubstr;
using testing::Lt;
using testing::SizeIs;

class TCMallocHookTest : public testing::Test {
 public:
  using Type = MallocHookRecorder::Type;

  MallocHookRecorder::CallEntry NewCall(
      const void* ptr, size_t requested_size, size_t allocated_size,
      HookMemoryMutable is_mutable = HookMemoryMutable::kMutable) {
    return {
        MallocHookRecorder::kNew, ptr,        requested_size,
        allocated_size,           is_mutable, MallocHookRecorder::kTCMalloc};
  }
  MallocHookRecorder::CallEntry DeleteCall(
      const void* ptr, std::optional<size_t> deallocated_size,
      size_t allocated_size,
      HookMemoryMutable is_mutable = HookMemoryMutable::kMutable) {
    return {MallocHookRecorder::kDelete,
            ptr,
            deallocated_size,
            allocated_size,
            is_mutable,
            MallocHookRecorder::kTCMalloc};
  }

  std::vector<MallocHookRecorder::CallEntry> Log(bool include_mmap) {
    return recorder_.ConsumeLog(include_mmap);
  }
  void RestartLog() { recorder_.Restart(); }

  size_t GetAllocatedSize(const void* ptr) {
    return MallocExtension::GetAllocatedSize(ptr).value();
  }

 private:
  MallocHookRecorder recorder_;
};

TEST_F(TCMallocHookTest, MallocInvokesHook) {
  void* p1 = malloc(10);
  EXPECT_THAT(Log(false), ElementsAre(NewCall(p1, 10, GetAllocatedSize(p1))));
  free(p1);
}

TEST_F(TCMallocHookTest, FreeInvokesHook) {
  void* p1 = malloc(15);
  size_t allocated_size = GetAllocatedSize(p1);
  // Copy pointers to suppress use-after-free warnings.
  void* copy_p1 = p1;
  benchmark::DoNotOptimize(copy_p1);
  free(p1);
  EXPECT_THAT(Log(false),
              ElementsAre(NewCall(copy_p1, 15, allocated_size),
                          DeleteCall(copy_p1, std::nullopt, allocated_size)));
}

TEST_F(TCMallocHookTest, FreeSizedInvokesHook) {
  void* p1 = malloc(15);
  size_t allocated_size = GetAllocatedSize(p1);
  // Copy pointers to suppress use-after-free warnings.
  void* copy_p1 = p1;
  benchmark::DoNotOptimize(copy_p1);
  free_sized(p1, 15);
  EXPECT_THAT(Log(false), ElementsAre(NewCall(copy_p1, 15, allocated_size),
                                      DeleteCall(copy_p1, 15, allocated_size)));
}

TEST_F(TCMallocHookTest, CallocInvokesHook) {
  void* p1 = calloc(13, 2);
  EXPECT_THAT(Log(false),
              ElementsAre(NewCall(p1, 13 * 2, GetAllocatedSize(p1))));
  free(p1);
}

TEST_F(TCMallocHookTest, ReallocInvokesHook) {
  void* p1 = calloc(1, 1);
  size_t p1_allocated_size = GetAllocatedSize(p1);
  RestartLog();
  // Copy pointers to suppress use-after-free warnings.
  void* copy_p1 = p1;
  benchmark::DoNotOptimize(copy_p1);
  void* p2 = realloc(p1, 30000);
  EXPECT_THAT(Log(false), ElementsAre(NewCall(p2, 30000, GetAllocatedSize(p2)),
                                      DeleteCall(copy_p1, std::nullopt,
                                                 p1_allocated_size)));
  free(p2);
}

TEST_F(TCMallocHookTest, ReallocShrinkInvokesHook) {
  void* p1 = calloc(1000, 1);
  size_t p1_allocated_size = GetAllocatedSize(p1);
  RestartLog();
  // Copy pointers to suppress use-after-free warnings.
  void* copy_p1 = p1;
  benchmark::DoNotOptimize(copy_p1);
  void* p2 = realloc(p1, 100);
  EXPECT_THAT(Log(false), ElementsAre(NewCall(p2, 100, GetAllocatedSize(p2)),
                                      DeleteCall(copy_p1, std::nullopt,
                                                 p1_allocated_size)));
  free(p2);
}

TEST_F(TCMallocHookTest, ReallocPopInvokesHook) {
  void* p1 = calloc(1000, 1);
  size_t p1_allocated_size = GetAllocatedSize(p1);
  RestartLog();
  // Copy pointers to suppress use-after-free warnings.
  void* copy_p1 = p1;
  benchmark::DoNotOptimize(copy_p1);
  void* p2 = realloc(p1, 999);
  EXPECT_THAT(
      Log(false),
      testing::AnyOf(
          ElementsAre(DeleteCall(copy_p1, std::nullopt, p1_allocated_size,
                                 HookMemoryMutable::kImmutable),
                      NewCall(p2, 999, GetAllocatedSize(p2),
                              HookMemoryMutable::kImmutable)),

          ElementsAre(NewCall(p2, 999, GetAllocatedSize(p2),
                              HookMemoryMutable::kMutable),
                      DeleteCall(copy_p1, std::nullopt, p1_allocated_size,
                                 HookMemoryMutable::kMutable))));
  free(p2);
}

TEST_F(TCMallocHookTest, BoundedReallocInvokesHook) {
  // Triggers the new_size < lower_bound_to_grow case in realloc
  void* p1 = calloc(8000, 1);
  size_t p1_allocated_size = GetAllocatedSize(p1);
  RestartLog();
  // Copy pointers to suppress use-after-free warnings.
  void* copy_p1 = p1;
  benchmark::DoNotOptimize(copy_p1);
  void* p2 = realloc(p1, 9000);
  EXPECT_THAT(Log(false), ElementsAre(NewCall(p2, 9000, GetAllocatedSize(p2)),
                                      DeleteCall(copy_p1, std::nullopt,
                                                 p1_allocated_size)));
  free(p2);
}

TEST_F(TCMallocHookTest, StrDupInvokesHook) {
  char* p1 = strdup("test");
  EXPECT_THAT(Log(false), ElementsAre(NewCall(p1, 5, GetAllocatedSize(p1))));
  free(p1);
}

TEST_F(TCMallocHookTest, PosixMemalignInvokesHook) {
  void* p1;
  ASSERT_EQ(posix_memalign(&p1, sizeof(p1), 40), 0);
  EXPECT_THAT(Log(false), ElementsAre(NewCall(p1, 40, GetAllocatedSize(p1))));
  free(p1);
}

TEST_F(TCMallocHookTest, MemalignInvokesHook) {
  void* p1 = memalign(sizeof(p1) * 2, 50);
  EXPECT_THAT(Log(false), ElementsAre(NewCall(p1, 50, GetAllocatedSize(p1))));
  free(p1);
}

TEST_F(TCMallocHookTest, VallocInvokesHook) {
  void* p1 = valloc(60);
  EXPECT_THAT(Log(false), ElementsAre(NewCall(p1, 60, GetAllocatedSize(p1))));
  free(p1);
}

#if defined(__BIONIC__) || defined(__GLIBC__) || defined(__NEWLIB__)
TEST_F(TCMallocHookTest, PvallocInvokesHook) {
  uint64_t pagesize = sysconf(_SC_PAGESIZE);
  void* p1 = pvalloc(70);
  EXPECT_THAT(Log(false),
              ElementsAre(NewCall(p1, pagesize, GetAllocatedSize(p1))));
  free(p1);
}
#endif

TEST_F(TCMallocHookTest, OperatorNewInvokesHook) {
  void* p1 = ::operator new(11);
  EXPECT_THAT(Log(false), ElementsAre(NewCall(p1, 11, GetAllocatedSize(p1))));
  ::operator delete(p1);
}

TEST_F(TCMallocHookTest, OperatorNewArrayInvokesHook) {
  void* p1 = ::operator new[](13);
  EXPECT_THAT(Log(false), ElementsAre(NewCall(p1, 13, GetAllocatedSize(p1))));
  ::operator delete[](p1);
}

TEST_F(TCMallocHookTest, NothrowOperatorNewInvokesHook) {
  void* p1 = ::operator new(11, std::nothrow);
  EXPECT_THAT(Log(false), ElementsAre(NewCall(p1, 11, GetAllocatedSize(p1))));
  ::operator delete(p1);
}

TEST_F(TCMallocHookTest, NothrowOperatorNewArrayInvokesHook) {
  void* p1 = ::operator new[](13, std::nothrow);
  EXPECT_THAT(Log(false), ElementsAre(NewCall(p1, 13, GetAllocatedSize(p1))));
  ::operator delete[](p1);
}

TEST_F(TCMallocHookTest, OperatorDeleteInvokesHook) {
  void* p1 = ::operator new(11);
  size_t allocated_size = GetAllocatedSize(p1);
  RestartLog();
  // Copy pointers to suppress use-after-free warnings.
  void* copy_p1 = p1;
  benchmark::DoNotOptimize(copy_p1);
  ::operator delete(p1);
  EXPECT_THAT(Log(false),
              ElementsAre(DeleteCall(copy_p1, std::nullopt, allocated_size)));
}

TEST_F(TCMallocHookTest, SizedOperatorDeleteInvokesHook) {
  void* p1 = ::operator new(11);
  size_t allocated_size = GetAllocatedSize(p1);
  RestartLog();
  // Copy pointers to suppress use-after-free warnings.
  void* copy_p1 = p1;
  benchmark::DoNotOptimize(copy_p1);
#ifdef __cpp_sized_deallocation
  ::operator delete(p1, 11);
  EXPECT_THAT(Log(false), ElementsAre(DeleteCall(copy_p1, 11, allocated_size)));
#else
  ::operator delete(p1);
  EXPECT_THAT(Log(false),
              ElementsAre(DeleteCall(copy_p1, std::nullopt, allocated_size)));
#endif
}

TEST_F(TCMallocHookTest, OperatorDeleteArrayInvokesHook) {
  void* p1 = ::operator new[](13);
  size_t allocated_size = GetAllocatedSize(p1);
  RestartLog();
  // Copy pointers to suppress use-after-free warnings.
  void* copy_p1 = p1;
  benchmark::DoNotOptimize(copy_p1);
  ::operator delete[](p1);
  EXPECT_THAT(Log(false),
              ElementsAre(DeleteCall(copy_p1, std::nullopt, allocated_size)));
}

TEST_F(TCMallocHookTest, SizedOperatorDeleteArrayInvokesHook) {
  void* p1 = ::operator new[](13);
  size_t allocated_size = GetAllocatedSize(p1);
  RestartLog();
  // Copy pointers to suppress use-after-free warnings.
  void* copy_p1 = p1;
  benchmark::DoNotOptimize(copy_p1);
#ifdef __cpp_sized_deallocation
  ::operator delete[](p1, 13);
  EXPECT_THAT(Log(false), ElementsAre(DeleteCall(copy_p1, 13, allocated_size)));
#else
  ::operator delete[](p1);
  EXPECT_THAT(Log(false),
              ElementsAre(DeleteCall(copy_p1, std::nullopt, allocated_size)));
#endif
}

TEST_F(TCMallocHookTest, NothrowOperatorDeleteInvokesHook) {
  void* p1 = ::operator new(11);
  size_t allocated_size = GetAllocatedSize(p1);
  RestartLog();
  // Copy pointers to suppress use-after-free warnings.
  void* copy_p1 = p1;
  benchmark::DoNotOptimize(copy_p1);
  ::operator delete(p1, std::nothrow);
  EXPECT_THAT(Log(false),
              ElementsAre(DeleteCall(copy_p1, std::nullopt, allocated_size)));
}

TEST_F(TCMallocHookTest, NothrowOperatorDeleteArrayInvokesHook) {
  void* p1 = ::operator new[](13);
  size_t allocated_size = GetAllocatedSize(p1);
  RestartLog();
  // Copy pointers to suppress use-after-free warnings.
  void* copy_p1 = p1;
  benchmark::DoNotOptimize(copy_p1);
  ::operator delete[](p1, std::nothrow);
  EXPECT_THAT(Log(false),
              ElementsAre(DeleteCall(copy_p1, std::nullopt, allocated_size)));
}

TEST_F(TCMallocHookTest, SizeReturningOperatorNewInvokesHook) {
  sized_ptr_t res = __size_returning_new(29);
  EXPECT_THAT(Log(false),
              ElementsAre(NewCall(res.p, res.n, GetAllocatedSize(res.p))));
  ::operator delete(res.p);
}

TEST_F(TCMallocHookTest, GuardedAllocatedSizeIsRequested) {
  MallocExtension::ActivateGuardedSampling();
  MallocExtension::SetProfileSamplingInterval(1);
  // Allocate a large block of memory to make sure
  // Sampler::RecordAllocationSlow is called so that
  // Sampler::bytes_until_sample_ is updated.
  void* p = ::operator new[](256 * 1024 * 1024);
  ::operator delete[](p);

  MallocExtension::SetGuardedSamplingInterval(0);  // Guard every sample
  // Allocate some memory until allocs_until_guarded_sample_ reaches 0.
  for (int i = 0; i < 1000; i++) {
    void* p = ::operator new[](32);
    ::operator delete[](p);
    RestartLog();  // Prevent overflowing log
  }

  sized_ptr_t res = __size_returning_new(34);
  EXPECT_THAT(Log(false), ElementsAre(NewCall(res.p, 34, 34)));
  EXPECT_EQ(GetAllocatedSize(res.p), 34);
  EXPECT_EQ(res.n, 34);

  ::operator delete(res.p);
  MallocExtension::SetProfileSamplingInterval(0);
}

// These are used as callbacks by the sanity-check.  Set* and Reset* register
// the hook that counts how many times the associated memory function is called.
// After each such call, call Verify* to verify that we used the tcmalloc
// version of the call, and not the libc.  Note the ... in the hook signature:
// we don't care what arguments the hook takes.
#define MAKE_HOOK_CALLBACK(hook_type, ...)                                   \
  static int g_##hook_type##_calls = 0;                                      \
  static void IncrementCallsTo##hook_type(__VA_ARGS__) {                     \
    g_##hook_type##_calls++;                                                 \
  }                                                                          \
  static void Verify##hook_type##WasCalled() {                               \
    ASSERT_GT(g_##hook_type##_calls, 0);                                     \
    g_##hook_type##_calls = 0; /* reset for next call */                     \
  }                                                                          \
  static void Set##hook_type() {                                             \
    ASSERT_TRUE(MallocHook::Add##hook_type(IncrementCallsTo##hook_type));    \
  }                                                                          \
  static void Reset##hook_type() {                                           \
    ASSERT_TRUE(MallocHook::Remove##hook_type(IncrementCallsTo##hook_type)); \
  }

#define MAKE_HOOK_CALLBACK_ABSL(hook_type, ...)                              \
  static int g_##hook_type##_calls = 0;                                      \
  static void IncrementCallsTo##hook_type(__VA_ARGS__) {                     \
    g_##hook_type##_calls++;                                                 \
  }                                                                          \
  static void Verify##hook_type##WasCalled() {                               \
    ASSERT_GT(g_##hook_type##_calls, 0);                                     \
    g_##hook_type##_calls = 0; /* reset for next call */                     \
  }                                                                          \
  static void Set##hook_type() {                                             \
    ASSERT_TRUE(MallocHook::Add##hook_type(IncrementCallsTo##hook_type));    \
  }                                                                          \
  static void Reset##hook_type() {                                           \
    ASSERT_TRUE(MallocHook::Remove##hook_type(IncrementCallsTo##hook_type)); \
  }

// We do one for each hook typedef in malloc_hook.h
MAKE_HOOK_CALLBACK_ABSL(NewHook, const MallocHook::NewInfo&);
MAKE_HOOK_CALLBACK_ABSL(DeleteHook, const MallocHook::DeleteInfo&);
MAKE_HOOK_CALLBACK(SampledNewHook, const MallocHook::SampledAlloc&);
MAKE_HOOK_CALLBACK(SampledDeleteHook,
                   const
                   MallocHook::SampledAlloc&);

TEST(TCMallocTest, GetStatsReportsHooks) {
#ifndef NDEBUG
  // In debug builds, key locks in TCMalloc verify that no allocations are
  // occurring with AllocationGuard.  We skip this scenario, but we confirm that
  // AllocationGuard does install hooks to avoid spurious skips if this changes.
  int new_hook_count = 0;
  {
    tcmalloc_internal::AllocationGuard guard;
    new_hook_count = tcmalloc_internal::new_hooks_.size();
  }
  EXPECT_GT(new_hook_count, 0);
  GTEST_SKIP() << "Under debug builds, AllocationGuard installs extra hooks.";
#endif

  std::string stats;

  if (absl::LeakCheckerIsActive()) {
    // The HeapLeakChecker uses hooks to track allocations, so we expect to have
    // some present independent of our own.
    stats = MallocExtension::GetStats();
    EXPECT_THAT(
        stats,
        HasSubstr(
            "MALLOC HOOKS: NEW=1 DELETE=1 SAMPLED_NEW=0 SAMPLED_DELETE=0"));
    return;
  }

  // We do not have any hooks configured.
  stats = MallocExtension::GetStats();
  EXPECT_THAT(
      stats,
      HasSubstr("MALLOC HOOKS: NEW=0 DELETE=0 SAMPLED_NEW=0 SAMPLED_DELETE=0"));

  // Add a hook, confirm it's reported, add a second, then remove them.
  SetNewHook();
  stats = MallocExtension::GetStats();
  EXPECT_THAT(
      stats,
      HasSubstr("MALLOC HOOKS: NEW=1 DELETE=0 SAMPLED_NEW=0 SAMPLED_DELETE=0"));
  SetNewHook();
  stats = MallocExtension::GetStats();
  EXPECT_THAT(
      stats,
      HasSubstr("MALLOC HOOKS: NEW=2 DELETE=0 SAMPLED_NEW=0 SAMPLED_DELETE=0"));
  ResetNewHook();
  ResetNewHook();

  SetDeleteHook();
  stats = MallocExtension::GetStats();
  EXPECT_THAT(
      stats,
      HasSubstr("MALLOC HOOKS: NEW=0 DELETE=1 SAMPLED_NEW=0 SAMPLED_DELETE=0"));
  SetDeleteHook();
  stats = MallocExtension::GetStats();
  EXPECT_THAT(
      stats,
      HasSubstr("MALLOC HOOKS: NEW=0 DELETE=2 SAMPLED_NEW=0 SAMPLED_DELETE=0"));
  ResetDeleteHook();
  ResetDeleteHook();

  SetSampledNewHook();
  stats = MallocExtension::GetStats();
  EXPECT_THAT(
      stats,
      HasSubstr("MALLOC HOOKS: NEW=0 DELETE=0 SAMPLED_NEW=1 SAMPLED_DELETE=0"));
  SetSampledNewHook();
  stats = MallocExtension::GetStats();
  EXPECT_THAT(
      stats,
      HasSubstr("MALLOC HOOKS: NEW=0 DELETE=0 SAMPLED_NEW=2 SAMPLED_DELETE=0"));
  ResetSampledNewHook();
  ResetSampledNewHook();

  SetSampledDeleteHook();
  stats = MallocExtension::GetStats();
  EXPECT_THAT(
      stats,
      HasSubstr("MALLOC HOOKS: NEW=0 DELETE=0 SAMPLED_NEW=0 SAMPLED_DELETE=1"));
  SetSampledDeleteHook();
  stats = MallocExtension::GetStats();
  EXPECT_THAT(
      stats,
      HasSubstr("MALLOC HOOKS: NEW=0 DELETE=0 SAMPLED_NEW=0 SAMPLED_DELETE=2"));
  ResetSampledDeleteHook();
  ResetSampledDeleteHook();
}

TEST(TCMallocTest, MarkThreadBusy) {
  static thread_local bool log_hooks = false;

  // Verify that MarkThreadBusy does not cause the hooks to be invoked.  See
  // b/112640764.
  const auto new_hook = [](const MallocHook::NewInfo&) {
    TC_CHECK(!log_hooks);
  };
  const auto delete_hook = [](const MallocHook::DeleteInfo&) {
    TC_CHECK(!log_hooks);
  };

  log_hooks = true;
  ASSERT_TRUE(MallocHook::AddNewHook(new_hook));
  ASSERT_TRUE(MallocHook::AddDeleteHook(delete_hook));

  MallocExtension::MarkThreadBusy();

  ASSERT_TRUE(MallocHook::RemoveNewHook(new_hook));
  ASSERT_TRUE(MallocHook::RemoveDeleteHook(delete_hook));
  log_hooks = false;
}

}  // namespace
}  // namespace tcmalloc
