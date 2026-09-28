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

#include "tcmalloc/pagemap.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include <algorithm>
#include <new>
#include <string>
#include <utility>
#include <vector>

#include "gtest/gtest.h"
#include "absl/random/random.h"
#include "tcmalloc/common.h"
#include "tcmalloc/internal/config.h"
#include "tcmalloc/internal/memory_tag.h"
#include "tcmalloc/internal/sampled_allocation.h"
#include "tcmalloc/span.h"
#include "tcmalloc/static_vars.h"

// Note: we leak memory every time a map is constructed, so do not
// create too many maps.

namespace tcmalloc {
namespace tcmalloc_internal {
namespace {

// Pick sizeclass to use for page numbered i
CompactSizeClass sc(intptr_t i) { return (i % 64) + 1; }

class PageMapTest : public ::testing::TestWithParam<int> {
 public:
  PageMapTest() {
    // Arrange to pass zero-filled memory as the backing store for map.
    memset(storage, 0, sizeof(Map));
    map = new (storage) Map();
  }

  ~PageMapTest() override {
    for (void* ptr : *ptrs()) {
      ::operator delete(ptr);
    }
    ptrs()->clear();
  }

 private:
  static std::vector<void*>* ptrs() {
    static std::vector<void*>* ret = new std::vector<void*>();
    return ret;
  }

  static void* alloc(size_t n) {
    void* ptr = ::operator new(n);
    ptrs()->push_back(ptr);
    return ptr;
  }

 public:
  static constexpr int kTestBits = kAddressBits - kPageShift;
  using Map = PageMap<kTestBits, alloc>;
  Map* map;

 private:
  alignas(Map) char storage[sizeof(Map)];
};

TEST_P(PageMapTest, Sequential) {
  const intptr_t limit = GetParam();

  for (intptr_t i = 0; i < limit; i++) {
    ASSERT_TRUE(map->Ensure(Range(PageId(i), Length(1))));
    ASSERT_EQ(0, map->sizeclass(PageId(i)));
    SpanMeta empty_meta =
        map->GetSpanMeta(PageId(i))->load(std::memory_order_relaxed);
    ASSERT_EQ(empty_meta.is_small, 0);
    ASSERT_EQ(empty_meta.num_pages_or_sampled_index, 0);

    const CompactSizeClass c = sc(i);
    map->RegisterSmallSpan(PageId(i), Length(1), c, /*donated=*/(i % 2) != 0);
    ASSERT_EQ(c, map->sizeclass(PageId(i)));
    SpanMeta meta =
        map->GetSpanMeta(PageId(i))->load(std::memory_order_relaxed);
    ASSERT_EQ(meta.is_small, 1);
    ASSERT_EQ(meta.size_class, c);
    ASSERT_EQ(meta.is_donated, (i % 2) != 0);
  }
  for (intptr_t i = 0; i < limit; i++) {
    const CompactSizeClass c = sc(i);
    ASSERT_EQ(map->sizeclass(PageId(i)), c);
    SpanMeta meta =
        map->GetSpanMeta(PageId(i))->load(std::memory_order_relaxed);
    ASSERT_EQ(meta.is_small, 1);
    ASSERT_EQ(meta.size_class, c);
    ASSERT_EQ(meta.is_donated, (i % 2) != 0);
  }
}

TEST_P(PageMapTest, Bulk) {
  const intptr_t limit = GetParam();

  ASSERT_TRUE(map->Ensure(Range(PageId(0), Length(limit))));
  for (intptr_t i = 0; i < limit; i++) {
    map->RegisterSmallSpan(PageId(i), Length(1), sc(i), false);
    ASSERT_EQ(map->sizeclass(PageId(i)), sc(i));
  }
  for (intptr_t i = 0; i < limit; i++) {
    ASSERT_EQ(map->sizeclass(PageId(i)), sc(i));
  }
  for (intptr_t i = 0; i < limit; i++) {
    map->UnregisterSmallSpan(PageId(i), Length(1));
    ASSERT_EQ(map->sizeclass(PageId(i)), 0);
  }
}

TEST_P(PageMapTest, Overflow) {
  const uintptr_t kLimit = uintptr_t{1} << kTestBits;
  ASSERT_FALSE(map->Ensure(Range(PageId(kLimit), Length(kLimit + 1))));
}

TEST_P(PageMapTest, RandomAccess) {
  const intptr_t limit = GetParam();

  std::vector<intptr_t> elements;
  for (intptr_t i = 0; i < limit; i++) {
    elements.push_back(i);
  }
  std::shuffle(elements.begin(), elements.end(), absl::BitGen());

  for (intptr_t i = 0; i < limit; i++) {
    ASSERT_TRUE(map->Ensure(Range(PageId(elements[i]), Length(1))));
    map->RegisterSmallSpan(PageId(elements[i]), Length(1), sc(elements[i]),
                           false);
    ASSERT_EQ(map->sizeclass(PageId(elements[i])), sc(elements[i]));
  }
  for (intptr_t i = 0; i < limit; i++) {
    ASSERT_EQ(map->sizeclass(PageId(elements[i])), sc(elements[i]));
  }
}

TEST_P(PageMapTest, LargeSpan) {
  const intptr_t limit = std::min<intptr_t>(GetParam(), 1000);
  for (intptr_t i = 0; i < limit; i += 10) {
    ASSERT_TRUE(map->Ensure(Range(PageId(i), Length(10))));
    map->RegisterLargeSpan(PageId(i), Length(10), /*donated=*/false,
                           /*is_sampled=*/true);
    SpanMeta meta =
        map->GetSpanMeta(PageId(i))->load(std::memory_order_relaxed);
    ASSERT_EQ(meta.is_small, 0);
    ASSERT_EQ(meta.is_sampled, 1);
    ASSERT_EQ(meta.num_pages_or_sampled_index, 10);
    ASSERT_EQ(map->sizeclass(PageId(i)), 0);

    map->UnregisterLargeSpan(PageId(i), Length(10));
    meta = map->GetSpanMeta(PageId(i))->load(std::memory_order_relaxed);
    ASSERT_EQ(meta.is_sampled, 0);
    ASSERT_EQ(meta.num_pages_or_sampled_index, 0);
  }
}

TEST_P(PageMapTest, HugePage) {
  const intptr_t limit = std::min<intptr_t>(GetParam(), 1000);
  for (intptr_t i = 0; i < limit; ++i) {
    ASSERT_TRUE(map->Ensure(Range(PageId(i), Length(1))));
    void* val = reinterpret_cast<void*>(static_cast<uintptr_t>(i + 42));
    map->SetHugepage(PageId(i), val);
    ASSERT_EQ(map->GetHugepage(PageId(i)), val);
    map->SetHugepage(PageId(i), nullptr);
    ASSERT_EQ(map->GetHugepage(PageId(i)), nullptr);
  }
}

TEST_P(PageMapTest, MultiPageSmallSpan) {
  const intptr_t limit = std::min<intptr_t>(GetParam(), 1000);
  for (intptr_t i = 0; i + 4 <= limit; i += 4) {
    ASSERT_TRUE(map->Ensure(Range(PageId(i), Length(4))));
    const CompactSizeClass c = sc(i);
    map->RegisterSmallSpan(PageId(i), Length(4), c, /*donated=*/true);
    for (int offset = 0; offset < 4; ++offset) {
      ASSERT_EQ(map->sizeclass(PageId(i + offset)), c);
      SpanMeta meta =
          map->GetSpanMeta(PageId(i + offset))->load(std::memory_order_relaxed);
      ASSERT_EQ(meta.is_small, 1);
      ASSERT_EQ(meta.size_class, c);
      ASSERT_EQ(meta.heap_index_or_page_offset, offset);
      ASSERT_EQ(meta.is_in_cfl, 0);
      ASSERT_EQ(meta.is_donated, 1);
    }
    map->UnregisterSmallSpan(PageId(i), Length(4));
    for (int offset = 0; offset < 4; ++offset) {
      ASSERT_EQ(map->sizeclass(PageId(i + offset)), 0);
    }
  }
}

TEST_P(PageMapTest, GetSpanMetaNullable) {
  // Unallocated page returns nullopt.
  EXPECT_EQ(map->GetSpanMetaNullable(PageId(100)), std::nullopt);

  // Allocated but empty/unregistered page returns nullopt.
  ASSERT_TRUE(map->Ensure(Range(PageId(100), Length(1))));
  EXPECT_EQ(map->GetSpanMetaNullable(PageId(100)), std::nullopt);

  // Registered small span returns non-nullopt valid meta.
  map->RegisterSmallSpan(PageId(100), Length(1), 1, false);
  std::optional<SpanMeta> meta = map->GetSpanMetaNullable(PageId(100));
  ASSERT_TRUE(meta.has_value());
  EXPECT_TRUE(meta->IsValid());
  EXPECT_EQ(meta->is_small, 1);

  // Unregistered (freed) span returns nullopt.
  map->UnregisterSmallSpan(PageId(100), Length(1));
  EXPECT_EQ(map->GetSpanMetaNullable(PageId(100)), std::nullopt);

  // Registered large span returns non-nullopt valid meta.
  map->RegisterLargeSpan(PageId(100), Length(2), false, false);
  meta = map->GetSpanMetaNullable(PageId(100));
  ASSERT_TRUE(meta.has_value());
  EXPECT_TRUE(meta->IsValid());
  EXPECT_EQ(meta->num_pages_or_sampled_index, 2);

  // Unregistered (freed) large span returns nullopt.
  map->UnregisterLargeSpan(PageId(100), Length(2));
  EXPECT_EQ(map->GetSpanMetaNullable(PageId(100)), std::nullopt);
}

TEST_P(PageMapTest, GetNextSetPage) {
  // Empty pagemap returns nullopt.
  EXPECT_EQ(map->get_next_set_page(PageId(0)), std::nullopt);

  // When a leaf is allocated, get_next_set_page returns pages in that leaf.
  ASSERT_TRUE(map->Ensure(Range(PageId(100), Length(1))));
  EXPECT_EQ(map->get_next_set_page(PageId(99)), PageId(100));
  EXPECT_EQ(map->get_next_set_page(PageId(100)), PageId(101));
}

TEST(PageMapClassTest, BasicSampledOperations) {
  ProdPageMap pm;
  PageId p1 = PageId(100);
  PageId p2 = PageId(101);
  {
    PageHeapSpinLockHolder l;
    ASSERT_TRUE(pm.Ensure(Range(p1, Length(2))));
  }

  StackTrace st1;
  StackTrace st2;
  SampledAllocation* s1 =
      tc_globals.sampled_allocation_recorder().Register(std::move(st1));
  SampledAllocation* s2 =
      tc_globals.sampled_allocation_recorder().Register(std::move(st2));
  s1->sampled_stack.allocated_size = 12345;
  s2->sampled_stack.allocated_size = 67890;

  EXPECT_EQ(pm.GetLargeSize(p1), 0);
  EXPECT_EQ(pm.GetLargeSize(p2), 0);

  pm.RegisterSampledSpan(p1, s1);
  EXPECT_EQ(pm.GetLargeSize(p1), 12345);
  EXPECT_EQ(pm.GetLargeSize(p2), 0);

  pm.RegisterSampledSpan(p2, s2);
  EXPECT_EQ(pm.GetLargeSize(p1), 12345);
  EXPECT_EQ(pm.GetLargeSize(p2), 67890);

  EXPECT_EQ(pm.UnregisterSampledSpan(p1), s1);
  EXPECT_EQ(pm.GetLargeSize(p1), 0);
  EXPECT_EQ(pm.GetLargeSize(p2), 67890);

  EXPECT_EQ(pm.UnregisterSampledSpan(p2), s2);
  EXPECT_EQ(pm.GetLargeSize(p2), 0);

  tc_globals.sampled_allocation_recorder().Unregister(s1);
  tc_globals.sampled_allocation_recorder().Unregister(s2);
}

INSTANTIATE_TEST_SUITE_P(Limits, PageMapTest, ::testing::Values(100, 1 << 16));

// Surround pagemap with unused memory. This isolates it so that it does not
// share pages with any other structures. This avoids the risk that adjacent
// objects might cause it to be mapped in. The padding is of sufficient size
// that this is true even if this structure is mapped with huge pages.
static struct PaddedPageMap {
  constexpr PaddedPageMap() : padding_before{}, pagemap{}, padding_after{} {}
  uint64_t padding_before[kHugePageSize / sizeof(uint64_t)];
  ProdPageMap pagemap;
  uint64_t padding_after[kHugePageSize / sizeof(uint64_t)];
} padded_pagemap_;

TEST(TestMemoryFootprint, Test) {
  uint64_t pagesize = sysconf(_SC_PAGESIZE);
  ASSERT_NE(pagesize, 0);
  size_t pages = sizeof(ProdPageMap) / pagesize + 1;
  std::vector<unsigned char> present(pages);

  // mincore needs the address rounded to the start page
  uint64_t basepage =
      reinterpret_cast<uintptr_t>(&padded_pagemap_.pagemap) & ~(pagesize - 1);
  ASSERT_EQ(mincore(reinterpret_cast<void*>(basepage), sizeof(ProdPageMap),
                    present.data()),
            0);
  for (int i = 0; i < pages; i++) {
    EXPECT_EQ(present[i], 0);
  }
}

}  // namespace
}  // namespace tcmalloc_internal
}  // namespace tcmalloc
