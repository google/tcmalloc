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
#include <limits.h>
#include <stdio.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>

#include "absl/base/optimization.h"
#include "absl/functional/function_ref.h"
#include "absl/strings/ascii.h"
#include "absl/strings/match.h"
#include "absl/strings/numbers.h"
#include "absl/strings/string_view.h"
#include "tcmalloc/internal/config.h"
#include "tcmalloc/internal/logging.h"
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

bool GetMemoryStatsFromStatus(
    MemoryStats& stats,
    absl::FunctionRef<ssize_t(char* buf, size_t count)> read) {
  char buf[MemoryStats::kBufferSize];
  size_t buf_len = 0;

  // Bitwise tracker for which fields we've seen to ensure stats is fully
  // populated.
  size_t seen = 0;
  while (true) {
    ssize_t rc = read(buf + buf_len, MemoryStats::kBufferSize - buf_len);
    if (rc <= 0) break;
    buf_len += rc;

    absl::string_view contents(buf, buf_len);
    while (!contents.empty()) {
      auto nl = contents.find('\n');
      if (nl == absl::string_view::npos) {
        break;  // Partial line at end of chunk, wait for next read
      }

      absl::string_view line = contents.substr(0, nl);
      contents = contents.substr(nl + 1);

      auto colon = line.find(':');
      if (colon == absl::string_view::npos) {
        continue;
      }

      absl::string_view key = line.substr(0, colon);
      absl::string_view rest =
          absl::StripAsciiWhitespace(line.substr(colon + 1));

      constexpr absl::string_view kb = " kB";
      const bool is_kb = absl::EndsWith(rest, kb);
      if (is_kb) {
        rest = rest.substr(0, rest.size() - kb.size());
      }

      int64_t val;
      if (!absl::SimpleAtoi(rest, &val)) {
        continue;
      }

      if (is_kb) {
        val *= 1024;
      }
      if (key == "VmSize") {
        stats.vss = val;
        seen |= 0x1;
      } else if (key == "VmRSS") {
        stats.rss = val;
        seen |= 0x2;
      } else if (key == "RssFile" || key == "RssShmem") {
        stats.shared += val;
        seen |= 0x4;
      } else if (key == "VmExe") {
        stats.code = val;
        seen |= 0x8;
      } else if (key == "VmData" || key == "VmStk") {
        stats.data += val;
        seen |= 0x10;
      } else if (key == "VmPTE") {
        stats.vmpte = val;
        seen |= 0x20;
      }
    }

    // Slide remainder back to 0.
    buf_len = contents.size();
    if (ABSL_PREDICT_FALSE(buf_len == MemoryStats::kBufferSize)) {
      // A single string without a newline exceeds the entire buffer. Discard
      // it.
      buf_len = 0;
    } else {
      memmove(buf, contents.data(), buf_len);
    }
  }

  return seen == 0x3f;
}

bool GetMemoryStats(MemoryStats& stats) {
#if !defined(__linux__)
  return false;
#endif

  FDCloser fd;
  fd.fd = signal_safe_open("/proc/self/status", O_RDONLY | O_CLOEXEC);
  TC_ASSERT_GE(fd.fd, 0);
  if (fd.fd < 0) {
    return false;
  }

  return GetMemoryStatsFromStatus(stats, [&](char* buf, size_t count) {
    return signal_safe_read(fd.fd, buf, count, nullptr);
  });
}

std::optional<double> GetHugepageFragmentationRatio(size_t node) {
#if !defined(__linux__)
  return std::nullopt;
#endif

  char path[PATH_MAX];
  snprintf(path, sizeof(path),
           "/sys/devices/system/node/node%zu/hugepage_fragmentation_ratio",
           node);

  int fd = signal_safe_open(path, O_RDONLY | O_CLOEXEC);
  if (fd < 0) {
    TC_CHECK_EQ(errno, ENOENT);
    return std::nullopt;
  }

  std::array<char, 16> buf;
  ssize_t rc = signal_safe_read(fd, buf.data(), buf.size(), nullptr);
  signal_safe_close(fd);

  if (ABSL_PREDICT_FALSE(rc <= 0)) {
    return std::nullopt;
  }

  absl::string_view str(buf.data(), rc);
  double ratio;
  if (!absl::SimpleAtod(str, &ratio)) {
    return std::nullopt;
  }

  return ratio;
}

}  // namespace tcmalloc_internal
}  // namespace tcmalloc
GOOGLE_MALLOC_SECTION_END
