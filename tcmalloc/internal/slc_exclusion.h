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

#ifndef TCMALLOC_INTERNAL_SLC_EXCLUSION_H_
#define TCMALLOC_INTERNAL_SLC_EXCLUSION_H_

#include <cstddef>
#include <cstdint>

#include "absl/strings/string_view.h"
#include "tcmalloc/internal/config.h"

GOOGLE_MALLOC_SECTION_BEGIN
namespace tcmalloc {
namespace tcmalloc_internal {

enum class SlcExclusionBackendState : uint8_t {
  kUnsupportedArch = 0,
  kUninitialized = 1,
  kActive = 2,
  kPinnedProgUnavailable = 3,
  kKernelUnsupported = 4,
  kTestOverride = 5,
  kSandboxed = 6,
};

[[nodiscard]] absl::string_view SlcExclusionBackendStateToString(
    SlcExclusionBackendState state);

struct SlcExclusionStats {
  SlcExclusionBackendState backend_state =
      SlcExclusionBackendState::kUnsupportedArch;
  int64_t prog_fd = -1;
  int64_t open_errno = 0;
  int64_t exclude_syscalls = 0;
  int64_t exclude_succeeded = 0;
  int64_t exclude_errors = 0;
  int64_t last_exclude_retval = 0;
  int64_t include_syscalls = 0;
  int64_t include_succeeded = 0;
  int64_t include_errors = 0;
  int64_t last_include_retval = 0;
};

// Fast lock-free check returning whether ARM64 eBPF SLC exclusion may succeed
// on this host. Returns false on non-ARM64 hosts, when running sandboxed, and
// after sticky discovery that the pinned BPF program or kernel PBHA helper is
// unavailable.
[[nodiscard]] bool SlcExclusionAvailable();

// Excludes or re-includes a 2 MiB-aligned virtual address range of 2 MiB
// multiples from the ARM64 System-Level Cache (SLC) via the pinned ebpfd
// `slc_policy_run` program (`BPF_PROG_TEST_RUN`).
//
// Returns 0 on success or a negative errno value on failure. Must be called
// without holding `pageheap_lock`, as the kernel walks page tables and flushes
// TLBs under `mmap_write_lock`.
[[nodiscard]] int32_t SlcExcludeRange(void* ptr, size_t size);
[[nodiscard]] int32_t SlcIncludeRange(void* ptr, size_t size);

[[nodiscard]] SlcExclusionStats GetSlcExclusionStats();

using SlcExclusionActuatorFn = int32_t (*)(void* ptr, size_t size, bool exclude,
                                           void* arg);

// Installs an in-process actuator hook for unit tests (works on all
// architectures) and resets counters on entry and exit.
class ScopedSlcExclusionActuatorForTesting {
 public:
  ScopedSlcExclusionActuatorForTesting(SlcExclusionActuatorFn fn, void* arg);
  ~ScopedSlcExclusionActuatorForTesting();

  ScopedSlcExclusionActuatorForTesting(
      const ScopedSlcExclusionActuatorForTesting&) = delete;
  ScopedSlcExclusionActuatorForTesting& operator=(
      const ScopedSlcExclusionActuatorForTesting&) = delete;

 private:
  SlcExclusionBackendState prev_state_;
  SlcExclusionActuatorFn prev_fn_;
  void* prev_arg_;
};

// Forces seccomp/sandbox2 detection for unit tests (works on all architectures)
// and resets counters on entry and exit.
class ScopedSlcSandboxStateForTesting {
 public:
  explicit ScopedSlcSandboxStateForTesting(bool force_sandboxed);
  ~ScopedSlcSandboxStateForTesting();

  ScopedSlcSandboxStateForTesting(const ScopedSlcSandboxStateForTesting&) =
      delete;
  ScopedSlcSandboxStateForTesting& operator=(
      const ScopedSlcSandboxStateForTesting&) = delete;

 private:
  SlcExclusionBackendState prev_state_;
  int prev_open_errno_;
  bool prev_force_sandboxed_;
};

void ResetSlcExclusionStatsForTesting();

}  // namespace tcmalloc_internal
}  // namespace tcmalloc
GOOGLE_MALLOC_SECTION_END

#endif  // TCMALLOC_INTERNAL_SLC_EXCLUSION_H_
