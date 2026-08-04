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

#ifndef TCMALLOC_HUGE_PAGE_TREATMENT_H_
#define TCMALLOC_HUGE_PAGE_TREATMENT_H_

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

// Interval for Page Tracker treatment.
constexpr absl::Duration kRecordInterval = absl::Minutes(5);

enum class CollapseErrorType : size_t {
  kENoMem = 0,
  kEBusy,
  kEInval,
  kEAgain,
  kEIntr,
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
  size_t treated_pages_unbacked_subreleased = 0;
  size_t treated_pages_stale_subreleased = 0;

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
      case CollapseErrorType::kEIntr:
        return "ETYPE_INTR";
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
      case EINTR:
        ++collapse_errors[ErrorTypeToIndex(CollapseErrorType::kEIntr)];
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

// This tracks a set of unfilled hugepages, and fulfills allocations
// with a goal of filling some hugepages as tightly as possible and emptying
// out the remainder.
template <class TrackerType>
class HugePageFiller;

inline size_t RoundDown(size_t metric, size_t align) {
  return metric & ~(align - 1);
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
      pt.SetDontFreeTracker(HugePageTreatmentType::kSampled);
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
      tracker->ClearDontFreeTracker(HugePageTreatmentType::kSampled);
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
class HugePageUnbackedTrackerTreatment final : public HugePageTreatment {
 public:
  // TODO(b/287498389): pass pageflags and residency as reference, as we have
  // multiple treatments that rely on querying them.
  explicit HugePageUnbackedTrackerTreatment(
      Clock clock, PageFlagsBase* pageflags, Residency* residency,
      MemoryModifyFunction& collapse, HugePageFiller<TrackerType>& page_filler,
      EnableCollapse enable_collapse,
      SubreleaseUnbackedMode subrelease_unbacked_mode,
      EnableUnfilteredCollapse enable_unfiltered_collapse,
      ReleaseStalePages release_stale_pages)
      : clock_(clock),
        pageflags_(pageflags),
        residency_(residency),
        collapse_(collapse),
        page_filler_(page_filler),
        enable_collapse_(enable_collapse),
        subrelease_unbacked_mode_(subrelease_unbacked_mode),
        enable_unfiltered_collapse_(enable_unfiltered_collapse),
        release_stale_pages_(release_stale_pages) {}
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

    auto PushCandidate = [&](PageTracker& pt) GOOGLE_MALLOC_SECTION {
      if (num_valid_trackers_ < kTotalTrackersToScan) {
        selected_trackers_[num_valid_trackers_] = &pt;
        ++num_valid_trackers_;
        pt.SetDontFreeTracker(HugePageTreatmentType::kCollapse);
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
      pt.SetDontFreeTracker(HugePageTreatmentType::kCollapse);
      last->ClearDontFreeTracker(HugePageTreatmentType::kCollapse);
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
    if (enable_collapse_ == EnableCollapse::kEnabled) {
      treatment_stats_.collapse_eligible += num_valid_trackers_;
    }
    // Outside of the pageheap lock, obtain the residency and pageflags
    // information for the collected addresses. Try to collapse the pages that
    // aren't hugepage backed, and for which, the number of unbacked and swapped
    // pages are less than kMaxUnbackedPagesForCollapse and
    // kMaxSwappedPagesForCollapse respectively.
    const double max_collapse_cycles =
        absl::ToDoubleSeconds(kMaxCollapseLatencyThreshold) * clock_.freq();
    const size_t pages_per_huge_page = kHugePageSize / GetPageSize();
    for (int i = 0; i < num_valid_trackers_; ++i) {
      PageTracker::HugePageResidencyState state;
      PageTracker* tracker = selected_trackers_[i];
      TC_ASSERT_NE(tracker, nullptr);
      // Assume element is hugepage if we can't read the value.
      bool is_hugepage =
          pf->IsHugepageBacked(tracker->location().start_addr()).value_or(true);

      state.entry_valid = true;
      state.record_time = clock_.now();

      // If the address is not hugepage backed, obtain the residency
      // information.
      state.maybe_hugepage_backed = is_hugepage;
      if (!is_hugepage) {
        auto bitmaps =
            res->GetUnbackedAndSwappedBitmaps(tracker->location().start_addr());
        state.unbacked = Scale<kPagesPerHugePage.raw_num()>(
            bitmaps.unbacked, pages_per_huge_page, ReductionOp::kAll);
        state.swapped = Scale<kPagesPerHugePage.raw_num()>(
            bitmaps.swapped, pages_per_huge_page, ReductionOp::kAny);
        auto single_page_bitmaps =
            pf->GetSinglePageBitmaps(tracker->location().start_addr());
        state.stale = Scale<kPagesPerHugePage.raw_num()>(
            single_page_bitmaps.stale, pages_per_huge_page, ReductionOp::kAny);

        const bool backoff =
            treatment_stats_.collapse_time_max_cycles > max_collapse_cycles;
        if (enable_collapse_ == EnableCollapse::kEnabled && !backoff) {
          bool should_collapse =
              enable_unfiltered_collapse_ ==
                  EnableUnfilteredCollapse::kEnabled ||
              (bitmaps.swapped.CountBits() < kMaxSwappedPagesForCollapse &&
               bitmaps.unbacked.CountBits() < kMaxUnbackedPagesForCollapse);
          if (should_collapse) {
            state.maybe_hugepage_backed = TryUserspaceCollapse(tracker);
          } else {
            state.collapse_skipped = true;
          }
        } else if (enable_collapse_ == EnableCollapse::kEnabled && backoff) {
          state.collapse_skipped = true;
          state.collapse_skipped_due_to_backoff = true;
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
      tracker->ClearDontFreeTracker(HugePageTreatmentType::kCollapse);
      if (tracker->fully_freed()) {
        continue;
      }
      tracker->SetHugePageResidencyState(residency_states_[i].tracker_state);
      if (residency_states_[i].tracker_state.maybe_hugepage_backed) {
        if (subrelease_unbacked_mode_ == SubreleaseUnbackedMode::kEnabled) {
          page_filler_.OnCollapseSuccess(tracker);
        }
        continue;
      }

      // It's possible that all the pages on the hugepage were freed when we had
      // released the pageheap lock. Check that the longest free range is less
      // than kPagesPerHugePage to make sure it's valid to release from that
      // tracker.
      if (!residency_states_[i].tracker_state.swapped.IsZero()) {
        // TODO: b/425749361 - Clear swapped bit for pages that were freed.
        Length released_length = page_filler_.HandleReleaseFree(tracker);
        if (released_length > Length(0)) {
          treatment_stats_.treated_pages_subreleased +=
              released_length.raw_num();
        }
      } else if (release_stale_pages_ == ReleaseStalePages::kEnabled &&
                 !residency_states_[i].tracker_state.stale.IsZero()) {
        Length released_length = page_filler_.HandleReleaseFree(tracker);
        if (released_length > Length(0)) {
          treatment_stats_.treated_pages_stale_subreleased +=
              released_length.raw_num();
        }
      }

      if (subrelease_unbacked_mode_ == SubreleaseUnbackedMode::kEnabled) {
        Length released_length = page_filler_.HandleUnbackedHugePage(
            tracker, residency_states_[i].tracker_state.unbacked);
        if (released_length > Length(0)) {
          treatment_stats_.treated_pages_unbacked_subreleased +=
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
    stats.treated_pages_unbacked_subreleased =
        treatment_stats_.treated_pages_unbacked_subreleased;
    stats.treated_pages_stale_subreleased =
        treatment_stats_.treated_pages_stale_subreleased;
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
  EnableCollapse enable_collapse_;
  SubreleaseUnbackedMode subrelease_unbacked_mode_;

  EnableUnfilteredCollapse enable_unfiltered_collapse_;
  ReleaseStalePages release_stale_pages_;
};

}  // namespace tcmalloc_internal
}  // namespace tcmalloc
GOOGLE_MALLOC_SECTION_END

#endif  // TCMALLOC_HUGE_PAGE_TREATMENT_H_
