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

#include "tcmalloc/internal/residency.h"

#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>

#ifndef MADV_GUARD_INSTALL
#define MADV_GUARD_INSTALL 102
#endif

#ifndef MADV_COLD
#define MADV_COLD 20
#endif

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "benchmark/benchmark.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "absl/log/check.h"
#include "absl/status/status.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/string_view.h"
#include "tcmalloc/internal/allocation_guard.h"
#include "tcmalloc/internal/config.h"
#include "tcmalloc/internal/is_aligned_to.h"
#include "tcmalloc/internal/page_size.h"
#include "tcmalloc/internal/range_tracker.h"
#include "tcmalloc/internal/util.h"
#include "tcmalloc/testing/smaps.h"

namespace tcmalloc {
namespace tcmalloc_internal {

class ResidencySpouse {
 public:
  explicit ResidencySpouse() = default;
  explicit ResidencySpouse(const char* const filename) : r_(filename) {}
  explicit ResidencySpouse(absl::string_view filename) : r_(filename.data()) {}

  template <typename... Args>
  decltype(auto) Get(Args&&... args) {
    AllocationGuard g;
    return r_.Get(std::forward<Args>(args)...);
  }

  decltype(auto) GetUnbackedAndSwappedBitmaps(const void* const addr) {
    AllocationGuard g;
    return r_.GetUnbackedAndSwappedBitmaps(addr);
  }

 private:
  ResidencyPageMap r_;
};

namespace {

using ::testing::_;
using ::testing::FieldsAre;
using ::testing::Optional;

constexpr uint64_t kPageExclusive = (1ULL << 56);
constexpr uint64_t kPageGuardRegion = (1ULL << 58);
constexpr uint64_t kPageSwapped = (1ULL << 62);
constexpr uint64_t kPagePresent = (1ULL << 63);

TEST(ResidenceTest, ThisProcess) {
  const size_t kHardwarePageSize = GetPageSize();
  const int kNumPages = 16;

  // Try both private and shared mappings to make sure we have the bit order of
  // /proc/pid/pageflags correct.
  for (const int flags :
       {MAP_ANONYMOUS | MAP_SHARED, MAP_ANONYMOUS | MAP_PRIVATE}) {
    const size_t kHead = kHardwarePageSize * 10;
    const size_t kTail = kHardwarePageSize * 10;

    std::optional<AllocationGuard> g;
    g.emplace();
    ResidencyPageMap r;
    g.reset();
    auto get = [&](const void* addr, size_t size) {
      AllocationGuard guard;
      return r.Get(addr, size);
    };
    // Overallocate kNumPages of memory, so we can munmap the page before and
    // after it.
    void* p = mmap(nullptr, kNumPages * kHardwarePageSize + kHead + kTail,
                   PROT_READ | PROT_WRITE, flags, -1, 0);
    ASSERT_NE(p, MAP_FAILED) << errno;

    EXPECT_THAT(get(p, (kNumPages + 2) * kHardwarePageSize),
                Optional(FieldsAre(0, 0, _)));
    void* q = reinterpret_cast<char*>(p) + kHead;
    void* last =
        reinterpret_cast<char*>(p) + kNumPages * kHardwarePageSize + kHead;
    ASSERT_EQ(munmap(p, kHardwarePageSize), 0);
    ASSERT_EQ(munmap(last, kHardwarePageSize), 0);

    EXPECT_THAT(get(p, kHead), Optional(FieldsAre(0, 0, _)));
    EXPECT_THAT(get(last, kTail), Optional(FieldsAre(0, 0, _)));

    memset(q, 0, kNumPages * kHardwarePageSize);
    (void)mlock(q, kNumPages * kHardwarePageSize);
    ::benchmark::DoNotOptimize(q);

    EXPECT_THAT(get(p, kHead), Optional(FieldsAre(0, 0, _)));
    EXPECT_THAT(get(last, kTail), Optional(FieldsAre(0, 0, _)));

    EXPECT_THAT(get(q, kHardwarePageSize),
                Optional(FieldsAre(kHardwarePageSize, 0, _)));

    EXPECT_THAT(get(q, (kNumPages + 2) * kHardwarePageSize),
                Optional(FieldsAre(kHardwarePageSize * kNumPages, 0, _)));

    EXPECT_THAT(get(reinterpret_cast<char*>(q) + 7, kHardwarePageSize - 7),
                Optional(FieldsAre(kHardwarePageSize - 7, 0, _)));

    EXPECT_THAT(get(reinterpret_cast<char*>(q) + 7, kHardwarePageSize),
                Optional(FieldsAre(kHardwarePageSize, 0, _)));

    EXPECT_THAT(get(reinterpret_cast<char*>(q) + 7, 3 * kHardwarePageSize),
                Optional(FieldsAre(kHardwarePageSize * 3, 0, _)));

    EXPECT_THAT(
        get(reinterpret_cast<char*>(q) + 7, kNumPages * kHardwarePageSize),
        Optional(FieldsAre(kHardwarePageSize * kNumPages - 7, 0, _)));

    EXPECT_THAT(
        get(reinterpret_cast<char*>(q) + 7, kNumPages * kHardwarePageSize - 7),
        Optional(FieldsAre(kHardwarePageSize * kNumPages - 7, 0, _)));

    EXPECT_THAT(get(reinterpret_cast<char*>(q) + 7,
                    (kNumPages + 1) * kHardwarePageSize),
                Optional(FieldsAre(kHardwarePageSize * kNumPages - 7, 0, _)));

    EXPECT_THAT(get(reinterpret_cast<char*>(q) + 7,
                    (kNumPages + 1) * kHardwarePageSize - 7),
                Optional(FieldsAre(kHardwarePageSize * kNumPages - 7, 0, _)));

    std::optional<Residency::Info> q_info =
        get(q, kNumPages * kHardwarePageSize);
    ASSERT_TRUE(q_info.has_value());
    EXPECT_THAT(*q_info, FieldsAre(kHardwarePageSize * kNumPages, 0, _));
    for (size_t i = 0; i < kNumPages; ++i) {
      EXPECT_TRUE(q_info->page_is_resident.GetBit(i));
    }

    ASSERT_EQ(munmap(q, kNumPages * kHardwarePageSize), 0);
  }
}

TEST(ResidenceTest, CannotOpen) {
  std::optional<Residency::Info> res;
  {
    AllocationGuard g;
    ResidencySpouse r("/tmp/a667ba48-18ba-4523-a8a7-b49ece3a6c2b");
    res = r.Get(nullptr, 1);
  }
  EXPECT_FALSE(res.has_value());
}

TEST(ResidenceTest, CannotRead) {
  std::optional<Residency::Info> res;
  {
    AllocationGuard g;
    ResidencySpouse r("/dev/null");
    res = r.Get(nullptr, 1);
  }
  EXPECT_FALSE(res.has_value());
}

TEST(ResidenceTest, CannotSeek) {
  std::optional<Residency::Info> res;
  {
    AllocationGuard g;
    ResidencySpouse r("/dev/null");
    res = r.Get(&r, 1);
  }
  EXPECT_FALSE(res.has_value());
}

TEST(ResidenceTest, GuardPagesNotCountedAsSwapped) {
  std::string file_path =
      absl::StrCat(testing::TempDir(), "/guard_pages_residency");
  int write_fd = signal_safe_open(
      file_path.c_str(), O_CREAT | O_WRONLY | O_TRUNC, S_IRUSR | S_IWUSR);
  ASSERT_NE(write_fd, -1) << errno;
  const uint64_t entries[3] = {
      kPageSwapped | kPageGuardRegion,
      kPagePresent | kPageExclusive,
      kPageSwapped,
  };
  ASSERT_EQ(write(write_fd, entries, sizeof(entries)), sizeof(entries));
  ASSERT_EQ(close(write_fd), 0);

  const size_t kHardwarePageSize = GetPageSize();
  std::optional<Residency::Info> info;
  {
    AllocationGuard g;
    ResidencySpouse r(file_path);
    info = r.Get(nullptr, 3 * kHardwarePageSize);
  }
  ASSERT_TRUE(info.has_value());
  EXPECT_EQ(info->bytes_resident, kHardwarePageSize);
  EXPECT_EQ(info->bytes_swapped, kHardwarePageSize);
  EXPECT_FALSE(info->page_is_resident.GetBit(0));
  EXPECT_TRUE(info->page_is_resident.GetBit(1));
  EXPECT_FALSE(info->page_is_resident.GetBit(2));
}

// Anonymous memory that is a VMA of its own: it is surrounded by PROT_NONE
// guard regions, so the kernel cannot merge it with neighboring mappings.
class IsolatedAnonMapping {
 public:
  IsolatedAnonMapping(size_t size, size_t alignment)
      : reservation_size_(size + 2 * alignment) {
    reservation_ = mmap(nullptr, reservation_size_, PROT_NONE,
                        MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    CHECK_NE(reservation_, MAP_FAILED) << errno;
    data_ = reinterpret_cast<char*>((reinterpret_cast<uintptr_t>(reservation_) +
                                     GetPageSize() + alignment - 1) &
                                    ~(alignment - 1));
    CHECK_EQ(mprotect(data_, size, PROT_READ | PROT_WRITE), 0) << errno;
  }

  ~IsolatedAnonMapping() {
    CHECK_EQ(munmap(reservation_, reservation_size_), 0) << errno;
  }

  IsolatedAnonMapping(const IsolatedAnonMapping&) = delete;
  IsolatedAnonMapping& operator=(const IsolatedAnonMapping&) = delete;

  char* data() const { return data_; }

 private:
  const size_t reservation_size_;
  void* reservation_;
  char* data_;
};

// Checks that ResidencyPageMap agrees with the kernel's smaps Rss for
// [addr, addr + size), which must be a VMA of its own. Returns Rss in `rss`.
void CheckResidencyMatchesSmaps(void* addr, size_t size, size_t& rss) {
  std::optional<SmapEntry> vma;
  ASSERT_TRUE(ForEachSmapEntry([&](const SmapEntry& entry) {
    if (entry.Overlaps(addr, size)) vma = entry;
  }));
  ASSERT_TRUE(vma.has_value());
  ASSERT_EQ(vma->start, reinterpret_cast<uintptr_t>(addr));
  ASSERT_EQ(vma->end - vma->start, size);

  std::optional<Residency::Info> info;
  {
    AllocationGuard g;
    ResidencyPageMap r;
    info = r.Get(addr, size);
  }
  ASSERT_TRUE(info.has_value());
  EXPECT_EQ(info->bytes_resident, vma->rss_bytes);
  EXPECT_EQ(info->page_is_resident.CountBits() * GetPageSize(), vma->rss_bytes);
  rss = vma->rss_bytes;
}

void ReadEveryPage(const void* p, size_t size) {
  const volatile char* v = static_cast<const char*>(p);
  for (size_t i = 0; i < size; i += GetPageSize()) {
    (void)v[i];
  }
}

TEST(ResidenceSmapsTest, TouchedAnon) {
  const size_t kSize = 16 * GetPageSize();
  IsolatedAnonMapping m(kSize, GetPageSize());
  ASSERT_EQ(madvise(m.data(), kSize, MADV_NOHUGEPAGE), 0) << errno;
  memset(m.data(), 1, kSize);

  size_t rss;
  ASSERT_NO_FATAL_FAILURE(CheckResidencyMatchesSmaps(m.data(), kSize, rss));
  EXPECT_EQ(rss, kSize);
}

TEST(ResidenceSmapsTest, ReadFaultedAnon) {
  // Read faults on untouched private anonymous memory map the shared zero
  // page, which is present in pagemap but not part of Rss.
  const size_t kSize = 16 * GetPageSize();
  IsolatedAnonMapping m(kSize, GetPageSize());
  ASSERT_EQ(madvise(m.data(), kSize, MADV_NOHUGEPAGE), 0) << errno;
  ReadEveryPage(m.data(), kSize);

  size_t rss;
  ASSERT_NO_FATAL_FAILURE(CheckResidencyMatchesSmaps(m.data(), kSize, rss));
  EXPECT_EQ(rss, size_t{0});
}

TEST(ResidenceSmapsTest, ThpSplit) {
  IsolatedAnonMapping m(kHugePageSize, kHugePageSize);
  if (madvise(m.data(), kHugePageSize, MADV_HUGEPAGE) != 0) {
    GTEST_SKIP() << "MADV_HUGEPAGE not supported: " << errno;
  }
  m.data()[0] = 1;
  // Intact THP: all subpages count.
  size_t rss;
  ASSERT_NO_FATAL_FAILURE(
      CheckResidencyMatchesSmaps(m.data(), kHugePageSize, rss));
  if (rss != kHugePageSize) {
    GTEST_SKIP() << "Mapping is not backed by a THP";
  }

  // MADV_COLD on part of the THP splits it. Newer kernels then remap the
  // untouched (all-zero) subpages to the shared zero page.
  if (madvise(m.data() + 100 * GetPageSize(), GetPageSize(), MADV_COLD) != 0) {
    GTEST_SKIP() << "MADV_COLD not supported: " << errno;
  }
  // Keep khugepaged from collapsing the range again between the reads below.
  ASSERT_EQ(madvise(m.data(), kHugePageSize, MADV_NOHUGEPAGE), 0) << errno;
  ASSERT_NO_FATAL_FAILURE(
      CheckResidencyMatchesSmaps(m.data(), kHugePageSize, rss));
}

TEST(ResidenceSmapsTest, FileBackedMappedTwice) {
  const size_t kSize = 16 * GetPageSize();
  std::string path = absl::StrCat(testing::TempDir(), "/residency_smaps_file");
  int fd = signal_safe_open(path.c_str(), O_CREAT | O_RDWR | O_TRUNC,
                            S_IRUSR | S_IWUSR);
  ASSERT_NE(fd, -1) << errno;
  std::vector<char> contents(kSize, 1);
  ASSERT_EQ(write(fd, contents.data(), kSize), kSize);

  // Map the file twice, so every page is mapped twice: PM_MMAP_EXCLUSIVE is
  // clear and only PM_FILE marks the pages as resident. File mappings never
  // merge with anonymous ones, nor with each other at the same file offset.
  void* mappings[2];
  for (void*& m : mappings) {
    m = mmap(nullptr, kSize, PROT_READ, MAP_PRIVATE, fd, 0);
    ASSERT_NE(m, MAP_FAILED) << errno;
  }
  ASSERT_EQ(close(fd), 0) << errno;
  for (void* m : mappings) ReadEveryPage(m, kSize);

  for (void* m : mappings) {
    size_t rss;
    ASSERT_NO_FATAL_FAILURE(CheckResidencyMatchesSmaps(m, kSize, rss));
    EXPECT_EQ(rss, kSize);
    ASSERT_EQ(munmap(m, kSize), 0) << errno;
  }
}

// Method that can write a region with a single hugepage
// a region with a single missing page, a region with every other page missing,
// a region with all missing pages, or a region with a hugepage in the middle.
void GenerateHolesInSinglePage(absl::string_view filename, int case_num,
                               int num_pages) {
  int write_fd =
      signal_safe_open(filename.data(), O_CREAT | O_WRONLY, S_IRUSR | S_IWUSR);
  CHECK_NE(write_fd, -1) << errno;
  std::vector<uint64_t> buf(num_pages, 0);
  for (int i = 0; i < num_pages; i++) {
    switch (case_num) {
      case 0:
        // All pages are present
        buf[i] = kPagePresent;
        break;
      case 1:
        // All Pages are swapped
        buf[i] = kPageSwapped;
        break;
      case 2:
        // All pages are holes
        buf[i] = 0;
        break;
      case 3:
        // Every other page is a hole, rest are present
        if (i % 2 == 0) {
          buf[i] = 0;
        } else if (i % 2 == 1) {
          buf[i] = kPagePresent;
        }
        break;
      case 4:
        // Every other page is swapped, rest are present
        if (i % 2 == 0) {
          buf[i] = kPageSwapped;
        } else if (i % 2 == 1) {
          buf[i] = kPagePresent;
        }
        break;
      case 5:
        // Every other page is swapped, rest are holes
        if (i % 2 == 0) {
          buf[i] = kPageSwapped;
        } else if (i % 2 == 1) {
          buf[i] = 0;
        }
        break;
      case 6:
        // Every other page is a MADV_GUARD_INSTALL guard page (PM_SWAP |
        // PM_GUARD_REGION), rest are present. Guard pages must be treated as
        // unbacked rather than swapped.
        if (i % 2 == 0) {
          buf[i] = kPageSwapped | kPageGuardRegion;
        } else {
          buf[i] = kPagePresent;
        }
        break;
    }
  }
  int size_of_write = num_pages * sizeof(uint64_t);
  CHECK_EQ(write(write_fd, buf.data(), size_of_write), size_of_write);
  CHECK_EQ(close(write_fd), 0) << errno;
}

Residency::SinglePageBitmaps GenerateExpectedSinglePageBitmaps(int case_num) {
  Bitmap<512> expected_unbacked;
  Bitmap<512> expected_swapped;
  switch (case_num) {
    case 0:
      // All Pages are present. Both bitmaps are 0
      break;
    case 1:
      // All Pages are swapped. Swapped bitmap is all 1
      expected_swapped.SetRange(0, 512);
      break;
    case 2:
      // All pages are holes. Unbacked bitmap is all 1
      expected_unbacked.SetRange(0, 512);
      break;
    case 3:
      // Every other page is a hole, rest are present.
      // Both bitmaps are 0 and 1 alternating
      for (int idx = 0; idx < 512; idx += 2) {
        if (idx % 2 == 0) {
          expected_unbacked.SetBit(idx);
        }
      }
      break;
    case 4:
      // Every other page is swapped, rest are present,
      // Swapped bitmap are 0 and 1 alternating
      for (int idx = 0; idx < 512; idx += 2) {
        if (idx % 2 == 0) {
          expected_swapped.SetBit(idx);
        }
      }
      break;
    case 5:
      // Every other page is swapped, rest are holes,
      // Swapped bitmaps are 0 and 1 alternating
      // Unbacked bitmaps are 1 and 0 alternating
      for (int idx = 0; idx < 512; idx++) {
        if (idx % 2 == 0) {
          expected_swapped.SetBit(idx);
        } else {
          expected_unbacked.SetBit(idx);
        }
      }
      break;
    case 6:
      // Every other page is a MADV_GUARD_INSTALL guard page, rest are present.
      for (int idx = 0; idx < 512; idx += 2) {
        expected_unbacked.SetBit(idx);
      }
      break;
  }
  return Residency::SinglePageBitmaps{expected_unbacked, expected_swapped,
                                      absl::StatusCode::kOk};
}

// Method that compares two bitmaps to see if they are equivalent
bool BitmapsAreEqual(const Bitmap<512>& bitmap1, const Bitmap<512>& bitmap2) {
  for (int i = 0; i < 512; ++i) {
    if (bitmap1.GetBit(i) != bitmap2.GetBit(i)) {
      return false;
    }
  }
  return true;
}

TEST(PageMapTest, GetUnbackedAndSwappedBitmaps) {
  constexpr int kNumCases = 7;
  std::array<Residency::SinglePageBitmaps, kNumCases> expected;
  for (int i = 0; i < kNumCases; ++i) {
    expected[i] = GenerateExpectedSinglePageBitmaps(i);
  }

  std::optional<AllocationGuard> g;
  for (int i = 0; i < kNumCases; ++i) {
    std::string file_path =
        absl::StrCat(testing::TempDir(), "/holes_in_single_page_", i);
    GenerateHolesInSinglePage(file_path, /*case_num=*/i,
                              /*num_pages=*/512);

    g.emplace();
    ResidencySpouse s(file_path);
    Residency::SinglePageBitmaps res =
        s.GetUnbackedAndSwappedBitmaps(reinterpret_cast<void*>(0));
    g.reset();
    EXPECT_THAT(res.status, expected[i].status);
    EXPECT_TRUE(BitmapsAreEqual(res.unbacked, expected[i].unbacked));
    EXPECT_TRUE(BitmapsAreEqual(res.swapped, expected[i].swapped));
  }
}

TEST(PageMapTest, CountHolesWithAddressBeyondFirstPage) {
  std::optional<AllocationGuard> g;
  std::string file_path =
      absl::StrCat(testing::TempDir(), "/holes_in_single_page");
  GenerateHolesInSinglePage(file_path, /*case_num=*/5,
                            /*num_pages=*/2048);
  Residency::SinglePageBitmaps expected = GenerateExpectedSinglePageBitmaps(5);
  g.emplace();
  ResidencySpouse s(file_path);
  Residency::SinglePageBitmaps res =
      s.GetUnbackedAndSwappedBitmaps(reinterpret_cast<void*>(2 << 21));
  g.reset();
  EXPECT_THAT(res.status, expected.status);
  EXPECT_TRUE(BitmapsAreEqual(res.unbacked, expected.unbacked));
  EXPECT_TRUE(BitmapsAreEqual(res.swapped, expected.swapped));
}

TEST(PageMapTest, VerifyAddressAlignmentCheckPasses) {
  std::optional<AllocationGuard> g;
  std::string file_path = absl::StrCat(testing::TempDir(), "/alignment_check");
  GenerateHolesInSinglePage(file_path, /*case_num=*/0,
                            /*num_pages=*/512);
  g.emplace();
  ResidencySpouse s(file_path);
  Residency::SinglePageBitmaps non_align_addr_res =
      s.GetUnbackedAndSwappedBitmaps(reinterpret_cast<void*>(0x00001));
  g.reset();
  EXPECT_EQ(non_align_addr_res.status, absl::StatusCode::kFailedPrecondition);
}

TEST(PageMapTest, VerifyAddressAlignmentBeyondFirstPageFails) {
  std::optional<AllocationGuard> g;
  g.emplace();
  ResidencySpouse s;
  Residency::SinglePageBitmaps res =
      s.GetUnbackedAndSwappedBitmaps(reinterpret_cast<void*>((2 << 21) + 1));
  g.reset();
  EXPECT_EQ(res.status, absl::StatusCode::kFailedPrecondition);
}

TEST(PageMapIntegrationTest, WorksOnActualData) {
  std::optional<AllocationGuard> g;
  void* addr = mmap(nullptr, 4 << 20, PROT_WRITE,
                    MAP_ANONYMOUS | MAP_POPULATE | MAP_PRIVATE, -1, 0);
  ASSERT_NE(addr, MAP_FAILED) << errno;
  auto position = reinterpret_cast<uintptr_t>(addr);
  if (!IsAlignedTo(position, kHugePageSize)) {
    position |= kHugePageSize - 1;
    position++;
    addr = reinterpret_cast<void*>(position);
  }
  g.emplace();
  ResidencyPageMap r;
  Residency::SinglePageBitmaps res = r.GetUnbackedAndSwappedBitmaps(addr);
  g.reset();
  ASSERT_EQ(res.status, absl::StatusCode::kOk);
  EXPECT_TRUE(res.unbacked.IsZero());
  EXPECT_TRUE(res.swapped.IsZero());
  ASSERT_EQ(munmap(reinterpret_cast<uint8_t*>(addr) + 1 * 4096, 4096), 0)
      << errno;
  ASSERT_EQ(munmap(reinterpret_cast<uint8_t*>(addr) + 17 * 4096, 4096), 0)
      << errno;

  g.emplace();
  res = r.GetUnbackedAndSwappedBitmaps(addr);
  g.reset();
  ASSERT_EQ(res.status, absl::StatusCode::kOk);
  EXPECT_FALSE(res.unbacked.IsZero());
  ASSERT_TRUE(res.unbacked.GetBit(1));
  ASSERT_TRUE(res.unbacked.GetBit(17));
  res.unbacked.ClearLowestBit();
  res.unbacked.ClearLowestBit();
  EXPECT_TRUE(res.unbacked.IsZero());
  EXPECT_TRUE(res.swapped.IsZero());
}

TEST(PageMapIntegrationTest, GuardPages) {
  const size_t kHardwarePageSize = GetPageSize();
  void* raw = mmap(nullptr, 2 * kHugePageSize, PROT_READ | PROT_WRITE,
                   MAP_ANONYMOUS | MAP_POPULATE | MAP_PRIVATE, -1, 0);
  ASSERT_NE(raw, MAP_FAILED) << errno;

  uint8_t* addr = reinterpret_cast<uint8_t*>(
      (reinterpret_cast<uintptr_t>(raw) + kHugePageSize - 1) &
      ~(kHugePageSize - 1));
  if (madvise(addr + kHardwarePageSize, kHardwarePageSize,
              MADV_GUARD_INSTALL) != 0) {
    ASSERT_EQ(munmap(raw, 2 * kHugePageSize), 0) << errno;
    GTEST_SKIP() << "MADV_GUARD_INSTALL not supported by kernel";
  }

  std::optional<AllocationGuard> g;
  g.emplace();
  ResidencyPageMap r;
  std::optional<Residency::Info> info = r.Get(addr, kHugePageSize);
  Residency::SinglePageBitmaps res = r.GetUnbackedAndSwappedBitmaps(addr);
  g.reset();

  ASSERT_THAT(info,
              Optional(FieldsAre(kHugePageSize - kHardwarePageSize, 0, _)));
  EXPECT_TRUE(info->page_is_resident.GetBit(0));
  EXPECT_FALSE(info->page_is_resident.GetBit(1));

  ASSERT_EQ(res.status, absl::StatusCode::kOk);
  EXPECT_EQ(res.unbacked.CountBits(), 1);
  EXPECT_TRUE(res.unbacked.GetBit(1));
  EXPECT_TRUE(res.swapped.IsZero());

  ASSERT_EQ(munmap(raw, 2 * kHugePageSize), 0) << errno;
}

}  // namespace
}  // namespace tcmalloc_internal
}  // namespace tcmalloc
