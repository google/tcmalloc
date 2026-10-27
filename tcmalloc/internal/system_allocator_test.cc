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

#include <errno.h>
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
using ::testing::Not;

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

    for (MemoryTag tag :
         {MemoryTag::kNormal, MemoryTag::kSampledOrCold, MemoryTag::kGuarded}) {
      SCOPED_TRACE(static_cast<unsigned int>(tag));

      void* p = allocator_.MmapAligned(size, alignment, tag);
      EXPECT_NE(p, nullptr);
      EXPECT_EQ(reinterpret_cast<uintptr_t>(p) % alignment, 0);
      EXPECT_EQ(IsNormalMemory(p), tag == MemoryTag::kNormal);
      EXPECT_EQ(IsSampledOrColdMemory(p), tag == MemoryTag::kSampledOrCold);
      EXPECT_EQ(IsGuardedMemory(p), tag == MemoryTag::kGuarded);
      EXPECT_EQ(GetMemoryTag(p), tag);
      EXPECT_EQ(GetMemoryTag(static_cast<char*>(p) + size - 1), tag);
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

TEST(SystemAllocatorTest, SetAnonVmaNameHandlesEmptyAndSubstringViews) {
  constexpr size_t kMinMmapAlloc = 1 << 30;
  NumaTopology<2> topology;
  topology.Init();
  SystemAllocator<NumaTopology<2>, 1> allocator(topology, kMinMmapAlloc);

  const size_t kPageSize = GetPageSize();
  AddressRange res =
      allocator.Allocate(kPageSize, kPageSize, MemoryTag::kNormal);
  ASSERT_NE(res.ptr, nullptr);

  constexpr absl::string_view kFullName = "custom_region_extra_trailing_bytes";
  allocator.SetAnonVmaName(res.ptr, res.bytes, kFullName.substr(0, 13));
  if (tcmalloc::NamedVMAsSupported()) {
    EXPECT_THAT(MappingName(res.ptr, res.bytes), HasSubstr("custom_region"));
    EXPECT_THAT(MappingName(res.ptr, res.bytes),
                Not(HasSubstr("extra_trailing_bytes")));
  }

  allocator.SetAnonVmaName(res.ptr, res.bytes, absl::string_view());
  if (tcmalloc::NamedVMAsSupported()) {
    EXPECT_THAT(MappingName(res.ptr, res.bytes),
                HasSubstr("tcmalloc_region_NORMAL"));
  }

  allocator.SetAnonVmaName(res.ptr, res.bytes, kFullName.substr(0, 13));
  if (tcmalloc::NamedVMAsSupported()) {
    EXPECT_THAT(MappingName(res.ptr, res.bytes), HasSubstr("custom_region"));
  }

  allocator.SetAnonVmaName(res.ptr, res.bytes, absl::string_view(""));
  if (tcmalloc::NamedVMAsSupported()) {
    EXPECT_THAT(MappingName(res.ptr, res.bytes),
                HasSubstr("tcmalloc_region_NORMAL"));
  }
}

TEST(SystemAllocatorTest, MmapAlignedRejectsOutOfPartitionSizeAndAlignment) {
  constexpr size_t kMinMmapAlloc = 1 << 30;
  NumaTopology<2> topology;
  topology.Init();
  SystemAllocator<NumaTopology<2>, 1> allocator(topology, kMinMmapAlloc);

  const size_t kPageSize = GetPageSize();
  constexpr size_t kPartitionSize = uintptr_t{1} << kTagShift;
  EXPECT_EQ(allocator.MmapAligned(kPageSize, 2 * kPartitionSize,
                                  MemoryTag::kSampledOrCold),
            nullptr);
  EXPECT_EQ(allocator.MmapAligned(kPartitionSize + kPageSize, kPageSize,
                                  MemoryTag::kNormal),
            nullptr);
}

class SharedMemoryRegion final : public AddressRegion {
 public:
  SharedMemoryRegion(uintptr_t start, size_t size)
      : start_(start), free_size_(size) {}

  std::pair<void*, size_t> Alloc(size_t size, size_t alignment) override {
    size = system_allocator_internal::RoundUp(size, kHugePageSize);
    alignment = std::max(alignment, kHugePageSize);
    uintptr_t end = start_ + free_size_;
    if (size > free_size_) return {nullptr, 0};
    uintptr_t result = (end - size) & ~(alignment - 1);
    if (result < start_) return {nullptr, 0};
    size_t actual_size = end - result;
    void* ptr = mmap(reinterpret_cast<void*>(result), actual_size,
                     PROT_READ | PROT_WRITE,
                     MAP_FIXED | MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (ptr == MAP_FAILED) return {nullptr, 0};
    free_size_ -= actual_size;
    return {ptr, actual_size};
  }

 private:
  uintptr_t start_;
  size_t free_size_;
};

class SharedMemoryRegionFactory final : public AddressRegionFactory {
 public:
  AddressRegion* Create(void* start, size_t size, UsageHint hint) override {
    void* region_space = MallocInternal(sizeof(SharedMemoryRegion));
    if (region_space == nullptr) return nullptr;
    return new (region_space)
        SharedMemoryRegion(reinterpret_cast<uintptr_t>(start), size);
  }
};

TEST(SystemAllocatorTest, ReleaseCustomRegionAfterRestoringDefaultFactory) {
  NumaTopology<2> topology;
  topology.Init();
  SystemAllocator<NumaTopology<2>, 1> allocator(topology, kMinMmapAlloc);

  AddressRegionFactory* default_factory = allocator.GetRegionFactory();
  ASSERT_NE(default_factory, nullptr);

  SharedMemoryRegionFactory shared_factory;
  allocator.SetRegionFactory(&shared_factory);

  const size_t kPageSize = GetPageSize();
  AddressRange res =
      allocator.Allocate(kPageSize, kPageSize, MemoryTag::kNormal);
  ASSERT_NE(res.ptr, nullptr);
  memset(res.ptr, 0xAB, res.bytes);
  ASSERT_EQ(static_cast<unsigned char*>(res.ptr)[0], 0xAB);

  // Swap back to the default anonymous factory before releasing the shared
  // region allocated from shared_factory. MADV_DONTNEED only unmaps PTEs on
  // MAP_SHARED | MAP_ANONYMOUS without punching a hole in the underlying
  // shmem page cache, whereas MADV_REMOVE punches a hole and zeroes the page.
  allocator.SetRegionFactory(default_factory);

  MemoryModifyStatus status = allocator.Release(res.ptr, res.bytes);
  EXPECT_TRUE(status.success);
  EXPECT_EQ(allocator.release_errors(), 0);
  EXPECT_EQ(static_cast<unsigned char*>(res.ptr)[0], 0);
  EXPECT_EQ(munmap(res.ptr, res.bytes), 0);
}

void BM_ReleasePages(benchmark::State& state) {
  static NumaTopology<2>* topology = [] {
    auto* t = new NumaTopology<2>();
    t->Init();
    return t;
  }();
  static SystemAllocator<NumaTopology<2>, 1>* allocator =
      new SystemAllocator<NumaTopology<2>, 1>(*topology, kMinMmapAlloc);
  const size_t kPageSize = GetPageSize();
  const size_t bytes = state.range(0) * kPageSize;
  AddressRange res = allocator->Allocate(bytes, kPageSize, MemoryTag::kNormal);
  TC_CHECK_NE(res.ptr, nullptr);
  for (auto _ : state) {
    MemoryModifyStatus st = allocator->Release(res.ptr, bytes);
    benchmark::DoNotOptimize(st);
  }
}
BENCHMARK(BM_ReleasePages)
    ->Arg(1)
    ->Arg(8)
    ->Arg(64)
    ->ThreadRange(1, 8)
    ->UseRealTime();

}  // namespace
}  // namespace tcmalloc_internal
}  // namespace tcmalloc
