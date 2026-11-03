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

#include "tcmalloc/internal/slc_exclusion.h"

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "gtest/gtest.h"

namespace tcmalloc {
namespace tcmalloc_internal {
namespace {

constexpr size_t kHugePageBytes = 2 * 1024 * 1024;

struct RecordedCall {
  uintptr_t addr;
  size_t size;
  bool exclude;
};

struct FakeActuatorState {
  int32_t exclude_retval = 0;
  int32_t include_retval = 0;
  std::vector<RecordedCall> calls;
};

int32_t FakeActuator(void* ptr, size_t size, bool exclude, void* arg) {
  auto* state = static_cast<FakeActuatorState*>(arg);
  state->calls.push_back(RecordedCall{
      .addr = reinterpret_cast<uintptr_t>(ptr),
      .size = size,
      .exclude = exclude,
  });
  return exclude ? state->exclude_retval : state->include_retval;
}

TEST(SlcExclusionTest, BackendStateStringsMatchProtoEnumNames) {
  EXPECT_EQ(SlcExclusionBackendStateToString(
                SlcExclusionBackendState::kUnsupportedArch),
            "SLC_EXCLUSION_UNSUPPORTED_ARCH");
  EXPECT_EQ(SlcExclusionBackendStateToString(
                SlcExclusionBackendState::kUninitialized),
            "SLC_EXCLUSION_UNINITIALIZED");
  EXPECT_EQ(SlcExclusionBackendStateToString(SlcExclusionBackendState::kActive),
            "SLC_EXCLUSION_ACTIVE");
  EXPECT_EQ(SlcExclusionBackendStateToString(
                SlcExclusionBackendState::kPinnedProgUnavailable),
            "SLC_EXCLUSION_PINNED_PROG_UNAVAILABLE");
  EXPECT_EQ(SlcExclusionBackendStateToString(
                SlcExclusionBackendState::kKernelUnsupported),
            "SLC_EXCLUSION_KERNEL_UNSUPPORTED");
  EXPECT_EQ(
      SlcExclusionBackendStateToString(SlcExclusionBackendState::kTestOverride),
      "SLC_EXCLUSION_TEST_OVERRIDE");
  EXPECT_EQ(
      SlcExclusionBackendStateToString(SlcExclusionBackendState::kSandboxed),
      "SLC_EXCLUSION_SANDBOXED");
  EXPECT_EQ(SlcExclusionBackendStateToString(
                static_cast<SlcExclusionBackendState>(255)),
            "SLC_EXCLUSION_UNSUPPORTED_ARCH");
}

TEST(SlcExclusionTest, RejectsInvalidAlignmentAndSizeWithoutSyscall) {
  FakeActuatorState fake;
  ScopedSlcExclusionActuatorForTesting scoped(&FakeActuator, &fake);

  void* aligned = reinterpret_cast<void*>(uintptr_t{0x400000200000ULL});
  void* unaligned = reinterpret_cast<void*>(uintptr_t{0x400000201000ULL});

  EXPECT_EQ(SlcExcludeRange(nullptr, kHugePageBytes), -EINVAL);
  EXPECT_EQ(SlcExcludeRange(aligned, 0), -EINVAL);
  EXPECT_EQ(SlcExcludeRange(unaligned, kHugePageBytes), -EINVAL);
  EXPECT_EQ(SlcExcludeRange(aligned, kHugePageBytes + 4096), -EINVAL);
  EXPECT_EQ(SlcIncludeRange(unaligned, kHugePageBytes), -EINVAL);

  EXPECT_TRUE(fake.calls.empty());
  const SlcExclusionStats stats = GetSlcExclusionStats();
  EXPECT_EQ(stats.exclude_syscalls, 0);
  EXPECT_EQ(stats.exclude_errors, 4);
  EXPECT_EQ(stats.last_exclude_retval, -EINVAL);
  EXPECT_EQ(stats.include_syscalls, 0);
  EXPECT_EQ(stats.include_errors, 1);
  EXPECT_EQ(stats.last_include_retval, -EINVAL);
}

TEST(SlcExclusionTest, ExcludesAndReIncludesAlignedHugePagesAndStripsTbi) {
  FakeActuatorState fake;
  ScopedSlcExclusionActuatorForTesting scoped(&FakeActuator, &fake);
  EXPECT_TRUE(SlcExclusionAvailable());

  // Simulate an ARM64 TBI top-byte tag on a normal-heap 2 MiB-aligned VA.
  const uintptr_t expected_va = 0x0000040000400000ULL;
  void* tbi_tagged =
      reinterpret_cast<void*>(expected_va | (uintptr_t{0xab} << 56));

  EXPECT_EQ(SlcExcludeRange(tbi_tagged, kHugePageBytes), 0);
  EXPECT_EQ(SlcIncludeRange(tbi_tagged, 2 * kHugePageBytes), 0);

  ASSERT_EQ(fake.calls.size(), 2u);
  EXPECT_EQ(fake.calls[0].addr, expected_va);
  EXPECT_EQ(fake.calls[0].size, kHugePageBytes);
  EXPECT_TRUE(fake.calls[0].exclude);
  EXPECT_EQ(fake.calls[1].addr, expected_va);
  EXPECT_EQ(fake.calls[1].size, 2 * kHugePageBytes);
  EXPECT_FALSE(fake.calls[1].exclude);

  const SlcExclusionStats stats = GetSlcExclusionStats();
  EXPECT_EQ(stats.backend_state, SlcExclusionBackendState::kTestOverride);
  EXPECT_EQ(stats.exclude_syscalls, 1);
  EXPECT_EQ(stats.exclude_succeeded, 1);
  EXPECT_EQ(stats.exclude_errors, 0);
  EXPECT_EQ(stats.last_exclude_retval, 0);
  EXPECT_EQ(stats.include_syscalls, 1);
  EXPECT_EQ(stats.include_succeeded, 1);
  EXPECT_EQ(stats.include_errors, 0);
  EXPECT_EQ(stats.last_include_retval, 0);
}

TEST(SlcExclusionTest, KernelUnsupportedStickilyDisablesAvailability) {
  for (const int32_t unsupported_err : {-EOPNOTSUPP, -ENOSYS}) {
    FakeActuatorState fake;
    fake.exclude_retval = unsupported_err;
    ScopedSlcExclusionActuatorForTesting scoped(&FakeActuator, &fake);
    ASSERT_TRUE(SlcExclusionAvailable());

    void* aligned = reinterpret_cast<void*>(uintptr_t{0x400000200000ULL});
    EXPECT_EQ(SlcExcludeRange(aligned, kHugePageBytes), unsupported_err);
    EXPECT_FALSE(SlcExclusionAvailable());

    const SlcExclusionStats stats = GetSlcExclusionStats();
    EXPECT_EQ(stats.backend_state,
              SlcExclusionBackendState::kKernelUnsupported);
    EXPECT_EQ(stats.exclude_syscalls, 1);
    EXPECT_EQ(stats.exclude_succeeded, 0);
    EXPECT_EQ(stats.exclude_errors, 1);
    EXPECT_EQ(stats.last_exclude_retval, unsupported_err);
  }
}

TEST(SlcExclusionTest, TransientErrorsPreserveAvailabilityAndTrackIncludeFail) {
  FakeActuatorState fake;
  fake.exclude_retval = -EIO;
  fake.include_retval = -EIO;
  ScopedSlcExclusionActuatorForTesting scoped(&FakeActuator, &fake);
  ASSERT_TRUE(SlcExclusionAvailable());

  void* aligned = reinterpret_cast<void*>(uintptr_t{0x400000200000ULL});
  EXPECT_EQ(SlcExcludeRange(aligned, kHugePageBytes), -EIO);
  EXPECT_TRUE(SlcExclusionAvailable());
  EXPECT_EQ(SlcIncludeRange(aligned, kHugePageBytes), -EIO);
  EXPECT_TRUE(SlcExclusionAvailable());

  const SlcExclusionStats stats = GetSlcExclusionStats();
  EXPECT_EQ(stats.backend_state, SlcExclusionBackendState::kTestOverride);
  EXPECT_EQ(stats.exclude_syscalls, 1);
  EXPECT_EQ(stats.exclude_succeeded, 0);
  EXPECT_EQ(stats.exclude_errors, 1);
  EXPECT_EQ(stats.last_exclude_retval, -EIO);
  EXPECT_EQ(stats.include_syscalls, 1);
  EXPECT_EQ(stats.include_succeeded, 0);
  EXPECT_EQ(stats.include_errors, 1);
  EXPECT_EQ(stats.last_include_retval, -EIO);
}

TEST(SlcExclusionTest, SandboxedProcessLatchesSandboxedWithoutBpfSyscall) {
  void* aligned = reinterpret_cast<void*>(uintptr_t{0x400000200000ULL});
  bool available_before = false;
  int32_t ex_rc = 0;
  bool available_after = true;
  int32_t in_rc = 0;
  SlcExclusionStats stats;
  {
    ScopedSlcSandboxStateForTesting scoped_sandbox(/*force_sandboxed=*/true);
    available_before = SlcExclusionAvailable();
    ex_rc = SlcExcludeRange(aligned, kHugePageBytes);
    available_after = SlcExclusionAvailable();
    in_rc = SlcIncludeRange(aligned, kHugePageBytes);
    stats = GetSlcExclusionStats();
  }

  EXPECT_TRUE(available_before);
  EXPECT_EQ(ex_rc, -EPERM);
  EXPECT_FALSE(available_after);
  EXPECT_EQ(in_rc, -EPERM);
  EXPECT_EQ(stats.backend_state, SlcExclusionBackendState::kSandboxed);
  EXPECT_EQ(stats.open_errno, EPERM);
  EXPECT_EQ(stats.exclude_syscalls, 0);
  EXPECT_EQ(stats.exclude_succeeded, 0);
  EXPECT_EQ(stats.exclude_errors, 1);
  EXPECT_EQ(stats.last_exclude_retval, -EPERM);
  EXPECT_EQ(stats.include_syscalls, 0);
  EXPECT_EQ(stats.include_succeeded, 0);
  EXPECT_EQ(stats.include_errors, 1);
  EXPECT_EQ(stats.last_include_retval, -EPERM);
}

#if !(defined(__aarch64__) && defined(__linux__))
TEST(SlcExclusionTest, NonArm64ReportsUnsupportedArchWithoutSyscalls) {
  ResetSlcExclusionStatsForTesting();
  EXPECT_FALSE(SlcExclusionAvailable());
  void* aligned = reinterpret_cast<void*>(uintptr_t{0x400000200000ULL});
  EXPECT_EQ(SlcExcludeRange(aligned, kHugePageBytes), -ENOTSUP);
  const SlcExclusionStats stats = GetSlcExclusionStats();
  EXPECT_EQ(stats.backend_state, SlcExclusionBackendState::kUnsupportedArch);
  EXPECT_EQ(stats.exclude_syscalls, 0);
  EXPECT_EQ(stats.exclude_errors, 1);
  EXPECT_EQ(stats.last_exclude_retval, -ENOTSUP);
  ResetSlcExclusionStatsForTesting();
}
#endif

}  // namespace
}  // namespace tcmalloc_internal
}  // namespace tcmalloc
