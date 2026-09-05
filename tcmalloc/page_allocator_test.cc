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
//
// Tests for infrastructure common to page allocator implementations
// (stats and logging.)
#include "tcmalloc/page_allocator.h"

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "absl/base/internal/spinlock.h"
#include "absl/base/nullability.h"
#include "tcmalloc/common.h"
#include "tcmalloc/internal/config.h"
#include "tcmalloc/internal/logging.h"
#include "tcmalloc/internal/memory_tag.h"
#include "tcmalloc/internal/page_allocator_hooks.h"
#include "tcmalloc/internal/pageflags.h"
#include "tcmalloc/malloc_extension.h"
#include "tcmalloc/page_allocator_interface.h"
#include "tcmalloc/page_allocator_test_util.h"
#include "tcmalloc/pagemap.h"
#include "tcmalloc/pages.h"
#include "tcmalloc/span.h"
#include "tcmalloc/static_vars.h"
#include "tcmalloc/stats.h"
#include "tcmalloc/testing/testutil.h"

namespace tcmalloc {
namespace tcmalloc_internal {
namespace {

class PageAllocatorTest : public testing::Test {
 protected:
  // Not in constructor so subclasses can mess about with environment
  // variables.
  void SetUp() override {
    // If this test is not linked against TCMalloc, the global arena used for
    // metadata will not be initialized.
    tc_globals.InitIfNecessary();

    before_ = MallocExtension::GetRegionFactory();
    if (before_ != nullptr) {
      extra_.emplace(before_);
      MallocExtension::SetRegionFactory(&*extra_);
    }
  }
  void TearDown() override {
    if (before_ != nullptr) {
      MallocExtension::SetRegionFactory(before_);
    }
    extra_.reset();
  }

  Span* New(Length n, SpanAllocInfo span_alloc_info,
            MemoryTag tag = MemoryTag::kNormal) {
    return allocator_.New(n, span_alloc_info, tag);
  }
  Span* NewAligned(Length n, Length align, SpanAllocInfo span_alloc_info,
                   MemoryTag tag = MemoryTag::kNormal) {
    return allocator_.NewAligned(n, align, span_alloc_info, tag);
  }
  void Delete(Span* s, SpanAllocInfo span_alloc_info,
              MemoryTag tag = MemoryTag::kNormal) {
#ifdef TCMALLOC_INTERNAL_LEGACY_LOCKING
    PageHeapSpinLockHolder l;
    allocator_.Delete(s, tag, span_alloc_info);
#else
    PageAllocatorInterface::AllocationState a{
        Range(s->first_page(), s->num_pages()),
        s->donated(),
    };
    Span::Delete(s);
    PageHeapSpinLockHolder l;
    allocator_.Delete(a, tag, span_alloc_info);
#endif  // TCMALLOC_INTERNAL_LEGACY_LOCKING
  }

  Length Release(Length n, PageReleaseReason reason) {
    PageHeapSpinLockHolder l;
    return allocator_.ReleaseAtLeastNPages(n, reason);
  }

  std::string Print() {
    return PrintToString(1024 * 1024, [&](Printer& out) {
      PageFlags pageflags;
      allocator_.Print(out, MemoryTag::kNormal, pageflags);
    });
  }

  PageAllocator allocator_;
  std::optional<ExtraRegionFactory> extra_;
  AddressRegionFactory* absl_nullable before_;
};

// We've already tested in stats_test that PageAllocInfo keeps good stats;
// here we're just testing that we make the proper Record calls.
TEST_F(PageAllocatorTest, Record) {
  constexpr SpanAllocInfo kSpanInfo = {/*objects_per_span=*/7,
                                       AccessDensityPrediction::kSparse};
  std::vector<MemoryTag> tags = {MemoryTag::kNormal};
  if (tc_globals.multiple_non_numa_partitions()) {
    tags.push_back(MemoryTag::kNormalP1);
  }

  std::vector<std::pair<Span*, MemoryTag>> spans;
  for (auto tag : tags) {
    for (int i = 0; i < 15; ++i) {
      Delete(New(Length(1), kSpanInfo, tag), kSpanInfo, tag);
    }

    for (int i = 0; i < 20; ++i) {
      spans.push_back(std::make_pair(New(Length(2), kSpanInfo, tag), tag));
    }

    for (int i = 0; i < 25; ++i) {
      Delete(NewAligned(Length(3), Length(2), kSpanInfo, tag), kSpanInfo, tag);
    }
  }
  {
    PageHeapSpinLockHolder l;
    for (auto tag : tags) {
      auto info = allocator_.info(tag);

      ASSERT_EQ(15, info.counts_for(Length(1)).nalloc);
      ASSERT_EQ(15, info.counts_for(Length(1)).nfree);

      ASSERT_EQ(20, info.counts_for(Length(2)).nalloc);
      ASSERT_EQ(0, info.counts_for(Length(2)).nfree);

      ASSERT_EQ(25, info.counts_for(Length(3)).nalloc);
      ASSERT_EQ(25, info.counts_for(Length(3)).nfree);

      for (auto i = Length(4); i <= kMaxPages; ++i) {
        ASSERT_EQ(0, info.counts_for(i).nalloc);
        ASSERT_EQ(0, info.counts_for(i).nfree);
      }

      const Length absurd =
          Length(uintptr_t{1} << (kAddressBits - 1 - kPageShift));
      for (Length i = kMaxPages + Length(1); i < absurd; i *= 2) {
        ASSERT_EQ(0, info.counts_for(i).nalloc);
        ASSERT_EQ(0, info.counts_for(i).nfree);
      }
    }
  }
  for (auto& [s, tag] : spans) Delete(s, kSpanInfo, tag);
}

// And that we call the print method properly.
TEST_F(PageAllocatorTest, PrintIt) {
  constexpr SpanAllocInfo kSpanInfo = {/*objects_per_span=*/17,
                                       AccessDensityPrediction::kDense};
  Delete(New(Length(1), kSpanInfo), kSpanInfo);
  std::string output = Print();
  EXPECT_THAT(output, testing::ContainsRegex("stats on allocation sizes"));
}

TEST_F(PageAllocatorTest, ShrinkFailureTest) {
  // Turn off subrelease so that we take the ShrinkHardBy path.
  const bool old_subrelease = Parameters::hpaa_subrelease();
  Parameters::set_hpaa_subrelease(false);

  constexpr SpanAllocInfo kSpanInfo = {/*objects_per_span=*/1,
                                       AccessDensityPrediction::kSparse};
  Span* normal = New(kPagesPerHugePage / 2, kSpanInfo, MemoryTag::kNormal);
  Span* sampled = New(kPagesPerHugePage / 2, kSpanInfo, MemoryTag::kSampled);

  BackingStats stats;
  {
    PageHeapSpinLockHolder l;
    stats = allocator_.stats();
  }
  EXPECT_EQ(stats.system_bytes, 2 * kHugePageSize);
  EXPECT_EQ(stats.free_bytes, kHugePageSize);
  EXPECT_EQ(stats.unmapped_bytes, 0);

  // Choose a limit so that we hit and we are not able to satisfy it.
  allocator_.set_limit(kPagesPerHugePage.in_bytes(), PageAllocator::kSoft);
  {
    PageHeapSpinLockHolder l;
    allocator_.ShrinkToUsageLimit(Length(0));
  }
  EXPECT_LE(1, allocator_.limit_hits(PageAllocator::kSoft));
  EXPECT_LE(
      0, allocator_.successful_shrinks_after_limit_hit(PageAllocator::kSoft));

  Delete(normal, kSpanInfo, MemoryTag::kNormal);
  Delete(sampled, kSpanInfo, MemoryTag::kSampled);
  Parameters::set_hpaa_subrelease(old_subrelease);
}

TEST_F(PageAllocatorTest, b270916852) {
  // Turn off subrelease so that we take the ShrinkHardBy path.
  const bool old_subrelease = Parameters::hpaa_subrelease();
  Parameters::set_hpaa_subrelease(false);

  constexpr SpanAllocInfo kSpanInfo = {/*objects_per_span=*/1,
                                       AccessDensityPrediction::kSparse};
  Span* normal = New(kPagesPerHugePage / 2, kSpanInfo, MemoryTag::kNormal);
  Span* sampled = New(kPagesPerHugePage / 2, kSpanInfo, MemoryTag::kSampled);

  BackingStats stats;
  {
    PageHeapSpinLockHolder l;
    stats = allocator_.stats();
  }
  EXPECT_EQ(stats.system_bytes, 2 * kHugePageSize);
  EXPECT_EQ(stats.free_bytes, kHugePageSize);
  EXPECT_EQ(stats.unmapped_bytes, 0);

  // Choose a limit so that
  // 1.  We hit it.  It should be less than stats.system_bytes.
  // 2.  It is below current usage.
  // 3.  It is above what can be released from a single page heap.
  const size_t metadata_bytes = []() {
    PageHeapSpinLockHolder l;
    return tc_globals.metadata_bytes();
  }();
  allocator_.set_limit(
      metadata_bytes + (3 * kPagesPerHugePage / 2).in_bytes() + kPageSize,
      PageAllocator::kSoft);
  {
    PageHeapSpinLockHolder l;
    allocator_.ShrinkToUsageLimit(Length(0));
  }
  EXPECT_LE(1, allocator_.limit_hits(PageAllocator::kSoft));
  EXPECT_LE(
      1, allocator_.successful_shrinks_after_limit_hit(PageAllocator::kSoft));

  Delete(normal, kSpanInfo, MemoryTag::kNormal);
  Delete(sampled, kSpanInfo, MemoryTag::kSampled);
  Parameters::set_hpaa_subrelease(old_subrelease);
}

struct HookRecord {
  size_t start_page_index;
  size_t n;
  size_t align;
  size_t objects_per_span;
  uint8_t density;
  MemoryTag tag;
};

constexpr int kMaxRecords = 10;

static HookRecord new_records[kMaxRecords];
static int new_record_count = 0;
static HookRecord delete_records[kMaxRecords];
static int delete_record_count = 0;

static void TestNewHook(size_t start_page_index, size_t n, size_t align,
                        size_t objects_per_span, uint8_t density,
                        MemoryTag tag) {
  if (new_record_count < kMaxRecords) {
    new_records[new_record_count++] = {start_page_index, n,       align,
                                       objects_per_span, density, tag};
  }
}

static void TestDeleteHook(size_t start_page_index, size_t n,
                           size_t objects_per_span, uint8_t density,
                           MemoryTag tag) {
  if (delete_record_count < kMaxRecords) {
    delete_records[delete_record_count++] = {start_page_index, n,       0,
                                             objects_per_span, density, tag};
  }
}

struct ReleaseHookRecord {
  size_t num_pages;
  size_t released;
  PageReleaseReason reason;
};

static ReleaseHookRecord release_records[kMaxRecords];
static int release_record_count = 0;

static void TestReleaseHook(size_t num_pages, size_t released,
                            PageReleaseReason reason) {
  if (release_record_count < kMaxRecords) {
    release_records[release_record_count++] = {num_pages, released, reason};
  }
}

TEST_F(PageAllocatorTest, Hooks) {
  new_record_count = 0;
  delete_record_count = 0;
  release_record_count = 0;

  EXPECT_TRUE(page_allocator_new_hooks.Add(&TestNewHook));
  EXPECT_TRUE(page_allocator_delete_hooks.Add(&TestDeleteHook));
  EXPECT_TRUE(page_allocator_release_hooks.Add(&TestReleaseHook));

  constexpr SpanAllocInfo kSpanInfo = {/*objects_per_span=*/5,
                                       AccessDensityPrediction::kSparse};
  Span* s = New(Length(3), kSpanInfo, MemoryTag::kNormal);
  ASSERT_NE(s, nullptr);
  const size_t expected_page_index = s->first_page().index();
  EXPECT_GE(new_record_count, 1);
  bool found_new = false;
  for (int i = 0; i < new_record_count; ++i) {
    if (new_records[i].start_page_index == expected_page_index &&
        new_records[i].n == 3) {
      found_new = true;
      EXPECT_EQ(new_records[i].align, 1);
      EXPECT_EQ(new_records[i].objects_per_span, 5);
      EXPECT_EQ(new_records[i].density,
                static_cast<uint8_t>(AccessDensityPrediction::kSparse));
      EXPECT_EQ(new_records[i].tag, MemoryTag::kNormal);
    }
  }
  EXPECT_TRUE(found_new);

  Delete(s, kSpanInfo, MemoryTag::kNormal);
  EXPECT_GE(delete_record_count, 1);
  bool found_delete = false;
  for (int i = 0; i < delete_record_count; ++i) {
    if (delete_records[i].start_page_index == expected_page_index &&
        delete_records[i].n == 3) {
      found_delete = true;
      EXPECT_EQ(delete_records[i].objects_per_span, 5);
      EXPECT_EQ(delete_records[i].density,
                static_cast<uint8_t>(AccessDensityPrediction::kSparse));
      EXPECT_EQ(delete_records[i].tag, MemoryTag::kNormal);
    }
  }
  EXPECT_TRUE(found_delete);

  constexpr PageReleaseReason kReasons[] = {
      PageReleaseReason::kReleaseMemoryToSystem,
      PageReleaseReason::kProcessBackgroundActions,
      PageReleaseReason::kSoftLimitExceeded,
      PageReleaseReason::kHardLimitExceeded,
  };
  Length requested = Length(1);
  for (PageReleaseReason reason : kReasons) {
    release_record_count = 0;
    Release(requested, reason);
    EXPECT_GE(release_record_count, 1);
    EXPECT_EQ(release_records[0].num_pages, requested.raw_num());
    EXPECT_EQ(release_records[0].reason, reason);
    ++requested;
  }

  EXPECT_TRUE(page_allocator_new_hooks.Remove(&TestNewHook));
  EXPECT_TRUE(page_allocator_delete_hooks.Remove(&TestDeleteHook));
  EXPECT_TRUE(page_allocator_release_hooks.Remove(&TestReleaseHook));
}

TEST_F(PageAllocatorTest, PageMap3UnallocatedHugePage) {
  // Compute leaf length based on pagemap radix tree configuration.
  constexpr size_t kLeafBits = (kAddressBits - kPageShift + 2) / 3;
  constexpr size_t kLeafLength = size_t{1} << kLeafBits;
  constexpr SpanAllocInfo kSpanInfo = {/*objects_per_span=*/1,
                                       AccessDensityPrediction::kSparse};
  Span* s = New(Length(1), kSpanInfo, MemoryTag::kNormal);
  ASSERT_NE(s, nullptr);
  HugePage hp0 = HugePageContaining(s->first_page());
  const size_t leaf_base_idx = s->first_page().index() & ~(kLeafLength - 1);
  HugePage leaf_base = HugePageContaining(PageId{leaf_base_idx});
  HugePage sibling_hp = (hp0 == leaf_base) ? (hp0 + NHugePages(1)) : leaf_base;

  ASSERT_TRUE(tc_globals.pagemap().HasLeaf(sibling_hp.first_page()));

  PageBitmap pages;
  bool ok;
  {
    PageHeapSpinLockHolder l;
    ok = allocator_.GetPageAllocationStatus(sibling_hp, pages,
                                            MemoryTag::kNormal);
  }
  EXPECT_FALSE(ok);

  Delete(s, kSpanInfo, MemoryTag::kNormal);
}

TEST_F(PageAllocatorTest, PageMap3UnallocatedHugePageRawHugePageSibling) {
  constexpr size_t kLeafBits = (kAddressBits - kPageShift + 2) / 3;
  constexpr size_t kLeafLength = size_t{1} << kLeafBits;
  constexpr SpanAllocInfo kSpanInfo = {/*objects_per_span=*/1,
                                       AccessDensityPrediction::kSparse};
  Span* s = New(kPagesPerHugePage, kSpanInfo, MemoryTag::kNormal);
  ASSERT_NE(s, nullptr);
  HugePage hp0 = HugePageContaining(s->first_page());
  const size_t leaf_base_idx = s->first_page().index() & ~(kLeafLength - 1);
  HugePage leaf_base = HugePageContaining(PageId{leaf_base_idx});
  HugePage sibling_hp = (hp0 == leaf_base) ? (hp0 + NHugePages(1)) : leaf_base;

  ASSERT_TRUE(tc_globals.pagemap().HasLeaf(sibling_hp.first_page()));

  PageBitmap pages;
  bool ok;
  {
    PageHeapSpinLockHolder l;
    ok = allocator_.GetPageAllocationStatus(sibling_hp, pages,
                                            MemoryTag::kNormal);
  }
  EXPECT_FALSE(ok);

  Delete(s, kSpanInfo, MemoryTag::kNormal);
}

TEST_F(PageAllocatorTest, MultiHugePageWithRecycledTombstone) {
  constexpr SpanAllocInfo kSpanInfo = {/*objects_per_span=*/1,
                                       AccessDensityPrediction::kSparse};
  // Allocate 3 raw hugepages.
  Span* s = New(kPagesPerHugePage * 3, kSpanInfo, MemoryTag::kNormal);
  ASSERT_NE(s, nullptr);
  HugePage hp0 = HugePageContaining(s->first_page());
  HugePage hp1 = hp0 + NHugePages(1);
  HugePage hp2 = hp0 + NHugePages(2);

  // Simulate an intermediate hugepage having an invalid_span tombstone
  // from a prior allocation/free cycle.
  tc_globals.pagemap().Set(hp1.first_page(),
                           const_cast<Span*>(&tc_globals.invalid_span()));

  PageBitmap pages;
  bool ok;
  {
    PageHeapSpinLockHolder l;
    ok = allocator_.GetPageAllocationStatus(hp2, pages, MemoryTag::kNormal);
  }
  EXPECT_TRUE(ok);

  Delete(s, kSpanInfo, MemoryTag::kNormal);
}

}  // namespace
}  // namespace tcmalloc_internal
}  // namespace tcmalloc
