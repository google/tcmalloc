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

#include "tcmalloc/internal/memory_stats.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <cstddef>
#include <cstdint>
#include <optional>

#include "absl/functional/function_ref.h"
#include "absl/strings/numbers.h"
#include "absl/strings/string_view.h"
#include "tcmalloc/internal/config.h"
#include "tcmalloc/internal/logging.h"
#include "tcmalloc/internal/page_size.h"
#include "tcmalloc/internal/util.h"

GOOGLE_MALLOC_SECTION_BEGIN
namespace tcmalloc {
namespace tcmalloc_internal {

namespace {

struct FDCloser {
  FDCloser() : fd(-1) {}
  ~FDCloser() {
    if (fd != -1) {
      signal_safe_close(fd);
    }
  }
  int fd;
};

}  // namespace

std::optional<int64_t> GetPSS() {
  FDCloser fd;
  fd.fd = signal_safe_open("/proc/self/smaps_rollup", O_RDONLY | O_CLOEXEC);
  if (fd.fd < 0) {
    return std::nullopt;
  }

  return ParseSmapsRollup([&](char* buf, size_t count) {
    return signal_safe_read(fd.fd, buf, count, nullptr);
  });
}

std::optional<int64_t> ParseSmapsRollup(
    absl::FunctionRef<ssize_t(char*, size_t)> read) {
  char buf[4096];
  ssize_t rc = read(buf, sizeof(buf) - 1);
  if (rc < 0) {
    return std::nullopt;
  }
  buf[rc] = '\0';

  absl::string_view contents(buf, rc);
  // Find "Pss:"
  // It is usually at the start of a line.
  size_t pos = contents.find("Pss:");
  if (pos == absl::string_view::npos) {
    return std::nullopt;
  }

  // Skip "Pss:"
  pos += 4;

  // Skip whitespace
  while (pos < contents.size() && contents[pos] == ' ') {
    pos++;
  }

  // Parse number
  // Find end of number
  size_t end = pos;
  while (end < contents.size() && contents[end] >= '0' &&
         contents[end] <= '9') {
    end++;
  }

  absl::string_view value_str = contents.substr(pos, end - pos);
  int64_t pss_kb = 0;
  if (!absl::SimpleAtoi(value_str, &pss_kb)) {
    return std::nullopt;
  }

  return pss_kb * 1024;
}

bool ParseStatm(absl::FunctionRef<ssize_t(char*, size_t)> read,
                MemoryStats* stats) {
  char buf[1024];
  ssize_t rc = read(buf, sizeof(buf));
  if (rc < 0 || rc >= static_cast<ssize_t>(sizeof(buf))) {
    return false;
  }
  buf[rc] = '\0';

  const size_t pagesize = GetPageSize();
  absl::string_view contents(buf, rc);
  absl::string_view::size_type start = 0;
  int index = 0;
  do {
    auto end = contents.find(' ', start);

    absl::string_view value;
    if (end == absl::string_view::npos) {
      value = contents.substr(start);
    } else {
      value = contents.substr(start, end - start);
    }

    int64_t parsed;
    if (!absl::SimpleAtoi(value, &parsed)) {
      return false;
    }

    // Fields in /proc/self/statm:
    //  [0] = vss
    //  [1] = rss
    //  [2] = shared
    //  [3] = code
    //  [4] = unused
    //  [5] = data + stack
    //  [6] = unused
    switch (index) {
      case 0:
        stats->vss = parsed * pagesize;
        break;
      case 1:
        stats->rss = parsed * pagesize;
        break;
      case 2:
        stats->shared = parsed * pagesize;
        break;
      case 3:
        stats->code = parsed * pagesize;
        break;
      case 5:
        stats->data = parsed * pagesize;
        break;
      case 4:
      case 6:
      default:
        // Unused
        break;
    }

    if (end == absl::string_view::npos) {
      break;
    }

    start = end + 1;
  } while (start < contents.size() && index++ < 6);

  if (index < 6) {
    return false;
  }

  return true;
}

bool GetMemoryStats(MemoryStats* stats) {
#if !defined(__linux__)
  return false;
#endif

  FDCloser fd;
  fd.fd = signal_safe_open("/proc/self/statm", O_RDONLY | O_CLOEXEC);
  TC_ASSERT_GE(fd.fd, 0);
  if (fd.fd < 0) {
    return false;
  }

  if (!ParseStatm(
          [&](char* buf, size_t count) {
            return signal_safe_read(fd.fd, buf, count, nullptr);
          },
          stats)) {
    return false;
  }

  return true;
}

}  // namespace tcmalloc_internal
}  // namespace tcmalloc
GOOGLE_MALLOC_SECTION_END
