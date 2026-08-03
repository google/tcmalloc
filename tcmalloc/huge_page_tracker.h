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

#ifndef TCMALLOC_HUGE_PAGE_TRACKER_H_
#define TCMALLOC_HUGE_PAGE_TRACKER_H_

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
namespace tcmalloc::tcmalloc_internal {

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
  void set_unbroken(bool status) { unbroken_ = status; }

  // Returns the hugepage whose availability is being tracked.
  HugePage location() const { return location_; }

  // Return all unused pages to the system, mark future frees to do same.
  // Returns the count of pages unbacked.
  Length ReleaseFree(MemoryModifyFunction& unback)
      ABSL_EXCLUSIVE_LOCKS_REQUIRED(pageheap_lock);

  Length MarkSubreleased(PageBitmap unbacked)
      ABSL_EXCLUSIVE_LOCKS_REQUIRED(pageheap_lock);

  Bitmap<kPagesPerHugePage.raw_num()> released_by_page() const {
    return released_by_page_;
  }

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
    // Records the unbacked bitmap for this hugepage. In terms of TCMalloc
    // pages. scaled via `ReductionOp::kAll`.
    PageBitmap unbacked;
    // Records the swapped bitmap for this hugepage. In terms of TCMalloc
    // pages. scaled via `ReductionOp::kAny`.
    PageBitmap swapped;
    // Records the stale bitmap for this hugepage. In terms of TCMalloc
    // pages. scaled via `ReductionOp::kAny`.
    PageBitmap stale;
    // Records whether collapse was skipped due to threshold constraints.
    bool collapse_skipped = false;
    // Records whether collapse was skipped due to backoff.
    bool collapse_skipped_due_to_backoff = false;
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

  void SetDontFreeTracker(HugePageTreatmentType type) {
    dont_free_tracker_mask_ |= static_cast<uint8_t>(type);
  }
  void ClearDontFreeTracker(HugePageTreatmentType type) {
    dont_free_tracker_mask_ &= ~static_cast<uint8_t>(type);
  }
  bool DontFreeTracker() const { return dont_free_tracker_mask_ != 0; }

  struct TagState {
    bool sampled_for_tagging = false;
    double record_time = 0;
  };
  TagState GetTagState() const { return tagged_state_; }
  void SetTagState(const TagState& state) { tagged_state_ = state; }

  void SetAnonVmaName(MemoryTagFunction& set_anon_vma_name,
                      std::optional<absl::string_view> name);

  struct HardwarePageResidencyInfo {
    size_t n_free_swapped;
    size_t n_used_swapped;
    size_t n_free_unbacked;
    size_t n_used_unbacked;
    size_t n_free_stale;
    size_t n_used_stale;
  };

  HardwarePageResidencyInfo CountInfoInHugePage(PageBitmap unbacked,
                                                PageBitmap swapped,
                                                PageBitmap stale) const;

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
  uint8_t dont_free_tracker_mask_ = 0;

  [[nodiscard]] bool ReleasePages(Range r, MemoryModifyFunction& unback) {
    bool success = unback(r).success;
    if (ABSL_PREDICT_TRUE(success)) {
      unbroken_ = false;
    }
    return success;
  }
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

inline PageTracker::HardwarePageResidencyInfo PageTracker::CountInfoInHugePage(
    PageBitmap unbacked, PageBitmap swapped, PageBitmap stale) const {
  // TODO(b/424551232): Add support for the scenario when native page size is
  // larger than TCMalloc page size.
  const size_t kHardwarePagesInHugePage = kHugePageSize / GetPageSize();
  if (kHardwarePagesInHugePage < kPagesPerHugePage.raw_num()) {
    return {.n_free_swapped = 0, .n_free_unbacked = 0};
  }
  TC_ASSERT_LE(kHardwarePagesInHugePage, kMaxResidencyBits);

  const Bitmap<kPagesPerHugePage.raw_num()> free = free_.bits();

  TC_ASSERT_EQ(kHardwarePagesInHugePage % kPagesPerHugePage.raw_num(), 0);
  const int shift = kHardwarePagesInHugePage / kPagesPerHugePage.raw_num();
  const int shift_bits = absl::bit_width<uint8_t>(shift - 1);
  TC_ASSERT_LT((kHardwarePagesInHugePage - 1) >> shift_bits,
               kPagesPerHugePage.raw_num());

  size_t n_unbacked[2] = {0, 0};
  size_t n_swapped[2] = {0, 0};
  size_t n_stale[2] = {0, 0};

  n_unbacked[0] = (free & unbacked).CountBits() * shift;
  n_unbacked[1] = (~free & unbacked).CountBits() * shift;
  n_swapped[0] = (free & swapped).CountBits() * shift;
  n_swapped[1] = (~free & swapped).CountBits() * shift;
  n_stale[0] = (free & stale).CountBits() * shift;
  n_stale[1] = (~free & stale).CountBits() * shift;

  return {.n_free_swapped = n_swapped[1],
          .n_used_swapped = n_swapped[0],
          .n_free_unbacked = n_unbacked[1],
          .n_used_unbacked = n_unbacked[0],
          .n_free_stale = n_stale[1],
          .n_used_stale = n_stale[0]};
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

inline Length PageTracker::MarkSubreleased(PageBitmap unbacked) {
  PageBitmap free = free_.bits();

  // TODO(b/525422238): The residency bitmap was captured outside of the
  // lock. So, in a rare case, it's possible that the page was allocated,
  // backed and then freed. So, the free page here is actually backed.
  // While we currently ignore this case (resulting in underestimating
  // RSS), we can potentially fix this by re-investigating the bitmaps
  // and marking the pages back to backed to eventually fix this.
  auto to_release = (~free) & (~released_by_page_) & unbacked;
  released_by_page_ = released_by_page_ | to_release;

  released_count_ += to_release.CountBits();
  // Mark this is unbroken regardless of whether it had any unbacked free
  // TCMalloc pages. Marking this will move this tracker to one of the
  // released lists.
  unbroken_ = false;
  TC_ASSERT_LE(Length(released_count_), kPagesPerHugePage);
  TC_ASSERT_EQ(released_by_page_.CountBits(), released_count_);
  return Length(to_release.CountBits());
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

}  // namespace tcmalloc::tcmalloc_internal
GOOGLE_MALLOC_SECTION_END

#endif  // TCMALLOC_HUGE_PAGE_TRACKER_H_
