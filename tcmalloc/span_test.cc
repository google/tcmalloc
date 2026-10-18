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

#include "tcmalloc/span.h"

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#include <memory>
#include <new>
#include <string>
#include <utility>
#include <vector>

#include "benchmark/benchmark.h"
#include "gtest/gtest.h"
#include "absl/base/internal/spinlock.h"
#include "absl/base/optimization.h"
#include "absl/container/flat_hash_map.h"
#include "absl/container/flat_hash_set.h"
#include "absl/random/random.h"
#include "absl/types/span.h"
#include "tcmalloc/common.h"
#include "tcmalloc/internal/logging.h"
#include "tcmalloc/pages.h"
#include "tcmalloc/static_vars.h"

namespace tcmalloc {
namespace tcmalloc_internal {
namespace {

class RawSpan {
 public:
  void Init(size_t size_class) {
    size_t size = tc_globals.sizemap().class_to_size(size_class);
    auto npages = tc_globals.sizemap().class_to_pages(size_class);
    size_t objects_per_span = npages.in_bytes() / size;

    int res = posix_memalign(&mem_, kPageSize, npages.in_bytes());
    TC_CHECK_EQ(res, 0);

    // Dynamically allocate so ASan can flag if we run out of bounds.
    span_ = std::make_unique<Span>(PageIdContaining(mem_));
    TC_CHECK_EQ(span_->BuildFreelist(size, objects_per_span, {}), 0);
  }

  ~RawSpan() {
    free(mem_);
  }

  Span& span() { return *span_; }

 private:
  void* mem_ = nullptr;
  std::unique_ptr<Span> span_;
};

class SpanTest : public testing::TestWithParam<size_t> {
 protected:
  size_t size_class_;
  size_t size_;
  Length npages_;
  size_t batch_size_;
  size_t objects_per_span_;
  uint32_t reciprocal_;
  RawSpan raw_span_;

 private:
  void SetUp() override {
#if ABSL_HAVE_HWADDRESS_SANITIZER
    GTEST_SKIP()
        << "Skipping under HWASan, which uses the top bits of the pointer.";
#endif

    size_class_ = GetParam();
    size_ = tc_globals.sizemap().class_to_size(size_class_);
    if (size_ == 0) {
      GTEST_SKIP() << "Skipping empty size class.";
    }

    npages_ = tc_globals.sizemap().class_to_pages(size_class_);
    batch_size_ = tc_globals.sizemap().num_objects_to_move(size_class_);
    objects_per_span_ = npages_.in_bytes() / size_;
    reciprocal_ = Span::CalcReciprocal(size_);

    raw_span_.Init(size_class_);
  }

  void TearDown() override {}
};

TEST_P(SpanTest, FreelistBasic) {
  Span& span_ = raw_span_.span();

  EXPECT_FALSE(span_.FreelistEmpty(objects_per_span_));
  void* batch[kMaxObjectsToMove];
  size_t popped = 0;
  size_t want = 1;
  char* start = static_cast<char*>(span_.start_address());
  std::vector<bool> objects(objects_per_span_);
  for (size_t x = 0; x < 2; ++x) {
    // Pop all objects in batches of varying size and ensure that we've got
    // all objects.
    for (;;) {
      size_t n = span_.FreelistPopBatch(absl::MakeSpan(batch, want), size_);
      popped += n;
      EXPECT_EQ(span_.FreelistEmpty(objects_per_span_),
                popped == objects_per_span_);
      for (size_t i = 0; i < n; ++i) {
        void* p = batch[i];
        uintptr_t off = reinterpret_cast<char*>(p) - start;
        EXPECT_LT(off, npages_.in_bytes());
        EXPECT_EQ(off % size_, 0);
        size_t idx = off / size_;
        EXPECT_FALSE(objects[idx]);
        objects[idx] = true;
      }
      if (n < want) {
        break;
      }
      ++want;
      if (want > batch_size_) {
        want = 1;
      }
    }
    EXPECT_TRUE(span_.FreelistEmpty(objects_per_span_));
    EXPECT_EQ(span_.FreelistPopBatch(absl::MakeSpan(batch, 1), size_), 0);
    EXPECT_EQ(popped, objects_per_span_);

    // Push all objects back except the last one (which would not be pushed).
    for (size_t idx = 0; idx < objects_per_span_ - 1; ++idx) {
      EXPECT_TRUE(objects[idx]);
      void* ptr = start + idx * size_;
      Span::ObjIdx objidx;
      Span::ObjectsToIdx({&ptr, 1}, &span_, size_, reciprocal_, &objidx);
      bool ok = span_.FreelistPushBatch(absl::MakeSpan(&objidx, 1), size_);
      EXPECT_TRUE(ok);
      EXPECT_FALSE(span_.FreelistEmpty(objects_per_span_));
      objects[idx] = false;
      --popped;
    }
    // On the last iteration we can actually push the last object.
    if (x == 1) {
      void* ptr = start + (objects_per_span_ - 1) * size_;
      Span::ObjIdx objidx;
      Span::ObjectsToIdx({&ptr, 1}, &span_, size_, reciprocal_, &objidx);
      bool ok = span_.FreelistPushBatch(absl::MakeSpan(&objidx, 1), size_);
      EXPECT_FALSE(ok);
      EXPECT_EQ(span_.Allocated(), 0);
    }
  }
}

TEST(SpanDeathTest, DoubleFreeAndCorruptedFreelist) {
#if ABSL_HAVE_HWADDRESS_SANITIZER
  GTEST_SKIP()
      << "Skipping under HWASan, which uses the top bits of the pointer.";
#endif

  size_t bitmap_sc = 0;
  size_t list_sc = 0;
  for (size_t sc = 1; sc < kNumClasses; ++sc) {
    const size_t size = tc_globals.sizemap().class_to_size(sc);
    if (size == 0) continue;
    const size_t objects_per_span =
        tc_globals.sizemap().class_to_pages(sc).in_bytes() / size;
    if (Span::UseBitmapForSize(size) && objects_per_span >= 4 &&
        bitmap_sc == 0) {
      bitmap_sc = sc;
    }
    if (!Span::UseBitmapForSize(size) && objects_per_span >= 32 &&
        size / sizeof(Span::ObjIdx) >= 4 && list_sc == 0) {
      list_sc = sc;
    }
  }
  ASSERT_NE(bitmap_sc, 0);
  ASSERT_NE(list_sc, 0);

  // 1. Pushing when allocated < batch.size() reports double free.
  {
    RawSpan raw;
    raw.Init(bitmap_sc);
    Span& span = raw.span();
    const size_t size = tc_globals.sizemap().class_to_size(bitmap_sc);
    const uint32_t reciprocal = Span::CalcReciprocal(size);
    void* ptr = nullptr;
    ASSERT_EQ(span.FreelistPopBatch(absl::MakeSpan(&ptr, 1), size), 1);
    Span::ObjIdx idx;
    Span::ObjectsToIdx({&ptr, 1}, &span, size, reciprocal, &idx);
    EXPECT_FALSE(span.FreelistPushBatch(absl::MakeSpan(&idx, 1), size));
    EXPECT_EQ(span.Allocated(), 0);
    EXPECT_DEATH((void)span.FreelistPushBatch(absl::MakeSpan(&idx, 1), size),
                 "Possible double free detected");
  }

  // 2. Double-freeing an object on a bitmap span reports double free both when
  // allocated > batch.size() and when allocated == batch.size().
  {
    RawSpan raw;
    raw.Init(bitmap_sc);
    Span& span = raw.span();
    const size_t size = tc_globals.sizemap().class_to_size(bitmap_sc);
    const uint32_t reciprocal = Span::CalcReciprocal(size);
    void* ptrs[3];
    ASSERT_EQ(span.FreelistPopBatch(absl::MakeSpan(ptrs, 3), size), 3);
    Span::ObjIdx idx0;
    Span::ObjectsToIdx({&ptrs[0], 1}, &span, size, reciprocal, &idx0);
    EXPECT_TRUE(span.FreelistPushBatch(absl::MakeSpan(&idx0, 1), size));
    EXPECT_EQ(span.Allocated(), 2);
    // allocated (2) > batch.size() (1), but idx0 is already free in bitmap_.
    EXPECT_DEATH((void)span.FreelistPushBatch(absl::MakeSpan(&idx0, 1), size),
                 "Possible double free detected");

    Span::ObjIdx idx1;
    Span::ObjectsToIdx({&ptrs[1], 1}, &span, size, reciprocal, &idx1);
    EXPECT_TRUE(span.FreelistPushBatch(absl::MakeSpan(&idx1, 1), size));
    EXPECT_EQ(span.Allocated(), 1);
    // allocated (1) == batch.size() (1), and idx0 is already free in bitmap_
    // while ptrs[2] is still live.
    EXPECT_DEATH((void)span.FreelistPushBatch(absl::MakeSpan(&idx0, 1), size),
                 "Possible double free detected");
  }

  // 3. Out-of-bounds ObjIdx in a list-mode span's embedded freelist aborts with
  // a corrupted freelist error in both debug and opt builds.
  {
    RawSpan raw;
    raw.Init(list_sc);
    Span& span = raw.span();
    const size_t size = tc_globals.sizemap().class_to_size(list_sc);
    const size_t objects_per_span =
        tc_globals.sizemap().class_to_pages(list_sc).in_bytes() / size;
    const uint32_t reciprocal = Span::CalcReciprocal(size);

    std::vector<void*> all(objects_per_span);
    size_t popped = 0;
    while (popped < objects_per_span) {
      size_t want =
          std::min<size_t>(objects_per_span - popped, kMaxObjectsToMove);
      size_t got =
          span.FreelistPopBatch(absl::MakeSpan(&all[popped], want), size);
      ASSERT_GT(got, 0);
      popped += got;
    }
    ASSERT_TRUE(span.FreelistEmpty(objects_per_span));

    // Return 12 (cache) + 1 (host1) + (size/2 - 1) (embedded in host1) + 1
    // (host2) objects.
    const size_t limit = size / sizeof(Span::ObjIdx) - 1;
    const size_t to_return = 12 + 1 + limit + 1;
    ASSERT_LT(to_return, objects_per_span);
    std::vector<Span::ObjIdx> idxs(to_return);
    Span::ObjectsToIdx(absl::MakeSpan(all.data(), to_return), &span, size,
                       reciprocal, idxs.data());
    EXPECT_TRUE(span.FreelistPushBatch(absl::MakeSpan(idxs), size));

    // Corrupt host1's top embedded slot to an out-of-page ObjIdx.
    auto* host1 = static_cast<Span::ObjIdx*>(all[12]);
    host1[limit] = static_cast<Span::ObjIdx>(kPageSize >> kAlignmentShift);

    // Drain the 12 cached objects and host2 (13 objects total).
    void* out[13];
    ASSERT_EQ(span.FreelistPopBatch(absl::MakeSpan(out, 13), size), 13);

    // Next pop reads host1[limit] and must abort with Corrupted freelist.
    EXPECT_DEATH((void)span.FreelistPopBatch(absl::MakeSpan(out, 1), size),
                 "Corrupted freelist");
  }
  {
    RawSpan raw;
    raw.Init(list_sc);
    Span& span = raw.span();
    const size_t size = tc_globals.sizemap().class_to_size(list_sc);
    const size_t objects_per_span =
        tc_globals.sizemap().class_to_pages(list_sc).in_bytes() / size;
    const uint32_t reciprocal = Span::CalcReciprocal(size);

    std::vector<void*> all(objects_per_span);
    size_t popped = 0;
    while (popped < objects_per_span) {
      size_t want =
          std::min<size_t>(objects_per_span - popped, kMaxObjectsToMove);
      size_t got =
          span.FreelistPopBatch(absl::MakeSpan(&all[popped], want), size);
      ASSERT_GT(got, 0);
      popped += got;
    }

    const size_t limit = size / sizeof(Span::ObjIdx) - 1;
    const size_t to_return = 12 + 1 + limit + 1;
    std::vector<Span::ObjIdx> idxs(to_return);
    Span::ObjectsToIdx(absl::MakeSpan(all.data(), to_return), &span, size,
                       reciprocal, idxs.data());
    EXPECT_TRUE(span.FreelistPushBatch(absl::MakeSpan(idxs), size));

    // Corrupt host2[0] (the next freelist_ head index loaded when host2 is
    // popped).
    auto* host2 = static_cast<Span::ObjIdx*>(all[12 + 1 + limit]);
    host2[0] = static_cast<Span::ObjIdx>(kPageSize >> kAlignmentShift);

    void* out[13];
    ASSERT_EQ(span.FreelistPopBatch(absl::MakeSpan(out, 13), size), 13);
    EXPECT_DEATH((void)span.FreelistPopBatch(absl::MakeSpan(out, 1), size),
                 "Corrupted freelist");
  }
}

INSTANTIATE_TEST_SUITE_P(All, SpanTest, testing::Range(size_t(1), kNumClasses));

TEST(SpanAllocatorTest, Alignment) {
  constexpr int kNumSpans = 1000;
  std::vector<Span*> spans;
  spans.reserve(kNumSpans);

  for (int i = 0; i < kNumSpans; ++i) {
    spans.push_back(Span::New(PageId{1}));
  }

  absl::flat_hash_map<uintptr_t, int> address_mod_cacheline;
  for (Span* s : spans) {
    ++address_mod_cacheline[reinterpret_cast<uintptr_t>(s) %
                            ABSL_CACHELINE_SIZE];
  }

  EXPECT_EQ(address_mod_cacheline[0], kNumSpans);

  // Verify alignof is respected.
  for (auto [alignment, count] : address_mod_cacheline) {
    EXPECT_EQ(alignment % alignof(Span), 0);
  }

  Span::Delete(absl::MakeSpan(spans));
}

}  // namespace
}  // namespace tcmalloc_internal
}  // namespace tcmalloc
