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
#include <atomic>
#include <limits>
#include <memory>
#include <new>
#include <string>
#include <thread>  // NOLINT(build/c++11)
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
    memset(storage_, 0, sizeof(Map));
    map_ = new (storage_) Map();
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
  Map* map_;

 private:
  alignas(Map) char storage_[sizeof(Map)];
};

TEST_P(PageMapTest, Sequential) {
  const intptr_t limit = GetParam();

  for (intptr_t i = 0; i < limit; i++) {
    ASSERT_TRUE(map_->Ensure(Range(PageId(i), Length(1))));
    map_->SetLarge(PageId(i), Length(i + 1), (i & 1) != 0);
    ASSERT_TRUE(map_->GetDescriptor(PageId(i)).valid());
    ASSERT_FALSE(map_->GetDescriptor(PageId(i)).sampled());
    ASSERT_FALSE(map_->GetDescriptor(PageId(i)).freed());
    ASSERT_EQ(map_->GetDescriptor(PageId(i)).size(), Length(i + 1));
    ASSERT_EQ(map_->GetDescriptor(PageId(i)).donated(), (i & 1) != 0);
    ASSERT_EQ(map_->GetExistingDescriptor(PageId(i)).size(), Length(i + 1));

    // Test size class handling
    ASSERT_EQ(0, map_->sizeclass(PageId(i)));
    map_->SetSmall(PageId(i), span(i), sc(i), false);
    ASSERT_EQ(sc(i), map_->sizeclass(PageId(i)));
  }
  for (intptr_t i = 0; i < limit; i++) {
    ASSERT_EQ(map_->GetDescriptor(PageId(i)).span(), span(i));
    ASSERT_EQ(map_->GetExistingDescriptor(PageId(i)).span(), span(i));
    ASSERT_EQ(map_->sizeclass(PageId(i)), sc(i));
    ASSERT_EQ(map_->GetDescriptor(PageId(i)).sizeclass(), sc(i));
  }
}

TEST_P(PageMapTest, Bulk) {
  const intptr_t limit = GetParam();

  ASSERT_TRUE(map_->Ensure(Range(PageId(0), Length(limit))));
  for (intptr_t i = 0; i < limit; i++) {
    map_->SetSmall(PageId(i), span(i), sc(i), false);
    ASSERT_EQ(map_->GetDescriptor(PageId(i)).span(), span(i));
  }
  for (intptr_t i = 0; i < limit; i++) {
    ASSERT_EQ(map_->GetDescriptor(PageId(i)).span(), span(i));
  }
}

TEST_P(PageMapTest, Overflow) {
  const uintptr_t kLimit = uintptr_t{1} << kTestBits;
  ASSERT_FALSE(map_->Ensure(Range(PageId(kLimit), Length(kLimit + 1))));
}

TEST_P(PageMapTest, EnsureOverflowAndPartialBounds) {
  const uintptr_t kLimit = uintptr_t{1} << kTestBits;
  const Length kMaxLen(std::numeric_limits<uintptr_t>::max());
  EXPECT_FALSE(map_->Ensure(Range(PageId(2), kMaxLen)));
  EXPECT_FALSE(map_->HasLeaf(PageId(2)));
  EXPECT_FALSE(map_->Ensure(Range(PageId(kLimit), kMaxLen)));
  EXPECT_FALSE(map_->Ensure(Range(PageId(kLimit - 1), Length(2))));
  EXPECT_FALSE(map_->HasLeaf(PageId(kLimit - 1)));
  EXPECT_FALSE(map_->Ensure(Range(PageId(1), kMaxLen)));
  EXPECT_FALSE(map_->HasLeaf(PageId(1)));
}

TEST_P(PageMapTest, RandomAccess) {
  const intptr_t limit = GetParam();

  std::vector<intptr_t> elements;
  for (intptr_t i = 0; i < limit; i++) {
    elements.push_back(i);
  }
  std::shuffle(elements.begin(), elements.end(), absl::BitGen());

  for (intptr_t i = 0; i < limit; i++) {
    ASSERT_TRUE(map_->Ensure(Range(PageId(elements[i]), Length(1))));
    map_->SetSmall(PageId(elements[i]), span(elements[i]), sc(elements[i]),
                   false);
    ASSERT_EQ(map_->GetDescriptor(PageId(elements[i])).span(),
              span(elements[i]));
  }
  for (intptr_t i = 0; i < limit; i++) {
    ASSERT_EQ(map_->GetDescriptor(PageId(i)).span(), span(i));
  }
}

TEST_P(PageMapTest, ConcurrentUpdateAndReadDescriptor) {
#if defined(ABSL_HAVE_THREAD_SANITIZER)
  GTEST_SKIP() << "PageMap does not run cleanly under TSan (b/193887621).";
#else
  const intptr_t limit = std::min<intptr_t>(GetParam(), 256);
  std::vector<std::unique_ptr<Span> > spans(limit);
  for (intptr_t i = 0; i < limit; ++i) {
    spans[i] = std::make_unique<Span>(PageId(i + 1));
  }

  std::atomic<bool> stop{false};
  auto reader_fn = [&]() {
    while (!stop.load(std::memory_order_relaxed)) {
      for (intptr_t i = 0; i < limit; ++i) {
        const PageId p(i + 1);
        const CompactSizeClass cls = map_->sizeclass(p);
        EXPECT_TRUE(cls == 0 || cls == sc(i));
        const PageMeta meta = map_->GetDescriptor(p);
        if (meta.valid()) {
          if (meta.sizeclass() != 0) {
            EXPECT_EQ(meta.sizeclass(), sc(i));
            EXPECT_EQ(meta.span(), spans[i].get());
          } else {
            EXPECT_EQ(meta.size(), Length(i + 1));
          }
        }
      }
      (void)map_->get_next_set_page(PageId(0));
    }
  };
  std::thread reader(reader_fn);

  for (int iter = 0; iter < 50; ++iter) {
    for (intptr_t i = 0; i < limit; ++i) {
      const PageId p(i + 1);
      ASSERT_TRUE(map_->Ensure(Range(p, Length(1))));
      map_->RegisterSizeClass(spans[i].get(), Length(1), sc(i),
                              /*donated=*/false);
      EXPECT_FALSE(map_->UnregisterSizeClass(spans[i].get(), Length(1)));
      map_->SetLarge(p, Length(i + 1), /*donated=*/(i & 1) != 0);
      map_->SetFreed(p);
    }
  }

  stop.store(true, std::memory_order_relaxed);
  reader.join();
#endif
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
