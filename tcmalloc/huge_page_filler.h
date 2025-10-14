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
#include <cstring>
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
#include "tcmalloc/huge_page_subrelease.h"
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

// Thresholds that determine collapse backoff behavior.
constexpr absl::Duration kMaxCollapseLatencyThreshold = absl::Milliseconds(30);
constexpr absl::Duration kMinCollapseLatencyThreshold = absl::Milliseconds(15);

// PageTracker keeps track of the allocation status of every page in a HugePage.
// It allows allocation and deallocation of a contiguous run of pages.
//
// Its mutating methods are annotated as requiring the pageheap_lock, in order
// to support unlocking the page heap lock in a dynamic annotation-friendly way.
class PageTracker : public TList<PageTracker>::Elem {
 public:
  PageTracker(HugePage p, bool was_donated, uint64_t now)
      : location_(p),
        released_count_(0),
        abandoned_count_(0),
        donated_(false),
        was_donated_(was_donated),
        was_released_(false),
        abandoned_(false),
        unbroken_(true),
        alloctime_(now),
        free_{},
        num_objects_(0) {
#ifndef __ppc64__
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Winvalid-offsetof"
#endif
    // Verify fields are structured so commonly accessed members (as part of
    // Put) are on the first two cache lines.  This allows the CentralFreeList
    // to accelerate deallocations by prefetching PageTracker instances before
    // taking the pageheap_lock.
    //
    // On PPC64, kHugePageSize / kPageSize is typically ~2K (16MB / 8KB),
    // requiring 512 bytes for representing free_.  While its cache line size is
    // larger, the entirety of free_ will not fit on two cache lines.
#ifdef NDEBUG
    static_assert(offsetof(PageTracker, location_) + sizeof(location_) <=
                      2 * ABSL_CACHELINE_SIZE,
                  "location_ should fall within the first two cachelines of "
                  "PageTracker.");
    static_assert(
        offsetof(PageTracker, donated_) + sizeof(donated_) <=
            2 * ABSL_CACHELINE_SIZE,
        "donated_ should fall within the first two cachelines of PageTracker.");
    static_assert(
        offsetof(PageTracker, free_) + sizeof(free_) <= 2 * ABSL_CACHELINE_SIZE,
        "free_ should fall within the first two cachelines of PageTracker.");
    static_assert(offsetof(PageTracker, alloctime_) + sizeof(alloctime_) <=
                      2 * ABSL_CACHELINE_SIZE,
                  "alloctime_ should fall within the first two cachelines of "
                  "PageTracker.");
#endif  // NDEBUG
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
#endif  // __ppc64__
  }

  struct PageAllocation {
    PageId page;
    Length previously_unbacked;
  };

  struct TrackerFeatures {
    bool is_valid = false;
    bool is_hugepage_backed = false;
    bool density = false;
    size_t allocations = 0;
    size_t objects = 0;
    double allocation_time = 0.0;
    double reallocation_time = 0.0;
    Length longest_free_range = kPagesPerHugePage;
  };

  // REQUIRES: there's a free range of at least n pages
  //
  // Returns a PageId i and a count of previously unbacked pages in the range
  // [i, i+n) in previously_unbacked.
  PageAllocation Get(Length n, SpanAllocInfo span_alloc_info)
      ABSL_EXCLUSIVE_LOCKS_REQUIRED(pageheap_lock);

  // REQUIRES: r was the result of a previous call to Get(n)
  void Put(Range r, SpanAllocInfo span_alloc_info)
      ABSL_EXCLUSIVE_LOCKS_REQUIRED(pageheap_lock);

  // Returns true if any unused pages have been returned-to-system.
  bool released() const { return released_count_ > 0; }

  // Was this tracker donated from the tail of a multi-hugepage allocation?
  // Only up-to-date when the tracker is on a TrackerList in the Filler;
  // otherwise the value is meaningless.
  bool donated() const { return donated_; }

  // Set/reset the donated flag. The donated status is lost, for instance,
  // when further allocations are made on the tracker.
  void set_donated(bool status) { donated_ = status; }

  // Tracks whether the page was given to the filler in the donated state.  It
  // is not cleared by the filler, allowing the HugePageAwareAllocator to track
  // memory persistently donated to the filler.
  bool was_donated() const { return was_donated_; }

  bool was_released() const { return was_released_; }
  void set_was_released(bool status) { was_released_ = status; }

  // Tracks whether the page, previously donated to the filler, was abondoned.
  // When a large allocation is deallocated but the huge page is not
  // reassembled, the pages are abondoned to the filler for future allocations.
  bool abandoned() const { return abandoned_; }
  void set_abandoned(bool status) { abandoned_ = status; }
  // Tracks how many pages were provided when the originating allocation of a
  // donated page was deallocated but other allocations were in use.
  //
  // Requires was_donated().
  Length abandoned_count() const { return Length(abandoned_count_); }
  void set_abandoned_count(Length count) {
    TC_ASSERT(was_donated_);
    abandoned_count_ = count.raw_num();
  }

  // These statistics help us measure the fragmentation of a hugepage and
  // the desirability of allocating from this hugepage.
  Length longest_free_range() const { return Length(free_.longest_free()); }
  size_t nallocs() const { return free_.allocs(); }
  size_t nobjects() const { return num_objects_; }
  Length used_pages() const { return Length(free_.used()); }
  Length released_pages() const { return Length(released_count_); }
  double alloctime() const { return alloctime_; }
  double last_page_allocation_time() const {
    return last_page_allocation_time_;
  }
  bool fully_freed() const { return longest_free_range() == kPagesPerHugePage; }
  Length free_pages() const;
  bool empty() const;

  // This is the snapshot of the features at the time of the last invocation of
  // RecordFeatures().
  TrackerFeatures features() const { return features_; }
  bool unbroken() const { return unbroken_; }

  // Returns the hugepage whose availability is being tracked.
  HugePage location() const { return location_; }

  // Return all unused pages to the system, mark future frees to do same.
  // Returns the count of pages unbacked.
  Length ReleaseFree(MemoryModifyFunction& unback)
      ABSL_EXCLUSIVE_LOCKS_REQUIRED(pageheap_lock);

  // Attempts to collapse memory tracked by this tracker. Returns true if the
  // collapse was successful.
  MemoryModifyStatus Collapse(MemoryModifyFunction& collapse);

  void AddSpanStats(SmallSpanStats* absl_nullable small,
                    LargeSpanStats* absl_nullable large) const;
  bool HasDenseSpans() const { return has_dense_spans_; }
  void SetHasDenseSpans() { has_dense_spans_ = true; }

  struct HugePageResidencyState {
    // Records whether the page is hugepage backed.
    bool maybe_hugepage_backed = false;
    // Records the time (in ticks) when the residency state was last updated.
    // This is used to determine when the tracker may be revisited for
    // collapse.
    double record_time;
    // Records whether metrics are valid. It is set the first time the
    // residency state is queried.
    bool entry_valid = false;
    // This records the trackers that are currently being collapsed. This is
    // used to avoid subreleasing the pages that are being collapsed.
    bool being_collapsed = false;
    // Records swap and unbacked bitmaps for this hugepage.
    Residency::SinglePageBitmaps bitmaps;
  };

  void SetHugePageResidencyState(const HugePageResidencyState& state) {
    hugepage_residency_state_ = state;
    // TODO(b/435718337):  As of July 2025, we primarily scan "normal"
    // (non-released) page lists and avoid collapsing released huge pages.
    //
    // If released() && state.maybe_hugepage_backed, then we should:
    // * was_released_ = false
    // * unbroken_ = true
    // * release_count_ = 0
    // * released_by_page.Clear()
    // * RemoveFromFillerList/AddToFillerList *this in the filler to reposition
    //   it to the appropriate freelist.
    //
    // since the tracker has transitioned from broken/no hugepage to hugepage'd.
  }

  HugePageResidencyState GetHugePageResidencyState() const {
    return hugepage_residency_state_;
  }

  void SetBeingCollapsed(bool value) {
    hugepage_residency_state_.being_collapsed = value;
  }

  void SetLastAllocationTime(double value) {
    last_page_allocation_time_ = value;
  }

  void RecordFeatures() {
    features_.is_hugepage_backed =
        hugepage_residency_state_.maybe_hugepage_backed;
    features_.density = has_dense_spans_;
    features_.allocations = nallocs();
    features_.objects = nobjects();
    features_.allocation_time = last_page_allocation_time_;
    features_.longest_free_range = longest_free_range();
  }

  bool BeingCollapsed() const {
    return hugepage_residency_state_.being_collapsed;
  }

  void SetDontFreeTracker(bool value) { dont_free_tracker_ = value; }
  bool DontFreeTracker() const { return dont_free_tracker_; }

  struct TagState {
    bool sampled_for_tagging = false;
    double record_time = 0;
  };
  TagState GetTagState() const { return tagged_state_; }
  void SetTagState(const TagState& state) { tagged_state_ = state; }

  void SetAnonVmaName(MemoryTagFunction& set_anon_vma_name,
                      std::optional<absl::string_view> name);

  struct NativePageResidencyInfo {
    size_t n_free_swapped;
    size_t n_free_unbacked;
  };

  NativePageResidencyInfo CountInfoInHugePage(
      Residency::SinglePageBitmaps bitmaps) const;

 private:
  HugePage location_;

  // Cached value of released_by_page_.CountBits(0, kPagesPerHugePages)
  //
  // TODO(b/151663108):  Logically, this is guarded by pageheap_lock.
  uint16_t released_count_;
  uint16_t abandoned_count_;
  bool donated_;
  bool was_donated_;
  bool was_released_;
  // Tracks whether we accounted for the abandoned state of the page. When a
  // large allocation is deallocated but the huge page can not be reassembled,
  // we measure the number of pages abandoned to the filler. To make sure that
  // we do not double-count any future deallocations, we maintain a state and
  // reset it once we measure those pages in abandoned_count_.
  bool abandoned_;
  bool unbroken_;
  double alloctime_;
  double last_page_allocation_time_ = 0;

  RangeTracker<kPagesPerHugePage.raw_num()> free_;

  uint64_t num_objects_;

  TrackerFeatures features_;

  TagState tagged_state_;

  // Bitmap of pages based on them being released to the OS.
  // * Not yet released pages are unset (considered "free")
  // * Released pages are set.
  //
  // Before releasing any locks to release memory to the OS, we mark the bitmap.
  //
  // Once released, a huge page is considered released *until* free_ is
  // exhausted and no pages released_by_page_ are set.  We may have up to
  // kPagesPerHugePage-1 parallel subreleases in-flight.
  //
  // TODO(b/151663108):  Logically, this is guarded by pageheap_lock.
  Bitmap<kPagesPerHugePage.raw_num()> released_by_page_;

  static_assert(kPagesPerHugePage.raw_num() <
                    std::numeric_limits<uint16_t>::max(),
                "nallocs must be able to support kPagesPerHugePage!");

  bool has_dense_spans_ = false;

  HugePageResidencyState hugepage_residency_state_;

  // This field is used to avoid freeing this tracker prematurely. When this
  // is set, any maintenance operation (e.g. collapse) that drops
  // pageheap_lock might manipulate the tracker state without holding the
  // lock. When all the pages on the tracked hugepage are freed, this field
  // is checked to ensure that the tracker is not freed right away.
  bool dont_free_tracker_ = false;

  [[nodiscard]] bool ReleasePages(Range r, MemoryModifyFunction& unback) {
    bool success = unback(r).success;
    if (ABSL_PREDICT_TRUE(success)) {
      unbroken_ = false;
    }
    return success;
  }
};

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

enum class CollapseErrorType : size_t {
  kENoMem = 0,
  kEBusy,
  kEInval,
  kEAgain,
  kOther,
  kErrorTypes
};

struct HugePageTreatmentStats {
  size_t collapse_eligible = 0;
  size_t collapse_attempted = 0;
  size_t collapse_succeeded = 0;
  std::array<size_t, static_cast<size_t>(CollapseErrorType::kErrorTypes)>
      collapse_errors = {0};
  size_t treated_pages_subreleased = 0;

  // TODO(287498389): Add latency histogram once we have a better idea of the
  // range of values.
  double collapse_time_total_cycles = 0;
  double collapse_time_max_cycles = 0;
  size_t collapse_intervals_skipped = 0;
  static absl::string_view ErrorTypeToString(CollapseErrorType type) {
    switch (type) {
      case CollapseErrorType::kENoMem:
        return "ETYPE_NOMEM";
      case CollapseErrorType::kEBusy:
        return "ETYPE_BUSY";
      case CollapseErrorType::kEInval:
        return "ETYPE_INVAL";
      case CollapseErrorType::kEAgain:
        return "ETYPE_AGAIN";
      case CollapseErrorType::kOther:
        return "ETYPE_OTHER";
      default:
        return "ETYPE_OTHER";
    }
  }

  static size_t ErrorTypeToIndex(CollapseErrorType type) {
    return static_cast<size_t>(type);
  }

  void UpdateCollapseErrorStats(int error_number) {
    switch (error_number) {
      case ENOMEM:
        ++collapse_errors[ErrorTypeToIndex(CollapseErrorType::kENoMem)];
        break;
      case EBUSY:
        ++collapse_errors[ErrorTypeToIndex(CollapseErrorType::kEBusy)];
        break;
      case EINVAL:
        ++collapse_errors[ErrorTypeToIndex(CollapseErrorType::kEInval)];
        break;
      case EAGAIN:
        ++collapse_errors[ErrorTypeToIndex(CollapseErrorType::kEAgain)];
        break;
      default:
        ++collapse_errors[ErrorTypeToIndex(CollapseErrorType::kOther)];
        break;
    }
  }

  HugePageTreatmentStats& operator+=(const HugePageTreatmentStats& rhs) {
    collapse_eligible += rhs.collapse_eligible;
    collapse_attempted += rhs.collapse_attempted;
    collapse_succeeded += rhs.collapse_succeeded;
    for (size_t i = 0; i < collapse_errors.size(); ++i) {
      collapse_errors[i] += rhs.collapse_errors[i];
    }
    collapse_time_total_cycles += rhs.collapse_time_total_cycles;
    // TODO(b/425749361): Add treated_pages_subreleased to the stats when we
    // start collecting cumulative stats.
    return *this;
  };
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
    const size_t kNativePagesInHugePage = kHugePageSize / GetPageSize();
    const int kStep = kNativePagesInHugePage / kBucketsInBetween;
    // Ensure that the number of native page buckets is at least the number of
    // buckets at a bound.
    TC_ASSERT_GE(kNativePagesInHugePage, kBucketsAtBounds);
    // First kBucketsAtBounds buckets have a step size of 1
    for (int i = 0; i <= kBucketsAtBounds &&
                    native_page_buckets_size_ < kNativePagesInHugePage;
         ++i) {
      native_page_bucket_bounds_[native_page_buckets_size_] = i;
      ++native_page_buckets_size_;
    }

    // All the buckets in between should increment with a step of
    // kNativePagesInHugePage / kBucketsInBetween
    for (int i = 0; i < kNativePagesInHugePage - kBucketsAtBounds; ++i) {
      int bound =
          native_page_bucket_bounds_[native_page_buckets_size_ - 1] + kStep;
      // We break early so that we can log histogram at the end with step 1
      if (bound >= kNativePagesInHugePage - kBucketsAtBounds) {
        break;
      }
      native_page_bucket_bounds_[native_page_buckets_size_] = bound;
      ++native_page_buckets_size_;
    }

    // End kBucketBoundsBuckets have a step size of 1
    for (int i = 0; i < kBucketsAtBounds; ++i) {
      int end_bound = kNativePagesInHugePage - kBucketsAtBounds + i;
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
  bool IsHugepageBacked(const TrackerType& tracker, PageFlagsBase& pageflags) {
    void* addr = tracker.location().start_addr();
    // TODO(b/28093874): Investigate if pageflags may be queried without
    // pageheap_lock.
    const bool is_hugepage_backed = pageflags.IsHugepageBacked(addr);
    return is_hugepage_backed;
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

    size_t treated_hugepages{};
    size_t hugepage_backed{};
    size_t total_pages{};
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

    if (IsHugepageBacked(pt, pageflags)) {
      ++records.hugepage_backed;
      if (pt.was_released()) {
        ++hugepage_backed_previously_released_;
      }
    }
    ++records.total_pages;

    PageTracker::HugePageResidencyState hugepage_residency_state =
        pt.GetHugePageResidencyState();
    if (hugepage_residency_state.entry_valid &&
        !hugepage_residency_state.maybe_hugepage_backed) {
      Residency::SinglePageBitmaps bitmaps = hugepage_residency_state.bitmaps;
      ++records
            .unbacked_histo[NativePageBucketNum(bitmaps.unbacked.CountBits())];
      ++records.swapped_histo[NativePageBucketNum(bitmaps.swapped.CountBits())];

      PageTracker::NativePageResidencyInfo info =
          pt.CountInfoInHugePage(bitmaps);
      ++records.free_unbacked_histo[NativePageBucketNum(info.n_free_unbacked)];
      ++records.free_swapped_histo[NativePageBucketNum(info.n_free_swapped)];
    }

    if (hugepage_residency_state.entry_valid) {
      ++records.treated_hugepages;
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

    PrintNativePageHisto(out, records.unbacked_histo, type,
                         "hps with a <= # of unbacked < b", 0);
    PrintNativePageHisto(out, records.swapped_histo, type,
                         "hps with a <= # of swapped < b", 0);
    PrintNativePageHisto(out, records.free_unbacked_histo, type,
                         "hps with a <= # of free AND unbacked < b", 0);
    PrintNativePageHisto(out, records.free_swapped_histo, type,
                         "hps with a <= # of free AND swapped < b", 0);

    out.printf("\nHugePageFiller: %zu of %s pages hugepage backed out of %zu.",
               records.hugepage_backed, TypeToStr(type), records.total_pages);

    out.printf("\nHugePageFiller: %zu of %s pages treated out of %zu.",
               records.treated_hugepages, TypeToStr(type), records.total_pages);

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
    PrintNativePageHisto(scoped, records.unbacked_histo, "unbacked_histogram",
                         0);
    PrintNativePageHisto(scoped, records.swapped_histo, "swapped_histogram", 0);
    PrintNativePageHisto(scoped, records.free_unbacked_histo,
                         "free_unbacked_histogram", 0);
    PrintNativePageHisto(scoped, records.free_swapped_histo,
                         "free_swapped_histogram", 0);
    PrintSampledTrackers(scoped, type, "sampled_trackers", records);
    scoped.PrintI64("total_pages", records.total_pages);
    scoped.PrintI64("num_pages_hugepage_backed", records.hugepage_backed);
    scoped.PrintI64("num_pages_treated", records.treated_hugepages);
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

  int NativePageBucketNum(size_t page) {
    auto it =
        std::upper_bound(native_page_bucket_bounds_,
                         native_page_bucket_bounds_ + buckets_size_, page);
    TC_CHECK_NE(it, native_page_bucket_bounds_);
    return it - native_page_bucket_bounds_ - 1;
  }

  void PrintNativePageHisto(Printer& out, Histo h, Type type,
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

  void PrintNativePageHisto(PbtxtRegion& hpaa, Histo h, absl::string_view key,
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

struct HugePageFillerOptions {
  bool use_preferential_collapse;
};

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
      MemoryTagFunction& set_anon_vma_name ABSL_ATTRIBUTE_LIFETIME_BOUND);

  HugePageFiller(
      Clock clock, MemoryTag tag,
      MemoryModifyFunction& unback ABSL_ATTRIBUTE_LIFETIME_BOUND,
      MemoryModifyFunction& unback_without_lock ABSL_ATTRIBUTE_LIFETIME_BOUND,
      MemoryModifyFunction& collapse ABSL_ATTRIBUTE_LIFETIME_BOUND,
      MemoryTagFunction& set_anon_vma_name ABSL_ATTRIBUTE_LIFETIME_BOUND,
      HugePageFillerOptions options);

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
  bool ShouldBackoffFromCollapse();

  // Based on the <latency> and <enomem_errors>, updates the max backoff delay.
  void UpdateMaxBackoffDelay(absl::Duration latency, int enomem_errors);

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
  // page, if `enable_release_free_swapped` is true.
  void TreatHugepageTrackers(bool enable_collapse,
                             bool enable_release_free_swapped,
                             bool use_userspace_collapse_heuristics,
                             PageFlagsBase* pageflags = nullptr,
                             Residency* residency = nullptr)
      ABSL_EXCLUSIVE_LOCKS_REQUIRED(pageheap_lock);

  // Utility function to release free pages from a given `page_tracker`
  // and handle accounting.
  Length HandleReleaseFree(PageTracker* page_tracker)
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

  // Functionality related to time series tracking.
  void UpdateFillerStatsTracker();
  using StatsTrackerType = SubreleaseStatsTracker<600>;
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
  bool preferential_collapse_;
  int max_backoff_delay_ = 1;
  int current_backoff_delay_ = 0;
  uintptr_t rng_ = 0;
};

inline typename PageTracker::PageAllocation PageTracker::Get(
    Length n, SpanAllocInfo span_alloc_info) {
  size_t index = free_.FindAndMark(n.raw_num());
  num_objects_ += span_alloc_info.objects_per_span;

  TC_ASSERT_EQ(released_by_page_.CountBits(), released_count_);

  size_t unbacked = 0;
  // If release_count_ == 0, CountBits will return 0 and ClearRange will be a
  // no-op (but will touch cachelines) due to the invariants guaranteed by
  // CountBits() == released_count_.
  //
  // This is a performance optimization, not a logical requirement.
  if (ABSL_PREDICT_FALSE(released_count_ > 0)) {
    unbacked = released_by_page_.CountBits(index, n.raw_num());
    released_by_page_.ClearRange(index, n.raw_num());
    TC_ASSERT_GE(released_count_, unbacked);
    released_count_ -= unbacked;
  }

  TC_ASSERT_EQ(released_by_page_.CountBits(), released_count_);
  return PageAllocation{location_.first_page() + Length(index),
                        Length(unbacked)};
}

inline void PageTracker::SetAnonVmaName(MemoryTagFunction& set_anon_vma_name,
                                        std::optional<absl::string_view> name) {
  set_anon_vma_name(Range(location_.first_page(), kPagesPerHugePage), name);
}

inline PageTracker::NativePageResidencyInfo PageTracker::CountInfoInHugePage(
    Residency::SinglePageBitmaps bitmaps) const {
  // TODO(b/424551232): Add support for the scenario when native page size is
  // larger than TCMalloc page size.
  const size_t kNativePagesInHugePage = kHugePageSize / GetPageSize();
  if (kNativePagesInHugePage < kPagesPerHugePage.raw_num()) {
    return {.n_free_swapped = 0, .n_free_unbacked = 0};
  }
  TC_ASSERT_LE(kNativePagesInHugePage, kMaxResidencyBits);

  Bitmap<kMaxResidencyBits> unbacked = bitmaps.unbacked;
  Bitmap<kMaxResidencyBits> swapped = bitmaps.swapped;
  Bitmap<kPagesPerHugePage.raw_num()> free = free_.bits();

  TC_ASSERT_EQ(kNativePagesInHugePage % kPagesPerHugePage.raw_num(), 0);
  const int shift = kNativePagesInHugePage / kPagesPerHugePage.raw_num();
  const int shift_bits = absl::bit_width<uint8_t>(shift - 1);
  TC_ASSERT_LT((kNativePagesInHugePage - 1) >> shift_bits,
               kPagesPerHugePage.raw_num());

  size_t swapped_idx = 0;
  size_t n_free_unbacked = 0;
  size_t n_free_swapped = 0;
  while (swapped_idx < kNativePagesInHugePage) {
    swapped_idx = swapped.FindSet(swapped_idx);
    if (swapped_idx >= kNativePagesInHugePage) {
      break;
    }
    // Compute the number of free pages that are swapped.
    // In free bitmap, 1 means used and 0 means free
    const int scaled_idx = swapped_idx >> shift_bits;
    if (!free.GetBit(scaled_idx)) {
      ++n_free_swapped;
    }
    ++swapped_idx;
  }

  size_t unbacked_idx = 0;
  while (unbacked_idx < kNativePagesInHugePage) {
    unbacked_idx = unbacked.FindSet(unbacked_idx);
    if (unbacked_idx >= kNativePagesInHugePage) {
      break;
    }
    // Compute the number of free pages that are unbacked
    const int scaled_idx = unbacked_idx >> shift_bits;
    if (!free.GetBit(scaled_idx)) {
      ++n_free_unbacked;
    }
    ++unbacked_idx;
  }
  return {.n_free_swapped = n_free_swapped, .n_free_unbacked = n_free_unbacked};
}

inline void PageTracker::Put(Range r, SpanAllocInfo span_alloc_info) {
  Length index = r.p - location_.first_page();
  free_.Unmark(index.raw_num(), r.n.raw_num());
  TC_ASSERT_GE(num_objects_, span_alloc_info.objects_per_span);
  num_objects_ -= span_alloc_info.objects_per_span;
}

inline Length PageTracker::ReleaseFree(MemoryModifyFunction& unback) {
  size_t count = 0;
  size_t index = 0;
  size_t n;
  // For purposes of tracking, pages which are not yet released are "free" in
  // the released_by_page_ bitmap.  We subrelease these pages in an iterative
  // process:
  //
  // 1.  Identify the next range of still backed pages.
  // 2.  Iterate on the free_ tracker within this range.  For any free range
  //     found, mark these as unbacked.
  // 3.  Release the subrange to the OS.
  while (released_by_page_.NextFreeRange(index, &index, &n)) {
    size_t free_index;
    size_t free_n;

    // Check for freed pages in this unreleased region.
    if (free_.NextFreeRange(index, &free_index, &free_n) &&
        free_index < index + n) {
      // If there is a free range which overlaps with [index, index+n), release
      // it.
      size_t end = std::min(free_index + free_n, index + n);

      // In debug builds, verify [free_index, end) is backed.
      size_t length = end - free_index;
      TC_ASSERT_EQ(released_by_page_.CountBits(free_index, length), 0);
      PageId p = location_.first_page() + Length(free_index);

      if (ABSL_PREDICT_TRUE(ReleasePages(Range(p, Length(length)), unback))) {
        // Mark pages as released.  Amortize the update to release_count_.
        released_by_page_.SetRange(free_index, length);
        count += length;
      }

      index = end;
    } else {
      // [index, index+n) did not have an overlapping range in free_, move to
      // the next backed range of pages.
      index += n;
    }
  }

  released_count_ += count;
  if (count > 0) {
    hugepage_residency_state_.maybe_hugepage_backed = false;
  }
  TC_ASSERT_LE(Length(released_count_), kPagesPerHugePage);
  TC_ASSERT_EQ(released_by_page_.CountBits(), released_count_);
  return Length(count);
}

inline MemoryModifyStatus PageTracker::Collapse(
    MemoryModifyFunction& collapse) {
  // TODO(b/287498389): Consider using an atomic variable instead of a lock to
  // store the being_collapsed state.
  {
    PageHeapSpinLockHolder l;
    // If the tracker is in the released state, we do no want to collapse it.
    if (released()) return {.success = false, .error_number = 0};
    TC_ASSERT(!BeingCollapsed());
    SetBeingCollapsed(/*value=*/true);
  }

  MemoryModifyStatus success =
      collapse(Range(location_.first_page(), kPagesPerHugePage));

  {
    PageHeapSpinLockHolder l;
    TC_ASSERT(!released());
    SetBeingCollapsed(/*value=*/false);
  }

  return success;
}

inline void PageTracker::AddSpanStats(SmallSpanStats* small,
                                      LargeSpanStats* large) const {
  size_t index = 0, n;

  while (free_.NextFreeRange(index, &index, &n)) {
    bool is_released = released_by_page_.GetBit(index);
    // Find the last bit in the run with the same state (set or cleared) as
    // index.
    size_t end;
    if (index >= kPagesPerHugePage.raw_num() - 1) {
      end = kPagesPerHugePage.raw_num();
    } else {
      end = is_released ? released_by_page_.FindClear(index + 1)
                        : released_by_page_.FindSet(index + 1);
    }
    n = std::min(end - index, n);
    TC_ASSERT_GT(n, 0);

    if (n < kMaxPages.raw_num()) {
      if (small != nullptr) {
        if (is_released) {
          small->returned_length[n]++;
        } else {
          small->normal_length[n]++;
        }
      }
    } else {
      if (large != nullptr) {
        large->spans++;
        if (is_released) {
          large->returned_pages += Length(n);
        } else {
          large->normal_pages += Length(n);
        }
      }
    }

    index += n;
  }
}

inline bool PageTracker::empty() const { return free_.used() == 0; }

inline Length PageTracker::free_pages() const {
  return kPagesPerHugePage - used_pages();
}

template <class TrackerType>
inline HugePageFiller<TrackerType>::HugePageFiller(
    MemoryTag tag, MemoryModifyFunction& unback,
    MemoryModifyFunction& unback_without_lock, MemoryModifyFunction& collapse,
    MemoryTagFunction& set_anon_vma_name)
    : HugePageFiller(
          Clock{.now = absl::base_internal::CycleClock::Now,
                .freq = absl::base_internal::CycleClock::Frequency},
          tag, unback, unback_without_lock, collapse, set_anon_vma_name,
          HugePageFillerOptions{
              .use_preferential_collapse =
                  system_allocator_internal::preferential_collapse()}) {}

// For testing with mock clock
template <class TrackerType>
inline HugePageFiller<TrackerType>::HugePageFiller(
    Clock clock, MemoryTag tag, MemoryModifyFunction& unback,
    MemoryModifyFunction& unback_without_lock, MemoryModifyFunction& collapse,
    MemoryTagFunction& set_anon_vma_name, HugePageFillerOptions options)
    : size_(NHugePages(0)),
      fillerstats_tracker_(clock, absl::Minutes(10), absl::Minutes(5)),
      clock_(clock),
      tag_(tag),
      unback_(unback),
      unback_without_lock_(unback_without_lock),
      collapse_(collapse),
      set_anon_vma_name_(set_anon_vma_name),
      preferential_collapse_(options.use_preferential_collapse) {
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

// TODO: b/425749361 - Add unit tests for subclasses.
class HugePageTreatment {
 public:
  virtual ~HugePageTreatment() = default;

  // Called on every page tracker. It assesses the top N trackers for this
  // treatment's criteria.
  virtual void SelectEligibleTrackers(PageTracker& pt)
      ABSL_EXCLUSIVE_LOCKS_REQUIRED(pageheap_lock) = 0;

  // Returns the number of trackers that have been selected for treatment.
  virtual int num_valid_trackers() const = 0;

  // Applies treatment to the selected trackers outside of pageheap lock. The
  // HugePageFiller will take care of preventing these trackers from going out
  // of scope/being freed while the page heap lock is unlocked
  virtual void Treat() ABSL_LOCKS_EXCLUDED(pageheap_lock) = 0;

  // Restores and records the state from treatment to the trackers under
  // pageheap lock.
  virtual void Restore() ABSL_EXCLUSIVE_LOCKS_REQUIRED(pageheap_lock) = 0;
};

inline size_t RoundDown(size_t metric, size_t align) {
  return metric & ~(align - 1);
}

class SampledTrackerTreatment final : public HugePageTreatment {
 public:
  explicit SampledTrackerTreatment(Clock clock, MemoryTag tag,
                                   MemoryTagFunction& set_anon_vma_name)
      : clock_(clock), tag_(tag), set_anon_vma_name_(set_anon_vma_name) {}
  ~SampledTrackerTreatment() override = default;

  static void operator delete(void*) { __builtin_trap(); }

  // Trying to apply treatments to the sampled trackers involves three
  // steps:
  // 1. Collect up to kTotalTrackersToScan trackers using
  //    SelectEligibleTrackers. Eligible pages here include:
  //   a. The trackers that were sampled for tagging when they were allocated.
  //   b. The trackers that were last scanned more than kRecordInterval ago.
  // 2. Apply the treatment using Treat. It encodes tracker features, such as
  //    the longest free range, number of allocations, etc. into a string and
  //    uses it to name the memory tracked by the tracker. This is done outside
  //    of the pageheap lock.
  // 3. Acquire the pageheap lock and restore the recorded state using Restore
  //    (e.g. reset the dont_free_tracker bit).

  void SelectEligibleTrackers(PageTracker& pt) override {
    if (num_valid_trackers_ >= kTotalTrackersToScan) return;

    // Collect all the addresses under pageheap lock that are to be sampled for
    // tagging, and that were last scanned more than kRecordInterval ago.
    const absl::Duration kRecordInterval = absl::Minutes(5);
    PageTracker::TagState tagged_state = pt.GetTagState();
    if (!tagged_state.sampled_for_tagging) return;
    double clock_now = clock_.now();
    double clock_freq = clock_.freq();
    double elapsed = std::max<double>(clock_now - tagged_state.record_time, 0);
    if (elapsed > absl::ToDoubleSeconds(kRecordInterval) * clock_freq) {
      selected_trackers_[num_valid_trackers_] = {
          &pt,
          pt.longest_free_range().raw_num(),
          pt.nallocs(),
          pt.nobjects(),
          pt.HasDenseSpans(),
          pt.released()};
      pt.SetTagState({.sampled_for_tagging = true, .record_time = clock_now});
      ++num_valid_trackers_;
      // Setting this bit makes sure that the tracker is not freed under us
      // when the pageheap lock is unlocked and we are in the middle of
      // applying the treatment.
      pt.SetDontFreeTracker(/*value=*/true);
    }
  }

  int num_valid_trackers() const override { return num_valid_trackers_; }

  void Treat() ABSL_LOCKS_EXCLUDED(pageheap_lock) override {
    TC_ASSERT_LE(num_valid_trackers_, kTotalTrackersToScan);
    // Record all the features we want to record, encode that into a string,
    // and use it to name the allocated region.
    for (int i = 0; i < num_valid_trackers_; ++i) {
      PageTracker* tracker = selected_trackers_[i].tracker;
      TC_ASSERT_NE(tracker, nullptr);
      const size_t lfr = selected_trackers_[i].lfr;
      const size_t nallocs = selected_trackers_[i].nallocs;
      const size_t nobjects = selected_trackers_[i].nobjects;
      const bool has_dense_spans = selected_trackers_[i].has_dense_spans;
      const bool released = selected_trackers_[i].released;

      char name[256];
      absl::SNPrintF(
          name, sizeof(name),
          "tcmalloc_region_%s_page_%d_lfr_%d_nallocs_%d_nobjects_%d_dense_%d_"
          "released_%d",
          MemoryTagToLabel(tag_), kPageSize, RoundDown(lfr, /*align=*/16),
          RoundDown(nallocs, /*align=*/16), absl::bit_ceil(nobjects),
          has_dense_spans, released);
      tracker->SetAnonVmaName(set_anon_vma_name_, name);
    }
  }

  void Restore() ABSL_EXCLUSIVE_LOCKS_REQUIRED(pageheap_lock) override {
    TC_ASSERT_LE(num_valid_trackers_, kTotalTrackersToScan);
    for (int i = 0; i < num_valid_trackers_; ++i) {
      PageTracker* tracker = selected_trackers_[i].tracker;
      TC_ASSERT_NE(tracker, nullptr);
      tracker->SetDontFreeTracker(/*value=*/false);
    }
  }

 private:
  static constexpr size_t kTotalTrackersToScan = 64;
  Clock clock_;
  MemoryTag tag_;
  MemoryTagFunction& set_anon_vma_name_;

  struct TrackerState {
    PageTracker* tracker;
    size_t lfr;
    size_t nallocs;
    size_t nobjects;
    bool has_dense_spans;
    bool released;
  };
  using TrackerArray = std::array<TrackerState, kTotalTrackersToScan>;
  TrackerArray selected_trackers_;
  int num_valid_trackers_ = 0;
};

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

template <class TrackerType>
class HugePageUnbackedTrackerTreatment final : public HugePageTreatment {
 public:
  // TODO(b/287498389): pass pageflags and residency as reference, as we have
  // multiple treatments that rely on querying them.
  explicit HugePageUnbackedTrackerTreatment(
      Clock clock, PageFlagsBase* pageflags, Residency* residency,
      MemoryModifyFunction& collapse, HugePageFiller<TrackerType>& page_filler,
      bool enable_collapse, bool enable_release_free_swap,
      bool use_userspace_collapse_heuristics)
      : clock_(clock),
        pageflags_(pageflags),
        residency_(residency),
        collapse_(collapse),
        page_filler_(page_filler),
        enable_collapse_(enable_collapse),
        enable_release_free_swap_(enable_release_free_swap),
        use_userspace_collapse_heuristics_(use_userspace_collapse_heuristics) {}
  ~HugePageUnbackedTrackerTreatment() override = default;

  static void operator delete(void*) { __builtin_trap(); }

  // Trying to apply treatments to the non-hugepage backed pages involves three
  // steps:
  // 1. Collect up to kTotalTrackersToScan trackers using
  //    SelectEligibleTrackers. Eligible pages here include:
  //   a. The trackers that manage pages that either were hugepage backed or
  //      were previously successfully collapsed.
  //   b. The trackers that were never scanned before.
  //   c. The trackers that were last scanned more than kRecordInterval ago.
  // 2. Release the pageheap lock and obtain the residency and pageflags
  //    information for the collected trackers. Attempt to apply treatments to
  //    the pages that aren't hugepage backed. In case of userspace collapse,
  //    it attempts to collapse pages that are composed of the number of
  //    unbacked and swapped pages less than kMaxUnbackedPagesForCollapse and
  //    kMaxSwappedPagesForCollapse respectively.
  // 3. Acquire the pageheap lock and restore the recorded state using Restore
  //    (e.g. update the residency information in the trackers).
  static bool CompareForHugePageTreatment(PageTracker* a, PageTracker* b) {
    TC_ASSERT_NE(a, nullptr);
    TC_ASSERT_NE(b, nullptr);
    if (a->nobjects() > b->nobjects()) return true;
    if (a->nobjects() < b->nobjects()) return false;

    // All things considered equal, prefer collapsing dense spans.
    if (!a->HasDenseSpans()) return false;
    return !b->HasDenseSpans();
  }

  void SelectEligibleTrackers(PageTracker& pt) override {
    if (num_valid_trackers_ >= kTotalTrackersToScan &&
        !use_userspace_collapse_heuristics_) {
      return;
    }

    auto PushCandidate = [&](PageTracker& pt) GOOGLE_MALLOC_SECTION {
      if (num_valid_trackers_ < kTotalTrackersToScan) {
        selected_trackers_[num_valid_trackers_] = &pt;
        ++num_valid_trackers_;
        pt.SetDontFreeTracker(/*value=*/true);
        if (num_valid_trackers_ == kTotalTrackersToScan) {
          std::make_heap(selected_trackers_.begin(),
                         selected_trackers_.begin() + num_valid_trackers_,
                         CompareForHugePageTreatment);
        }
        return;
      }

      if (CompareForHugePageTreatment(selected_trackers_[0], &pt)) {
        return;
      }
      std::pop_heap(selected_trackers_.begin(),
                    selected_trackers_.begin() + num_valid_trackers_,
                    CompareForHugePageTreatment);
      PageTracker* last = selected_trackers_[num_valid_trackers_ - 1];
      TC_ASSERT_NE(last, nullptr);
      pt.SetDontFreeTracker(/*value=*/true);
      last->SetDontFreeTracker(/*value=*/false);
      selected_trackers_[num_valid_trackers_ - 1] = &pt;
      std::push_heap(selected_trackers_.begin(),
                     selected_trackers_.begin() + num_valid_trackers_,
                     CompareForHugePageTreatment);
    };

    PageTracker::HugePageResidencyState state = pt.GetHugePageResidencyState();
    if (state.maybe_hugepage_backed) return;

    if (!state.entry_valid) {
      PushCandidate(pt);
      return;
    }
    double elapsed = std::max<double>(clock_.now() - state.record_time, 0);
    if (elapsed > absl::ToDoubleSeconds(kRecordInterval) * clock_.freq()) {
      PushCandidate(pt);
    }
  }

  int num_valid_trackers() const override { return num_valid_trackers_; }

  bool tracker_list_full() const {
    return num_valid_trackers_ >= kTotalTrackersToScan;
  }

  void Treat() ABSL_LOCKS_EXCLUDED(pageheap_lock) override {
    // Obtain residency information for the collected addresses.
    PageFlagsBase* pf = pageflags_;
    PageFlags pageflags_obj;
    if (pf == nullptr) {
      pf = &pageflags_obj;
    }

    Residency* res = residency_;
    ResidencyPageMap residency_obj;
    if (res == nullptr) {
      res = &residency_obj;
    }

    TC_ASSERT_LE(num_valid_trackers_, kTotalTrackersToScan);
    if (enable_collapse_) {
      treatment_stats_.collapse_eligible += num_valid_trackers_;
    }
    // Outside of the pageheap lock, obtain the residency and pageflags
    // information for the collected addresses. Try to collapse the pages that
    // aren't hugepage backed, and for which, the number of unbacked and swapped
    // pages are less than kMaxUnbackedPagesForCollapse and
    // kMaxSwappedPagesForCollapse respectively.
    const double max_collapse_cycles =
        absl::ToDoubleSeconds(kMaxCollapseLatencyThreshold) * clock_.freq();
    for (int i = 0; i < num_valid_trackers_; ++i) {
      PageTracker::HugePageResidencyState state;
      PageTracker* tracker = selected_trackers_[i];
      TC_ASSERT_NE(tracker, nullptr);
      bool is_hugepage = pf->IsHugepageBacked(tracker->location().start_addr());
      state.entry_valid = true;
      state.record_time = clock_.now();

      // If the address is not hugepage backed, obtain the residency
      // information.
      state.maybe_hugepage_backed = is_hugepage;
      if (!is_hugepage) {
        state.bitmaps =
            res->GetUnbackedAndSwappedBitmaps(tracker->location().start_addr());

        const bool backoff =
            treatment_stats_.collapse_time_max_cycles > max_collapse_cycles;
        if (enable_collapse_ && !backoff) {
          size_t total_swapped_pages = state.bitmaps.swapped.CountBits();
          size_t total_unbacked_pages = state.bitmaps.unbacked.CountBits();
          if (total_swapped_pages < kMaxSwappedPagesForCollapse &&
              total_unbacked_pages < kMaxUnbackedPagesForCollapse) {
            state.maybe_hugepage_backed = TryUserspaceCollapse(tracker);
          }
        }
      }
      residency_states_[i].tracker = tracker;
      residency_states_[i].tracker_state = state;
    }
  }

  void Restore() ABSL_EXCLUSIVE_LOCKS_REQUIRED(pageheap_lock) override {
    TC_ASSERT_LE(num_valid_trackers_, kTotalTrackersToScan);
    for (int i = 0; i < num_valid_trackers_; ++i) {
      PageTracker* tracker = residency_states_[i].tracker;
      TC_ASSERT_NE(tracker, nullptr);
      tracker->SetDontFreeTracker(/*value=*/false);
      tracker->SetHugePageResidencyState(residency_states_[i].tracker_state);
      if (residency_states_[i].tracker_state.maybe_hugepage_backed) {
        continue;
      }

      // It's possible that all the pages on the hugepage were freed when we had
      // released the pageheap lock. Check that the longest free range is less
      // than kPagesPerHugePage to make sure it's valid to release from that
      // tracker.
      if (enable_release_free_swap_ &&
          !residency_states_[i].tracker_state.bitmaps.swapped.IsZero() &&
          !tracker->fully_freed()) {
        Length released_length = page_filler_.HandleReleaseFree(tracker);
        if (released_length > Length(0)) {
          treatment_stats_.treated_pages_subreleased +=
              released_length.raw_num();
        }
      }
    }
  }

  HugePageTreatmentStats GetStats() const { return treatment_stats_; }

  void UpdateHugePageTreatmentStats(HugePageTreatmentStats& stats) {
    stats += treatment_stats_;
    // TODO(b/425749361): Roll this up to the overloaded operator once we start
    // reporting cumulative treated_pages_subreleased stat.
    stats.treated_pages_subreleased =
        treatment_stats_.treated_pages_subreleased;
    stats.collapse_time_max_cycles =
        std::max(stats.collapse_time_max_cycles,
                 treatment_stats_.collapse_time_max_cycles);
  }

 private:
  bool TryUserspaceCollapse(PageTracker* tracker) {
    double before = clock_.now();
    MemoryModifyStatus ret = tracker->Collapse(collapse_);
    double after = clock_.now();
    double elapsed = std::max<double>(after - before, 0);
    treatment_stats_.collapse_time_total_cycles += elapsed;
    treatment_stats_.collapse_time_max_cycles =
        std::max(elapsed, treatment_stats_.collapse_time_max_cycles);
    treatment_stats_.collapse_attempted++;
    if (ret.success) {
      treatment_stats_.collapse_succeeded++;
    } else {
      // If the collapsed operation failed, errno should have been set.
      treatment_stats_.UpdateCollapseErrorStats(ret.error_number);
    }
    return ret.success;
  }

  static constexpr size_t kTotalTrackersToScan = 64;
  static constexpr absl::Duration kRecordInterval = absl::Minutes(5);
  static constexpr size_t kMaxSwappedPagesForCollapse = 128;
  static constexpr size_t kMaxUnbackedPagesForCollapse = 64;

  Clock clock_;
  PageFlagsBase* pageflags_;
  Residency* residency_;
  MemoryModifyFunction& collapse_;

  using TrackerArray = std::array<PageTracker*, kTotalTrackersToScan>;
  TrackerArray selected_trackers_;
  int num_valid_trackers_ = 0;

  struct ResidencyState {
    PageTracker* tracker;
    PageTracker::HugePageResidencyState tracker_state;
  };
  std::array<ResidencyState, kTotalTrackersToScan> residency_states_;
  HugePageTreatmentStats treatment_stats_;
  HugePageFiller<TrackerType>& page_filler_;
  bool enable_collapse_;
  bool enable_release_free_swap_;
  bool use_userspace_collapse_heuristics_;
};

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
void HugePageFiller<TrackerType>::UpdateMaxBackoffDelay(absl::Duration latency,
                                                        int enomem_errors) {
  if (preferential_collapse_ && enomem_errors > 0) {
    // Backoff more aggressively if we encounter ENOMEM errors.
    max_backoff_delay_ = std::min(max_backoff_delay_ << 4, kMaxBackoffDelay);
    return;
  }

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
    bool enable_collapse, bool enable_release_free_swapped,
    bool use_userspace_collapse_heuristics, PageFlagsBase* pageflags,
    Residency* residency) {
  if (enable_collapse && ShouldBackoffFromCollapse()) {
    enable_collapse = false;
    ++treatment_stats_.collapse_intervals_skipped;
  }
  const bool collect_non_hugepage_trackers =
      enable_collapse || enable_release_free_swapped;
  SampledTrackerTreatment sampled_tracker_treatment(clock_, tag_,
                                                    set_anon_vma_name_);
  HugePageUnbackedTrackerTreatment<TrackerType> unbacked_tracker_treatment(
      clock_, pageflags, residency, collapse_, *this, enable_collapse,
      enable_release_free_swapped, use_userspace_collapse_heuristics);

  // Collect up to kTotalTrackersToScan trackers from the regular sparse and
  // dense lists. if enable_release_free_swapped is true, we also collect
  // trackers from regular_alloc_partial_released_ and donated_alloc_.
  if (enable_release_free_swapped) {
    regular_alloc_partial_released_[AccessDensityPrediction::kSparse].Iter(
        [&](TrackerType& pt) GOOGLE_MALLOC_SECTION {
          unbacked_tracker_treatment.SelectEligibleTrackers(pt);
        },
        /*start=*/kChunks);

    regular_alloc_partial_released_[AccessDensityPrediction::kDense].Iter(
        [&](TrackerType& pt) GOOGLE_MALLOC_SECTION {
          unbacked_tracker_treatment.SelectEligibleTrackers(pt);
        },
        /*start=*/kChunks);

    donated_alloc_.Iter(
        [&](TrackerType& pt) GOOGLE_MALLOC_SECTION {
          unbacked_tracker_treatment.SelectEligibleTrackers(pt);
        },
        /*start=*/0);
  }

  regular_alloc_[AccessDensityPrediction::kDense].Iter(
      [&](TrackerType& pt) GOOGLE_MALLOC_SECTION {
        sampled_tracker_treatment.SelectEligibleTrackers(pt);
        if (collect_non_hugepage_trackers) {
          unbacked_tracker_treatment.SelectEligibleTrackers(pt);
        }
      },
      /*start=*/0);

  regular_alloc_[AccessDensityPrediction::kSparse].Iter(
      [&](TrackerType& pt) GOOGLE_MALLOC_SECTION {
        sampled_tracker_treatment.SelectEligibleTrackers(pt);
        if (collect_non_hugepage_trackers) {
          unbacked_tracker_treatment.SelectEligibleTrackers(pt);
        }
      },
      /*start=*/0);

  regular_alloc_partial_released_[AccessDensityPrediction::kSparse].Iter(
      [&](TrackerType& pt) GOOGLE_MALLOC_SECTION {
        sampled_tracker_treatment.SelectEligibleTrackers(pt);
      },
      /*start=*/0);

  regular_alloc_partial_released_[AccessDensityPrediction::kDense].Iter(
      [&](TrackerType& pt) GOOGLE_MALLOC_SECTION {
        sampled_tracker_treatment.SelectEligibleTrackers(pt);
      },
      /*start=*/0);
  regular_alloc_released_[AccessDensityPrediction::kSparse].Iter(
      [&](TrackerType& pt) GOOGLE_MALLOC_SECTION {
        sampled_tracker_treatment.SelectEligibleTrackers(pt);
      },
      /*start=*/0);

  regular_alloc_released_[AccessDensityPrediction::kDense].Iter(
      [&](TrackerType& pt) GOOGLE_MALLOC_SECTION {
        sampled_tracker_treatment.SelectEligibleTrackers(pt);
      },
      /*start=*/0);

  pageheap_lock.unlock();
  sampled_tracker_treatment.Treat();
  unbacked_tracker_treatment.Treat();

  HugePageTreatmentStats stats = unbacked_tracker_treatment.GetStats();
  if (stats.collapse_attempted > 0) {
    absl::Duration max_collapse_latency = absl::Milliseconds(
        stats.collapse_time_max_cycles * 1000 / clock_.freq());
    int enomem_errors =
        stats.collapse_errors[HugePageTreatmentStats::ErrorTypeToIndex(
            CollapseErrorType::kENoMem)];
    UpdateMaxBackoffDelay(max_collapse_latency, enomem_errors);
  }

  // Lock the pageheap lock and update residency information in the tracker.
  pageheap_lock.lock();
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
  stats.used_pages_in_subreleased_huge_pages =
      n_used_released_[AccessDensityPrediction::kDense] +
      n_used_released_[AccessDensityPrediction::kSparse] +
      n_used_partial_released_[AccessDensityPrediction::kDense] +
      n_used_partial_released_[AccessDensityPrediction::kSparse];
  stats.huge_pages[StatsTrackerType::kDonated] = donated_alloc_.size();
  for (const AccessDensityPrediction type :
       {AccessDensityPrediction::kDense, AccessDensityPrediction::kSparse}) {
    stats.huge_pages[StatsTrackerType::kRegular] += regular_alloc_[type].size();
    stats.huge_pages[StatsTrackerType::kPartialReleased] +=
        regular_alloc_partial_released_[type].size();
    stats.huge_pages[StatsTrackerType::kReleased] +=
        regular_alloc_released_[type].size();
  }
  stats.num_pages_subreleased = subrelease_stats_.num_pages_subreleased;
  stats.num_partial_alloc_pages_subreleased =
      subrelease_stats_.num_partial_alloc_pages_subreleased;
  stats.num_hugepages_broken = subrelease_stats_.num_hugepages_broken;
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

  if (!pt->released()) {
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

  if (!pt->released()) {
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
