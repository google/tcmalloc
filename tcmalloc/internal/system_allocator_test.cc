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

#include "tcmalloc/internal/system_allocator.h"

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/prctl.h>

#include <atomic>
#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "benchmark/benchmark.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "absl/base/attributes.h"
#include "absl/strings/str_format.h"
#include "absl/strings/string_view.h"
#include "tcmalloc/internal/allocation_guard.h"
#include "tcmalloc/internal/config.h"
#include "tcmalloc/internal/exponential_biased.h"
#include "tcmalloc/internal/logging.h"
#include "tcmalloc/internal/numa.h"
#include "tcmalloc/internal/page_size.h"
#include "tcmalloc/internal/proc_maps.h"
#include "tcmalloc/testing/testutil.h"

namespace tcmalloc {
namespace tcmalloc_internal {
namespace {

constexpr size_t kMinMmapAlloc = 1 << 30;

using ::testing::HasSubstr;

// Returns the filename associated with the runtime mapping that includes the
// [start, start+size) address range, or the empty string if not found.
std::string MappingName(void* mmap_start, size_t mmap_size) {
  uintptr_t mmap_start_addr = reinterpret_cast<uintptr_t>(mmap_start);
  uintptr_t mmap_end_addr = mmap_start_addr + mmap_size;

  uint64_t start, end, offset;
  int64_t inode;
  char *flags, *filename;

  ProcMapsIterator::Buffer iterbuf;
  ProcMapsIterator it(&iterbuf);
  while (
      it.NextExt(&start, &end, &flags, &offset, &inode, &filename, nullptr)) {
    if (start <= mmap_start_addr && mmap_end_addr <= end) {
      return std::string(filename);
    }
  }
  return "";
}

class MmapAlignedTest : public testing::TestWithParam<size_t> {
 protected:
  void MmapAndCheck(size_t size, size_t alignment) {
    topology_.Init();
    SCOPED_TRACE(absl::StrFormat("size = %u, alignment = %u", size, alignment));

    for (MemoryTag tag : {MemoryTag::kNormal, MemoryTag::kSampledOrCold}) {
      SCOPED_TRACE(static_cast<unsigned int>(tag));

      void* p = allocator_.MmapAligned(size, alignment, tag);
      EXPECT_NE(p, nullptr);
      EXPECT_EQ(reinterpret_cast<uintptr_t>(p) % alignment, 0);
      EXPECT_EQ(IsNormalMemory(p), tag == MemoryTag::kNormal);
      EXPECT_EQ(GetMemoryTag(p), tag);
      EXPECT_EQ(GetMemoryTag(static_cast<char*>(p) + size - 1), tag);
      // Page heap mappings must avoid the guarded sub-region.
      if (IsSampledOrColdTag(tag) && size <= kPageHeapSubRegionBit &&
          alignment <= kPageHeapSubRegionBit) {
        EXPECT_TRUE(InPageHeapSubRegion(p));
        EXPECT_TRUE(InPageHeapSubRegion(static_cast<char*>(p) + size - 1));
      }
      if (IsSampledOrColdTag(tag) && size <= kPageHeapSubRegionBit &&
          alignment <= kPageHeapSubRegionBit) {
        void* g =
            allocator_.MmapAligned(size, alignment, tag, SubRegion::kGuarded);
        ASSERT_NE(g, nullptr);
        EXPECT_EQ(reinterpret_cast<uintptr_t>(g) % alignment, 0);
        EXPECT_EQ(GetMemoryTag(g), tag);
        EXPECT_TRUE(InGuardedSubRegion(g));
        EXPECT_TRUE(InGuardedSubRegion(static_cast<char*>(g) + size - 1));
        EXPECT_EQ(munmap(g, size), 0);
      }
      if (tcmalloc::NamedVMAsSupported()) {
        EXPECT_THAT(MappingName(p, size),
                    HasSubstr(absl::StrFormat("tcmalloc_region_%s",
                                              MemoryTagToLabel(tag))));
      }
      EXPECT_EQ(munmap(p, size), 0);
    }
  }


  static constexpr size_t kNumaPartitions = 2;
  static constexpr size_t kNumBaseClasses = 50;
  NumaTopology<kNumaPartitions, kNumBaseClasses> topology_;
  SystemAllocator<NumaTopology<kNumaPartitions, kNumBaseClasses>, 1> allocator_{
      topology_, kMinMmapAlloc};
};

TEST(SubRegionTest, GuardedPoolIsDisjointFromThePageHeap) {
  constexpr size_t kSize = 1 << 21;
  NumaTopology<2> topology;
  SystemAllocator<NumaTopology<2>, 1> allocator(topology, kSize);

  constexpr MemoryTag kTag = MemoryTag::kSampledOrCold;
  void* heap = allocator.MmapAligned(kSize, kSize, kTag);
  void* guarded =
      allocator.MmapAligned(kSize, kSize, kTag, SubRegion::kGuarded);
  ASSERT_NE(heap, nullptr);
  ASSERT_NE(guarded, nullptr);

  // Same partition ...
  EXPECT_EQ(GetMemoryTag(heap), kTag);
  EXPECT_EQ(GetMemoryTag(guarded), kTag);
  // ... opposite halves of it.
  EXPECT_TRUE(InPageHeapSubRegion(heap));
  EXPECT_FALSE(InGuardedSubRegion(heap));
  EXPECT_TRUE(InGuardedSubRegion(guarded));
  EXPECT_FALSE(InPageHeapSubRegion(guarded));

  EXPECT_EQ(munmap(heap, kSize), 0);
  EXPECT_EQ(munmap(guarded, kSize), 0);
}

// Interleaving both sub-regions exercises the separate cursors: neither may
// drag the other across the boundary.
TEST(SubRegionTest, InterleavedMappingsStayInTheirSubRegion) {
  constexpr size_t kSize = 1 << 21;
  constexpr int kRounds = 32;
  constexpr MemoryTag kTag = MemoryTag::kSampledOrCold;
  NumaTopology<2> topology;
  SystemAllocator<NumaTopology<2>, 1> allocator(topology, kSize);

  std::vector<std::pair<void*, SubRegion>> maps;
  for (int i = 0; i < kRounds; ++i) {
    for (SubRegion sr : {SubRegion::kPageHeap, SubRegion::kGuarded}) {
      void* p = allocator.MmapAligned(kSize, kSize, kTag, sr);
      ASSERT_NE(p, nullptr);
      maps.emplace_back(p, sr);
    }
  }
  for (auto [p, sr] : maps) {
    void* last = static_cast<char*>(p) + kSize - 1;
    EXPECT_EQ(GetMemoryTag(p), kTag);
    EXPECT_EQ(GetMemoryTag(last), kTag);
    const bool heap = sr == SubRegion::kPageHeap;
    EXPECT_EQ(InPageHeapSubRegion(p), heap);
    EXPECT_EQ(InPageHeapSubRegion(last), heap);
    EXPECT_EQ(munmap(p, kSize), 0);
  }
}

// A cursor that starts in the guarded half but whose next mapping would end in
// the page heap half must be rejected.  The first 1.5 TiB mapping lands at the
// start of the guarded half and leaves the cursor at 1.5 TiB; once it is
// unmapped, the cursor alone would place the second mapping across the
// boundary.
TEST(SubRegionTest, GuardedCursorDoesNotCrossIntoPageHeap) {
  if (kSanitizerPresent) {
    GTEST_SKIP() << "Skipping under constrained address space";
  }
  constexpr size_t kSize = kPageHeapSubRegionBit / 4 * 3;
  constexpr size_t kAlign = 1 << 12;
  NumaTopology<2> topology;
  SystemAllocator<NumaTopology<2>, 1> allocator(topology, kAlign);
  constexpr MemoryTag kTag = MemoryTag::kSampledOrCold;

  void* a = allocator.MmapAligned(kSize, kAlign, kTag, SubRegion::kGuarded);
  ASSERT_NE(a, nullptr);
  ASSERT_TRUE(InGuardedSubRegion(static_cast<char*>(a) + kSize - 1));
  ASSERT_EQ(munmap(a, kSize), 0);

  void* b = allocator.MmapAligned(kSize, kAlign, kTag, SubRegion::kGuarded);
  ASSERT_NE(b, nullptr);
  EXPECT_EQ(GetMemoryTag(b), kTag);
  EXPECT_TRUE(InGuardedSubRegion(b));
  EXPECT_TRUE(InGuardedSubRegion(static_cast<char*>(b) + kSize - 1));
  EXPECT_EQ(munmap(b, kSize), 0);
}

constexpr size_t kSmallButSlowTCMallocPageSize = 1 << 12;
constexpr size_t kDefaultTCMallocPageSize = 1 << 13;

INSTANTIATE_TEST_SUITE_P(VariedAlignment, MmapAlignedTest,
                         testing::Values(kSmallButSlowTCMallocPageSize,
                                         kDefaultTCMallocPageSize,
                                         kHugePageSize, kMinMmapAlloc,
                                         uintptr_t{1} << kTagShift));

TEST_P(MmapAlignedTest, CorrectAlignmentAndTag) {
  MmapAndCheck(kHugePageSize, GetParam());
}

// Ensure mmap sizes near kTagMask still have the correct tag at the beginning
// and end of the mapping.
TEST_F(MmapAlignedTest, LargeSizeSmallAlignment) {
  MmapAndCheck(uintptr_t{1} << kTagShift, 1 << 12);
}

TEST(SystemAllocatorTest, ReleaseLockedMemory) {
  constexpr size_t kMinMmapAlloc = 1 << 30;
  NumaTopology<2> topology;
  topology.Init();
  SystemAllocator<NumaTopology<2>, 1> allocator(topology, kMinMmapAlloc);

  const size_t kPageSize = GetPageSize();
  AddressRange res =
      allocator.Allocate(kPageSize, kPageSize, MemoryTag::kNormal);
  ASSERT_NE(res.ptr, nullptr);

  if (mlock(res.ptr, res.bytes) == 0) {
    memset(res.ptr, 0xAB, res.bytes);
  }
  MemoryModifyStatus status = allocator.Release(res.ptr, res.bytes);
  EXPECT_TRUE(status.success);
  EXPECT_EQ(allocator.release_errors(), 0);
}

}  // namespace
}  // namespace tcmalloc_internal
}  // namespace tcmalloc
