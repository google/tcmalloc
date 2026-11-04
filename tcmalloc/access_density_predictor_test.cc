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

#include "tcmalloc/access_density_predictor.h"

#include <stddef.h>
#include <stdint.h>

#include <algorithm>
#include <set>
#include <string>
#include <vector>

#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "absl/base/attributes.h"
#include "absl/cleanup/cleanup.h"
#include "absl/strings/str_format.h"
#include "absl/time/time.h"
#include "absl/types/span.h"
#include "tcmalloc/central_freelist_options.h"
#include "tcmalloc/common.h"
#include "tcmalloc/global_stats.h"
#include "tcmalloc/huge_page_tracker.h"
#include "tcmalloc/huge_pages.h"
#include "tcmalloc/internal/config.h"
#include "tcmalloc/internal/logging.h"
#include "tcmalloc/internal/slc_exclusion.h"
#include "tcmalloc/malloc_extension.h"
#include "tcmalloc/pagemap.h"
#include "tcmalloc/pages.h"
#include "tcmalloc/parameters.h"
#include "tcmalloc/span.h"
#include "tcmalloc/static_forwarder.h"
#include "tcmalloc/static_vars.h"
#include "tcmalloc/testing/testutil.h"

namespace tcmalloc {
namespace tcmalloc_internal {
namespace {

using ProdForwarder = StaticForwarder<Static, tc_globals>;

// Compile-time check that AccessDensityPredictor is zero-initialized for .bss.
TCMALLOC_ATTRIBUTE_NO_DESTROY ABSL_CONST_INIT
    ABSL_ATTRIBUTE_SECTION_VARIABLE(.bss) AccessDensityPredictor bss_predictor
    [[maybe_unused]];

void InitUniform1PageClasses(
    AccessDensityPredictor::SizeClassInfo (&classes)[kColdClassesStart],
    size_t num_classes_per_partition = 16) {
  for (size_t p = 0; p < kNormalPartitions; ++p) {
    const size_t base = p * kNumBaseClasses;
    for (size_t i = 1;
         i <= std::min(num_classes_per_partition, kNumBaseClasses - 1); ++i) {
      classes[base + i] = AccessDensityPredictor::SizeClassInfo{
          .object_size = i * 64,
          .pages_per_span = Length(1),
          .frontend_allocated_objects = 0,
          .backend_allocated_objects = 0,
      };
    }
  }
}

TEST(AccessDensityPredictorTest,
     UninitializedUsesStaticObjectsPerSpanFallback) {
  AccessDensityPredictor predictor;
  EXPECT_FALSE(predictor.AnyPartitionInitialized());
  EXPECT_FALSE(predictor.IsPartitionInitialized(0));

  EXPECT_EQ(
      predictor.Predict(
          /*size_class=*/1,
          central_freelist_internal::kFewObjectsAllocMaxLimit + 1, Length(1)),
      AccessDensityPrediction::kDense);
  EXPECT_EQ(predictor.Predict(
                /*size_class=*/1,
                central_freelist_internal::kFewObjectsAllocMaxLimit, Length(1)),
            AccessDensityPrediction::kSparse);
}

TEST(AccessDensityPredictorTest, RejectsMultiPageColdAndOutOfBoundsClasses) {
  AccessDensityPredictor predictor;
  AccessDensityPredictor::SizeClassInfo classes[kColdClassesStart];
  InitUniform1PageClasses(classes, /*num_classes_per_partition=*/4);
  // Mark class 2 as multi-page even with a huge allocation rate.
  classes[2].pages_per_span = Length(2);

  const absl::Time t0 = absl::FromUnixSeconds(1000);
  predictor.Update(t0, kNormalPartitions, classes);

  classes[1].frontend_allocated_objects = 10000;
  classes[2].frontend_allocated_objects = 100000000;
  predictor.Update(t0 + absl::Seconds(1), kNormalPartitions, classes);
  ASSERT_TRUE(predictor.IsPartitionInitialized(0));

  // Class 0, negative size classes, multi-page spans, cold classes, and
  // out-of-range classes must always return kSparse.
  EXPECT_EQ(
      predictor.Predict(/*size_class=*/0, /*objects_per_span=*/64, Length(1)),
      AccessDensityPrediction::kSparse);
  EXPECT_EQ(
      predictor.Predict(/*size_class=*/-1, /*objects_per_span=*/64, Length(1)),
      AccessDensityPrediction::kSparse);
  EXPECT_EQ(
      predictor.Predict(/*size_class=*/2, /*objects_per_span=*/64, Length(2)),
      AccessDensityPrediction::kSparse);
  EXPECT_EQ(
      predictor.Predict(/*size_class=*/1, /*objects_per_span=*/64, Length(2)),
      AccessDensityPrediction::kSparse);
  EXPECT_EQ(
      predictor.Predict(/*size_class=*/static_cast<int>(kColdClassesStart),
                        /*objects_per_span=*/64, Length(1)),
      AccessDensityPrediction::kSparse);
  EXPECT_EQ(predictor.Predict(/*size_class=*/static_cast<int>(kNumClasses + 5),
                              /*objects_per_span=*/64, Length(1)),
            AccessDensityPrediction::kSparse);
}

TEST(AccessDensityPredictorTest, BaselineNonPositiveElapsedAndCounterClamp) {
  AccessDensityPredictor predictor;
  AccessDensityPredictor::SizeClassInfo classes[kColdClassesStart];
  InitUniform1PageClasses(classes, /*num_classes_per_partition=*/4);
  classes[1].frontend_allocated_objects = 5000;
  classes[2].frontend_allocated_objects = 5000;

  const absl::Time t0 = absl::FromUnixSeconds(2000);
  EXPECT_EQ(predictor.updates(), 0);
  // First Update records baseline only; partition remains uninitialized.
  predictor.Update(t0, /*active_partitions=*/1, classes);
  EXPECT_FALSE(predictor.AnyPartitionInitialized());
  EXPECT_EQ(predictor.updates(), 0);

  // Equal or backwards timestamps are ignored without corrupting baseline.
  classes[1].frontend_allocated_objects = 50000;
  predictor.Update(t0, /*active_partitions=*/1, classes);
  EXPECT_FALSE(predictor.AnyPartitionInitialized());
  EXPECT_EQ(predictor.updates(), 0);
  predictor.Update(t0 - absl::Seconds(1), /*active_partitions=*/1, classes);
  EXPECT_FALSE(predictor.AnyPartitionInitialized());
  EXPECT_EQ(predictor.updates(), 0);

  // Simulate counter decrease on class 2 (clamped to 0) while class 1 advances
  // from its t0 baseline (5000 -> 50000).
  classes[2].frontend_allocated_objects = 1000;
  predictor.Update(t0 + absl::Seconds(1), /*active_partitions=*/1, classes);
  EXPECT_TRUE(predictor.IsPartitionInitialized(0));
  EXPECT_EQ(predictor.updates(), 1);
  EXPECT_EQ(
      predictor.Predict(/*size_class=*/1, /*objects_per_span=*/4, Length(1)),
      AccessDensityPrediction::kDense);
  EXPECT_EQ(
      predictor.Predict(/*size_class=*/2, /*objects_per_span=*/64, Length(1)),
      AccessDensityPrediction::kSparse);

  predictor.ResetForTesting();
  EXPECT_FALSE(predictor.AnyPartitionInitialized());
  EXPECT_EQ(predictor.updates(), 0);
}

TEST(AccessDensityPredictorTest, MonotonicInAllocRateAndImmuneToSingleWhale) {
  AccessDensityPredictor predictor;
  AccessDensityPredictor::SizeClassInfo classes[kColdClassesStart];
  InitUniform1PageClasses(classes, /*num_classes_per_partition=*/12);

  const absl::Time t0 = absl::FromUnixSeconds(3000);
  predictor.Update(t0, /*active_partitions=*/1, classes);

  // Class 1 is an object-count whale (1M allocs/s = 90.9% of raw object
  // allocations), classes 2..6 are genuinely hot (20k allocs/s each = 9.1% of
  // raw object allocations combined, which a 50% or 90% raw-count threshold
  // would starve into kSparse), and classes 7..12 are cold (1 alloc/s each).
  // Under sqrt(rate) weighting:
  //   sqrt(1e6) = 1000.0, 5 * sqrt(2e4) = 707.1, 6 * sqrt(1) = 6.0
  // so cum_share entering class 6 is 0.914 < 0.925 (all 6 hot classes are
  // kDense) and cum_share entering class 7 is 0.9965 >= 0.925 (classes 7..12
  // are kSparse).
  classes[1].frontend_allocated_objects = 1000000;
  for (size_t sc = 2; sc <= 6; ++sc) {
    classes[sc].frontend_allocated_objects = 20000;
  }
  for (size_t sc = 7; sc <= 12; ++sc) {
    classes[sc].frontend_allocated_objects = 1;
  }

  predictor.Update(t0 + absl::Seconds(1), /*active_partitions=*/1, classes);

  for (int sc = 1; sc <= 6; ++sc) {
    EXPECT_EQ(predictor.Predict(sc, /*objects_per_span=*/2, Length(1)),
              AccessDensityPrediction::kDense)
        << "Expected hot class " << sc << " to be kDense";
  }
  for (int sc = 7; sc <= 12; ++sc) {
    EXPECT_EQ(predictor.Predict(sc, /*objects_per_span=*/128, Length(1)),
              AccessDensityPrediction::kSparse)
        << "Expected cold class " << sc << " to be kSparse";
  }
}

TEST(AccessDensityPredictorTest, CombinesFrontendAndBackendCounters) {
  AccessDensityPredictor predictor;
  AccessDensityPredictor::SizeClassInfo classes[kColdClassesStart];
  InitUniform1PageClasses(classes, /*num_classes_per_partition=*/4);

  const absl::Time t0 = absl::FromUnixSeconds(4000);
  predictor.Update(t0, /*active_partitions=*/1, classes);

  // Class 1 is 100% CpuCache-shielded (high frontend, 0 backend).
  // Class 2 is unsampled / backend-driven (0 frontend, high backend).
  // Classes 3 and 4 are cold (1 alloc/s).
  classes[1].frontend_allocated_objects = 100000;
  classes[1].backend_allocated_objects = 0;
  classes[2].frontend_allocated_objects = 0;
  classes[2].backend_allocated_objects = 100000;
  classes[3].backend_allocated_objects = 1;
  classes[4].frontend_allocated_objects = 1;

  predictor.Update(t0 + absl::Seconds(1), /*active_partitions=*/1, classes);

  EXPECT_EQ(
      predictor.Predict(/*size_class=*/1, /*objects_per_span=*/4, Length(1)),
      AccessDensityPrediction::kDense);
  EXPECT_EQ(
      predictor.Predict(/*size_class=*/2, /*objects_per_span=*/4, Length(1)),
      AccessDensityPrediction::kDense);
  EXPECT_EQ(
      predictor.Predict(/*size_class=*/3, /*objects_per_span=*/64, Length(1)),
      AccessDensityPrediction::kSparse);
  EXPECT_EQ(
      predictor.Predict(/*size_class=*/4, /*objects_per_span=*/64, Length(1)),
      AccessDensityPrediction::kSparse);
}

TEST(AccessDensityPredictorTest, EwmaAdaptsAcrossWorkloadPhaseTransition) {
  AccessDensityPredictor predictor;
  AccessDensityPredictor::SizeClassInfo classes[kColdClassesStart];
  InitUniform1PageClasses(classes, /*num_classes_per_partition=*/2);

  absl::Time t = absl::FromUnixSeconds(5000);
  predictor.Update(t, /*active_partitions=*/1, classes);

  // Phase 1: class 1 hot (100k/s), class 2 cold (1/s).
  t += absl::Seconds(1);
  classes[1].frontend_allocated_objects += 100000;
  classes[2].frontend_allocated_objects += 1;
  predictor.Update(t, /*active_partitions=*/1, classes);
  EXPECT_EQ(
      predictor.Predict(/*size_class=*/1, /*objects_per_span=*/4, Length(1)),
      AccessDensityPrediction::kDense);
  EXPECT_EQ(
      predictor.Predict(/*size_class=*/2, /*objects_per_span=*/64, Length(1)),
      AccessDensityPrediction::kSparse);

  // Phase 2: class 1 stops allocating while class 2 surges (1M/s) across
  // enough EWMA windows for class 1's decayed rate to drop below 7.5% share.
  for (int step = 0; step < 30; ++step) {
    t += absl::Seconds(5);
    classes[2].frontend_allocated_objects += 5000000;
    predictor.Update(t, /*active_partitions=*/1, classes);
  }
  EXPECT_EQ(
      predictor.Predict(/*size_class=*/1, /*objects_per_span=*/64, Length(1)),
      AccessDensityPrediction::kSparse);
  EXPECT_EQ(
      predictor.Predict(/*size_class=*/2, /*objects_per_span=*/4, Length(1)),
      AccessDensityPrediction::kDense);
}

TEST(AccessDensityPredictorTest, PartitionsAreClassifiedIndependently) {
  if constexpr (kNormalPartitions < 2) {
    GTEST_SKIP() << "Build variant has a single normal partition.";
  }

  AccessDensityPredictor predictor;
  AccessDensityPredictor::SizeClassInfo classes[kColdClassesStart];
  InitUniform1PageClasses(classes, /*num_classes_per_partition=*/2);

  const size_t p1_base = kNumBaseClasses;
  absl::Time t = absl::FromUnixSeconds(6000);
  predictor.Update(t, /*active_partitions=*/2, classes);

  // Window 1: only Partition 1 has traffic; Partition 0 has 0 allocations and
  // must remain on the static fallback.
  t += absl::Seconds(1);
  classes[p1_base + 2].frontend_allocated_objects += 100000;
  classes[p1_base + 1].frontend_allocated_objects += 1;
  predictor.Update(t, /*active_partitions=*/2, classes);

  EXPECT_FALSE(predictor.IsPartitionInitialized(0));
  EXPECT_TRUE(predictor.IsPartitionInitialized(1));
  EXPECT_EQ(predictor.Predict(static_cast<int>(p1_base + 2),
                              /*objects_per_span=*/4, Length(1)),
            AccessDensityPrediction::kDense);
  EXPECT_EQ(predictor.Predict(static_cast<int>(p1_base + 1),
                              /*objects_per_span=*/64, Length(1)),
            AccessDensityPrediction::kSparse);

  // Window 2: Partition 0 has opposite hotness (class 1 hot, class 2 cold).
  t += absl::Seconds(5);
  classes[1].frontend_allocated_objects += 500000;
  classes[2].frontend_allocated_objects += 1;
  classes[p1_base + 2].frontend_allocated_objects += 500000;
  classes[p1_base + 1].frontend_allocated_objects += 1;
  predictor.Update(t, /*active_partitions=*/2, classes);

  EXPECT_TRUE(predictor.IsPartitionInitialized(0));
  EXPECT_EQ(
      predictor.Predict(/*size_class=*/1, /*objects_per_span=*/4, Length(1)),
      AccessDensityPrediction::kDense);
  EXPECT_EQ(
      predictor.Predict(/*size_class=*/2, /*objects_per_span=*/64, Length(1)),
      AccessDensityPrediction::kSparse);
  EXPECT_EQ(predictor.Predict(static_cast<int>(p1_base + 2),
                              /*objects_per_span=*/4, Length(1)),
            AccessDensityPrediction::kDense);
  EXPECT_EQ(predictor.Predict(static_cast<int>(p1_base + 1),
                              /*objects_per_span=*/64, Length(1)),
            AccessDensityPrediction::kSparse);
}

void RefillAndReturnThroughTransferCache(int size_class, size_t total_objects) {
  const size_t num_to_move =
      tc_globals.sizemap().num_objects_to_move(size_class);
  const size_t batch_size =
      std::min<size_t>(kMaxObjectsToMove, std::max<size_t>(1, num_to_move));
  void* batch[kMaxObjectsToMove];
  size_t remaining = total_objects;
  while (remaining > 0) {
    const size_t want = std::min(remaining, batch_size);
    const int got = tc_globals.transfer_cache().RemoveRange(
        size_class, absl::MakeSpan(batch, want));
    ASSERT_GT(got, 0);
    tc_globals.transfer_cache().InsertRange(
        size_class, absl::MakeSpan(batch, static_cast<size_t>(got)));
    remaining -= std::min(remaining, static_cast<size_t>(got));
  }
}

struct TestClasses {
  int few_obj_sc = -1;
  size_t few_objs_per_span = 0;
  int many_obj_sc = -1;
  size_t many_objs_per_span = 0;
};

TestClasses FindPartition0SinglePageClasses() {
  TestClasses out;
  for (size_t sc = 1; sc < kNumBaseClasses; ++sc) {
    const size_t obj_size = tc_globals.sizemap().class_to_size(sc);
    const Length pages = tc_globals.sizemap().class_to_pages(sc);
    if (obj_size == 0 || pages != Length(1)) {
      continue;
    }
    const size_t objs_per_span =
        tc_globals.central_freelist(sc).objects_per_span();
    if (out.many_obj_sc == -1 &&
        objs_per_span > central_freelist_internal::kFewObjectsAllocMaxLimit) {
      out.many_obj_sc = static_cast<int>(sc);
      out.many_objs_per_span = objs_per_span;
    }
    if (out.few_obj_sc == -1 &&
        objs_per_span <= central_freelist_internal::kFewObjectsAllocMaxLimit) {
      out.few_obj_sc = static_cast<int>(sc);
      out.few_objs_per_span = objs_per_span;
    }
  }
  return out;
}

TEST(AccessDensityPredictorIntegrationTest,
     StaticForwarderRoutesByMeasuredAllocationRate) {
  ScopedBackgroundProcessActionsEnabled disable_bg(false);
  tc_globals.InitIfNecessary();
  tc_globals.access_density_predictor().ResetForTesting();
  absl::Cleanup reset = [] {
    tc_globals.access_density_predictor().ResetForTesting();
  };

  const TestClasses cls = FindPartition0SinglePageClasses();
  ASSERT_GT(cls.few_obj_sc, 0);
  ASSERT_GT(cls.many_obj_sc, 0);

  // Establish baseline t0, then drive 16,000 backend allocations on few_obj_sc
  // (which static objects_per_span <= 16 would classify kSparse) and only 1
  // object on many_obj_sc (which static objects_per_span > 16 would classify
  // kDense).
  const absl::Time t0 = absl::FromUnixSeconds(10000);
  tc_globals.UpdateAccessDensityPredictions(t0);
  RefillAndReturnThroughTransferCache(cls.few_obj_sc, /*total_objects=*/16000);
  RefillAndReturnThroughTransferCache(cls.many_obj_sc, /*total_objects=*/1);
  tc_globals.UpdateAccessDensityPredictions(t0 + absl::Seconds(1));

  ASSERT_TRUE(tc_globals.access_density_predictor().IsPartitionInitialized(0));

  Span* hot_few_span = ProdForwarder::AllocateSpan(
      cls.few_obj_sc, cls.few_objs_per_span, Length(1));
  ASSERT_NE(hot_few_span, nullptr);
  const PageTracker* hot_pt =
      ProdForwarder::GetHugepage(HugePageContaining(hot_few_span->first_page()))
          .Get();
  ASSERT_NE(hot_pt, nullptr);
  EXPECT_TRUE(hot_pt->HasDenseSpans());

  Span* cold_many_span = ProdForwarder::AllocateSpan(
      cls.many_obj_sc, cls.many_objs_per_span, Length(1));
  ASSERT_NE(cold_many_span, nullptr);
  const PageTracker* cold_pt =
      ProdForwarder::GetHugepage(
          HugePageContaining(cold_many_span->first_page()))
          .Get();
  ASSERT_NE(cold_pt, nullptr);
  EXPECT_FALSE(cold_pt->HasDenseSpans());

  ProdForwarder::DeallocateSpans(cls.few_objs_per_span, Length(1),
                                 absl::MakeSpan(&hot_few_span, 1));
  ProdForwarder::DeallocateSpans(cls.many_objs_per_span, Length(1),
                                 absl::MakeSpan(&cold_many_span, 1));
}

TEST(AccessDensityPredictorIntegrationTest,
     StaticForwarderHandlesPredictionFlipBeforeDeallocation) {
  ScopedBackgroundProcessActionsEnabled disable_bg(false);
  tc_globals.InitIfNecessary();
  tc_globals.access_density_predictor().ResetForTesting();
  absl::Cleanup reset = [] {
    tc_globals.access_density_predictor().ResetForTesting();
  };

  const TestClasses cls = FindPartition0SinglePageClasses();
  ASSERT_GT(cls.few_obj_sc, 0);
  ASSERT_GT(cls.many_obj_sc, 0);

  // Epoch 1: few_obj_sc is kDense, many_obj_sc is kSparse.
  absl::Time t = absl::FromUnixSeconds(20000);
  tc_globals.UpdateAccessDensityPredictions(t);
  RefillAndReturnThroughTransferCache(cls.few_obj_sc, /*total_objects=*/16000);
  RefillAndReturnThroughTransferCache(cls.many_obj_sc, /*total_objects=*/1);
  t += absl::Seconds(1);
  tc_globals.UpdateAccessDensityPredictions(t);

  Span* epoch1_dense_span = ProdForwarder::AllocateSpan(
      cls.few_obj_sc, cls.few_objs_per_span, Length(1));
  Span* epoch1_sparse_span = ProdForwarder::AllocateSpan(
      cls.many_obj_sc, cls.many_objs_per_span, Length(1));
  ASSERT_NE(epoch1_dense_span, nullptr);
  ASSERT_NE(epoch1_sparse_span, nullptr);
  EXPECT_TRUE(ProdForwarder::GetHugepage(
                  HugePageContaining(epoch1_dense_span->first_page()))
                  .Get()
                  ->HasDenseSpans());
  EXPECT_FALSE(ProdForwarder::GetHugepage(
                   HugePageContaining(epoch1_sparse_span->first_page()))
                   .Get()
                   ->HasDenseSpans());

  // Epoch 2: decay few_obj_sc over 45 idle windows (0.7^45 ~ 1e-7) and then
  // drive many_obj_sc across 5 windows so many_obj_sc flips to kDense and
  // few_obj_sc decays to kSparse while Epoch 1 spans are still live.
  for (int step = 0; step < 45; ++step) {
    t += absl::Seconds(1);
    tc_globals.UpdateAccessDensityPredictions(t);
  }
  for (int step = 0; step < 5; ++step) {
    RefillAndReturnThroughTransferCache(cls.many_obj_sc,
                                        /*total_objects=*/16000);
    t += absl::Seconds(1);
    tc_globals.UpdateAccessDensityPredictions(t);
  }

  Span* epoch2_sparse_span = ProdForwarder::AllocateSpan(
      cls.few_obj_sc, cls.few_objs_per_span, Length(1));
  Span* epoch2_dense_span = ProdForwarder::AllocateSpan(
      cls.many_obj_sc, cls.many_objs_per_span, Length(1));
  ASSERT_NE(epoch2_sparse_span, nullptr);
  ASSERT_NE(epoch2_dense_span, nullptr);
  EXPECT_FALSE(ProdForwarder::GetHugepage(
                   HugePageContaining(epoch2_sparse_span->first_page()))
                   .Get()
                   ->HasDenseSpans());
  EXPECT_TRUE(ProdForwarder::GetHugepage(
                  HugePageContaining(epoch2_dense_span->first_page()))
                  .Get()
                  ->HasDenseSpans());

  const PageId epoch1_dense_page = epoch1_dense_span->first_page();
  const PageId epoch1_sparse_page = epoch1_sparse_span->first_page();
  const PageId epoch2_sparse_page = epoch2_sparse_span->first_page();
  const PageId epoch2_dense_page = epoch2_dense_span->first_page();
  EXPECT_EQ(
      tc_globals.pagemap().GetExistingDescriptor(epoch1_dense_page).sizeclass(),
      cls.few_obj_sc);
  EXPECT_EQ(tc_globals.pagemap()
                .GetExistingDescriptor(epoch1_sparse_page)
                .sizeclass(),
            cls.many_obj_sc);
  EXPECT_EQ(tc_globals.pagemap()
                .GetExistingDescriptor(epoch2_sparse_page)
                .sizeclass(),
            cls.few_obj_sc);
  EXPECT_EQ(
      tc_globals.pagemap().GetExistingDescriptor(epoch2_dense_page).sizeclass(),
      cls.many_obj_sc);

  // Deallocate both pre-flip and post-flip spans and confirm pagemap/filler
  // accounting completes cleanly without assertion failures.
  ProdForwarder::DeallocateSpans(cls.few_objs_per_span, Length(1),
                                 absl::MakeSpan(&epoch1_dense_span, 1));
  ProdForwarder::DeallocateSpans(cls.many_objs_per_span, Length(1),
                                 absl::MakeSpan(&epoch1_sparse_span, 1));
  ProdForwarder::DeallocateSpans(cls.few_objs_per_span, Length(1),
                                 absl::MakeSpan(&epoch2_sparse_span, 1));
  ProdForwarder::DeallocateSpans(cls.many_objs_per_span, Length(1),
                                 absl::MakeSpan(&epoch2_dense_span, 1));
  EXPECT_TRUE(
      tc_globals.pagemap().GetExistingDescriptor(epoch1_dense_page).freed());
  EXPECT_TRUE(
      tc_globals.pagemap().GetExistingDescriptor(epoch1_sparse_page).freed());
  EXPECT_TRUE(
      tc_globals.pagemap().GetExistingDescriptor(epoch2_sparse_page).freed());
  EXPECT_TRUE(
      tc_globals.pagemap().GetExistingDescriptor(epoch2_dense_page).freed());
}

TEST(AccessDensityPredictorIntegrationTest,
     SlcExcludesOnlyPredictedSparseFillerHugePages) {
  ScopedBackgroundProcessActionsEnabled disable_bg(false);
  tc_globals.InitIfNecessary();
  const bool old_adp = Parameters::access_density_predictor();
  const bool old_slc = Parameters::slc_exclude_sparse_filler();
  Parameters::set_access_density_predictor(true);
  Parameters::set_slc_exclude_sparse_filler(true);
  tc_globals.access_density_predictor().ResetForTesting();
  absl::Cleanup reset = [old_adp, old_slc] {
    Parameters::set_access_density_predictor(old_adp);
    Parameters::set_slc_exclude_sparse_filler(old_slc);
    tc_globals.access_density_predictor().ResetForTesting();
  };

  const TestClasses cls = FindPartition0SinglePageClasses();
  ASSERT_GT(cls.few_obj_sc, 0);
  ASSERT_GT(cls.many_obj_sc, 0);

  // Pre-reserve span storage before enabling SLC exclusion so test bookkeeping
  // never allocates spans inside the sparse filler hugepages under test.
  const size_t kSpansToAlloc = kPagesPerHugePage.raw_num() + 1;
  std::vector<Span*> dense_spans;
  std::vector<Span*> sparse_spans;
  dense_spans.reserve(kSpansToAlloc);
  sparse_spans.reserve(kSpansToAlloc);

  // Epoch 1: classify few_obj_sc (<= 16 objs/span) as kDense and many_obj_sc
  // (> 16 objs/span) as kSparse.
  absl::Time t = absl::FromUnixSeconds(50000);
  tc_globals.UpdateAccessDensityPredictions(t);
  RefillAndReturnThroughTransferCache(cls.few_obj_sc, /*total_objects=*/16000);
  RefillAndReturnThroughTransferCache(cls.many_obj_sc, /*total_objects=*/1);
  t += absl::Seconds(1);
  tc_globals.UpdateAccessDensityPredictions(t);

  // Install the test actuator after warmup so warmup allocations do not
  // pollute recorder counts (especially in small_but_slow where transfer cache
  // capacity is 0). SlcExcludeRange runs under pageheap_lock (AllocationGuard),
  // so the actuator must not allocate heap memory.
  struct SlcRecorder {
    uintptr_t excluded_hugepages[64] = {};
    size_t num_excluded = 0;
    int exclude_calls = 0;
    int include_calls = 0;

    bool Contains(uintptr_t addr) const {
      for (size_t i = 0; i < num_excluded; ++i) {
        if (excluded_hugepages[i] == addr) return true;
      }
      return false;
    }
    void Insert(uintptr_t addr) {
      if (Contains(addr) || num_excluded >= 64) return;
      excluded_hugepages[num_excluded++] = addr;
    }
    void Erase(uintptr_t addr) {
      for (size_t i = 0; i < num_excluded; ++i) {
        if (excluded_hugepages[i] == addr) {
          excluded_hugepages[i] = excluded_hugepages[--num_excluded];
          return;
        }
      }
    }
  } recorder;
  auto actuator =
      +[](void* ptr, size_t size, bool exclude, void* ctx) -> int32_t {
    const uintptr_t base = reinterpret_cast<uintptr_t>(ptr);
    if (base % kHugePageSize != 0 || size != kHugePageSize) {
      return -22;
    }
    auto* rec = static_cast<SlcRecorder*>(ctx);
    if (exclude) {
      ++rec->exclude_calls;
      rec->Insert(base);
    } else {
      ++rec->include_calls;
      rec->Erase(base);
    }
    return 0;
  };
  ScopedSlcExclusionActuatorForTesting scoped_actuator(actuator, &recorder);

  // Allocate >= 1 hugepage of 1-page spans in each class to force at least one
  // fresh RefillFiller hugepage in both dense and sparse partitions.
  for (size_t i = 0; i < kSpansToAlloc; ++i) {
    Span* s = ProdForwarder::AllocateSpan(cls.few_obj_sc, cls.few_objs_per_span,
                                          Length(1));
    ASSERT_NE(s, nullptr);
    dense_spans.push_back(s);
  }
  EXPECT_EQ(recorder.exclude_calls, 0);
  for (Span* s : dense_spans) {
    const HugePage hp = HugePageContaining(s->first_page());
    bool pt_nonnull = false;
    bool has_dense = false;
    bool is_candidate = false;
    bool is_excluded = false;
    {
      PageHeapSpinLockHolder l;
      const PageTracker* pt = ProdForwarder::GetHugepage(hp).Get();
      if (pt != nullptr) {
        pt_nonnull = true;
        has_dense = pt->HasDenseSpans();
        is_candidate = pt->slc_exclude_candidate();
        is_excluded = pt->slc_excluded();
      }
    }
    ASSERT_TRUE(pt_nonnull);
    EXPECT_TRUE(has_dense);
    EXPECT_FALSE(is_candidate);
    EXPECT_FALSE(is_excluded);
    EXPECT_FALSE(
        recorder.Contains(reinterpret_cast<uintptr_t>(hp.start_addr())));
  }

  uintptr_t newly_excluded_hps[64] = {};
  size_t num_newly_excluded = 0;
  auto record_newly_excluded = [&](uintptr_t addr) {
    for (size_t i = 0; i < num_newly_excluded; ++i) {
      if (newly_excluded_hps[i] == addr) return;
    }
    if (num_newly_excluded < 64) {
      newly_excluded_hps[num_newly_excluded++] = addr;
    }
  };
  for (size_t i = 0; i < kSpansToAlloc; ++i) {
    Span* s = ProdForwarder::AllocateSpan(cls.many_obj_sc,
                                          cls.many_objs_per_span, Length(1));
    ASSERT_NE(s, nullptr);
    sparse_spans.push_back(s);
    const HugePage hp = HugePageContaining(s->first_page());
    bool pt_nonnull = false;
    bool has_dense = false;
    bool is_candidate = false;
    bool is_excluded = false;
    {
      PageHeapSpinLockHolder l;
      const PageTracker* pt = ProdForwarder::GetHugepage(hp).Get();
      if (pt != nullptr) {
        pt_nonnull = true;
        has_dense = pt->HasDenseSpans();
        is_candidate = pt->slc_exclude_candidate();
        is_excluded = pt->slc_excluded();
      }
    }
    ASSERT_TRUE(pt_nonnull);
    EXPECT_FALSE(has_dense);
    if (is_excluded) {
      EXPECT_TRUE(is_candidate);
      record_newly_excluded(reinterpret_cast<uintptr_t>(hp.start_addr()));
    }
  }
  EXPECT_GE(recorder.exclude_calls, 1);
  ASSERT_GT(num_newly_excluded, 0u);
  for (size_t i = 0; i < num_newly_excluded; ++i) {
    EXPECT_TRUE(recorder.Contains(newly_excluded_hps[i]));
  }

  // Flip predictions across epochs so many_obj_sc becomes kDense BEFORE freeing
  // its sparse_spans; ReleaseHugepage must still re-include the drained sparse
  // PageTracker before returning it to HugeCache.
  for (int step = 0; step < 45; ++step) {
    t += absl::Seconds(1);
    tc_globals.UpdateAccessDensityPredictions(t);
  }
  for (int step = 0; step < 5; ++step) {
    RefillAndReturnThroughTransferCache(cls.many_obj_sc,
                                        /*total_objects=*/16000);
    t += absl::Seconds(1);
    tc_globals.UpdateAccessDensityPredictions(t);
  }

  for (Span* s : dense_spans) {
    ProdForwarder::DeallocateSpans(cls.few_objs_per_span, Length(1),
                                   absl::MakeSpan(&s, 1));
  }
  for (Span* s : sparse_spans) {
    ProdForwarder::DeallocateSpans(cls.many_objs_per_span, Length(1),
                                   absl::MakeSpan(&s, 1));
  }

  EXPECT_GE(recorder.include_calls, static_cast<int>(num_newly_excluded));
  EXPECT_EQ(recorder.num_excluded, 0u);
}

std::string DumpPlaintextStats() {
  std::string buffer(1 << 20, '\0');
  Printer printer(buffer.data(), buffer.size());
  DumpStats(printer, /*level=*/2);
  buffer.resize(printer.SpaceRequired());
  return buffer;
}

std::string DumpProtoStats() {
  std::string buffer(3 << 20, '\0');
  Printer printer(buffer.data(), buffer.size());
  DumpStatsInPbtxt(printer, /*level=*/2);
  buffer.resize(printer.SpaceRequired());
  return buffer;
}

TEST(AccessDensityPredictorIntegrationTest, MalloczStatusAndDisableParameter) {
  using ::testing::HasSubstr;

  ScopedBackgroundProcessActionsEnabled disable_bg(false);
  tc_globals.InitIfNecessary();
  const bool old_enabled = Parameters::access_density_predictor();
  Parameters::set_access_density_predictor(true);
  tc_globals.access_density_predictor().ResetForTesting();
  absl::Cleanup reset = [old_enabled] {
    Parameters::set_access_density_predictor(old_enabled);
    tc_globals.access_density_predictor().ResetForTesting();
  };

  size_t static_dense = 0;
  size_t static_sparse = 0;
  for (size_t sc = 1; sc < kNumBaseClasses; ++sc) {
    const size_t obj_size = tc_globals.sizemap().class_to_size(sc);
    if (obj_size == 0) {
      continue;
    }
    const Length pages = tc_globals.sizemap().class_to_pages(sc);
    const size_t objs_per_span =
        tc_globals.central_freelist(sc).objects_per_span();
    if (AccessDensityPredictor::StaticPredict(objs_per_span, pages) ==
        AccessDensityPrediction::kDense) {
      ++static_dense;
    } else {
      ++static_sparse;
    }
  }
  ASSERT_GT(static_dense + static_sparse, 0);

  const TestClasses cls = FindPartition0SinglePageClasses();
  ASSERT_GT(cls.few_obj_sc, 0);
  ASSERT_GT(cls.many_obj_sc, 0);
  const size_t few_obj_size =
      tc_globals.sizemap().class_to_size(cls.few_obj_sc);
  const size_t many_obj_size =
      tc_globals.sizemap().class_to_size(cls.many_obj_sc);

  auto expect_mallocz_status = [&](bool expected_enabled,
                                   uint64_t expected_updates,
                                   bool expected_initialized,
                                   size_t expected_dense,
                                   size_t expected_sparse,
                                   bool expected_few_dense,
                                   bool expected_many_dense,
                                   double min_few_backend_rate) {
    const std::string text =
        kSanitizerPresent ? DumpPlaintextStats() : MallocExtension::GetStats();
    EXPECT_THAT(text, HasSubstr(absl::StrFormat(
                          "PARAMETER tcmalloc_access_density_predictor %d\n",
                          expected_enabled ? 1 : 0)));
    EXPECT_THAT(text, HasSubstr(absl::StrFormat(
                          "AccessDensityPredictor: enabled=%d, updates=%llu\n",
                          expected_enabled ? 1 : 0,
                          static_cast<unsigned long long>(expected_updates))));
    EXPECT_THAT(
        text,
        HasSubstr(absl::StrFormat(
            "AccessDensityPredictor: partition 0: initialized=%d, "
            "dense_size_classes=%zu, sparse_size_classes=%zu\n",
            expected_initialized ? 1 : 0, expected_dense, expected_sparse)));
    EXPECT_THAT(text, HasSubstr("  dense_classes: "));
    EXPECT_THAT(text, HasSubstr("  sparse_classes: "));
    EXPECT_THAT(text,
                HasSubstr(absl::StrFormat(
                    "  class %3d [ %8zu bytes ] :   1 pages; %8zu objs/span; "
                    "filler=%s;%s frontend_rate=",
                    cls.few_obj_sc, few_obj_size, cls.few_objs_per_span,
                    expected_few_dense ? "dense" : "sparse",
                    expected_few_dense ? " " : "")));
    EXPECT_THAT(text,
                HasSubstr(absl::StrFormat(
                    "  class %3d [ %8zu bytes ] :   1 pages; %8zu objs/span; "
                    "filler=%s;%s frontend_rate=",
                    cls.many_obj_sc, many_obj_size, cls.many_objs_per_span,
                    expected_many_dense ? "dense" : "sparse",
                    expected_many_dense ? " " : "")));
    if (expected_dense == 1 && expected_few_dense) {
      EXPECT_THAT(text,
                  HasSubstr(absl::StrFormat("  dense_classes: %d(%zuB)\n",
                                            cls.few_obj_sc, few_obj_size)));
    }
  };

  // 1. Before any update: enabled=1, updates=0, initialized=0, static counts.
  expect_mallocz_status(/*expected_enabled=*/true, /*expected_updates=*/0,
                        /*expected_initialized=*/false, static_dense,
                        static_sparse, /*expected_few_dense=*/false,
                        /*expected_many_dense=*/true,
                        /*min_few_backend_rate=*/0.0);

  // 2. Drive allocation traffic on few_obj_sc only so exactly 1 class in
  // partition 0 is classified kDense after UpdateAccessDensityPredictions.
  const absl::Time t0 = absl::FromUnixSeconds(30000);
  tc_globals.UpdateAccessDensityPredictions(t0);
  RefillAndReturnThroughTransferCache(cls.few_obj_sc, /*total_objects=*/16000);
  tc_globals.UpdateAccessDensityPredictions(t0 + absl::Seconds(1));

  const size_t total_normal_classes = static_dense + static_sparse;
  expect_mallocz_status(/*expected_enabled=*/true, /*expected_updates=*/1,
                        /*expected_initialized=*/true, /*expected_dense=*/1,
                        /*expected_sparse=*/total_normal_classes - 1,
                        /*expected_few_dense=*/true,
                        /*expected_many_dense=*/false,
                        /*min_few_backend_rate=*/16000.0);

  // Allocate live spans under the active predictor before flipping at runtime.
  Span* active_few_span = ProdForwarder::AllocateSpan(
      cls.few_obj_sc, cls.few_objs_per_span, Length(1));
  Span* active_many_span = ProdForwarder::AllocateSpan(
      cls.many_obj_sc, cls.many_objs_per_span, Length(1));
  ASSERT_NE(active_few_span, nullptr);
  ASSERT_NE(active_many_span, nullptr);
  EXPECT_TRUE(ProdForwarder::GetHugepage(
                  HugePageContaining(active_few_span->first_page()))
                  .Get()
                  ->HasDenseSpans());
  EXPECT_FALSE(ProdForwarder::GetHugepage(
                   HugePageContaining(active_many_span->first_page()))
                   .Get()
                   ->HasDenseSpans());

  // 3. Flip true -> false at runtime while active_few_span/active_many_span
  // remain live: /mallocz reports enabled=0 and static class counts, and
  // StaticForwarder routes new spans via the static heuristic.
  Parameters::set_access_density_predictor(false);
  expect_mallocz_status(/*expected_enabled=*/false, /*expected_updates=*/1,
                        /*expected_initialized=*/true, static_dense,
                        static_sparse, /*expected_few_dense=*/false,
                        /*expected_many_dense=*/true,
                        /*min_few_backend_rate=*/16000.0);

  Span* fallback_few_span = ProdForwarder::AllocateSpan(
      cls.few_obj_sc, cls.few_objs_per_span, Length(1));
  Span* fallback_many_span = ProdForwarder::AllocateSpan(
      cls.many_obj_sc, cls.many_objs_per_span, Length(1));
  ASSERT_NE(fallback_few_span, nullptr);
  ASSERT_NE(fallback_many_span, nullptr);
  EXPECT_FALSE(ProdForwarder::GetHugepage(
                   HugePageContaining(fallback_few_span->first_page()))
                   .Get()
                   ->HasDenseSpans());
  EXPECT_TRUE(ProdForwarder::GetHugepage(
                  HugePageContaining(fallback_many_span->first_page()))
                  .Get()
                  ->HasDenseSpans());

  // 4. Flip false -> true at runtime while both sets of spans are in flight:
  // /mallocz and StaticForwarder resume rate-based predictions, and spans
  // allocated on both sides of the transitions deallocate cleanly.
  Parameters::set_access_density_predictor(true);
  expect_mallocz_status(/*expected_enabled=*/true, /*expected_updates=*/1,
                        /*expected_initialized=*/true, /*expected_dense=*/1,
                        /*expected_sparse=*/total_normal_classes - 1,
                        /*expected_few_dense=*/true,
                        /*expected_many_dense=*/false,
                        /*min_few_backend_rate=*/16000.0);

  Span* reenabled_few_span = ProdForwarder::AllocateSpan(
      cls.few_obj_sc, cls.few_objs_per_span, Length(1));
  Span* reenabled_many_span = ProdForwarder::AllocateSpan(
      cls.many_obj_sc, cls.many_objs_per_span, Length(1));
  ASSERT_NE(reenabled_few_span, nullptr);
  ASSERT_NE(reenabled_many_span, nullptr);
  EXPECT_TRUE(ProdForwarder::GetHugepage(
                  HugePageContaining(reenabled_few_span->first_page()))
                  .Get()
                  ->HasDenseSpans());
  EXPECT_FALSE(ProdForwarder::GetHugepage(
                   HugePageContaining(reenabled_many_span->first_page()))
                   .Get()
                   ->HasDenseSpans());

  ProdForwarder::DeallocateSpans(cls.few_objs_per_span, Length(1),
                                 absl::MakeSpan(&active_few_span, 1));
  ProdForwarder::DeallocateSpans(cls.many_objs_per_span, Length(1),
                                 absl::MakeSpan(&active_many_span, 1));
  ProdForwarder::DeallocateSpans(cls.few_objs_per_span, Length(1),
                                 absl::MakeSpan(&fallback_few_span, 1));
  ProdForwarder::DeallocateSpans(cls.many_objs_per_span, Length(1),
                                 absl::MakeSpan(&fallback_many_span, 1));
  ProdForwarder::DeallocateSpans(cls.few_objs_per_span, Length(1),
                                 absl::MakeSpan(&reenabled_few_span, 1));
  ProdForwarder::DeallocateSpans(cls.many_objs_per_span, Length(1),
                                 absl::MakeSpan(&reenabled_many_span, 1));
}

TEST(AccessDensityPredictorTest,
     PrintReportsEmptyBucketsMultiPageAndExactClassRates) {
  using ::testing::HasSubstr;

  AccessDensityPredictor predictor;
  AccessDensityPredictor::SizeClassInfo classes[kColdClassesStart];
  // Class 1: 2048 B, 1 page, 4 objs/span (static sparse).
  // Class 2: 4096 B, 1 page, 2 objs/span (static sparse).
  // Class 3: 6144 B, 3 pages, 4 objs/span (multi-page, always sparse).
  classes[1] = {.object_size = 2048, .pages_per_span = Length(1)};
  classes[2] = {.object_size = 4096, .pages_per_span = Length(1)};
  classes[3] = {.object_size = 6144, .pages_per_span = Length(3)};

  auto geometry_fn = [](size_t sc) -> AccessDensityPredictor::ClassGeometry {
    switch (sc) {
      case 1:
        return {.object_size = 2048,
                .objects_per_span = 4,
                .pages_per_span = Length(1)};
      case 2:
        return {.object_size = 4096,
                .objects_per_span = 2,
                .pages_per_span = Length(1)};
      case 3:
        return {.object_size = 6144,
                .objects_per_span = 4,
                .pages_per_span = Length(3)};
      default:
        return {};
    }
  };

  auto render_text = [&](bool enabled) {
    std::string buf(16 << 10, '\0');
    Printer out(buf.data(), buf.size());
    predictor.Print(out, enabled, /*active_partitions=*/1, geometry_fn);
    buf.resize(out.SpaceRequired());
    return buf;
  };

  // Uninitialized: all 3 classes have <= 16 objs/span or >1 page, so dense is
  // empty ("none") and all 3 are in sparse_classes.
  const std::string uninit_text = render_text(/*enabled=*/true);
  EXPECT_THAT(uninit_text,
              HasSubstr("AccessDensityPredictor: partition 0: initialized=0, "
                        "dense_size_classes=0, sparse_size_classes=3\n"));
  EXPECT_THAT(uninit_text, HasSubstr("  dense_classes: none\n"));
  EXPECT_THAT(uninit_text,
              HasSubstr("  sparse_classes: 1(2048B) 2(4096B) 3(6144B)\n"));

  // Drive hot rates on both 1-page classes (1 & 2) AND multi-page class 3.
  // If we also test a geometry with only classes 1 & 2 (both 1-page and hot),
  // sparse_classes becomes "none".
  const absl::Time t0 = absl::FromUnixSeconds(40000);
  predictor.Update(t0, /*active_partitions=*/1, classes);
  classes[1].frontend_allocated_objects = 2500;
  classes[1].backend_allocated_objects = 100;
  classes[2].frontend_allocated_objects = 2500;
  classes[2].backend_allocated_objects = 50;
  classes[3].frontend_allocated_objects = 9000;
  classes[3].backend_allocated_objects = 300;
  predictor.Update(t0 + absl::Seconds(1), /*active_partitions=*/1, classes);

  const std::string active_text = render_text(/*enabled=*/true);
  EXPECT_THAT(active_text,
              HasSubstr("AccessDensityPredictor: partition 0: initialized=1, "
                        "dense_size_classes=2, sparse_size_classes=1\n"));
  EXPECT_THAT(active_text, HasSubstr("  dense_classes: 1(2048B) 2(4096B)\n"));
  EXPECT_THAT(active_text, HasSubstr("  sparse_classes: 3(6144B)\n"));
  EXPECT_THAT(
      active_text,
      HasSubstr(
          "  class   1 [     2048 bytes ] :   1 pages;        4 objs/span; "
          "filler=dense;  frontend_rate=    2500.0 objs/s; "
          "backend_rate=     100.0 objs/s\n"));
  EXPECT_THAT(
      active_text,
      HasSubstr(
          "  class   3 [     6144 bytes ] :   3 pages;        4 objs/span; "
          "filler=sparse; frontend_rate=    9000.0 objs/s; "
          "backend_rate=     300.0 objs/s\n"));

  // Geometry with only classes 1 & 2 exercises "sparse_classes: none".
  std::string all_dense_buf(16 << 10, '\0');
  Printer all_dense_out(all_dense_buf.data(), all_dense_buf.size());
  predictor.Print(all_dense_out, /*enabled=*/true, /*active_partitions=*/1,
                  [&](size_t sc) -> AccessDensityPredictor::ClassGeometry {
                    return sc <= 2 ? geometry_fn(sc)
                                   : AccessDensityPredictor::ClassGeometry{};
                  });
  all_dense_buf.resize(all_dense_out.SpaceRequired());
  EXPECT_THAT(all_dense_buf, HasSubstr("  dense_classes: 1(2048B) 2(4096B)\n"));
  EXPECT_THAT(all_dense_buf, HasSubstr("  sparse_classes: none\n"));

  if constexpr (kNormalPartitions >= 2) {
    const size_t p1_base = kNumBaseClasses;
    classes[p1_base + 1] = {.object_size = 2048, .pages_per_span = Length(1)};
    classes[p1_base + 2] = {.object_size = 4096, .pages_per_span = Length(1)};
    predictor.ResetForTesting();
    predictor.Update(t0, /*active_partitions=*/2, classes);
    classes[1].frontend_allocated_objects += 5000;
    classes[p1_base + 2].frontend_allocated_objects += 8000;
    predictor.Update(t0 + absl::Seconds(1), /*active_partitions=*/2, classes);

    auto two_part_geom =
        [&](size_t sc) -> AccessDensityPredictor::ClassGeometry {
      if (sc < kNumBaseClasses) {
        return geometry_fn(sc);
      }
      if (sc == p1_base + 1 || sc == p1_base + 2) {
        return geometry_fn(sc - p1_base);
      }
      return {};
    };
    std::string mp_buf(16 << 10, '\0');
    Printer mp_out(mp_buf.data(), mp_buf.size());
    predictor.Print(mp_out, /*enabled=*/true, /*active_partitions=*/2,
                    two_part_geom);
    mp_buf.resize(mp_out.SpaceRequired());
    EXPECT_THAT(mp_buf, HasSubstr(absl::StrFormat(
                            "AccessDensityPredictor: partition 1: "
                            "initialized=1, dense_size_classes=1, "
                            "sparse_size_classes=1\n  dense_classes: %zu(4096B)"
                            "\n  sparse_classes: %zu(2048B)\n",
                            p1_base + 2, p1_base + 1)));
  }
}

}  // namespace
}  // namespace tcmalloc_internal
}  // namespace tcmalloc
