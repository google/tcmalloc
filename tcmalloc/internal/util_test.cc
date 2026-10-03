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

#include "tcmalloc/internal/util.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

#include <atomic>
#include <string>

#include "gtest/gtest.h"
#include "absl/flags/flag.h"
#include "absl/log/check.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/string_view.h"
#include "absl/time/clock.h"
#include "absl/time/time.h"
#include "absl/types/span.h"
#include "tcmalloc/internal/page_size.h"

ABSL_FLAG(int32_t, num_ping_pongs, 20,
          "Number of safe read/write iterations to attempt");

// This is the interval at which we will deliver signals during read/write
// tests. It should be large enough that scheduling effects cannot perturb the
// test.
ABSL_FLAG(absl::Duration, signal_interval, absl::Milliseconds(5),
          "Interval between signals");

namespace tcmalloc {
namespace tcmalloc_internal {
namespace {

// Internal variables and helpers to deal with signals

volatile int signal_iterations;
volatile int pipe_fd[2];
volatile int pipe_reads;
volatile int pipe_writes;
struct itimerval signal_interval;

const char* kPipeToken = "GOOGgoogGOOGgoogGOOGgoogGOOGgoog";
int kPipeTokensPerPage;

enum signal_modes {
  kInactive = 0,
  kPipeWrite,
  kPipeRead,
  kInterruptOnly,
};
std::atomic<signal_modes> g_signal_mode;
signal_modes get_signal_mode() {
  return g_signal_mode.load(std::memory_order_acquire);
}
void set_signal_mode(signal_modes new_mode) {
  g_signal_mode.store(new_mode, std::memory_order_release);
}

void __ProgramSignalCallBackMode(enum signal_modes new_mode,
                                 absl::Duration interval) {
  struct timeval timeval = absl::ToTimeval(interval);

  signal_interval.it_value = timeval;
  signal_interval.it_interval = timeval;

  setitimer(ITIMER_REAL, &signal_interval, nullptr);
  set_signal_mode(new_mode);
}

// Disable the current SIG_ALRM call-back (if any)
void DisableSignalCallBack() {
  if (get_signal_mode() == kInactive) return;

  __ProgramSignalCallBackMode(kInactive, absl::ZeroDuration());
}

// Programs a SIG_ALRM to be repeated with a period of usec_interval
void SetSignalCallBackMode(enum signal_modes new_mode, int iterations,
                           absl::Duration interval) {
  DisableSignalCallBack();
  signal_iterations = iterations;
  __ProgramSignalCallBackMode(new_mode, interval);
}

bool EveryOther() {
  int i = signal_iterations;
  if (i == 0) return false;
  --signal_iterations;
  return (i % 2) == 0;
}

void sa_alrm(int sig) {
  int saved_errno = errno;
  signal_modes signal_mode = get_signal_mode();
  switch (signal_mode) {
    case kPipeWrite:  // write one token
      if (EveryOther()) {
        pipe_writes++;
        ABSL_RAW_CHECK(write(pipe_fd[1], kPipeToken, strlen(kPipeToken)) ==
                           strlen(kPipeToken),
                       "Write to pipe failed");
      }
      break;
    case kPipeRead:  // read one page of tokens
      if (EveryOther()) {
        char buf[128];  // must be larger than kPipeToken
        pipe_reads++;

        for (int i = 0; i < kPipeTokensPerPage; i++) {
          ABSL_RAW_CHECK(
              read(pipe_fd[0], buf, strlen(kPipeToken)) == strlen(kPipeToken),
              "Token read from pipe is too short");
          ABSL_RAW_CHECK(strncmp(buf, kPipeToken, strlen(kPipeToken)) == 0,
                         "Read non-matching token from pipe");
        }
      }
      break;
    case kInterruptOnly:
      break;
    case kInactive:
      // If we reach here already in a disabled state then our signal handler
      // has lost synchronization with the main loop.
      ABSL_RAW_LOG(FATAL, "invalid signal_mode state");
      break;
  }
  errno = saved_errno;
}

// Start of test implementation
//
class UtilInternalTest : public ::testing::Test {
 protected:
  static void SetUpTestSuite() {
    struct sigaction sig;

    // Set up signal handler
    memset(&sig, 0, sizeof(sig));  // sa_flags == 0 => SA_RESTART not set
    sig.sa_handler = sa_alrm;
    CHECK_EQ(sigaction(SIGALRM, &sig, nullptr), 0);  // install signal handler
    set_signal_mode(kInactive);

    // Pipe blocking is about the page boundary so this must fit evenly
    kPipeTokensPerPage = GetPageSize() / strlen(kPipeToken);
    CHECK_EQ(GetPageSize() % strlen(kPipeToken), 0);
  }

  static void TearDownTestSuite() { signal(SIGALRM, SIG_DFL); }

  void SetUp() override {
    // Unblock SIGALRM.
    sigset_t unblock_signals;
    CHECK_EQ(sigemptyset(&unblock_signals), 0);
    CHECK_EQ(sigaddset(&unblock_signals, SIGALRM), 0);
    CHECK_EQ(sigprocmask(SIG_UNBLOCK, &unblock_signals, &blocked_signals_), 0);

    CHECK_EQ(pipe(const_cast<int*>(pipe_fd)), 0);
    signal_modes signal_mode = get_signal_mode();
    CHECK_EQ(signal_mode, kInactive);
  }

  void TearDown() override {
    close(pipe_fd[0]);
    close(pipe_fd[1]);

    DisableSignalCallBack();

    CHECK_EQ(sigprocmask(SIG_SETMASK, &blocked_signals_, nullptr), 0);
  }

 private:
  // Blocked signals at SetUp, SIGALRM will be removed.
  sigset_t blocked_signals_;
};

// Test that open/close work, there's not a nice way to make these block so we
// do this by brute force.  Ensuring they work consistently over many iterations
// while we're being barraged by signals.
TEST_F(UtilInternalTest, open_close) {
  int fd;
  const std::string tmp_file = ::testing::TempDir() + "/openclose.dat";

  // We need to able to pass the mode, so just use creat(2) here
  CHECK_GT(fd = creat(tmp_file.c_str(), S_IRWXU), 0);
  CHECK_EQ(close(fd), 0);

  SetSignalCallBackMode(kInterruptOnly, 10000000, absl::Milliseconds(1));
  absl::Time end = absl::Now() + absl::Milliseconds(200);
  do {
    for (int i = 0; i < 100; i++) {
      ASSERT_GT(
          fd = signal_safe_open(tmp_file.c_str(), O_WRONLY | O_CREAT | O_TRUNC,
                                S_IRUSR | S_IWUSR),
          0);

      ASSERT_EQ(signal_safe_close(fd), 0);
    }
  } while (absl::Now() < end);
  DisableSignalCallBack();
}

// Test that signal_safe_poll ignores signals.
TEST_F(UtilInternalTest, poll) {
  struct pollfd pfd;
  constexpr absl::Duration timeout = absl::Milliseconds(200);
  absl::Time start, end;
  pfd.fd = pipe_fd[0];
  pfd.events = POLL_IN;
  pfd.revents = 0;

  SetSignalCallBackMode(kInterruptOnly, 10000000, absl::Milliseconds(1));
  start = absl::Now();
  // Ensure we are not interrupted
  ASSERT_EQ(signal_safe_poll(&pfd, 1, timeout), 0);
  end = absl::Now();
  EXPECT_GT(end - start, timeout);  // Validate timeout
  CHECK_EQ(
      signal_safe_write(pipe_fd[1], kPipeToken, strlen(kPipeToken), nullptr),
      strlen(kPipeToken));
  ASSERT_EQ(signal_safe_poll(&pfd, 1, timeout), 1);
}

// Test that signal_safe_read is never interrupted by performing blocking reads
// against a pipe.  The signal handler will write into the pipe every other time
// it fires, if signal_safe_was not interrupted by a signal that *did not* write
// then reads should equal writes after a chosen number of iterations.
TEST_F(UtilInternalTest, signal_safe_read) {
  char buf[128];

  pipe_reads = pipe_writes = 0;
  SetSignalCallBackMode(kPipeWrite, absl::GetFlag(FLAGS_num_ping_pongs) * 2,
                        absl::GetFlag(FLAGS_signal_interval));
  while (pipe_reads < absl::GetFlag(FLAGS_num_ping_pongs)) {
    size_t bytes_read;
    ssize_t rc;
    buf[0] = '\0';  // signal_safe_read will over-write this
    rc = signal_safe_read(pipe_fd[0], buf, strlen(kPipeToken), &bytes_read);
    ASSERT_EQ(rc, strlen(kPipeToken));
    ASSERT_EQ(rc, bytes_read);
    ASSERT_EQ(strncmp(kPipeToken, buf, strlen(kPipeToken)), 0);
    pipe_reads++;
  }
  ASSERT_EQ(pipe_reads, pipe_writes);
  DisableSignalCallBack();
}

std::string CreateTempFile(absl::string_view contents) {
  std::string tempname = testing::TempDir();
  tempname.append(".XXXXXX");

  int fd = mkstemp(&tempname[0]);
  auto ret = write(fd, contents.data(), contents.size());
  CHECK_EQ(ret, contents.size());
  close(fd);

  return tempname;
}

// Test interaction of EINTR handling with EOF.
TEST_F(UtilInternalTest, read_interrupt_eof) {
  static const int kBufLen = 128;
  static char buf[kBufLen];
  static constexpr absl::string_view kTestStr = "0123456789ABCDEF";
  std::string fname = CreateTempFile(kTestStr);

  int fd = signal_safe_open(fname.c_str(), O_RDONLY);
  PCHECK(fd >= 0);
  int ret = signal_safe_read(fd, buf, sizeof(buf), nullptr);
  ASSERT_EQ(ret, kTestStr.size());

  // fake coming into EOF with EINTR around--this should just happily
  // return 0
  errno = EINTR;
  EXPECT_EQ(signal_safe_read(fd, buf, sizeof(buf), nullptr), 0);

  signal_safe_close(fd);
}

// Test that signal_safe_read is never interrupted by performing blocking write
// against a pipe.  This is very similar to the read test above except since the
// blocking boundary is a page we have to read/write a full page of tokens in
// each iteration.
TEST_F(UtilInternalTest, signal_safe_write) {
  int fd_flags, rc;

  signal_modes signal_mode = get_signal_mode();
  CHECK_EQ(signal_mode, kInactive);

  // first fill up the pipe
  fd_flags = fcntl(pipe_fd[1], F_GETFL);
  CHECK_EQ(fcntl(pipe_fd[1], F_SETFL, fd_flags | O_NONBLOCK), 0);
  do {
    rc = write(pipe_fd[1], kPipeToken, strlen(kPipeToken));
  } while ((rc > 0) || (rc == -1 && errno == EINTR));
  CHECK(errno == EAGAIN || errno == EINVAL);  // POSIX allows for either.
  CHECK_EQ(fcntl(pipe_fd[1], F_SETFL, fd_flags & ~O_NONBLOCK), 0);

  pipe_reads = pipe_writes = 0;
  SetSignalCallBackMode(kPipeRead, absl::GetFlag(FLAGS_num_ping_pongs) * 2,
                        absl::GetFlag(FLAGS_signal_interval));

  while (pipe_writes < absl::GetFlag(FLAGS_num_ping_pongs)) {
    size_t bytes_written;
    ssize_t rc;
    // write a full page so that we block again
    for (int i = 0; i < kPipeTokensPerPage; i++) {
      rc = signal_safe_write(pipe_fd[1], kPipeToken, strlen(kPipeToken),
                             &bytes_written);
      ASSERT_EQ(rc, strlen(kPipeToken));
      ASSERT_EQ(rc, bytes_written);
    }
    pipe_writes++;
  }
  ASSERT_EQ(pipe_reads, pipe_writes);
  DisableSignalCallBack();

  CHECK_EQ(fcntl(pipe_fd[1], F_SETFL, fd_flags), 0);
}

TEST(UtilTest, SafeCopyMemory) {
  // Test valid memory copy.
  const std::string src(128, 'A');
  char dst[128];

  EXPECT_TRUE(SafeCopyMemory(src.data(), dst, src.size()));
  EXPECT_EQ(absl::string_view(dst, sizeof(dst)), src);

  EXPECT_FALSE(SafeCopyMemory(src.data(), dst, 0));

#if defined(__linux__)
  // Test reading from PROT_NONE unreadable memory.
  const size_t page_size = GetPageSize();
  void* prot_none =
      mmap(nullptr, page_size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (prot_none != MAP_FAILED) {
    EXPECT_FALSE(SafeCopyMemory(prot_none, dst, 16));
    munmap(prot_none, page_size);
  }

  // Test reading from an invalid pointer address (unmapped address).
  void* invalid_ptr = reinterpret_cast<void*>(0x100);
  EXPECT_FALSE(SafeCopyMemory(invalid_ptr, dst, 16));
#endif  // defined(__linux__)
}

TEST(UtilTest, SafeCopyMemoryMultipleChunks) {
  const std::string chunk1(64, '1');
  const std::string chunk2(64, '2');
  const std::string chunk3(64, '3');
  char dst[192] = {};

  const absl::string_view chunks[] = {chunk1, chunk2, chunk3};

  EXPECT_TRUE(SafeCopyMemory(absl::MakeConstSpan(chunks), dst));
  EXPECT_EQ(absl::string_view(dst, sizeof(dst)),
            absl::StrCat(chunk1, chunk2, chunk3));

  // Empty chunks
  EXPECT_FALSE(SafeCopyMemory(absl::Span<const absl::string_view>{}, dst));
}

}  // namespace
}  // namespace tcmalloc_internal
}  // namespace tcmalloc

int main(int argc, char** argv) {
  // Add SIGALRM to blocked signals so that threads inherit and we can unblock
  // on the main test thread. This avoids data races on variables shared between
  // the test and the signal handler.
  sigset_t blocked_signals;
  CHECK_EQ(sigemptyset(&blocked_signals), 0);
  CHECK_EQ(sigaddset(&blocked_signals, SIGALRM), 0);
  CHECK_EQ(sigprocmask(SIG_BLOCK, &blocked_signals, nullptr), 0);
  // Init and run tests.
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
