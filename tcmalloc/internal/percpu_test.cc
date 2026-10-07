// Copyright 2024 The TCMalloc Authors
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

#include "tcmalloc/internal/percpu.h"

#include <signal.h>
#include <sys/time.h>
#include <ucontext.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstring>

#include "gtest/gtest.h"
#include "absl/base/attributes.h"
#include "absl/time/time.h"
#include "tcmalloc/internal/logging.h"
#include "tcmalloc/testing/testutil.h"

namespace tcmalloc::tcmalloc_internal::subtle::percpu {
namespace {

// percpu.h is the first include of this TU, so the RSEQ gate must be
// evaluated without relying on macros provided by later includes.
#if defined(ABSL_HAVE_HWADDRESS_SANITIZER)
static_assert(TCMALLOC_PERCPU_RSEQ_SUPPORTED_PLATFORM == 0,
              "RSEQ must be disabled under HWASan");
static_assert(TCMALLOC_INTERNAL_PERCPU_USE_RSEQ == 0,
              "RSEQ must be disabled under HWASan");
#endif

ABSL_CONST_INIT std::atomic<int> alarms{0};
ABSL_CONST_INIT std::atomic<int> iterations{0};
ABSL_CONST_INIT std::atomic<int> last_alarm_iteration{-1};
ABSL_CONST_INIT std::atomic<int> same_iteration_alarms{0};
constexpr int kMaxSameIterationAlarms = 1;
ABSL_CONST_INIT std::atomic<bool> starved{false};

void SetTimer(absl::Duration interval) {
  const struct timeval timeval = absl::ToTimeval(interval);
  struct itimerval signal_interval;
  signal_interval.it_value = timeval;
  signal_interval.it_interval = timeval;
  setitimer(ITIMER_REAL, &signal_interval, nullptr);
}

void sa_alrm(int sig, siginfo_t* info, void* ucontext) {
  const int saved_errno = errno;
  alarms.fetch_add(1, std::memory_order_relaxed);
  TC_CHECK(IsFast());

  const int iter = iterations.load(std::memory_order_relaxed);
  if (iter >= 0 &&
      last_alarm_iteration.exchange(iter, std::memory_order_relaxed) == iter) {
    if (same_iteration_alarms.fetch_add(1, std::memory_order_relaxed) >=
        kMaxSameIterationAlarms) {
      same_iteration_alarms.store(0, std::memory_order_relaxed);
      starved.store(true, std::memory_order_relaxed);
      sigaddset(&static_cast<ucontext_t*>(ucontext)->uc_sigmask, SIGALRM);
    }
  } else {
    same_iteration_alarms.store(0, std::memory_order_relaxed);
  }
  errno = saved_errno;
}

TEST(PerCpu, SignalHandling) {
  if (!IsFast()) {
    GTEST_SKIP() << "per-CPU unavailable";
  }

  alarms.store(0, std::memory_order_relaxed);
  iterations.store(0, std::memory_order_relaxed);
  last_alarm_iteration.store(-1, std::memory_order_relaxed);
  same_iteration_alarms.store(0, std::memory_order_relaxed);
  starved.store(false, std::memory_order_relaxed);

  struct sigaction sig;
  memset(&sig, 0, sizeof(sig));  // SA_RESTART not set
  sig.sa_sigaction = sa_alrm;
  sig.sa_flags = SA_SIGINFO;
  struct sigaction old_sig;
  ASSERT_EQ(sigaction(SIGALRM, &sig, &old_sig), 0);  // install signal handler

  absl::Duration interval = absl::Microseconds(1);
  SetTimer(interval);

  sigset_t alrm_set;
  sigemptyset(&alrm_set);
  sigaddset(&alrm_set, SIGALRM);

  for (int i = 0; i < 15000; ++i) {
    iterations.store(i, std::memory_order_relaxed);
    if (starved.load(std::memory_order_relaxed)) {
      starved.store(false, std::memory_order_relaxed);
      interval = std::min(interval * 2, absl::Milliseconds(10));
      SetTimer(interval);
      ASSERT_EQ(sigprocmask(SIG_UNBLOCK, &alrm_set, nullptr), 0);
    }
    UnregisterRseq();
    TC_CHECK(IsFast());
  }
  iterations.store(-1, std::memory_order_relaxed);

  SetTimer(absl::ZeroDuration());
  ASSERT_EQ(sigprocmask(SIG_UNBLOCK, &alrm_set, nullptr), 0);
  ASSERT_EQ(sigaction(SIGALRM, &old_sig, nullptr), 0);

  EXPECT_GT(alarms.load(std::memory_order_relaxed), 0);
}

TEST(PerCpu, UnregisteredThread) {
  if (!IsFast()) {
    GTEST_SKIP() << "per-CPU unavailable";
  }

  UnregisterRseq();
  ASSERT_FALSE(IsFastNoInit());
  (void)UsingRseqVirtualCpus();
  EXPECT_FALSE(IsFastNoInit());
#if TCMALLOC_INTERNAL_PERCPU_USE_RSEQ
  EXPECT_EQ(__rseq_abi.mm_cid, 0u);
#endif
  EXPECT_GE(VirtualCpu::Synchronize(), 0);
  EXPECT_TRUE(IsFastNoInit());
}

}  // namespace
}  // namespace tcmalloc::tcmalloc_internal::subtle::percpu
