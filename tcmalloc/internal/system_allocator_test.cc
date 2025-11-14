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

#include <limits>
#include <optional>
#include <string>
#include <utility>

#include "benchmark/benchmark.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "absl/base/attributes.h"
#include "absl/strings/str_format.h"
#include "absl/strings/string_view.h"
#include "tcmalloc/internal/config.h"
#include "tcmalloc/internal/logging.h"
#include "tcmalloc/internal/numa.h"
#include "tcmalloc/internal/proc_maps.h"

#ifndef PR_SET_VMA
#define PR_SET_VMA 0x53564d41
#endif

#ifndef PR_SET_VMA_ANON_NAME
#define PR_SET_VMA_ANON_NAME 0
#endif

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
  ProcMapsIterator it(0, &iterbuf);  // 0 means "current pid"
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
    SCOPED_TRACE(absl::StrFormat("size = %u, alignment = %u", size, alignment));

    for (MemoryTag tag :
         {MemoryTag::kNormal, MemoryTag::kSampled, MemoryTag::kCold}) {
      SCOPED_TRACE(static_cast<unsigned int>(tag));

      void* p = allocator_.MmapAligned(size, alignment, tag);
      EXPECT_NE(p, nullptr);
      EXPECT_EQ(reinterpret_cast<uintptr_t>(p) % alignment, 0);
      EXPECT_EQ(IsNormalMemory(p), tag == MemoryTag::kNormal);
      EXPECT_EQ(GetMemoryTag(p), tag);
      EXPECT_EQ(GetMemoryTag(static_cast<char*>(p) + size - 1), tag);
      if (PrSetVmaIsSupported()) {
        EXPECT_THAT(MappingName(p, size),
                    HasSubstr(absl::StrFormat("tcmalloc_region_%s",
                                              MemoryTagToLabel(tag))));
      }
      EXPECT_EQ(munmap(p, size), 0);
    }
  }

  static bool PrSetVmaIsSupported() {
    static bool pr_set_vma_works = [] {
      constexpr size_t kMmapSize = 4096;
      void* addr = mmap(NULL, kMmapSize, PROT_NONE,
                        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
      if (addr == MAP_FAILED) {
        return false;
      }
      int err =
          prctl(PR_SET_VMA, PR_SET_VMA_ANON_NAME, addr, kMmapSize, "test");
      munmap(addr, kMmapSize);
      return err == 0;
    }();
    return pr_set_vma_works;
  }

  static constexpr size_t kNumaPartitions = 2;
  static constexpr size_t kNumBaseClasses = 50;
  NumaTopology<kNumaPartitions, kNumBaseClasses> topology_;
  SystemAllocator<NumaTopology<kNumaPartitions, kNumBaseClasses>, 1> allocator_{
      topology_, kMinMmapAlloc};
};

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

}  // namespace
}  // namespace tcmalloc_internal
}  // namespace tcmalloc
