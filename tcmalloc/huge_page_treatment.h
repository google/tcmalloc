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
#include "tcmalloc/internal/explicitly_constructed.h"
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

// Upper bound on the trackers recorded as hugepage backed more than
// kRecordInterval ago that one treatment pass re-observes, oldest record
// first.  Bounds the pageflags reads a pass makes on a large, fully
// hugepage-backed heap (one 4 KiB pread per tracker); the remainder wait for a
// later pass.  At one pass per 5 s this sweeps 512 trackers (1 GiB) every 5 s,
// so a heap of N hugepage-backed trackers re-observes each of them every
// max(kRecordInterval, N / 100 s).
constexpr size_t kTrackersToReobserve = 512;

// Upper bound on the re-observed trackers found split since they were recorded
// as hugepage backed whose residency a pass records.  Each such tracker needs
// a full residency record; the rest keep their stale record, so they stay the
// oldest and are re-observed first by a later pass.
constexpr size_t kReobservedSplitsToRecord = 128;

enum class CollapseErrorType : size_t {
  kENoMem = 0,
  kEBusy,
  kEInval,
  kEAgain,
  kEIntr,
  kEAcces,
  kEFault,
  kRejected,
  kOther,
  kErrorTypes
};

struct HugePageTreatmentStats {
  HugeLength collapse_eligible;
  HugeLength collapse_attempted;
  HugeLength collapse_succeeded;
  HugeLength collapse_donated_succeeded;
  std::array<size_t, static_cast<size_t>(CollapseErrorType::kErrorTypes)>
      collapse_errors = {0};
  Length treated_pages_subreleased;
  Length treated_pages_unbacked_subreleased;
  Length total_treated_pages_unbacked_subreleased;
  Length treated_pages_stale_subreleased;

  // TODO(287498389): Add latency histogram once we have a better idea of the
  // range of values.
  double collapse_time_total_cycles = 0;
  double collapse_time_max_cycles = 0;
  size_t collapse_intervals_skipped = 0;
  [[nodiscard]] static absl::string_view ErrorTypeToString(
      CollapseErrorType type) {
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
      case CollapseErrorType::kEAcces:
        return "ETYPE_ACCES";
      case CollapseErrorType::kEFault:
        return "ETYPE_FAULT";
      case CollapseErrorType::kRejected:
        return "ETYPE_REJECTED";
      case CollapseErrorType::kOther:
        return "ETYPE_OTHER";
      default:
        return "ETYPE_OTHER";
    }
  }

  [[nodiscard]] static size_t ErrorTypeToIndex(CollapseErrorType type) {
    return static_cast<size_t>(type);
  }

  void UpdateCollapseErrorStats(int error_number) {
    switch (error_number) {
      case 0:
        ++collapse_errors[ErrorTypeToIndex(CollapseErrorType::kRejected)];
        break;
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
      case EACCES:
        ++collapse_errors[ErrorTypeToIndex(CollapseErrorType::kEAcces)];
        break;
      case EFAULT:
        ++collapse_errors[ErrorTypeToIndex(CollapseErrorType::kEFault)];
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
    collapse_donated_succeeded += rhs.collapse_donated_succeeded;
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

[[nodiscard]] inline size_t RoundDown(size_t metric, size_t align) {
  return metric & ~(align - 1);
}

inline constexpr size_t kMaxAnonVmaNameSize = 80;

inline size_t FormatSampledTrackerVmaName(absl::Span<char> buffer,
                                          MemoryTag tag, size_t page_size,
                                          Length lfr, size_t nallocs,
                                          size_t nobjects, bool has_dense_spans,
                                          bool released) {
  TC_ASSERT_GE(buffer.size(), kMaxAnonVmaNameSize);
  return absl::SNPrintF(
      buffer.data(), std::min(buffer.size(), kMaxAnonVmaNameSize),
      "tcmalloc_region_%s_pg_%zu_lfr_%zu_na_%zu_no_%zu_d_%d_r_%d",
      MemoryTagToLabel(tag), page_size, RoundDown(lfr.raw_num(), /*align=*/16),
      RoundDown(nallocs, /*align=*/16),
      nobjects == 0 ? 0 : absl::bit_ceil(nobjects), has_dense_spans, released);
}

// TODO: b/425749361 - Add unit tests for subclasses.
class HugePageTreatment {
 public:
  virtual ~HugePageTreatment() = default;

  // Called on every page tracker. It assesses the top N trackers for this
  // treatment's criteria.
  virtual void SelectEligibleTrackers(PageTracker& pt)
      ABSL_EXCLUSIVE_LOCKS_REQUIRED(pageheap_lock) = 0;

  // Called once every tracker has been offered to SelectEligibleTrackers,
  // still under pageheap_lock.  Pins (DontFreeTracker) the trackers the
  // treatment settled on, so that a tracker it only considered is never
  // pinned and unpinned.
  virtual void FinishSelection()
      ABSL_EXCLUSIVE_LOCKS_REQUIRED(pageheap_lock) = 0;

  // Returns the number of trackers that have been selected for treatment.
  [[nodiscard]] virtual int num_valid_trackers() const = 0;

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
      : tag_(tag),
        set_anon_vma_name_(set_anon_vma_name),
        clock_now_(clock.now()),
        record_interval_cycles_(absl::ToDoubleSeconds(kRecordInterval) *
                                clock.freq()) {}
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
  //    passes them to the caller-supplied set_anon_vma_name callback.
  // 3. Unset the tracking bits using Restore, which is called under
  //    pageheap_lock.

  void SelectEligibleTrackers(PageTracker& pt)
      ABSL_EXCLUSIVE_LOCKS_REQUIRED(pageheap_lock) override {
    if (num_valid_trackers_ >= kTotalTrackersToScan) return;

    // Collect all the addresses under pageheap lock that are to be sampled for
    // tagging, and that were last scanned more than kRecordInterval ago.
    PageTracker::TagState tagged_state = pt.GetTagState();
    if (!tagged_state.sampled_for_tagging) return;

    // Evaluate against the cached clock to avoid repeatedly reading the
    // hardware clock under lock.
    double elapsed = std::max<double>(clock_now_ - tagged_state.record_time, 0);
    if (elapsed > record_interval_cycles_) {
#ifdef TCMALLOC_INTERNAL_LEGACY_LOCKING2
      selected_trackers_[num_valid_trackers_] = {
          &pt,           pt.longest_free_range(), pt.nallocs(),
          pt.nobjects(), pt.HasDenseSpans(),      pt.released()};
#else
      selected_trackers_[num_valid_trackers_].Construct(
          TrackerState{&pt, pt.longest_free_range(), pt.nallocs(),
                       pt.nobjects(), pt.HasDenseSpans(), pt.released()});
#endif
      pt.SetTagState({.sampled_for_tagging = true, .record_time = clock_now_});
      ++num_valid_trackers_;
    }
  }

  void FinishSelection() ABSL_EXCLUSIVE_LOCKS_REQUIRED(pageheap_lock) override {
    TC_ASSERT_LE(num_valid_trackers_, kTotalTrackersToScan);
    for (int i = 0; i < num_valid_trackers_; ++i) {
#ifdef TCMALLOC_INTERNAL_LEGACY_LOCKING2
      PageTracker* tracker = selected_trackers_[i].tracker;
#else
      PageTracker* tracker = selected_trackers_[i].get_mutable().tracker;
#endif
      TC_ASSERT_NE(tracker, nullptr);
      // Setting this bit makes sure that the tracker is not freed under us
      // when the pageheap lock is unlocked and we are in the middle of
      // applying the treatment.
      tracker->SetDontFreeTracker(HugePageTreatmentType::kSampled);
    }
  }

  [[nodiscard]] int num_valid_trackers() const override {
    return num_valid_trackers_;
  }

  void Treat() ABSL_LOCKS_EXCLUDED(pageheap_lock) override {
    TC_ASSERT_LE(num_valid_trackers_, kTotalTrackersToScan);
    // Record all the features we want to record, encode that into a string,
    // and use it to name the allocated region.
    for (int i = 0; i < num_valid_trackers_; ++i) {
#ifdef TCMALLOC_INTERNAL_LEGACY_LOCKING2
      PageTracker* tracker = selected_trackers_[i].tracker;
      TC_ASSERT_NE(tracker, nullptr);
      const Length lfr = selected_trackers_[i].lfr;
      const size_t nallocs = selected_trackers_[i].nallocs;
      const size_t nobjects = selected_trackers_[i].nobjects;
      const bool has_dense_spans = selected_trackers_[i].has_dense_spans;
      const bool released = selected_trackers_[i].released;

      char name[kMaxAnonVmaNameSize];
      FormatSampledTrackerVmaName(absl::MakeSpan(name), tag_, kPageSize, lfr,
                                  nallocs, nobjects, has_dense_spans, released);
      tracker->SetAnonVmaName(set_anon_vma_name_, name);
#else
      const TrackerState& s = selected_trackers_[i].get_mutable();
      TC_ASSERT_NE(s.tracker, nullptr);
      char name[kMaxAnonVmaNameSize];
      FormatSampledTrackerVmaName(absl::MakeSpan(name), tag_, kPageSize, s.lfr,
                                  s.nallocs, s.nobjects, s.has_dense_spans,
                                  s.released);
      s.tracker->SetAnonVmaName(set_anon_vma_name_, name);
#endif
    }
  }

  void Restore() ABSL_EXCLUSIVE_LOCKS_REQUIRED(pageheap_lock) override {
    TC_ASSERT_LE(num_valid_trackers_, kTotalTrackersToScan);
    for (int i = 0; i < num_valid_trackers_; ++i) {
#ifdef TCMALLOC_INTERNAL_LEGACY_LOCKING2
      PageTracker* tracker = selected_trackers_[i].tracker;
#else
      PageTracker* tracker = selected_trackers_[i].get_mutable().tracker;
#endif
      TC_ASSERT_NE(tracker, nullptr);
      tracker->ClearDontFreeTracker(HugePageTreatmentType::kSampled);
    }
  }

 private:
  static constexpr size_t kTotalTrackersToScan = 64;
  MemoryTag tag_;
  MemoryTagFunction& set_anon_vma_name_;
  double clock_now_;
  double record_interval_cycles_;

  struct TrackerState {
    PageTracker* tracker;
    Length lfr;
    size_t nallocs;
    size_t nobjects;
    bool has_dense_spans;
    bool released;
  };
#ifdef TCMALLOC_INTERNAL_LEGACY_LOCKING2
  using TrackerArray = std::array<TrackerState, kTotalTrackersToScan>;
#else
  using TrackerArray =
      std::array<ExplicitlyConstructed<TrackerState>, kTotalTrackersToScan>;
#endif
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
      ReleaseStalePages release_stale_pages,
      PreserveDonatedOnCollapse preserve_donated_on_collapse,
      CollapseReleasedHugePages collapse_released,
      ReobserveHugepageBacked reobserve_hugepage_backed)
      : clock_(clock),
        pageflags_(pageflags),
        residency_(residency),
        collapse_(collapse),
        page_filler_(page_filler),
        enable_collapse_(enable_collapse),
        subrelease_unbacked_mode_(subrelease_unbacked_mode),
        enable_unfiltered_collapse_(enable_unfiltered_collapse),
        release_stale_pages_(release_stale_pages),
        preserve_donated_on_collapse_(preserve_donated_on_collapse),
        collapse_released_(collapse_released),
        reobserve_hugepage_backed_(reobserve_hugepage_backed),
        clock_now_(clock.now()),
        record_interval_cycles_(absl::ToDoubleSeconds(kRecordInterval) *
                                clock.freq()) {}
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
  //    Alongside them, collect the kTrackersToReobserve trackers with the
  //    oldest records of being hugepage backed, among those recorded more than
  //    kRecordInterval ago.  The kernel may have split those since (compaction,
  //    swap), so re-observing them keeps the recorded state, and the
  //    hugepage_backed telemetry that HugePageFiller::Print derives from it,
  //    from repeating a first observation forever.  Taking the oldest records
  //    rather than the first found makes the sweep round-robin over a heap
  //    larger than kTrackersToReobserve instead of starving its tail.
  // 2. Release the pageheap lock and obtain the residency and pageflags
  //    information for the collected trackers. Attempt to apply treatments to
  //    the pages that aren't hugepage backed. In case of userspace collapse,
  //    it attempts to collapse pages that are composed of the number of
  //    unbacked and swapped pages less than kMaxUnbackedPagesForCollapse and
  //    kMaxSwappedPagesForCollapse respectively.  A re-observed tracker that
  //    turns out to have been split is treated like any other candidate.
  // 3. Acquire the pageheap lock and restore the recorded state using Restore
  //    (e.g. update the residency information in the trackers).
  [[nodiscard]] static bool CompareForHugePageTreatment(PageTracker* a,
                                                        PageTracker* b) {
    TC_ASSERT_NE(a, nullptr);
    TC_ASSERT_NE(b, nullptr);
    if (a->nobjects() > b->nobjects()) return true;
    if (a->nobjects() < b->nobjects()) return false;

    // All things considered equal, prefer collapsing dense spans.
    if (!a->HasDenseSpans()) return false;
    return !b->HasDenseSpans();
  }

  void SelectEligibleTrackers(PageTracker& pt)
      ABSL_EXCLUSIVE_LOCKS_REQUIRED(pageheap_lock) override {
    // Trackers are pinned in FinishSelection, once the set is final:
    // pinning here and unpinning on eviction would touch a cold tracker per
    // eviction under pageheap_lock.
    auto PushCandidate = [&](PageTracker& pt) GOOGLE_MALLOC_SECTION {
      if (num_valid_trackers_ < kTotalTrackersToScan) {
        selected_trackers_[num_valid_trackers_] = &pt;
        ++num_valid_trackers_;
        if (num_valid_trackers_ == kTotalTrackersToScan) {
          std::make_heap(selected_trackers_.begin(),
                         selected_trackers_.begin() + num_valid_trackers_,
                         CompareForHugePageTreatment);
        }
        return;
      }

#ifdef TCMALLOC_INTERNAL_LEGACY_LOCKING2
      if (CompareForHugePageTreatment(selected_trackers_[0], &pt)) {
        return;
      }
#else
      if (!CompareForHugePageTreatment(&pt, selected_trackers_[0])) {
        return;
      }
#endif
      std::pop_heap(selected_trackers_.begin(),
                    selected_trackers_.begin() + num_valid_trackers_,
                    CompareForHugePageTreatment);
      selected_trackers_[num_valid_trackers_ - 1] = &pt;
      std::push_heap(selected_trackers_.begin(),
                     selected_trackers_.begin() + num_valid_trackers_,
                     CompareForHugePageTreatment);
    };

#ifdef TCMALLOC_INTERNAL_LEGACY_LOCKING2
    PageTracker::HugePageResidencyState state = pt.GetHugePageResidencyState();
#else
    const PageTracker::HugePageResidencyState& state =
        pt.GetHugePageResidencyState();
#endif
    if (!state.entry_valid) {
      PushCandidate(pt);
      return;
    }
    // Evaluate against the cached clock and the precomputed interval to avoid
    // repeatedly reading the hardware clock and recomputing the threshold
    // under lock.
    double elapsed = std::max<double>(clock_now_ - state.record_time, 0);
    if (elapsed <= record_interval_cycles_) return;
    if (!state.maybe_hugepage_backed) {
      PushCandidate(pt);
      return;
    }
    if (reobserve_hugepage_backed_ == ReobserveHugepageBacked::kDisabled) {
      return;
    }
    // Keep the kTrackersToReobserve oldest records.  Rather than a heap, whose
    // displacement walks log2(K) dependent loads per tracker in the worst
    // case, buffer candidates and compact the buffer with nth_element when it
    // fills: amortized O(1) per candidate, and nothing per tracker no older
    // than the cutoff a compaction established.
    if (state.record_time >= reobserve_cutoff_) return;
    reobserved_trackers_[num_reobserved_trackers_] = {&pt, state.record_time};
    ++num_reobserved_trackers_;
    if (num_reobserved_trackers_ == reobserved_trackers_.size()) {
      CompactReobserved();
    }
  }

  void FinishSelection() ABSL_EXCLUSIVE_LOCKS_REQUIRED(pageheap_lock) override {
    TC_ASSERT_LE(num_valid_trackers_, kTotalTrackersToScan);
    if (num_reobserved_trackers_ > kTrackersToReobserve) {
      CompactReobserved();
    }
    TC_ASSERT_LE(num_reobserved_trackers_, kTrackersToReobserve);
    for (int i = 0; i < num_valid_trackers_; ++i) {
      TC_ASSERT_NE(selected_trackers_[i], nullptr);
      Pin(*selected_trackers_[i]);
    }
    for (int i = 0; i < num_reobserved_trackers_; ++i) {
      Pin(*reobserved_trackers_[i].tracker);
    }
  }

  [[nodiscard]] int num_valid_trackers() const override {
    return num_valid_trackers_;
  }

  // Stale hugepage-backed trackers selected for re-observation.  Like
  // num_valid_trackers, nonzero means Treat has pageflags work to do.
  [[nodiscard]] int num_reobserved_trackers() const {
    return num_reobserved_trackers_;
  }

  [[nodiscard]] bool tracker_list_full() const {
    return num_valid_trackers_ >= kTotalTrackersToScan;
  }

  void Treat() ABSL_LOCKS_EXCLUDED(pageheap_lock) override {
#ifndef TCMALLOC_INTERNAL_LEGACY_LOCKING2
    if (num_valid_trackers_ == 0 && num_reobserved_trackers_ == 0) {
      return;
    }
#endif

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
    TC_ASSERT_LE(num_reobserved_trackers_, kTrackersToReobserve);
    if (enable_collapse_ == EnableCollapse::kEnabled) {
      treatment_stats_.collapse_eligible += NHugePages(num_valid_trackers_);
    }
    // Outside of the pageheap lock, obtain the residency and pageflags
    // information for the collected addresses. Try to collapse the pages that
    // aren't hugepage backed, and for which, the number of unbacked and swapped
    // pages are less than kMaxUnbackedPagesForCollapse and
    // kMaxSwappedPagesForCollapse respectively.
    const double max_collapse_cycles =
        absl::ToDoubleSeconds(kMaxCollapseLatencyThreshold) * clock_.freq();
    const size_t pages_per_huge_page = kHugePageSize / GetPageSize();
    num_residency_states_ = 0;
    for (int i = 0; i < num_valid_trackers_; ++i) {
      PageTracker* tracker = selected_trackers_[i];
      Record(tracker, Observe(tracker, IsHugepageBacked(tracker, *pf), *pf,
                              *res, pages_per_huge_page, max_collapse_cycles));
    }
    for (int i = 0; i < num_reobserved_trackers_; ++i) {
      ReobservedTracker& entry = reobserved_trackers_[i];
      if (IsHugepageBacked(entry.tracker, *pf)) {
        // Still hugepage backed: only the record time changes, which Restore
        // applies without a full residency record.
        entry.record_time = clock_.now();
        entry.outcome = ReobservedTracker::Outcome::kBacked;
        continue;
      }
      // A tracker the kernel split since it was recorded as hugepage backed
      // is a collapse candidate like the ones selected above; it only becomes
      // known as one here.
      if (num_residency_states_ - num_valid_trackers_ ==
          static_cast<int>(kReobservedSplitsToRecord)) {
        // This pass has recorded as many splits as it may; its record stays
        // the oldest, so a later pass re-observes it first.
        continue;
      }
      if (enable_collapse_ == EnableCollapse::kEnabled) {
        treatment_stats_.collapse_eligible += NHugePages(1);
      }
      entry.outcome = ReobservedTracker::Outcome::kRecorded;
      Record(entry.tracker,
             Observe(entry.tracker, /*is_hugepage=*/false, *pf, *res,
                     pages_per_huge_page, max_collapse_cycles));
    }
  }

  void Restore() ABSL_EXCLUSIVE_LOCKS_REQUIRED(pageheap_lock) override {
    TC_ASSERT_LE(num_residency_states_, residency_states_.size());
    for (int i = 0; i < num_residency_states_; ++i) {
#ifdef TCMALLOC_INTERNAL_LEGACY_LOCKING2
      ResidencyState& entry = residency_states_[i];
#else
      ResidencyState& entry = residency_states_[i].get_mutable();
#endif
      RestoreOne(entry.tracker, entry.tracker_state);
    }
    TC_ASSERT_LE(num_reobserved_trackers_, kTrackersToReobserve);
    for (int i = 0; i < num_reobserved_trackers_; ++i) {
      const ReobservedTracker& entry = reobserved_trackers_[i];
      TC_ASSERT_NE(entry.tracker, nullptr);
      switch (entry.outcome) {
        case ReobservedTracker::Outcome::kRecorded:
          // Restored from residency_states_ above.
          break;
        case ReobservedTracker::Outcome::kUnobserved:
          Unpin(*entry.tracker);
          break;
        case ReobservedTracker::Outcome::kBacked: {
          // Observed as still hugepage backed: refresh the record time on the
          // state already recorded, which carries no residency bitmaps for a
          // hugepage-backed tracker.
          PageTracker::HugePageResidencyState state =
              entry.tracker->GetHugePageResidencyState();
          TC_ASSERT(state.maybe_hugepage_backed);
          state.record_time = entry.record_time;
          RestoreOne(entry.tracker, state);
          break;
        }
      }
    }
  }

  [[nodiscard]] HugePageTreatmentStats GetStats() const {
    return treatment_stats_;
  }

  void UpdateHugePageTreatmentStats(HugePageTreatmentStats& stats) {
    stats += treatment_stats_;
    // TODO(b/425749361): Roll this up to the overloaded operator once we start
    // reporting cumulative treated_pages_subreleased stat.
    stats.treated_pages_subreleased =
        treatment_stats_.treated_pages_subreleased;
    stats.treated_pages_unbacked_subreleased =
        treatment_stats_.treated_pages_unbacked_subreleased;
    stats.total_treated_pages_unbacked_subreleased +=
        treatment_stats_.treated_pages_unbacked_subreleased;
    stats.treated_pages_stale_subreleased =
        treatment_stats_.treated_pages_stale_subreleased;
    stats.collapse_time_max_cycles =
        std::max(stats.collapse_time_max_cycles,
                 treatment_stats_.collapse_time_max_cycles);
  }

 private:
  // Keeps tracker from being freed while pageheap_lock is dropped, and starts
  // tracking releases and frees on it over that window.
  static void Pin(PageTracker& tracker)
      ABSL_EXCLUSIVE_LOCKS_REQUIRED(pageheap_lock) {
    tracker.SetDontFreeTracker(HugePageTreatmentType::kCollapse);
    tracker.ClearReleasedSinceSelected();
    tracker.ClearFreedSinceSelected();
  }

  static void Unpin(PageTracker& tracker)
      ABSL_EXCLUSIVE_LOCKS_REQUIRED(pageheap_lock) {
    tracker.ClearDontFreeTracker(HugePageTreatmentType::kCollapse);
    tracker.ClearFreedSinceSelected();
  }

  // Queries pageflags for tracker's hugepage with pageheap_lock dropped.
  [[nodiscard]] static bool IsHugepageBacked(PageTracker* tracker,
                                             PageFlagsBase& pf) {
    TC_ASSERT_NE(tracker, nullptr);
    // Assume element is hugepage if we can't read the value.
    return pf.IsHugepageBacked(tracker->location().start_addr()).value_or(true);
  }

  // Builds the state to record for tracker, whose hugepage pageflags reported
  // as is_hugepage, with pageheap_lock dropped: for a hugepage that is not
  // hugepage backed, queries residency and attempts a collapse if enabled and
  // eligible.
  [[nodiscard]] PageTracker::HugePageResidencyState Observe(
      PageTracker* tracker, bool is_hugepage, PageFlagsBase& pf, Residency& res,
      size_t pages_per_huge_page, double max_collapse_cycles) {
    TC_ASSERT_NE(tracker, nullptr);
    PageTracker::HugePageResidencyState state;
    state.entry_valid = true;
    state.record_time = clock_.now();

    // If the address is not hugepage backed, obtain the residency
    // information.
    state.maybe_hugepage_backed = is_hugepage;
    if (!is_hugepage) {
      auto bitmaps =
          res.GetUnbackedAndSwappedBitmaps(tracker->location().start_addr());
      state.unbacked = Scale<kPagesPerHugePage.raw_num()>(
          bitmaps.unbacked, pages_per_huge_page, ReductionOp::kAll);
      state.swapped = Scale<kPagesPerHugePage.raw_num()>(
          bitmaps.swapped, pages_per_huge_page, ReductionOp::kAny);
      auto single_page_bitmaps =
          pf.GetSinglePageBitmaps(tracker->location().start_addr());
      state.stale = Scale<kPagesPerHugePage.raw_num()>(
          single_page_bitmaps.stale, pages_per_huge_page, ReductionOp::kAny);

      const bool backoff =
          treatment_stats_.collapse_time_max_cycles > max_collapse_cycles;
      if (enable_collapse_ == EnableCollapse::kEnabled && !backoff) {
        bool should_collapse =
            enable_unfiltered_collapse_ == EnableUnfilteredCollapse::kEnabled ||
            (bitmaps.swapped.CountBits() < kMaxSwappedPagesForCollapse &&
             bitmaps.unbacked.CountBits() < kMaxUnbackedPagesForCollapse);
        if (should_collapse) {
          state.maybe_hugepage_backed = TryUserspaceCollapse(tracker);
          if (state.maybe_hugepage_backed) {
            state.unbacked.Clear();
            state.swapped.Clear();
          }
        } else {
          state.collapse_skipped = true;
        }
      } else if (enable_collapse_ == EnableCollapse::kEnabled && backoff) {
        state.collapse_skipped = true;
        state.collapse_skipped_due_to_backoff = true;
      }
    }
    return state;
  }

  // Appends state to the records Restore applies to tracker.
  void Record(PageTracker* tracker,
              const PageTracker::HugePageResidencyState& state) {
    TC_ASSERT_LT(num_residency_states_, residency_states_.size());
#ifdef TCMALLOC_INTERNAL_LEGACY_LOCKING2
    residency_states_[num_residency_states_] = {tracker, state};
#else
    residency_states_[num_residency_states_].Construct(
        ResidencyState{tracker, state});
#endif
    ++num_residency_states_;
  }

  // Applies the recorded state to tracker and releases its pin.
  void RestoreOne(PageTracker* tracker,
                  PageTracker::HugePageResidencyState& state)
      ABSL_EXCLUSIVE_LOCKS_REQUIRED(pageheap_lock) {
    TC_ASSERT_NE(tracker, nullptr);
    // While we did not hold pageheap_lock, tracker may have been emptied
    // (and parked on fully_freed_trackers_) or claimed by a ReleasePages
    // that is unbacking it.  Either way it is off the filler lists; leave it
    // alone.
    if (tracker->fully_freed() || tracker->BeingReleased()) {
      Unpin(*tracker);
      return;
    }
    if (state.maybe_hugepage_backed &&
        tracker->GetHugePageResidencyState().released_since_selected) {
      // The hugepage was observed (or collapsed) as hugepage backed, but
      // another thread released pages from it while pageheap_lock was
      // dropped, breaking it again (the pages may since have been
      // reallocated, so released() alone would not show it).  Record it as
      // broken so that a later pass reexamines it instead of skipping it as
      // hugepage backed.
      state.maybe_hugepage_backed = false;
    }
    tracker->SetHugePageResidencyState(state);
    if (state.maybe_hugepage_backed) {
      Unpin(*tracker);
      return;
    }

    // HandleReleaseFree drops pageheap_lock, so the kCollapse pin stays set
    // across it: a concurrent Put that empties tracker then parks it rather
    // than returning it underneath us, and any Put during that lock drop
    // sets freed_since_selected() before HandleUnbackedHugePage.
    if (!state.swapped.IsZero()) {
      // TODO: b/425749361 - Clear swapped bit for pages that were freed.
      Length released_length = page_filler_.HandleReleaseFree(tracker);
      if (released_length > Length(0)) {
        treatment_stats_.treated_pages_subreleased += released_length;
      }
    } else if (release_stale_pages_ == ReleaseStalePages::kEnabled &&
               !state.stale.IsZero()) {
      Length released_length = page_filler_.HandleReleaseFree(tracker);
      if (released_length > Length(0)) {
        treatment_stats_.treated_pages_stale_subreleased += released_length;
      }
    }
    if (tracker->fully_freed()) {
      Unpin(*tracker);
      return;
    }

    if (subrelease_unbacked_mode_ == SubreleaseUnbackedMode::kEnabled &&
        !tracker->freed_since_selected()) {
      Length released_length =
          page_filler_.HandleUnbackedHugePage(tracker, state.unbacked);
      if (released_length > Length(0)) {
        treatment_stats_.treated_pages_unbacked_subreleased += released_length;
      }
    }
    Unpin(*tracker);
  }

  [[nodiscard]] bool TryUserspaceCollapse(PageTracker* tracker) {
    double before = clock_.now();
    MemoryModifyStatus ret = page_filler_.Collapse(
        tracker, preserve_donated_on_collapse_, collapse_released_);
    double after = clock_.now();
    double elapsed = std::max<double>(after - before, 0);
    treatment_stats_.collapse_time_total_cycles += elapsed;
    treatment_stats_.collapse_time_max_cycles =
        std::max(elapsed, treatment_stats_.collapse_time_max_cycles);
    treatment_stats_.collapse_attempted += NHugePages(1);
    if (ret.success) {
      treatment_stats_.collapse_succeeded += NHugePages(1);
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

  // Trackers recorded as hugepage backed more than kRecordInterval ago, the
  // oldest records first.
  struct ReobservedTracker {
    PageTracker* tracker;
    // The record time at selection; for Outcome::kBacked, the observation
    // time Restore records instead.
    double record_time;
    enum class Outcome : uint8_t {
      // Not observed this pass; Restore only drops the pin.
      kUnobserved,
      // Observed as still hugepage backed.
      kBacked,
      // Observed as split; its record is in residency_states_.
      kRecorded,
    } outcome = Outcome::kUnobserved;
  };
  [[nodiscard]] static bool OlderRecord(const ReobservedTracker& a,
                                        const ReobservedTracker& b) {
    return a.record_time < b.record_time;
  }
  // Keeps the kTrackersToReobserve oldest records buffered and raises
  // reobserve_cutoff_ to the youngest of them.
  void CompactReobserved() {
    TC_ASSERT_GT(num_reobserved_trackers_, kTrackersToReobserve);
    auto begin = reobserved_trackers_.begin();
    auto cut = begin + (kTrackersToReobserve - 1);
    std::nth_element(begin, cut, begin + num_reobserved_trackers_, OlderRecord);
    reobserve_cutoff_ = cut->record_time;
    num_reobserved_trackers_ = kTrackersToReobserve;
  }
  // Candidates for re-observation: after FinishSelection, the (at most)
  // kTrackersToReobserve oldest; before it, up to twice as many, compacted to
  // the oldest kTrackersToReobserve whenever the buffer fills.
  std::array<ReobservedTracker, 2 * kTrackersToReobserve> reobserved_trackers_;
  int num_reobserved_trackers_ = 0;
  // Records at or after this time are not among the kTrackersToReobserve
  // oldest seen so far, and are not buffered.
  double reobserve_cutoff_ = std::numeric_limits<double>::infinity();

  struct ResidencyState {
    PageTracker* tracker;
    PageTracker::HugePageResidencyState tracker_state;
  };
  // Filled by Treat() for selected_trackers_, then for the
  // reobserved_trackers_ found split.
#ifdef TCMALLOC_INTERNAL_LEGACY_LOCKING2
  std::array<ResidencyState, kTotalTrackersToScan + kReobservedSplitsToRecord>
      residency_states_;
#else
  std::array<ExplicitlyConstructed<ResidencyState>,
             kTotalTrackersToScan + kReobservedSplitsToRecord>
      residency_states_;
#endif
  int num_residency_states_ = 0;
  HugePageTreatmentStats treatment_stats_;
  HugePageFiller<TrackerType>& page_filler_;
  EnableCollapse enable_collapse_;
  SubreleaseUnbackedMode subrelease_unbacked_mode_;

  EnableUnfilteredCollapse enable_unfiltered_collapse_;
  ReleaseStalePages release_stale_pages_;
  PreserveDonatedOnCollapse preserve_donated_on_collapse_;
  CollapseReleasedHugePages collapse_released_;
  ReobserveHugepageBacked reobserve_hugepage_backed_;

  // Clock state cached at construction time, so that we do not read the
  // hardware clock nor recompute the record interval threshold for every
  // tracker we examine under pageheap_lock.
  double clock_now_;
  double record_interval_cycles_;
};

}  // namespace tcmalloc_internal
}  // namespace tcmalloc
GOOGLE_MALLOC_SECTION_END

#endif  // TCMALLOC_HUGE_PAGE_TREATMENT_H_
