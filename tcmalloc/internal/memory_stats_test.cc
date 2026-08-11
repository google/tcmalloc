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
#include <stdint.h>
#include <unistd.h>

#include <cstddef>

#include "gtest/gtest.h"
#include "absl/functional/function_ref.h"
#include "absl/strings/numbers.h"
#include "absl/strings/string_view.h"
#include "tcmalloc/internal/page_size.h"

namespace tcmalloc {
namespace tcmalloc_internal {
namespace {

bool GetMemoryStatsFromStatm(
    MemoryStats& stats,
    absl::FunctionRef<ssize_t(char* buf, size_t count)> read) {
  constexpr size_t kBufSize = 1024;
  char buf[kBufSize];
  ssize_t rc = read(buf, kBufSize);
  if (rc < 0 || rc >= static_cast<ssize_t>(kBufSize)) {
    return false;
  }

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
        stats.vss = parsed * pagesize;
        break;
      case 1:
        stats.rss = parsed * pagesize;
        break;
      case 2:
        stats.shared = parsed * pagesize;
        break;
      case 3:
        stats.code = parsed * pagesize;
        break;
      case 5:
        stats.data = parsed * pagesize;
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

TEST(Stats, ValidRanges) {
  MemoryStats stats;
#if defined(__linux__)
  ASSERT_TRUE(GetMemoryStats(stats));
#else
  ASSERT_FALSE(GetMemoryStats(stats));
  return;
#endif

  EXPECT_GT(stats.vss, 0);
  EXPECT_GE(stats.vss, stats.rss);
  EXPECT_GT(stats.rss, 0);
  EXPECT_GE(stats.rss, stats.shared);
  EXPECT_GT(stats.shared, 0);
  EXPECT_GE(stats.vss, stats.code + stats.data);
  EXPECT_GT(stats.code, 0);
  EXPECT_GT(stats.data, 0);
  EXPECT_GT(stats.vmpte, 0);
}

#ifdef __linux__
TEST(Stats, DoubleCheckWithStatm) {
  MemoryStats status_stats;
  ASSERT_TRUE(GetMemoryStats(status_stats));

  MemoryStats statm_stats;
  int fd = open("/proc/self/statm", O_RDONLY | O_CLOEXEC);
  ASSERT_GE(fd, 0);
  ASSERT_TRUE(GetMemoryStatsFromStatm(
      statm_stats,
      [&](char* buf, size_t count) { return read(fd, buf, count); }));
  close(fd);

  const int64_t kMargin = 2 << 20;
  EXPECT_GT(status_stats.vss, statm_stats.vss - kMargin);
  EXPECT_LT(status_stats.vss, statm_stats.vss + kMargin);

  EXPECT_GT(status_stats.rss, statm_stats.rss - kMargin);
  EXPECT_LT(status_stats.rss, statm_stats.rss + kMargin);

  EXPECT_GT(status_stats.shared, statm_stats.shared - kMargin);
  EXPECT_LT(status_stats.shared, statm_stats.shared + kMargin);

  EXPECT_GT(status_stats.code, statm_stats.code - kMargin);
  EXPECT_LT(status_stats.code, statm_stats.code + kMargin);

  EXPECT_GT(status_stats.data, statm_stats.data - kMargin);
  EXPECT_LT(status_stats.data, statm_stats.data + kMargin);
}
#endif  // __linux__

TEST(Stats, HugepageFragmentationRatio) {
  auto ratio = GetHugepageFragmentationRatio(0);
#if defined(__linux__)
  if (ratio.has_value()) {
    EXPECT_GE(*ratio, 0.0);
    EXPECT_LE(*ratio, 1.0);
  }
#else
  EXPECT_FALSE(ratio.has_value());
#endif
}

}  // namespace
}  // namespace tcmalloc_internal
}  // namespace tcmalloc
