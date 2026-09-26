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

#include <stddef.h>
#include <stdint.h>

#include <memory>
#include <new>
#include <utility>
#include <vector>

#include "gtest/gtest.h"
#include "absl/algorithm/container.h"
#include "absl/base/attributes.h"
#include "absl/base/thread_annotations.h"
#include "absl/container/fixed_array.h"
#include "absl/random/random.h"
#include "absl/synchronization/mutex.h"
#include "absl/time/clock.h"
#include "absl/time/time.h"
#include "absl/types/span.h"
#include "tcmalloc/arena.h"
#include "tcmalloc/common.h"
#include "tcmalloc/experiment.h"
#include "tcmalloc/experiment_config.h"
#include "tcmalloc/huge_pages.h"
#include "tcmalloc/internal/logging.h"
#include "tcmalloc/pagemap.h"
#include "tcmalloc/pages.h"
#include "tcmalloc/parameters.h"
#include "tcmalloc/sizemap.h"
#include "tcmalloc/span.h"
#include "tcmalloc/static_vars.h"
#include "tcmalloc/stats.h"
#include "tcmalloc/testing/thread_manager.h"

namespace tcmalloc {
namespace tcmalloc_internal {
namespace {

// Stand-in for Static, so that allocations can be observed on an Arena that is
// not shared with the running allocator.
class FakeState {
 public:
  Arena& arena() { return arena_; }

 private:
  Arena arena_;
};

ABSL_CONST_INIT FakeState fake_state;

using FakeForwarder = StaticForwarder<FakeState, fake_state>;
using ProdForwarder = StaticForwarder<Static, tc_globals>;

size_t TestBytes(const ArenaStats& stats) {
  return stats.bytes_allocated_per_type[static_cast<size_t>(ArenaAlloc::kTest)];
}

TEST(StaticForwarderTest, SizeMap) {
  tc_globals.InitIfNecessary();
  for (int size_class = 0; size_class < kNumClasses; ++size_class) {
    EXPECT_EQ(ProdForwarder::class_to_size(size_class),
              tc_globals.sizemap().class_to_size(size_class));
    EXPECT_EQ(ProdForwarder::num_objects_to_move(size_class),
              tc_globals.sizemap().num_objects_to_move(size_class));
  }
}

TEST(StaticForwarderTest, AllocUsesStateArena) {
  constexpr size_t kSize = 24;
  size_t total = 0;
  for (size_t alignment : {size_t{8}, size_t{64}, size_t{4096}}) {
    void* p = FakeForwarder::Alloc(ArenaAlloc::kTest, kSize,
                                   std::align_val_t{alignment});
    EXPECT_EQ(reinterpret_cast<uintptr_t>(p) % alignment, 0);
    total += kSize;
  }

  // Every byte allocated from fake_state's arena is accounted to the tag passed
  // to Alloc.
  const ArenaStats stats = fake_state.arena().stats();
  EXPECT_GE(TestBytes(stats), total);
  EXPECT_EQ(TestBytes(stats), stats.bytes_allocated);
}

TEST(StaticForwarderTest, AllocWithPageHeapLockHeld) {
  tc_globals.InitIfNecessary();
  constexpr size_t kSize = 16;
  const size_t before = TestBytes(tc_globals.arena().stats());

  // TransferCacheManager::Init allocates while holding pageheap_lock, so Alloc
  // must not acquire it.
  void* p;
  {
    PageHeapSpinLockHolder l;
    p = ProdForwarder::Alloc(ArenaAlloc::kTest, kSize, kAlignment);
  }
  EXPECT_EQ(reinterpret_cast<uintptr_t>(p) % static_cast<size_t>(kAlignment),
            0);
  EXPECT_GE(TestBytes(tc_globals.arena().stats()) - before, kSize);
}

TEST(StaticForwarderTest, AllocReportedImpending) {
  tc_globals.InitIfNecessary();
  constexpr size_t kSize = 64;
  const ArenaStats before = tc_globals.arena().stats();

  void* p = ProdForwarder::AllocReportedImpending(kSize, kAlignment);
  EXPECT_EQ(reinterpret_cast<uintptr_t>(p) % static_cast<size_t>(kAlignment),
            0);

  // The allocation is accounted to kCpuCache, but bytes_allocated excludes
  // kSize, which the caller already reported via
  // ArenaUpdateAllocatedAndNonresident.
  const ArenaStats after = tc_globals.arena().stats();
  constexpr size_t kCpuCache = static_cast<size_t>(ArenaAlloc::kCpuCache);
  const size_t cpu_cache_bytes = after.bytes_allocated_per_type[kCpuCache] -
                                 before.bytes_allocated_per_type[kCpuCache];
  EXPECT_GE(cpu_cache_bytes, kSize);
  EXPECT_EQ(after.bytes_allocated - before.bytes_allocated,
            cpu_cache_bytes - kSize);
}

TEST(StaticForwarderTest, ArenaUpdateAllocatedAndNonresident) {
  tc_globals.InitIfNecessary();
  constexpr int64_t kBytes = 1 << 20;
  const ArenaStats before = tc_globals.arena().stats();

  ProdForwarder::ArenaUpdateAllocatedAndNonresident(kBytes, kBytes);
  ArenaStats stats = tc_globals.arena().stats();
  EXPECT_EQ(stats.bytes_allocated - before.bytes_allocated, kBytes);
  EXPECT_EQ(stats.bytes_nonresident - before.bytes_nonresident, kBytes);

  ProdForwarder::ArenaUpdateAllocatedAndNonresident(-kBytes, -kBytes);
  stats = tc_globals.arena().stats();
  EXPECT_EQ(stats.bytes_allocated, before.bytes_allocated);
  EXPECT_EQ(stats.bytes_nonresident, before.bytes_nonresident);
}

TEST(StaticForwarderTest, Accessors) {
  tc_globals.InitIfNecessary();
  EXPECT_EQ(ProdForwarder::reuse_size_classes(),
            Static::size_class_configuration() ==
                SizeClassConfiguration::kReuseRelaxedBelow64);
  EXPECT_EQ(&ProdForwarder::numa_topology(), &tc_globals.numa_topology());
  EXPECT_EQ(&ProdForwarder::sharded_transfer_cache(),
            &tc_globals.sharded_transfer_cache());
  EXPECT_EQ(&ProdForwarder::transfer_cache(), &tc_globals.transfer_cache());
  EXPECT_EQ(ProdForwarder::UseGenericShardedCache(),
            IsExperimentActive(Experiment::TCMALLOC_SHARDED_TC_ABLATION) &&
                !IsExperimentActive(
                    Experiment::TEST_ONLY_TCMALLOC_SHARDED_TRANSFER_CACHE));
  EXPECT_EQ(ProdForwarder::UseShardedCacheForLargeClassesOnly(),
            IsExperimentActive(
                Experiment::TEST_ONLY_TCMALLOC_SHARDED_TRANSFER_CACHE));
#ifndef TCMALLOC_INTERNAL_SMALL_BUT_SLOW
  // The sharded transfer cache captures its configuration during Init.  The
  // small-but-slow build has no sharded transfer cache.
  EXPECT_EQ(ProdForwarder::UseGenericShardedCache(),
            tc_globals.sharded_transfer_cache().UseGenericCache());
  EXPECT_EQ(ProdForwarder::UseShardedCacheForLargeClassesOnly(),
            tc_globals.sharded_transfer_cache().UseCacheForLargeClassesOnly());
#endif  // TCMALLOC_INTERNAL_SMALL_BUT_SLOW
  EXPECT_EQ(ProdForwarder::active_partitions(), tc_globals.active_partitions());
  EXPECT_EQ(ProdForwarder::UseWiderSlabs(),
            tc_globals.active_partitions() == 1);
  EXPECT_EQ(ProdForwarder::multiple_non_numa_partitions(),
            Static::multiple_non_numa_partitions());
  EXPECT_EQ(ProdForwarder::HaveHooks(), Static::HaveHooks());
  EXPECT_EQ(&ProdForwarder::arena(), &tc_globals.arena());
}

TEST(StaticForwarderTest, PageMap) {
  tc_globals.InitIfNecessary();
  constexpr int kSizeClass = 1;
  const Length pages_per_span = ProdForwarder::class_to_pages(kSizeClass);
  const size_t objects_per_span =
      pages_per_span.in_bytes() / ProdForwarder::class_to_size(kSizeClass);
  Span* span =
      ProdForwarder::AllocateSpan(kSizeClass, objects_per_span, pages_per_span);
  ASSERT_NE(span, nullptr);
  const PageId p = span->first_page();
  const HugePage hp = HugePageContaining(p);

  EXPECT_EQ(ProdForwarder::GetHugepage(hp),
            tc_globals.pagemap().GetHugepage(hp.first_page()));
  {
    PageHeapSpinLockHolder l;
    EXPECT_TRUE(ProdForwarder::Ensure(Range(p, pages_per_span)));
  }

  // ClearSpan and SetSpan expect the size class to be unregistered.
  tc_globals.pagemap().UnregisterSizeClass(span);
  ProdForwarder::ClearSpan(p);
  EXPECT_EQ(tc_globals.pagemap().GetDescriptor(p), &tc_globals.invalid_span());
  ProdForwarder::SetSpan(p, span);
  EXPECT_EQ(tc_globals.pagemap().GetDescriptor(p), span);

  ProdForwarder::DeallocateSpans(objects_per_span, absl::MakeSpan(&span, 1));
}

TEST(StaticForwarderTest, Parameters) {
  EXPECT_EQ(ProdForwarder::enable_unfiltered_collapse(),
            Parameters::enable_unfiltered_collapse());
  EXPECT_EQ(ProdForwarder::filler_skip_subrelease_long_interval(),
            Parameters::filler_skip_subrelease_long_interval());
  EXPECT_EQ(ProdForwarder::filler_skip_subrelease_short_interval(),
            Parameters::filler_skip_subrelease_short_interval());
  EXPECT_EQ(ProdForwarder::hpaa_subrelease(), Parameters::hpaa_subrelease());
  EXPECT_EQ(ProdForwarder::huge_region_adaptive_release(),
            Parameters::huge_region_adaptive_release());
  EXPECT_EQ(ProdForwarder::madvise_cold_regions_nohugepage(),
            Parameters::madvise_cold_regions_nohugepage());
  EXPECT_EQ(ProdForwarder::per_cpu_caches_dynamic_slab_enabled(),
            Parameters::per_cpu_caches_dynamic_slab_enabled());
  EXPECT_EQ(ProdForwarder::per_cpu_caches_dynamic_slab_grow_threshold(),
            Parameters::per_cpu_caches_dynamic_slab_grow_threshold());
  EXPECT_EQ(ProdForwarder::per_cpu_caches_dynamic_slab_shrink_threshold(),
            Parameters::per_cpu_caches_dynamic_slab_shrink_threshold());
  EXPECT_EQ(ProdForwarder::release_drained_slab_metadata(),
            Parameters::release_drained_slab_metadata());
  EXPECT_EQ(ProdForwarder::release_max_cold_pages(),
            Parameters::release_max_cold_pages());
  EXPECT_EQ(ProdForwarder::release_max_filler_pages(),
            Parameters::release_max_filler_pages());
  EXPECT_EQ(ProdForwarder::release_partial_alloc_pages(),
            Parameters::release_partial_alloc_pages());
  EXPECT_EQ(ProdForwarder::release_stale_pages(),
            Parameters::release_stale_pages());
  EXPECT_EQ(ProdForwarder::subrelease_unbacked_hugepages(),
            Parameters::subrelease_unbacked_hugepages());
  EXPECT_EQ(ProdForwarder::BackAllocations(),
            Parameters::back_small_allocations());
  EXPECT_EQ(ProdForwarder::BackSizeThresholdBytes(),
            Parameters::back_size_threshold_bytes());
}

class StaticForwarderSpanTest : public testing::TestWithParam<size_t> {
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

TEST_P(StaticForwarderSpanTest, Simple) {
  Span* span = ProdForwarder::AllocateSpan(size_class_, objects_per_span_,
                                           pages_per_span_);
  ASSERT_NE(span, nullptr);

  absl::FixedArray<void*> batch(objects_per_span_);
  const uint64_t alloc_time = ProdForwarder::clock_now();
  size_t allocated = span->BuildFreelist(object_size_, objects_per_span_,
                                         absl::MakeSpan(batch), alloc_time);
  ASSERT_EQ(allocated, objects_per_span_);

  EXPECT_EQ(size_class_, tc_globals.pagemap().sizeclass(span->first_page()));
  EXPECT_EQ(size_class_, tc_globals.pagemap().sizeclass(span->last_page()));

  // span_test.cc provides test coverage for Span, but we need to obtain several
  // objects to confirm we can map back to the Span pointer from the PageMap.
  for (void* ptr : batch) {
    Span* got;
    ProdForwarder::MapObjectsToSpans({&ptr, 1}, &got, size_class_);
    EXPECT_EQ(span, got);
  }

  for (void* ptr : batch) {
    EXPECT_EQ(span->FreelistPushBatch(absl::MakeSpan(&ptr, 1), object_size_,
                                      size_reciprocal_),
              ptr != batch.back());
  }

  ProdForwarder::DeallocateSpans(objects_per_span_, absl::MakeSpan(&span, 1));
}

TEST(StaticForwarderDeathTest, MapObjectsToSpansErrors) {
  constexpr int size_class = 1;
  const size_t object_size = tc_globals.sizemap().class_to_size(size_class);
  const Length pages_per_span = tc_globals.sizemap().class_to_pages(size_class);
  const size_t objects_per_span = pages_per_span.in_bytes() / object_size;
  const size_t size_reciprocal = Span::CalcReciprocal(object_size);

  Span* span =
      ProdForwarder::AllocateSpan(size_class, objects_per_span, pages_per_span);
  ASSERT_NE(span, nullptr);

  absl::FixedArray<void*> batch(objects_per_span);
  const uint64_t alloc_time = ProdForwarder::clock_now();
  size_t allocated = span->BuildFreelist(object_size, objects_per_span,
                                         absl::MakeSpan(batch), alloc_time);
  ASSERT_EQ(allocated, objects_per_span);

  // Mismatched size class
  void* ptr = batch[0];
  Span* got = nullptr;
  EXPECT_DEATH(ProdForwarder::MapObjectsToSpans({&ptr, 1}, &got,
                                                /*expected_size_class=*/2),
               "Mismatched-size-class");

  // Corrupted / unallocated pointer
  void* invalid_ptr = nullptr;
  EXPECT_DEATH(
      ProdForwarder::MapObjectsToSpans({&invalid_ptr, 1}, &got, size_class),
      "Attempted to free corrupted pointer");

  for (void* p : batch) {
    (void)span->FreelistPushBatch(absl::MakeSpan(&p, 1), object_size,
                                  size_reciprocal);
  }
  ProdForwarder::DeallocateSpans(objects_per_span, absl::MakeSpan(&span, 1));

  // Double free (after deallocation, span descriptor is invalid)
  EXPECT_DEATH(ProdForwarder::MapObjectsToSpans({&ptr, 1}, &got, size_class),
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
      ProdForwarder::MapObjectsToSpans({&data->batch[0], 1}, &got, size_class_);
      EXPECT_EQ(data->span, got);

      free_spans.push_back(data->span);
    }

    auto span_span = absl::MakeSpan(free_spans);
    for (int i = 0; i < spans.size(); i += kMaxObjectsToMove) {
      ProdForwarder::DeallocateSpans(objects_per_span_,
                                     span_span.subspan(i, kMaxObjectsToMove));
    }
  }

  void Grow() {
    // Allocate a Span
    Span* span = ProdForwarder::AllocateSpan(size_class_, objects_per_span_,
                                             pages_per_span_);
    ASSERT_NE(span, nullptr);

    auto d = std::make_unique<SpanData>();
    d->span = span;

    size_t allocated = span->BuildFreelist(
        object_size_, objects_per_span_, absl::MakeSpan(d->batch, batch_size_),
        ProdForwarder::clock_now());
    EXPECT_LE(allocated, objects_per_span_);

    EXPECT_EQ(size_class_, tc_globals.pagemap().sizeclass(span->first_page()));
    EXPECT_EQ(size_class_, tc_globals.pagemap().sizeclass(span->last_page()));
    // Confirm we can map at least one object back.
    Span* got;
    ProdForwarder::MapObjectsToSpans({&d->batch[0], 1}, &got, size_class_);
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
      ProdForwarder::MapObjectsToSpans({&data->batch[0], 1}, &got, size_class_);
      EXPECT_EQ(data->span, got);

      free_spans.push_back(data->span);
    }

    auto span_span = absl::MakeSpan(free_spans);
    for (int i = 0; i < spans.size(); i += kMaxObjectsToMove) {
      ProdForwarder::DeallocateSpans(objects_per_span_,
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

TEST_P(StaticForwarderSpanTest, Fuzz) {
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

INSTANTIATE_TEST_SUITE_P(All, StaticForwarderSpanTest,
                         testing::Range(size_t(1), kNumClasses));

}  // namespace
}  // namespace tcmalloc_internal
}  // namespace tcmalloc
