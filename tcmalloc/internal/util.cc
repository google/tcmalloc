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
#include "tcmalloc/internal/util.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <string.h>

#if defined(__linux__)
#include <sys/uio.h>
#if defined(__GLIBC__) && \
    ((__GLIBC__ > 2) || (__GLIBC__ == 2 && __GLIBC_MINOR__ >= 15))
#define HAS_PROCESS_VM_READV 1
#endif
#endif  // defined(__linux__)
#include <unistd.h>

#include <algorithm>
#include <type_traits>

#include "absl/strings/string_view.h"
#include "absl/time/clock.h"
#include "absl/time/time.h"
#include "absl/types/span.h"
#include "tcmalloc/internal/config.h"

GOOGLE_MALLOC_SECTION_BEGIN
namespace tcmalloc {
namespace tcmalloc_internal {

int signal_safe_open(const char* path, int flags, ...) {
  int fd;
  va_list ap;
  using mode_t_va_arg_type =
      std::conditional_t<sizeof(mode_t) < sizeof(int), int, mode_t>;

  va_start(ap, flags);
  mode_t mode = va_arg(ap, mode_t_va_arg_type);
  va_end(ap);

  do {
    fd = ((flags & O_CREAT) ? open(path, flags, mode) : open(path, flags));
  } while (fd == -1 && errno == EINTR);

  return fd;
}

int signal_safe_close(int fd) {
  int rc;

  do {
    rc = close(fd);
  } while (rc == -1 && errno == EINTR);

  return rc;
}

ssize_t signal_safe_write(int fd, const char* buf, size_t count,
                          size_t* bytes_written) {
  ssize_t rc;
  size_t total_bytes = 0;

  do {
    rc = write(fd, buf + total_bytes, count - total_bytes);
    if (rc > 0) total_bytes += rc;
  } while ((rc > 0 && count > total_bytes) || (rc == -1 && errno == EINTR));

  if (bytes_written != nullptr) *bytes_written = total_bytes;

  return rc;
}

int signal_safe_poll(struct pollfd* fds, int nfds, absl::Duration timeout) {
  int rc = 0;
  absl::Duration elapsed = absl::ZeroDuration();

  // We can't use gettimeofday since it's not async signal safe.  We could use
  // clock_gettime but that would require linking //base against librt.
  // Fortunately, timeout is of sufficiently coarse granularity that we can just
  // approximate it.
  while ((elapsed <= timeout || timeout < absl::ZeroDuration()) && (rc == 0)) {
    if (elapsed > absl::ZeroDuration())
      ::absl::SleepFor(::absl::Milliseconds(1));
    elapsed += absl::Milliseconds(1);
    while ((rc = poll(fds, nfds, 0)) == -1 && errno == EINTR) {
    }
  }

  return rc;
}

ssize_t signal_safe_read(int fd, char* buf, size_t count, size_t* bytes_read) {
  ssize_t rc;
  size_t total_bytes = 0;
  struct pollfd pfd;

  // poll is required for testing whether there is any data left on fd in the
  // case of a signal interrupting a partial read.  This is needed since this
  // case is only defined to return the number of bytes read up to that point,
  // with no indication whether more could have been read (up to count).
  pfd.fd = fd;
  pfd.events = POLL_IN;
  pfd.revents = 0;

  do {
    rc = read(fd, buf + total_bytes, count - total_bytes);
    if (rc > 0) total_bytes += rc;

    if (rc == 0) break;  // EOF
    // try again if there's space to fill, no (non-interrupt) error,
    // and data is available.
  } while (total_bytes < count && (rc > 0 || errno == EINTR) &&
           (signal_safe_poll(&pfd, 1, absl::ZeroDuration()) == 1 ||
            total_bytes == 0));

  if (bytes_read) *bytes_read = total_bytes;

  if (rc != -1 || errno == EINTR)
    rc = total_bytes;  // return the cumulative bytes read
  return rc;
}

bool SafeCopyMemory(absl::Span<const absl::string_view> src_chunks, void* dst) {
  if (src_chunks.empty() || dst == nullptr) {
    return false;
  }
#if defined(HAS_PROCESS_VM_READV)
  size_t dst_offset = 0;
  constexpr size_t kMaxIov = 64;
  static_assert(kMaxIov <= IOV_MAX);
  struct iovec src_vec[kMaxIov];

  while (!src_chunks.empty()) {
    // Process in batches of up to kMaxIov chunks.
    const size_t batch_count = std::min<size_t>(src_chunks.size(), kMaxIov);
    size_t batch_bytes = 0;
    for (size_t i = 0; i < batch_count; ++i) {
      src_vec[i] =
          iovec{const_cast<char*>(src_chunks[i].data()), src_chunks[i].size()};
      batch_bytes += src_chunks[i].size();
    }
    src_chunks.remove_prefix(batch_count);

    struct iovec dst_iov;
    dst_iov.iov_base = static_cast<char*>(dst) + dst_offset;
    dst_iov.iov_len = batch_bytes;

    ssize_t bytes = process_vm_readv(getpid(), &dst_iov, /*liovcnt=*/1, src_vec,
                                     /*riovcnt=*/batch_count, /*flags=*/0);
    if (bytes != batch_bytes) {
      return false;
    }

    dst_offset += batch_bytes;
  }
  return true;
#else
  return false;
#endif  // defined(HAS_PROCESS_VM_READV)
}

bool SafeCopyMemory(const void* src, void* dst, size_t size) {
  if (size == 0) {
    return false;
  }
  const absl::string_view chunk(static_cast<const char*>(src), size);
  return SafeCopyMemory(absl::MakeConstSpan(&chunk, 1), dst);
}

}  // namespace tcmalloc_internal
}  // namespace tcmalloc
GOOGLE_MALLOC_SECTION_END
