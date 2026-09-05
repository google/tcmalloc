// Copyright 2026 The TCMalloc Authors
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

#include "tcmalloc/static_forwarder.h"

#include <stdint.h>

#include <memory>
#include <utility>
#include <vector>

#include "benchmark/benchmark.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "absl/algorithm/container.h"
#include "absl/container/fixed_array.h"
#include "absl/random/random.h"
#include "absl/synchronization/mutex.h"
#include "absl/time/clock.h"
#include "absl/time/time.h"
#include "absl/types/span.h"
#include "tcmalloc/common.h"
#include "tcmalloc/internal/logging.h"
#include "tcmalloc/pages.h"
#include "tcmalloc/span.h"
#include "tcmalloc/static_vars.h"
#include "tcmalloc/stats.h"
#include "tcmalloc/testing/testutil.h"
#include "tcmalloc/testing/thread_manager.h"

namespace tcmalloc {
namespace tcmalloc_internal {
namespace {

using StaticForwarder = Forwarder;

class StaticForwarderTest : public testing::TestWithParam<size_t> {
 protected:
  size_t size_class_;
  size_t object_size_;
  Length pages_per_span_;
  size_t batch_size_;
  size_t objects_per_span_;
  uint32_t size_reciprocal_;

 private:
  void SetUp() override {
    size_class_ = GetParam();
    if (IsColdSizeClass(size_class_)) {
#if ABSL_HAVE_THREAD_SANITIZER || ABSL_HAVE_HWADDRESS_SANITIZER
      GTEST_SKIP() << "Skipping test under sanitizers that conflict with "
                      "address placement";
#endif

      if (!ColdFeatureActive()) {
        // If !ColdFeatureActive(), we will use the normal page heap, which will
        // keep us from seeing memory get the expected tags.
        GTEST_SKIP() << "Skipping cold size classes without cold experiment";
      }
    }
    object_size_ = tc_globals.sizemap().class_to_size(size_class_);
    if (object_size_ == 0) {
      GTEST_SKIP() << "Skipping empty size class.";
    }

    pages_per_span_ = tc_globals.sizemap().class_to_pages(size_class_);
    batch_size_ = tc_globals.sizemap().num_objects_to_move(size_class_);
    objects_per_span_ = pages_per_span_.in_bytes() / object_size_;
    size_reciprocal_ = Span::CalcReciprocal(object_size_);
  }
};

TEST_P(StaticForwarderTest, Simple) {
  Span* span = StaticForwarder::AllocateSpan(size_class_, objects_per_span_,
                                             pages_per_span_);
  ASSERT_NE(span, nullptr);

  absl::FixedArray<void*> batch(objects_per_span_);
  const uint64_t alloc_time = StaticForwarder::clock_now();
  size_t allocated = span->BuildFreelist(object_size_, objects_per_span_,
                                         absl::MakeSpan(batch), alloc_time);
  ASSERT_EQ(allocated, objects_per_span_);

  EXPECT_EQ(size_class_, tc_globals.pagemap().sizeclass(span->first_page()));
  EXPECT_EQ(size_class_, tc_globals.pagemap().sizeclass(span->last_page()));

  // span_test.cc provides test coverage for Span, but we need to obtain several
  // objects to confirm we can map back to the Span pointer from the PageMap.
  for (void* ptr : batch) {
    Span* got;
    StaticForwarder::MapObjectsToSpans({&ptr, 1}, &got, size_class_);
    EXPECT_EQ(span, got);
  }

  for (void* ptr : batch) {
    EXPECT_EQ(span->FreelistPushBatch(absl::MakeSpan(&ptr, 1), object_size_,
                                      size_reciprocal_),
              ptr != batch.back());
  }

  StaticForwarder::DeallocateSpans(objects_per_span_, absl::MakeSpan(&span, 1));
}

TEST(StaticForwarderDeathTest, MapObjectsToSpansErrors) {
  constexpr int size_class = 1;
  const size_t object_size = tc_globals.sizemap().class_to_size(size_class);
  const Length pages_per_span = tc_globals.sizemap().class_to_pages(size_class);
  const size_t objects_per_span = pages_per_span.in_bytes() / object_size;
  const size_t size_reciprocal = Span::CalcReciprocal(object_size);

  Span* span = StaticForwarder::AllocateSpan(size_class, objects_per_span,
                                             pages_per_span);
  ASSERT_NE(span, nullptr);

  absl::FixedArray<void*> batch(objects_per_span);
  const uint64_t alloc_time = StaticForwarder::clock_now();
  size_t allocated = span->BuildFreelist(object_size, objects_per_span,
                                         absl::MakeSpan(batch), alloc_time);
  ASSERT_EQ(allocated, objects_per_span);

  // Mismatched size class
  void* ptr = batch[0];
  Span* got = nullptr;
  EXPECT_DEATH(StaticForwarder::MapObjectsToSpans({&ptr, 1}, &got,
                                                  /*expected_size_class=*/2),
               "Mismatched-size-class");

  // Corrupted / unallocated pointer
  void* invalid_ptr = nullptr;
  EXPECT_DEATH(
      StaticForwarder::MapObjectsToSpans({&invalid_ptr, 1}, &got, size_class),
      "Attempted to free corrupted pointer");

  for (void* p : batch) {
    (void)span->FreelistPushBatch(absl::MakeSpan(&p, 1), object_size,
                                  size_reciprocal);
  }
  StaticForwarder::DeallocateSpans(objects_per_span, absl::MakeSpan(&span, 1));

  // Double free (after deallocation, span descriptor is invalid)
  EXPECT_DEATH(StaticForwarder::MapObjectsToSpans({&ptr, 1}, &got, size_class),
               "Possible double free detected|Mismatched-size-class");
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
      absl::MutexLock l(mu_);
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
                tc_globals.pagemap().sizeclass(data->span->first_page()));
      EXPECT_EQ(size_class_,
                tc_globals.pagemap().sizeclass(data->span->last_page()));
      // Confirm we can map at least one object back.
      Span* got;
      StaticForwarder::MapObjectsToSpans({&data->batch[0], 1}, &got,
                                         size_class_);
      EXPECT_EQ(data->span, got);

      free_spans.push_back(data->span);
    }

    auto span_span = absl::MakeSpan(free_spans);
    for (int i = 0; i < spans.size(); i += kMaxObjectsToMove) {
      StaticForwarder::DeallocateSpans(objects_per_span_,
                                       span_span.subspan(i, kMaxObjectsToMove));
    }
  }

  void Grow() {
    // Allocate a Span
    Span* span = StaticForwarder::AllocateSpan(size_class_, objects_per_span_,
                                               pages_per_span_);
    ASSERT_NE(span, nullptr);

    auto d = std::make_unique<SpanData>();
    d->span = span;

    size_t allocated = span->BuildFreelist(
        object_size_, objects_per_span_, absl::MakeSpan(d->batch, batch_size_),
        StaticForwarder::clock_now());
    EXPECT_LE(allocated, objects_per_span_);

    EXPECT_EQ(size_class_, tc_globals.pagemap().sizeclass(span->first_page()));
    EXPECT_EQ(size_class_, tc_globals.pagemap().sizeclass(span->last_page()));
    // Confirm we can map at least one object back.
    Span* got;
    StaticForwarder::MapObjectsToSpans({&d->batch[0], 1}, &got, size_class_);
    EXPECT_EQ(span, got);

    absl::MutexLock l(mu_);
    spans_allocated_++;
    data_.push_back(std::move(d));
  }

  void Shrink() {
    absl::BitGen rng;
    std::vector<std::unique_ptr<SpanData>> spans;

    {
      absl::MutexLock l(mu_);
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
                tc_globals.pagemap().sizeclass(data->span->first_page()));
      EXPECT_EQ(size_class_,
                tc_globals.pagemap().sizeclass(data->span->last_page()));
      // Confirm we can map at least one object back.
      Span* got;
      StaticForwarder::MapObjectsToSpans({&data->batch[0], 1}, &got,
                                         size_class_);
      EXPECT_EQ(data->span, got);

      free_spans.push_back(data->span);
    }

    auto span_span = absl::MakeSpan(free_spans);
    for (int i = 0; i < spans.size(); i += kMaxObjectsToMove) {
      StaticForwarder::DeallocateSpans(objects_per_span_,
                                       span_span.subspan(i, kMaxObjectsToMove));
    }
  }

  void Shuffle(absl::BitGen& rng) {
    // Shuffle the shared vector.
    absl::MutexLock l(mu_);
    absl::c_shuffle(data_, rng);
  }

  int64_t BytesAllocated() {
    absl::MutexLock l(mu_);
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
  PageHeapSpinLockHolder l;
  return tc_globals.page_allocator().stats();
}

TEST_P(StaticForwarderTest, Fuzz) {
#if ABSL_HAVE_THREAD_SANITIZER || ABSL_HAVE_HWADDRESS_SANITIZER
  // TODO(b/193887621):  Enable this test under TSan after addressing benign
  // true positives.
  GTEST_SKIP() << "Skipping test under Thread Sanitizer.";
#endif  // ABSL_HAVE_THREAD_SANITIZER || ABSL_HAVE_HWADDRESS_SANITIZER

  const auto page_heap_before = PageHeapStats();

  StaticForwarderEnvironment env(size_class_, object_size_, objects_per_span_,
                                 pages_per_span_, batch_size_);
  ThreadManager threads;
  threads.Start(10, [&](int) { env.RandomlyPoke(); });

  absl::SleepFor(absl::Milliseconds(50));

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

TEST(StaticForwarderTest, BasicState) {
  Forwarder forwarder;
  EXPECT_GT(forwarder.clock_now(), 0);
  EXPECT_GT(forwarder.clock_frequency(), 0);
  EXPECT_GE(forwarder.active_partitions(), 1);
  EXPECT_NE(&forwarder.arena(), nullptr);
  EXPECT_GT(forwarder.BackSizeThresholdBytes(), 0);
}

TEST(StaticForwarderTest, ShardedForwarder) {
  ShardedForwarder::Init();
  EXPECT_EQ(ShardedForwarder::UseGenericCache(),
            IsExperimentActive(Experiment::TCMALLOC_SHARDED_TC_ABLATION) &&
                !IsExperimentActive(
                    Experiment::TEST_ONLY_TCMALLOC_SHARDED_TRANSFER_CACHE));
  EXPECT_EQ(ShardedForwarder::EnableCacheForLargeClassesOnly(),
            IsExperimentActive(
                Experiment::TEST_ONLY_TCMALLOC_SHARDED_TRANSFER_CACHE));
}

}  // namespace
}  // namespace tcmalloc_internal
}  // namespace tcmalloc
