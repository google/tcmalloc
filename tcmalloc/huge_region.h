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

#ifndef TCMALLOC_HUGE_REGION_H_
#define TCMALLOC_HUGE_REGION_H_
#include <stddef.h>
#include <stdint.h>

#include <algorithm>

#include "absl/base/attributes.h"
#include "absl/base/internal/cycleclock.h"
#include "absl/base/nullability.h"
#include "absl/base/optimization.h"
#include "tcmalloc/huge_cache.h"
#include "tcmalloc/huge_pages.h"
#include "tcmalloc/internal/config.h"
#include "tcmalloc/internal/linked_list.h"
#include "tcmalloc/internal/logging.h"
#include "tcmalloc/internal/range_tracker.h"
#include "tcmalloc/pages.h"
#include "tcmalloc/stats.h"

GOOGLE_MALLOC_SECTION_BEGIN
namespace tcmalloc {
namespace tcmalloc_internal {

constexpr double kFractionToReleaseFromRegion = 0.1;
enum class HugeRegionUsageOption : bool {
  // This is a default behavior. We use slack to determine when to use
  // HugeRegion. When slack is greater than 64MB (to ignore small binaries), and
  // greater than the number of small allocations, we allocate large allocations
  // from HugeRegion.
  kDefault,
  // When the experiment TEST_ONLY_TCMALLOC_USE_HUGE_REGIONS_MORE_OFTEN is
  // enabled, we use number of abandoned pages in addition to slack to make a
  // decision. If the size of abandoned pages plus slack exceeds 64MB (to ignore
  // small binaries), we use HugeRegion for large allocations.
  kUseForAllLargeAllocs
};

// Track allocations from a fixed-size multiple huge page region.
// Similar to PageTracker but a few important differences:
// - crosses multiple hugepages
// - backs region on demand
// - supports breaking up the partially-allocated region for use elsewhere
//
// This is intended to help with fast allocation of regions too large
// for HugePageFiller, but too small to round to a full hugepage; both
// lengths that do fit in a hugepage, but often wouldn't fit in
// available gaps (1.75 MiB), and lengths that don't fit, but would
// introduce unacceptable fragmentation (2.1 MiB).
//
class HugeRegion : public TList<HugeRegion>::Elem {
 public:
  // We could template this if there was any need.
  static constexpr HugeLength kRegionSize = HLFromBytes(1024 * 1024 * 1024);
  static constexpr size_t kNumHugePages = kRegionSize.raw_num();
  static constexpr HugeLength size() { return kRegionSize; }

  // REQUIRES: r.len() == size(); r unbacked.
  HugeRegion(HugeRange r,
             MemoryModifyFunction& unback ABSL_ATTRIBUTE_LIFETIME_BOUND);
  HugeRegion() = delete;

  // If available, return a range of n free pages, setting *from_released =
  // true iff the returned range is currently unbacked.
  // Returns false if no range available.
  bool MaybeGet(Length n, PageId* absl_nonnull p,
                bool* absl_nonnull from_released);

  // Return r for new allocations.
  // If release=true, release any hugepages made empty as a result.
  // REQUIRES: Range{p, n} was the result of a previous MaybeGet.
  void Put(Range r, bool release);

  // Release <desired> numbae of pages from free-and-backed hugepages from
  // region.
  HugeLength Release(Length desired);

  // Is p located in this region?
  bool contains(PageId p) { return location_.contains(p); }

  // Stats
  Length used_pages() const { return Length(tracker_.used()); }
  Length free_pages() const {
    return size().in_pages() - unmapped_pages() - used_pages();
  }
  Length unmapped_pages() const { return (size() - nbacked_).in_pages(); }

  void AddSpanStats(SmallSpanStats* small, LargeSpanStats* large) const;

  HugeLength backed() const;

  // Returns the number of hugepages that have been fully free (i.e. no
  // allocated pages on them), but are backed. We release hugepages lazily when
  // huge-regions-more-often feature is enabled.
  HugeLength free_backed() const;

  void Print(Printer& out) const;
  void PrintInPbtxt(PbtxtRegion& detail) const;

  BackingStats stats() const;

  // We don't define this as operator< because it's a rather specialized order.
  bool BetterToAllocThan(const HugeRegion* rhs) const {
    return longest_free() < rhs->longest_free();
  }

 private:
  RangeTracker<kRegionSize.in_pages().raw_num()> tracker_;

  HugeRange location_;

  Length longest_free() const { return Length(tracker_.longest_free()); }

  // Adjust counts of allocs-per-hugepage for r being added/removed.

  // *from_released is set to true iff r is currently unbacked
  void Inc(Range r, bool* from_released);
  // If release is true, unback any hugepage that becomes empty.
  void Dec(Range r, bool release);

  HugeLength UnbackHugepages(bool should_unback[kNumHugePages]);

  // How many pages are used in each hugepage?
  Length pages_used_[kNumHugePages];
  // Is this hugepage backed?
  bool backed_[kNumHugePages];
  HugeLength nbacked_;
  HugeLength total_unbacked_{NHugePages(0)};

  MemoryModifyFunction& unback_;
};

// Manage a set of regions from which we allocate.
// Strategy: Allocate from the most fragmented region that fits.
template <typename Region>
class HugeRegionSet {
 public:
  explicit HugeRegionSet(HugeRegionUsageOption use_huge_region_more_often)
      : n_(0), use_huge_region_more_often_(use_huge_region_more_often) {}

  // If available, return a range of n free pages, setting *from_released =
  // true iff the returned range is currently unbacked.
  // Returns false if no range available.
  bool MaybeGet(Length n, PageId* absl_nonnull page,
                bool* absl_nonnull from_released);

  // Return an allocation to a region (if one matches!)
  bool MaybePut(Range r);

  // Add region to the set.
  void Contribute(Region* region);

  // Release hugepages that are unused but backed.
  // Releases up to <release_fraction> times number of free-but-backed hugepages
  // from each huge region. Note that this clamps release_fraction between 0 and
  // 1 if a fraction outside those bounds is specified.
  Length ReleasePages(double release_fraction);

  void Print(Printer& out) const;
  void PrintInPbtxt(PbtxtRegion& hpaa) const;
  void AddSpanStats(SmallSpanStats* small, LargeSpanStats* large) const;
  BackingStats stats() const;
  HugeLength free_backed() const;
  size_t ActiveRegions() const;
  bool UseHugeRegionMoreOften() const {
    return use_huge_region_more_often_ ==
           HugeRegionUsageOption::kUseForAllLargeAllocs;
  }

 private:
  void Fix(Region* absl_nonnull r) {
    // We've changed r's fragmentation--move it through the list to the
    // correct home (if needed).
    Rise(r);
    Fall(r);
  }

  // Check if r has to move forward in the list.
  void Rise(Region* absl_nonnull r) {
    auto prev = list_.at(r);
    --prev;
    if (prev == list_.end()) return;           // we're at the front
    if (!r->BetterToAllocThan(*prev)) return;  // we're far enough forward
    list_.remove(r);
    for (auto iter = prev; iter != list_.end(); --iter) {
      if (!r->BetterToAllocThan(*iter)) {
        list_.append(*iter, r);
        return;
      }
    }
    list_.prepend(r);
  }

  // Check if r has to move backward in the list.
  void Fall(Region* absl_nonnull r) {
    auto next = list_.at(r);
    ++next;
    if (next == list_.end()) return;          // we're at the back
    if (!next->BetterToAllocThan(r)) return;  // we're far enough back
    list_.remove(r);
    for (auto iter = next; iter != list_.end(); ++iter) {
      if (!iter->BetterToAllocThan(r)) {
        list_.prepend(*iter, r);
        return;
      }
    }
    list_.append(r);
  }

  // Add r in its sorted place.
  void AddToList(Region* absl_nonnull r) {
    for (Region* curr : list_) {
      if (r->BetterToAllocThan(curr)) {
        list_.prepend(curr, r);
        return;
      }
    }

    // Note this handles the empty-list case
    list_.append(r);
  }

  size_t n_;
  HugeRegionUsageOption use_huge_region_more_often_;
  // Sorted by longest_free increasing.
  TList<Region> list_;
};

// REQUIRES: r.len() == size(); r unbacked.
inline HugeRegion::HugeRegion(HugeRange r, MemoryModifyFunction& unback)
    : tracker_{},
      location_(r),
      pages_used_{},
      backed_{},
      nbacked_(NHugePages(0)),
      unback_(unback) {
  for (int i = 0; i < kNumHugePages; ++i) {
    // These are already 0 but for clarity...
    pages_used_[i] = Length(0);
    backed_[i] = false;
  }
}

inline bool HugeRegion::MaybeGet(Length n, PageId* p, bool* from_released) {
  if (n > longest_free()) return false;
  TC_ASSERT_GT(n, Length(0));
  auto index = Length(tracker_.FindAndMark(n.raw_num()));

  PageId page = location_.start().first_page() + index;
  *p = page;

  // the last hugepage we touch
  Inc(Range{page, n}, from_released);
  return true;
}

// If release=true, release any hugepages made empty as a result.
inline void HugeRegion::Put(Range r, bool release) {
  Length index = r.p - location_.start().first_page();
  tracker_.Unmark(index.raw_num(), r.n.raw_num());

  Dec(r, release);
}

// Release hugepages that are unused but backed.
// TODO(b/199203282): We release <desired> pages, rounded up to a hugepage, from
// free but backed hugepages from the region. We can explore a more
// sophisticated mechanism similar to Filler/Cache, that accounts for a recent
// peak while releasing pages.
inline HugeLength HugeRegion::Release(Length desired) {
  if (desired == Length(0)) return NHugePages(0);

  const Length free_yet_backed = free_backed().in_pages();
  const Length to_release = std::min(desired, free_yet_backed);

  HugeLength release_target = NHugePages(0);
  bool should_unback[kNumHugePages] = {};
  for (size_t i = 0; i < kNumHugePages; ++i) {
    if (backed_[i] && pages_used_[i] == Length(0)) {
      should_unback[i] = true;
      ++release_target;
    }

    if (release_target.in_pages() >= to_release) break;
  }
  return UnbackHugepages(should_unback);
}

inline void HugeRegion::AddSpanStats(SmallSpanStats* small,
                                     LargeSpanStats* large) const {
  size_t index = 0, n;
  Length f, u;
  // This is complicated a bit by the backed/unbacked status of pages.
  while (tracker_.NextFreeRange(index, &index, &n)) {
    // [index, index + n) is an *unused* range.  As it may cross
    // hugepages, we may need to truncate it so it is either a
    // *free* or a *released* range, and compute a reasonable value
    // for its "when".
    PageId p = location_.start().first_page() + Length(index);
    const HugePage hp = HugePageContaining(p);
    size_t i = (hp - location_.start()) / NHugePages(1);
    const bool backed = backed_[i];
    Length truncated;
    while (n > 0 && backed_[i] == backed) {
      const PageId lim = (location_.start() + NHugePages(i + 1)).first_page();
      Length here = std::min(Length(n), lim - p);
      truncated += here;
      n -= here.raw_num();
      p += here;
      i++;
      TC_ASSERT(i < kNumHugePages || n == 0);
    }
    n = truncated.raw_num();
    const bool released = !backed;
    if (released) {
      u += Length(n);
    } else {
      f += Length(n);
    }
    if (Length(n) < kMaxPages) {
      if (small != nullptr) {
        if (released) {
          small->returned_length[n]++;
        } else {
          small->normal_length[n]++;
        }
      }
    } else {
      if (large != nullptr) {
        large->spans++;
        if (released) {
          large->returned_pages += Length(n);
        } else {
          large->normal_pages += Length(n);
        }
      }
    }

    index += n;
  }
  TC_CHECK_EQ(f, free_pages());
  TC_CHECK_EQ(u, unmapped_pages());
}

inline HugeLength HugeRegion::free_backed() const {
  HugeLength r = NHugePages(0);
  for (size_t i = 0; i < kNumHugePages; ++i) {
    if (backed_[i] && pages_used_[i] == Length(0)) {
      ++r;
    }
  }
  return r;
}

inline HugeLength HugeRegion::backed() const {
  HugeLength b;
  for (int i = 0; i < kNumHugePages; ++i) {
    if (backed_[i]) {
      ++b;
    }
  }

  return b;
}

inline void HugeRegion::Print(Printer& out) const {
  const size_t kib_used = used_pages().in_bytes() / 1024;
  const size_t kib_free = free_pages().in_bytes() / 1024;
  const size_t kib_longest_free = longest_free().in_bytes() / 1024;
  const HugeLength unbacked = size() - backed();
  const size_t mib_unbacked = unbacked.in_mib();
  out.printf(
      "HugeRegion: %zu KiB used, %zu KiB free, "
      "%zu KiB contiguous space, %zu MiB unbacked, "
      "%zu MiB unbacked lifetime\n",
      kib_used, kib_free, kib_longest_free, mib_unbacked,
      total_unbacked_.in_bytes() / 1024 / 1024);
}

inline void HugeRegion::PrintInPbtxt(PbtxtRegion& detail) const {
  detail.PrintI64("used_bytes", used_pages().in_bytes());
  detail.PrintI64("free_bytes", free_pages().in_bytes());
  detail.PrintI64("longest_free_range_bytes", longest_free().in_bytes());
  const HugeLength unbacked = size() - backed();
  detail.PrintI64("unbacked_bytes", unbacked.in_bytes());
  detail.PrintI64("total_unbacked_bytes", total_unbacked_.in_bytes());
  detail.PrintI64("backed_fully_free_bytes", free_backed().in_bytes());
}

inline BackingStats HugeRegion::stats() const {
  BackingStats s;
  s.system_bytes = location_.len().in_bytes();
  s.free_bytes = free_pages().in_bytes();
  s.unmapped_bytes = unmapped_pages().in_bytes();
  return s;
}

inline void HugeRegion::Inc(Range r, bool* from_released) {
  bool should_back = false;
  while (r.n > Length(0)) {
    const HugePage hp = HugePageContaining(r.p);
    const size_t i = (hp - location_.start()) / NHugePages(1);
    const PageId lim = (hp + NHugePages(1)).first_page();
    Length here = std::min(r.n, lim - r.p);
    if (pages_used_[i] == Length(0) && !backed_[i]) {
      backed_[i] = true;
      should_back = true;
      ++nbacked_;
    }
    pages_used_[i] += here;
    TC_ASSERT_LE(pages_used_[i], kPagesPerHugePage);
    r.p += here;
    r.n -= here;
  }
  *from_released = should_back;
}

inline void HugeRegion::Dec(Range r, bool release) {
  bool should_unback[kNumHugePages] = {};
  while (r.n > Length(0)) {
    const HugePage hp = HugePageContaining(r.p);
    const size_t i = (hp - location_.start()) / NHugePages(1);
    const PageId lim = (hp + NHugePages(1)).first_page();
    Length here = std::min(r.n, lim - r.p);
    TC_ASSERT_GT(here, Length(0));
    TC_ASSERT_GE(pages_used_[i], here);
    TC_ASSERT(backed_[i]);
    pages_used_[i] -= here;
    if (pages_used_[i] == Length(0)) {
      should_unback[i] = true;
    }
    r.p += here;
    r.n -= here;
  }
  if (release) {
    UnbackHugepages(should_unback);
  }
}

inline HugeLength HugeRegion::UnbackHugepages(
    bool should_unback[kNumHugePages]) {
  HugeLength released = NHugePages(0);
  size_t i = 0;
  while (i < kNumHugePages) {
    if (!should_unback[i]) {
      i++;
      continue;
    }
    size_t j = i;
    while (j < kNumHugePages && should_unback[j]) {
      j++;
    }

    HugeLength hl = NHugePages(j - i);
    HugePage p = location_.start() + NHugePages(i);
    if (ABSL_PREDICT_TRUE(unback_(HugeRange(p, hl)).success)) {
      nbacked_ -= hl;
      total_unbacked_ += hl;

      for (size_t k = i; k < j; k++) {
        TC_ASSERT(should_unback[k]);
        backed_[k] = false;
      }

      released += hl;
    }
    i = j;
  }

  return released;
}

// If available, return a range of n free pages, setting *from_released =
// true iff the returned range is currently unbacked.
// Returns false if no range available.
template <typename Region>
inline bool HugeRegionSet<Region>::MaybeGet(Length n, PageId* page,
                                            bool* from_released) {
  for (Region* region : list_) {
    if (region->MaybeGet(n, page, from_released)) {
      Fix(region);
      return true;
    }
  }
  return false;
}

// Return an allocation to a region (if one matches!)
template <typename Region>
inline bool HugeRegionSet<Region>::MaybePut(Range r) {
  // When HugeRegionMoreOften experiment is enabled, we do not release
  // free-but-backed hugepages when we deallocate pages, but we do that
  // periodically on the background thread.
  const bool release = !UseHugeRegionMoreOften();
  for (Region* region : list_) {
    if (region->contains(r.p)) {
      region->Put(r, release);
      Fix(region);
      return true;
    }
  }

  return false;
}

// Add region to the set.
template <typename Region>
inline void HugeRegionSet<Region>::Contribute(Region* region) {
  n_++;
  AddToList(region);
}

template <typename Region>
inline Length HugeRegionSet<Region>::ReleasePages(double release_fraction) {
  const Length free_yet_backed = free_backed().in_pages();
  const size_t to_release =
      free_yet_backed.raw_num() * std::clamp<double>(release_fraction, 0, 1);
  const Length to_release_pages = Length(to_release);

  Length released;
  for (Region* region : list_) {
    released += region->Release(to_release_pages - released).in_pages();
    if (released >= to_release_pages) return released;
  }
  return released;
}

template <typename Region>
inline void HugeRegionSet<Region>::Print(Printer& out) const {
  out.printf("HugeRegionSet: 1 MiB+ allocations best-fit into %zu MiB slabs\n",
             Region::size().in_bytes() / 1024 / 1024);
  out.printf("HugeRegionSet: %zu total regions\n", n_);
  Length total_free;
  HugeLength total_backed = NHugePages(0);
  HugeLength total_free_backed = NHugePages(0);

  for (Region* region : list_) {
    region->Print(out);
    total_free += region->free_pages();
    total_backed += region->backed();
    total_free_backed += region->free_backed();
  }

  out.printf(
      "HugeRegionSet: %zu hugepages backed, %zu backed and free, "
      "out of %zu total\n",
      total_backed.raw_num(), total_free_backed.raw_num(),
      Region::size().raw_num() * n_);

  const Length in_pages = total_backed.in_pages();
  out.printf("HugeRegionSet: %zu pages free in backed region, %.4f free\n",
             total_free.raw_num(),
             in_pages > Length(0) ? static_cast<double>(total_free.raw_num()) /
                                        static_cast<double>(in_pages.raw_num())
                                  : 0.0);
}

template <typename Region>
inline void HugeRegionSet<Region>::PrintInPbtxt(PbtxtRegion& hpaa) const {
  hpaa.PrintI64("min_huge_region_alloc_size", 1024 * 1024);
  hpaa.PrintI64("huge_region_size", Region::size().in_bytes());
  for (Region* region : list_) {
    auto detail = hpaa.CreateSubRegion("huge_region_details");
    region->PrintInPbtxt(detail);
  }
}

template <typename Region>
inline void HugeRegionSet<Region>::AddSpanStats(SmallSpanStats* small,
                                                LargeSpanStats* large) const {
  for (Region* region : list_) {
    region->AddSpanStats(small, large);
  }
}

template <typename Region>
inline size_t HugeRegionSet<Region>::ActiveRegions() const {
  return n_;
}

template <typename Region>
inline BackingStats HugeRegionSet<Region>::stats() const {
  BackingStats stats;
  for (Region* region : list_) {
    stats += region->stats();
  }

  return stats;
}

template <typename Region>
inline HugeLength HugeRegionSet<Region>::free_backed() const {
  HugeLength pages;
  for (Region* region : list_) {
    pages += region->free_backed();
  }

  return pages;
}

}  // namespace tcmalloc_internal
}  // namespace tcmalloc
GOOGLE_MALLOC_SECTION_END

#endif  // TCMALLOC_HUGE_REGION_H_
