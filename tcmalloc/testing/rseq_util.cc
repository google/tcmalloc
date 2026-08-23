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

#include "tcmalloc/testing/rseq_util.h"

#include <features.h>

#include <cerrno>
#include <cstring>
#ifdef __GLIBC__
#include <gnu/libc-version.h>
#endif  // __GLIBC__
#include <stdio.h>

#include "gtest/gtest.h"
#include "tcmalloc/internal/environment.h"
#include "tcmalloc/internal/percpu.h"
#include "tcmalloc/testing/testutil.h"

namespace tcmalloc::testing {

bool ExpectWorkingRseq() {
#ifdef __GLIBC__
  const char* libc_version = gnu_get_libc_version();
  int major, minor;
  if (sscanf(libc_version, "%d.%d", &major, &minor) == 2) {
    // TODO(b/245776120): Implement support.
    if (major > 2 || (major == 2 && minor >= 35)) {
      const char* value =
          tcmalloc_internal::thread_safe_getenv("GLIBC_TUNABLES");
      if (value == nullptr ||
          strstr(value, "glibc.pthread.rseq=0") == nullptr) {
        return false;
      }
    }
  }
#endif
#if TCMALLOC_INTERNAL_PERCPU_USE_RSEQ && defined(__NR_rseq)
  if (syscall(__NR_rseq, nullptr, 0, 0, TCMALLOC_PERCPU_RSEQ_SIGNATURE) != 0) {
    switch (errno) {
      case EBUSY:
      case EINVAL:
        return true;
      case ENOSYS:
        return false;
      default:
        return false;
    }
  }

  return true;
#else
  return false;
#endif
}

namespace {

TEST(RestartableSequences, HasWorkingRseq) {
  const bool expect_fast = ExpectWorkingRseq();

  // We record the state of IsFastNoInit so we can put things back to the way
  // they are.  In general, tests should work whether or not restartable
  // sequences has already been registered, but we avoid perturbing things.
  const bool was_fast = tcmalloc_internal::subtle::percpu::IsFastNoInit();
  const bool is_fast = tcmalloc_internal::subtle::percpu::IsFast();

  if (!was_fast && is_fast) {
    tcmalloc::UnregisterRseq();
  }

  EXPECT_EQ(is_fast, expect_fast);
}

}  // namespace
}  // namespace tcmalloc::testing
