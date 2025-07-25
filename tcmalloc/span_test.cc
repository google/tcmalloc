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

#include <new>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "absl/base/internal/spinlock.h"
#include "absl/base/optimization.h"
#include "absl/container/flat_hash_map.h"
#include "absl/container/flat_hash_set.h"
#include "absl/random/random.h"
#include "absl/types/span.h"
#include "tcmalloc/common.h"
#include "tcmalloc/experiment.h"
#include "tcmalloc/experiment_config.h"
#include "tcmalloc/internal/logging.h"
#include "tcmalloc/pages.h"
#include "tcmalloc/static_vars.h"

namespace tcmalloc {
namespace tcmalloc_internal {
namespace {

constexpr uint64_t kSpanAllocTime = 1234;

class RawSpan {
 public:
  void Init(size_t size_class) {
    size_t size = tc_globals.sizemap().class_to_size(size_class);
    auto npages = Length(tc_globals.sizemap().class_to_pages(size_class));
    size_t objects_per_span = npages.in_bytes() / size;

    // Dynamically allocate so ASan can flag if we run out of bounds.
    buf_ = ::operator new(sizeof(Span), std::align_val_t(alignof(Span)));

    int res = posix_memalign(&mem_, kPageSize, npages.in_bytes());
    TC_CHECK_EQ(res, 0);

    span_ = new (buf_) Span(Range(PageIdContaining(mem_), npages));
    TC_CHECK_EQ(
        span_->BuildFreelist(size, objects_per_span, {}, kSpanAllocTime), 0);
  }

  ~RawSpan() {
    free(mem_);
    ::operator delete(buf_, std::align_val_t(alignof(Span)));
  }

  Span& span() { return *span_; }

 private:
  void* buf_ = nullptr;
  void* mem_ = nullptr;
  Span* span_;
};

class SpanTest : public testing::TestWithParam<size_t> {
 protected:
  size_t size_class_;
  size_t size_;
  size_t npages_;
  size_t batch_size_;
  size_t objects_per_span_;
  uint32_t reciprocal_;
  RawSpan raw_span_;

 private:
  void SetUp() override {
    size_class_ = GetParam();
    size_ = tc_globals.sizemap().class_to_size(size_class_);
    if (size_ == 0) {
      GTEST_SKIP() << "Skipping empty size class.";
    }

    npages_ = tc_globals.sizemap().class_to_pages(size_class_);
    batch_size_ = tc_globals.sizemap().num_objects_to_move(size_class_);
    objects_per_span_ = npages_ * kPageSize / size_;
    reciprocal_ = Span::CalcReciprocal(size_);

    raw_span_.Init(size_class_);
  }

  void TearDown() override {}
};

TEST_P(SpanTest, FreelistBasic) {
  Span& span_ = raw_span_.span();

  EXPECT_FALSE(span_.FreelistEmpty(size_));
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
      EXPECT_NEAR(
          span_.Fragmentation(size_),
          static_cast<double>(objects_per_span_) / static_cast<double>(popped) -
              1.,
          1e-5);
      EXPECT_EQ(span_.FreelistEmpty(size_), popped == objects_per_span_);
      for (size_t i = 0; i < n; ++i) {
        void* p = batch[i];
        uintptr_t off = reinterpret_cast<char*>(p) - start;
        EXPECT_LT(off, span_.bytes_in_span());
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
    EXPECT_TRUE(span_.FreelistEmpty(size_));
    EXPECT_EQ(span_.FreelistPopBatch(absl::MakeSpan(batch, 1), size_), 0);
    EXPECT_EQ(popped, objects_per_span_);

    // Push all objects back except the last one (which would not be pushed).
    for (size_t idx = 0; idx < objects_per_span_ - 1; ++idx) {
      EXPECT_TRUE(objects[idx]);
      void* ptr = start + idx * size_;
      bool ok = span_.FreelistPushBatch({&ptr, 1}, size_, reciprocal_);
      EXPECT_TRUE(ok);
      EXPECT_FALSE(span_.FreelistEmpty(size_));
      objects[idx] = false;
      --popped;
    }
    // On the last iteration we can actually push the last object.
    if (x == 1) {
      void* ptr = start + (objects_per_span_ - 1) * size_;
      bool ok = span_.FreelistPushBatch({&ptr, 1}, size_, reciprocal_);
      EXPECT_FALSE(ok);
    }
  }
}

TEST_P(SpanTest, AllocTime) {
  Span& span_ = raw_span_.span();
  EXPECT_EQ(span_.AllocTime(size_), kSpanAllocTime);
}

TEST_P(SpanTest, FreelistRandomized) {
  Span& span_ = raw_span_.span();

  char* start = static_cast<char*>(span_.start_address());

  // Do a bunch of random pushes/pops with random batch size.
  absl::BitGen rng;
  absl::flat_hash_set<void*> objects;
  void* batch[kMaxObjectsToMove];
  for (size_t x = 0; x < 10000; ++x) {
    if (!objects.empty() && absl::Bernoulli(rng, 1.0 / 2)) {
      void* p = *objects.begin();
      if (span_.FreelistPushBatch({&p, 1}, size_, reciprocal_)) {
        objects.erase(objects.begin());
      } else {
        EXPECT_EQ(objects.size(), 1);
      }
      EXPECT_EQ(span_.FreelistEmpty(size_), objects_per_span_ == 1);
    } else {
      size_t want = absl::Uniform<int32_t>(rng, 0, batch_size_) + 1;
      size_t n = span_.FreelistPopBatch(absl::MakeSpan(batch, want), size_);
      if (n < want) {
        EXPECT_TRUE(span_.FreelistEmpty(size_));
      }
      for (size_t i = 0; i < n; ++i) {
        EXPECT_TRUE(objects.insert(batch[i]).second);
      }
    }
  }

  EXPECT_TRUE(span_.AllocTime(size_) == kSpanAllocTime);
  // Now pop everything what's there.
  for (;;) {
    size_t n =
        span_.FreelistPopBatch(absl::MakeSpan(batch, batch_size_), size_);
    for (size_t i = 0; i < n; ++i) {
      EXPECT_TRUE(objects.insert(batch[i]).second);
    }
    if (n < batch_size_) {
      break;
    }
  }
  // Check that we have collected all objects.
  EXPECT_EQ(objects.size(), objects_per_span_);
  for (void* p : objects) {
    uintptr_t off = reinterpret_cast<char*>(p) - start;
    EXPECT_LT(off, span_.bytes_in_span());
    EXPECT_EQ(off % size_, 0);
  }
}

INSTANTIATE_TEST_SUITE_P(All, SpanTest, testing::Range(size_t(1), kNumClasses));

TEST(SpanAllocatorTest, Alignment) {
  Range r(PageId{1}, Length{2});

  constexpr int kNumSpans = 1000;
  std::vector<Span*> spans;
  spans.reserve(kNumSpans);

  {
#ifdef TCMALLOC_INTERNAL_LEGACY_LOCKING
    PageHeapSpinLockHolder l;
#endif  // TCMALLOC_INTERNAL_LEGACY_LOCKING
    for (int i = 0; i < kNumSpans; ++i) {
      spans.push_back(Span::New(r));
    }
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

  {
#ifdef TCMALLOC_INTERNAL_LEGACY_LOCKING
    PageHeapSpinLockHolder l;
#endif  // TCMALLOC_INTERNAL_LEGACY_LOCKING
    for (Span* s : spans) {
      Span::Delete(s);
    }
  }
}

}  // namespace
}  // namespace tcmalloc_internal
}  // namespace tcmalloc
