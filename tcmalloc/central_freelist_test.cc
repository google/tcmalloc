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

#include "tcmalloc/central_freelist.h"

#include <algorithm>
#include <utility>

#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "absl/container/fixed_array.h"
#include "absl/random/random.h"
#include "tcmalloc/common.h"
#include "tcmalloc/mock_static_forwarder.h"
#include "tcmalloc/pagemap.h"
#include "tcmalloc/static_vars.h"
#include "tcmalloc/testing/thread_manager.h"

namespace tcmalloc {
namespace tcmalloc_internal {

namespace central_freelist_internal {

class StaticForwarderTest : public testing::TestWithParam<size_t> {
 protected:
  size_t size_class_;
  size_t object_size_;
  Length pages_per_span_;
  size_t batch_size_;
  size_t objects_per_span_;

 private:
  void SetUp() override {
    size_class_ = GetParam();
    if (IsExpandedSizeClass(size_class_)) {
#if ABSL_HAVE_THREAD_SANITIZER
      GTEST_SKIP() << "Skipping test under sanitizers that conflict with "
                      "address placement";
#endif

      if (!ColdExperimentActive()) {
        // If !ColdExperimentActive(), we will use the normal page heap, which
        // will keep us from seeing memory get the expected tags.
        GTEST_SKIP()
            << "Skipping expanded size classes without cold experiment";
      }
    }
    object_size_ = Static::sizemap().class_to_size(size_class_);
    if (object_size_ == 0) {
      GTEST_SKIP() << "Skipping empty size class.";
    }

    pages_per_span_ = Length(Static::sizemap().class_to_pages(size_class_));
    batch_size_ = Static::sizemap().num_objects_to_move(size_class_);
    objects_per_span_ = pages_per_span_.in_bytes() / object_size_;
  }
};

TEST_P(StaticForwarderTest, Simple) {
  Span* span = StaticForwarder::AllocateSpan(size_class_, pages_per_span_);
  ASSERT_NE(span, nullptr);

  absl::FixedArray<void*> batch(objects_per_span_);
  size_t allocated = span->BuildFreelist(object_size_, objects_per_span_,
                                         &batch[0], objects_per_span_);
  ASSERT_EQ(allocated, objects_per_span_);

  EXPECT_EQ(size_class_, Static::pagemap().sizeclass(span->first_page()));
  EXPECT_EQ(size_class_, Static::pagemap().sizeclass(span->last_page()));

  // span_test.cc provides test coverage for Span, but we need to obtain several
  // objects to confirm we can map back to the Span pointer from the PageMap.
  for (void* ptr : batch) {
    EXPECT_EQ(span, StaticForwarder::MapObjectToSpan(ptr));
  }

  for (void* ptr : batch) {
    span->FreelistPush(ptr, object_size_);
  }

  StaticForwarder::DeallocateSpans(size_class_, absl::MakeSpan(&span, 1));
}

class StaticForwarderEnvironment {
  struct SpanData {
    Span* span;
    void* batch[kMaxObjectsToMove];
  };

 public:
  StaticForwarderEnvironment(int size_class, size_t object_size,
                             size_t objects_per_span, Length pages_per_span,
                             int batch_size)
      : size_class_(size_class),
        object_size_(object_size),
        objects_per_span_(objects_per_span),
        pages_per_span_(pages_per_span),
        batch_size_(batch_size) {}

  ~StaticForwarderEnvironment() { Drain(); }

  void RandomlyPoke() {
    absl::BitGen rng;
    double coin = absl::Uniform(rng, 0.0, 1.0);

    if (coin < 0.5) {
      Grow();
    } else if (coin < 0.9) {
      // Deallocate Spans.  We may deallocate more than 1 span, so we bias
      // towards allocating Spans more often than we deallocate.
      Shrink();
    } else {
      Shuffle(rng);
    }
  }

  void Drain() {
    std::vector<std::unique_ptr<SpanData>> spans;

    {
      absl::MutexLock l(&mu_);
      if (data_.empty()) {
        return;
      }

      spans = std::move(data_);
      data_.clear();
    }

    // Check mappings.
    std::vector<Span*> free_spans;
    for (const auto& data : spans) {
      EXPECT_EQ(size_class_,
                Static::pagemap().sizeclass(data->span->first_page()));
      EXPECT_EQ(size_class_,
                Static::pagemap().sizeclass(data->span->last_page()));
      // Confirm we can map at least one object back.
      EXPECT_EQ(data->span, StaticForwarder::MapObjectToSpan(data->batch[0]));

      free_spans.push_back(data->span);
    }

    StaticForwarder::DeallocateSpans(size_class_, absl::MakeSpan(free_spans));
  }

  void Grow() {
    // Allocate a Span
    Span* span = StaticForwarder::AllocateSpan(size_class_, pages_per_span_);
    ASSERT_NE(span, nullptr);

    auto d = absl::make_unique<SpanData>();
    d->span = span;

    size_t allocated = span->BuildFreelist(object_size_, objects_per_span_,
                                           d->batch, batch_size_);
    EXPECT_LE(allocated, objects_per_span_);

    EXPECT_EQ(size_class_, Static::pagemap().sizeclass(span->first_page()));
    EXPECT_EQ(size_class_, Static::pagemap().sizeclass(span->last_page()));
    // Confirm we can map at least one object back.
    EXPECT_EQ(span, StaticForwarder::MapObjectToSpan(d->batch[0]));

    absl::MutexLock l(&mu_);
    spans_allocated_++;
    data_.push_back(std::move(d));
  }

  void Shrink() {
    absl::BitGen rng;
    std::vector<std::unique_ptr<SpanData>> spans;

    {
      absl::MutexLock l(&mu_);
      if (data_.empty()) {
        return;
      }

      size_t count = absl::LogUniform<size_t>(rng, 1, data_.size());
      spans.reserve(count);

      for (int i = 0; i < count; i++) {
        spans.push_back(std::move(data_.back()));
        data_.pop_back();
      }
    }

    // Check mappings.
    std::vector<Span*> free_spans;
    for (auto& data : spans) {
      EXPECT_EQ(size_class_,
                Static::pagemap().sizeclass(data->span->first_page()));
      EXPECT_EQ(size_class_,
                Static::pagemap().sizeclass(data->span->last_page()));
      // Confirm we can map at least one object back.
      EXPECT_EQ(data->span, StaticForwarder::MapObjectToSpan(data->batch[0]));

      free_spans.push_back(data->span);
    }

    StaticForwarder::DeallocateSpans(size_class_, absl::MakeSpan(free_spans));
  }

  void Shuffle(absl::BitGen& rng) {
    // Shuffle the shared vector.
    absl::MutexLock l(&mu_);
    absl::c_shuffle(data_, rng);
  }

  int64_t BytesAllocated() {
    absl::MutexLock l(&mu_);
    return pages_per_span_.in_bytes() * spans_allocated_;
  }

 private:
  int size_class_;
  size_t object_size_;
  size_t objects_per_span_;
  Length pages_per_span_;
  int batch_size_;

  absl::Mutex mu_;
  int64_t spans_allocated_ ABSL_GUARDED_BY(mu_) = 0;
  std::vector<std::unique_ptr<SpanData>> data_ ABSL_GUARDED_BY(mu_);
};

static BackingStats PageHeapStats() {
  absl::base_internal::SpinLockHolder l(&pageheap_lock);
  return Static::page_allocator().stats();
}

TEST_P(StaticForwarderTest, Fuzz) {
#if ABSL_HAVE_THREAD_SANITIZER
  // TODO(b/193887621):  Enable this test under TSan after addressing benign
  // true positives.
  GTEST_SKIP() << "Skipping test under Thread Sanitizer.";
#endif  // ABSL_HAVE_THREAD_SANITIZER

  const auto page_heap_before = PageHeapStats();

  StaticForwarderEnvironment env(size_class_, object_size_, objects_per_span_,
                                 pages_per_span_, batch_size_);
  ThreadManager threads;
  threads.Start(10, [&](int) { env.RandomlyPoke(); });

  absl::SleepFor(absl::Seconds(0.2));

  threads.Stop();

  const auto page_heap_after = PageHeapStats();
  // Confirm we did not leak Spans by ensuring the page heap did not grow nearly
  // 1:1 by the total number of Spans we ever allocated.
  //
  // Since we expect to allocate a significant number of spans, we apply a
  // factor of 1/2 (which is unlikely to be flaky) to avoid false negatives
  // if/when a background thread triggers a deallocation.
  const int64_t bytes_allocated = env.BytesAllocated();
  EXPECT_GT(bytes_allocated, 0);
  EXPECT_LE(static_cast<int64_t>(page_heap_after.system_bytes) -
                static_cast<int64_t>(page_heap_before.system_bytes),
            bytes_allocated / 2);
}

INSTANTIATE_TEST_SUITE_P(All, StaticForwarderTest,
                         testing::Range(size_t(1), kNumClasses));

}  // namespace central_freelist_internal

namespace {

template <typename Env>
using CentralFreeListTest = ::testing::Test;
TYPED_TEST_SUITE_P(CentralFreeListTest);

TYPED_TEST_P(CentralFreeListTest, IsolatedSmoke) {
  TypeParam e;

  EXPECT_CALL(e.forwarder(), AllocateSpan).Times(1);

  absl::FixedArray<void*> batch(TypeParam::kBatchSize);
  int allocated =
      e.central_freelist().RemoveRange(&batch[0], TypeParam::kBatchSize);
  ASSERT_GT(allocated, 0);
  EXPECT_LE(allocated, TypeParam::kBatchSize);

  // We should observe span's utilization captured in the histogram. The number
  // of spans in rest of the buckets should be zero.
  const int allocated_bw = absl::bit_width(static_cast<unsigned>(allocated));
  for (int i = 1; i <= absl::bit_width(TypeParam::kObjectsPerSpan); ++i) {
    if (i == allocated_bw) {
      EXPECT_EQ(e.central_freelist().NumSpansWith(i), 1);
    } else {
      EXPECT_EQ(e.central_freelist().NumSpansWith(i), 0);
    }
  }

  EXPECT_CALL(e.forwarder(), MapObjectToSpan).Times(allocated);
  EXPECT_CALL(e.forwarder(), DeallocateSpans).Times(1);

  SpanStats stats = e.central_freelist().GetSpanStats();
  EXPECT_EQ(stats.num_spans_requested, 1);
  EXPECT_EQ(stats.num_spans_returned, 0);
  EXPECT_EQ(stats.obj_capacity, 1024);

  e.central_freelist().InsertRange(absl::MakeSpan(&batch[0], allocated));

  stats = e.central_freelist().GetSpanStats();
  EXPECT_EQ(stats.num_spans_requested, 1);
  EXPECT_EQ(stats.num_spans_returned, 1);
  EXPECT_EQ(stats.obj_capacity, 0);

  // Span captured in the histogram with the earlier utilization should have
  // been removed.
  for (int i = 1; i < absl::bit_width(TypeParam::kObjectsPerSpan); ++i) {
    EXPECT_EQ(e.central_freelist().NumSpansWith(i), 0);
  }
}

TYPED_TEST_P(CentralFreeListTest, SpanUtilizationHistogram) {
  TypeParam e;

  constexpr size_t kNumSpans = 10;

  // Request kNumSpans spans.
  void* batch[kMaxObjectsToMove];
  const int num_objects_to_fetch = kNumSpans * TypeParam::kObjectsPerSpan;
  int total_fetched = 0;
  // Tracks object and corresponding span idx from which it was allocated.
  std::vector<std::pair<void*, int>> objects_to_span_idx;
  // Tracks number of objects allocated per span.
  std::vector<size_t> allocated_per_span(kNumSpans, 0);
  int span_idx = 0;

  while (total_fetched < num_objects_to_fetch) {
    size_t n = num_objects_to_fetch - total_fetched;
    int got = e.central_freelist().RemoveRange(
        batch, std::min(n, TypeParam::kBatchSize));
    total_fetched += got;

    // Increment span_idx if current objects have been fetched from the new
    // span.
    if (total_fetched > (span_idx + 1) * TypeParam::kObjectsPerSpan) {
      ++span_idx;
    }
    // Record fetched object and associated span index.
    for (int i = 0; i < got; ++i) {
      objects_to_span_idx.push_back(std::make_pair(batch[i], span_idx));
    }
    ASSERT(span_idx < kNumSpans);
    allocated_per_span[span_idx] += got;
  }

  // Make sure that we have fetched exactly from kNumSpans spans.
  EXPECT_EQ(span_idx + 1, kNumSpans);

  // We should have kNumSpans spans in the histogram with number of allocated
  // objects equal to TypeParam::kObjectsPerSpan (i.e. in the last bucket).
  // Rest of the buckets should be empty.
  const int expected_bw = absl::bit_width(TypeParam::kObjectsPerSpan);
  EXPECT_EQ(e.central_freelist().NumSpansWith(expected_bw), kNumSpans);
  for (int i = 1; i < expected_bw; ++i) {
    EXPECT_EQ(e.central_freelist().NumSpansWith(i), 0);
  }

  // Shuffle.
  absl::BitGen rng;
  std::shuffle(objects_to_span_idx.begin(), objects_to_span_idx.end(), rng);

  // Return objects, a fraction at a time, each time checking that histogram is
  // correct.
  int total_returned = 0;
  const int last_bucket = absl::bit_width(TypeParam::kObjectsPerSpan) - 1;
  while (total_returned < num_objects_to_fetch) {
    uint64_t size_to_pop = std::min(objects_to_span_idx.size() - total_returned,
                                    TypeParam::kBatchSize);

    for (int i = 0; i < size_to_pop; ++i) {
      const auto [ptr, span_idx] = objects_to_span_idx[i + total_returned];
      batch[i] = ptr;
      ASSERT(span_idx < kNumSpans);
      --allocated_per_span[span_idx];
    }
    total_returned += size_to_pop;
    e.central_freelist().InsertRange({batch, size_to_pop});

    // Calculate expected histogram.
    size_t expected[absl::bit_width(TypeParam::kObjectsPerSpan)] = {0};
    for (int i = 0; i < kNumSpans; ++i) {
      // If span has non-zero allocated objects, include it in the histogram.
      if (allocated_per_span[i]) {
        const size_t bucket = absl::bit_width(allocated_per_span[i]) - 1;
        ASSERT(bucket <= last_bucket);
        ++expected[bucket];
      }
    }

    // Fetch number of spans logged in the histogram and compare it with the
    // expected histogram that we calculated using the tracked allocated
    // objects per span.
    for (int i = 1; i <= last_bucket; ++i) {
      EXPECT_EQ(e.central_freelist().NumSpansWith(i), expected[i - 1]);
    }
  }

  // Since no span is live here, histogram must be empty.
  for (int i = 1; i <= last_bucket; ++i) {
    EXPECT_EQ(e.central_freelist().NumSpansWith(i), 0);
  }
}

TYPED_TEST_P(CentralFreeListTest, MultipleSpans) {
  TypeParam e;
  std::vector<void*> all_objects;

  constexpr size_t kNumSpans = 10;

  // Request kNumSpans spans.
  void* batch[kMaxObjectsToMove];
  const int num_objects_to_fetch = kNumSpans * TypeParam::kObjectsPerSpan;
  int total_fetched = 0;
  while (total_fetched < num_objects_to_fetch) {
    size_t n = num_objects_to_fetch - total_fetched;
    int got = e.central_freelist().RemoveRange(
        batch, std::min(n, TypeParam::kBatchSize));
    for (int i = 0; i < got; ++i) {
      all_objects.push_back(batch[i]);
    }
    total_fetched += got;
  }

  // We should have kNumSpans spans in the histogram with number of
  // allocated objects equal to TypeParam::kObjectsPerSpan (i.e. in the last
  // bucket). Rest of the buckets should be empty.
  const int expected_bw = absl::bit_width(TypeParam::kObjectsPerSpan);
  EXPECT_EQ(e.central_freelist().NumSpansWith(expected_bw), kNumSpans);
  for (int i = 1; i < expected_bw; ++i) {
    EXPECT_EQ(e.central_freelist().NumSpansWith(i), 0);
  }

  SpanStats stats = e.central_freelist().GetSpanStats();
  EXPECT_EQ(stats.num_spans_requested, kNumSpans);
  EXPECT_EQ(stats.num_spans_returned, 0);

  EXPECT_EQ(all_objects.size(), num_objects_to_fetch);

  // Shuffle
  absl::BitGen rng;
  std::shuffle(all_objects.begin(), all_objects.end(), rng);

  // Return all
  int total_returned = 0;
  bool checked_half = false;
  while (total_returned < num_objects_to_fetch) {
    uint64_t size_to_pop =
        std::min(all_objects.size() - total_returned, TypeParam::kBatchSize);
    for (int i = 0; i < size_to_pop; ++i) {
      batch[i] = all_objects[i + total_returned];
    }
    total_returned += size_to_pop;
    e.central_freelist().InsertRange({batch, size_to_pop});
    // sanity check
    if (!checked_half && total_returned >= (num_objects_to_fetch / 2)) {
      stats = e.central_freelist().GetSpanStats();
      EXPECT_GT(stats.num_spans_requested, stats.num_spans_returned);
      EXPECT_NE(stats.obj_capacity, 0);
      // Total spans recorded in the histogram must be equal to the number of
      // live spans.
      size_t spans_in_histogram = 0;
      for (int i = 1; i <= absl::bit_width(TypeParam::kObjectsPerSpan); ++i) {
        spans_in_histogram += e.central_freelist().NumSpansWith(i);
      }
      EXPECT_EQ(spans_in_histogram, stats.num_live_spans());
      checked_half = true;
    }
  }

  stats = e.central_freelist().GetSpanStats();
  EXPECT_EQ(stats.num_spans_requested, stats.num_spans_returned);
  // Since no span is live, histogram must be empty.
  for (int i = 1; i <= absl::bit_width(TypeParam::kObjectsPerSpan); ++i) {
    EXPECT_EQ(e.central_freelist().NumSpansWith(i), 0);
  }
  EXPECT_EQ(stats.obj_capacity, 0);
}

REGISTER_TYPED_TEST_SUITE_P(CentralFreeListTest, IsolatedSmoke,
                            SpanUtilizationHistogram, MultipleSpans);

namespace unit_tests {

using Env = FakeCentralFreeListEnvironment<
    central_freelist_internal::CentralFreeList<MockStaticForwarder>>;

INSTANTIATE_TYPED_TEST_SUITE_P(CentralFreeList, CentralFreeListTest,
                               ::testing::Types<Env>);

}  // namespace unit_tests

}  // namespace
}  // namespace tcmalloc_internal
}  // namespace tcmalloc
