// Copyright 2023 The TCMalloc Authors
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

#include "tcmalloc/internal/pageflags.h"

#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <ostream>
#include <string>
#include <utility>
#include <vector>

#include "benchmark/benchmark.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "absl/flags/flag.h"
#include "absl/log/check.h"
#include "absl/log/log.h"
#include "absl/random/distributions.h"
#include "absl/random/random.h"
#include "absl/status/status.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_format.h"
#include "absl/strings/string_view.h"
#include "absl/time/clock.h"
#include "absl/time/time.h"
#include "tcmalloc/internal/allocation_guard.h"
#include "tcmalloc/internal/util.h"

ABSL_FLAG(bool, check_staleness, false,
          "If true, actually wait around for memory to go stale.");

namespace tcmalloc {
namespace tcmalloc_internal {

class PageFlagsFriend {
 public:
  explicit PageFlagsFriend() = default;
  explicit PageFlagsFriend(absl::string_view filename) : r_(filename.data()) {}

  template <typename... Args>
  decltype(auto) Get(Args&&... args) {
    return r_.Get(std::forward<Args>(args)...);
  }

  decltype(auto) MaybeReadStaleScanSeconds(absl::string_view filename) {
    return r_.MaybeReadStaleScanSeconds(filename.data());
  }

  decltype(auto) CachedScanSeconds() { return r_.cached_scan_seconds_; }

  decltype(auto) IsHugepageBacked(const void* const addr) {
    return r_.IsHugepageBacked(addr);
  }

  void SetCachedScanSeconds(
      decltype(PageFlags::cached_scan_seconds_) scan_seconds) {
    r_.cached_scan_seconds_ = scan_seconds;
  }

 private:
  PageFlags r_;
};

std::ostream& operator<<(std::ostream& os, const PageStats& s) {
  return os << "{ stale = " << s.bytes_stale << ", locked = " << s.bytes_locked
            << ", stale_scan_seconds = " << s.stale_scan_seconds << "}";
}

namespace {

using ::testing::FieldsAre;
using ::testing::Optional;

constexpr uint64_t kPageHead = (1UL << 15);
constexpr uint64_t kPageTail = (1UL << 16);
constexpr uint64_t kPageThp = (1UL << 22);
constexpr uint64_t kPageHuge = (1UL << 17);
constexpr uint64_t kPageStale = (1UL << 44);

constexpr size_t kPagemapEntrySize = 8;
constexpr size_t kHugePageSize = 2 << 20;
constexpr size_t kHugePageMask = ~(kHugePageSize - 1);

// Write the given content into the given filename. Suitable only for tests.
void SetContents(absl::string_view filename, absl::string_view content) {
  int fd =
      signal_safe_open(filename.data(), O_CREAT | O_WRONLY | O_TRUNC, 0644);
  CHECK_NE(fd, -1) << errno << " while writing to " << filename;
  int written =
      signal_safe_write(fd, content.data(), content.length(), nullptr);
  CHECK_EQ(written, content.length()) << errno;
  CHECK_EQ(signal_safe_close(fd), 0) << errno;
}

TEST(PageFlagsTest, Smoke) {
  GTEST_SKIP() << "pageflags not commonly available";
  auto res = PageFlags{}.Get(nullptr, 0);
  EXPECT_THAT(res, Optional(PageStats{}));
}

TEST(PageFlagsTest, Stack) {
  GTEST_SKIP() << "pageflags not commonly available";

  char buf[256];
  std::fill(std::begin(buf), std::end(buf), 12);
  ::benchmark::DoNotOptimize(buf);

  PageFlags s;
  EXPECT_THAT(s.Get(reinterpret_cast<void*>(buf), sizeof(buf)),
              Optional(PageStats{}));
}

TEST(PageFlagsTest, Alignment) {
  GTEST_SKIP() << "pageflags not commonly available";

  const size_t kPageSize = getpagesize();
  const int kNumPages = 6 * kHugePageSize / kPageSize;
  for (auto mmap_hint : std::initializer_list<void*>{
           nullptr, reinterpret_cast<void*>(0x00007BADDE000000),
           reinterpret_cast<void*>(0x00007BADDF001000)}) {
    void* p = mmap(
        mmap_hint, kNumPages * kPageSize, PROT_READ | PROT_WRITE,
        (mmap_hint == nullptr ? 0 : MAP_FIXED) | MAP_ANONYMOUS | MAP_PRIVATE,
        -1, 0);
    ASSERT_NE(p, MAP_FAILED) << errno;
    ASSERT_EQ(madvise(p, kPageSize * kNumPages, MADV_HUGEPAGE), 0) << errno;

    PageFlags s;
    EXPECT_THAT(s.Get(p, kPageSize * kNumPages), Optional(PageStats{})) << p;
    munmap(p, kNumPages * kPageSize);
  }
}

// Write an alternate "pageflags" file comprising all stale pages at the path
// indicated by `filename`. The actual pageflags are copied from the pages that
// obj spans, but we add KPF_STALE. The alternate pageflags file starts at 0, so
// we return a pointer to `obj` in the alternate virtual memory space. If `obj`
// is page-aligned, this is a zero pointer (not to be confused with a null
// pointer).
void* GenerateAllStaleTest(absl::string_view filename, void* obj, size_t size) {
  const size_t kPageSize = getpagesize();

  uintptr_t ptr = reinterpret_cast<uintptr_t>(obj);
  uintptr_t pages_start = ptr & kHugePageMask;
  uintptr_t new_offset = ptr - pages_start;

  off_t file_read_offset = pages_start / kPageSize * kPagemapEntrySize;
  int read_fd = signal_safe_open("/proc/self/pageflags", O_RDONLY);
  CHECK_NE(read_fd, -1)
      << strerror(errno)
      << " while reading pageflags; does your kernel support it?";
  int write_fd = signal_safe_open(filename.data(), O_CREAT | O_WRONLY, S_IRUSR);
  CHECK_NE(write_fd, -1) << errno;

  CHECK_EQ(::lseek(read_fd, file_read_offset, SEEK_SET), file_read_offset);
  std::array<uint64_t, kHugePageSize / sizeof(uint64_t)> buf;
  for (int i = 0; i < size / kHugePageSize + 3; ++i) {
    CHECK_EQ(signal_safe_read(read_fd, reinterpret_cast<char*>(buf.data()),
                              kHugePageSize, nullptr),
             kHugePageSize);
    for (uint64_t& page : buf) {
      if ((page & kPageHead) == kPageHead || (page & kPageTail) != kPageTail) {
        page |= kPageStale;
      }
    }
    CHECK_EQ(write(write_fd, buf.data(), kHugePageSize), kHugePageSize);
  }
  CHECK_EQ(close(read_fd), 0) << errno;
  CHECK_EQ(close(write_fd), 0) << errno;
  return reinterpret_cast<void*>(new_offset);
}

TEST(PageFlagsTest, Stale) {
  GTEST_SKIP() << "pageflags not commonly available";

  constexpr size_t kPageSize = 4096;
  constexpr int kNumPages = 6 * kHugePageSize / kPageSize;
  // This is hardcoded because we need to know number of pages in a hugepage.
  ASSERT_EQ(getpagesize(), kPageSize);
  char* p = reinterpret_cast<char*>(
      mmap(reinterpret_cast<void*>(0x00007BADDE001000), kNumPages * kPageSize,
           PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0));
  ASSERT_NE(p, MAP_FAILED) << errno;
  absl::BitGen rng;
  for (int i = 0; i < kNumPages * kPageSize; ++i) {
    p[i] = absl::Uniform(rng, 0, 256);
  }

  // TODO(patrickx): Use MADV_COLLAPSE when broadly available.
  // four hugepages
  // nohugepage hugepage hugepage hugepage nohugepage hugepage
  ASSERT_EQ(madvise(p, kHugePageSize, MADV_NOHUGEPAGE), 0) << errno;
  ASSERT_EQ(madvise(p + kHugePageSize, 3 * kHugePageSize, MADV_HUGEPAGE), 0)
      << errno;
  ASSERT_EQ(madvise(p + 4 * kHugePageSize, kHugePageSize, MADV_NOHUGEPAGE), 0)
      << errno;
  ASSERT_EQ(madvise(p + 5 * kHugePageSize, kHugePageSize, MADV_HUGEPAGE), 0)
      << errno;
  PageFlags s;
  ASSERT_THAT(s.Get(p, kPageSize * kNumPages), Optional(PageStats{}));

  // This doesn't work within a short test timeout. But if you have your own
  // machine with appropriate patches, you can try it out!
  if (absl::GetFlag(FLAGS_check_staleness)) {
    absl::Time start = absl::Now();
    bool ok = false;
    do {
      auto res = s.Get(p, kPageSize * kNumPages);
      ASSERT_TRUE(res.has_value());
      if (res->bytes_stale > kNumPages * kPageSize / 2) {
        LOG(INFO) << absl::StrFormat("Got %ld bytes stale, pointer is at %p",
                                     res->bytes_stale, p);
        ok = true;
        break;
      }
      LOG(INFO) << "still waiting; stale = " << res->bytes_stale;
      absl::SleepFor(absl::Seconds(5));
    } while (absl::Now() - start < absl::Seconds(600));
    EXPECT_TRUE(ok) << "Failed to get enough stale memory.";
  } else {
    std::string fake_pageflags =
        absl::StrCat(testing::TempDir(), "/fake_pageflags");
    void* fake_p =
        GenerateAllStaleTest(fake_pageflags, p, kNumPages * kPageSize);
    // fake_p is likely already aligned, but might as well make sure. This is
    // likely a zero pointer (not to be confused with nullptr).
    void* base_p = reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(fake_p) &
                                           ~(kPageSize - 1));
    PageFlagsFriend mocks(fake_pageflags);
    constexpr uint64_t kSetScanSeconds = 63;
    mocks.SetCachedScanSeconds(kSetScanSeconds);
    for (int num_pages = 0; num_pages < kNumPages; ++num_pages) {
      for (int offset = -1; offset <= 1; ++offset) {
        if (num_pages == 0 && offset == -1) continue;
        // Messing around with scan_seconds is kind of confusing here but not as
        // much overhead as adding a custom matcher. But if you add yet another
        // field here it's time to write one.
        uint64_t scan_seconds = kSetScanSeconds;
        if (num_pages * kPageSize + offset == 0) scan_seconds = 0;
        // CAUTION: If you think this test is very flaky, it's possible it's
        // only passing when the machine you get scheduled on is out of
        // hugepages.
        EXPECT_THAT(mocks.Get(base_p, num_pages * kPageSize + offset),
                    Optional(FieldsAre(num_pages * kPageSize + offset, 0,
                                       scan_seconds)))
            << num_pages << "," << offset;

        EXPECT_THAT(
            mocks.Get((char*)fake_p - offset, num_pages * kPageSize + offset),
            Optional(
                FieldsAre(num_pages * kPageSize + offset, 0, scan_seconds)))
            << num_pages << "," << offset;

        EXPECT_THAT(mocks.Get(fake_p, num_pages * kPageSize + offset),
                    Optional(FieldsAre(num_pages * kPageSize + offset, 0,
                                       scan_seconds)))
            << num_pages << "," << offset;

        EXPECT_THAT(
            mocks.Get((char*)fake_p + offset, num_pages * kPageSize + offset),
            Optional(
                FieldsAre(num_pages * kPageSize + offset, 0, scan_seconds)))
            << num_pages << "," << offset;

        scan_seconds = kSetScanSeconds;
        if (num_pages == 0) scan_seconds = 0;
        EXPECT_THAT(
            mocks.Get((char*)kHugePageSize + offset, num_pages * kPageSize),
            Optional(FieldsAre(num_pages * kPageSize, 0, scan_seconds)))
            << num_pages << "," << offset;
      }
    }

    EXPECT_THAT(mocks.Get(reinterpret_cast<char*>(2 * kHugePageSize +
                                                  16 * kPageSize + 2),
                          kHugePageSize * 3),
                Optional(FieldsAre(kHugePageSize * 3, 0, kSetScanSeconds)));
  }

  ASSERT_EQ(munmap(p, kNumPages * kPageSize), 0) << errno;
}

TEST(PageFlagsTest, Locked) {
  GTEST_SKIP() << "pageflags not commonly available";

#if ABSL_HAVE_ADDRESS_SANITIZER || ABSL_HAVE_MEMORY_SANITIZER || \
    ABSL_HAVE_THREAD_SANITIZER
  GTEST_SKIP() << "Skipped under sanitizers.";
#endif

  constexpr size_t kPageSize = 4096;
  constexpr int kNumPages = 6 * kHugePageSize / kPageSize;
  // This is hardcoded because we need to know number of pages in a hugepage.
  ASSERT_EQ(getpagesize(), kPageSize);
  char* p = reinterpret_cast<char*>(
      mmap(reinterpret_cast<void*>(0x00007BADDE000000), kNumPages * kPageSize,
           PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0));
  ASSERT_NE(p, MAP_FAILED) << errno;
  absl::BitGen rng;
  for (int i = 0; i < kNumPages * kPageSize; ++i) {
    p[i] = absl::Uniform(rng, 0, 256);
  }

  PageFlags s;
  ASSERT_THAT(s.Get(p, kPageSize * kNumPages), Optional(PageStats{}));

  ASSERT_EQ(madvise(p, kHugePageSize, MADV_NOHUGEPAGE), 0) << errno;
  ASSERT_EQ(madvise(p + kHugePageSize, 3 * kHugePageSize, MADV_HUGEPAGE), 0)
      << errno;
  ASSERT_EQ(madvise(p + 4 * kHugePageSize, kHugePageSize, MADV_NOHUGEPAGE), 0)
      << errno;
  ASSERT_EQ(madvise(p + 5 * kHugePageSize, kHugePageSize, MADV_HUGEPAGE), 0)
      << errno;

  ASSERT_THAT(s.Get(p, kPageSize * kNumPages), Optional(PageStats{}));

  ASSERT_EQ(mlock(p, kPageSize * kNumPages), 0) << errno;

  // Wait until the kernel has had time to propagate flags.
  absl::Time start = absl::Now();
  do {
    auto res = s.Get(p, kPageSize * kNumPages);
    ASSERT_TRUE(res.has_value());
    if (res->bytes_locked > kNumPages * kPageSize / 2) {
      LOG(INFO) << "Got " << res->bytes_locked
                << " bytes locked, pointer is at " << (uintptr_t)p;

      if (res->bytes_locked == kNumPages * kPageSize) {
        break;
      }
    }
    LOG(INFO) << "still waiting; locked = " << res->bytes_locked;
    absl::SleepFor(absl::Seconds(5));
  } while (absl::Now() - start < absl::Seconds(60));

  auto res = s.Get(p, kPageSize * kNumPages);
  ASSERT_TRUE(res.has_value());
  ASSERT_EQ(res->bytes_locked, kPageSize * kNumPages);

  ASSERT_EQ(munmap(p, kNumPages * kPageSize), 0) << errno;
}

TEST(PageFlagsTest, OnlyTails) {
  const size_t kPageSize = getpagesize();
  std::vector<uint64_t> data(5 * kHugePageSize / kPageSize);
  for (auto& page : data) {
    page |= kPageTail;
    page |= kPageThp;
  }
  std::string file_path = absl::StrCat(testing::TempDir(), "/only-tails");
  int write_fd =
      signal_safe_open(file_path.c_str(), O_CREAT | O_WRONLY, S_IRUSR);
  ASSERT_NE(write_fd, -1) << errno;

  size_t bytes_to_write = data.size() * sizeof(data[0]);
  ASSERT_EQ(write(write_fd, data.data(), bytes_to_write), bytes_to_write)
      << errno;
  ASSERT_EQ(close(write_fd), 0) << errno;

  PageFlagsFriend s(file_path);
  ASSERT_EQ(s.Get(reinterpret_cast<char*>(kHugePageSize), kHugePageSize),
            std::nullopt);
}

// Queries and checks the pageflags if the pages are hugepage-backed or not.
// TODO(b/28093874): Check to see if we can add a real pageflags test (e.g.
// using MADV_COLLAPSE) to confirm the hugepage status using pageflags.
TEST(PageFlagsTest, IsHugepageBacked) {
  const auto test_hugepage_status = [&](uint64_t flags, bool expected) {
    const size_t kPageSize = getpagesize();
    const size_t kPagesPerHugepage = kHugePageSize / kPageSize;

    std::vector<uint64_t> data(kPagesPerHugepage);
    for (auto& page : data) {
      page |= flags;
    }
    std::string file_path =
        absl::StrCat(testing::TempDir(), "/hugepage_backed_", flags);
    int write_fd =
        signal_safe_open(file_path.c_str(), O_CREAT | O_WRONLY, S_IRUSR);
    ASSERT_NE(write_fd, -1) << errno;

    size_t bytes_to_write = data.size() * sizeof(data[0]);
    ASSERT_EQ(write(write_fd, data.data(), bytes_to_write), bytes_to_write)
        << errno;
    ASSERT_EQ(close(write_fd), 0) << errno;

    PageFlagsFriend s(file_path);
    for (int page = 0; page < kPagesPerHugepage; ++page) {
      ASSERT_EQ(s.IsHugepageBacked(reinterpret_cast<char*>(page)), expected);
    }
    CHECK_EQ(signal_safe_close(write_fd), 0) << errno;
  };

  test_hugepage_status(kPageHuge, /*expected=*/true);
  test_hugepage_status(kPageHuge | kPageThp, /*expected=*/true);
  test_hugepage_status(kPageThp, /*expected=*/true);
  test_hugepage_status(/*flags=*/0, /*expected=*/false);
}

TEST(PageFlagsTest, TooManyTails) {
  const size_t kPageSize = getpagesize();
  std::vector<uint64_t> data(7 * kHugePageSize / kPageSize);
  for (auto& page : data) {
    page |= kPageTail;
    page |= kPageThp;
  }
  data[kHugePageSize / kPageSize] = kPageHead | kPageThp;
  data[2 * kHugePageSize / kPageSize] = kPageHead | kPageThp;
  data[3 * kHugePageSize / kPageSize] = kPageHead | kPageThp;
  data[5 * kHugePageSize / kPageSize] = kPageHead | kPageThp;

  std::string file_path = absl::StrCat(testing::TempDir(), "/too-many-tails");
  int write_fd =
      signal_safe_open(file_path.c_str(), O_CREAT | O_WRONLY, S_IRUSR);
  ASSERT_NE(write_fd, -1) << errno;

  size_t bytes_to_write = data.size() * sizeof(data[0]);
  ASSERT_EQ(write(write_fd, data.data(), bytes_to_write), bytes_to_write)
      << errno;
  ASSERT_EQ(close(write_fd), 0) << errno;

  PageFlagsFriend s(file_path);
  EXPECT_THAT(s.Get(reinterpret_cast<char*>(kHugePageSize), kHugePageSize),
              Optional(PageStats{}));
  EXPECT_THAT(s.Get(reinterpret_cast<char*>(kHugePageSize), 3 * kHugePageSize),
              Optional(PageStats{}));

  EXPECT_THAT(s.Get(reinterpret_cast<char*>(3 * kHugePageSize), kHugePageSize),
              Optional(PageStats{}));
  EXPECT_THAT(
      s.Get(reinterpret_cast<char*>(3 * kHugePageSize), 2 * kHugePageSize),
      std::nullopt);
  EXPECT_THAT(
      s.Get(reinterpret_cast<char*>(3 * kHugePageSize), 3 * kHugePageSize),
      std::nullopt);
}

TEST(PageFlagsTest, NotThp) {
  const size_t kPageSize = getpagesize();
  std::vector<uint64_t> data(3 * kHugePageSize / kPageSize);
  for (auto& page : data) {
    page |= kPageHead;
  }

  std::string file_path = absl::StrCat(testing::TempDir(), "/not-thp");
  int write_fd =
      signal_safe_open(file_path.c_str(), O_CREAT | O_WRONLY, S_IRUSR);
  ASSERT_NE(write_fd, -1) << errno;

  size_t bytes_to_write = data.size() * sizeof(data[0]);
  ASSERT_EQ(write(write_fd, data.data(), bytes_to_write), bytes_to_write)
      << errno;
  ASSERT_EQ(close(write_fd), 0) << errno;

  PageFlagsFriend s(file_path);
  EXPECT_THAT(s.Get(nullptr, kHugePageSize), std::nullopt);
}

TEST(PageFlagsTest, CannotOpen) {
  PageFlagsFriend s("/tmp/a667ba48-18ba-4523-a8a7-b49ece3a6c2b");
  EXPECT_FALSE(s.Get(nullptr, 1).has_value());
}

TEST(PageFlagsTest, CannotRead) {
  PageFlagsFriend s("/dev/null");
  EXPECT_FALSE(s.Get(nullptr, 1).has_value());
}

TEST(PageFlagsTest, CannotSeek) {
  PageFlagsFriend s("/dev/null");
  EXPECT_FALSE(s.Get(&s, 1).has_value());
}

// For this and the following tests, the EXPECT_* macros allocate memory so
// that's why we have a strange dance with resetting the AllocationGuard.
TEST(StaleSeconds, Read) {
  GTEST_SKIP() << "pageflags not commonly available";
  // Allocate at least a couple hugepages or pageflags might have a short read.
  auto alloc = std::make_unique<std::array<char, (1 << 22)>>();

  std::string fake_pageflags =
      absl::StrCat(testing::TempDir(), "/fake_pageflags2");
  void* fake_p = GenerateAllStaleTest(fake_pageflags, &*alloc, 1);
  std::string fake_stale_seconds =
      absl::StrCat(testing::TempDir(), "/fake_stale_seconds");
  SetContents(fake_stale_seconds, "123");

  std::optional<AllocationGuard> g;
  g.emplace();
  PageFlagsFriend s(fake_pageflags);
  auto read_seconds = s.MaybeReadStaleScanSeconds(fake_stale_seconds);
  auto result = s.Get(fake_p, 1);
  g.reset();

  EXPECT_THAT(read_seconds, 123);
  EXPECT_THAT(result, Optional(FieldsAre(1, 0, 123)));
}

void ExpectStaleSecondsFailedReadFrom(absl::string_view filename) {
  std::optional<AllocationGuard> g;
  g.emplace();
  PageFlagsFriend s;
  auto read_seconds = s.MaybeReadStaleScanSeconds(filename);
  auto cached_seconds = s.CachedScanSeconds();
  g.reset();

  EXPECT_EQ(read_seconds, cached_seconds);
  EXPECT_EQ(cached_seconds, 0);
}

TEST(StaleSeconds, NotFound) {
  ExpectStaleSecondsFailedReadFrom(absl::StrCat(
      testing::TempDir(), "/nonexistent-f52a3d06-ee84-42c1-a298-a93a4b164ff0"));
}

TEST(StaleSeconds, Bad) {
  std::string fake_stale_seconds =
      absl::StrCat(testing::TempDir(), "/fake_stale_seconds2");
  SetContents(fake_stale_seconds, "[always]");

  ExpectStaleSecondsFailedReadFrom(fake_stale_seconds);
}

TEST(StaleSeconds, IntOutOfBounds) {
  std::string fake_stale_seconds =
      absl::StrCat(testing::TempDir(), "/fake_stale_seconds3");
  SetContents(fake_stale_seconds, "-1");

  ExpectStaleSecondsFailedReadFrom(fake_stale_seconds);
}

TEST(StaleSeconds, TextOverflow) {
  std::string fake_stale_seconds =
      absl::StrCat(testing::TempDir(), "/fake_stale_seconds4");
  std::string contents(1 << 22, '9');
  SetContents(fake_stale_seconds, contents);

  ExpectStaleSecondsFailedReadFrom(fake_stale_seconds);
}

}  // namespace
}  // namespace tcmalloc_internal
}  // namespace tcmalloc
