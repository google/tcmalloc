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

#ifndef MADV_COLLAPSE  // Linux 6.1+
#define MADV_COLLAPSE 25
#endif

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
#include "tcmalloc/internal/page_size.h"
#include "tcmalloc/internal/range_tracker.h"
#include "tcmalloc/internal/strerror.h"
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

  decltype(auto) GetSinglePageBitmaps(const void* addr) {
    return r_.GetSinglePageBitmaps(addr);
  }

  decltype(auto) MaybeReadOne(uintptr_t vaddr, uint64_t& flags, bool& is_huge) {
    return r_.MaybeReadOne(vaddr, flags, is_huge);
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
            << ", incompressible = " << s.bytes_incompressible
            << ", stale_scan_seconds = " << s.stale_scan_seconds << "}";
}

namespace {

using ::testing::FieldsAre;
using ::testing::Optional;

constexpr uint64_t kPageHead = (1UL << 15);
constexpr uint64_t kPageTail = (1UL << 16);
constexpr uint64_t kPageHuge = (1UL << 17);
constexpr uint64_t kPageUnevictable = (1UL << 18);
constexpr uint64_t kPageThp = (1UL << 22);
constexpr uint64_t kPageMlocked = (1UL << 33);
constexpr uint64_t kPageStale = (1UL << 44);
constexpr uint64_t kPageIncompressible = (1UL << 47);

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

// Maps `size` bytes of anonymous memory starting `misalignment` bytes past a
// hugepage boundary. mmap only promises hardware page alignment, and asking
// for an absolute address is not portable: under MSan and TSan on aarch64 the
// runtime's mmap interceptor fails MAP_FIXED requests outside the sanitizer's
// application range with EINVAL and silently drops non-fixed hints. Instead,
// map `size` plus two hugepages of slack wherever the kernel chooses, pick the
// sub-range with the requested alignment, and unmap the slack around it.
char* MmapWithHugepageMisalignment(size_t size, size_t misalignment) {
  const size_t kHardwarePageSize = getpagesize();
  CHECK_EQ(size % kHardwarePageSize, 0);
  CHECK_EQ(misalignment % kHardwarePageSize, 0);
  CHECK_LT(misalignment, kHugePageSize);

  const size_t padded_size = size + 2 * kHugePageSize;
  void* raw = mmap(nullptr, padded_size, PROT_READ | PROT_WRITE,
                   MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
  CHECK_NE(raw, MAP_FAILED) << errno;

  const uintptr_t raw_start = reinterpret_cast<uintptr_t>(raw);
  const uintptr_t raw_end = raw_start + padded_size;
  const uintptr_t start =
      ((raw_start + kHugePageSize - 1) & kHugePageMask) + misalignment;
  const uintptr_t end = start + size;
  CHECK_LE(end, raw_end);
  if (start > raw_start) {
    CHECK_EQ(munmap(raw, start - raw_start), 0) << errno;
  }
  if (end < raw_end) {
    CHECK_EQ(munmap(reinterpret_cast<void*>(end), raw_end - end), 0) << errno;
  }
  return reinterpret_cast<char*>(start);
}

TEST(PageFlagsTest, Alignment) {
  GTEST_SKIP() << "pageflags not commonly available";

  const size_t kHardwarePageSize = getpagesize();
  const size_t kSize = 6 * kHugePageSize;
  // A hugepage-aligned start and a start one hardware page past a hugepage
  // boundary.
  for (size_t misalignment : {size_t{0}, kHardwarePageSize}) {
    char* p = MmapWithHugepageMisalignment(kSize, misalignment);
    ASSERT_EQ(madvise(p, kSize, MADV_HUGEPAGE), 0) << errno;

    PageFlags s;
    EXPECT_THAT(s.Get(p, kSize), Optional(PageStats{}))
        << static_cast<void*>(p);
    ASSERT_EQ(munmap(p, kSize), 0) << errno;
  }
}

// Write an alternate "pageflags" file comprising all stale pages at the path
// indicated by `filename`. The actual pageflags are copied from the pages that
// obj spans, but we add KPF_STALE. The alternate pageflags file starts at 0, so
// we return a pointer to `obj` in the alternate virtual memory space. If `obj`
// is page-aligned, this is a zero pointer (not to be confused with a null
// pointer).
void* GenerateAllStaleTest(absl::string_view filename, void* obj, size_t size) {
  const size_t kHardwarePageSize = getpagesize();

  uintptr_t ptr = reinterpret_cast<uintptr_t>(obj);
  uintptr_t pages_start = ptr & kHugePageMask;
  uintptr_t new_offset = ptr - pages_start;

  off_t file_read_offset = pages_start / kHardwarePageSize * kPagemapEntrySize;
  int read_fd = signal_safe_open("/proc/self/pageflags", O_RDONLY);
  CHECK_NE(read_fd, -1)
      << StrError(errno)
      << " while reading pageflags; does your kernel support it?";
  int write_fd =
      signal_safe_open(filename.data(), O_CREAT | O_WRONLY, S_IRUSR | S_IWUSR);
  CHECK_NE(write_fd, -1) << errno;

  CHECK_EQ(::lseek(read_fd, file_read_offset, SEEK_SET), file_read_offset);
  // Copy the entries for `obj`'s hugepages plus three more, one hugepage's
  // worth of entries at a time: kHugePageSize / kHardwarePageSize entries of
  // kPagemapEntrySize bytes, not kHugePageSize bytes, which would span 1 GiB
  // of address space per iteration and run past the end of the address space
  // when `obj` sits near the top of the mmap area.
  std::vector<uint64_t> buf(kHugePageSize / kHardwarePageSize);
  const size_t kEntryBytesPerHugePage = buf.size() * kPagemapEntrySize;
  for (int i = 0; i < size / kHugePageSize + 3; ++i) {
    CHECK_EQ(signal_safe_read(read_fd, reinterpret_cast<char*>(buf.data()),
                              kEntryBytesPerHugePage, nullptr),
             kEntryBytesPerHugePage);
    for (uint64_t& page : buf) {
      if ((page & kPageHead) == kPageHead || (page & kPageTail) != kPageTail) {
        page |= kPageStale;
      }
    }
    CHECK_EQ(write(write_fd, buf.data(), kEntryBytesPerHugePage),
             kEntryBytesPerHugePage);
  }
  CHECK_EQ(close(read_fd), 0) << errno;
  CHECK_EQ(close(write_fd), 0) << errno;
  return reinterpret_cast<void*>(new_offset);
}

TEST(PageFlagsTest, Stale) {
  GTEST_SKIP() << "pageflags not commonly available";

  constexpr size_t kHardwarePageSize = 4096;
  constexpr int kNumPages = 6 * kHugePageSize / kHardwarePageSize;
  // This is hardcoded because we need to know number of pages in a hugepage.
  ASSERT_EQ(getpagesize(), kHardwarePageSize);
  // Start one hardware page past a hugepage boundary so that `fake_p` below is
  // non-zero and the `fake_p - offset` cases stay inside the fake address
  // space.
  char* p = MmapWithHugepageMisalignment(kNumPages * kHardwarePageSize,
                                         /*misalignment=*/kHardwarePageSize);
  absl::BitGen rng;
  for (int i = 0; i < kNumPages * kHardwarePageSize; ++i) {
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
  ASSERT_THAT(s.Get(p, kHardwarePageSize * kNumPages), Optional(PageStats{}));

  // This doesn't work within a short test timeout. But if you have your own
  // machine with appropriate patches, you can try it out!
  if (absl::GetFlag(FLAGS_check_staleness)) {
    absl::Time start = absl::Now();
    bool ok = false;
    do {
      auto res = s.Get(p, kHardwarePageSize * kNumPages);
      ASSERT_TRUE(res.has_value());
      if (res->bytes_stale > kNumPages * kHardwarePageSize / 2) {
        LOG(INFO) << absl::StrFormat("Got %ld bytes stale, pointer is at %p",
                                     res->bytes_stale, p);
        ok = true;
        break;
      }
      LOG(INFO) << "still waiting; stale = " << res->bytes_stale;
      absl::SleepFor(absl::Milliseconds(100));
    } while (absl::Now() - start < absl::Seconds(600));
    EXPECT_TRUE(ok) << "Failed to get enough stale memory.";
  } else {
    std::string fake_pageflags =
        absl::StrCat(testing::TempDir(), "/fake_pageflags");
    void* fake_p =
        GenerateAllStaleTest(fake_pageflags, p, kNumPages * kHardwarePageSize);
    // fake_p is exactly one hardware page into the fake address space (see the
    // mapping above), so it is already aligned and base_p == fake_p; round
    // anyway in case the misalignment changes.
    void* base_p = reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(fake_p) &
                                           ~(kHardwarePageSize - 1));
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
        if (num_pages * kHardwarePageSize + offset == 0) scan_seconds = 0;
        // CAUTION: If you think this test is very flaky, it's possible it's
        // only passing when the machine you get scheduled on is out of
        // hugepages.
        EXPECT_THAT(mocks.Get(base_p, num_pages * kHardwarePageSize + offset),
                    Optional(FieldsAre(num_pages * kHardwarePageSize + offset,
                                       0, 0, scan_seconds)))
            << num_pages << "," << offset;

        EXPECT_THAT(mocks.Get((char*)fake_p - offset,
                              num_pages * kHardwarePageSize + offset),
                    Optional(FieldsAre(num_pages * kHardwarePageSize + offset,
                                       0, 0, scan_seconds)))
            << num_pages << "," << offset;

        EXPECT_THAT(mocks.Get(fake_p, num_pages * kHardwarePageSize + offset),
                    Optional(FieldsAre(num_pages * kHardwarePageSize + offset,
                                       0, 0, scan_seconds)))
            << num_pages << "," << offset;

        EXPECT_THAT(mocks.Get((char*)fake_p + offset,
                              num_pages * kHardwarePageSize + offset),
                    Optional(FieldsAre(num_pages * kHardwarePageSize + offset,
                                       0, 0, scan_seconds)))
            << num_pages << "," << offset;

        scan_seconds = kSetScanSeconds;
        if (num_pages == 0) scan_seconds = 0;
        EXPECT_THAT(mocks.Get((char*)kHugePageSize + offset,
                              num_pages * kHardwarePageSize),
                    Optional(FieldsAre(num_pages * kHardwarePageSize, 0, 0,
                                       scan_seconds)))
            << num_pages << "," << offset;
      }
    }

    EXPECT_THAT(mocks.Get(reinterpret_cast<char*>(2 * kHugePageSize +
                                                  16 * kHardwarePageSize + 2),
                          kHugePageSize * 3),
                Optional(FieldsAre(kHugePageSize * 3, 0, 0, kSetScanSeconds)));
  }

  ASSERT_EQ(munmap(p, kNumPages * kHardwarePageSize), 0) << errno;
}

TEST(PageFlagsTest, Locked) {
  GTEST_SKIP() << "pageflags not commonly available";

#if ABSL_HAVE_ADDRESS_SANITIZER || ABSL_HAVE_MEMORY_SANITIZER || \
    ABSL_HAVE_THREAD_SANITIZER
  GTEST_SKIP() << "Skipped under sanitizers.";
#endif

  constexpr size_t kHardwarePageSize = 4096;
  constexpr int kNumPages = 6 * kHugePageSize / kHardwarePageSize;
  // This is hardcoded because we need to know number of pages in a hugepage.
  ASSERT_EQ(getpagesize(), kHardwarePageSize);
  // Hugepage-aligned so that the MADV_HUGEPAGE ranges below are whole
  // hugepages.
  char* p = MmapWithHugepageMisalignment(kNumPages * kHardwarePageSize,
                                         /*misalignment=*/0);

  PageFlags s;
  ASSERT_THAT(s.Get(p, kHardwarePageSize * kNumPages), Optional(PageStats{}));

  ASSERT_EQ(madvise(p, kHugePageSize, MADV_NOHUGEPAGE), 0) << errno;
  ASSERT_EQ(madvise(p + kHugePageSize, 3 * kHugePageSize, MADV_HUGEPAGE), 0)
      << errno;
  ASSERT_EQ(madvise(p + 4 * kHugePageSize, kHugePageSize, MADV_NOHUGEPAGE), 0)
      << errno;
  ASSERT_EQ(madvise(p + 5 * kHugePageSize, kHugePageSize, MADV_HUGEPAGE), 0)
      << errno;

  absl::BitGen rng;
  for (int i = 0; i < kNumPages * kHardwarePageSize; ++i) {
    p[i] = absl::Uniform(rng, 0, 256);
  }

  ASSERT_THAT(s.Get(p, kHardwarePageSize * kNumPages), Optional(PageStats{}));

  // Synchronously collapse the MADV_HUGEPAGE regions into transparent
  // hugepages before measuring. Otherwise khugepaged forms them
  // asynchronously, and while a collapse is in flight the kernel's per-page
  // flags (COMPOUND_HEAD/TAIL and MLOCKED/UNEVICTABLE) are transiently
  // inconsistent, so the reported locked byte count oscillates a few pages
  // around the true value. MADV_COLLAPSE must run before mlock() (it can fail
  // on pinned pages) and is best-effort (Linux 6.1+); ignore failures on
  // kernels that lack it. Afterwards, set MADV_NOHUGEPAGE on those ranges
  // before mlock() so that if MADV_COLLAPSE could not collapse a range,
  // khugepaged cannot asynchronously collapse or migrate pages during or after
  // mlock() (already-collapsed THPs remain intact).
  (void)madvise(p + kHugePageSize, 3 * kHugePageSize, MADV_COLLAPSE);
  (void)madvise(p + 5 * kHugePageSize, kHugePageSize, MADV_COLLAPSE);
  ASSERT_EQ(madvise(p + kHugePageSize, 3 * kHugePageSize, MADV_NOHUGEPAGE), 0)
      << errno;
  ASSERT_EQ(madvise(p + 5 * kHugePageSize, kHugePageSize, MADV_NOHUGEPAGE), 0)
      << errno;

  ASSERT_EQ(mlock(p, kHardwarePageSize * kNumPages), 0) << errno;

  // Wait until the kernel has had time to propagate flags, then assert on the
  // same reading that observed the full count. A separate re-read would be a
  // TOCTOU: it could sample a transiently inconsistent state and undercount.
  std::optional<PageStats> res;
  absl::Time start = absl::Now();
  do {
    res = s.Get(p, kHardwarePageSize * kNumPages);
    ASSERT_TRUE(res.has_value());
    if (res->bytes_locked == kNumPages * kHardwarePageSize) {
      LOG(INFO) << "Got " << res->bytes_locked
                << " bytes locked, pointer is at " << (uintptr_t)p;
      break;
    }
    LOG(INFO) << "still waiting; locked = " << res->bytes_locked;
    absl::SleepFor(absl::Milliseconds(100));
  } while (absl::Now() - start < absl::Seconds(60));

  ASSERT_TRUE(res.has_value());
  ASSERT_EQ(res->bytes_locked, kHardwarePageSize * kNumPages);

  ASSERT_EQ(munmap(p, kNumPages * kHardwarePageSize), 0) << errno;
}

TEST(PageFlagsTest, MlockedFlags) {
  GTEST_SKIP() << "pageflags not commonly available";

#if ABSL_HAVE_ADDRESS_SANITIZER || ABSL_HAVE_MEMORY_SANITIZER || \
    ABSL_HAVE_THREAD_SANITIZER
  GTEST_SKIP() << "Skipped under sanitizers.";
#endif

  const size_t kHardwarePageSize = getpagesize();
  const size_t kSize = kHugePageSize;
  char* p = MmapWithHugepageMisalignment(kSize, /*misalignment=*/0);
  absl::BitGen rng;
  for (size_t i = 0; i < kSize; ++i) {
    p[i] = absl::Uniform<uint8_t>(rng);
  }

  PageFlagsFriend s;
  ASSERT_THAT(s.Get(p, kSize), Optional(PageStats{}));
  for (size_t offset = 0; offset < kSize; offset += kHardwarePageSize) {
    uint64_t flags = 0;
    bool is_huge = false;
    ASSERT_EQ(
        s.MaybeReadOne(reinterpret_cast<uintptr_t>(p) + offset, flags, is_huge),
        absl::StatusCode::kOk);
    EXPECT_EQ(flags & kPageMlocked, 0)
        << "offset=" << offset << " flags=0x" << absl::Hex(flags);
  }

  (void)madvise(p, kSize, MADV_COLLAPSE);
  ASSERT_EQ(madvise(p, kSize, MADV_NOHUGEPAGE), 0) << errno;
  ASSERT_EQ(mlock(p, kSize), 0) << errno;

  std::optional<PageStats> res;
  absl::Time start = absl::Now();
  do {
    res = s.Get(p, kSize);
    ASSERT_TRUE(res.has_value());
    if (res->bytes_locked == kSize) {
      break;
    }
    absl::SleepFor(absl::Milliseconds(100));
  } while (absl::Now() - start < absl::Seconds(60));
  ASSERT_TRUE(res.has_value());
  ASSERT_EQ(res->bytes_locked, kSize);

  for (size_t offset = 0; offset < kSize; offset += kHardwarePageSize) {
    uint64_t flags = 0;
    bool is_huge = false;
    ASSERT_EQ(
        s.MaybeReadOne(reinterpret_cast<uintptr_t>(p) + offset, flags, is_huge),
        absl::StatusCode::kOk);
    EXPECT_EQ(flags & kPageMlocked, kPageMlocked)
        << "offset=" << offset << " flags=0x" << absl::Hex(flags);
  }

  ASSERT_EQ(munmap(p, kSize), 0) << errno;
}

TEST(PageFlagsTest, UnevictableWithoutMlockedIsNotLocked) {
  const size_t kHardwarePageSize = getpagesize();
  const size_t kPagesPerHugepage = kHugePageSize / kHardwarePageSize;
  std::vector<uint64_t> data(3 * kPagesPerHugepage, 0);

  // Hugepage 0: individual native pages with combinations of KPF_UNEVICTABLE
  // and KPF_MLOCKED.
  data[0] = kPageUnevictable;
  data[1] = kPageMlocked;
  data[2] = kPageMlocked | kPageUnevictable;

  // Hugepage 1: compound THP that is unevictable but not mlocked.
  data[kPagesPerHugepage] = kPageHead | kPageThp | kPageUnevictable;
  for (size_t i = 1; i < kPagesPerHugepage; ++i) {
    data[kPagesPerHugepage + i] = kPageTail | kPageThp;
  }

  // Hugepage 2: compound THP that is mlocked (and unevictable).
  data[2 * kPagesPerHugepage] =
      kPageHead | kPageThp | kPageMlocked | kPageUnevictable;
  for (size_t i = 1; i < kPagesPerHugepage; ++i) {
    data[2 * kPagesPerHugepage + i] = kPageTail | kPageThp;
  }

  std::string file_path =
      absl::StrCat(testing::TempDir(), "/unevictable-vs-mlocked");
  SetContents(file_path,
              absl::string_view(reinterpret_cast<const char*>(data.data()),
                                data.size() * sizeof(data[0])));

  PageFlagsFriend s(file_path);
  EXPECT_THAT(
      s.Get(reinterpret_cast<char*>(0 * kHardwarePageSize), kHardwarePageSize),
      Optional(FieldsAre(0, 0, 0, 0)));
  EXPECT_THAT(
      s.Get(reinterpret_cast<char*>(1 * kHardwarePageSize), kHardwarePageSize),
      Optional(FieldsAre(0, kHardwarePageSize, 0, 0)));
  EXPECT_THAT(
      s.Get(reinterpret_cast<char*>(2 * kHardwarePageSize), kHardwarePageSize),
      Optional(FieldsAre(0, kHardwarePageSize, 0, 0)));
  EXPECT_THAT(s.Get(reinterpret_cast<char*>(0), 3 * kHardwarePageSize),
              Optional(FieldsAre(0, 2 * kHardwarePageSize, 0, 0)));
  EXPECT_THAT(s.Get(reinterpret_cast<char*>(kHugePageSize), kHugePageSize),
              Optional(FieldsAre(0, 0, 0, 0)));
  EXPECT_THAT(s.Get(reinterpret_cast<char*>(2 * kHugePageSize), kHugePageSize),
              Optional(FieldsAre(0, kHugePageSize, 0, 0)));
}

TEST(PageFlagsTest, Incompressible) {
  const size_t kHardwarePageSize = getpagesize();
  const size_t kPagesPerHugepage = kHugePageSize / kHardwarePageSize;
  std::vector<uint64_t> data(3 * kPagesPerHugepage, 0);

  // Hugepage 0: individual native pages with combinations of
  // KPF_INCOMPRESSIBLE, KPF_UNEVICTABLE, and KPF_MLOCKED.
  data[0] = 0;
  data[1] = kPageIncompressible | kPageUnevictable;
  data[2] = kPageIncompressible | kPageStale;
  data[3] = kPageMlocked | kPageUnevictable;

  // Hugepage 1: compound THP with KPF_INCOMPRESSIBLE on the head page.
  data[kPagesPerHugepage] =
      kPageHead | kPageThp | kPageIncompressible | kPageUnevictable;
  for (size_t i = 1; i < kPagesPerHugepage; ++i) {
    data[kPagesPerHugepage + i] = kPageTail | kPageThp;
  }

  // Hugepage 2: compound THP without KPF_INCOMPRESSIBLE.
  data[2 * kPagesPerHugepage] = kPageHead | kPageThp;
  for (size_t i = 1; i < kPagesPerHugepage; ++i) {
    data[2 * kPagesPerHugepage + i] = kPageTail | kPageThp;
  }

  std::string file_path = absl::StrCat(testing::TempDir(), "/incompressible");
  SetContents(file_path,
              absl::string_view(reinterpret_cast<const char*>(data.data()),
                                data.size() * sizeof(data[0])));

  PageFlagsFriend s(file_path);
  constexpr uint64_t kSetScanSeconds = 63;
  s.SetCachedScanSeconds(kSetScanSeconds);
  // Page 0: bit 47 not set -> 0 bytes_incompressible.
  EXPECT_THAT(
      s.Get(reinterpret_cast<char*>(0 * kHardwarePageSize), kHardwarePageSize),
      Optional(FieldsAre(0, 0, 0, 0)));
  // Page 1: incompressible + unevictable -> bytes_incompressible only.
  EXPECT_THAT(
      s.Get(reinterpret_cast<char*>(1 * kHardwarePageSize), kHardwarePageSize),
      Optional(FieldsAre(0, 0, kHardwarePageSize, 0)));
  // Partial span inside page 1.
  EXPECT_THAT(s.Get(reinterpret_cast<char*>(1 * kHardwarePageSize + 100), 250),
              Optional(FieldsAre(0, 0, 250, 0)));
  // Span across pages 0..3: 1 stale page (page 2), 1 locked page (page 3),
  // 2 incompressible pages (pages 1 and 2).
  EXPECT_THAT(s.Get(reinterpret_cast<char*>(0), 4 * kHardwarePageSize),
              Optional(FieldsAre(kHardwarePageSize, kHardwarePageSize,
                                 2 * kHardwarePageSize, kSetScanSeconds)));
  // Hugepage 1: compound THP inherits head flags across all tail pages.
  EXPECT_THAT(s.Get(reinterpret_cast<char*>(kHugePageSize), kHugePageSize),
              Optional(FieldsAre(0, 0, kHugePageSize, 0)));
  // Hugepage 2: compound THP without KPF_INCOMPRESSIBLE -> 0.
  EXPECT_THAT(s.Get(reinterpret_cast<char*>(2 * kHugePageSize), kHugePageSize),
              Optional(FieldsAre(0, 0, 0, 0)));
}

TEST(PageFlagsTest, OnlyTails) {
  const size_t kHardwarePageSize = getpagesize();
  std::vector<uint64_t> data(5 * kHugePageSize / kHardwarePageSize);
  for (auto& page : data) {
    page |= kPageTail;
    page |= kPageThp;
  }
  std::string file_path = absl::StrCat(testing::TempDir(), "/only-tails");
  int write_fd = signal_safe_open(file_path.c_str(), O_CREAT | O_WRONLY,
                                  S_IRUSR | S_IWUSR);
  ASSERT_NE(write_fd, -1) << errno;

  size_t bytes_to_write = data.size() * sizeof(data[0]);
  ASSERT_EQ(write(write_fd, data.data(), bytes_to_write), bytes_to_write)
      << errno;
  ASSERT_EQ(close(write_fd), 0) << errno;

  PageFlagsFriend s(file_path);
  ASSERT_EQ(s.Get(reinterpret_cast<char*>(kHugePageSize), kHugePageSize),
            std::nullopt);
}

TEST(PageFlagsTest, IsHugepageBackedBadFile) {
  PageFlagsFriend s("/tmp/04a94de3-cb23-4397-8554-de209645e7bf");
  EXPECT_THAT(s.IsHugepageBacked(nullptr), std::nullopt);
  EXPECT_THAT(s.IsHugepageBacked(this), std::nullopt);
}

// Queries and checks the pageflags if the pages are hugepage-backed or not.
// TODO(b/28093874): Check to see if we can add a real pageflags test (e.g.
// using MADV_COLLAPSE) to confirm the hugepage status using pageflags.
TEST(PageFlagsTest, IsHugepageBacked) {
  const auto test_hugepage_status = [&](uint64_t flags, bool expected) {
    const size_t kHardwarePageSize = getpagesize();
    const size_t kPagesPerHugepage = kHugePageSize / kHardwarePageSize;

    std::vector<uint64_t> data(kPagesPerHugepage);
    for (auto& page : data) {
      page |= flags;
    }
    std::string file_path =
        absl::StrCat(testing::TempDir(), "/hugepage_backed_", flags);
    int write_fd = signal_safe_open(file_path.c_str(), O_CREAT | O_WRONLY,
                                    S_IRUSR | S_IWUSR);
    ASSERT_NE(write_fd, -1) << errno;

    size_t bytes_to_write = data.size() * sizeof(data[0]);
    ASSERT_EQ(write(write_fd, data.data(), bytes_to_write), bytes_to_write)
        << errno;
    ASSERT_EQ(close(write_fd), 0) << errno;

    PageFlagsFriend s(file_path);
    for (int page = 0; page < kPagesPerHugepage; ++page) {
      ASSERT_EQ(s.IsHugepageBacked(reinterpret_cast<char*>(page)), expected);
    }
  };

  test_hugepage_status(kPageHuge, /*expected=*/true);
  test_hugepage_status(kPageHuge | kPageThp, /*expected=*/true);
  test_hugepage_status(kPageThp, /*expected=*/true);
  test_hugepage_status(/*flags=*/0, /*expected=*/false);
}

TEST(PageFlagsTest, TooManyTails) {
  const size_t kHardwarePageSize = getpagesize();
  std::vector<uint64_t> data(7 * kHugePageSize / kHardwarePageSize);
  for (auto& page : data) {
    page |= kPageTail;
    page |= kPageThp;
  }
  data[kHugePageSize / kHardwarePageSize] = kPageHead | kPageThp;
  data[2 * kHugePageSize / kHardwarePageSize] = kPageHead | kPageThp;
  data[3 * kHugePageSize / kHardwarePageSize] = kPageHead | kPageThp;
  data[5 * kHugePageSize / kHardwarePageSize] = kPageHead | kPageThp;

  std::string file_path = absl::StrCat(testing::TempDir(), "/too-many-tails");
  int write_fd = signal_safe_open(file_path.c_str(), O_CREAT | O_WRONLY,
                                  S_IRUSR | S_IWUSR);
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
  const size_t kHardwarePageSize = getpagesize();
  std::vector<uint64_t> data(3 * kHugePageSize / kHardwarePageSize);
  for (auto& page : data) {
    page |= kPageHead;
  }

  std::string file_path = absl::StrCat(testing::TempDir(), "/not-thp");
  int write_fd = signal_safe_open(file_path.c_str(), O_CREAT | O_WRONLY,
                                  S_IRUSR | S_IWUSR);
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
  EXPECT_EQ(s.IsHugepageBacked(nullptr), std::nullopt);
}

TEST(PageFlagsTest, CannotSeek) {
  PageFlagsFriend s("/dev/null");
  EXPECT_FALSE(s.Get(&s, 1).has_value());
  EXPECT_EQ(s.IsHugepageBacked(&s), std::nullopt);
}

// For this and the following tests, the EXPECT_* macros allocate memory so
// that's why we have a strange dance with resetting the AllocationGuard.
TEST(StaleSeconds, Read) {
  GTEST_SKIP() << "pageflags not commonly available";
  // Allocate at least a couple hugepages or pageflags might have a short read.
  auto alloc = std::make_unique<std::array<char, (1 << 22)>>();

#if ABSL_HAVE_ADDRESS_SANITIZER || ABSL_HAVE_MEMORY_SANITIZER || \
    ABSL_HAVE_THREAD_SANITIZER
  GTEST_SKIP() << "Skipped under sanitizers.";
#endif

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
  EXPECT_THAT(result, Optional(FieldsAre(1, 0, 0, 123)));
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
#if ABSL_HAVE_ADDRESS_SANITIZER || ABSL_HAVE_MEMORY_SANITIZER || \
    ABSL_HAVE_THREAD_SANITIZER
  GTEST_SKIP() << "Skipped under sanitizers.";
#endif

  std::string fake_stale_seconds =
      absl::StrCat(testing::TempDir(), "/fake_stale_seconds2");
  SetContents(fake_stale_seconds, "[always]");

  ExpectStaleSecondsFailedReadFrom(fake_stale_seconds);
}

TEST(StaleSeconds, IntOutOfBounds) {
#if ABSL_HAVE_ADDRESS_SANITIZER || ABSL_HAVE_MEMORY_SANITIZER || \
    ABSL_HAVE_THREAD_SANITIZER
  GTEST_SKIP() << "Skipped under sanitizers.";
#endif

  std::string fake_stale_seconds =
      absl::StrCat(testing::TempDir(), "/fake_stale_seconds3");
  SetContents(fake_stale_seconds, "-1");

  ExpectStaleSecondsFailedReadFrom(fake_stale_seconds);
}

TEST(StaleSeconds, TextOverflow) {
#if ABSL_HAVE_ADDRESS_SANITIZER || ABSL_HAVE_MEMORY_SANITIZER || \
    ABSL_HAVE_THREAD_SANITIZER
  GTEST_SKIP() << "Skipped under sanitizers.";
#endif

  std::string fake_stale_seconds =
      absl::StrCat(testing::TempDir(), "/fake_stale_seconds4");
  std::string contents(1 << 22, '9');
  SetContents(fake_stale_seconds, contents);

  ExpectStaleSecondsFailedReadFrom(fake_stale_seconds);
}

TEST(PageFlagsTest, GetSinglePageBitmapsErrorCases) {
  {
    PageFlagsFriend s;
    EXPECT_EQ(s.GetSinglePageBitmaps(reinterpret_cast<const void*>(1)).status,
              absl::StatusCode::kFailedPrecondition);
  }
  {
    PageFlagsFriend s("/dev/null/impossible");
    EXPECT_EQ(s.GetSinglePageBitmaps(nullptr).status,
              absl::StatusCode::kUnavailable);
  }
  {
    std::string fake_pageflags =
        absl::StrCat(testing::TempDir(), "/fake_pageflags_short");
    SetContents(fake_pageflags, "x");
    PageFlagsFriend s(fake_pageflags);
    EXPECT_EQ(s.GetSinglePageBitmaps(nullptr).status,
              absl::StatusCode::kUnavailable);
  }
  {
    std::string fake_pageflags =
        absl::StrCat(testing::TempDir(), "/fake_pageflags_tail");
    const size_t kHardwarePageSize = getpagesize();
    const size_t kHardwarePagesInHugePage = kHugePageSize / kHardwarePageSize;
    std::vector<uint64_t> data(kHardwarePagesInHugePage, kPageTail);

    std::string content(reinterpret_cast<const char*>(data.data()),
                        data.size() * sizeof(uint64_t));
    SetContents(fake_pageflags, content);

    PageFlagsFriend s(fake_pageflags);
    EXPECT_EQ(s.GetSinglePageBitmaps(nullptr).status,
              absl::StatusCode::kFailedPrecondition);
  }
}

TEST(PageFlagsTest, GetSinglePageBitmapsSuccess) {
  std::string fake_pageflags =
      absl::StrCat(testing::TempDir(), "/fake_pageflags_correct");
  // GetSinglePageBitmaps reads and scans kHugePageSize / GetPageSize()
  // entries, which is only kMaxResidencyBits on 4 KiB-page hosts.
  const size_t kHardwarePagesInHugePage = kHugePageSize / GetPageSize();
  std::vector<uint64_t> data(kHardwarePagesInHugePage, 0);

  for (size_t i = 0; i < kHardwarePagesInHugePage; ++i) {
    if (i % 2 == 0) {
      data[i] |= kPageStale;
    }
    if (i % 4 == 0) {
      data[i] |= kPageMlocked | kPageUnevictable;
    } else if (i % 4 == 1) {
      data[i] |= kPageUnevictable;
    }
    if (i % 8 == 0) {
      data[i] |= kPageIncompressible | kPageUnevictable;
    }
  }

  std::string content(reinterpret_cast<const char*>(data.data()),
                      data.size() * sizeof(uint64_t));
  SetContents(fake_pageflags, content);

  PageFlagsFriend s(fake_pageflags);
  constexpr uint64_t kSetScanSeconds = 63;
  s.SetCachedScanSeconds(kSetScanSeconds);

  auto ret = s.GetSinglePageBitmaps(nullptr);
  EXPECT_EQ(ret.status, absl::StatusCode::kOk);
  EXPECT_EQ(ret.stale.CountBits(), kHardwarePagesInHugePage / 2);
  EXPECT_EQ(ret.locked.CountBits(), kHardwarePagesInHugePage / 4);
  EXPECT_EQ(ret.incompressible.CountBits(), kHardwarePagesInHugePage / 8);
  EXPECT_EQ(ret.stale_scan_seconds, kSetScanSeconds);
}

TEST(PageFlagsTest, DeferredSplitThpFlags) {
  const size_t kHardwarePageSize = GetPageSize();
  const size_t kPagesPerHugepage = kHugePageSize / kHardwarePageSize;
  std::vector<uint64_t> data(4 * kPagesPerHugepage, 0);

  auto set_tails = [&](size_t huge_idx, size_t first_tail) {
    for (size_t i = first_tail; i < kPagesPerHugepage; ++i) {
      data[huge_idx * kPagesPerHugepage + i] = kPageTail | kPageThp;
    }
  };

  // Hugepage 0: intact mlocked THP.
  data[0] = kPageHead | kPageThp | kPageMlocked;
  set_tails(0, 1);

  // Hugepage 1: deferred-split THP with stale+incompressible compound head at
  // page 0, unbacked page 1 (0), native mlocked order-0 page 2, and tail pages
  // 3..N-1 inheriting the compound head's stale+incompressible flags.
  data[kPagesPerHugepage] =
      kPageHead | kPageThp | kPageStale | kPageIncompressible;
  data[kPagesPerHugepage + 2] = kPageMlocked;
  set_tails(1, 3);

  // Hugepages 2 and 3: orphaned tail pages 1..N-1 with unbacked (0) or native
  // order-0 (kPageMlocked) page 0.
  set_tails(2, 1);
  data[3 * kPagesPerHugepage] = kPageMlocked;
  set_tails(3, 1);

  std::string file_path =
      absl::StrCat(testing::TempDir(), "/deferred_split_thp_flags");
  SetContents(file_path,
              absl::string_view(reinterpret_cast<const char*>(data.data()),
                                data.size() * sizeof(data[0])));

  PageFlagsFriend s(file_path);
  constexpr uint64_t kSetScanSeconds = 63;
  s.SetCachedScanSeconds(kSetScanSeconds);

  const size_t kHeadBytes = (kPagesPerHugepage - 2) * kHardwarePageSize;
  const size_t kTailBytes = (kPagesPerHugepage - 3) * kHardwarePageSize;
  struct Case {
    uintptr_t offset;
    size_t size;
    PageStats expected;
  };
  for (const Case& tc : {
           Case{kHugePageSize, kHugePageSize,
                PageStats{kHeadBytes, kHardwarePageSize, kHeadBytes,
                          kSetScanSeconds}},
           Case{kHugePageSize + kHardwarePageSize,
                kHugePageSize - kHardwarePageSize,
                PageStats{kTailBytes, kHardwarePageSize, kTailBytes,
                          kSetScanSeconds}},
           Case{kHugePageSize + 2 * kHardwarePageSize + 100,
                kHugePageSize - 2 * kHardwarePageSize - 100,
                PageStats{kTailBytes, kHardwarePageSize - 100, kTailBytes,
                          kSetScanSeconds}},
           Case{kHugePageSize + 3 * kHardwarePageSize,
                kHugePageSize - 3 * kHardwarePageSize,
                PageStats{kTailBytes, 0, kTailBytes, kSetScanSeconds}},
           Case{0, 2 * kHugePageSize,
                PageStats{kHeadBytes, kHugePageSize + kHardwarePageSize,
                          kHeadBytes, kSetScanSeconds}},
       }) {
    SCOPED_TRACE(absl::StrCat("offset=", tc.offset, " size=", tc.size));
    EXPECT_THAT(s.Get(reinterpret_cast<const void*>(tc.offset), tc.size),
                Optional(tc.expected));
  }

  auto h1_bitmaps =
      s.GetSinglePageBitmaps(reinterpret_cast<const void*>(kHugePageSize));
  EXPECT_EQ(h1_bitmaps.status, absl::StatusCode::kOk);
  EXPECT_EQ(h1_bitmaps.stale.CountBits(), kPagesPerHugepage - 2);
  EXPECT_EQ(h1_bitmaps.locked.CountBits(), 1);
  EXPECT_TRUE(h1_bitmaps.locked.GetBit(2));
  EXPECT_EQ(h1_bitmaps.incompressible.CountBits(), kPagesPerHugepage - 2);

  for (size_t huge_idx : {size_t{2}, size_t{3}}) {
    const uintptr_t h = huge_idx * kHugePageSize;
    EXPECT_THAT(
        s.Get(reinterpret_cast<const void*>(h), kHardwarePageSize),
        Optional(FieldsAre(0, huge_idx == 3 ? kHardwarePageSize : 0, 0, 0)));
    EXPECT_EQ(s.Get(reinterpret_cast<const void*>(h), kHugePageSize),
              std::nullopt);
    EXPECT_EQ(s.Get(reinterpret_cast<const void*>(h + kHardwarePageSize),
                    kHardwarePageSize),
              std::nullopt);
    EXPECT_EQ(s.GetSinglePageBitmaps(reinterpret_cast<const void*>(h)).status,
              absl::StatusCode::kFailedPrecondition);
  }
  EXPECT_EQ(s.Get(nullptr, 3 * kHugePageSize), std::nullopt);
}

TEST(PageFlagsTest, DeferredSplitThp) {
  GTEST_SKIP() << "pageflags not commonly available";

#if ABSL_HAVE_ADDRESS_SANITIZER || ABSL_HAVE_MEMORY_SANITIZER || \
    ABSL_HAVE_THREAD_SANITIZER
  GTEST_SKIP() << "Skipped under sanitizers.";
#endif

  const size_t kHardwarePageSize = getpagesize();
  const size_t kSize = 3 * kHugePageSize;
  char* p = MmapWithHugepageMisalignment(kSize, /*misalignment=*/0);
  struct Unmapper {
    char* ptr;
    size_t size;
    ~Unmapper() { CHECK_EQ(munmap(ptr, size), 0) << errno; }
  } unmap_p{p, kSize};
  std::memset(p, 0x5a, kSize);

  if (madvise(p, kSize, MADV_COLLAPSE) != 0) {
    GTEST_SKIP() << "MADV_COLLAPSE unavailable or out of hugepages: " << errno;
  }
  ASSERT_EQ(madvise(p, kSize, MADV_NOHUGEPAGE), 0) << errno;

  PageFlags s;
  char* h0 = p;
  char* h1 = p + kHugePageSize;
  char* h2 = p + 2 * kHugePageSize;
  if (!s.IsHugepageBacked(h0).value_or(false) ||
      !s.IsHugepageBacked(h1).value_or(false) ||
      !s.IsHugepageBacked(h2).value_or(false)) {
    GTEST_SKIP() << "THP collapse did not materialize compound pages.";
  }

  auto wait_for_locked = [&](const void* addr, size_t len, size_t expected) {
    absl::Time start = absl::Now();
    std::optional<PageStats> res;
    do {
      res = s.Get(addr, len);
      ASSERT_TRUE(res.has_value());
      if (res->bytes_locked == expected) break;
      absl::SleepFor(absl::Milliseconds(10));
    } while (absl::Now() - start < absl::Seconds(60));
    ASSERT_THAT(res, Optional(FieldsAre(0, expected, 0, 0)));
  };

  // h0: intact mlocked 2 MiB THP (KPF_COMPOUND_HEAD | KPF_THP | KPF_MLOCKED on
  // page 0, KPF_COMPOUND_TAIL | KPF_THP on pages 1..511).
  ASSERT_EQ(mlock(h0, kHugePageSize), 0) << errno;
  wait_for_locked(h0, kHugePageSize, kHugePageSize);

  // h1: deferred-split THP with interior pages 1 and 2 unbacked via
  // MADV_DONTNEED (splitting the PMD into PTEs while leaving page 0 as
  // PageHead | PageThp and pages 3..511 as PageTail | PageThp), and page 2
  // refaulted and mlocked as a native order-0 page. Under memory pressure, the
  // kernel's deferred_split shrinker may asynchronously split a partially
  // unmapped THP into order-0 native pages; retry once only if a split occurred
  // mid-query so tail pages are no longer hugepage-backed.
  ASSERT_EQ(
      madvise(h1 + kHardwarePageSize, 2 * kHardwarePageSize, MADV_DONTNEED), 0)
      << errno;
  h1[2 * kHardwarePageSize] = 1;
  ASSERT_EQ(mlock(h1 + 2 * kHardwarePageSize, kHardwarePageSize), 0) << errno;
  wait_for_locked(h1 + 2 * kHardwarePageSize, kHardwarePageSize,
                  kHardwarePageSize);

  auto h1_bitmaps = s.GetSinglePageBitmaps(h1);
  if (h1_bitmaps.status != absl::StatusCode::kOk) {
    absl::SleepFor(absl::Milliseconds(10));
    if (!s.IsHugepageBacked(h1 + 3 * kHardwarePageSize).value_or(true)) {
      h1_bitmaps = s.GetSinglePageBitmaps(h1);
    }
  }
  EXPECT_EQ(h1_bitmaps.status, absl::StatusCode::kOk);
  EXPECT_EQ(h1_bitmaps.locked.CountBits(), 1);
  EXPECT_TRUE(h1_bitmaps.locked.GetBit(2));

  // Querying h1 from page 0 (aligned and unaligned), from unbacked page 1,
  // from refaulted+mlocked page 2, from tail page 3, and across hugepage
  // boundaries from h0.
  struct RangeCase {
    const char* addr;
    size_t size;
    size_t expected_locked;
  };
  for (const RangeCase& tc : {
           RangeCase{h1, kHugePageSize, kHardwarePageSize},
           RangeCase{h1 + 100, kHugePageSize - 200, kHardwarePageSize},
           RangeCase{h1 + kHardwarePageSize, kHugePageSize - kHardwarePageSize,
                     kHardwarePageSize},
           RangeCase{h1 + 2 * kHardwarePageSize + 100,
                     kHugePageSize - 2 * kHardwarePageSize - 100,
                     kHardwarePageSize - 100},
           RangeCase{h1 + 3 * kHardwarePageSize,
                     kHugePageSize - 3 * kHardwarePageSize, 0},
           RangeCase{h0, 2 * kHugePageSize, kHugePageSize + kHardwarePageSize},
           RangeCase{h0 + kHugePageSize / 2, kHugePageSize,
                     kHugePageSize / 2 + kHardwarePageSize},
       }) {
    SCOPED_TRACE(absl::StrCat("offset=", tc.addr - p, " size=", tc.size));
    auto res = s.Get(tc.addr, tc.size);
    if (!res.has_value()) {
      absl::SleepFor(absl::Milliseconds(10));
      if (!s.IsHugepageBacked(h1 + 3 * kHardwarePageSize).value_or(true)) {
        res = s.Get(tc.addr, tc.size);
      }
    }
    EXPECT_THAT(res, Optional(FieldsAre(0, tc.expected_locked, 0, 0)));
  }

  // h2: unbacking page 0 (the compound head) leaves page 0 as KPF_NOPAGE while
  // pages 1..511 remain orphaned PageTail | PageThp pages; refaulting and
  // mlocking page 0 then installs a native order-0 page (!PageHead &&
  // !PageTail) at the 2 MiB base. While the tail pages remain compound THP
  // tails, any query reaching them fails; if the kernel's deferred_split
  // shrinker splits h2 into order-0 pages, queries report the order-0 stats.
  ASSERT_EQ(madvise(h2, kHardwarePageSize, MADV_DONTNEED), 0) << errno;
  for (bool refault_head : {false, true}) {
    SCOPED_TRACE(refault_head);
    const size_t head_locked = refault_head ? kHardwarePageSize : 0;
    if (refault_head) {
      h2[0] = 1;
      ASSERT_EQ(mlock(h2, kHardwarePageSize), 0) << errno;
    }
    wait_for_locked(h2, kHardwarePageSize, head_locked);
    for (const RangeCase& tc : {
             RangeCase{h2, kHugePageSize, head_locked},
             RangeCase{h2 + kHardwarePageSize, kHardwarePageSize, 0},
             RangeCase{h0, 3 * kHugePageSize,
                       kHugePageSize + kHardwarePageSize + head_locked},
         }) {
      SCOPED_TRACE(absl::StrCat("offset=", tc.addr - p, " size=", tc.size));
      auto res = s.Get(tc.addr, tc.size);
      if (s.IsHugepageBacked(h2 + kHardwarePageSize).value_or(false)) {
        EXPECT_EQ(res, std::nullopt);
      } else if (res.has_value()) {
        EXPECT_THAT(res, Optional(FieldsAre(0, tc.expected_locked, 0, 0)));
      }
    }
    auto h2_bitmaps = s.GetSinglePageBitmaps(h2);
    if (s.IsHugepageBacked(h2 + kHardwarePageSize).value_or(false)) {
      EXPECT_EQ(h2_bitmaps.status, absl::StatusCode::kFailedPrecondition);
    } else if (h2_bitmaps.status == absl::StatusCode::kOk) {
      EXPECT_EQ(h2_bitmaps.locked.CountBits(), refault_head ? 1 : 0);
    }
  }
}

TEST(PageFlagsTest, Mthp) {
  const size_t kHardwarePageSize = GetPageSize();
  const size_t kPagesPerHugepage = kHugePageSize / kHardwarePageSize;
  constexpr size_t kMthpPages = 4;
  ASSERT_GE(kPagesPerHugepage, 5 * kMthpPages);
  std::vector<uint64_t> data(kPagesPerHugepage, 0);

  auto set_mthp = [&](size_t start_page, uint64_t head_flags) {
    data[start_page] = kPageHead | kPageThp | head_flags;
    for (size_t i = 1; i < kMthpPages; ++i) {
      data[start_page + i] = kPageTail | kPageThp;
    }
  };

  // Pages 0..3: intact stale mTHP.
  set_mthp(0, kPageStale);
  // Pages 4..7: native order-0 pages (page 4 mlocked, pages 5..7 clean).
  data[4] = kPageMlocked;
  // Pages 8..11: intact incompressible mTHP.
  set_mthp(8, kPageIncompressible);
  // Pages 12..15: deferred-split mlocked mTHP (page 13 unbacked, page 14
  // refaulted stale order-0 page, page 15 tail inheriting mlocked from 12).
  set_mthp(12, kPageMlocked);
  data[13] = 0;
  data[14] = kPageStale;
  // Pages 16..19: intact clean mTHP (resets last_head_read_ to clean flags).
  set_mthp(16, 0);

  std::string file_path = absl::StrCat(testing::TempDir(), "/mthp_flags");
  SetContents(file_path,
              absl::string_view(reinterpret_cast<const char*>(data.data()),
                                data.size() * sizeof(data[0])));

  PageFlagsFriend s(file_path);
  constexpr uint64_t kSetScanSeconds = 63;
  s.SetCachedScanSeconds(kSetScanSeconds);

  const size_t p = kHardwarePageSize;
  struct Case {
    uintptr_t offset;
    size_t size;
    PageStats expected;
  };
  for (const Case& tc : {
           Case{0, kHugePageSize,
                PageStats{5 * p, 3 * p, 4 * p, kSetScanSeconds}},
           Case{0, 8 * p, PageStats{4 * p, p, 0, kSetScanSeconds}},
           Case{p, 19 * p, PageStats{4 * p, 3 * p, 4 * p, kSetScanSeconds}},
           Case{4 * p, 16 * p, PageStats{p, 3 * p, 4 * p, kSetScanSeconds}},
           Case{8 * p + 100, 4 * p - 100, PageStats{0, 0, 4 * p - 100, 0}},
           Case{12 * p, 8 * p, PageStats{p, 2 * p, 0, kSetScanSeconds}},
       }) {
    SCOPED_TRACE(absl::StrCat("offset=", tc.offset, " size=", tc.size));
    EXPECT_THAT(s.Get(reinterpret_cast<const void*>(tc.offset), tc.size),
                Optional(tc.expected));
  }

  auto bitmaps = s.GetSinglePageBitmaps(nullptr);
  EXPECT_EQ(bitmaps.status, absl::StatusCode::kOk);
  EXPECT_EQ(bitmaps.stale.CountBits(), 5);
  EXPECT_EQ(bitmaps.locked.CountBits(), 3);
  EXPECT_EQ(bitmaps.incompressible.CountBits(), 4);
}

// Supporting Get() starting mid-mTHP on a tail page of a non-2 MiB-aligned mTHP
// requires locating that mTHP's head rather than rewinding to
// vaddr & kHugePageMask (which either reads an unrelated mTHP head at page 0 or
// fails when page 0 is native/unbacked). Note that /proc/self/pageflags does
// not expose compound folio order, so if sub-pages are unbacked,
// [Head@0, Tail@1..3, 0@4, Tail@5..7] is indistinguishable between an 8-page
// deferred-split mTHP with interior page 4 unbacked and a 4-page mTHP at 0..3
// followed by a headless deferred-split mTHP at 4..7 with head page 4 unbacked.
TEST(PageFlagsTest, DISABLED_MthpUnalignedTailStart) {
  const size_t kHardwarePageSize = GetPageSize();
  const size_t kPagesPerHugepage = kHugePageSize / kHardwarePageSize;
  constexpr size_t kMthpPages = 4;
  ASSERT_GE(kPagesPerHugepage, 2 * kMthpPages);
  std::vector<uint64_t> data(2 * kPagesPerHugepage, 0);

  auto set_mthp = [&](size_t start_page, uint64_t head_flags) {
    data[start_page] = kPageHead | kPageThp | head_flags;
    for (size_t i = 1; i < kMthpPages; ++i) {
      data[start_page + i] = kPageTail | kPageThp;
    }
  };

  // Hugepage 0: stale mTHP at pages 0..3, mlocked mTHP at pages 4..7.
  // Starting at tail page 5 currently rewinds to page 0 and misattributes stale
  // instead of mlocked.
  set_mthp(0, kPageStale);
  set_mthp(4, kPageMlocked);

  // Hugepage 1: native pages at 0..3, mlocked mTHP at pages 4..7.
  // Starting at tail page 5 currently rewinds to page 0 (!PageHead) and returns
  // std::nullopt.
  set_mthp(kPagesPerHugepage + 4, kPageMlocked);

  std::string file_path =
      absl::StrCat(testing::TempDir(), "/mthp_unaligned_tail_start");
  SetContents(file_path,
              absl::string_view(reinterpret_cast<const char*>(data.data()),
                                data.size() * sizeof(data[0])));

  PageFlagsFriend s(file_path);
  s.SetCachedScanSeconds(63);
  const size_t p = kHardwarePageSize;
  for (uintptr_t base : {uintptr_t{0}, uintptr_t{kHugePageSize}}) {
    SCOPED_TRACE(base);
    EXPECT_THAT(s.Get(reinterpret_cast<const void*>(base + 5 * p), 3 * p),
                Optional(PageStats{0, 3 * p, 0, 0}));
  }
}

}  // namespace
}  // namespace tcmalloc_internal
}  // namespace tcmalloc
