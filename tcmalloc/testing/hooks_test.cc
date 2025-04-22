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
#include <string>
#include <vector>

#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "absl/debugging/leak_check.h"
#include "tcmalloc/internal/allocation_guard.h"
#include "tcmalloc/internal/logging.h"
#include "tcmalloc/malloc_extension.h"
#include "tcmalloc/malloc_hook.h"

namespace tcmalloc {
namespace {

using testing::Contains;
using testing::ElementsAre;
using testing::HasSubstr;
using testing::Lt;
using testing::SizeIs;

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

#define MAKE_HOOK_CALLBACK_ABSL(hook_type, ...)                     \
  static int g_##hook_type##_calls = 0;                             \
  static void IncrementCallsTo##hook_type(__VA_ARGS__) {            \
    g_##hook_type##_calls++;                                        \
  }                                                                 \
  static void Verify##hook_type##WasCalled() {                      \
    ASSERT_GT(g_##hook_type##_calls, 0);                            \
    g_##hook_type##_calls = 0; /* reset for next call */            \
  }                                                                 \
  static void Set##hook_type() {                                    \
    ASSERT_TRUE(absl::base_internal::MallocHook::Add##hook_type(    \
        IncrementCallsTo##hook_type));                              \
  }                                                                 \
  static void Reset##hook_type() {                                  \
    ASSERT_TRUE(absl::base_internal::MallocHook::Remove##hook_type( \
        IncrementCallsTo##hook_type));                              \
  }

// We do one for each hook typedef in malloc_hook.h
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
  }
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

}  // namespace
}  // namespace tcmalloc
