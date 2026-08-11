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

#ifndef TCMALLOC_HUGE_PAGE_FILLER_H_
#define TCMALLOC_HUGE_PAGE_FILLER_H_

#include <stddef.h>
#include <stdint.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <climits>
#include <cstddef>
#include <limits>
#include <optional>

#include "absl/algorithm/container.h"
#include "absl/base/attributes.h"
#include "absl/base/internal/cycleclock.h"
#include "absl/base/nullability.h"
#include "absl/base/optimization.h"
#include "absl/base/thread_annotations.h"
#include "absl/numeric/bits.h"
#include "absl/strings/str_format.h"
#include "absl/strings/string_view.h"
#include "absl/time/time.h"
#include "absl/types/span.h"
#include "tcmalloc/common.h"
#include "tcmalloc/hinted_tracker_lists.h"
#include "tcmalloc/huge_cache.h"
#include "tcmalloc/huge_page_options.h"
#include "tcmalloc/huge_page_subrelease.h"
#include "tcmalloc/huge_page_tracker.h"
#include "tcmalloc/huge_page_treatment.h"
#include "tcmalloc/huge_pages.h"
#include "tcmalloc/internal/clock.h"
#include "tcmalloc/internal/config.h"
#include "tcmalloc/internal/exponential_biased.h"
#include "tcmalloc/internal/linked_list.h"
#include "tcmalloc/internal/logging.h"
#include "tcmalloc/internal/memory_tag.h"
#include "tcmalloc/internal/optimization.h"
#include "tcmalloc/internal/page_size.h"
#include "tcmalloc/internal/pageflags.h"
#include "tcmalloc/internal/range_tracker.h"
#include "tcmalloc/internal/residency.h"
#include "tcmalloc/internal/system_allocator.h"
#include "tcmalloc/pages.h"
#include "tcmalloc/span.h"
#include "tcmalloc/stats.h"

GOOGLE_MALLOC_SECTION_BEGIN
namespace tcmalloc {
namespace tcmalloc_internal {

// Records number of hugepages in different types of allocs.
//
// We use an additional element in the array to record the total sum of pages
// in kSparse and kDense allocs.
struct HugePageFillerStats {
  // Number of hugepages in fully-released alloc.
  HugeLength n_fully_released[AccessDensityPrediction::kPredictionCounts + 1];
  // Number of hugepages in partially-released alloc.
  HugeLength n_partial_released[AccessDensityPrediction::kPredictionCounts + 1];
  // Total hugepages that are either in fully- or partially-released allocs.
  HugeLength n_released[AccessDensityPrediction::kPredictionCounts + 1];
  // Total hugepages in the filler of a particular object count.
  HugeLength n_total[AccessDensityPrediction::kPredictionCounts + 1];
  // Total hugepages that have been fully allocated.
  HugeLength n_full[AccessDensityPrediction::kPredictionCounts + 1];
  // Number of hugepages in partially allocated (but not released) allocs.
  HugeLength n_partial[AccessDensityPrediction::kPredictionCounts + 1];
};

namespace huge_page_filler_internal {
// Computes some histograms of fullness. Because nearly empty/full huge pages
// are much more interesting, we calculate 4 buckets at each of the beginning
// and end of size one, and then divide the overall space by 16 to have 16
// (mostly) even buckets in the middle.
class UsageInfo {
 public:
  enum Type {
    kSparseRegular,
    kDenseRegular,
    kDonated,
    kSparsePartialReleased,
    kDensePartialReleased,
    kSparseReleased,
    kDenseReleased,
    kNumTypes
  };

  static constexpr size_t kMaxSampledTrackers = 64;

  UsageInfo() {
    size_t i;
    for (i = 0; i <= kBucketsAtBounds && i < kPagesPerHugePage.raw_num(); ++i) {
      bucket_bounds_[buckets_size_] = i;
      buckets_size_++;
    }
    // Histograms should have kBucketsAtBounds buckets at the start and at the
    // end. Additionally kPagesPerHugePage - kBucketsAtBounds must not
    // underflow. Hence the assert below.
    static_assert(kPagesPerHugePage.raw_num() >= kBucketsAtBounds);
    if (i < kPagesPerHugePage.raw_num() - kBucketsAtBounds) {
      // Because kPagesPerHugePage is a power of two, it must be at least 16
      // to get inside this "if".  The test fails if either (i=5 and
      // kPagesPerHugePage=8), or kPagesPerHugePage <= kBucketsAtBounds.
      TC_ASSERT_GE(kPagesPerHugePage, Length(16));
      constexpr int step = kPagesPerHugePage.raw_num() / 16;
      // We want to move in "step"-sized increments, aligned every "step".
      // So first we have to round i up to the nearest step boundary. This
      // logic takes advantage of step being a power of two, so step-1 is
      // all ones in the low-order bits.
      i = ((i - 1) | (step - 1)) + 1;
      for (; i < kPagesPerHugePage.raw_num() - kBucketsAtBounds; i += step) {
        bucket_bounds_[buckets_size_] = i;
        buckets_size_++;
      }
      i = kPagesPerHugePage.raw_num() - kBucketsAtBounds;
    }
    for (; i < kPagesPerHugePage.raw_num(); ++i) {
      bucket_bounds_[buckets_size_] = i;
      buckets_size_++;
    }

    // Native page Histograms bounds
    const size_t kHardwarePagesInHugePage = kHugePageSize / GetPageSize();
    const int kStep = kHardwarePagesInHugePage / kBucketsInBetween;
    // Ensure that the number of native page buckets is at least the number of
    // buckets at a bound.
    TC_ASSERT_GE(kHardwarePagesInHugePage, kBucketsAtBounds);
    // First kBucketsAtBounds buckets have a step size of 1
    for (int i = 0; i <= kBucketsAtBounds &&
                    native_page_buckets_size_ < kHardwarePagesInHugePage;
         ++i) {
      native_page_bucket_bounds_[native_page_buckets_size_] = i;
      ++native_page_buckets_size_;
    }

    // All the buckets in between should increment with a step of
    // kHardwarePagesInHugePage / kBucketsInBetween
    for (int i = 0; i < kHardwarePagesInHugePage - kBucketsAtBounds; ++i) {
      int bound =
          native_page_bucket_bounds_[native_page_buckets_size_ - 1] + kStep;
      // We break early so that we can log histogram at the end with step 1
      if (bound >= kHardwarePagesInHugePage - kBucketsAtBounds) {
        break;
      }
      native_page_bucket_bounds_[native_page_buckets_size_] = bound;
      ++native_page_buckets_size_;
    }

    // End kBucketBoundsBuckets have a step size of 1
    for (int i = 0; i < kBucketsAtBounds; ++i) {
      int end_bound = kHardwarePagesInHugePage - kBucketsAtBounds + i;
      // Prevent duplicate end bounds from being added to the histogram
      if (native_page_bucket_bounds_[native_page_buckets_size_ - 1] >=
          end_bound) {
        continue;
      }
      native_page_bucket_bounds_[native_page_buckets_size_] = end_bound;
      ++native_page_buckets_size_;
    }

    lifetime_bucket_bounds_[0] = 0;
    lifetime_bucket_bounds_[1] = 1;
    for (int i = 2; i <= kLifetimeBuckets; ++i) {
      lifetime_bucket_bounds_[i] = lifetime_bucket_bounds_[i - 1] * 10;
    }
    TC_CHECK_LE(buckets_size_, kBucketCapacity);
  }

  template <class TrackerType>
  std::optional<bool> IsHugepageBacked(const TrackerType& tracker,
                                       PageFlagsBase& pageflags) {
    void* addr = tracker.location().start_addr();
    // TODO(b/28093874): Investigate if pageflags may be queried without
    // pageheap_lock.
    return pageflags.IsHugepageBacked(addr);
  }

  // Reports the number of pages that were previously released, but later became
  // full and are hugepage backed.
  size_t HugepageBackedPreviouslyReleased() {
    return hugepage_backed_previously_released_;
  }

  // Maximum number of buckets at the start and end.
  static constexpr size_t kBucketsAtBounds = 8;
  // 16 buckets in the middle.
  static constexpr size_t kBucketsInBetween = 16;
  static constexpr size_t kBucketCapacity =
      kBucketsAtBounds + kBucketsInBetween + kBucketsAtBounds;

  static constexpr size_t kLifetimeBuckets = 8;
  using LifetimeHisto = uint32_t[kLifetimeBuckets];

  using Histo = uint32_t[kBucketCapacity];
  using SampledTrackers = PageTracker::TrackerFeatures[kMaxSampledTrackers];

  struct UsageInfoRecords {
    Histo free_page_histo{};
    Histo longest_free_histo{};
    Histo nalloc_histo{};
    LifetimeHisto live_lifetime_histo{};
    Histo long_lived_hps_histo{};
    LifetimeHisto low_occupancy_lifetime_histo{};
    SampledTrackers sampled_trackers{};
    Histo unbacked_histo{};
    Histo swapped_histo{};
    Histo free_unbacked_histo{};
    Histo free_swapped_histo{};
    Histo stale_histo{};
    Histo free_stale_histo{};

    size_t treated_hugepages{};
    size_t hugepage_backed{};
    size_t total_pages{};
    size_t collapse_skipped{};
    size_t collapse_skipped_due_to_backoff{};
    Length num_free_non_hugepage_backed{};
    Length num_free_hugepage_backed{};
    Length num_used_non_hugepage_backed{};
    Length num_used_hugepage_backed{};
    Length num_free_swapped{};
    Length num_used_swapped{};
    Length num_free_unbacked{};
    Length num_used_unbacked{};
    Length num_free_stale{};
    Length num_used_stale{};
  };

  template <class TrackerType>
  void Record(const TrackerType& pt, PageFlagsBase& pageflags, double clock_now,
              double clock_frequency, UsageInfoRecords& records,
              size_t& num_selected) {
    const Length free = kPagesPerHugePage - pt.used_pages();
    const Length lf = pt.longest_free_range();
    const size_t nalloc = pt.nallocs();
    // This is a little annoying as our buckets *have* to differ;
    // nalloc is in [1,256], free_pages and longest_free are in [0, 255].
    records.free_page_histo[BucketNum(free.raw_num())]++;
    records.longest_free_histo[BucketNum(lf.raw_num())]++;
    records.nalloc_histo[BucketNum(nalloc - 1)]++;

    const double elapsed = std::max<double>(clock_now - pt.alloctime(), 0);
    const absl::Duration lifetime =
        absl::Milliseconds(elapsed * 1000 / clock_frequency);
    ++records.live_lifetime_histo[LifetimeBucketNum(lifetime)];

    if (lifetime >= kLongLivedLifetime) {
      ++records.long_lived_hps_histo[BucketNum(nalloc - 1)];
    }

    if (free >= kLowOccupancyNumFreePages) {
      ++records.low_occupancy_lifetime_histo[LifetimeBucketNum(lifetime)];
    }

    if (IsHugepageBacked(pt, pageflags).value_or(false)) {
      ++records.hugepage_backed;
      if (pt.was_released()) {
        ++hugepage_backed_previously_released_;
      }
    }
    ++records.total_pages;

    PageTracker::HugePageResidencyState hugepage_residency_state =
        pt.GetHugePageResidencyState();
    if (hugepage_residency_state.entry_valid) {
      TC_ASSERT_GE(free, pt.released_pages());
      ++records.treated_hugepages;
      if (hugepage_residency_state.collapse_skipped) {
        ++records.collapse_skipped;
      }
      if (hugepage_residency_state.collapse_skipped_due_to_backoff) {
        ++records.collapse_skipped_due_to_backoff;
      }
      if (hugepage_residency_state.maybe_hugepage_backed) {
        records.num_free_hugepage_backed += (free - pt.released_pages());
        records.num_used_hugepage_backed += pt.used_pages();
      } else {
        records.num_free_non_hugepage_backed += (free - pt.released_pages());
        records.num_used_non_hugepage_backed += pt.used_pages();
        PageTracker::HardwarePageResidencyInfo info = pt.CountInfoInHugePage(
            hugepage_residency_state.unbacked, hugepage_residency_state.swapped,
            hugepage_residency_state.stale);

        auto unbacked_bits = info.n_used_unbacked + info.n_free_unbacked;
        auto swapped_bits = info.n_used_swapped + info.n_free_swapped;
        auto stale_bits = info.n_used_stale + info.n_free_stale;

        ++records.unbacked_histo[HardwarePageBucketNum(unbacked_bits)];
        ++records.swapped_histo[HardwarePageBucketNum(swapped_bits)];
        ++records.stale_histo[HardwarePageBucketNum(stale_bits)];

        ++records
              .free_unbacked_histo[HardwarePageBucketNum(info.n_free_unbacked)];
        ++records
              .free_swapped_histo[HardwarePageBucketNum(info.n_free_swapped)];
        ++records.free_stale_histo[HardwarePageBucketNum(info.n_free_stale)];
        records.num_free_swapped += Length(info.n_free_swapped);
        records.num_used_swapped += Length(info.n_used_swapped);
        records.num_free_unbacked += Length(info.n_free_unbacked);
        records.num_used_unbacked += Length(info.n_used_unbacked);
        records.num_free_stale += Length(info.n_free_stale);
        records.num_used_stale += Length(info.n_used_stale);
      }
    }

    PageTracker::TrackerFeatures tracker_features = pt.features();
    PageTracker::TagState tag_state = pt.GetTagState();

    // Selecting the first 64 tagged trackers could yield unrepresentative data
    // if we sample >> kMaxSampledTrackers, we expect this to be fine in the
    // common case, at least for initial exploration.
    if (tag_state.sampled_for_tagging && num_selected < kMaxSampledTrackers) {
      tracker_features.is_valid = true;
      tracker_features.reallocation_time =
          (pt.last_page_allocation_time() - tracker_features.allocation_time) /
          clock_frequency;
      records.sampled_trackers[num_selected] = tracker_features;
      ++num_selected;
    }
  }

  void Print(UsageInfoRecords& records, Type type, Printer& out) {
    TC_ASSERT_LT(type, kNumTypes);

    PrintHisto(out, records.free_page_histo, type,
               "hps with a<= # of free pages <b", 0);

    // For donated huge pages, number of allocs=1 and longest free range =
    // number of free pages, so it isn't useful to show the next two.
    if (type != kDonated) {
      PrintHisto(out, records.longest_free_histo, type,
                 "hps with a<= longest free range <b", 0);
      PrintHisto(out, records.nalloc_histo, type,
                 "hps with a<= # of allocations <b", 1);
    }

    PrintLifetimeHisto(out, records.live_lifetime_histo, type,
                       "hps with live lifetime a <= # hps < b");

    out.printf(
        "\nHugePageFiller: # of hps with >= %3zu free pages, with different "
        "lifetimes.",
        kLowOccupancyNumFreePages.raw_num());
    PrintLifetimeHisto(out, records.low_occupancy_lifetime_histo, type,
                       "hps with lifetime a <= # hps < b");

    out.printf("\nHugePageFiller: # of hps with lifetime >= %3zu ms.",
               absl::ToInt64Milliseconds(kLongLivedLifetime));
    PrintHisto(out, records.long_lived_hps_histo, type,
               "hps with a <= # of allocations < b", 0);

    PrintHardwarePageHisto(out, records.unbacked_histo, type,
                           "hps with a <= # of unbacked < b", 0);
    PrintHardwarePageHisto(out, records.swapped_histo, type,
                           "hps with a <= # of swapped < b", 0);
    PrintHardwarePageHisto(out, records.stale_histo, type,
                           "hps with a <= # of stale < b", 0);
    PrintHardwarePageHisto(out, records.free_unbacked_histo, type,
                           "hps with a <= # of free AND unbacked < b", 0);
    PrintHardwarePageHisto(out, records.free_swapped_histo, type,
                           "hps with a <= # of free AND swapped < b", 0);
    PrintHardwarePageHisto(out, records.free_stale_histo, type,
                           "hps with a <= # of free AND stale < b", 0);

    out.printf("\nHugePageFiller: %zu of %s free native pages are swapped.",
               records.num_free_swapped.raw_num(), TypeToStr(type));
    out.printf("\nHugePageFiller: %zu of %s used native pages are swapped.",
               records.num_used_swapped.raw_num(), TypeToStr(type));
    out.printf("\nHugePageFiller: %zu of %s free native pages are unbacked.",
               records.num_free_unbacked.raw_num(), TypeToStr(type));
    out.printf("\nHugePageFiller: %zu of %s used native pages are unbacked.",
               records.num_used_unbacked.raw_num(), TypeToStr(type));
    out.printf("\nHugePageFiller: %zu of %s free native pages are stale.",
               records.num_free_stale.raw_num(), TypeToStr(type));
    out.printf("\nHugePageFiller: %zu of %s used native pages are stale.",
               records.num_used_stale.raw_num(), TypeToStr(type));
    out.printf("\nHugePageFiller: %zu of %s pages hugepage backed out of %zu.",
               records.hugepage_backed, TypeToStr(type), records.total_pages);
    out.printf(
        "\nHugePageFiller: Of the non-hugepage backed pages of type %s, "
        "%zu tcmalloc pages are free, %zu tcmalloc pages are used.",
        TypeToStr(type), records.num_free_non_hugepage_backed.raw_num(),
        records.num_used_non_hugepage_backed.raw_num());
    out.printf(
        "\nHugePageFiller: Of the hugepage backed pages of type %s, "
        "%zu tcmalloc pages are free, %zu tcmalloc pages are used.",
        TypeToStr(type), records.num_free_hugepage_backed.raw_num(),
        records.num_used_hugepage_backed.raw_num());

    out.printf("\nHugePageFiller: %zu of %s pages treated out of %zu.",
               records.treated_hugepages, TypeToStr(type), records.total_pages);
    out.printf("\nHugePageFiller: %zu of %s pages skipped collapse out of %zu.",
               records.collapse_skipped, TypeToStr(type), records.total_pages);
    out.printf(
        "\nHugePageFiller: %zu of %s pages skipped collapse due to backoff out "
        "of %zu.",
        records.collapse_skipped_due_to_backoff, TypeToStr(type),
        records.total_pages);

    out.printf("\n");
    PrintSampledTrackers(out, type, records);
  }

  void Print(UsageInfoRecords& records, Type type, PbtxtRegion& hpaa) {
    PbtxtRegion scoped = hpaa.CreateSubRegion("filler_tracker");
    scoped.PrintRaw("type", AllocType(type));
    scoped.PrintRaw("objects", ObjectType(type));
    PrintHisto(scoped, records.free_page_histo, "free_pages_histogram", 0);
    PrintHisto(scoped, records.longest_free_histo,
               "longest_free_range_histogram", 0);
    PrintHisto(scoped, records.nalloc_histo, "allocations_histogram", 1);
    PrintLifetimeHisto(scoped, records.live_lifetime_histo,
                       "lifetime_histogram");
    PrintLifetimeHisto(scoped, records.low_occupancy_lifetime_histo,
                       "low_occupancy_lifetime_histogram");
    PrintHisto(scoped, records.long_lived_hps_histo,
               "long_lived_hugepages_histogram", 0);
    PrintHardwarePageHisto(scoped, records.unbacked_histo, "unbacked_histogram",
                           0);
    PrintHardwarePageHisto(scoped, records.swapped_histo, "swapped_histogram",
                           0);
    PrintHardwarePageHisto(scoped, records.free_unbacked_histo,
                           "free_unbacked_histogram", 0);
    PrintHardwarePageHisto(scoped, records.free_swapped_histo,
                           "free_swapped_histogram", 0);
    PrintSampledTrackers(scoped, type, "sampled_trackers", records);
    scoped.PrintI64("total_pages", records.total_pages);
    scoped.PrintI64("num_pages_hugepage_backed", records.hugepage_backed);
    scoped.PrintI64("num_free_pages_non_hugepage_backed",
                    records.num_free_non_hugepage_backed.raw_num());
    scoped.PrintI64("num_used_pages_non_hugepage_backed",
                    records.num_used_non_hugepage_backed.raw_num());
    scoped.PrintI64("num_free_pages_hugepage_backed",
                    records.num_free_hugepage_backed.raw_num());
    scoped.PrintI64("num_used_pages_hugepage_backed",
                    records.num_used_hugepage_backed.raw_num());
    scoped.PrintI64("num_pages_treated", records.treated_hugepages);
    scoped.PrintI64("num_pages_collapse_skipped", records.collapse_skipped);
    scoped.PrintI64("num_pages_collapse_skipped_due_to_backoff",
                    records.collapse_skipped_due_to_backoff);
    scoped.PrintI64("num_pages_free_swapped",
                    records.num_free_swapped.raw_num());
    scoped.PrintI64("num_pages_used_swapped",
                    records.num_used_swapped.raw_num());
    scoped.PrintI64("num_pages_free_unbacked",
                    records.num_free_unbacked.raw_num());
    scoped.PrintI64("num_pages_used_unbacked",
                    records.num_used_unbacked.raw_num());
    scoped.PrintI64("num_pages_free_stale", records.num_free_stale.raw_num());
    scoped.PrintI64("num_pages_used_stale", records.num_used_stale.raw_num());
  }

 private:
  // Threshold for a page to be long-lived, as a lifetime in milliseconds, for
  // telemetry purposes only.
  static constexpr absl::Duration kLongLivedLifetime =
      absl::Milliseconds(100000);
  // Threshold for a hugepage considered to have a low occupancy, for logging
  // lifetime telemetry only.
  static constexpr Length kLowOccupancyNumFreePages =
      Length(kPagesPerHugePage.raw_num() - (kPagesPerHugePage.raw_num() >> 3));

  int BucketNum(size_t page) {
    auto it =
        std::upper_bound(bucket_bounds_, bucket_bounds_ + buckets_size_, page);
    TC_CHECK_NE(it, bucket_bounds_);
    return it - bucket_bounds_ - 1;
  }

  int LifetimeBucketNum(absl::Duration duration) {
    int64_t duration_ms = absl::ToInt64Milliseconds(duration);
    auto it = std::upper_bound(lifetime_bucket_bounds_,
                               lifetime_bucket_bounds_ + kLifetimeBuckets,
                               duration_ms);
    TC_CHECK_NE(it, lifetime_bucket_bounds_);
    return it - lifetime_bucket_bounds_ - 1;
  }

  int HardwarePageBucketNum(size_t page) {
    auto it =
        std::upper_bound(native_page_bucket_bounds_,
                         native_page_bucket_bounds_ + buckets_size_, page);
    TC_CHECK_NE(it, native_page_bucket_bounds_);
    return it - native_page_bucket_bounds_ - 1;
  }

  void PrintHardwarePageHisto(Printer& out, Histo h, Type type,
                              absl::string_view blurb, size_t offset) {
    out.printf("\nHugePageFiller: # of %s %s", TypeToStr(type), blurb);
    for (size_t i = 0; i < native_page_buckets_size_; ++i) {
      if (i % 6 == 0) {
        out.printf("\nHugePageFiller:");
      }
      out.printf(" <%3zu<=%6zu", native_page_bucket_bounds_[i] + offset, h[i]);
    }
    out.printf("\n");
  }

  void PrintHardwarePageHisto(PbtxtRegion& hpaa, Histo h, absl::string_view key,
                              size_t offset) {
    for (size_t i = 0; i < buckets_size_; ++i) {
      if (h[i] == 0) continue;
      auto hist = hpaa.CreateSubRegion(key);
      hist.PrintI64("lower_bound", native_page_bucket_bounds_[i] + offset);
      hist.PrintI64(
          "upper_bound",
          (i == buckets_size_ - 1 ? native_page_bucket_bounds_[i]
                                  : native_page_bucket_bounds_[i + 1] - 1) +
              offset);
      hist.PrintI64("value", h[i]);
    }
  }

  void PrintHisto(Printer& out, Histo h, Type type, absl::string_view blurb,
                  size_t offset) {
    out.printf("\nHugePageFiller: # of %s %s", TypeToStr(type), blurb);
    for (size_t i = 0; i < buckets_size_; ++i) {
      if (i % 6 == 0) {
        out.printf("\nHugePageFiller:");
      }
      out.printf(" <%3zu<=%6zu", bucket_bounds_[i] + offset, h[i]);
    }
    out.printf("\n");
  }

  void PrintLifetimeHisto(Printer& out, LifetimeHisto h, Type type,
                          absl::string_view blurb) {
    out.printf("\nHugePageFiller: # of %s %s", TypeToStr(type), blurb);
    for (size_t i = 0; i < kLifetimeBuckets; ++i) {
      if (i % 6 == 0) {
        out.printf("\nHugePageFiller:");
      }
      out.printf(" < %3zu ms <= %6zu", lifetime_bucket_bounds_[i], h[i]);
    }
    out.printf("\n");
  }

  void PrintSampledTrackers(Printer& out, Type type,
                            UsageInfoRecords& records) {
    out.printf("\nHugePageFiller: Sampled Trackers for %s pages:",
               TypeToStr(type));
    for (size_t i = 0; i < kMaxSampledTrackers; ++i) {
      if (records.sampled_trackers[i].is_valid) {
        out.printf(
            "\nHugePageFiller: Allocations: %d, Longest Free Range: %d, "
            "Objects: %d, Is Hugepage Backed?: %d, Density: %d, "
            "Reallocation Time: %f",
            records.sampled_trackers[i].allocations,
            records.sampled_trackers[i].longest_free_range.raw_num(),
            records.sampled_trackers[i].objects,
            records.sampled_trackers[i].is_hugepage_backed,
            records.sampled_trackers[i].density,
            records.sampled_trackers[i].reallocation_time);
        records.sampled_trackers[i].is_valid = false;
      }
    }
    out.printf("\n");
  }

  void PrintHisto(PbtxtRegion& hpaa, Histo h, absl::string_view key,
                  size_t offset) {
    for (size_t i = 0; i < buckets_size_; ++i) {
      if (h[i] == 0) continue;
      auto hist = hpaa.CreateSubRegion(key);
      hist.PrintI64("lower_bound", bucket_bounds_[i] + offset);
      hist.PrintI64("upper_bound",
                    (i == buckets_size_ - 1 ? bucket_bounds_[i]
                                            : bucket_bounds_[i + 1] - 1) +
                        offset);
      hist.PrintI64("value", h[i]);
    }
  }

  void PrintLifetimeHisto(PbtxtRegion& hpaa, LifetimeHisto h,
                          absl::string_view key) {
    for (size_t i = 0; i < kLifetimeBuckets; ++i) {
      if (h[i] == 0) continue;
      auto hist = hpaa.CreateSubRegion(key);
      hist.PrintI64("lower_bound", lifetime_bucket_bounds_[i]);
      hist.PrintI64("upper_bound", (i == kLifetimeBuckets - 1
                                        ? lifetime_bucket_bounds_[i]
                                        : lifetime_bucket_bounds_[i + 1]));
      hist.PrintI64("value", h[i]);
    }
  }

  void PrintSampledTrackers(PbtxtRegion& hpaa, Type type, absl::string_view key,
                            UsageInfoRecords& records) {
    for (size_t i = 0; i < kMaxSampledTrackers; ++i) {
      if (records.sampled_trackers[i].is_valid) {
        auto sampled_tracker = hpaa.CreateSubRegion(key);
        sampled_tracker.PrintI64("allocations",
                                 records.sampled_trackers[i].allocations);
        sampled_tracker.PrintI64(
            "longest_free_range",
            records.sampled_trackers[i].longest_free_range.raw_num());
        sampled_tracker.PrintI64("objects",
                                 records.sampled_trackers[i].objects);
        sampled_tracker.PrintBool(
            "is_hugepage_backed",
            records.sampled_trackers[i].is_hugepage_backed);
        sampled_tracker.PrintBool("density",
                                  records.sampled_trackers[i].density);
        sampled_tracker.PrintDouble(
            "reallocation_time_sec",
            records.sampled_trackers[i].reallocation_time);
        records.sampled_trackers[i].is_valid = false;
      }
    }
  }

  absl::string_view TypeToStr(Type type) const {
    TC_ASSERT_LT(type, kNumTypes);
    switch (type) {
      case kSparseRegular:
        return "sparsely-accessed regular";
      case kDenseRegular:
        return "densely-accessed regular";
      case kDonated:
        return "donated";
      case kSparsePartialReleased:
        return "sparsely-accessed partial released";
      case kDensePartialReleased:
        return "densely-accessed partial released";
      case kSparseReleased:
        return "sparsely-accessed released";
      case kDenseReleased:
        return "densely-accessed released";
      default:
        TC_BUG("bad type %v", type);
    }
  }

  absl::string_view AllocType(Type type) const {
    TC_ASSERT_LT(type, kNumTypes);
    switch (type) {
      case kSparseRegular:
      case kDenseRegular:
        return "REGULAR";
      case kDonated:
        return "DONATED";
      case kSparsePartialReleased:
      case kDensePartialReleased:
        return "PARTIAL";
      case kSparseReleased:
      case kDenseReleased:
        return "RELEASED";
      default:
        TC_BUG("bad type %v", type);
    }
  }

  absl::string_view ObjectType(Type type) const {
    TC_ASSERT_LT(type, kNumTypes);
    switch (type) {
      case kSparseRegular:
      case kDonated:
      case kSparsePartialReleased:
      case kSparseReleased:
        return "SPARSELY_ACCESSED";
      case kDenseRegular:
      case kDensePartialReleased:
      case kDenseReleased:
        return "DENSELY_ACCESSED";
      default:
        TC_BUG("bad type %v", type);
    }
  }

  // Arrays, because they are split per alloc type.
  size_t bucket_bounds_[kBucketCapacity];
  size_t native_page_bucket_bounds_[kBucketCapacity];
  size_t lifetime_bucket_bounds_[kLifetimeBuckets + 1];
  size_t hugepage_backed_previously_released_ = 0;
  int buckets_size_ = 0;
  int native_page_buckets_size_ = 0;
};
}  // namespace huge_page_filler_internal

// This tracks a set of unfilled hugepages, and fulfills allocations
// with a goal of filling some hugepages as tightly as possible and emptying
// out the remainder.
template <class TrackerType>
class HugePageFiller {
 public:
  explicit HugePageFiller(
      MemoryTag tag, MemoryModifyFunction& unback ABSL_ATTRIBUTE_LIFETIME_BOUND,
      MemoryModifyFunction& unback_without_lock ABSL_ATTRIBUTE_LIFETIME_BOUND,
      MemoryModifyFunction& collapse ABSL_ATTRIBUTE_LIFETIME_BOUND,
      MemoryTagFunction& set_anon_vma_name ABSL_ATTRIBUTE_LIFETIME_BOUND,
      SubreleaseUnbackedMode subrelease_unbacked_mode);

  HugePageFiller(
      Clock clock, MemoryTag tag,
      MemoryModifyFunction& unback ABSL_ATTRIBUTE_LIFETIME_BOUND,
      MemoryModifyFunction& unback_without_lock ABSL_ATTRIBUTE_LIFETIME_BOUND,
      MemoryModifyFunction& collapse ABSL_ATTRIBUTE_LIFETIME_BOUND,
      MemoryTagFunction& set_anon_vma_name ABSL_ATTRIBUTE_LIFETIME_BOUND,
      SubreleaseUnbackedMode subrelease_unbacked_mode);

  typedef TrackerType Tracker;

  struct TryGetResult {
    TrackerType* absl_nullable pt;
    PageId page;
    bool from_released;
  };

  // Our API is simple, but note that it does not include an unconditional
  // allocation, only a "try"; we expect callers to allocate new hugepages if
  // needed.  This simplifies using it in a few different contexts (and improves
  // the testing story - no dependencies.)
  //
  // n is the number of TCMalloc pages to be allocated.  num_objects is the
  // number of individual objects that would be allocated on these n pages.
  //
  // On failure, returns nullptr/PageId{0}.
  TryGetResult TryGet(Length n, SpanAllocInfo span_alloc_info)
      ABSL_EXCLUSIVE_LOCKS_REQUIRED(pageheap_lock);

  // Marks r as usable by new allocations into *pt; returns pt if that hugepage
  // is now empty (nullptr otherwise.)
  //
  // REQUIRES: pt is owned by this object (has been Contribute()), and
  // {pt, Range{p, n}} was the result of a previous TryGet.
  TrackerType* absl_nullable Put(TrackerType* absl_nonnull pt, Range r,
                                 SpanAllocInfo span_alloc_info)
      ABSL_EXCLUSIVE_LOCKS_REQUIRED(pageheap_lock);

  // Contributes a tracker to the filler. If "donated," then the tracker is
  // marked as having come from the tail of a multi-hugepage allocation, which
  // causes it to be treated slightly differently.
  void Contribute(TrackerType* absl_nonnull pt TCMALLOC_CAPTURED_BY_THIS,
                  bool donated, SpanAllocInfo span_alloc_info);

  TrackerType* absl_nullable FetchFullyFreedTracker()
      ABSL_EXCLUSIVE_LOCKS_REQUIRED(pageheap_lock);

  HugeLength size() const { return size_; }

  // Useful statistics
  Length pages_allocated(AccessDensityPrediction type) const {
    TC_ASSERT_LT(type, AccessDensityPrediction::kPredictionCounts);
    return pages_allocated_[type];
  }
  Length pages_allocated() const {
    return pages_allocated_[AccessDensityPrediction::kSparse] +
           pages_allocated_[AccessDensityPrediction::kDense];
  }
  Length used_pages() const { return pages_allocated(); }
  Length unmapped_pages() const { return unmapped_; }
  Length free_pages() const;
  Length used_pages_in_released() const {
    TC_ASSERT_LE(n_used_released_[AccessDensityPrediction::kSparse],
                 regular_alloc_released_[AccessDensityPrediction::kSparse]
                     .size()
                     .in_pages());
    TC_ASSERT_LE(n_used_released_[AccessDensityPrediction::kDense],
                 regular_alloc_released_[AccessDensityPrediction::kDense]
                     .size()
                     .in_pages());
    return n_used_released_[AccessDensityPrediction::kDense] +
           n_used_released_[AccessDensityPrediction::kSparse];
  }
  Length used_pages_in_partial_released() const {
    TC_ASSERT_LE(
        n_used_partial_released_[AccessDensityPrediction::kSparse],
        regular_alloc_partial_released_[AccessDensityPrediction::kSparse]
            .size()
            .in_pages());
    TC_ASSERT_LE(
        n_used_partial_released_[AccessDensityPrediction::kDense],
        regular_alloc_partial_released_[AccessDensityPrediction::kDense]
            .size()
            .in_pages());
    return n_used_partial_released_[AccessDensityPrediction::kDense] +
           n_used_partial_released_[AccessDensityPrediction::kSparse];
  }
  Length used_pages_in_any_subreleased() const {
    return used_pages_in_released() + used_pages_in_partial_released();
  }

  HugeLength previously_released_huge_pages() const {
    return n_was_released_[AccessDensityPrediction::kDense] +
           n_was_released_[AccessDensityPrediction::kSparse];
  }

  Length FreePagesInPartialAllocs() const;

  // Fraction of used pages that are on non-released hugepages and
  // thus could be backed by kernel hugepages. (Of course, we can't
  // guarantee that the kernel had available 2-mib regions of physical
  // memory--so this being 1 doesn't mean that everything actually
  // *is* hugepage-backed!)
  double hugepage_frac() const;

  // Returns the amount of memory to release if all remaining options of
  // releasing memory involve subreleasing pages. Provided intervals are used
  // for making skip subrelease decisions.
  Length GetDesiredSubreleasePages(Length desired, Length total_released,
                                   SkipSubreleaseIntervals intervals)
      ABSL_EXCLUSIVE_LOCKS_REQUIRED(pageheap_lock);

  // Tries to release desired pages by iteratively releasing from the emptiest
  // possible hugepage and releasing its free memory to the system. If
  // release_partial_alloc_pages is enabled, it also releases all the free
  // pages from the partial allocs. Note that the number of pages released may
  // be greater than the desired number of pages.
  // Returns the number of pages actually released. The releasing target can be
  // reduced by skip subrelease which is disabled if all intervals are zero.
  static constexpr double kPartialAllocPagesRelease = 0.1;
  Length ReleasePages(Length desired, SkipSubreleaseIntervals intervals,
                      bool release_partial_alloc_pages, bool hit_limit)
      ABSL_EXCLUSIVE_LOCKS_REQUIRED(pageheap_lock);
  // Number of candidate hugepages selected in each iteration for releasing
  // their free memory.
  static constexpr size_t kCandidatesForReleasingMemory =
      kPagesPerHugePage.raw_num();

  void AddSpanStats(SmallSpanStats* small, LargeSpanStats* large) const;

  BackingStats stats() const;
  SubreleaseStats subrelease_stats() const { return subrelease_stats_; }
  HugePageTreatmentStats GetHugePageTreatmentStats() const
      ABSL_EXCLUSIVE_LOCKS_REQUIRED(pageheap_lock) {
    return treatment_stats_;
  };

  HugePageFillerStats GetStats() const;
  void Print(Printer& out, bool everything, PageFlagsBase& pageflags)
      ABSL_EXCLUSIVE_LOCKS_REQUIRED(pageheap_lock);
  void PrintInPbtxt(PbtxtRegion& hpaa, PageFlagsBase& pageflags)
      ABSL_EXCLUSIVE_LOCKS_REQUIRED(pageheap_lock);

  template <typename F>
  void ForEachHugePage(const F& func)
      ABSL_EXCLUSIVE_LOCKS_REQUIRED(pageheap_lock);

  static constexpr int kMaxBackoffDelay = 128;

  // Returns true if we should back off from MADV_COLLAPSE. In case of high
  // collapse latency, this is used to reduce the frequency of collapse
  // attempts.
  bool ShouldBackoffFromCollapse() ABSL_EXCLUSIVE_LOCKS_REQUIRED(pageheap_lock);

  // Based on the <latency>, updates the max backoff delay.
  void UpdateMaxBackoffDelay(absl::Duration latency)
      ABSL_EXCLUSIVE_LOCKS_REQUIRED(pageheap_lock);

  // Iterates through all hugepage trackers and applies different treatments.
  // Treatments applied include:
  // 1. Attempt to collapse eligible memory into hugepages if
  // <enable_collapse> is true. It uses heuristics to determine eligibility of
  // the pages for collapse. It
  // * Attempts to collapse up to kTotalTrackersToScan trackers.
  // * Collapses pages with less than kMaxSwappedPagesForCollapse swapped
  //   pages and kMaxUnbackedPagesForCollapse unbacked pages.
  // 2. Periodically set a name for the allocated region tracked by sampled
  // trackers. Every iteration, it scans up to 64 sampled trackers, records
  // features such as longest free range, nallocs, etc. and encodes them into a
  // string that is used for naming the region. Once set, the tracker is
  // revisited only after five minutes.
  // 3. Attempt to release free/unreleased pages from trackers with a swapped
  // page.
  void TreatHugepageTrackers(
      EnableCollapse enable_collapse,
      EnableUnfilteredCollapse enable_unfiltered_collapse,
      ReleaseStalePages release_stale_pages, PageFlagsBase* pageflags = nullptr,
      Residency* residency = nullptr)
      ABSL_EXCLUSIVE_LOCKS_REQUIRED(pageheap_lock);

  // Utility function to release free pages from a given `page_tracker`
  // and handle accounting.
  Length HandleReleaseFree(PageTracker* page_tracker)
      ABSL_EXCLUSIVE_LOCKS_REQUIRED(pageheap_lock);

  void OnCollapseSuccess(TrackerType* absl_nonnull pt)
      ABSL_EXCLUSIVE_LOCKS_REQUIRED(pageheap_lock);

  // Utility function to handle a non-hugepage backed `page_tracker` and
  // mark its unmapped pages appropriately.
  Length HandleUnbackedHugePage(PageTracker* page_tracker, PageBitmap unbacked)
      ABSL_EXCLUSIVE_LOCKS_REQUIRED(pageheap_lock);

 private:
  // This class wraps an array of N TrackerLists and a Bitmap storing which
  // elements are non-empty.
  template <size_t N>
  class PageTrackerLists : public HintedTrackerLists<TrackerType, N> {
   public:
    HugeLength size() const {
      return NHugePages(HintedTrackerLists<TrackerType, N>::size());
    }
  };

  SubreleaseStats subrelease_stats_;

  // We group hugepages first by longest-free (as a measure of fragmentation),
  // then into kChunks chunks inside there by desirability of
  // allocation.
  static constexpr size_t kChunks = 8;
  // Which chunk should this hugepage be in?
  // This returns the largest possible value kChunks - 1 iff
  // pt has a single allocation.
  size_t IndexFor(const TrackerType& pt) const;
  // Returns index for the list where hugepages with at least one free range of
  // pages whose length is at least as much as "longest".
  size_t ListFor(Length longest, size_t chunk, AccessDensityPrediction density,
                 size_t nallocs) const;
  // Returns index for sparse alloclists.
  size_t SparseListFor(Length longest, size_t chunk) const;
  // Returns index for dense alloclists.
  size_t DenseListFor(size_t chunk, size_t nallocs) const;
  static constexpr size_t kNumLists = kPagesPerHugePage.raw_num() * kChunks;

  // List of hugepages from which no pages have been released to the OS.
  PageTrackerLists<kNumLists>
      regular_alloc_[AccessDensityPrediction::kPredictionCounts];
  PageTrackerLists<kPagesPerHugePage.raw_num()> donated_alloc_;
  // Partially released ones that we are trying to release.
  //
  // regular_alloc_partial_released_ contains huge pages that are partially
  // allocated, partially free, and partially returned to the OS.
  //
  // regular_alloc_released_:  This list contains huge pages whose pages are
  // either allocated or returned to the OS.  There are no pages that are free,
  // but not returned to the OS.
  PageTrackerLists<kNumLists> regular_alloc_partial_released_
      [AccessDensityPrediction::kPredictionCounts];
  PageTrackerLists<kNumLists>
      regular_alloc_released_[AccessDensityPrediction::kPredictionCounts];

  // Records a list of fully freed trackers. We might end up with trackers that
  // are fully freed, but not deleted, when: the trackers are being userspace-
  // collapsed, and an intermediate Put operation deallocates all the pages
  // in the tracker. The list temporarily holds these trackers before they are
  // deleted, once the collapse operation completes.
  TList<TrackerType> fully_freed_trackers_;

  HugePageTreatmentStats treatment_stats_ ABSL_GUARDED_BY(pageheap_lock);

  // n_used_released_ contains the number of pages in huge pages that are not
  // free (i.e., allocated).  Only the hugepages in regular_alloc_released_ are
  // considered.
  Length n_used_released_[AccessDensityPrediction::kPredictionCounts];

  HugeLength n_was_released_[AccessDensityPrediction::kPredictionCounts];
  // n_used_partial_released_ is the number of pages which have been allocated
  // from the hugepages in the set regular_alloc_partial_released.
  Length n_used_partial_released_[AccessDensityPrediction::kPredictionCounts];

  // RemoveFromFillerList pt from the appropriate PageTrackerList.
  void RemoveFromFillerList(TrackerType* absl_nonnull pt);
  // Put pt in the appropriate PageTrackerList.
  void AddToFillerList(TrackerType* absl_nonnull pt);
  // Like AddToFillerList(), but for use when donating from the tail of a
  // multi-hugepage allocation.
  void DonateToFillerList(TrackerType* absl_nonnull pt);

  void PrintAllocStatsInPbtxt(absl::string_view field, PbtxtRegion& hpaa,
                              const HugePageFillerStats& stats,
                              AccessDensityPrediction count) const;

  static constexpr size_t kLifetimeBuckets =
      huge_page_filler_internal::UsageInfo::kLifetimeBuckets;
  using LifetimeHisto = huge_page_filler_internal::UsageInfo::LifetimeHisto;
  void RecordLifetime(const TrackerType* pt);
  void PrintLifetimeHisto(Printer& out, LifetimeHisto h,
                          AccessDensityPrediction type,
                          absl::string_view blurb) const;
  void PrintLifetimeHistoInPbtxt(PbtxtRegion& hpaa, LifetimeHisto h,
                                 absl::string_view key);

  int LifetimeBucketNum(absl::Duration duration) {
    int64_t duration_ms = absl::ToInt64Milliseconds(duration);
    auto it = std::upper_bound(lifetime_bucket_bounds_,
                               lifetime_bucket_bounds_ + kLifetimeBuckets,
                               duration_ms);
    TC_CHECK_NE(it, lifetime_bucket_bounds_);
    return it - lifetime_bucket_bounds_ - 1;
  }

  // CompareForSubrelease identifies the worse candidate for subrelease, between
  // the choice of huge pages a and b.
  static bool CompareForSubrelease(const TrackerType* absl_nonnull a,
                                   const TrackerType* absl_nonnull b) {
    TC_ASSERT_NE(a, nullptr);
    TC_ASSERT_NE(b, nullptr);

    if (a->used_pages() < b->used_pages()) return true;
    if (a->used_pages() > b->used_pages()) return false;
    // If 'a' has dense spans, then we do not prefer to release from 'a'
    // compared to 'b'.
    if (a->HasDenseSpans()) return false;
    // We know 'a' does not have dense spans.  If 'b' has dense spans, then we
    // prefer to release from 'a'.  Otherwise, we do not prefer either.
    return b->HasDenseSpans();
  }

  // SelectCandidates identifies the candidates.size() best candidates in the
  // given tracker list.
  //
  // To support gathering candidates from multiple tracker lists,
  // current_candidates is nonzero.
  template <size_t N>
  static int SelectCandidates(absl::Span<TrackerType*> candidates,
                              int current_candidates,
                              const PageTrackerLists<N>& tracker_list,
                              size_t tracker_start);

  // Release desired pages from the page trackers in candidates.  Returns the
  // number of pages released.
  Length ReleaseCandidates(absl::Span<TrackerType* absl_nonnull> candidates,
                           Length target)
      ABSL_EXCLUSIVE_LOCKS_REQUIRED(pageheap_lock);

  HugeLength size_;

  Length pages_allocated_[AccessDensityPrediction::kPredictionCounts];
  Length unmapped_;

  // How much have we eagerly unmapped (in already released hugepages), but
  // not reported to ReleasePages calls?
  Length unmapping_unaccounted_;

  // Functionality related to time series tracking, using 3600 slots to record
  // at least 60-mins demand history (maximumly using 1 slot every second).
  void UpdateFillerStatsTracker();
  using StatsTrackerType = SubreleaseStatsTracker<3600>;
  StatsTrackerType fillerstats_tracker_;

  // Lifetime tracking for completely-freed hugepages
  LifetimeHisto lifetime_histo_[AccessDensityPrediction::kPredictionCounts]{};
  size_t lifetime_bucket_bounds_[kLifetimeBuckets + 1];

  Clock clock_;
  const MemoryTag tag_;
  // TODO(b/73749855):  Remove remaining uses of unback_.
  MemoryModifyFunction& unback_;
  MemoryModifyFunction& unback_without_lock_;
  MemoryModifyFunction& collapse_;
  MemoryTagFunction& set_anon_vma_name_;
  int max_backoff_delay_ ABSL_GUARDED_BY(pageheap_lock) = 1;
  int current_backoff_delay_ ABSL_GUARDED_BY(pageheap_lock) = 0;
  uintptr_t rng_ = 0;
  SubreleaseUnbackedMode subrelease_unbacked_mode_;
};

template <class TrackerType>
inline HugePageFiller<TrackerType>::HugePageFiller(
    MemoryTag tag, MemoryModifyFunction& unback,
    MemoryModifyFunction& unback_without_lock, MemoryModifyFunction& collapse,
    MemoryTagFunction& set_anon_vma_name,
    SubreleaseUnbackedMode subrelease_unbacked_mode)
    : HugePageFiller(Clock{.now = absl::base_internal::CycleClock::Now,
                           .freq = absl::base_internal::CycleClock::Frequency},
                     tag, unback, unback_without_lock, collapse,
                     set_anon_vma_name, subrelease_unbacked_mode) {}

// For testing with mock clock
template <class TrackerType>
inline HugePageFiller<TrackerType>::HugePageFiller(
    Clock clock, MemoryTag tag, MemoryModifyFunction& unback,
    MemoryModifyFunction& unback_without_lock, MemoryModifyFunction& collapse,
    MemoryTagFunction& set_anon_vma_name,
    SubreleaseUnbackedMode subrelease_unbacked_mode)
    : size_(NHugePages(0)),
      fillerstats_tracker_(clock, absl::Minutes(60), absl::Minutes(5),
                           absl::Minutes(10)),
      clock_(clock),
      tag_(tag),
      unback_(unback),
      unback_without_lock_(unback_without_lock),
      collapse_(collapse),
      set_anon_vma_name_(set_anon_vma_name),
      subrelease_unbacked_mode_(subrelease_unbacked_mode) {
  lifetime_bucket_bounds_[0] = 0;
  lifetime_bucket_bounds_[1] = 1;
  for (int i = 2; i <= kLifetimeBuckets; ++i) {
    lifetime_bucket_bounds_[i] = lifetime_bucket_bounds_[i - 1] * 10;
  }
}

template <class TrackerType>
inline typename HugePageFiller<TrackerType>::TryGetResult
HugePageFiller<TrackerType>::TryGet(Length n, SpanAllocInfo span_alloc_info) {
  TC_ASSERT_GT(n, Length(0));
  TC_ASSERT(span_alloc_info.density == AccessDensityPrediction::kSparse ||
            n == Length(1));

  // How do we choose which hugepage to allocate from (among those with
  // a free range of at least n?) Our goal is to be as space-efficient
  // as possible, which leads to two priorities:
  //
  // (1) avoid fragmentation; keep free ranges in a hugepage as long
  //     as possible. This maintains our ability to satisfy large
  //     requests without allocating new hugepages
  // (2) fill mostly-full hugepages more; let mostly-empty hugepages
  //     empty out.  This lets us recover totally empty hugepages (and
  //     return them to the OS.)
  //
  // In practice, avoiding fragmentation is by far more important:
  // space usage can explode if we don't zealously guard large free ranges.
  //
  // Our primary measure of fragmentation of a hugepage by a proxy measure: the
  // longest free range it contains. If this is short, any free space is
  // probably fairly fragmented.  It also allows us to instantly know if a
  // hugepage can support a given allocation.
  //
  // We quantize the number of allocations in a hugepage (chunked
  // logarithmically.) We favor allocating from hugepages with many allocations
  // already present, which helps with (2) above. Note that using the number of
  // allocations works substantially better than the number of allocated pages;
  // to first order allocations of any size are about as likely to be freed, and
  // so (by simple binomial probability distributions) we're more likely to
  // empty out a hugepage with 2 5-page allocations than one with 5 1-pages.
  //
  // The above suggests using the hugepage with the shortest longest empty
  // range, breaking ties in favor of fewest number of allocations. This works
  // well for most workloads but caused bad page heap fragmentation for some:
  // b/63301358 and b/138618726. The intuition for what went wrong is
  // that although the tail of large allocations is donated to the Filler (see
  // HugePageAwareAllocator::AllocRawHugepages) for use, we don't actually
  // want to use them until the regular Filler hugepages are used up. That
  // way, they can be reassembled as a single large hugepage range if the
  // large allocation is freed.
  // Some workloads can tickle this discrepancy a lot, because they have a lot
  // of large, medium-lifetime allocations. To fix this we treat hugepages
  // that are freshly donated as less preferable than hugepages that have been
  // already used for small allocations, regardless of their longest_free_range.
  //
  // Overall our allocation preference is:
  //  - We prefer allocating from used freelists rather than freshly donated
  //  - We prefer donated pages over previously released hugepages ones.
  //  - Among donated freelists we prefer smaller longest_free_range
  //  - Among used freelists we prefer smaller longest_free_range
  //    with ties broken by (quantized) alloc counts
  //
  // We group hugepages by longest_free_range and quantized alloc count and
  // store each group in a TrackerList. All freshly-donated groups are stored
  // in a "donated" array and the groups with (possibly prior) small allocs are
  // stored in a "regular" array. Each of these arrays is encapsulated in a
  // PageTrackerLists object, which stores the array together with a bitmap to
  // quickly find non-empty lists. The lists are ordered to satisfy the
  // following two useful properties:
  //
  // - later (nonempty) freelists can always fulfill requests that
  //   earlier ones could.
  // - earlier freelists, by the above criteria, are preferred targets
  //   for allocation.
  //
  // So all we have to do is find the first nonempty freelist in the regular
  // PageTrackerList that *could* support our allocation, and it will be our
  // best choice. If there is none we repeat with the donated PageTrackerList.
  ASSUME(n < kPagesPerHugePage);
  TrackerType* pt;

  bool was_released = false;
  const AccessDensityPrediction type = span_alloc_info.density;
  do {
    const size_t listindex =
        ListFor(n, 0, type, kPagesPerHugePage.raw_num() - 1);
    pt = regular_alloc_[type].GetLeast(listindex);
    if (pt) {
      TC_ASSERT(!pt->donated());
      break;
    }
    if (ABSL_PREDICT_TRUE(type == AccessDensityPrediction::kSparse)) {
      pt = donated_alloc_.GetLeast(n.raw_num());
      if (pt) {
        break;
      }
    }
    pt = regular_alloc_partial_released_[type].GetLeast(listindex);
    if (pt) {
      TC_ASSERT(!pt->donated());
      was_released = true;
      TC_ASSERT_GE(n_used_partial_released_[type], pt->used_pages());
      n_used_partial_released_[type] -= pt->used_pages();
      break;
    }
    pt = regular_alloc_released_[type].GetLeast(listindex);
    if (pt) {
      TC_ASSERT(!pt->donated());
      was_released = true;
      TC_ASSERT_GE(n_used_released_[type], pt->used_pages());
      n_used_released_[type] -= pt->used_pages();
      break;
    }

    return {nullptr, PageId{0}, false};
  } while (false);
  ASSUME(pt != nullptr);
  TC_ASSERT_GE(pt->longest_free_range(), n);
  // type == AccessDensityPrediction::kDense => pt->HasDenseSpans(). This
  // also verifies we do not end up with a donated pt on the kDense path.
  TC_ASSERT(type == AccessDensityPrediction::kSparse || pt->HasDenseSpans());

  // Log previous features before modifying the page tracker.
  const auto now = clock_.now();
  if (pt->GetTagState().sampled_for_tagging) {
    pt->RecordFeatures();
  }
  pt->SetLastAllocationTime(now);
  const auto page_allocation = pt->Get(n, span_alloc_info);
  AddToFillerList(pt);
  pages_allocated_[type] += n;

  // If it was in a released state earlier, and is about to be full again,
  // record that the state has been toggled back and update the stat counter.
  if (was_released && !pt->released() && !pt->was_released()) {
    pt->set_was_released(/*status=*/true);
    ++n_was_released_[type];
  }
  TC_ASSERT(was_released || page_allocation.previously_unbacked == Length(0));
  TC_ASSERT_GE(unmapped_, page_allocation.previously_unbacked);
  unmapped_ -= page_allocation.previously_unbacked;
  // We're being used for an allocation, so we are no longer considered
  // donated by this point.
  TC_ASSERT(!pt->donated());
  UpdateFillerStatsTracker();
  return {pt, page_allocation.page, was_released};
}

template <class TrackerType>
void HugePageFiller<TrackerType>::RecordLifetime(const TrackerType* pt) {
  const double now = clock_.now();
  const double frequency = clock_.freq();
  const double elapsed = std::max<double>(now - pt->alloctime(), 0);
  const absl::Duration lifetime =
      absl::Milliseconds(elapsed * 1000 / frequency);
  if (pt->HasDenseSpans()) {
    ++lifetime_histo_[AccessDensityPrediction::kDense]
                     [LifetimeBucketNum(lifetime)];
  } else {
    ++lifetime_histo_[AccessDensityPrediction::kSparse]
                     [LifetimeBucketNum(lifetime)];
  }
}

template <class TrackerType>
void HugePageFiller<TrackerType>::PrintLifetimeHisto(
    Printer& out, LifetimeHisto h, AccessDensityPrediction type,
    absl::string_view blurb) const {
  absl::string_view typestring = type == AccessDensityPrediction::kDense
                                     ? "densely-accessed"
                                     : "sparsely-accessed";
  out.printf("\nHugePageFiller: # of %s %s", typestring, blurb);
  for (size_t i = 0; i < kLifetimeBuckets; ++i) {
    if (i % 6 == 0) {
      out.printf("\nHugePageFiller:");
    }
    out.printf(" < %3zu ms <= %6zu", lifetime_bucket_bounds_[i], h[i]);
  }
  out.printf("\n");
}

template <class TrackerType>
void HugePageFiller<TrackerType>::PrintLifetimeHistoInPbtxt(
    PbtxtRegion& hpaa, LifetimeHisto h, absl::string_view key) {
  for (size_t i = 0; i < kLifetimeBuckets; ++i) {
    if (h[i] == 0) continue;
    auto hist = hpaa.CreateSubRegion(key);
    hist.PrintI64("lower_bound", lifetime_bucket_bounds_[i]);
    hist.PrintI64("upper_bound",
                  (i == kLifetimeBuckets - 1 ? lifetime_bucket_bounds_[i]
                                             : lifetime_bucket_bounds_[i + 1]));
    hist.PrintI64("value", h[i]);
  }
}

// Marks r as usable by new allocations into *pt; returns pt if that hugepage is
// now empty (nullptr otherwise.)
//
// REQUIRES: pt is owned by this object (has been Contribute()), and {pt,
// Range(p, n)} was the result of a previous TryGet.
template <class TrackerType>
inline TrackerType* HugePageFiller<TrackerType>::Put(
    TrackerType* pt, Range r, SpanAllocInfo span_alloc_info) {
  RemoveFromFillerList(pt);
  pt->Put(r, span_alloc_info);
  if (pt->HasDenseSpans()) {
    TC_ASSERT_GE(pages_allocated_[AccessDensityPrediction::kDense], r.n);
    pages_allocated_[AccessDensityPrediction::kDense] -= r.n;
  } else {
    TC_ASSERT_GE(pages_allocated_[AccessDensityPrediction::kSparse], r.n);
    pages_allocated_[AccessDensityPrediction::kSparse] -= r.n;
  }

  if (pt->longest_free_range() == kPagesPerHugePage) {
    TC_ASSERT_EQ(pt->nallocs(), 0);
    --size_;
    if (pt->released()) {
      const Length free_pages = pt->free_pages();
      const Length released_pages = pt->released_pages();
      TC_ASSERT_GE(free_pages, released_pages);
      TC_ASSERT_GE(unmapped_, released_pages);
      unmapped_ -= released_pages;

      if (free_pages > released_pages) {
        // pt is partially released.  As the rest of the hugepage-aware
        // allocator works in terms of whole hugepages, we need to release the
        // rest of the hugepage.  This simplifies subsequent accounting by
        // allowing us to work with hugepage-granularity, rather than needing to
        // retain pt's state indefinitely.
        bool success =
            unback_without_lock_(HugeRange(pt->location(), NHugePages(1)))
                .success;

        if (ABSL_PREDICT_TRUE(success)) {
          unmapping_unaccounted_ += free_pages - released_pages;
        }
      }
    }

    if (pt->was_released()) {
      pt->set_was_released(/*status=*/false);
      if (pt->HasDenseSpans()) {
        --n_was_released_[AccessDensityPrediction::kDense];
      } else {
        --n_was_released_[AccessDensityPrediction::kSparse];
      }
    }

    if (!pt->DontFreeTracker()) {
      RecordLifetime(pt);
      UpdateFillerStatsTracker();
      if (pt->GetTagState().sampled_for_tagging) {
        // Set the default region name if the tracked was sampled.
        pt->SetAnonVmaName(set_anon_vma_name_, /*name=*/std::nullopt);
      }
      return pt;
    }
  }
  AddToFillerList(pt);
  UpdateFillerStatsTracker();
  return nullptr;
}

template <class TrackerType>
inline void HugePageFiller<TrackerType>::Contribute(
    TrackerType* pt, bool donated, SpanAllocInfo span_alloc_info) {
  // A contributed huge page should not yet be subreleased.
  TC_ASSERT_EQ(pt->released_pages(), Length(0));

  const AccessDensityPrediction type = span_alloc_info.density;

  // Decide whether to sample this tracker for tagging.
  rng_ = ExponentialBiased::NextRandom(rng_);
  pt->SetTagState({.sampled_for_tagging = (rng_ % 100 == 0)});

  pages_allocated_[type] += pt->used_pages();
  TC_ASSERT(!(type == AccessDensityPrediction::kDense && donated));
  if (donated) {
    TC_ASSERT(pt->was_donated());
    DonateToFillerList(pt);
  } else {
    if (type == AccessDensityPrediction::kDense) {
      pt->SetHasDenseSpans();
    }
    AddToFillerList(pt);
  }

  ++size_;
  UpdateFillerStatsTracker();
}

template <class TrackerType>
template <size_t N>
inline int HugePageFiller<TrackerType>::SelectCandidates(
    absl::Span<TrackerType*> candidates, int current_candidates,
    const PageTrackerLists<N>& tracker_list, size_t tracker_start) {
  auto PushCandidate = [&](TrackerType& pt) GOOGLE_MALLOC_SECTION {
    TC_ASSERT_GT(pt.free_pages(), Length(0));
    TC_ASSERT_GT(pt.free_pages(), pt.released_pages());

    // If the tracker is being collapsed, don't release it. Collapse might race
    // with the release, and we might collapse the pages that have been recently
    // released.
    if (pt.BeingCollapsed()) return;

    // If we have few candidates, we can avoid creating a heap.
    //
    // In ReleaseCandidates(), we unconditionally sort the list and linearly
    // iterate through it--rather than pop_heap repeatedly--so we only need the
    // heap for creating a bounded-size priority queue.
    if (current_candidates < candidates.size()) {
      candidates[current_candidates] = &pt;
      current_candidates++;

      if (current_candidates == candidates.size()) {
        std::make_heap(candidates.begin(), candidates.end(),
                       CompareForSubrelease);
      }
      return;
    }

    // Consider popping the worst candidate from our list.
    if (CompareForSubrelease(candidates[0], &pt)) {
      // pt is worse than the current worst.
      return;
    }

    std::pop_heap(candidates.begin(), candidates.begin() + current_candidates,
                  CompareForSubrelease);
    candidates[current_candidates - 1] = &pt;
    std::push_heap(candidates.begin(), candidates.begin() + current_candidates,
                   CompareForSubrelease);
  };

  tracker_list.Iter(PushCandidate, tracker_start);

  return current_candidates;
}

template <class TrackerType>
inline Length HugePageFiller<TrackerType>::ReleaseCandidates(
    absl::Span<TrackerType*> candidates, Length target) {
  absl::c_sort(candidates, CompareForSubrelease);

  Length total_released;
  HugeLength total_broken = NHugePages(0);
#ifndef NDEBUG
  Length last;
#endif
  for (int i = 0; i < candidates.size() && total_released < target; i++) {
    TrackerType* best = candidates[i];
    TC_ASSERT_NE(best, nullptr);

    // Verify that we have pages that we can release.
    TC_ASSERT_NE(best->free_pages(), Length(0));
    // TODO(b/73749855):  This assertion may need to be relaxed if we release
    // the pageheap_lock here.  A candidate could change state with another
    // thread while we have the lock released for another candidate.
    TC_ASSERT_GT(best->free_pages(), best->released_pages());

#ifndef NDEBUG
    // Double check that our sorting criteria were applied correctly.
    TC_ASSERT_LE(last, best->used_pages());
    last = best->used_pages();
#endif

    if (best->unbroken()) {
      ++total_broken;
    }
    RemoveFromFillerList(best);
    Length ret = best->ReleaseFree(unback_);
    unmapped_ += ret;
    TC_ASSERT_GE(unmapped_, best->released_pages());
    total_released += ret;
    AddToFillerList(best);
    // If the candidate we just released from previously had was_released set,
    // clear it. was_released is tracked only for pages that aren't in
    // released state.
    if (best->was_released() && best->released()) {
      best->set_was_released(/*status=*/false);
      if (best->HasDenseSpans()) {
        --n_was_released_[AccessDensityPrediction::kDense];
      } else {
        --n_was_released_[AccessDensityPrediction::kSparse];
      }
    }
  }

  subrelease_stats_.num_pages_subreleased += total_released;
  subrelease_stats_.num_hugepages_broken += total_broken;

  // Keep separate stats if the on going release is triggered by reaching
  // tcmalloc limit
  if (subrelease_stats_.limit_hit()) {
    subrelease_stats_.total_pages_subreleased_due_to_limit += total_released;
    subrelease_stats_.total_hugepages_broken_due_to_limit += total_broken;
  }
  return total_released;
}

template <class TrackerType>
inline Length HugePageFiller<TrackerType>::FreePagesInPartialAllocs() const {
  return regular_alloc_partial_released_[AccessDensityPrediction::kSparse]
             .size()
             .in_pages() +
         regular_alloc_partial_released_[AccessDensityPrediction::kDense]
             .size()
             .in_pages() +
         regular_alloc_released_[AccessDensityPrediction::kSparse]
             .size()
             .in_pages() +
         regular_alloc_released_[AccessDensityPrediction::kDense]
             .size()
             .in_pages() -
         used_pages_in_any_subreleased() - unmapped_pages();
}

template <class TrackerType>
inline Length HugePageFiller<TrackerType>::GetDesiredSubreleasePages(
    Length desired, Length total_released, SkipSubreleaseIntervals intervals) {
  // Don't subrelease pages if it would push you under either the latest peak or
  // the sum of short-term demand fluctuation peak and long-term demand trend.
  // This is a bit subtle: We want the current *mapped* pages not to be below
  // the recent *demand* requirement, i.e., if we have a large amount of free
  // memory right now but demand is below the requirement, we still want to
  // subrelease.
  TC_ASSERT_LT(total_released, desired);
  if (!intervals.SkipSubreleaseEnabled()) {
    return desired;
  }
  UpdateFillerStatsTracker();
  Length required_pages;
  // As mentioned above, there are two ways to calculate the demand
  // requirement. We give priority to using the peak if peak_interval is set.
  if (intervals.IsPeakIntervalSet()) {
    required_pages =
        fillerstats_tracker_.GetRecentPeak(intervals.peak_interval);
  } else {
    required_pages = fillerstats_tracker_.GetRecentDemand(
        intervals.short_interval, intervals.long_interval);
  }

  Length current_pages = used_pages() + free_pages();

  if (required_pages != Length(0)) {
    Length new_desired;
    if (required_pages >= current_pages) {
      new_desired = total_released;
    } else {
      new_desired = total_released + (current_pages - required_pages);
    }

    if (new_desired >= desired) {
      return desired;
    }
    // Remaining target amount to release after applying skip subrelease. Note:
    // the remaining target should always be smaller or equal to the number of
    // free pages according to the mechanism (recent peak is always larger or
    // equal to current used_pages), however, we still calculate allowed release
    // using the minimum of the two to avoid relying on that assumption.
    Length releasable_pages =
        std::min(free_pages(), (new_desired - total_released));
    // Reports the amount of memory that we didn't release due to this
    // mechanism, but never more than skipped free pages. In other words,
    // skipped_pages is zero if all free pages are allowed to be released by
    // this mechanism. Note, only free pages in the smaller of the two
    // (current_pages and required_pages) are skipped, the rest are allowed to
    // be subreleased.
    Length skipped_pages =
        std::min((free_pages() - releasable_pages), (desired - new_desired));
    fillerstats_tracker_.ReportSkippedSubreleasePages(
        skipped_pages, std::min(current_pages, required_pages));
    return new_desired;
  }

  return desired;
}

// Tries to release desired pages by iteratively releasing from the emptiest
// possible hugepage and releasing its free memory to the system. Return the
// number of pages actually released.
template <class TrackerType>
inline Length HugePageFiller<TrackerType>::ReleasePages(
    Length desired, SkipSubreleaseIntervals intervals,
    bool release_partial_alloc_pages, bool hit_limit) {
  Length total_released;

  // If the feature to release all free pages in partially-released allocs is
  // enabled, we increase the desired number of pages below to the total number
  // of releasable pages in partially-released allocs. We disable this feature
  // for cases when hit_limit is set to true (i.e. when memory limit is hit).
  const bool release_all_from_partial_allocs =
      release_partial_alloc_pages && !hit_limit;
  if (ABSL_PREDICT_FALSE(release_all_from_partial_allocs)) {
    // If we have fewer than desired number of free pages in partial allocs, we
    // would try to release pages from full allocs as well (after we include
    // unaccounted unmapped pages and release from partial allocs). Else, we aim
    // to release up to the total number of free pages in partially-released
    // allocs.
    size_t from_partial_allocs =
        kPartialAllocPagesRelease * FreePagesInPartialAllocs().raw_num();
    desired = std::max(desired, Length(from_partial_allocs));
  }

  // We also do eager release, once we've called this at least once:
  // claim credit for anything that gets done.
  if (unmapping_unaccounted_.raw_num() > 0) {
    // TODO(ckennelly):  This may overshoot in releasing more than desired
    // pages.
    Length n = unmapping_unaccounted_;
    unmapping_unaccounted_ = Length(0);
    subrelease_stats_.num_pages_subreleased += n;
    total_released += n;
  }

  if (total_released >= desired) {
    return total_released;
  }

  // Only reduce desired if skip subrelease is on.
  //
  // Additionally, if we hit the limit, we should not be applying skip
  // subrelease.  OOM may be imminent.
  if (intervals.SkipSubreleaseEnabled() && !hit_limit) {
    desired = GetDesiredSubreleasePages(desired, total_released, intervals);
    if (desired <= total_released) {
      return total_released;
    }
  }

  subrelease_stats_.set_limit_hit(hit_limit);

  // Optimize for releasing up to a huge page worth of small pages (scattered
  // over many parts of the filler).  Since we hold pageheap_lock, we cannot
  // allocate here.
  using CandidateArray =
      std::array<TrackerType*, kCandidatesForReleasingMemory>;

  while (total_released < desired) {
    CandidateArray candidates;
    // We can skip the first kChunks lists as they are known
    // to be 100% full. (Those lists are likely to be long.)
    //
    // We do not examine the regular_alloc_released_ lists, as only contain
    // completely released pages.
    int n_candidates = SelectCandidates(
        absl::MakeSpan(candidates), 0,
        regular_alloc_partial_released_[AccessDensityPrediction::kSparse],
        kChunks);
    n_candidates = SelectCandidates(
        absl::MakeSpan(candidates), n_candidates,
        regular_alloc_partial_released_[AccessDensityPrediction::kDense],
        kChunks);

    Length released =
        ReleaseCandidates(absl::MakeSpan(candidates.data(), n_candidates),
                          desired - total_released);
    subrelease_stats_.num_partial_alloc_pages_subreleased += released;
    if (released == Length(0)) {
      break;
    }
    total_released += released;
  }

  // Only consider breaking up a hugepage if there are no partially released
  // pages.
  while (total_released < desired) {
    CandidateArray candidates;
    // TODO(b/199203282): revisit the order in which allocs are searched for
    // release candidates.
    //
    // We select candidate hugepages from few_objects_alloc_ first as we expect
    // hugepages in this alloc to become free earlier than those in other
    // allocs.
    int n_candidates = SelectCandidates(
        absl::MakeSpan(candidates), /*current_candidates=*/0,
        regular_alloc_[AccessDensityPrediction::kSparse], kChunks);
    n_candidates = SelectCandidates(
        absl::MakeSpan(candidates), n_candidates,
        regular_alloc_[AccessDensityPrediction::kDense], kChunks);
    // TODO(b/138864853): Perhaps remove donated_alloc_ from here, it's not a
    // great candidate for partial release.
    n_candidates = SelectCandidates(absl::MakeSpan(candidates), n_candidates,
                                    donated_alloc_, 0);

    Length released =
        ReleaseCandidates(absl::MakeSpan(candidates.data(), n_candidates),
                          desired - total_released);
    if (released == Length(0)) {
      break;
    }
    total_released += released;
  }

  return total_released;
}

template <class TrackerType>
inline void HugePageFiller<TrackerType>::AddSpanStats(
    SmallSpanStats* small, LargeSpanStats* large) const {
  auto loop = [&](const TrackerType& pt) { pt.AddSpanStats(small, large); };
  // We can skip the first kChunks lists as they are known to be
  // 100% full.
  donated_alloc_.Iter(loop, 0);
  for (const AccessDensityPrediction type :
       {AccessDensityPrediction::kDense, AccessDensityPrediction::kSparse}) {
    regular_alloc_[type].Iter(loop, kChunks);
    regular_alloc_partial_released_[type].Iter(loop, 0);
    regular_alloc_released_[type].Iter(loop, 0);
  }
}

template <class TrackerType>
inline BackingStats HugePageFiller<TrackerType>::stats() const {
  BackingStats s;
  s.system_bytes = size_.in_bytes();
  s.free_bytes = free_pages().in_bytes();
  s.unmapped_bytes = unmapped_pages().in_bytes();
  return s;
}

template <class TrackerType>
inline HugePageFillerStats HugePageFiller<TrackerType>::GetStats() const {
  HugePageFillerStats stats;
  // Note kChunks, not kNumLists here--we're iterating *full* lists.
  for (size_t chunk = 0; chunk < kChunks; ++chunk) {
    size_t sparselist =
        ListFor(/*longest=*/Length(0), chunk, AccessDensityPrediction::kSparse,
                /*nallocs=*/0);
    stats.n_full[AccessDensityPrediction::kSparse] += NHugePages(
        regular_alloc_[AccessDensityPrediction::kSparse][sparselist].length());

    size_t denselist = ListFor(
        /*longest=*/Length(0), chunk, AccessDensityPrediction::kDense,
        kPagesPerHugePage.raw_num());
    stats.n_full[AccessDensityPrediction::kDense] += NHugePages(
        regular_alloc_[AccessDensityPrediction::kDense][denselist].length());
  }
  stats.n_full[AccessDensityPrediction::kPredictionCounts] =
      stats.n_full[AccessDensityPrediction::kSparse] +
      stats.n_full[AccessDensityPrediction::kDense];

  // We only use donated allocs for allocating sparse pages.
  stats.n_total[AccessDensityPrediction::kSparse] = donated_alloc_.size();
  for (const AccessDensityPrediction count :
       {AccessDensityPrediction::kSparse, AccessDensityPrediction::kDense}) {
    stats.n_fully_released[count] = regular_alloc_released_[count].size();
    stats.n_partial_released[count] =
        regular_alloc_partial_released_[count].size();
    stats.n_released[count] =
        stats.n_fully_released[count] + stats.n_partial_released[count];
    stats.n_total[count] +=
        stats.n_released[count] + regular_alloc_[count].size();
    stats.n_partial[count] =
        stats.n_total[count] - stats.n_released[count] - stats.n_full[count];
  }

  // Collect total stats that is the sum of both kSparse and kDense allocs.
  stats.n_fully_released[AccessDensityPrediction::kPredictionCounts] =
      stats.n_fully_released[AccessDensityPrediction::kSparse] +
      stats.n_fully_released[AccessDensityPrediction::kDense];
  stats.n_partial_released[AccessDensityPrediction::kPredictionCounts] =
      stats.n_partial_released[AccessDensityPrediction::kSparse] +
      stats.n_partial_released[AccessDensityPrediction::kDense];
  stats.n_released[AccessDensityPrediction::kPredictionCounts] =
      stats.n_released[AccessDensityPrediction::kSparse] +
      stats.n_released[AccessDensityPrediction::kDense];

  stats.n_total[AccessDensityPrediction::kPredictionCounts] = size();
  stats.n_partial[AccessDensityPrediction::kPredictionCounts] =
      size() - stats.n_released[AccessDensityPrediction::kPredictionCounts] -
      stats.n_full[AccessDensityPrediction::kPredictionCounts];
  return stats;
}

// Returns true if backoff delay has reached the maximum threshold.
template <class TrackerType>
inline bool HugePageFiller<TrackerType>::ShouldBackoffFromCollapse() {
  // TODO(b/287498389): In addition to latency, consider backing off if collapse
  // fails too often.
  ++current_backoff_delay_;
  if (current_backoff_delay_ < max_backoff_delay_) {
    return true;
  }
  current_backoff_delay_ = 0;
  return false;
}

template <class TrackerType>
void HugePageFiller<TrackerType>::UpdateMaxBackoffDelay(
    absl::Duration latency) {
  // These latency thresholds are chosen empirically.
  const bool increase = latency > kMaxCollapseLatencyThreshold;
  const bool decrease = latency < kMinCollapseLatencyThreshold;
  if (increase) {
    max_backoff_delay_ = std::min(max_backoff_delay_ << 1, kMaxBackoffDelay);
  } else if (decrease) {
    max_backoff_delay_ = std::max(max_backoff_delay_ >> 1, 1);
  }
}

template <class TrackerType>
inline void HugePageFiller<TrackerType>::TreatHugepageTrackers(
    EnableCollapse enable_collapse,
    EnableUnfilteredCollapse enable_unfiltered_collapse,
    ReleaseStalePages release_stale_pages, PageFlagsBase* pageflags,
    Residency* residency) {
  if (enable_collapse == EnableCollapse::kEnabled &&
      ShouldBackoffFromCollapse()) {
    enable_collapse = EnableCollapse::kDisabled;
    ++treatment_stats_.collapse_intervals_skipped;
  }
  bool enable_subrelease_unbacked =
      subrelease_unbacked_mode_ == SubreleaseUnbackedMode::kEnabled;

  SampledTrackerTreatment sampled_tracker_treatment(clock_, tag_,
                                                    set_anon_vma_name_);
  HugePageUnbackedTrackerTreatment<TrackerType> unbacked_tracker_treatment(
      clock_, pageflags, residency, collapse_, *this, enable_collapse,
      subrelease_unbacked_mode_, enable_unfiltered_collapse,
      release_stale_pages);

  // Collect up to kTotalTrackersToScan trackers from our lists.
  regular_alloc_partial_released_[AccessDensityPrediction::kSparse].Iter(
      [&](TrackerType& pt) GOOGLE_MALLOC_SECTION {
        sampled_tracker_treatment.SelectEligibleTrackers(pt);
        unbacked_tracker_treatment.SelectEligibleTrackers(pt);
      },
      /*start=*/0);

  regular_alloc_partial_released_[AccessDensityPrediction::kDense].Iter(
      [&](TrackerType& pt) GOOGLE_MALLOC_SECTION {
        sampled_tracker_treatment.SelectEligibleTrackers(pt);
        unbacked_tracker_treatment.SelectEligibleTrackers(pt);
      },
      /*start=*/0);

  donated_alloc_.Iter(
      [&](TrackerType& pt) GOOGLE_MALLOC_SECTION {
        unbacked_tracker_treatment.SelectEligibleTrackers(pt);
      },
      /*start=*/0);

  regular_alloc_[AccessDensityPrediction::kDense].Iter(
      [&](TrackerType& pt) GOOGLE_MALLOC_SECTION {
        sampled_tracker_treatment.SelectEligibleTrackers(pt);
        unbacked_tracker_treatment.SelectEligibleTrackers(pt);
      },
      /*start=*/0);

  regular_alloc_[AccessDensityPrediction::kSparse].Iter(
      [&](TrackerType& pt) GOOGLE_MALLOC_SECTION {
        sampled_tracker_treatment.SelectEligibleTrackers(pt);
        unbacked_tracker_treatment.SelectEligibleTrackers(pt);
      },
      /*start=*/0);

  regular_alloc_released_[AccessDensityPrediction::kSparse].Iter(
      [&](TrackerType& pt) GOOGLE_MALLOC_SECTION {
        sampled_tracker_treatment.SelectEligibleTrackers(pt);
        if (enable_subrelease_unbacked) {
          unbacked_tracker_treatment.SelectEligibleTrackers(pt);
        }
      },
      /*start=*/0);

  regular_alloc_released_[AccessDensityPrediction::kDense].Iter(
      [&](TrackerType& pt) GOOGLE_MALLOC_SECTION {
        sampled_tracker_treatment.SelectEligibleTrackers(pt);
        if (enable_subrelease_unbacked) {
          unbacked_tracker_treatment.SelectEligibleTrackers(pt);
        }
      },
      /*start=*/0);

  pageheap_lock.unlock();
  sampled_tracker_treatment.Treat();
  unbacked_tracker_treatment.Treat();

  HugePageTreatmentStats stats = unbacked_tracker_treatment.GetStats();

  // Lock the pageheap lock and update residency information in the tracker.
  pageheap_lock.lock();
  if (stats.collapse_attempted > 0) {
    absl::Duration max_collapse_latency = absl::Milliseconds(
        stats.collapse_time_max_cycles * 1000 / clock_.freq());
    UpdateMaxBackoffDelay(max_collapse_latency);
  }
  sampled_tracker_treatment.Restore();
  unbacked_tracker_treatment.Restore();

  unbacked_tracker_treatment.UpdateHugePageTreatmentStats(treatment_stats_);
  // It should be rare that we find anything in the fully freed list, because
  // we only sample 1% of the trackers for naming, and an interleaving Put
  // operation would have to free all the pages while the memory is being named.
  for (TrackerType* tracker : fully_freed_trackers_) {
    tracker->SetAnonVmaName(set_anon_vma_name_, /*name=*/std::nullopt);
  }
}

template <class TrackerType>
inline Length HugePageFiller<TrackerType>::HandleReleaseFree(
    PageTracker* tracker) {
  RemoveFromFillerList(tracker);
  Length released_length = tracker->ReleaseFree(unback_);
  subrelease_stats_.total_pages_subreleased += released_length;
  unmapped_ += released_length;
  unmapping_unaccounted_ += released_length;
  AddToFillerList(tracker);
  return released_length;
}

template <class TrackerType>
inline void HugePageFiller<TrackerType>::OnCollapseSuccess(TrackerType* pt) {
  if (pt->unbroken()) return;
  RemoveFromFillerList(pt);
  pt->set_unbroken(/*status=*/true);
  AddToFillerList(pt);
}

template <class TrackerType>
inline Length HugePageFiller<TrackerType>::HandleUnbackedHugePage(
    PageTracker* tracker, PageBitmap unbacked) {
  RemoveFromFillerList(tracker);
  Length unmapped_length = tracker->MarkSubreleased(unbacked);
  subrelease_stats_.total_pages_subreleased += unmapped_length;
  unmapped_ += unmapped_length;
  unmapping_unaccounted_ += unmapped_length;
  AddToFillerList(tracker);
  return unmapped_length;
}

template <class TrackerType>
inline void HugePageFiller<TrackerType>::Print(Printer& out, bool everything,
                                               PageFlagsBase& pageflags) {
  out.printf("HugePageFiller: densely pack small requests into hugepages\n");
  const HugePageFillerStats stats = GetStats();

  // A donated alloc full list is impossible because it would have never been
  // donated in the first place. (It's an even hugepage.)
  TC_ASSERT(donated_alloc_[0].empty());
  // Evaluate a/b, avoiding division by zero
  const auto safe_div = [](Length a, Length b) {
    return b == Length(0) ? 0.
                          : static_cast<double>(a.raw_num()) /
                                static_cast<double>(b.raw_num());
  };
  out.printf(
      "HugePageFiller: Overall, %zu total, %zu full, %zu partial, %zu released "
      "(%zu partially), 0 quarantined\n",
      size().raw_num(),
      stats.n_full[AccessDensityPrediction::kPredictionCounts].raw_num(),
      stats.n_partial[AccessDensityPrediction::kPredictionCounts].raw_num(),
      stats.n_released[AccessDensityPrediction::kPredictionCounts].raw_num(),
      stats.n_partial_released[AccessDensityPrediction::kPredictionCounts]
          .raw_num());

  out.printf(
      "HugePageFiller: those with sparsely-accessed spans, %zu total, "
      "%zu full, %zu partial, %zu released (%zu partially), 0 quarantined\n",
      stats.n_total[AccessDensityPrediction::kSparse].raw_num(),
      stats.n_full[AccessDensityPrediction::kSparse].raw_num(),
      stats.n_partial[AccessDensityPrediction::kSparse].raw_num(),
      stats.n_released[AccessDensityPrediction::kSparse].raw_num(),
      stats.n_partial_released[AccessDensityPrediction::kSparse].raw_num());

  out.printf(
      "HugePageFiller: those with densely-accessed spans, %zu total, "
      "%zu full, %zu partial, %zu released (%zu partially), 0 quarantined\n",
      stats.n_total[AccessDensityPrediction::kDense].raw_num(),
      stats.n_full[AccessDensityPrediction::kDense].raw_num(),
      stats.n_partial[AccessDensityPrediction::kDense].raw_num(),
      stats.n_released[AccessDensityPrediction::kDense].raw_num(),
      stats.n_partial_released[AccessDensityPrediction::kDense].raw_num());

  out.printf("HugePageFiller: %zu pages free in %zu hugepages, %.4f free\n",
             free_pages().raw_num(), size().raw_num(),
             safe_div(free_pages(), size().in_pages()));

  const HugeLength n_nonfull =
      stats.n_partial[AccessDensityPrediction::kPredictionCounts] +
      stats.n_partial_released[AccessDensityPrediction::kPredictionCounts];
  TC_ASSERT_LE(free_pages(), n_nonfull.in_pages());
  out.printf("HugePageFiller: among non-fulls, %.4f free\n",
             safe_div(free_pages(), n_nonfull.in_pages()));

  out.printf(
      "HugePageFiller: %zu used pages in subreleased hugepages (%zu of them in "
      "partially released)\n",
      used_pages_in_any_subreleased().raw_num(),
      used_pages_in_partial_released().raw_num());

  out.printf(
      "HugePageFiller: %zu hugepages partially released, %.4f released\n",
      stats.n_released[AccessDensityPrediction::kPredictionCounts].raw_num(),
      safe_div(unmapped_pages(),
               stats.n_released[AccessDensityPrediction::kPredictionCounts]
                   .in_pages()));
  out.printf("HugePageFiller: %.4f of used pages hugepageable\n",
             hugepage_frac());

  // Subrelease
  out.printf(
      "HugePageFiller: Since startup, %zu pages subreleased, %zu hugepages "
      "broken, (%zu pages, %zu hugepages due to reaching tcmalloc limit)\n",
      subrelease_stats_.total_pages_subreleased.raw_num(),
      subrelease_stats_.total_hugepages_broken.raw_num(),
      subrelease_stats_.total_pages_subreleased_due_to_limit.raw_num(),
      subrelease_stats_.total_hugepages_broken_due_to_limit.raw_num());

  if (!everything) return;

  out.printf(
      "HugePageFiller: Out of %zu eligible hugepages, %zu were "
      "attempted, and %zu were collapsed.\n",
      treatment_stats_.collapse_eligible, treatment_stats_.collapse_attempted,
      treatment_stats_.collapse_succeeded);

  out.printf(
      "HugePageFiller: Of the failed collapse operations, number of operations "
      "that failed per error type");
  for (int i = 0; i < treatment_stats_.collapse_errors.size(); ++i) {
    out.printf(", %s: %zu",
               HugePageTreatmentStats::ErrorTypeToString(
                   static_cast<CollapseErrorType>(i)),
               treatment_stats_.collapse_errors[i]);
  }
  out.printf("\n");

  out.printf(
      "HugePageFiller: Latency of collapse operations: "
      "%f ms (total), %f us (maximum)\n",
      treatment_stats_.collapse_time_total_cycles * 1000 / clock_.freq(),
      treatment_stats_.collapse_time_max_cycles * 1000 * 1000 / clock_.freq());

  out.printf(
      "HugePageFiller: Backoff delay for collapse currently is %d interval(s), "
      "number of intervals skipped due to backoff is %d\n",
      max_backoff_delay_, treatment_stats_.collapse_intervals_skipped);

  out.printf(
      "HugePageFiller: In the previous treatment interval, "
      "subreleased %zu pages.\n",
      treatment_stats_.treated_pages_subreleased);
  out.printf(
      "HugePageFiller: In the previous treatment interval, "
      "subreleased %zu stale pages.\n",
      treatment_stats_.treated_pages_stale_subreleased);

  out.printf(
      "HugePageFiller: In the previous treatment interval, "
      "marked %zu unbacked pages as subreleased. Since startup, %zu.\n",
      treatment_stats_.treated_pages_unbacked_subreleased,
      treatment_stats_.total_treated_pages_unbacked_subreleased);

  out.printf("\n");
  out.printf("HugePageFiller: fullness histograms\n");

  // Compute some histograms of fullness.
  using huge_page_filler_internal::UsageInfo;
  UsageInfo usage;
  const double now = clock_.now();
  const double frequency = clock_.freq();
  {
    size_t num_selected = 0;
    UsageInfo::UsageInfoRecords records;
    regular_alloc_[AccessDensityPrediction::kSparse].Iter(
        [&](const TrackerType& pt) {
          usage.Record(pt, pageflags, now, frequency, records, num_selected);
        },
        0);
    usage.Print(records, UsageInfo::kSparseRegular, out);
    num_selected = 0;
  }

  {
    size_t num_selected = 0;
    UsageInfo::UsageInfoRecords records;
    regular_alloc_[AccessDensityPrediction::kDense].Iter(
        [&](const TrackerType& pt) {
          usage.Record(pt, pageflags, now, frequency, records, num_selected);
        },
        0);
    usage.Print(records, UsageInfo::kDenseRegular, out);
  }

  {
    size_t num_selected = 0;
    UsageInfo::UsageInfoRecords records;
    donated_alloc_.Iter(
        [&](const TrackerType& pt) {
          usage.Record(pt, pageflags, now, frequency, records, num_selected);
        },
        0);
    usage.Print(records, UsageInfo::kDonated, out);
  }

  {
    size_t num_selected = 0;
    UsageInfo::UsageInfoRecords records;
    regular_alloc_partial_released_[AccessDensityPrediction::kSparse].Iter(
        [&](const TrackerType& pt) {
          usage.Record(pt, pageflags, now, frequency, records, num_selected);
        },
        0);
    usage.Print(records, UsageInfo::kSparsePartialReleased, out);
  }

  {
    size_t num_selected = 0;
    UsageInfo::UsageInfoRecords records;
    regular_alloc_partial_released_[AccessDensityPrediction::kDense].Iter(
        [&](const TrackerType& pt) {
          usage.Record(pt, pageflags, now, frequency, records, num_selected);
        },
        0);
    usage.Print(records, UsageInfo::kDensePartialReleased, out);
  }

  {
    size_t num_selected = 0;
    UsageInfo::UsageInfoRecords records;
    regular_alloc_released_[AccessDensityPrediction::kSparse].Iter(
        [&](const TrackerType& pt) {
          usage.Record(pt, pageflags, now, frequency, records, num_selected);
        },
        0);
    usage.Print(records, UsageInfo::kSparseReleased, out);
  }

  {
    size_t num_selected = 0;
    UsageInfo::UsageInfoRecords records;
    regular_alloc_released_[AccessDensityPrediction::kDense].Iter(
        [&](const TrackerType& pt) {
          usage.Record(pt, pageflags, now, frequency, records, num_selected);
        },
        0);
    usage.Print(records, UsageInfo::kDenseReleased, out);
  }

  out.printf(
      "\nHugePageFiller: %zu hugepages became full after being previously "
      "released, "
      "out of which %zu pages are hugepage backed.\n",
      previously_released_huge_pages().raw_num(),
      usage.HugepageBackedPreviouslyReleased());

  PrintLifetimeHisto(out, lifetime_histo_[AccessDensityPrediction::kDense],
                     AccessDensityPrediction::kDense,
                     "hps with completed lifetime a <= # hps < b");
  PrintLifetimeHisto(out, lifetime_histo_[AccessDensityPrediction::kSparse],
                     AccessDensityPrediction::kSparse,
                     "hps with completed lifetime a <= # hps < b");
  out.printf("\n");
  fillerstats_tracker_.Print(out, "HugePageFiller");
}

template <class TrackerType>
inline void HugePageFiller<TrackerType>::PrintAllocStatsInPbtxt(
    absl::string_view field, PbtxtRegion& hpaa,
    const HugePageFillerStats& stats, AccessDensityPrediction count) const {
  TC_ASSERT_LT(count, AccessDensityPrediction::kPredictionCounts);
  PbtxtRegion alloc_region = hpaa.CreateSubRegion(field);
  alloc_region.PrintI64("full_huge_pages", stats.n_full[count].raw_num());
  alloc_region.PrintI64("partial_huge_pages", stats.n_partial[count].raw_num());
  alloc_region.PrintI64("released_huge_pages",
                        stats.n_released[count].raw_num());
  alloc_region.PrintI64("partially_released_huge_pages",
                        stats.n_partial_released[count].raw_num());
}

template <class TrackerType>
inline void HugePageFiller<TrackerType>::PrintInPbtxt(
    PbtxtRegion& hpaa, PageFlagsBase& pageflags) {
  const HugePageFillerStats stats = GetStats();

  // A donated alloc full list is impossible because it would have never been
  // donated in the first place. (It's an even hugepage.)
  TC_ASSERT(donated_alloc_[0].empty());
  // Evaluate a/b, avoiding division by zero
  const auto safe_div = [](Length a, Length b) {
    return b == Length(0) ? 0.
                          : static_cast<double>(a.raw_num()) /
                                static_cast<double>(b.raw_num());
  };

  hpaa.PrintI64(
      "filler_full_huge_pages",
      stats.n_full[AccessDensityPrediction::kPredictionCounts].raw_num());
  hpaa.PrintI64(
      "filler_partial_huge_pages",
      stats.n_partial[AccessDensityPrediction::kPredictionCounts].raw_num());
  hpaa.PrintI64(
      "filler_released_huge_pages",
      stats.n_released[AccessDensityPrediction::kPredictionCounts].raw_num());
  hpaa.PrintI64(
      "filler_partially_released_huge_pages",
      stats.n_partial_released[AccessDensityPrediction::kPredictionCounts]
          .raw_num());

  PrintAllocStatsInPbtxt("filler_sparsely_accessed_alloc_stats", hpaa, stats,
                         AccessDensityPrediction::kSparse);
  PrintAllocStatsInPbtxt("filler_densely_accessed_alloc_stats", hpaa, stats,
                         AccessDensityPrediction::kDense);

  hpaa.PrintI64("filler_free_pages", free_pages().raw_num());
  hpaa.PrintI64("filler_used_pages_in_subreleased",
                used_pages_in_any_subreleased().raw_num());
  hpaa.PrintI64("filler_used_pages_in_partial_released",
                used_pages_in_partial_released().raw_num());
  hpaa.PrintI64(
      "filler_unmapped_bytes",
      static_cast<uint64_t>(
          stats.n_released[AccessDensityPrediction::kPredictionCounts]
              .raw_num() *
          safe_div(unmapped_pages(),
                   stats.n_released[AccessDensityPrediction::kPredictionCounts]
                       .in_pages())));
  hpaa.PrintI64(
      "filler_hugepageable_used_bytes",
      static_cast<uint64_t>(
          hugepage_frac() *
          static_cast<double>(
              pages_allocated_[AccessDensityPrediction::kSparse].in_bytes() +
              pages_allocated_[AccessDensityPrediction::kDense].in_bytes())));
  hpaa.PrintI64("filler_previously_released_huge_pages",
                previously_released_huge_pages().raw_num());
  hpaa.PrintI64("filler_num_pages_subreleased",
                subrelease_stats_.total_pages_subreleased.raw_num());
  hpaa.PrintI64("filler_num_hugepages_broken",
                subrelease_stats_.total_hugepages_broken.raw_num());
  hpaa.PrintI64(
      "filler_num_pages_subreleased_due_to_limit",
      subrelease_stats_.total_pages_subreleased_due_to_limit.raw_num());
  hpaa.PrintI64(
      "filler_num_hugepages_broken_due_to_limit",
      subrelease_stats_.total_hugepages_broken_due_to_limit.raw_num());
  // Compute some histograms of fullness.
  using huge_page_filler_internal::UsageInfo;
  UsageInfo usage;
  const double now = clock_.now();
  const double frequency = clock_.freq();
  {
    UsageInfo::UsageInfoRecords records;
    size_t num_selected = 0;
    regular_alloc_[AccessDensityPrediction::kSparse].Iter(
        [&](const TrackerType& pt) {
          usage.Record(pt, pageflags, now, frequency, records, num_selected);
        },
        0);
    usage.Print(records, UsageInfo::kSparseRegular, hpaa);
  }

  {
    UsageInfo::UsageInfoRecords records;
    size_t num_selected = 0;
    regular_alloc_[AccessDensityPrediction::kDense].Iter(
        [&](const TrackerType& pt) {
          usage.Record(pt, pageflags, now, frequency, records, num_selected);
        },
        0);
    usage.Print(records, UsageInfo::kDenseRegular, hpaa);
  }

  {
    UsageInfo::UsageInfoRecords records;
    size_t num_selected = 0;
    donated_alloc_.Iter(
        [&](const TrackerType& pt) {
          usage.Record(pt, pageflags, now, frequency, records, num_selected);
        },
        0);
    usage.Print(records, UsageInfo::kDonated, hpaa);
  }

  {
    UsageInfo::UsageInfoRecords records;
    size_t num_selected = 0;
    regular_alloc_partial_released_[AccessDensityPrediction::kSparse].Iter(
        [&](const TrackerType& pt) {
          usage.Record(pt, pageflags, now, frequency, records, num_selected);
        },
        0);
    usage.Print(records, UsageInfo::kSparsePartialReleased, hpaa);
  }

  {
    UsageInfo::UsageInfoRecords records;
    size_t num_selected = 0;
    regular_alloc_partial_released_[AccessDensityPrediction::kDense].Iter(
        [&](const TrackerType& pt) {
          usage.Record(pt, pageflags, now, frequency, records, num_selected);
        },
        0);
    usage.Print(records, UsageInfo::kDensePartialReleased, hpaa);
  }

  {
    UsageInfo::UsageInfoRecords records;
    size_t num_selected = 0;
    regular_alloc_released_[AccessDensityPrediction::kSparse].Iter(
        [&](const TrackerType& pt) {
          usage.Record(pt, pageflags, now, frequency, records, num_selected);
        },
        0);
    usage.Print(records, UsageInfo::kSparseReleased, hpaa);
  }

  {
    UsageInfo::UsageInfoRecords records;
    size_t num_selected = 0;
    regular_alloc_released_[AccessDensityPrediction::kDense].Iter(
        [&](const TrackerType& pt) {
          usage.Record(pt, pageflags, now, frequency, records, num_selected);
        },
        0);
    usage.Print(records, UsageInfo::kDenseReleased, hpaa);
  }

  hpaa.PrintI64("filler_previously_released_backed_huge_pages",
                usage.HugepageBackedPreviouslyReleased());
  {
    PbtxtRegion huge_page_treatment_region =
        hpaa.CreateSubRegion("filler_huge_page_treatment_stats");
    huge_page_treatment_region.PrintI64("collapse_eligible",
                                        treatment_stats_.collapse_eligible);
    huge_page_treatment_region.PrintI64("collapse_attempted",
                                        treatment_stats_.collapse_attempted);
    huge_page_treatment_region.PrintI64("collapse_succeeded",
                                        treatment_stats_.collapse_succeeded);
    for (int i = 0; i < treatment_stats_.collapse_errors.size(); ++i) {
      PbtxtRegion collapse_errors_region =
          huge_page_treatment_region.CreateSubRegion("collapse_errors");
      collapse_errors_region.PrintRaw("type",
                                      HugePageTreatmentStats::ErrorTypeToString(
                                          static_cast<CollapseErrorType>(i)));
      collapse_errors_region.PrintI64("count",
                                      treatment_stats_.collapse_errors[i]);
    }
    huge_page_treatment_region.PrintI64(
        "collapse_total_time_ms",
        treatment_stats_.collapse_time_total_cycles * 1000 / clock_.freq());
    huge_page_treatment_region.PrintI64(
        "collapse_max_time_us", treatment_stats_.collapse_time_max_cycles *
                                    1000 * 1000 / clock_.freq());
    huge_page_treatment_region.PrintI64("collapse_backoff_delay",
                                        max_backoff_delay_);
    huge_page_treatment_region.PrintI64(
        "collapse_intervals_skipped",
        treatment_stats_.collapse_intervals_skipped);

    huge_page_treatment_region.PrintI64(
        "treated_pages_subreleased",
        treatment_stats_.treated_pages_subreleased);
    huge_page_treatment_region.PrintI64(
        "treated_pages_unbacked_subreleased",
        treatment_stats_.treated_pages_unbacked_subreleased);
    huge_page_treatment_region.PrintI64(
        "total_treated_pages_unbacked_subreleased",
        treatment_stats_.total_treated_pages_unbacked_subreleased);
    huge_page_treatment_region.PrintI64(
        "treated_pages_stale_subreleased",
        treatment_stats_.treated_pages_stale_subreleased);
  }
  PrintLifetimeHistoInPbtxt(hpaa,
                            lifetime_histo_[AccessDensityPrediction::kDense],
                            "densely_accessed_completed_lifetime_histogram");
  PrintLifetimeHistoInPbtxt(hpaa,
                            lifetime_histo_[AccessDensityPrediction::kSparse],
                            "sparsely_accessed_completed_lifetime_histogram");
  fillerstats_tracker_.PrintSubreleaseStatsInPbtxt(hpaa,
                                                   "filler_skipped_subrelease");
  fillerstats_tracker_.PrintTimeseriesStatsInPbtxt(hpaa,
                                                   "filler_stats_timeseries");
}

template <class TrackerType>
inline void HugePageFiller<TrackerType>::UpdateFillerStatsTracker() {
  StatsTrackerType::SubreleaseStats stats;
  stats.num_pages = pages_allocated();
  stats.free_pages = free_pages();
  stats.unmapped_pages = unmapped_pages();
  stats.num_pages_subreleased = subrelease_stats_.num_pages_subreleased;
  fillerstats_tracker_.Report(stats);
  subrelease_stats_.reset();
}

template <class TrackerType>
inline size_t HugePageFiller<TrackerType>::IndexFor(
    const TrackerType& pt) const {
  TC_ASSERT(!pt.empty());
  // For dense hugepages, the first dimension that we manage trackers along is
  // nallocs. This is different from tracking for sparse spans -- the first
  // dimension is the longest-free-range and the second one uses nallocs. So,
  // for dense spans, there is no need to distribute them using nallocs again.
  if (pt.HasDenseSpans()) return 0;

  // Prefer to allocate from hugepages with many allocations already present;
  // spaced logarithmically.
  const size_t na = pt.nallocs();
  // This equals 63 - ceil(log2(na))
  // (or 31 if size_t is 4 bytes, etc.)
  const size_t neg_ceil_log = __builtin_clzl(2 * na - 1);

  // We want the same spread as neg_ceil_log, but spread over [0,
  // kChunks) (clamped at the left edge) instead of [0, 64). So
  // subtract off the difference (computed by forcing na=1 to
  // kChunks - 1.)
  const size_t kOffset = __builtin_clzl(1) - (kChunks - 1);
  const size_t i = std::max(neg_ceil_log, kOffset) - kOffset;
  TC_ASSERT_LT(i, kChunks);
  return i;
}

template <class TrackerType>
inline size_t HugePageFiller<TrackerType>::SparseListFor(
    const Length longest, const size_t chunk) const {
  TC_ASSERT_LT(longest, kPagesPerHugePage);
  return longest.raw_num() * kChunks + chunk;
}

template <class TrackerType>
inline size_t HugePageFiller<TrackerType>::DenseListFor(const size_t chunk,
                                                        size_t nallocs) const {
  TC_ASSERT_LE(nallocs, kPagesPerHugePage.raw_num());
  // For the dense tracker with hugepages sorted on allocs, the hugepages are
  // placed only in lists that are multiples of kChunks.  The in-between lists
  // are empty.
  return (kPagesPerHugePage.raw_num() - nallocs) * kChunks + chunk;
}

template <class TrackerType>
inline size_t HugePageFiller<TrackerType>::ListFor(
    const Length longest, const size_t chunk,
    const AccessDensityPrediction density, size_t nallocs) const {
  TC_ASSERT_LT(chunk, kChunks);
  switch (density) {
    case AccessDensityPrediction::kSparse:
      return SparseListFor(longest, chunk);
    case AccessDensityPrediction::kDense:
      return DenseListFor(chunk, nallocs);
    default:
      TC_BUG("bad density %v", density);
  }
}

template <class TrackerType>
inline void HugePageFiller<TrackerType>::RemoveFromFillerList(TrackerType* pt) {
  Length longest = pt->longest_free_range();
  TC_ASSERT_LT(longest, kPagesPerHugePage);

  if (pt->donated()) {
    donated_alloc_.Remove(pt, longest.raw_num());
    return;
  }

  const AccessDensityPrediction type = pt->HasDenseSpans()
                                           ? AccessDensityPrediction::kDense
                                           : AccessDensityPrediction::kSparse;
  size_t i = ListFor(longest, IndexFor(*pt), type, pt->nallocs());

  if (!pt->released() &&
      (pt->unbroken() ||
       subrelease_unbacked_mode_ == SubreleaseUnbackedMode::kDisabled)) {
    regular_alloc_[type].Remove(pt, i);
  } else if (pt->free_pages() <= pt->released_pages()) {
    regular_alloc_released_[type].Remove(pt, i);
    TC_ASSERT_GE(n_used_released_[type], pt->used_pages());
    n_used_released_[type] -= pt->used_pages();
  } else {
    regular_alloc_partial_released_[type].Remove(pt, i);
    TC_ASSERT_GE(n_used_partial_released_[type], pt->used_pages());
    n_used_partial_released_[type] -= pt->used_pages();
  }
}

template <class TrackerType>
inline TrackerType* absl_nullable
HugePageFiller<TrackerType>::FetchFullyFreedTracker() {
  if (fully_freed_trackers_.empty()) {
    return nullptr;
  }

  TrackerType* pt = fully_freed_trackers_.first();
  fully_freed_trackers_.remove(pt);
  return pt;
}

template <class TrackerType>
inline void HugePageFiller<TrackerType>::AddToFillerList(TrackerType* pt) {
  Length longest = pt->longest_free_range();
  TC_ASSERT_LE(longest, kPagesPerHugePage);

  if (longest == kPagesPerHugePage) {
    TC_ASSERT(pt->empty());
    TC_ASSERT(pt->DontFreeTracker());
    fully_freed_trackers_.prepend(pt);
    return;
  }

  // Once a donated alloc is used in any way, it degenerates into being a
  // regular alloc. This allows the algorithm to keep using it (we had to be
  // desperate to use it in the first place), and thus preserves the other
  // donated allocs.
  pt->set_donated(false);

  const AccessDensityPrediction type = pt->HasDenseSpans()
                                           ? AccessDensityPrediction::kDense
                                           : AccessDensityPrediction::kSparse;
  size_t i = ListFor(longest, IndexFor(*pt), type, pt->nallocs());

  if (!pt->released() &&
      (pt->unbroken() ||
       subrelease_unbacked_mode_ == SubreleaseUnbackedMode::kDisabled)) {
    regular_alloc_[type].Add(pt, i);
  } else if (pt->free_pages() <= pt->released_pages()) {
    regular_alloc_released_[type].Add(pt, i);
    n_used_released_[type] += pt->used_pages();
  } else {
    regular_alloc_partial_released_[type].Add(pt, i);
    n_used_partial_released_[type] += pt->used_pages();
  }
}

template <class TrackerType>
inline void HugePageFiller<TrackerType>::DonateToFillerList(TrackerType* pt) {
  Length longest = pt->longest_free_range();
  TC_ASSERT_LT(longest, kPagesPerHugePage);

  // We should never be donating already-released trackers!
  TC_ASSERT(!pt->released());
  pt->set_donated(true);

  // Donated allocs always follow finer indexing based on the longest free
  // range.
  donated_alloc_.Add(pt, longest.raw_num());
}

template <class TrackerType>
inline double HugePageFiller<TrackerType>::hugepage_frac() const {
  // How many of our used pages are on non-huge pages? Since
  // everything on a released hugepage is either used or released,
  // just the difference:
  const Length used = used_pages();
  const Length used_on_rel = used_pages_in_any_subreleased();
  TC_ASSERT_GE(used, used_on_rel);
  const Length used_on_huge = used - used_on_rel;

  const Length denom = used > Length(0) ? used : Length(1);
  const double ret =
      static_cast<double>(used_on_huge.raw_num()) / denom.raw_num();
  TC_ASSERT_GE(ret, 0);
  TC_ASSERT_LE(ret, 1);
  return std::clamp<double>(ret, 0, 1);
}

template <class TrackerType>
template <typename F>
void HugePageFiller<TrackerType>::ForEachHugePage(const F& func) {
  donated_alloc_.Iter(func, 0);
  regular_alloc_[AccessDensityPrediction::kSparse].Iter(func, 0);
  regular_alloc_[AccessDensityPrediction::kDense].Iter(func, 0);
  regular_alloc_partial_released_[AccessDensityPrediction::kSparse].Iter(func,
                                                                         0);
  regular_alloc_partial_released_[AccessDensityPrediction::kDense].Iter(func,
                                                                        0);
  regular_alloc_released_[AccessDensityPrediction::kSparse].Iter(func, 0);
  regular_alloc_released_[AccessDensityPrediction::kDense].Iter(func, 0);
}

// Helper for stat functions.
template <class TrackerType>
inline Length HugePageFiller<TrackerType>::free_pages() const {
  return size().in_pages() - used_pages() - unmapped_pages();
}

}  // namespace tcmalloc_internal
}  // namespace tcmalloc
GOOGLE_MALLOC_SECTION_END

#endif  // TCMALLOC_HUGE_PAGE_FILLER_H_
