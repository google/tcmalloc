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

#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>

#if defined(__aarch64__) && defined(__linux__)
#include <fcntl.h>
#include <linux/bpf.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

#include "absl/base/attributes.h"
#include "absl/base/internal/spinlock.h"
#include "absl/strings/string_view.h"
#include "tcmalloc/internal/config.h"

GOOGLE_MALLOC_SECTION_BEGIN
namespace tcmalloc {
namespace tcmalloc_internal {
namespace {

constexpr size_t kSlcPageBlockAlignment = 2 * 1024 * 1024;

#if defined(__aarch64__) && defined(__linux__)
constexpr char kPinnedSlcPolicyProgPath[] =
    "/sys/fs/bpf/ebpfd/program_sets/ebpf_slc_policy/programs/slc_policy_run/0";
constexpr SlcExclusionBackendState kDefaultBackendState =
    SlcExclusionBackendState::kUninitialized;

// Must match `struct slc_sys_args` in `ebpf_slc_policy.c`.
struct SlcSysArgs {
  int32_t pid;
  uint64_t start;
  uint64_t len;
  bool exclusion_state;
};
static_assert(sizeof(SlcSysArgs) == 32);
static_assert(offsetof(SlcSysArgs, pid) == 0);
static_assert(offsetof(SlcSysArgs, start) == 8);
static_assert(offsetof(SlcSysArgs, len) == 16);
static_assert(offsetof(SlcSysArgs, exclusion_state) == 24);
#else
constexpr SlcExclusionBackendState kDefaultBackendState =
    SlcExclusionBackendState::kUnsupportedArch;
#endif

ABSL_CONST_INIT absl::base_internal::SpinLock g_slc_init_lock(
    absl::base_internal::SCHEDULE_KERNEL_ONLY);

ABSL_CONST_INIT std::atomic<SlcExclusionBackendState> g_backend_state{
    kDefaultBackendState};
ABSL_CONST_INIT std::atomic<int> g_prog_fd{-1};
ABSL_CONST_INIT std::atomic<int> g_open_errno{0};

ABSL_CONST_INIT std::atomic<int64_t> g_exclude_syscalls{0};
ABSL_CONST_INIT std::atomic<int64_t> g_exclude_succeeded{0};
ABSL_CONST_INIT std::atomic<int64_t> g_exclude_errors{0};
ABSL_CONST_INIT std::atomic<int64_t> g_last_exclude_retval{0};

ABSL_CONST_INIT std::atomic<int64_t> g_include_syscalls{0};
ABSL_CONST_INIT std::atomic<int64_t> g_include_succeeded{0};
ABSL_CONST_INIT std::atomic<int64_t> g_include_errors{0};
ABSL_CONST_INIT std::atomic<int64_t> g_last_include_retval{0};

ABSL_CONST_INIT std::atomic<SlcExclusionActuatorFn> g_test_actuator{nullptr};
ABSL_CONST_INIT std::atomic<void*> g_test_actuator_arg{nullptr};
ABSL_CONST_INIT std::atomic<bool> g_force_sandboxed_for_testing{false};

void RecordOutcome(bool exclude, bool counted_syscall, int32_t retval) {
  if (exclude) {
    if (counted_syscall) {
      g_exclude_syscalls.fetch_add(1, std::memory_order_relaxed);
    }
    if (retval == 0) {
      g_exclude_succeeded.fetch_add(1, std::memory_order_relaxed);
    } else {
      g_exclude_errors.fetch_add(1, std::memory_order_relaxed);
    }
    g_last_exclude_retval.store(retval, std::memory_order_relaxed);
  } else {
    if (counted_syscall) {
      g_include_syscalls.fetch_add(1, std::memory_order_relaxed);
    }
    if (retval == 0) {
      g_include_succeeded.fetch_add(1, std::memory_order_relaxed);
    } else {
      g_include_errors.fetch_add(1, std::memory_order_relaxed);
    }
    g_last_include_retval.store(retval, std::memory_order_relaxed);
  }
}

bool IsRunningInSandbox2() {
  if (g_force_sandboxed_for_testing.load(std::memory_order_acquire)) {
    return true;
  }
#if defined(__aarch64__) && defined(__linux__)
  const int old_errno = errno;
  // Match `tcmalloc::tcmalloc_internal::IsSandboxed()`: sandbox2's default
  // seccomp policy returns `errno == 0xfdb` on `syscall(0x2f000fdb)` and kills
  // `__NR_bpf` even when `AllowAllSyscalls()` is configured. Kept inline here
  // because this library is exported upstream and sits below `//tcmalloc`.
  constexpr uint64_t kSyscallNumber = 0x2f000fdb;
  constexpr int kSyscallSandbox2Errno = 0x000000fdb;
  const int64_t ret = syscall(kSyscallNumber);
  const bool is_sandboxed = (ret != 0 && errno == kSyscallSandbox2Errno);
  errno = old_errno;
  return is_sandboxed;
#else
  return false;
#endif
}

#if defined(__aarch64__) && defined(__linux__)
int OpenPinnedSlcProgWithFlags(uint32_t file_flags) {
  union bpf_attr attr;
  std::memset(&attr, 0, sizeof(attr));
  attr.pathname = reinterpret_cast<uint64_t>(kPinnedSlcPolicyProgPath);
  attr.file_flags = file_flags;
  int fd = -1;
  do {
    fd = static_cast<int>(syscall(__NR_bpf, BPF_OBJ_GET, &attr, sizeof(attr)));
  } while (fd < 0 && errno == EINTR);
  return fd;
}

int EnsureProgFd() {
  SlcExclusionBackendState state =
      g_backend_state.load(std::memory_order_acquire);
  if (state == SlcExclusionBackendState::kActive) {
    return g_prog_fd.load(std::memory_order_relaxed);
  }
  if (state != SlcExclusionBackendState::kUninitialized) {
    return -1;
  }

  absl::base_internal::SpinLockHolder lock(g_slc_init_lock);
  state = g_backend_state.load(std::memory_order_relaxed);
  if (state == SlcExclusionBackendState::kActive) {
    return g_prog_fd.load(std::memory_order_relaxed);
  }
  if (state != SlcExclusionBackendState::kUninitialized) {
    return -1;
  }

  if (IsRunningInSandbox2()) {
    g_open_errno.store(EPERM, std::memory_order_relaxed);
    g_backend_state.store(SlcExclusionBackendState::kSandboxed,
                          std::memory_order_release);
    return -1;
  }

  int fd = OpenPinnedSlcProgWithFlags(BPF_F_RDONLY);
  if (fd < 0) {
    fd = OpenPinnedSlcProgWithFlags(0);
  }
  if (fd < 0) {
    const int err = errno != 0 ? errno : ENOENT;
    g_open_errno.store(err, std::memory_order_relaxed);
    g_backend_state.store(SlcExclusionBackendState::kPinnedProgUnavailable,
                          std::memory_order_release);
    return -1;
  }

  (void)fcntl(fd, F_SETFD, FD_CLOEXEC);
  g_prog_fd.store(fd, std::memory_order_relaxed);
  g_open_errno.store(0, std::memory_order_relaxed);
  g_backend_state.store(SlcExclusionBackendState::kActive,
                        std::memory_order_release);
  return fd;
}
#endif

int32_t ActuateSlcRange(void* ptr, size_t size, bool exclude) {
  const uintptr_t raw_addr = reinterpret_cast<uintptr_t>(ptr);
  // Strip any ARM64 TBI top-byte tag while preserving bits 0..55 (including
  // TCMalloc's normal heap memory tags in bits 42..44).
  const uintptr_t untagged_addr = raw_addr & ~(uintptr_t{0xff} << 56);
  if (ptr == nullptr || size == 0 ||
      (untagged_addr % kSlcPageBlockAlignment) != 0 ||
      (size % kSlcPageBlockAlignment) != 0) {
    RecordOutcome(exclude, /*counted_syscall=*/false, -EINVAL);
    return -EINVAL;
  }

  SlcExclusionActuatorFn test_fn =
      g_test_actuator.load(std::memory_order_acquire);
  if (test_fn != nullptr) {
    void* arg = g_test_actuator_arg.load(std::memory_order_relaxed);
    const int32_t retval =
        test_fn(reinterpret_cast<void*>(untagged_addr), size, exclude, arg);
    RecordOutcome(exclude, /*counted_syscall=*/true, retval);
    if (exclude && (retval == -EOPNOTSUPP || retval == -ENOSYS)) {
      g_backend_state.store(SlcExclusionBackendState::kKernelUnsupported,
                            std::memory_order_release);
    }
    return retval;
  }

  const SlcExclusionBackendState pre_state =
      g_backend_state.load(std::memory_order_acquire);
  if (pre_state == SlcExclusionBackendState::kSandboxed ||
      (pre_state == SlcExclusionBackendState::kUninitialized &&
       IsRunningInSandbox2())) {
    if (pre_state != SlcExclusionBackendState::kSandboxed) {
      absl::base_internal::SpinLockHolder lock(g_slc_init_lock);
      if (g_backend_state.load(std::memory_order_relaxed) ==
          SlcExclusionBackendState::kUninitialized) {
        g_open_errno.store(EPERM, std::memory_order_relaxed);
        g_backend_state.store(SlcExclusionBackendState::kSandboxed,
                              std::memory_order_release);
      }
    }
    RecordOutcome(exclude, /*counted_syscall=*/false, -EPERM);
    return -EPERM;
  }

#if defined(__aarch64__) && defined(__linux__)
  const int fd = EnsureProgFd();
  if (fd < 0) {
    const SlcExclusionBackendState state =
        g_backend_state.load(std::memory_order_acquire);
    int err = g_open_errno.load(std::memory_order_relaxed);
    if (state == SlcExclusionBackendState::kKernelUnsupported) {
      err = EOPNOTSUPP;
    } else if (state == SlcExclusionBackendState::kSandboxed) {
      err = EPERM;
    } else if (err <= 0) {
      err = ENOENT;
    }
    RecordOutcome(exclude, /*counted_syscall=*/false, -err);
    return -err;
  }

  if (IsRunningInSandbox2()) {
    g_open_errno.store(EPERM, std::memory_order_relaxed);
    g_backend_state.store(SlcExclusionBackendState::kSandboxed,
                          std::memory_order_release);
    RecordOutcome(exclude, /*counted_syscall=*/false, -EPERM);
    return -EPERM;
  }

  SlcSysArgs args;
  std::memset(&args, 0, sizeof(args));
  args.pid = 0;
  args.start = static_cast<uint64_t>(untagged_addr);
  args.len = static_cast<uint64_t>(size);
  args.exclusion_state = exclude;

  union bpf_attr attr;
  std::memset(&attr, 0, sizeof(attr));
  attr.test.prog_fd = static_cast<uint32_t>(fd);
  attr.test.ctx_in = reinterpret_cast<uint64_t>(&args);
  attr.test.ctx_size_in = sizeof(args);

  int64_t rc = -1;
  do {
    rc = syscall(__NR_bpf, BPF_PROG_TEST_RUN, &attr, sizeof(attr));
  } while (rc < 0 && errno == EINTR);

  int32_t retval = 0;
  if (rc == 0) {
    retval = static_cast<int32_t>(attr.test.retval);
  } else {
    retval = -(errno != 0 ? errno : EIO);
  }

  RecordOutcome(exclude, /*counted_syscall=*/true, retval);
  if (exclude && (retval == -EOPNOTSUPP || retval == -ENOSYS)) {
    g_backend_state.store(SlcExclusionBackendState::kKernelUnsupported,
                          std::memory_order_release);
  }
  return retval;
#else
  RecordOutcome(exclude, /*counted_syscall=*/false, -ENOTSUP);
  return -ENOTSUP;
#endif
}

}  // namespace

absl::string_view SlcExclusionBackendStateToString(
    SlcExclusionBackendState state) {
  switch (state) {
    case SlcExclusionBackendState::kUnsupportedArch:
      return "SLC_EXCLUSION_UNSUPPORTED_ARCH";
    case SlcExclusionBackendState::kUninitialized:
      return "SLC_EXCLUSION_UNINITIALIZED";
    case SlcExclusionBackendState::kActive:
      return "SLC_EXCLUSION_ACTIVE";
    case SlcExclusionBackendState::kPinnedProgUnavailable:
      return "SLC_EXCLUSION_PINNED_PROG_UNAVAILABLE";
    case SlcExclusionBackendState::kKernelUnsupported:
      return "SLC_EXCLUSION_KERNEL_UNSUPPORTED";
    case SlcExclusionBackendState::kTestOverride:
      return "SLC_EXCLUSION_TEST_OVERRIDE";
    case SlcExclusionBackendState::kSandboxed:
      return "SLC_EXCLUSION_SANDBOXED";
  }
  return "SLC_EXCLUSION_UNSUPPORTED_ARCH";
}

bool SlcExclusionAvailable() {
  const SlcExclusionBackendState state =
      g_backend_state.load(std::memory_order_acquire);
  return state == SlcExclusionBackendState::kUninitialized ||
         state == SlcExclusionBackendState::kActive ||
         state == SlcExclusionBackendState::kTestOverride;
}

int32_t SlcExcludeRange(void* ptr, size_t size) {
  return ActuateSlcRange(ptr, size, /*exclude=*/true);
}

int32_t SlcIncludeRange(void* ptr, size_t size) {
  return ActuateSlcRange(ptr, size, /*exclude=*/false);
}

SlcExclusionStats GetSlcExclusionStats() {
  SlcExclusionStats stats;
  stats.backend_state = g_backend_state.load(std::memory_order_acquire);
  stats.prog_fd = g_prog_fd.load(std::memory_order_relaxed);
  stats.open_errno = g_open_errno.load(std::memory_order_relaxed);
  stats.exclude_syscalls = g_exclude_syscalls.load(std::memory_order_relaxed);
  stats.exclude_succeeded = g_exclude_succeeded.load(std::memory_order_relaxed);
  stats.exclude_errors = g_exclude_errors.load(std::memory_order_relaxed);
  stats.last_exclude_retval =
      g_last_exclude_retval.load(std::memory_order_relaxed);
  stats.include_syscalls = g_include_syscalls.load(std::memory_order_relaxed);
  stats.include_succeeded = g_include_succeeded.load(std::memory_order_relaxed);
  stats.include_errors = g_include_errors.load(std::memory_order_relaxed);
  stats.last_include_retval =
      g_last_include_retval.load(std::memory_order_relaxed);
  return stats;
}

void ResetSlcExclusionStatsForTesting() {
  g_exclude_syscalls.store(0, std::memory_order_relaxed);
  g_exclude_succeeded.store(0, std::memory_order_relaxed);
  g_exclude_errors.store(0, std::memory_order_relaxed);
  g_last_exclude_retval.store(0, std::memory_order_relaxed);
  g_include_syscalls.store(0, std::memory_order_relaxed);
  g_include_succeeded.store(0, std::memory_order_relaxed);
  g_include_errors.store(0, std::memory_order_relaxed);
  g_last_include_retval.store(0, std::memory_order_relaxed);
}

ScopedSlcExclusionActuatorForTesting::ScopedSlcExclusionActuatorForTesting(
    SlcExclusionActuatorFn fn, void* arg) {
  absl::base_internal::SpinLockHolder lock(g_slc_init_lock);
  prev_state_ = g_backend_state.load(std::memory_order_relaxed);
  prev_fn_ = g_test_actuator.load(std::memory_order_relaxed);
  prev_arg_ = g_test_actuator_arg.load(std::memory_order_relaxed);
  ResetSlcExclusionStatsForTesting();
  g_test_actuator_arg.store(arg, std::memory_order_relaxed);
  g_test_actuator.store(fn, std::memory_order_release);
  g_backend_state.store(
      fn != nullptr ? SlcExclusionBackendState::kTestOverride : prev_state_,
      std::memory_order_release);
}

ScopedSlcExclusionActuatorForTesting::~ScopedSlcExclusionActuatorForTesting() {
  absl::base_internal::SpinLockHolder lock(g_slc_init_lock);
  g_test_actuator.store(prev_fn_, std::memory_order_release);
  g_test_actuator_arg.store(prev_arg_, std::memory_order_relaxed);
  g_backend_state.store(prev_state_, std::memory_order_release);
  ResetSlcExclusionStatsForTesting();
}

ScopedSlcSandboxStateForTesting::ScopedSlcSandboxStateForTesting(
    bool force_sandboxed) {
  absl::base_internal::SpinLockHolder lock(g_slc_init_lock);
  prev_state_ = g_backend_state.load(std::memory_order_relaxed);
  prev_open_errno_ = g_open_errno.load(std::memory_order_relaxed);
  prev_force_sandboxed_ =
      g_force_sandboxed_for_testing.load(std::memory_order_relaxed);
  ResetSlcExclusionStatsForTesting();
  g_open_errno.store(0, std::memory_order_relaxed);
  g_force_sandboxed_for_testing.store(force_sandboxed,
                                      std::memory_order_release);
  if (force_sandboxed) {
    g_backend_state.store(SlcExclusionBackendState::kUninitialized,
                          std::memory_order_release);
  }
}

ScopedSlcSandboxStateForTesting::~ScopedSlcSandboxStateForTesting() {
  absl::base_internal::SpinLockHolder lock(g_slc_init_lock);
  g_force_sandboxed_for_testing.store(prev_force_sandboxed_,
                                      std::memory_order_release);
  g_open_errno.store(prev_open_errno_, std::memory_order_relaxed);
  g_backend_state.store(prev_state_, std::memory_order_release);
  ResetSlcExclusionStatsForTesting();
}

}  // namespace tcmalloc_internal
}  // namespace tcmalloc
GOOGLE_MALLOC_SECTION_END
