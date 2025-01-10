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

#include "tcmalloc/system-alloc.h"

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
#include "tcmalloc/common.h"
#include "tcmalloc/internal/config.h"
#include "tcmalloc/internal/logging.h"
#include "tcmalloc/internal/numa.h"
#include "tcmalloc/internal/proc_maps.h"
#include "tcmalloc/malloc_extension.h"

#ifndef PR_SET_VMA
#define PR_SET_VMA 0x53564d41
#endif

#ifndef PR_SET_VMA_ANON_NAME
#define PR_SET_VMA_ANON_NAME 0
#endif

namespace tcmalloc {
namespace tcmalloc_internal {
namespace {

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

  NumaTopology<kNumaPartitions, kNumBaseClasses> topology_;
  SystemAllocator<NumaTopology<kNumaPartitions, kNumBaseClasses>> allocator_{
      topology_};
};
INSTANTIATE_TEST_SUITE_P(VariedAlignment, MmapAlignedTest,
                         testing::Values(kPageSize, kHugePageSize,
                                         kMinMmapAlloc,
                                         uintptr_t{1} << kTagShift));

TEST_P(MmapAlignedTest, CorrectAlignmentAndTag) {
  MmapAndCheck(kHugePageSize, GetParam());
}

// Ensure mmap sizes near kTagMask still have the correct tag at the beginning
// and end of the mapping.
TEST_F(MmapAlignedTest, LargeSizeSmallAlignment) {
  MmapAndCheck(uintptr_t{1} << kTagShift, kPageSize);
}

// Was SimpleRegion::Alloc invoked at least once?
static bool simple_region_alloc_invoked = false;

class SimpleRegion : public AddressRegion {
 public:
  SimpleRegion(uintptr_t start, size_t size)
      : start_(start), free_size_(size) {}

  std::pair<void*, size_t> Alloc(size_t size, size_t alignment) override {
    simple_region_alloc_invoked = true;
    uintptr_t result = (start_ + free_size_ - size) & ~(alignment - 1);
    if (result < start_ || result >= start_ + free_size_) return {nullptr, 0};
    size_t actual_size = start_ + free_size_ - result;
    free_size_ -= actual_size;
    void* ptr = reinterpret_cast<void*>(result);
    int err = mprotect(ptr, actual_size, PROT_READ | PROT_WRITE);
    TC_CHECK_EQ(err, 0);
    return {ptr, actual_size};
  }

 private:
  uintptr_t start_;
  size_t free_size_;
};

class SimpleRegionFactory : public AddressRegionFactory {
 public:
  constexpr SimpleRegionFactory() = default;

  AddressRegion* Create(void* start, size_t size, UsageHint hint) override {
    void* region_space = MallocInternal(sizeof(SimpleRegion));
    TC_CHECK_NE(region_space, nullptr);
    usage_hint_ = hint;
    return new (region_space)
        SimpleRegion(reinterpret_cast<uintptr_t>(start), size);
  }

  // Used to verify that the correct usage hint was passed to Create().
  std::optional<UsageHint> usage_hint_;
};

const char* hintToString(AddressRegionFactory::UsageHint usage_hint) {
  using UsageHint = AddressRegionFactory::UsageHint;
  switch (usage_hint) {
    case UsageHint::kNormal:
      return "kNormal";
    case UsageHint::kInfrequentAllocation:
      return "kInfrequentAllocation";
    case UsageHint::kMetadata:
      return "kMetadata";
    case UsageHint::kInfrequentAccess:
      return "kInfrequentAccess";
    case UsageHint::kNormalNumaAwareS0:
      return "kNormalNumaAwareS0";
    case UsageHint::kNormalNumaAwareS1:
      return "kNormalNumaAware";
    default:
      return "unknown";
  }
}

TCMALLOC_ATTRIBUTE_NO_DESTROY ABSL_CONST_INIT SimpleRegionFactory f;

TEST(Basic, InvokedTest) {
  MallocExtension::SetRegionFactory(&f);

  // An allocation size that is likely to trigger the system allocator.
  void* ptr = ::operator new(kMinMmapAlloc);
  // TODO(b/183453911): Remove workaround for GCC 10.x deleting operator new,
  // https://gcc.gnu.org/bugzilla/show_bug.cgi?id=94295.
  benchmark::DoNotOptimize(ptr);
  ::operator delete(ptr);

  // Make sure that our allocator was invoked.
  ASSERT_TRUE(simple_region_alloc_invoked);
}

TEST(Basic, RetryFailTest) {
  // Check with the allocator still works after a failed allocation.
  //
  // There is no way to call malloc and guarantee it will fail.  malloc takes a
  // size_t parameter and the C++ standard does not constrain the size of
  // size_t.  For example, consider an implementation where size_t is 32 bits
  // and pointers are 64 bits.
  //
  // It is likely, though, that sizeof(size_t) == sizeof(void*).  In that case,
  // the first allocation here might succeed but the second allocation must
  // fail.
  //
  // If the second allocation succeeds, you will have to rewrite or
  // disable this test.
  const size_t kHugeSize = std::numeric_limits<size_t>::max() / 2;
  void* p1 = malloc(kHugeSize);
  void* p2 = malloc(kHugeSize);
  ASSERT_EQ(p2, nullptr);
  if (p1 != nullptr) free(p1);

  void* q = malloc(1024);
  ASSERT_NE(q, nullptr);
  free(q);
}

// Verify default behavior for tcmalloc metadata utilizes usage hint kMetadata.
TEST(UsageHint, VerifyUsageHintkMetadataTest) {
  MallocExtension::SetRegionFactory(&f);
  // Need a large enough size to trigger the system allocator,
  //  2.0 is an arbitrary number. Else it would continue to use the previous
  //  hugepage region and a new usage hint wouldn't be assigned
  void* ptr = ::operator new(kMinMmapAlloc * 2.0);

  ASSERT_TRUE(f.usage_hint_.has_value());
  EXPECT_EQ(*f.usage_hint_, AddressRegionFactory::UsageHint::kMetadata)
      << "Usage hint is " << hintToString(*f.usage_hint_);
  ::operator delete(ptr);
}

}  // namespace
}  // namespace tcmalloc_internal
}  // namespace tcmalloc
