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
#include <mutex>
#include <new>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "gtest/gtest.h"
#include "absl/random/random.h"
#include "tcmalloc/common.h"
#include "tcmalloc/internal/config.h"
#include "tcmalloc/span.h"

// Note: we leak memory every time a map is constructed, so do not
// create too many maps.

namespace tcmalloc {
namespace tcmalloc_internal {
namespace {

// Pick span pointer to use for page numbered i
Span* span(intptr_t i) { return reinterpret_cast<Span*>(i + 1); }

// Pick sizeclass to use for page numbered i
uint8_t sc(intptr_t i) { return (i % 15) + 1; }

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
    map->SetLarge(PageId(i), Length(i + 1), (i & 1) != 0);
    ASSERT_TRUE(map->GetDescriptor(PageId(i)).valid());
    ASSERT_FALSE(map->GetDescriptor(PageId(i)).sampled());
    ASSERT_FALSE(map->GetDescriptor(PageId(i)).freed());
    ASSERT_EQ(map->GetDescriptor(PageId(i)).size(), Length(i + 1));
    ASSERT_EQ(map->GetDescriptor(PageId(i)).donated(), (i & 1) != 0);
    ASSERT_EQ(map->GetExistingDescriptor(PageId(i)).size(), Length(i + 1));

    // Test size class handling
    ASSERT_EQ(0, map->sizeclass(PageId(i)));
    map->SetSmall(PageId(i), span(i), sc(i), false);
    ASSERT_EQ(sc(i), map->sizeclass(PageId(i)));
  }
  for (intptr_t i = 0; i < limit; i++) {
    ASSERT_EQ(map->GetDescriptor(PageId(i)).span(), span(i));
    ASSERT_EQ(map->GetExistingDescriptor(PageId(i)).span(), span(i));
    ASSERT_EQ(map->sizeclass(PageId(i)), sc(i));
    ASSERT_EQ(map->GetDescriptor(PageId(i)).sizeclass(), sc(i));
  }
}

TEST_P(PageMapTest, Bulk) {
  const intptr_t limit = GetParam();

  ASSERT_TRUE(map->Ensure(Range(PageId(0), Length(limit))));
  for (intptr_t i = 0; i < limit; i++) {
    map->SetSmall(PageId(i), span(i), sc(i), false);
    ASSERT_EQ(map->GetDescriptor(PageId(i)).span(), span(i));
  }
  for (intptr_t i = 0; i < limit; i++) {
    ASSERT_EQ(map->GetDescriptor(PageId(i)).span(), span(i));
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
    map->SetSmall(PageId(elements[i]), span(elements[i]), sc(elements[i]),
                  false);
    ASSERT_EQ(map->GetDescriptor(PageId(elements[i])).span(),
              span(elements[i]));
  }
  for (intptr_t i = 0; i < limit; i++) {
    ASSERT_EQ(map->GetDescriptor(PageId(i)).span(), span(i));
  }
}

INSTANTIATE_TEST_SUITE_P(Limits, PageMapTest, ::testing::Values(100, 1 << 16));

TEST_F(PageMapTest, ConcurrentAccess) {
  constexpr int kNumThreads = 4;
  constexpr int kIterations = 200;
  std::mutex pageheap_mu;

  ASSERT_TRUE(map->Ensure(Range(PageId(0), Length(1))));

  std::vector<std::thread> threads;
  threads.reserve(kNumThreads);
  for (int t = 0; t < kNumThreads; ++t) {
    threads.emplace_back([&, t]() {
      const PageId base_p((t + 1) * 256);
      Span small_span(base_p);
      auto* sampled_alloc =
          reinterpret_cast<SampledAllocation*>((uintptr_t(t) + 1) << 6);
      for (int iter = 0; iter < kIterations; ++iter) {
        const PageId mid_p(((iter * kNumThreads + t + 1) & 63) << 12);
        const PageId root_p((uintptr_t(t + 1) << 24) | ((iter & 7) << 12));
        {
          std::lock_guard<std::mutex> l(pageheap_mu);
          ASSERT_TRUE(map->Ensure(Range(base_p, Length(4))));
          ASSERT_TRUE(map->Ensure(Range(mid_p, Length(1))));
          ASSERT_TRUE(map->Ensure(Range(root_p, Length(1))));
          map->SetHugepage(PageId(0),
                           reinterpret_cast<void*>(uintptr_t(iter + 1)));
          map->SetHugepage(base_p,
                           reinterpret_cast<void*>(uintptr_t(iter + 1)));
        }

        (void)map->GetHugepage(PageId(0));
        EXPECT_NE(map->GetHugepage(base_p), nullptr);

        const bool donated = (iter & 1) != 0;
        map->RegisterSizeClass(&small_span, Length(2), sc(t), donated);
        EXPECT_EQ(map->sizeclass(base_p), sc(t));
        EXPECT_EQ(map->GetDescriptor(base_p).span(), &small_span);
        EXPECT_EQ(map->GetExistingDescriptor(base_p + Length(1)).span(),
                  &small_span);
        EXPECT_EQ(map->UnregisterSizeClass(&small_span, Length(2)), donated);
        EXPECT_EQ(map->sizeclass(base_p), 0);
        EXPECT_TRUE(map->GetDescriptor(base_p).freed());

        const PageId large_p = base_p + Length(2);
        map->SetLarge(large_p, Length(4), donated);
        EXPECT_EQ(map->GetDescriptor(large_p).size(), Length(4));
        EXPECT_EQ(map->GetDescriptor(large_p).donated(), donated);
        map->SetFreed(large_p);
        EXPECT_TRUE(map->GetDescriptor(large_p).freed());

        const PageId sampled_p = base_p + Length(3);
        map->SetSampled(sampled_p, sampled_alloc, donated);
        EXPECT_TRUE(map->GetDescriptor(sampled_p).sampled());
        EXPECT_EQ(map->GetDescriptor(sampled_p).sampled_allocation(),
                  sampled_alloc);
        map->SetFreed(sampled_p);

        EXPECT_TRUE(map->HasLeaf(mid_p));
        (void)map->GetDescriptor(mid_p);
        (void)map->sizeclass(root_p);
        (void)map->get_next_set_page(base_p);
      }
    });
  }
  for (auto& th : threads) {
    th.join();
  }
}

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
