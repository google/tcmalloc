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
//
// Wrapping interface for HugeAllocator that handles backing and
// unbacking, including a hot cache of backed single hugepages.
#ifndef TCMALLOC_HUGE_CACHE_H_
#define TCMALLOC_HUGE_CACHE_H_
#include <stddef.h>
#include <stdint.h>

#include <algorithm>
#include <limits>

#include "absl/time/time.h"
#include "tcmalloc/common.h"
#include "tcmalloc/experiment.h"
#include "tcmalloc/experiment_config.h"
#include "tcmalloc/huge_allocator.h"
#include "tcmalloc/huge_pages.h"
#include "tcmalloc/internal/logging.h"
#include "tcmalloc/internal/timeseries_tracker.h"
#include "tcmalloc/stats.h"

namespace tcmalloc {

typedef void (*MemoryModifyFunction)(void *start, size_t len);

// Track the extreme values of a HugeLength value over the past
// kWindow (time ranges approximate.)
template <size_t kEpochs = 16>
class MinMaxTracker {
 public:
  explicit constexpr MinMaxTracker(ClockFunc clock, absl::Duration w)
      : kEpochLength(w / kEpochs), timeseries_(clock, w) {}

  void Report(HugeLength val);
  void Print(TCMalloc_Printer *out) const;
  void PrintInPbtxt(PbtxtRegion *hpaa) const;

  // If t < kEpochLength, these functions return statistics for last epoch. The
  // granularity is kEpochLength (rounded up).
  HugeLength MaxOverTime(absl::Duration t) const;
  HugeLength MinOverTime(absl::Duration t) const;

 private:
  const absl::Duration kEpochLength;

  static constexpr HugeLength kMaxVal =
      NHugePages(std::numeric_limits<size_t>::max());
  struct Extrema {
    HugeLength min, max;

    static Extrema Nil() {
      Extrema e;
      e.max = NHugePages(0);
      e.min = kMaxVal;
      return e;
    }

    void Report(HugeLength n) {
      max = std::max(max, n);
      min = std::min(min, n);
    }

    bool empty() const { return (*this == Nil()); }

    bool operator==(const Extrema &other) const;
    bool operator!=(const Extrema &other) const;
  };

  TimeSeriesTracker<Extrema, HugeLength, kEpochs> timeseries_;
};

// Explicit instantiations are defined in huge_cache.cc.
extern template class MinMaxTracker<>;
extern template class MinMaxTracker<600>;

template <size_t kEpochs>
constexpr HugeLength MinMaxTracker<kEpochs>::kMaxVal;

class MovingAverageTracker {
 public:
  explicit MovingAverageTracker(ClockFunc clock, absl::Duration time_constant,
                                absl::Duration resolution)
      : kTimeConstant(time_constant),
        kResolution(resolution),
        res_per_time_constant_(kTimeConstant / kResolution),
        clock_(clock),
        last_update_(clock_()) {}

  void Report(HugeLength val);

  HugeLength RollingMaxAverage() const;

 private:
  const absl::Duration kTimeConstant;
  const absl::Duration kResolution;
  const size_t res_per_time_constant_{0};

  static constexpr HugeLength kMaxVal =
      NHugePages(std::numeric_limits<size_t>::max());

  HugeLength last_max_ = NHugePages(0);
  HugeLength last_val_ = NHugePages(0);
  double rolling_max_average_{0};
  ClockFunc clock_;
  int64_t last_update_;
};

class HugeCache {
 public:
  // For use in production
  HugeCache(HugeAllocator *allocator, MetadataAllocFunction meta_allocate,
            MemoryModifyFunction unback)
      : HugeCache(allocator, meta_allocate, unback, GetCurrentTimeNanos) {}

  // For testing with mock clock
  HugeCache(HugeAllocator *allocator, MetadataAllocFunction meta_allocate,
            MemoryModifyFunction unback, ClockFunc clock)
      : allocator_(allocator),
        cache_(meta_allocate),
        clock_(clock),
        last_limit_change_(clock()),
        last_regret_update_(clock()),
        detailed_tracker_(clock, absl::Minutes(10)),
        usage_tracker_(clock, kCacheTime * 2),
        off_peak_tracker_(clock, kCacheTime * 2),
        size_tracker_(clock, kCacheTime * 2),
        unback_(unback) {}
  // Allocate a usable set of <n> contiguous hugepages.  Try to give out
  // memory that's currently backed from the kernel if we have it available.
  // *from_released is set to false if the return range is already backed;
  // otherwise, it is set to true (and the caller should back it.)
  HugeRange Get(HugeLength n, bool *from_released);

  // Deallocate <r> (assumed to be backed by the kernel.)
  void Release(HugeRange r);
  // As Release, but the range is assumed to _not_ be backed.
  void ReleaseUnbacked(HugeRange r);

  // Release to the system up to <n> hugepages of cache contents; returns
  // the number of hugepages released.
  HugeLength ReleaseCachedPages(HugeLength n);

  // Backed memory available.
  HugeLength size() const { return size_; }
  // Total memory cached (in HugeLength * nanoseconds)
  uint64_t regret() const { return regret_; }
  // Current limit for how much backed memory we'll cache.
  HugeLength limit() const { return limit_; }
  // Sum total of unreleased requests.
  HugeLength usage() const { return usage_; }

  size_t hits() const { return hits_; }
  size_t misses() const { return misses_; }
  uint64_t max_size() const { return max_size_.raw_num(); }
  uint64_t max_rss() const { return max_rss_.raw_num(); }
  uint64_t weighted_hits() const { return weighted_hits_; }
  uint64_t weighted_misses() const { return weighted_misses_; }

  const MinMaxTracker<> *usage_tracker() const { return &usage_tracker_; }

  void AddSpanStats(SmallSpanStats *small, LargeSpanStats *large,
                    PageAgeHistograms *ages) const;

  BackingStats stats() const {
    BackingStats s;
    s.system_bytes = (usage() + size()).in_bytes();
    s.free_bytes = size().in_bytes();
    s.unmapped_bytes = 0;
    return s;
  }

  void Print(TCMalloc_Printer *out);
  void PrintInPbtxt(PbtxtRegion *hpaa);

 private:
  HugeAllocator *allocator_;

  // We just cache-missed a request for <missed> pages;
  // should we grow?
  void MaybeGrowCacheLimit(HugeLength missed);
  // Check if the cache seems consistently too big.  Returns the
  // number of pages *evicted* (not the change in limit).
  HugeLength MaybeShrinkCacheLimit();

  // Ensure the cache contains at most <target> hugepages,
  // returning the number removed.
  HugeLength ShrinkCache(HugeLength target);

  HugeRange DoGet(HugeLength n, bool *from_released);

  HugeAddressMap::Node *Find(HugeLength n);

  HugeAddressMap cache_;
  HugeLength size_{NHugePages(0)};

  HugeLength limit_{NHugePages(10)};
  const absl::Duration kCacheTime = absl::Seconds(1);

  size_t hits_{0};
  size_t misses_{0};
  size_t fills_{0};
  size_t overflows_{0};
  uint64_t weighted_hits_{0};
  uint64_t weighted_misses_{0};

  // Sum(size of Gets) - Sum(size of Releases), i.e. amount of backed
  // hugepages our user currently wants to have.
  void IncUsage(HugeLength n);
  void DecUsage(HugeLength n);
  HugeLength usage_{NHugePages(0)};

  // This is tcmalloc::GetCurrentTimeNanos, except overridable for tests.
  ClockFunc clock_;
  int64_t last_limit_change_;

  // 10 hugepages is a good baseline for our cache--easily wiped away
  // by periodic release, and not that much memory on any real server.
  // However, we can go below it if we haven't used that much for 30 seconds.
  HugeLength MinCacheLimit() const { return NHugePages(10); }

  uint64_t regret_{0};  // overflows if we cache 585 hugepages for 1 year
  int64_t last_regret_update_;
  void UpdateSize(HugeLength size);

  MinMaxTracker<600> detailed_tracker_;

  MinMaxTracker<> usage_tracker_;
  MinMaxTracker<> off_peak_tracker_;
  MinMaxTracker<> size_tracker_;
  HugeLength max_size_{NHugePages(0)};
  HugeLength max_rss_{NHugePages(0)};

  HugeLength total_fast_unbacked_{NHugePages(0)};
  HugeLength total_periodic_unbacked_{NHugePages(0)};

  MemoryModifyFunction unback_;
};

}  // namespace tcmalloc

#endif  // TCMALLOC_HUGE_CACHE_H_
