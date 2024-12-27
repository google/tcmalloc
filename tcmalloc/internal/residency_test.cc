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
#include "tcmalloc/internal/page_size.h"
#include "tcmalloc/internal/range_tracker.h"
#include "tcmalloc/internal/util.h"

namespace tcmalloc {
namespace tcmalloc_internal {

class ResidencySpouse {
 public:
  explicit ResidencySpouse() = default;
  explicit ResidencySpouse(const char* const filename) : r_(filename) {}
  explicit ResidencySpouse(absl::string_view filename) : r_(filename.data()) {}

  template <typename... Args>
  decltype(auto) Get(Args&&... args) {
    return r_.Get(std::forward<Args>(args)...);
  }

  decltype(auto) GetHolesAndSwappedBitmaps(const void* const addr) {
    return r_.GetHolesAndSwappedBitmaps(addr);
  }

 private:
  Residency r_;
};

namespace {

using ::testing::FieldsAre;
using ::testing::Optional;

constexpr uint64_t kPageSwapped = (1ULL << 62);
constexpr uint64_t kPagePresent = (1ULL << 63);

TEST(ResidenceTest, ThisProcess) {
  const size_t kPageSize = GetPageSize();
  const int kNumPages = 16;

  // Try both private and shared mappings to make sure we have the bit order of
  // /proc/pid/pageflags correct.
  for (const int flags :
       {MAP_ANONYMOUS | MAP_SHARED, MAP_ANONYMOUS | MAP_PRIVATE}) {
    const size_t kHead = kPageSize * 10;
    const size_t kTail = kPageSize * 10;

    Residency r;
    // Overallocate kNumPages of memory, so we can munmap the page before and
    // after it.
    void* p = mmap(nullptr, kNumPages * kPageSize + kHead + kTail,
                   PROT_READ | PROT_WRITE, flags, -1, 0);
    ASSERT_NE(p, MAP_FAILED) << errno;

    EXPECT_THAT(r.Get(p, (kNumPages + 2) * kPageSize),
                Optional(FieldsAre(0, 0)));
    ASSERT_EQ(munmap(p, kPageSize), 0);
    void* q = reinterpret_cast<char*>(p) + kHead;
    void* last = reinterpret_cast<char*>(p) + kNumPages * kPageSize + kHead;
    ASSERT_EQ(munmap(last, kPageSize), 0);

    EXPECT_THAT(r.Get(p, kHead), Optional(FieldsAre(0, 0)));
    EXPECT_THAT(r.Get(last, kTail), Optional(FieldsAre(0, 0)));

    memset(q, 0, kNumPages * kPageSize);
    (void)mlock(q, kNumPages * kPageSize);
    ::benchmark::DoNotOptimize(q);

    EXPECT_THAT(r.Get(p, kHead), Optional(FieldsAre(0, 0)));
    EXPECT_THAT(r.Get(last, kTail), Optional(FieldsAre(0, 0)));

    EXPECT_THAT(r.Get(q, kPageSize), Optional(FieldsAre(kPageSize, 0)));

    EXPECT_THAT(r.Get(q, (kNumPages + 2) * kPageSize),
                Optional(FieldsAre(kPageSize * kNumPages, 0)));

    EXPECT_THAT(r.Get(reinterpret_cast<char*>(q) + 7, kPageSize - 7),
                Optional(FieldsAre(kPageSize - 7, 0)));

    EXPECT_THAT(r.Get(reinterpret_cast<char*>(q) + 7, kPageSize),
                Optional(FieldsAre(kPageSize, 0)));

    EXPECT_THAT(r.Get(reinterpret_cast<char*>(q) + 7, 3 * kPageSize),
                Optional(FieldsAre(kPageSize * 3, 0)));

    EXPECT_THAT(r.Get(reinterpret_cast<char*>(q) + 7, kNumPages * kPageSize),
                Optional(FieldsAre(kPageSize * kNumPages - 7, 0)));

    EXPECT_THAT(
        r.Get(reinterpret_cast<char*>(q) + 7, kNumPages * kPageSize - 7),
        Optional(FieldsAre(kPageSize * kNumPages - 7, 0)));

    EXPECT_THAT(
        r.Get(reinterpret_cast<char*>(q) + 7, (kNumPages + 1) * kPageSize),
        Optional(FieldsAre(kPageSize * kNumPages - 7, 0)));

    EXPECT_THAT(
        r.Get(reinterpret_cast<char*>(q) + 7, (kNumPages + 1) * kPageSize - 7),
        Optional(FieldsAre(kPageSize * kNumPages - 7, 0)));

    ASSERT_EQ(munmap(q, kNumPages * kPageSize), 0);
  }
}

TEST(ResidenceTest, CannotOpen) {
  ResidencySpouse r("/tmp/a667ba48-18ba-4523-a8a7-b49ece3a6c2b");
  EXPECT_FALSE(r.Get(nullptr, 1).has_value());
}

TEST(ResidenceTest, CannotRead) {
  ResidencySpouse r("/dev/null");
  EXPECT_FALSE(r.Get(nullptr, 1).has_value());
}

TEST(ResidenceTest, CannotSeek) {
  ResidencySpouse r("/dev/null");
  EXPECT_FALSE(r.Get(&r, 1).has_value());
}

// Method that can write a region with a single hugepage
// a region with a single missing page, a region with every other page missing,
// a region with all missing pages, or a region with a hugepage in the middle.
void GenerateHolesInSinglePage(absl::string_view filename, int case_num,
                               int num_pages) {
  int write_fd = signal_safe_open(filename.data(), O_CREAT | O_WRONLY, S_IRUSR);
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
    }
  }
  int size_of_write = num_pages * sizeof(uint64_t);
  CHECK_EQ(write(write_fd, buf.data(), size_of_write), size_of_write);
  CHECK_EQ(close(write_fd), 0) << errno;
}

Residency::SinglePageBitmaps GenerateExpectedSinglePageBitmaps(int case_num) {
  Bitmap<512> expected_holes;
  Bitmap<512> expected_swapped;
  switch (case_num) {
    case 0:
      // All Pages are present. Both bitmaps are 0
      break;
    case 1:
      // All Pages are swapped. Both bitmaps are all 1
      expected_holes.SetRange(0, 512);
      expected_swapped.SetRange(0, 512);
      break;
    case 2:
      // All pages are holes. Holes bitmap is all 1
      expected_holes.SetRange(0, 512);
      break;
    case 3:
      // Every other page is a hole, rest are present.
      // Both bitmaps are 0 and 1 alternating
      for (int idx = 0; idx < 512; idx += 2) {
        if (idx % 2 == 0) {
          expected_holes.SetBit(idx);
        }
      }
      break;
    case 4:
      // Every other page is swapped, rest are present,
      // Bitmaps are 0 and 1 alternating
      for (int idx = 0; idx < 512; idx += 2) {
        if (idx % 2 == 0) {
          expected_holes.SetBit(idx);
          expected_swapped.SetBit(idx);
        }
      }
      break;
    case 5:
      // Every other page is swapped, rest are holes,
      // swapped bitmaps are 0 and 1 alternating, holes bitmaps are all 1
      for (int idx = 0; idx < 512; idx++) {
        expected_holes.SetRange(0, 512);
        if (idx % 2 == 0) {
          expected_swapped.SetBit(idx);
        }
      }
      break;
  }
  return Residency::SinglePageBitmaps{expected_holes, expected_swapped,
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

TEST(PageMapTest, GetHolesAndSwappedBitmaps) {
  constexpr int kNumCases = 6;
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
        s.GetHolesAndSwappedBitmaps(reinterpret_cast<void*>(0));
    g.reset();
    EXPECT_THAT(res.status, expected[i].status);
    EXPECT_TRUE(BitmapsAreEqual(res.holes, expected[i].holes));
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
      s.GetHolesAndSwappedBitmaps(reinterpret_cast<void*>(2 << 21));
  g.reset();
  EXPECT_THAT(res.status, expected.status);
  EXPECT_TRUE(BitmapsAreEqual(res.holes, expected.holes));
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
      s.GetHolesAndSwappedBitmaps(reinterpret_cast<void*>(0x00001));
  g.reset();
  EXPECT_EQ(non_align_addr_res.status, absl::StatusCode::kFailedPrecondition);
}

TEST(PageMapTest, VerifyAddressAlignmentBeyondFirstPageFails) {
  std::optional<AllocationGuard> g;
  g.emplace();
  ResidencySpouse s;
  Residency::SinglePageBitmaps res =
      s.GetHolesAndSwappedBitmaps(reinterpret_cast<void*>((2 << 21) + 1));
  g.reset();
  EXPECT_EQ(res.status, absl::StatusCode::kFailedPrecondition);
}

TEST(PageMapIntegrationTest, WorksOnActualData) {
  std::optional<AllocationGuard> g;
  void* addr = mmap(nullptr, 4 << 20, PROT_WRITE,
                    MAP_ANONYMOUS | MAP_POPULATE | MAP_PRIVATE, -1, 0);
  ASSERT_NE(addr, MAP_FAILED) << errno;
  auto position = reinterpret_cast<uintptr_t>(addr);
  if ((position & (kHugePageSize - 1)) != 0) {
    position |= kHugePageSize - 1;
    position++;
    addr = reinterpret_cast<void*>(position);
  }
  g.emplace();
  Residency r;
  auto res = r.GetHolesAndSwappedBitmaps(addr);
  g.reset();
  ASSERT_EQ(res.status, absl::StatusCode::kOk);
  EXPECT_TRUE(res.holes.IsZero());
  EXPECT_TRUE(res.swapped.IsZero());
  ASSERT_EQ(munmap(reinterpret_cast<uint8_t*>(addr) + 1 * 4096, 4096), 0)
      << errno;
  ASSERT_EQ(munmap(reinterpret_cast<uint8_t*>(addr) + 17 * 4096, 4096), 0)
      << errno;

  g.emplace();
  res = r.GetHolesAndSwappedBitmaps(addr);
  g.reset();
  ASSERT_EQ(res.status, absl::StatusCode::kOk);
  EXPECT_FALSE(res.holes.IsZero());
  ASSERT_TRUE(res.holes.GetBit(1));
  ASSERT_TRUE(res.holes.GetBit(17));
  res.holes.ClearLowestBit();
  res.holes.ClearLowestBit();
  EXPECT_TRUE(res.holes.IsZero());
  EXPECT_TRUE(res.swapped.IsZero());
}

}  // namespace
}  // namespace tcmalloc_internal
}  // namespace tcmalloc
