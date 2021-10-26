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

#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "absl/random/random.h"
#include "tcmalloc/common.h"
#include "tcmalloc/static_vars.h"

namespace tcmalloc {
namespace tcmalloc_internal {
namespace {

// TODO(b/162552708) Mock out the page heap to interact with CFL instead
class CFLTest : public testing::TestWithParam<size_t> {
 protected:
  size_t cl_;
  size_t batch_size_;
  size_t objects_per_span_;
  CentralFreeList cfl_;

 private:
  void SetUp() override {
    cl_ = GetParam();
    if (IsExpandedSizeClass(cl_)) {
      if (!ColdExperimentActive()) {
        // If !ColdExperimentActive(), we will use the normal page heap, which
        // will keep us from seeing memory get the expected tags.
        GTEST_SKIP()
            << "Skipping expanded size classes without cold experiment";
      }

#ifdef THREAD_SANITIZER
      GTEST_SKIP() << "Skipping expanded size class under tsan";
#endif
    }
    size_t object_size = Static::sizemap().class_to_size(cl_);
    if (object_size == 0) {
      GTEST_SKIP() << "Skipping empty size class.";
    }

    auto pages_per_span = Length(Static::sizemap().class_to_pages(cl_));
    batch_size_ = Static::sizemap().num_objects_to_move(cl_);
    objects_per_span_ = pages_per_span.in_bytes() / object_size;
    cfl_.Init(cl_);
  }

  void TearDown() override { EXPECT_EQ(cfl_.length(), 0); }
};

TEST_P(CFLTest, SingleBatch) {
  void* batch[kMaxObjectsToMove];
  uint64_t got = cfl_.RemoveRange(batch, batch_size_);
  ASSERT_GT(got, 0);

  // We should observe span's utilization captured in the histogram. The
  // number of spans in rest of the buckets should be zero.
  int bucket = absl::bit_width(got);
  SpanUtilHistogram histogram = cfl_.GetSpanUtilHistogram();
  for (int i = 0; i < kSpanUtilBucketCapacity; ++i) {
    if (i == bucket) {
      EXPECT_EQ(histogram.value[i], 1);
    } else {
      EXPECT_EQ(histogram.value[i], 0);
    }
  }

  cfl_.InsertRange({batch, got});
  SpanStats stats = cfl_.GetSpanStats();
  EXPECT_EQ(stats.num_spans_requested, 1);
  EXPECT_EQ(stats.num_spans_returned, 1);
  EXPECT_EQ(stats.obj_capacity, 0);

  // Span captured in the histogram with the earlier utilization should have
  // been removed.
  histogram = cfl_.GetSpanUtilHistogram();
  for (int i = 0; i < kSpanUtilBucketCapacity; ++i) {
    EXPECT_EQ(histogram.value[i], 0);
  }
}

TEST_P(CFLTest, SpanUtilizationHistogram) {
  const size_t num_spans = 10;

  // Request num_spans spans.
  void* batch[kMaxObjectsToMove];
  const int num_objects_to_fetch = num_spans * objects_per_span_;
  int total_fetched = 0;
  // Tracks object and corresponding span idx from which it was allocated.
  std::vector<std::pair<void*, int>> objects_to_span_idx;
  // Tracks number of objects allocated per span.
  std::vector<size_t> allocated_per_span(num_spans, 0);
  int span_idx = 0;

  while (total_fetched < num_objects_to_fetch) {
    size_t n = num_objects_to_fetch - total_fetched;
    int got = cfl_.RemoveRange(batch, std::min(n, batch_size_));
    total_fetched += got;

    // Increment span_idx if current objects have been fetched from the new
    // span.
    if (total_fetched > (span_idx + 1) * objects_per_span_) {
      ++span_idx;
    }
    // Record fetched object and associated span index.
    for (int i = 0; i < got; ++i) {
      objects_to_span_idx.push_back(std::make_pair(batch[i], span_idx));
    }
    ASSERT(span_idx < num_spans);
    allocated_per_span[span_idx] += got;
  }

  // Make sure that we have fetched exactly from num_spans spans.
  EXPECT_EQ(span_idx + 1, num_spans);

  // We should have num_spans spans in the histogram with number of allocated
  // objects equal to objects_per_span_ (i.e. in the last bucket).
  // Rest of the buckets should be empty.
  SpanUtilHistogram histogram = cfl_.GetSpanUtilHistogram();
  int last_bucket = absl::bit_width(objects_per_span_);
  ASSERT(last_bucket < kSpanUtilBucketCapacity);
  for (int i = 0; i < kSpanUtilBucketCapacity; ++i) {
    if (i == last_bucket) {
      EXPECT_EQ(histogram.value[last_bucket], num_spans);
    } else {
      EXPECT_EQ(histogram.value[i], 0);
    }
  }

  // Shuffle.
  absl::BitGen rng;
  std::shuffle(objects_to_span_idx.begin(), objects_to_span_idx.end(), rng);

  // Return objects, a fraction at a time, each time checking that histogram is
  // correct.
  int total_returned = 0;
  while (total_returned < num_objects_to_fetch) {
    uint64_t size_to_pop =
        std::min(objects_to_span_idx.size() - total_returned, batch_size_);

    for (int i = 0; i < size_to_pop; ++i) {
      const auto [ptr, span_idx] = objects_to_span_idx[i + total_returned];
      batch[i] = ptr;
      ASSERT(span_idx < num_spans);
      --allocated_per_span[span_idx];
    }
    total_returned += size_to_pop;
    cfl_.InsertRange({batch, size_to_pop});

    // Calculate expected histogram.
    SpanUtilHistogram expected;
    for (int i = 0; i < num_spans; ++i) {
      size_t allocated = absl::bit_width(allocated_per_span[i]);
      // If span has non-zero allocated objects, include it in the histogram.
      if (allocated) {
        ASSERT(allocated <= absl::bit_width(objects_per_span_));
        ++expected.value[allocated];
      }
    }

    // Fetch histogram and compare it with the expected histogram that we
    // calculated using the tracked allocated objects per span.
    SpanUtilHistogram histogram = cfl_.GetSpanUtilHistogram();
    for (int i = 0; i < kSpanUtilBucketCapacity; i++) {
      EXPECT_EQ(histogram.value[i], expected.value[i]);
    }
  }

  // Since no span is live here, histogram must be empty.
  histogram = cfl_.GetSpanUtilHistogram();
  for (int i = 0; i < kSpanUtilBucketCapacity; ++i) {
    EXPECT_EQ(histogram.value[i], 0);
  }
}

TEST_P(CFLTest, MultipleSpans) {
  std::vector<void*> all_objects;

  const size_t num_spans = 10;

  // Request num_spans spans.
  void* batch[kMaxObjectsToMove];
  const int num_objects_to_fetch = num_spans * objects_per_span_;
  int total_fetched = 0;
  while (total_fetched < num_objects_to_fetch) {
    size_t n = num_objects_to_fetch - total_fetched;
    int got = cfl_.RemoveRange(batch, std::min(n, batch_size_));
    for (int i = 0; i < got; ++i) {
      all_objects.push_back(batch[i]);
    }
    total_fetched += got;
  }

  // We should have num_spans spans in the histogram with number of
  // allocated objects equal to objects_per_span_ (i.e. in the last bucket).
  // Rest of the buckets should be empty.
  SpanUtilHistogram histogram = cfl_.GetSpanUtilHistogram();
  int last_bucket = absl::bit_width(objects_per_span_);
  ASSERT(last_bucket < kSpanUtilBucketCapacity);
  for (int i = 0; i < kSpanUtilBucketCapacity; ++i) {
    if (i == last_bucket) {
      EXPECT_EQ(histogram.value[last_bucket], num_spans);
    } else {
      EXPECT_EQ(histogram.value[i], 0);
    }
  }

  SpanStats stats = cfl_.GetSpanStats();
  EXPECT_EQ(stats.num_spans_requested, num_spans);
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
        std::min(all_objects.size() - total_returned, batch_size_);
    for (int i = 0; i < size_to_pop; ++i) {
      batch[i] = all_objects[i + total_returned];
    }
    total_returned += size_to_pop;
    cfl_.InsertRange({batch, size_to_pop});
    // sanity check
    if (!checked_half && total_returned >= (num_objects_to_fetch / 2)) {
      stats = cfl_.GetSpanStats();
      EXPECT_GT(stats.num_spans_requested, stats.num_spans_returned);
      EXPECT_NE(stats.obj_capacity, 0);
      // Total spans recorded in the histogram must be equal to the number of
      // live spans.
      histogram = cfl_.GetSpanUtilHistogram();
      size_t spans_in_histogram = 0;
      for (int i = 0; i < kSpanUtilBucketCapacity; ++i) {
        spans_in_histogram += histogram.value[i];
      }
      EXPECT_EQ(spans_in_histogram, stats.num_live_spans());
      checked_half = true;
    }
  }

  stats = cfl_.GetSpanStats();
  EXPECT_EQ(stats.num_spans_requested, stats.num_spans_returned);
  // Since no span is live, histogram must be empty.
  histogram = cfl_.GetSpanUtilHistogram();
  for (int i = 0; i < kSpanUtilBucketCapacity; ++i) {
    EXPECT_EQ(histogram.value[i], 0);
  }
  EXPECT_EQ(stats.obj_capacity, 0);
}

INSTANTIATE_TEST_SUITE_P(All, CFLTest, testing::Range(size_t(1), kNumClasses));
}  // namespace
}  // namespace tcmalloc_internal
}  // namespace tcmalloc
