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

#include "tcmalloc/huge_page_filler.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdint>
#include <optional>
#include <random>
#include <string>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "absl/algorithm/container.h"
#include "absl/base/internal/cycleclock.h"
#include "absl/base/macros.h"
#include "absl/base/thread_annotations.h"
#include "absl/container/flat_hash_map.h"
#include "absl/container/flat_hash_set.h"
#include "absl/flags/flag.h"
#include "absl/random/random.h"
#include "absl/status/status.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_format.h"
#include "absl/strings/str_join.h"
#include "absl/strings/string_view.h"
#include "absl/synchronization/blocking_counter.h"
#include "absl/synchronization/mutex.h"
#include "absl/time/time.h"
#include "tcmalloc/common.h"
#include "tcmalloc/huge_cache.h"
#include "tcmalloc/huge_page_subrelease.h"
#include "tcmalloc/huge_pages.h"
#include "tcmalloc/internal/clock.h"
#include "tcmalloc/internal/config.h"
#include "tcmalloc/internal/logging.h"
#include "tcmalloc/internal/memory_tag.h"
#include "tcmalloc/internal/pageflags.h"
#include "tcmalloc/internal/range_tracker.h"
#include "tcmalloc/internal/residency.h"
#include "tcmalloc/internal/system_allocator.h"
#include "tcmalloc/pages.h"
#include "tcmalloc/span.h"
#include "tcmalloc/stats.h"

using tcmalloc::tcmalloc_internal::Length;

ABSL_FLAG(Length, page_tracker_defrag_lim, Length(32),
          "Max allocation size for defrag test");

ABSL_FLAG(double, release_until, 0.01,
          "fraction of used we target in pageheap");
ABSL_FLAG(uint64_t, bytes, 1024 * 1024 * 1024, "baseline usage");
ABSL_FLAG(double, growth_factor, 2.0, "growth over baseline");

namespace tcmalloc {
namespace tcmalloc_internal {
namespace {

// This is an arbitrary distribution taken from page requests from
// an empirical driver test.  It seems realistic enough. We trim it to
// [1, last].
//
std::discrete_distribution<size_t> EmpiricalDistribution(Length last) {
  std::vector<size_t> page_counts = []() {
    std::vector<size_t> ret(12289);
    ret[1] = 375745576;
    ret[2] = 59737961;
    ret[3] = 35549390;
    ret[4] = 43896034;
    ret[5] = 17484968;
    ret[6] = 15830888;
    ret[7] = 9021717;
    ret[8] = 208779231;
    ret[9] = 3775073;
    ret[10] = 25591620;
    ret[11] = 2483221;
    ret[12] = 3595343;
    ret[13] = 2232402;
    ret[16] = 17639345;
    ret[21] = 4215603;
    ret[25] = 4212756;
    ret[28] = 760576;
    ret[30] = 2166232;
    ret[32] = 3021000;
    ret[40] = 1186302;
    ret[44] = 479142;
    ret[48] = 570030;
    ret[49] = 101262;
    ret[55] = 592333;
    ret[57] = 236637;
    ret[64] = 785066;
    ret[65] = 44700;
    ret[73] = 539659;
    ret[80] = 342091;
    ret[96] = 488829;
    ret[97] = 504;
    ret[113] = 242921;
    ret[128] = 157206;
    ret[129] = 145;
    ret[145] = 117191;
    ret[160] = 91818;
    ret[192] = 67824;
    ret[193] = 144;
    ret[225] = 40711;
    ret[256] = 38569;
    ret[257] = 1;
    ret[297] = 21738;
    ret[320] = 13510;
    ret[384] = 19499;
    ret[432] = 13856;
    ret[490] = 9849;
    ret[512] = 3024;
    ret[640] = 3655;
    ret[666] = 3963;
    ret[715] = 2376;
    ret[768] = 288;
    ret[1009] = 6389;
    ret[1023] = 2788;
    ret[1024] = 144;
    ret[1280] = 1656;
    ret[1335] = 2592;
    ret[1360] = 3024;
    ret[1536] = 432;
    ret[2048] = 288;
    ret[2560] = 72;
    ret[3072] = 360;
    ret[12288] = 216;
    return ret;
  }();

  Length lim = last;
  auto i = page_counts.begin();
  // remember lim might be too big (in which case we use the whole
  // vector...)

  auto j = page_counts.size() > lim.raw_num() ? i + (lim.raw_num() + 1)
                                              : page_counts.end();

  return std::discrete_distribution<size_t>(i, j);
}

class PageTrackerTest : public testing::Test {
 protected:
  PageTrackerTest()
      :  // an unlikely magic page
        huge_(HugePageContaining(reinterpret_cast<void*>(0x1abcde200000))),
        tracker_(huge_,
                 /*was_donated=*/false,
                 absl::base_internal::CycleClock::Now()) {}

  ~PageTrackerTest() override { mock_unback_.VerifyAndClear(); }

  struct PAlloc {
    PageId p;
    Length n;
    SpanAllocInfo span_alloc_info;

    PAlloc(PageId pp, Length nn, SpanAllocInfo s)
        : p(pp), n(nn), span_alloc_info(s) {}
  };

  void Mark(PAlloc a, size_t mark) {
    EXPECT_LE(huge_.first_page(), a.p);
    size_t index = (a.p - huge_.first_page()).raw_num();
    size_t end = index + a.n.raw_num();
    EXPECT_LE(end, kPagesPerHugePage.raw_num());
    for (; index < end; ++index) {
      marks_[index] = mark;
    }
  }

  class MockMemoryInterface final : public MemoryModifyFunction {
   public:
    [[nodiscard]] MemoryModifyStatus operator()(Range r) override {
      TC_CHECK_LT(actual_index_, ABSL_ARRAYSIZE(actual_));
      actual_[actual_index_].r = r;
      TC_CHECK_LT(actual_index_, ABSL_ARRAYSIZE(expected_));
      // Assume expected calls occur and use those return values.
      const bool success = expected_[actual_index_].success;
      const int error_number = expected_[actual_index_].error_number;
      ++actual_index_;
      return {.success = success, .error_number = error_number};
    }

    void Expect(PageId p, Length len, bool success, int error_number = 0) {
      TC_CHECK_LT(expected_index_, kMaxCalls);
      expected_[expected_index_] = {Range(p, len), success, error_number};
      ++expected_index_;
    }

    void VerifyAndClear() {
      EXPECT_EQ(expected_index_, actual_index_);
      for (size_t i = 0, n = std::min(expected_index_, actual_index_); i < n;
           ++i) {
        EXPECT_EQ(expected_[i].r.p, actual_[i].r.p);
        EXPECT_EQ(expected_[i].r.n, actual_[i].r.n);
      }
      expected_index_ = 0;
      actual_index_ = 0;
    }

   private:
    struct CallArgs {
      Range r;
      bool success = true;
      int error_number = 0;
    };

    static constexpr size_t kMaxCalls = 10;
    CallArgs expected_[kMaxCalls] = {};
    CallArgs actual_[kMaxCalls] = {};
    size_t expected_index_{0};
    size_t actual_index_{0};
  };

  MockMemoryInterface mock_collapse_;

  void Check(PAlloc a, size_t mark) {
    EXPECT_LE(huge_.first_page(), a.p);
    size_t index = (a.p - huge_.first_page()).raw_num();
    size_t end = index + a.n.raw_num();
    EXPECT_LE(end, kPagesPerHugePage.raw_num());
    for (; index < end; ++index) {
      EXPECT_EQ(marks_[index], mark);
    }
  }
  void ExpectUnbackPages(PAlloc a, bool success = true) {
    mock_unback_.Expect(a.p, a.n, success);
  }

  void ExpectCollapsedPages(PAlloc a, bool success = true,
                            int error_number = 0) {
    mock_collapse_.Expect(a.p, a.n, success, error_number);
  }

  PAlloc Get(Length n, SpanAllocInfo span_alloc_info) {
    PageHeapSpinLockHolder l;
    PageId p = tracker_.Get(n, span_alloc_info).page;
    return {p, n, span_alloc_info};
  }

  void Put(PAlloc a) {
    PageHeapSpinLockHolder l;
    tracker_.Put(Range(a.p, a.n), a.span_alloc_info);
  }

  Length ReleaseFree() {
    PageHeapSpinLockHolder l;
    return tracker_.ReleaseFree(mock_unback_);
  }

  // strict because release calls should only happen when we ask
  MockMemoryInterface mock_unback_;

  size_t marks_[kPagesPerHugePage.raw_num()];
  HugePage huge_;
  PageTracker tracker_;

  MemoryModifyStatus Collapse() { return tracker_.Collapse(mock_collapse_); }
};

class FakePageFlags : public PageFlagsBase {
 public:
  FakePageFlags() = default;
  std::optional<PageStats> Get(const void* addr, size_t size) override {
    // unimplemented
    return std::nullopt;
  }

  void MarkHugePageBacked(void* addr, bool is_hugepage_backed) {
    PageId p = PageIdContaining(addr);
    HugePage hp = HugePageContaining(p);
    is_hugepage_backed_[hp.start_addr()] = is_hugepage_backed;
  }

  bool IsHugepageBacked(const void* addr) override {
    PageId p = PageIdContaining(addr);
    HugePage hp = HugePageContaining(p);
    if (!is_hugepage_backed_.contains(hp.start_addr())) return false;
    return is_hugepage_backed_[hp.start_addr()];
  }

 private:
  absl::flat_hash_map<const void*, bool> is_hugepage_backed_;
};

class FakeResidency : public Residency {
 public:
  FakeResidency() = default;
  std::optional<Info> Get(const void* addr, size_t size) override {
    return std::nullopt;
  };

  SinglePageBitmaps GetUnbackedAndSwappedBitmaps(const void* addr) override {
    PageId p = PageIdContaining(addr);
    HugePage hp = HugePageContaining(p);
    EXPECT_TRUE(residency_bitmaps_.contains(hp.start_addr()));
    return residency_bitmaps_[hp.start_addr()];
  };

  void SetUnbackedAndSwappedBitmaps(const void* addr,
                                    const Bitmap<kMaxResidencyBits>& unbacked,
                                    const Bitmap<kMaxResidencyBits>& swapped) {
    PageId p = PageIdContaining(addr);
    HugePage hp = HugePageContaining(p);
    residency_bitmaps_[hp.start_addr()] = {unbacked, swapped,
                                           absl::StatusCode::kOk};
  }

  const size_t kNativePagesInHugePage = kHugePageSize / kPageSize;
  size_t GetNativePagesInHugePage() const override {
    return kNativePagesInHugePage;
  };

 private:
  absl::flat_hash_map<const void*, SinglePageBitmaps> residency_bitmaps_;
};

TEST_F(PageTrackerTest, AllocSane) {
  Length free = kPagesPerHugePage;
  auto n = Length(1);
  std::vector<PAlloc> allocs;
  // This should work without fragmentation.
  while (n <= free) {
    ASSERT_GE(tracker_.longest_free_range(), n);
    EXPECT_EQ(tracker_.used_pages(), kPagesPerHugePage - free);
    EXPECT_EQ(tracker_.free_pages(), free);
    PAlloc a = Get(n, {1, AccessDensityPrediction::kSparse});
    Mark(a, n.raw_num());
    allocs.push_back(a);
    free -= n;
    ++n;
  }

  // All should be distinct
  for (auto alloc : allocs) {
    Check(alloc, alloc.n.raw_num());
  }
}

TEST_F(PageTrackerTest, Collapse) {
  static const Length kAllocSize = kPagesPerHugePage / 4;
  SpanAllocInfo info = {1, AccessDensityPrediction::kSparse};
  PAlloc a1 = Get(kAllocSize, info);
  PAlloc a2 = Get(kAllocSize, info);
  PAlloc a3 = Get(kAllocSize, info);
  PAlloc a4 = Get(kAllocSize, info);

  PAlloc first_page_alloc = PAlloc(huge_.first_page(), kPagesPerHugePage, info);
  ExpectCollapsedPages(first_page_alloc, /*success=*/true);
  Collapse();
  mock_collapse_.VerifyAndClear();

  Put(a2);
  ExpectCollapsedPages(first_page_alloc, /*success=*/false);
  Collapse();
  mock_collapse_.VerifyAndClear();

  Put(a4);
  ExpectCollapsedPages(first_page_alloc, /*success=*/true);
  Collapse();
  mock_collapse_.VerifyAndClear();

  Put(a1);
  Put(a3);
}

TEST_F(PageTrackerTest, CollapseErrorNumber) {
  static const Length kAllocSize = kPagesPerHugePage / 4;
  SpanAllocInfo info = {1, AccessDensityPrediction::kSparse};
  PAlloc a1 = Get(kAllocSize, info);
  PAlloc a2 = Get(kAllocSize, info);

  PAlloc first_page_alloc = PAlloc(huge_.first_page(), kPagesPerHugePage, info);
  ExpectCollapsedPages(first_page_alloc, /*success=*/true, /*error_number=*/0);
  MemoryModifyStatus ret = Collapse();
  EXPECT_TRUE(ret.success);
  EXPECT_EQ(ret.error_number, 0);
  mock_collapse_.VerifyAndClear();

  Put(a2);
  ExpectCollapsedPages(first_page_alloc, /*success=*/false, /*error_number=*/1);
  ret = Collapse();
  EXPECT_FALSE(ret.success);
  EXPECT_EQ(ret.error_number, 1);
  mock_collapse_.VerifyAndClear();

  Put(a1);
}

TEST_F(PageTrackerTest, CollapseReleasedPage) {
  static const Length kAllocSize = kPagesPerHugePage / 4;
  SpanAllocInfo info = {1, AccessDensityPrediction::kSparse};
  PAlloc a1 = Get(kAllocSize, info);
  PAlloc a2 = Get(kAllocSize, info);
  PAlloc a3 = Get(kAllocSize, info);
  PAlloc a4 = Get(kAllocSize, info);

  PAlloc first_page_alloc = PAlloc(huge_.first_page(), kPagesPerHugePage, info);
  ExpectCollapsedPages(first_page_alloc, /*success=*/true);
  Collapse();
  mock_collapse_.VerifyAndClear();

  Put(a2);
  ExpectUnbackPages(a2, /*success=*/true);
  ReleaseFree();
  mock_unback_.VerifyAndClear();

  // The page was released, so we should not be able to collapse it.
  ASSERT_TRUE(tracker_.released());
  EXPECT_FALSE(Collapse().success);

  a2 = Get(kAllocSize, info);
  ASSERT_FALSE(tracker_.released());
  ExpectCollapsedPages(first_page_alloc, /*success=*/true);
  Collapse();
  mock_collapse_.VerifyAndClear();

  Put(a1);
  Put(a3);
  Put(a4);
}

TEST_F(PageTrackerTest, ReleasingReturn) {
  static const Length kAllocSize = kPagesPerHugePage / 4;
  SpanAllocInfo info = {1, AccessDensityPrediction::kSparse};
  PAlloc a1 = Get(kAllocSize - Length(3), info);
  PAlloc a2 = Get(kAllocSize, info);
  PAlloc a3 = Get(kAllocSize + Length(1), info);
  PAlloc a4 = Get(kAllocSize + Length(2), info);

  Put(a2);
  Put(a4);
  // We now have a hugepage that looks like [alloced] [free] [alloced] [free].
  // The free parts should be released when we mark the hugepage as such,
  // but not the allocated parts.
  ExpectUnbackPages(a2, /*success=*/true);
  ExpectUnbackPages(a4, /*success=*/true);
  ReleaseFree();
  mock_unback_.VerifyAndClear();

  EXPECT_EQ(tracker_.released_pages(), a2.n + a4.n);
  EXPECT_EQ(tracker_.free_pages(), a2.n + a4.n);

  Put(a1);
  Put(a3);
}

TEST_F(PageTrackerTest, ReleasingRetain) {
  static const Length kAllocSize = kPagesPerHugePage / 4;
  SpanAllocInfo info = {1, AccessDensityPrediction::kSparse};
  PAlloc a1 = Get(kAllocSize - Length(3), info);
  PAlloc a2 = Get(kAllocSize, info);
  PAlloc a3 = Get(kAllocSize + Length(1), info);
  PAlloc a4 = Get(kAllocSize + Length(2), info);

  Put(a2);
  Put(a4);
  // We now have a hugepage that looks like [alloced] [free] [alloced] [free].
  // The free parts should be released when we mark the hugepage as such,
  // but not the allocated parts.
  ExpectUnbackPages(a2);
  ExpectUnbackPages(a4);
  ReleaseFree();
  mock_unback_.VerifyAndClear();

  // Now we return the other parts, and they shouldn't get released.
  Put(a1);
  Put(a3);

  mock_unback_.VerifyAndClear();

  // But they will if we ReleaseFree.
  ExpectUnbackPages(a1);
  ExpectUnbackPages(a3);
  ReleaseFree();
  mock_unback_.VerifyAndClear();
}

TEST_F(PageTrackerTest, ReleasingRetainFailure) {
  static const Length kAllocSize = kPagesPerHugePage / 4;
  SpanAllocInfo info = {1, AccessDensityPrediction::kSparse};
  PAlloc a1 = Get(kAllocSize - Length(3), info);
  PAlloc a2 = Get(kAllocSize, info);
  PAlloc a3 = Get(kAllocSize + Length(1), info);
  PAlloc a4 = Get(kAllocSize + Length(2), info);

  Put(a2);
  Put(a4);
  // We now have a hugepage that looks like [alloced] [free] [alloced] [free].
  // The free parts should be released when we mark the hugepage as such if
  // successful, but not the allocated parts.
  ExpectUnbackPages(a2, /*success=*/true);
  ExpectUnbackPages(a4, /*success=*/false);
  ReleaseFree();
  mock_unback_.VerifyAndClear();

  EXPECT_EQ(tracker_.released_pages(), a2.n);
  EXPECT_EQ(tracker_.free_pages(), a2.n + a4.n);

  // Now we return the other parts, and they shouldn't get released.
  Put(a1);
  Put(a3);

  mock_unback_.VerifyAndClear();

  // But they will if we ReleaseFree.  We attempt to coalesce the deallocation
  // of a3/a4.
  ExpectUnbackPages(a1, /*success=*/true);
  ExpectUnbackPages(PAlloc{std::min(a3.p, a4.p), a3.n + a4.n, info},
                    /*success=*/false);
  ReleaseFree();
  mock_unback_.VerifyAndClear();

  EXPECT_EQ(tracker_.released_pages(), a1.n + a2.n);
  EXPECT_EQ(tracker_.free_pages(), a1.n + a2.n + a3.n + a4.n);
}

TEST_F(PageTrackerTest, Defrag) {
  absl::BitGen rng;
  const Length N = absl::GetFlag(FLAGS_page_tracker_defrag_lim);
  SpanAllocInfo info = {1, AccessDensityPrediction::kSparse};
  auto dist = EmpiricalDistribution(N);

  std::vector<PAlloc> allocs;

  std::vector<PAlloc> doomed;
  while (tracker_.longest_free_range() > Length(0)) {
    Length n;
    do {
      n = Length(dist(rng));
    } while (n > tracker_.longest_free_range());
    PAlloc a = Get(n, info);
    (absl::Bernoulli(rng, 1.0 / 2) ? allocs : doomed).push_back(a);
  }

  for (auto d : doomed) {
    Put(d);
  }

  static const size_t kReps = 250 * 1000;

  std::vector<double> frag_samples;
  std::vector<Length> longest_free_samples;
  frag_samples.reserve(kReps);
  longest_free_samples.reserve(kReps);
  for (size_t i = 0; i < kReps; ++i) {
    const Length free = kPagesPerHugePage - tracker_.used_pages();
    // Ideally, we'd like all of our free space to stay in a single
    // nice little run.
    const Length longest = tracker_.longest_free_range();
    double frag = free > Length(0)
                      ? static_cast<double>(longest.raw_num()) / free.raw_num()
                      : 1;

    if (i % (kReps / 25) == 0) {
      printf("free = %zu longest = %zu frag = %f\n", free.raw_num(),
             longest.raw_num(), frag);
    }
    frag_samples.push_back(frag);
    longest_free_samples.push_back(longest);

    // Randomly grow or shrink (picking the only safe option when we're either
    // full or empty.)
    if (tracker_.longest_free_range() == Length(0) ||
        (absl::Bernoulli(rng, 1.0 / 2) && !allocs.empty())) {
      size_t index = absl::Uniform<int32_t>(rng, 0, allocs.size());
      std::swap(allocs[index], allocs.back());
      Put(allocs.back());
      allocs.pop_back();
    } else {
      Length n;
      do {
        n = Length(dist(rng));
      } while (n > tracker_.longest_free_range());
      allocs.push_back(Get(n, info));
    }
  }

  std::sort(frag_samples.begin(), frag_samples.end());
  std::sort(longest_free_samples.begin(), longest_free_samples.end());

  {
    const double p10 = frag_samples[kReps * 10 / 100];
    const double p25 = frag_samples[kReps * 25 / 100];
    const double p50 = frag_samples[kReps * 50 / 100];
    const double p75 = frag_samples[kReps * 75 / 100];
    const double p90 = frag_samples[kReps * 90 / 100];
    printf("Fragmentation quantiles:\n");
    printf("p10: %f p25: %f p50: %f p75: %f p90: %f\n", p10, p25, p50, p75,
           p90);
    // We'd like to prety consistently rely on (75% of the time) reasonable
    // defragmentation (50% of space is fully usable...)
    // ...but we currently can't hit that mark consistently.
    EXPECT_GE(p25, 0.07);
  }

  {
    const Length p10 = longest_free_samples[kReps * 10 / 100];
    const Length p25 = longest_free_samples[kReps * 25 / 100];
    const Length p50 = longest_free_samples[kReps * 50 / 100];
    const Length p75 = longest_free_samples[kReps * 75 / 100];
    const Length p90 = longest_free_samples[kReps * 90 / 100];
    printf("Longest free quantiles:\n");
    printf("p10: %zu p25: %zu p50: %zu p75: %zu p90: %zu\n", p10.raw_num(),
           p25.raw_num(), p50.raw_num(), p75.raw_num(), p90.raw_num());
    // Similarly, we'd really like for there usually (p50) to be a space
    // for a large allocation (N - note that we've cooked the books so that
    // the page tracker is going to be something like half empty (ish) and N
    // is small, so that should be doable.)
    // ...but, of course, it isn't.
    EXPECT_GE(p50, Length(4));
  }

  for (auto a : allocs) {
    Put(a);
  }
}

TEST_F(PageTrackerTest, Stats) {
  struct Helper {
    static void Stat(const PageTracker& tracker,
                     std::vector<Length>* small_backed,
                     std::vector<Length>* small_unbacked,
                     LargeSpanStats* large) {
      SmallSpanStats small;
      *large = LargeSpanStats();
      tracker.AddSpanStats(&small, large);
      small_backed->clear();
      small_unbacked->clear();
      for (auto i = Length(0); i < kMaxPages; ++i) {
        for (int j = 0; j < small.normal_length[i.raw_num()]; ++j) {
          small_backed->push_back(i);
        }

        for (int j = 0; j < small.returned_length[i.raw_num()]; ++j) {
          small_unbacked->push_back(i);
        }
      }
    }
  };

  LargeSpanStats large;
  std::vector<Length> small_backed, small_unbacked;

  SpanAllocInfo info1 = {kPagesPerHugePage.raw_num(),
                         AccessDensityPrediction::kDense};
  const PageId p = Get(kPagesPerHugePage, info1).p;
  const PageId end = p + kPagesPerHugePage;
  PageId next = p;
  Length n = kMaxPages + Length(1);
  SpanAllocInfo info2 = {n.raw_num(), AccessDensityPrediction::kDense};
  Put({next, n, info2});
  next += kMaxPages + Length(1);

  Helper::Stat(tracker_, &small_backed, &small_unbacked, &large);
  EXPECT_THAT(small_backed, testing::ElementsAre());
  EXPECT_THAT(small_unbacked, testing::ElementsAre());
  EXPECT_EQ(large.spans, 1);
  EXPECT_EQ(large.normal_pages, kMaxPages + Length(1));
  EXPECT_EQ(large.returned_pages, Length(0));

  ++next;
  SpanAllocInfo info3 = {1, AccessDensityPrediction::kSparse};
  Put({next, Length(1), info3});
  next += Length(1);
  Helper::Stat(tracker_, &small_backed, &small_unbacked, &large);
  EXPECT_THAT(small_backed, testing::ElementsAre(Length(1)));
  EXPECT_THAT(small_unbacked, testing::ElementsAre());
  EXPECT_EQ(large.spans, 1);
  EXPECT_EQ(large.normal_pages, kMaxPages + Length(1));
  EXPECT_EQ(large.returned_pages, Length(0));

  ++next;
  SpanAllocInfo info4 = {2, AccessDensityPrediction::kSparse};
  Put({next, Length(2), info4});
  next += Length(2);
  Helper::Stat(tracker_, &small_backed, &small_unbacked, &large);
  EXPECT_THAT(small_backed, testing::ElementsAre(Length(1), Length(2)));
  EXPECT_THAT(small_unbacked, testing::ElementsAre());
  EXPECT_EQ(large.spans, 1);
  EXPECT_EQ(large.normal_pages, kMaxPages + Length(1));
  EXPECT_EQ(large.returned_pages, Length(0));

  ++next;
  SpanAllocInfo info5 = {3, AccessDensityPrediction::kSparse};
  Put({next, Length(3), info5});
  next += Length(3);
  ASSERT_LE(next, end);
  Helper::Stat(tracker_, &small_backed, &small_unbacked, &large);
  EXPECT_THAT(small_backed,
              testing::ElementsAre(Length(1), Length(2), Length(3)));
  EXPECT_THAT(small_unbacked, testing::ElementsAre());
  EXPECT_EQ(large.spans, 1);
  EXPECT_EQ(large.normal_pages, kMaxPages + Length(1));
  EXPECT_EQ(large.returned_pages, Length(0));

  n = kMaxPages + Length(1);
  ExpectUnbackPages({p, n, info2});
  ExpectUnbackPages({p + kMaxPages + Length(2), Length(1), info3});
  ExpectUnbackPages({p + kMaxPages + Length(4), Length(2), info4});
  ExpectUnbackPages({p + kMaxPages + Length(7), Length(3), info5});
  EXPECT_EQ(kMaxPages + Length(7), ReleaseFree());
  Helper::Stat(tracker_, &small_backed, &small_unbacked, &large);
  EXPECT_THAT(small_backed, testing::ElementsAre());
  EXPECT_THAT(small_unbacked,
              testing::ElementsAre(Length(1), Length(2), Length(3)));
  EXPECT_EQ(large.spans, 1);
  EXPECT_EQ(large.normal_pages, Length(0));
  EXPECT_EQ(large.returned_pages, kMaxPages + Length(1));
}

TEST_F(PageTrackerTest, b151915873) {
  // This test verifies, while generating statistics for the huge page, that we
  // do not go out-of-bounds in our bitmaps (b/151915873).

  // While the PageTracker relies on FindAndMark to decide which pages to hand
  // out, we do not specify where in the huge page we get our allocations.
  // Allocate single pages and then use their returned addresses to create the
  // desired pattern in the bitmaps, namely:
  //
  // |      | kPagesPerHugePage - 2 | kPagesPerHugePages - 1 |
  // | .... | not free              | free                   |
  //
  // This causes AddSpanStats to try index = kPagesPerHugePage - 1, n=1.  We
  // need to not overflow FindClear/FindSet.

  std::vector<PAlloc> allocs;
  allocs.reserve(kPagesPerHugePage.raw_num());
  SpanAllocInfo info = {1, AccessDensityPrediction::kSparse};
  for (int i = 0; i < kPagesPerHugePage.raw_num(); i++) {
    allocs.push_back(Get(Length(1), info));
  }

  std::sort(allocs.begin(), allocs.end(),
            [](const PAlloc& a, const PAlloc& b) { return a.p < b.p; });

  Put(allocs.back());
  allocs.erase(allocs.begin() + allocs.size() - 1);

  ASSERT_EQ(tracker_.used_pages(), kPagesPerHugePage - Length(1));

  SmallSpanStats small;
  LargeSpanStats large;

  tracker_.AddSpanStats(&small, &large);

  EXPECT_EQ(small.normal_length[1], 1);
  EXPECT_THAT(0,
              testing::AllOfArray(&small.normal_length[2],
                                  &small.normal_length[kMaxPages.raw_num()]));
}

class FakeClock {
 public:
  FakeClock() = default;
  static int64_t now() { return clock_; }
  static double freq() { return absl::ToDoubleNanoseconds(absl::Seconds(2)); }
  static void Advance(absl::Duration d) {
    clock_ += absl::ToDoubleSeconds(d) * freq();
  }
  static void ResetClock() { clock_ = 1234; }

 private:
  static int64_t clock_;
};

int64_t FakeClock::clock_ = 1234;

class MockCollapse final : public MemoryModifyFunction {
 public:
  MockCollapse() = default;
  [[nodiscard]] MemoryModifyStatus operator()(Range r) override {
    EXPECT_EQ(r.n, kPagesPerHugePage);
    ++collapsed_[r.start_addr()];
    FakeClock::Advance(latency_);
    return {.success = success_, .error_number = error_number_};
  }

  bool TriedCollapse(void* addr) const {
    PageId p = PageIdContaining(addr);
    HugePage hp = HugePageContaining(p);
    return collapsed_.contains(hp.start_addr());
  }

  int TimesCollapsed(void* addr) {
    PageId p = PageIdContaining(addr);
    HugePage hp = HugePageContaining(p);
    return collapsed_.contains(hp.start_addr()) ? collapsed_[hp.start_addr()]
                                                : 0;
  }

  void SetSuccess(bool success) { success_ = success; }
  void SetErrorNumber(int error_number) { error_number_ = error_number; }
  void SetLatency(absl::Duration latency) { latency_ = latency; }

 private:
  absl::flat_hash_map<void*, int> collapsed_;
  bool success_ = true;
  int error_number_ = 0;
  absl::Duration latency_;
};

class MockSetAnonVmaName final : public MemoryTagFunction {
 public:
  MockSetAnonVmaName() = default;
  void operator()(Range r, std::optional<absl::string_view> name) override {
    EXPECT_EQ(r.n, kPagesPerHugePage);
    if (name.has_value()) {
      EXPECT_EQ(name, expected_name_);
    } else {
      EXPECT_EQ(expected_name_, "tcmalloc_region_NORMAL");
    }
    ++times_called_;
  }
  void SetExpectedName(absl::string_view name) { expected_name_ = name; }
  int TimesCalled() { return times_called_; }

 private:
  absl::string_view expected_name_ = "tcmalloc_region_NORMAL";
  int times_called_ = 0;
};

class BlockingUnback final : public MemoryModifyFunction {
 public:
  constexpr BlockingUnback() = default;

  [[nodiscard]] MemoryModifyStatus operator()(Range r) override {
    if (!mu_) {
      return {.success = success_, .error_number = 0};
    }

    if (counter_) {
      counter_->DecrementCount();
    }

    mu_->lock();
    mu_->unlock();
    return {.success = success_, .error_number = 0};
  }

  absl::BlockingCounter* counter_ = nullptr;
  bool success_ = true;

 private:
  static thread_local absl::Mutex* mu_;
};

thread_local absl::Mutex* BlockingUnback::mu_ = nullptr;

class FillerTest : public testing::TestWithParam<bool> {
 protected:
  // We have backing of one word per (normal-sized) page for our "hugepages".
  std::vector<size_t> backing_;
  // This is space efficient enough that we won't bother recycling pages.
  HugePage GetBacking() {
    intptr_t i = backing_.size();
    backing_.resize(i + kPagesPerHugePage.raw_num());
    intptr_t addr = i << kPageShift;
    TC_CHECK_EQ(addr % kHugePageSize, 0);
    return HugePageContaining(reinterpret_cast<void*>(addr));
  }

  size_t* GetFakePage(PageId p) { return &backing_[p.index()]; }

  void MarkRange(PageId p, Length n, size_t mark) {
    for (auto i = Length(0); i < n; ++i) {
      *GetFakePage(p + i) = mark;
    }
  }

  void CheckRange(PageId p, Length n, size_t mark) {
    for (auto i = Length(0); i < n; ++i) {
      EXPECT_EQ(mark, *GetFakePage(p + i));
    }
  }

  HugePageTreatmentStats GetHugePageTreatmentStats()
      ABSL_LOCKS_EXCLUDED(pageheap_lock) {
    PageHeapSpinLockHolder l;
    return filler_.GetHugePageTreatmentStats();
  }

  HugePageFiller<PageTracker> filler_;
  BlockingUnback blocking_unback_;
  MockCollapse collapse_;
  MockSetAnonVmaName set_anon_vma_name_;
  bool use_preferential_collapse_;

  FillerTest()
      : filler_(
            Clock{.now = FakeClock::now, .freq = FakeClock::freq},
            MemoryTag::kNormal, blocking_unback_, blocking_unback_, collapse_,
            set_anon_vma_name_,
            HugePageFillerOptions{.use_preferential_collapse = GetParam()}) {
    FakeClock::ResetClock();
    // Reset success state
    blocking_unback_.success_ = true;
    use_preferential_collapse_ = GetParam();
  }

  ~FillerTest() override { EXPECT_EQ(filler_.size(), NHugePages(0)); }

  struct PAlloc {
    PageTracker* pt;
    PageId p;
    Length n;
    size_t mark;
    SpanAllocInfo span_alloc_info;
    bool from_released;
  };

  void Mark(const PAlloc& alloc) { MarkRange(alloc.p, alloc.n, alloc.mark); }

  void Check(const PAlloc& alloc) { CheckRange(alloc.p, alloc.n, alloc.mark); }

  size_t next_mark_{0};

  HugeLength hp_contained_{NHugePages(0)};
  Length total_allocated_{0};

  absl::InsecureBitGen gen_;
  // We usually choose the number of objects per span at random, but in tests
  // where the output is hardcoded, we disable randomization through the
  // variable below.
  bool randomize_density_ = true;

  void CheckStats() {
    EXPECT_EQ(filler_.size(), hp_contained_);
    auto stats = filler_.stats();
    const uint64_t freelist_bytes = stats.free_bytes + stats.unmapped_bytes;
    const uint64_t used_bytes = stats.system_bytes - freelist_bytes;
    EXPECT_EQ(used_bytes, total_allocated_.in_bytes());
    EXPECT_EQ(freelist_bytes,
              (hp_contained_.in_pages() - total_allocated_).in_bytes());
  }

  PAlloc AllocateWithSpanAllocInfo(Length n, SpanAllocInfo span_alloc_info,
                                   bool donated = false) {
    TC_CHECK_LE(n, kPagesPerHugePage);
    PAlloc ret = AllocateRaw(n, span_alloc_info, donated);
    ret.n = n;
    Mark(ret);
    CheckStats();
    return ret;
  }

  std::vector<PAlloc> AllocateVectorWithSpanAllocInfo(
      Length n, SpanAllocInfo span_alloc_info, bool donated = false) {
    TC_CHECK_LE(n, kPagesPerHugePage);
    Length t(0);
    std::vector<PAlloc> ret;
    Length alloc_len =
        (span_alloc_info.density == AccessDensityPrediction::kDense) ? Length(1)
                                                                     : n;
    while (t < n) {
      ret.push_back(AllocateRaw(alloc_len, span_alloc_info, donated));
      ret.back().n = alloc_len;
      Mark(ret.back());
      CheckStats();
      t += alloc_len;
    }
    return ret;
  }

  std::vector<PAlloc> AllocateVector(Length n, bool donated = false) {
    std::vector<PAlloc> ret;
    size_t objects =
        randomize_density_ ? (1 << absl::Uniform<size_t>(gen_, 0, 8)) : 1;
    AccessDensityPrediction density =
        randomize_density_
            ? (absl::Bernoulli(gen_, 0.5) ? AccessDensityPrediction::kSparse
                                          : AccessDensityPrediction::kDense)
            : AccessDensityPrediction::kSparse;

    SpanAllocInfo info = {.objects_per_span = objects, .density = density};
    Length alloc_len =
        (density == AccessDensityPrediction::kDense) ? Length(1) : n;
    Length total_len(0);
    while (total_len < n) {
      ret.push_back(AllocateRaw(alloc_len, info, donated));
      ret.back().n = alloc_len;
      Mark(ret.back());
      CheckStats();
      total_len += alloc_len;
    }
    return ret;
  }

  PAlloc Allocate(Length n, bool donated = false) {
    TC_CHECK_LE(n, kPagesPerHugePage);
    PAlloc ret;
    size_t objects =
        randomize_density_ ? (1 << absl::Uniform<size_t>(gen_, 0, 8)) : 1;

    AccessDensityPrediction density =
        randomize_density_
            ? (absl::Bernoulli(gen_, 0.5) ? AccessDensityPrediction::kSparse
                                          : AccessDensityPrediction::kDense)
            : AccessDensityPrediction::kSparse;
    SpanAllocInfo info = {.objects_per_span = objects, .density = density};
    ret = AllocateRaw(n, info, donated);
    ret.n = n;
    Mark(ret);
    CheckStats();
    return ret;
  }

  bool AllReleased(const std::vector<PAlloc>& pv) const {
    for (const auto& p : pv) {
      if (!p.pt->released()) return false;
    }
    return true;
  }

  // Returns true iff the filler returned an empty hugepage
  bool Delete(const PAlloc& p) {
    Check(p);
    bool r = DeleteRaw(p);
    CheckStats();
    return r;
  }

  // Return true iff the final Delete() call returns true.
  bool DeleteVector(const std::vector<PAlloc>& pv) {
    bool ret = false;
    for (const auto& p : pv) {
      ret = Delete(p);
    }
    return ret;
  }
  bool DeleteRange(std::vector<PAlloc>::iterator begin,
                   std::vector<PAlloc>::iterator end) {
    bool ret = false;
    for (auto it = begin; it != end; ++it) {
      ret = Delete(*it);
    }
    return ret;
  }

  Length ReleasePages(Length desired, SkipSubreleaseIntervals intervals = {}) {
    PageHeapSpinLockHolder l;
    return filler_.ReleasePages(desired, intervals,
                                /*release_partial_alloc_pages=*/false,
                                /*hit_limit=*/false);
  }

  Length ReleasePartialPages(Length desired,
                             SkipSubreleaseIntervals intervals = {}) {
    PageHeapSpinLockHolder l;
    return filler_.ReleasePages(desired, intervals,
                                /*release_partial_alloc_pages=*/true,
                                /*hit_limit=*/false);
  }

  Length HardReleasePages(Length desired) {
    PageHeapSpinLockHolder l;
    return filler_.ReleasePages(desired, SkipSubreleaseIntervals{},
                                /*release_partial_alloc_pages=*/false,
                                /*hit_limit=*/true);
  }

  void TreatHugepageTrackers(bool enable_collapse,
                             bool enable_release_free_swapped,
                             bool use_userspace_collapse_heuristics,
                             PageFlagsBase* pageflags, Residency* residency) {
    // Note that scoped pageheap lock isn't used here. This is because the
    // pageheap lock is manually unlocked before the collapse operation, and the
    // scoped lock doesn't recognize the manual unlock. In tests, collapse
    // allocates, so we use manual lock and unlock here.
    pageheap_lock.lock();
    filler_.TreatHugepageTrackers(enable_collapse, enable_release_free_swapped,
                                  use_userspace_collapse_heuristics, pageflags,
                                  residency);
    pageheap_lock.unlock();
  }

  // Generates an "interesting" pattern of allocations that highlights all the
  // various features of our stats.
  std::vector<PAlloc> GenerateInterestingAllocs();

  // Tests fragmentation
  void FragmentationTest();

  // Returns true iff the filler returned an empty hugepage.
  bool DeleteRaw(const PAlloc& p) {
    PageTracker* pt;
    {
      PageHeapSpinLockHolder l;
      pt = filler_.Put(p.pt, Range(p.p, p.n), p.span_alloc_info);
    }
    total_allocated_ -= p.n;
    if (pt != nullptr) {
      EXPECT_EQ(pt->longest_free_range(), kPagesPerHugePage);
      EXPECT_TRUE(pt->empty());
      --hp_contained_;
      delete pt;
      return true;
    }

    return false;
  }

 private:
  PAlloc AllocateRaw(Length n, SpanAllocInfo span_alloc_info, bool donated) {
    EXPECT_LT(n, kPagesPerHugePage);
    // Densely-accessed spans are not allocated from donated hugepages.  So
    // assert that we do not test such a situation.
    EXPECT_TRUE(!donated ||
                span_alloc_info.density == AccessDensityPrediction::kSparse);
    PAlloc ret;
    ret.n = n;
    ret.pt = nullptr;
    ret.mark = ++next_mark_;
    ret.span_alloc_info = span_alloc_info;
    if (!donated) {  // Donated means always create a new hugepage
      PageHeapSpinLockHolder l;
      auto [pt, page, from_released] = filler_.TryGet(n, span_alloc_info);
      ret.pt = pt;
      ret.p = page;
      ret.from_released = from_released;
    }
    if (ret.pt == nullptr) {
      ret.pt = new PageTracker(GetBacking(), donated, clock_);
      {
        PageHeapSpinLockHolder l;
        ret.p = ret.pt->Get(n, span_alloc_info).page;
      }
      filler_.Contribute(ret.pt, donated, span_alloc_info);
      ++hp_contained_;
    }

    total_allocated_ += n;
    return ret;
  }

  static int64_t clock_;
};

int64_t FillerTest::clock_{1234};

TEST_P(FillerTest, Density) {
  absl::BitGen rng;
  // Start with a really annoying setup: some hugepages half empty (randomly)
  std::vector<PAlloc> allocs;
  std::vector<PAlloc> doomed_allocs;
  static const HugeLength kNumHugePages = NHugePages(64);
  for (auto i = Length(0); i < kNumHugePages.in_pages(); ++i) {
    ASSERT_EQ(filler_.pages_allocated(), i);
    PAlloc p = Allocate(Length(1));
    if (absl::Bernoulli(rng, 1.0 / 2)) {
      allocs.push_back(p);
    } else {
      doomed_allocs.push_back(p);
    }
  }
  for (auto d : doomed_allocs) {
    Delete(d);
  }
  EXPECT_LE(filler_.size(), kNumHugePages + NHugePages(1));
  EXPECT_GE(filler_.size(), kNumHugePages);
  // We want a good chance of touching ~every allocation.
  size_t n = allocs.size();
  // Now, randomly add and delete to the allocations.
  // We should converge to full and empty pages.
  for (int j = 0; j < 6; j++) {
    absl::c_shuffle(allocs, rng);

    for (int i = 0; i < n; ++i) {
      Delete(allocs[i]);
      allocs[i] = Allocate(Length(1));
      ASSERT_EQ(filler_.pages_allocated(), Length(n));
    }
  }

  EXPECT_GE(allocs.size() / kPagesPerHugePage.raw_num() + 3,
            filler_.size().raw_num());

  // clean up, check for failures
  for (auto a : allocs) {
    Delete(a);
    ASSERT_EQ(filler_.pages_allocated(), Length(--n));
  }
}

TEST_P(FillerTest, ReleaseFreePagesWhenAnyPageIsSwappedRespectsClock) {
  const Length kAlloc = kPagesPerHugePage;
  std::vector<PAlloc> p1 = AllocateVector(kAlloc - Length(1));
  ASSERT_TRUE(!p1.empty());

  FakePageFlags pageflags;
  FakeResidency residency;
  for (const auto& pa : p1) {
    pageflags.MarkHugePageBacked(pa.p.start_addr(),
                                 /*is_hugepage_backed=*/false);
    EXPECT_FALSE(pageflags.IsHugepageBacked(pa.p.start_addr()));

    Bitmap<kMaxResidencyBits> unbacked, swapped;
    residency.SetUnbackedAndSwappedBitmaps(pa.p.start_addr(), unbacked,
                                           swapped);
  }
  // No pages should be released because there aren't any swapped pages.
  ASSERT_EQ(filler_.size(), NHugePages(1));
  TreatHugepageTrackers(/*enable_collapse=*/false,
                        /*enable_release_free_swapped=*/true,
                        /*use_userspace_collapse_heuristics=*/false, &pageflags,
                        &residency);
  EXPECT_EQ(filler_.subrelease_stats().total_pages_subreleased, Length(0));
  EXPECT_EQ(GetHugePageTreatmentStats().treated_pages_subreleased, 0);

  for (const auto& pa : p1) {
    pageflags.MarkHugePageBacked(pa.p.start_addr(),
                                 /*is_hugepage_backed=*/false);
    EXPECT_FALSE(pageflags.IsHugepageBacked(pa.p.start_addr()));

    Bitmap<kMaxResidencyBits> unbacked, swapped;
    swapped.SetRange(/*index=*/0, /*n=*/512);
    residency.SetUnbackedAndSwappedBitmaps(pa.p.start_addr(), unbacked,
                                           swapped);
  }
  // Though there is now any swapped page and some free/unreleased pages, we
  // haven't advanced the clock, so no pages should be released.
  TreatHugepageTrackers(/*enable_collapse=*/false,
                        /*enable_release_free_swapped=*/true,
                        /*use_userspace_collapse_heuristics=*/false, &pageflags,
                        &residency);
  EXPECT_EQ(filler_.subrelease_stats().total_pages_subreleased, Length(0));
  EXPECT_EQ(GetHugePageTreatmentStats().treated_pages_subreleased, 0);

  // Advance the clock and try again, the last page is free, so it
  // should be released.
  FakeClock::Advance(absl::Minutes(100));
  TreatHugepageTrackers(/*enable_collapse=*/false,
                        /*enable_release_free_swapped=*/true,
                        /*use_userspace_collapse_heuristics=*/false, &pageflags,
                        &residency);
  EXPECT_EQ(filler_.subrelease_stats().total_pages_subreleased, Length(1));
  EXPECT_EQ(GetHugePageTreatmentStats().treated_pages_subreleased, 1);
  DeleteVector(p1);
}

// Checks that the filler prefers to collapse dense hugepages before sparse
// hugepages.
TEST_P(FillerTest, CollapseDenseBeforeSparse) {
  const Length kAlloc = kPagesPerHugePage - Length(1);
  constexpr int kNumAllocs = 64;
  using AllocChunk = std::vector<PAlloc>;
  std::vector<AllocChunk> dense_allocs(kNumAllocs);
  std::vector<AllocChunk> sparse_allocs(kNumAllocs);
  FakePageFlags pageflags;
  FakeResidency residency;
  for (auto density :
       {AccessDensityPrediction::kDense, AccessDensityPrediction::kSparse}) {
    for (int i = 0; i < kNumAllocs; ++i) {
      SpanAllocInfo info;
      info.objects_per_span = 1;
      info.density = density;
      AllocChunk alloc = AllocateVectorWithSpanAllocInfo(kAlloc, info);
      if (density == AccessDensityPrediction::kDense) {
        dense_allocs[i] = alloc;
      } else {
        sparse_allocs[i] = alloc;
      }

      // Mark all the pages eligible for collapse.
      for (const auto& pa : alloc) {
        pageflags.MarkHugePageBacked(pa.p.start_addr(),
                                     /*is_hugepage_backed=*/false);
        EXPECT_FALSE(pageflags.IsHugepageBacked(pa.p.start_addr()));
        Bitmap<kMaxResidencyBits> unbacked, swapped;
        unbacked.SetRange(/*index=*/0, /*n=*/1);
        swapped.SetRange(/*index=*/0, /*n=*/1);
        residency.SetUnbackedAndSwappedBitmaps(pa.p.start_addr(), unbacked,
                                               swapped);
      }
    }
  }
  ASSERT_EQ(filler_.size(), NHugePages(kNumAllocs * 2));
  TreatHugepageTrackers(/*enable_collapse=*/true,
                        /*enable_release_free_swapped=*/false,
                        /*use_userspace_collapse_heuristics=*/true, &pageflags,
                        &residency);

  for (int i = 0; i < kNumAllocs; ++i) {
    for (const auto& pa : dense_allocs[i]) {
      EXPECT_TRUE(collapse_.TriedCollapse(pa.p.start_addr()));
      EXPECT_EQ(collapse_.TimesCollapsed(pa.p.start_addr()), 1);
    }
    for (const auto& pa : sparse_allocs[i]) {
      EXPECT_FALSE(collapse_.TriedCollapse(pa.p.start_addr()));
    }
  }

  for (int i = 0; i < kNumAllocs; ++i) {
    DeleteVector(dense_allocs[i]);
    DeleteVector(sparse_allocs[i]);
  }
}

// Checks that the filler orders collapse operations per the number of
// objects allocated on those hugepages.
TEST_P(FillerTest, CollapseOrderNObjects) {
  const Length kAlloc = kPagesPerHugePage - Length(1);
  constexpr int kNumAllocs = 64;
  using AllocChunk = std::vector<PAlloc>;
  std::vector<AllocChunk> one_object_allocs(kNumAllocs);
  std::vector<AllocChunk> multi_object_allocs(kNumAllocs);
  FakePageFlags pageflags;
  FakeResidency residency;
  for (int objects : {1, 16}) {
    for (int i = 0; i < kNumAllocs; ++i) {
      SpanAllocInfo info;
      info.objects_per_span = objects;
      info.density = AccessDensityPrediction::kSparse;
      AllocChunk alloc = AllocateVectorWithSpanAllocInfo(kAlloc, info);
      if (objects == 1) {
        one_object_allocs[i] = alloc;
      } else {
        multi_object_allocs[i] = alloc;
      }

      // Mark all the pages eligible for collapse.
      for (const auto& pa : alloc) {
        pageflags.MarkHugePageBacked(pa.p.start_addr(),
                                     /*is_hugepage_backed=*/false);
        EXPECT_FALSE(pageflags.IsHugepageBacked(pa.p.start_addr()));
        Bitmap<kMaxResidencyBits> unbacked, swapped;
        unbacked.SetRange(/*index=*/0, /*n=*/1);
        swapped.SetRange(/*index=*/0, /*n=*/1);
        residency.SetUnbackedAndSwappedBitmaps(pa.p.start_addr(), unbacked,
                                               swapped);
      }
    }
  }
  ASSERT_EQ(filler_.size(), NHugePages(kNumAllocs * 2));
  TreatHugepageTrackers(/*enable_collapse=*/true,
                        /*enable_release_free_swapped=*/false,
                        /*use_userspace_collapse_heuristics=*/true, &pageflags,
                        &residency);

  for (int i = 0; i < kNumAllocs; ++i) {
    for (const auto& pa : multi_object_allocs[i]) {
      EXPECT_TRUE(collapse_.TriedCollapse(pa.p.start_addr()));
      EXPECT_EQ(collapse_.TimesCollapsed(pa.p.start_addr()), 1);
    }
    for (const auto& pa : one_object_allocs[i]) {
      EXPECT_FALSE(collapse_.TriedCollapse(pa.p.start_addr()));
    }
  }

  for (int i = 0; i < kNumAllocs; ++i) {
    DeleteVector(multi_object_allocs[i]);
    DeleteVector(one_object_allocs[i]);
  }
}
// Checks that we release pages that are free when any page is swapped.
// Also checks that we don't release pages that were previously released.
// The second native page is swapped. The last two pages are free.
TEST_P(FillerTest, ReleaseFreePagesWhenAnyPageIsSwapped) {
  const Length kAlloc = kPagesPerHugePage;
  std::vector<PAlloc> p1 = AllocateVector(kAlloc - Length(2));
  ASSERT_TRUE(!p1.empty());

  FakePageFlags pageflags;
  FakeResidency residency;
  for (const auto& pa : p1) {
    pageflags.MarkHugePageBacked(pa.p.start_addr(),
                                 /*is_hugepage_backed=*/false);
    EXPECT_FALSE(pageflags.IsHugepageBacked(pa.p.start_addr()));

    Bitmap<kMaxResidencyBits> unbacked, swapped;
    swapped.SetRange(/*index=*/1, /*n=*/1);
    residency.SetUnbackedAndSwappedBitmaps(pa.p.start_addr(), unbacked,
                                           swapped);
  }

  ASSERT_EQ(filler_.size(), NHugePages(1));
  TreatHugepageTrackers(/*enable_collapse=*/false,
                        /*enable_release_free_swapped=*/true,
                        /*use_userspace_collapse_heuristics=*/false, &pageflags,
                        &residency);
  // We expect to release the two free pages, since the second native page is
  // swapped. We expect to log this correctly.
  EXPECT_EQ(filler_.subrelease_stats().total_pages_subreleased, Length(2));
  EXPECT_EQ(GetHugePageTreatmentStats().treated_pages_subreleased, 2);
  std::string buffer(1024 * 1024, '\0');
  {
    PageHeapSpinLockHolder l;
    Printer printer(&*buffer.begin(), buffer.size());
    filler_.Print(printer, true, pageflags);
  }
  buffer.resize(strlen(buffer.c_str()));
  EXPECT_THAT(buffer,
              testing::HasSubstr("HugePageFiller: In the previous treatment "
                                 "interval, subreleased 2 pages."));

  // Check that pages are not released again. We have to advance the clock.
  FakeClock::Advance(absl::Minutes(100));
  TreatHugepageTrackers(/*enable_collapse=*/false,
                        /*enable_release_free_swapped=*/true,
                        /*use_userspace_collapse_heuristics=*/false, &pageflags,
                        &residency);
  EXPECT_EQ(filler_.subrelease_stats().total_pages_subreleased, Length(2));
  EXPECT_EQ(GetHugePageTreatmentStats().treated_pages_subreleased, 0);

  DeleteVector(p1);
}

// Checks that we don't release pages when there aren't any pages that are
// swapped.
TEST_P(FillerTest, ReleaseNoFreePages) {
  const Length kAlloc = kPagesPerHugePage / 2;
  // 2 TCMalloc pages are free.
  std::vector<PAlloc> p1 = AllocateVector(kAlloc - Length(2));
  ASSERT_TRUE(!p1.empty());

  FakePageFlags pageflags;
  FakeResidency residency;
  for (const auto& pa : p1) {
    pageflags.MarkHugePageBacked(pa.p.start_addr(),
                                 /*is_hugepage_backed=*/false);
    EXPECT_FALSE(pageflags.IsHugepageBacked(pa.p.start_addr()));

    // No pages are swapped.
    Bitmap<kMaxResidencyBits> unbacked, swapped;
    residency.SetUnbackedAndSwappedBitmaps(pa.p.start_addr(), unbacked,
                                           swapped);
  }

  ASSERT_EQ(filler_.size(), NHugePages(1));
  TreatHugepageTrackers(/*enable_collapse=*/false,
                        /*enable_release_free_swapped=*/true,
                        /*use_userspace_collapse_heuristics=*/false, &pageflags,
                        &residency);

  SubreleaseStats subrelease_stats = filler_.subrelease_stats();
  EXPECT_EQ(subrelease_stats.total_pages_subreleased, Length(0));
  EXPECT_EQ(GetHugePageTreatmentStats().treated_pages_subreleased, 0);
  DeleteVector(p1);
}

// Given two hugepages with free pages, checks that an alloc comes from the
// intact hugepage instead of the one from which we subreleased EVEN IF it has a
// more suitable LFR.
TEST_P(FillerTest, CheckAllocationsComeFromIntactHugepage) {
  // Fill up the first hugepage with elements from p1 and p2. Fill up almost the
  // entire second hugepage with elements from p3. Delete p2 so that both
  // hugepages have 2 free pages.
  std::vector<PAlloc> p1 = AllocateVector(kPagesPerHugePage - Length(2));
  std::vector<PAlloc> p2 =
      AllocateVectorWithSpanAllocInfo(Length(2), p1.back().span_alloc_info);
  std::vector<PAlloc> p3 = AllocateVectorWithSpanAllocInfo(
      kPagesPerHugePage - Length(3), p1.back().span_alloc_info);
  DeleteVector(p2);

  FakePageFlags pageflags;
  FakeResidency residency;
  for (const auto& pa : p1) {
    pageflags.MarkHugePageBacked(pa.p.start_addr(),
                                 /*is_hugepage_backed=*/false);
    EXPECT_FALSE(pageflags.IsHugepageBacked(pa.p.start_addr()));
    Bitmap<kMaxResidencyBits> unbacked, swapped;
    swapped.SetRange(/*index=*/0, /*n=*/5);
    residency.SetUnbackedAndSwappedBitmaps(pa.p.start_addr(), unbacked,
                                           swapped);
  }

  for (const auto& pa : p3) {
    pageflags.MarkHugePageBacked(pa.p.start_addr(),
                                 /*is_hugepage_backed=*/false);
    EXPECT_FALSE(pageflags.IsHugepageBacked(pa.p.start_addr()));
    // No pages are swapped.
    Bitmap<kMaxResidencyBits> unbacked, swapped;
    residency.SetUnbackedAndSwappedBitmaps(pa.p.start_addr(), unbacked,
                                           swapped);
  }

  // We have two hugepages with free pages. Since the first hugepage has
  // at least one swapped page, we subrelease from that.
  ASSERT_EQ(filler_.size(), NHugePages(2));
  TreatHugepageTrackers(/*enable_collapse=*/false,
                        /*enable_release_free_swapped=*/true,
                        /*use_userspace_collapse_heuristics=*/false, &pageflags,
                        &residency);

  // There should be two pages released, from p1's hugepage.
  EXPECT_EQ(filler_.subrelease_stats().total_pages_subreleased, Length(2));
  EXPECT_EQ(GetHugePageTreatmentStats().treated_pages_subreleased, 2);
  // We make an allocation. We expect it to come from the same hugepage as
  // the elements of p3, since this hugepage has not been subreleased from,
  // while the hugepage containing elements form p1 has been subreleased from.
  // This is the case even though the first hugepage has a more suitable LFR (2
  // vs 3) for an allocation of 1 page.
  PAlloc p = AllocateWithSpanAllocInfo(Length(1), p3.back().span_alloc_info);
  EXPECT_EQ(p.pt->location().start_addr(),
            p3.back().pt->location().start_addr());

  Delete(p);
  DeleteVector(p1);
  DeleteVector(p3);
}

// Parallelizes collapse and background swapped-subrelease operation,
// concurrently with deallocating certain allocations. By deallocating
// concurrently, some trackers end up in fully freed lists. We want to make sure
// that we handle this case correctly.
TEST_P(FillerTest, ParallelCollapseRelease) {
  std::atomic<bool> done(false);

  FakePageFlags pageflags;
  FakeResidency residency;
  SpanAllocInfo info;
  info.objects_per_span = 1;
  info.density = AccessDensityPrediction::kSparse;
  std::vector<PAlloc> allocated;
  const Length kAlloc = kPagesPerHugePage / 2 + Length(1);
  for (int i = 0; i < 1000; ++i) {
    PAlloc p1 = AllocateWithSpanAllocInfo(kAlloc, info);
    allocated.push_back(p1);
    pageflags.MarkHugePageBacked(p1.p.start_addr(),
                                 /*is_hugepage_backed=*/false);
    EXPECT_FALSE(pageflags.IsHugepageBacked(p1.p.start_addr()));
    Bitmap<kMaxResidencyBits> unbacked, swapped;
    unbacked.SetRange(/*index=*/0, /*n=*/128);
    swapped.SetRange(/*index=*/128, /*n=*/128);
    residency.SetUnbackedAndSwappedBitmaps(p1.p.start_addr(), unbacked,
                                           swapped);
  }

  auto collase_function = [&]() {
    absl::BitGen rng;
    while (!done.load(std::memory_order_acquire)) {
      TreatHugepageTrackers(/*enable_collapse=*/true,
                            /*enable_release_free_swapped=*/true,
                            /*use_userspace_collapse_heuristics=*/false,
                            &pageflags, &residency);
    }
  };
  std::thread collapse_thread = std::thread(collase_function);

  // Note that we need to use DeleteRaw here, since Delete checks for filler
  // stats as well. As interleaving collapse operations might not delete all the
  // fully freed trackers, and because CheckStats doesn't hold a lock, the
  // intermediate stat check might fail. Instead, we check for stats in the end.
  for (auto& p : allocated) {
    DeleteRaw(p);
  }

  done = true;
  collapse_thread.join();

  while (true) {
    PageTracker* pt;
    {
      PageHeapSpinLockHolder l;
      pt = filler_.FetchFullyFreedTracker();
    }
    if (pt == nullptr) {
      break;
    }
    EXPECT_EQ(pt->longest_free_range(), kPagesPerHugePage);
    EXPECT_TRUE(pt->empty());
    --hp_contained_;
    delete pt;
  }
  CheckStats();
}

// Makes sure that we do not collapse the pages that are already hugepage
// backed.
TEST_P(FillerTest, DontCollapseAlreadyHugepages) {
  const Length kAlloc = kPagesPerHugePage / 2;
  std::vector<PAlloc> p1 = AllocateVector(kAlloc - Length(1));
  ASSERT_TRUE(!p1.empty());
  FakePageFlags pageflags;
  FakeResidency residency;

  for (const auto& pa : p1) {
    pageflags.MarkHugePageBacked(pa.p.start_addr(),
                                 /*is_hugepage_backed=*/true);
    EXPECT_TRUE(pageflags.IsHugepageBacked(pa.p.start_addr()));
    Bitmap<kMaxResidencyBits> unbacked, swapped;
    residency.SetUnbackedAndSwappedBitmaps(pa.p.start_addr(), unbacked,
                                           swapped);
  }
  ASSERT_EQ(filler_.size(), NHugePages(1));
  TreatHugepageTrackers(/*enable_collapse=*/true,
                        /*enable_release_free_swapped=*/false,
                        /*use_userspace_collapse_heuristics=*/false, &pageflags,
                        &residency);

  for (const auto& pa : p1) {
    EXPECT_FALSE(collapse_.TriedCollapse(pa.p.start_addr()));
  }
  HugePageTreatmentStats treatment_stats = GetHugePageTreatmentStats();
  EXPECT_EQ(treatment_stats.collapse_eligible, 1);
  EXPECT_EQ(treatment_stats.collapse_attempted, 0);
  EXPECT_EQ(treatment_stats.collapse_succeeded, 0);
  EXPECT_EQ(treatment_stats.collapse_time_total_cycles, 0);
  EXPECT_EQ(treatment_stats.collapse_time_max_cycles, 0);
  DeleteVector(p1);
}

TEST_P(FillerTest, DontCollapseAlreadyCollapsed) {
  const Length kAlloc = kPagesPerHugePage / 2;
  std::vector<PAlloc> p1 = AllocateVector(kAlloc - Length(1));
  ASSERT_TRUE(!p1.empty());
  FakePageFlags pageflags;
  FakeResidency residency;
  auto check_stats = [&](int expected_eligible, int expected_attempted,
                         int expected_succeeded) {
    HugePageTreatmentStats treatment_stats = GetHugePageTreatmentStats();
    EXPECT_EQ(treatment_stats.collapse_eligible, expected_eligible);
    EXPECT_EQ(treatment_stats.collapse_attempted, expected_attempted);
    EXPECT_EQ(treatment_stats.collapse_succeeded, expected_succeeded);
    EXPECT_EQ(treatment_stats.collapse_time_total_cycles, 0);
    EXPECT_EQ(treatment_stats.collapse_time_max_cycles, 0);
  };

  for (const auto& pa : p1) {
    pageflags.MarkHugePageBacked(pa.p.start_addr(),
                                 /*is_hugepage_backed=*/false);
    EXPECT_FALSE(pageflags.IsHugepageBacked(pa.p.start_addr()));
    Bitmap<kMaxResidencyBits> unbacked, swapped;
    residency.SetUnbackedAndSwappedBitmaps(pa.p.start_addr(), unbacked,
                                           swapped);
  }
  ASSERT_EQ(filler_.size(), NHugePages(1));
  TreatHugepageTrackers(/*enable_collapse=*/true,
                        /*enable_release_free_swapped=*/false,
                        /*use_userspace_collapse_heuristics=*/false, &pageflags,
                        &residency);
  check_stats(/*expected_eligible=*/1, /*expected_attempted=*/1,
              /*expected_succeeded=*/1);

  HugePageTreatmentStats treatment_stats = GetHugePageTreatmentStats();
  EXPECT_EQ(treatment_stats.collapse_eligible, 1);
  EXPECT_EQ(treatment_stats.collapse_attempted, 1);
  EXPECT_EQ(treatment_stats.collapse_succeeded, 1);

  for (const auto& pa : p1) {
    EXPECT_TRUE(collapse_.TriedCollapse(pa.p.start_addr()));
    EXPECT_EQ(collapse_.TimesCollapsed(pa.p.start_addr()), 1);
  }

  // The first collapse was successful, so the second collapse should not
  // occur.
  TreatHugepageTrackers(/*enable_collapse=*/true,
                        /*enable_release_free_swapped=*/false,
                        /*use_userspace_collapse_heuristics=*/false, &pageflags,
                        &residency);
  for (const auto& pa : p1) {
    EXPECT_EQ(collapse_.TimesCollapsed(pa.p.start_addr()), 1);
  }
  check_stats(/*expected_eligible=*/1, /*expected_attempted=*/1,
              /*expected_succeeded=*/1);
  DeleteVector(p1);
}

// Checks that the anonymous VMA name is being recorded correctly for the
// sampled tracker.
TEST_P(FillerTest, SetAnonVmaName) {
  FakePageFlags pageflags;
  FakeResidency residency;
  SpanAllocInfo info;
  info.objects_per_span = 256;
  info.density = AccessDensityPrediction::kDense;
  PAlloc p = AllocateWithSpanAllocInfo(Length(1), info);
  p.pt->SetTagState({.sampled_for_tagging = true});
  set_anon_vma_name_.SetExpectedName(
      "tcmalloc_region_NORMAL_page_8192_lfr_240_nallocs_0_nobjects_256_dense_1_"
      "released_0");

  FakeClock::Advance(absl::Minutes(10));
  TreatHugepageTrackers(/*enable_collapse=*/false,
                        /*enable_release_free_swapped=*/false,
                        /*use_userspace_collapse_heuristics=*/false, &pageflags,
                        &residency);
  EXPECT_EQ(set_anon_vma_name_.TimesCalled(), 1);

  // Advance clock by 10 seconds, which is not enough to sample the tracker
  // again.
  FakeClock::Advance(absl::Seconds(10));
  TreatHugepageTrackers(/*enable_collapse=*/false,
                        /*enable_release_free_swapped=*/false,
                        /*use_userspace_collapse_heuristics=*/false, &pageflags,
                        &residency);
  EXPECT_EQ(set_anon_vma_name_.TimesCalled(), 1);

  // Allocate and advance.  nobjects should round up.
  info.objects_per_span = 128;
  PAlloc p2 = AllocateWithSpanAllocInfo(Length(1), info);

  set_anon_vma_name_.SetExpectedName(
      "tcmalloc_region_NORMAL_page_8192_lfr_240_nallocs_0_nobjects_512_dense_1_"
      "released_0");

  FakeClock::Advance(absl::Minutes(10));
  TreatHugepageTrackers(/*enable_collapse=*/false,
                        /*enable_release_free_swapped=*/false,
                        /*use_userspace_collapse_heuristics=*/false, &pageflags,
                        &residency);
  EXPECT_EQ(set_anon_vma_name_.TimesCalled(), 2);

  // Allocate and advance.  nobjects should remain unchanged.
  PAlloc p3 = AllocateWithSpanAllocInfo(Length(1), info);

  FakeClock::Advance(absl::Minutes(10));
  TreatHugepageTrackers(/*enable_collapse=*/false,
                        /*enable_release_free_swapped=*/false,
                        /*use_userspace_collapse_heuristics=*/false, &pageflags,
                        &residency);
  EXPECT_EQ(set_anon_vma_name_.TimesCalled(), 3);

  set_anon_vma_name_.SetExpectedName("tcmalloc_region_NORMAL");
  Delete(p);
  Delete(p2);
  Delete(p3);
  EXPECT_EQ(set_anon_vma_name_.TimesCalled(), 4);
}

// Checks that we collapse hugepages that are eligible to be collapsed.
TEST_P(FillerTest, CollapseHugepages) {
  const Length kAlloc = kPagesPerHugePage / 2;
  std::vector<PAlloc> p1 = AllocateVector(kAlloc - Length(1));
  ASSERT_TRUE(!p1.empty());

  FakePageFlags pageflags;
  FakeResidency residency;
  for (const auto& pa : p1) {
    pageflags.MarkHugePageBacked(pa.p.start_addr(),
                                 /*is_hugepage_backed=*/false);
    EXPECT_FALSE(pageflags.IsHugepageBacked(pa.p.start_addr()));

    Bitmap<kMaxResidencyBits> unbacked, swapped;
    unbacked.SetRange(/*index=*/0, /*n=*/1);
    swapped.SetRange(/*index=*/0, /*n=*/1);
    residency.SetUnbackedAndSwappedBitmaps(pa.p.start_addr(), unbacked,
                                           swapped);
  }

  ASSERT_EQ(filler_.size(), NHugePages(1));
  TreatHugepageTrackers(/*enable_collapse=*/true,
                        /*enable_release_free_swapped=*/false,
                        /*use_userspace_collapse_heuristics=*/false, &pageflags,
                        &residency);
  for (const auto& pa : p1) {
    EXPECT_TRUE(collapse_.TriedCollapse(pa.p.start_addr()));
    EXPECT_EQ(collapse_.TimesCollapsed(pa.p.start_addr()), 1);
  }
  HugePageTreatmentStats treatment_stats = GetHugePageTreatmentStats();
  EXPECT_EQ(treatment_stats.collapse_eligible, 1);
  EXPECT_EQ(treatment_stats.collapse_attempted, 1);
  EXPECT_EQ(treatment_stats.collapse_succeeded, 1);

  DeleteVector(p1);
}

// Checks that we do not collapse hugepages that are not eligible to be
// collapsed.
TEST_P(FillerTest, DontCollapseHugepages) {
  const Length kAlloc = kPagesPerHugePage / 2;

  auto test_collapse = [&](size_t total_unbacked, size_t total_swapped) {
    std::vector<PAlloc> p1 = AllocateVector(kAlloc - Length(1));
    ASSERT_TRUE(!p1.empty());

    FakePageFlags pageflags;
    FakeResidency residency;
    for (const auto& pa : p1) {
      pageflags.MarkHugePageBacked(pa.p.start_addr(),
                                   /*is_hugepage_backed=*/false);
      EXPECT_FALSE(pageflags.IsHugepageBacked(pa.p.start_addr()));

      Bitmap<kMaxResidencyBits> unbacked, swapped;
      unbacked.SetRange(/*index=*/0, total_unbacked);
      swapped.SetRange(/*index=*/0, total_swapped);
      residency.SetUnbackedAndSwappedBitmaps(pa.p.start_addr(), unbacked,
                                             swapped);
    }

    ASSERT_EQ(filler_.size(), NHugePages(1));
    TreatHugepageTrackers(/*enable_collapse=*/true,
                          /*enable_release_free_swapped=*/false,
                          /*use_userspace_collapse_heuristics=*/false,
                          &pageflags, &residency);
    for (const auto& pa : p1) {
      EXPECT_FALSE(collapse_.TriedCollapse(pa.p.start_addr()));
    }

    DeleteVector(p1);
  };

  for (auto unbacked : {kMaxResidencyBits / 2, kMaxResidencyBits}) {
    for (auto swapped : {kMaxResidencyBits / 2, kMaxResidencyBits}) {
      test_collapse(unbacked, swapped);
    }
  }

  HugePageTreatmentStats treatment_stats = GetHugePageTreatmentStats();
  EXPECT_EQ(treatment_stats.collapse_eligible, 4);
  EXPECT_EQ(treatment_stats.collapse_attempted, 0);
  EXPECT_EQ(treatment_stats.collapse_succeeded, 0);
  EXPECT_EQ(treatment_stats.collapse_time_total_cycles, 0);
  EXPECT_EQ(treatment_stats.collapse_time_max_cycles, 0);
}

TEST_P(FillerTest, CollapseLatency) {
  const Length kAlloc = kPagesPerHugePage / 2;
  std::vector<PAlloc> p1 = AllocateVector(kAlloc - Length(1));
  ASSERT_TRUE(!p1.empty());

  FakePageFlags pageflags;
  FakeResidency residency;
  for (const auto& pa : p1) {
    pageflags.MarkHugePageBacked(pa.p.start_addr(),
                                 /*is_hugepage_backed=*/false);
    EXPECT_FALSE(pageflags.IsHugepageBacked(pa.p.start_addr()));

    Bitmap<kMaxResidencyBits> unbacked, swapped;
    unbacked.SetRange(/*index=*/0, /*n=*/1);
    swapped.SetRange(/*index=*/0, /*n=*/1);
    residency.SetUnbackedAndSwappedBitmaps(pa.p.start_addr(), unbacked,
                                           swapped);
  }

  ASSERT_EQ(filler_.size(), NHugePages(1));

  HugePageTreatmentStats treatment_stats = GetHugePageTreatmentStats();
  EXPECT_EQ(treatment_stats.collapse_time_total_cycles, 0);
  EXPECT_EQ(treatment_stats.collapse_time_max_cycles, 0);

  const absl::Duration latency = absl::Microseconds(100);
  collapse_.SetLatency(latency);
  TreatHugepageTrackers(/*enable_collapse=*/true,
                        /*enable_release_free_swapped=*/false,
                        /*use_userspace_collapse_heuristics=*/false, &pageflags,
                        &residency);
  for (const auto& pa : p1) {
    EXPECT_TRUE(collapse_.TriedCollapse(pa.p.start_addr()));
    EXPECT_EQ(collapse_.TimesCollapsed(pa.p.start_addr()), 1);
  }
  treatment_stats = GetHugePageTreatmentStats();
  EXPECT_EQ(treatment_stats.collapse_eligible, 1);
  EXPECT_EQ(treatment_stats.collapse_attempted, 1);
  EXPECT_EQ(treatment_stats.collapse_succeeded, 1);
  EXPECT_EQ(treatment_stats.collapse_time_total_cycles,
            absl::ToDoubleSeconds(latency) * FakeClock::freq());
  EXPECT_EQ(treatment_stats.collapse_time_max_cycles,
            absl::ToDoubleSeconds(latency) * FakeClock::freq());

  DeleteVector(p1);
}

// Test that collapse mechanism backs off as soon as it encounters the first
// operation with a high latency.
TEST_P(FillerTest, EarlyBackoff) {
  const Length kAlloc = kPagesPerHugePage - Length(1);

  collapse_.SetLatency(kMaxCollapseLatencyThreshold + absl::Milliseconds(1));

  SpanAllocInfo info;
  info.objects_per_span = 1;
  info.density = AccessDensityPrediction::kSparse;
  std::vector<PAlloc> p1 = AllocateVectorWithSpanAllocInfo(kAlloc, info);
  std::vector<PAlloc> p2 =
      AllocateVectorWithSpanAllocInfo(kAlloc - Length(1), info);
  ASSERT_TRUE(!p1.empty());
  ASSERT_TRUE(!p2.empty());

  FakePageFlags pageflags;
  FakeResidency residency;
  for (const auto& pa : p1) {
    pageflags.MarkHugePageBacked(pa.p.start_addr(),
                                 /*is_hugepage_backed=*/false);
    EXPECT_FALSE(pageflags.IsHugepageBacked(pa.p.start_addr()));

    Bitmap<kMaxResidencyBits> unbacked, swapped;
    unbacked.SetRange(/*index=*/0, /*n=*/1);
    swapped.SetRange(/*index=*/0, /*n=*/1);
    residency.SetUnbackedAndSwappedBitmaps(pa.p.start_addr(), unbacked,
                                           swapped);
  }

  // Allocate a second hugepage with longer free range than p1. This ensures
  // collapse mechanism will first attempt to collapse p1, and then p2.
  for (const auto& pa : p2) {
    pageflags.MarkHugePageBacked(pa.p.start_addr(),
                                 /*is_hugepage_backed=*/false);
    EXPECT_FALSE(pageflags.IsHugepageBacked(pa.p.start_addr()));

    Bitmap<kMaxResidencyBits> unbacked, swapped;
    unbacked.SetRange(/*index=*/0, /*n=*/1);
    swapped.SetRange(/*index=*/0, /*n=*/1);
    residency.SetUnbackedAndSwappedBitmaps(pa.p.start_addr(), unbacked,
                                           swapped);
  }

  ASSERT_EQ(filler_.size(), NHugePages(2));

  TreatHugepageTrackers(/*enable_collapse=*/true,
                        /*enable_release_free_swapped=*/false,
                        /*use_userspace_collapse_heuristics=*/false, &pageflags,
                        &residency);

  // As latency of each collapse is high, p1 should have been collapsed, but
  // p2 should have been skipped.
  for (const auto& pa : p1) {
    EXPECT_TRUE(collapse_.TriedCollapse(pa.p.start_addr()));
    EXPECT_EQ(collapse_.TimesCollapsed(pa.p.start_addr()), 1);
  }

  for (const auto& pa : p2) {
    EXPECT_FALSE(collapse_.TriedCollapse(pa.p.start_addr()));
    EXPECT_EQ(collapse_.TimesCollapsed(pa.p.start_addr()), 0);
  }

  HugePageTreatmentStats treatment_stats = GetHugePageTreatmentStats();
  EXPECT_EQ(treatment_stats.collapse_eligible, 2);
  EXPECT_EQ(treatment_stats.collapse_attempted, 1);
  EXPECT_EQ(treatment_stats.collapse_succeeded, 1);

  DeleteVector(p1);
  DeleteVector(p2);
}

// Tests that, due to increased collapse latency, we exponentially backoff from
// collapse.
TEST_P(FillerTest, BackoffFromCollapse) {
  const Length kAlloc = kPagesPerHugePage / 2;

  constexpr size_t kMaxBackoffDelay =
      HugePageFiller<PageTracker>::kMaxBackoffDelay;
  bool expect_collapse = true;
  int expected_collapse_count = 0;
  int backoff_count = 0;
  int max_backoff = 1;
  int expected_collapse_intervals_skipped = 0;
  absl::Duration total_latency = absl::ZeroDuration();
  for (absl::Duration latency : {absl::Seconds(1), absl::Milliseconds(1)}) {
    collapse_.SetLatency(latency);
    bool backoff_increase = latency == absl::Seconds(1);
    while (true) {
      std::vector<PAlloc> p1 = AllocateVector(kAlloc - Length(1));
      ASSERT_TRUE(!p1.empty());

      FakePageFlags pageflags;
      FakeResidency residency;
      for (const auto& pa : p1) {
        pageflags.MarkHugePageBacked(pa.p.start_addr(),
                                     /*is_hugepage_backed=*/false);
        EXPECT_FALSE(pageflags.IsHugepageBacked(pa.p.start_addr()));

        Bitmap<kMaxResidencyBits> unbacked, swapped;
        unbacked.SetRange(/*index=*/0, /*n=*/1);
        swapped.SetRange(/*index=*/0, /*n=*/1);
        residency.SetUnbackedAndSwappedBitmaps(pa.p.start_addr(), unbacked,
                                               swapped);
      }

      ASSERT_EQ(filler_.size(), NHugePages(1));

      TreatHugepageTrackers(/*enable_collapse=*/true,
                            /*enable_release_free_swapped=*/false,
                            /*use_userspace_collapse_heuristics=*/false,
                            &pageflags, &residency);
      // We exponentially backoff from collapse, depending on the latency.
      // If the latency is high, increase max_backoff periodically. In case it
      // is low, periodically decrease max_backoff.
      ++backoff_count;
      expect_collapse = backoff_count >= max_backoff;
      if (expect_collapse) {
        ++expected_collapse_count;
        max_backoff = backoff_increase
                          ? std::min<size_t>(max_backoff * 2, kMaxBackoffDelay)
                          : std::max<size_t>(max_backoff / 2, 1);
        backoff_count = 0;
      } else {
        ++expected_collapse_intervals_skipped;
      }

      for (const auto& pa : p1) {
        EXPECT_EQ(collapse_.TriedCollapse(pa.p.start_addr()), expect_collapse);
        EXPECT_EQ(collapse_.TimesCollapsed(pa.p.start_addr()),
                  expect_collapse ? 1 : 0);
      }
      HugePageTreatmentStats treatment_stats = GetHugePageTreatmentStats();
      EXPECT_EQ(treatment_stats.collapse_eligible, expected_collapse_count);
      EXPECT_EQ(treatment_stats.collapse_attempted, expected_collapse_count);
      EXPECT_EQ(treatment_stats.collapse_succeeded, expected_collapse_count);
      EXPECT_EQ(treatment_stats.collapse_intervals_skipped,
                expected_collapse_intervals_skipped);
      total_latency += expect_collapse ? latency : absl::ZeroDuration();
      EXPECT_EQ(treatment_stats.collapse_time_total_cycles,
                absl::ToDoubleSeconds(total_latency) * FakeClock::freq());
      EXPECT_EQ(treatment_stats.collapse_time_max_cycles,
                absl::ToDoubleSeconds(absl::Seconds(1)) * FakeClock::freq());

      DeleteVector(p1);
      if (backoff_increase && max_backoff >= kMaxBackoffDelay) break;
      if (!backoff_increase && max_backoff <= 1) break;
    }
  }
}

TEST_P(FillerTest, BackoffOnENoMem) {
  if (!use_preferential_collapse_) {
    GTEST_SKIP() << "The test checks behavior of preferential collapse. Skip "
                    "it when it is not enabled.";
  }
  const Length kAlloc = kPagesPerHugePage / 2;

  constexpr size_t kMaxBackoffDelay =
      HugePageFiller<PageTracker>::kMaxBackoffDelay;
  bool expect_collapse_attempted = true;
  int expected_collapse_count = 0;
  int successful_collapse_count = 0;
  int backoff_count = 0;
  int max_backoff = 1;
  int expected_collapse_intervals_skipped = 0;

  for (bool success : {false, true}) {
    collapse_.SetSuccess(success);
    if (!success) {
      collapse_.SetErrorNumber(ENOMEM);
    } else {
      collapse_.SetErrorNumber(0);
    }
    bool backoff_increase = !success;
    while (true) {
      std::vector<PAlloc> p1 = AllocateVector(kAlloc - Length(1));
      ASSERT_TRUE(!p1.empty());

      FakePageFlags pageflags;
      FakeResidency residency;
      for (const auto& pa : p1) {
        pageflags.MarkHugePageBacked(pa.p.start_addr(),
                                     /*is_hugepage_backed=*/false);
        EXPECT_FALSE(pageflags.IsHugepageBacked(pa.p.start_addr()));

        Bitmap<kMaxResidencyBits> unbacked, swapped;
        unbacked.SetRange(/*index=*/0, /*n=*/1);
        swapped.SetRange(/*index=*/0, /*n=*/1);
        residency.SetUnbackedAndSwappedBitmaps(pa.p.start_addr(), unbacked,
                                               swapped);
      }

      ASSERT_EQ(filler_.size(), NHugePages(1));

      TreatHugepageTrackers(/*enable_collapse=*/true,
                            /*enable_release_free_swapped=*/false,
                            /*use_userspace_collapse_heuristics=*/false,
                            &pageflags, &residency);
      // We exponentially backoff from collapse, depending on the collapse
      // error.
      ++backoff_count;
      expect_collapse_attempted = backoff_count >= max_backoff;
      if (expect_collapse_attempted) {
        ++expected_collapse_count;
        max_backoff = backoff_increase
                          ? std::min<size_t>(max_backoff * 16, kMaxBackoffDelay)
                          : std::max<size_t>(max_backoff / 2, 1);
        backoff_count = 0;
        successful_collapse_count += success ? 1 : 0;
      } else {
        ++expected_collapse_intervals_skipped;
      }

      for (const auto& pa : p1) {
        EXPECT_EQ(collapse_.TriedCollapse(pa.p.start_addr()),
                  expect_collapse_attempted);
        EXPECT_EQ(collapse_.TimesCollapsed(pa.p.start_addr()),
                  expect_collapse_attempted ? 1 : 0);
      }
      HugePageTreatmentStats treatment_stats = GetHugePageTreatmentStats();
      EXPECT_EQ(treatment_stats.collapse_eligible, expected_collapse_count);
      EXPECT_EQ(treatment_stats.collapse_attempted, expected_collapse_count);
      EXPECT_EQ(treatment_stats.collapse_succeeded, successful_collapse_count);
      EXPECT_EQ(treatment_stats.collapse_intervals_skipped,
                expected_collapse_intervals_skipped);
      DeleteVector(p1);
      if (backoff_increase && max_backoff >= kMaxBackoffDelay) break;
      if (!backoff_increase && max_backoff <= 1) break;
    }
  }
}

// Don't collapse pages that are released.
TEST_P(FillerTest, DontCollapseReleasedPages) {
  const Length kAlloc = kPagesPerHugePage / 2;
  std::vector<PAlloc> p1 = AllocateVector(kAlloc);
  std::vector<PAlloc> p2 =
      AllocateVectorWithSpanAllocInfo(kAlloc, p1.front().span_alloc_info);
  ASSERT_TRUE(!p1.empty());
  ASSERT_TRUE(!p2.empty());

  FakePageFlags pageflags;
  FakeResidency residency;
  for (const auto& pa : p1) {
    pageflags.MarkHugePageBacked(pa.p.start_addr(),
                                 /*is_hugepage_backed=*/false);
    EXPECT_FALSE(pageflags.IsHugepageBacked(pa.p.start_addr()));

    Bitmap<kMaxResidencyBits> unbacked, swapped;
    unbacked.SetRange(/*index=*/0, kMaxResidencyBits / 2);
    swapped.SetRange(/*index=*/0, kMaxResidencyBits / 2);
    residency.SetUnbackedAndSwappedBitmaps(pa.p.start_addr(), unbacked,
                                           swapped);
  }

  DeleteVector(p2);
  EXPECT_EQ(ReleasePartialPages(kAlloc), kAlloc);
  ASSERT_EQ(filler_.unmapped_pages(), kAlloc);
  ASSERT_TRUE(AllReleased(p2));

  ASSERT_EQ(filler_.size(), NHugePages(1));
  TreatHugepageTrackers(/*enable_collapse=*/true,
                        /*enable_release_free_swapped=*/false,
                        /*use_userspace_collapse_heuristics=*/false, &pageflags,
                        &residency);

  HugePageTreatmentStats treatment_stats = GetHugePageTreatmentStats();
  EXPECT_EQ(treatment_stats.collapse_eligible, 0);
  EXPECT_EQ(treatment_stats.collapse_attempted, 0);
  EXPECT_EQ(treatment_stats.collapse_succeeded, 0);
  EXPECT_EQ(treatment_stats.collapse_time_total_cycles, 0);
  EXPECT_EQ(treatment_stats.collapse_time_max_cycles, 0);
  for (const auto& pa : p1) {
    EXPECT_FALSE(collapse_.TriedCollapse(pa.p.start_addr()));
  }

  DeleteVector(p1);
}

TEST_P(FillerTest, CollapseFailure) {
  const Length kAlloc = kPagesPerHugePage / 2;
  std::vector<PAlloc> p1 = AllocateVector(kAlloc - Length(1));
  ASSERT_TRUE(!p1.empty());
  collapse_.SetSuccess(/*success=*/false);
  collapse_.SetErrorNumber(EINVAL);

  auto check_stats = [&](int expected_eligible, int expected_attempted,
                         int expected_succeeded, CollapseErrorType error_type,
                         int error_count) {
    HugePageTreatmentStats treatment_stats = GetHugePageTreatmentStats();
    EXPECT_EQ(treatment_stats.collapse_eligible, expected_eligible);
    EXPECT_EQ(treatment_stats.collapse_attempted, expected_attempted);
    EXPECT_EQ(treatment_stats.collapse_succeeded, expected_succeeded);
    EXPECT_EQ(treatment_stats.collapse_errors[static_cast<int>(error_type)],
              error_count);
    EXPECT_EQ(treatment_stats.collapse_time_total_cycles, 0);
    EXPECT_EQ(treatment_stats.collapse_time_max_cycles, 0);
  };

  FakePageFlags pageflags;
  FakeResidency residency;
  for (const auto& pa : p1) {
    pageflags.MarkHugePageBacked(pa.p.start_addr(),
                                 /*is_hugepage_backed=*/false);
    EXPECT_FALSE(pageflags.IsHugepageBacked(pa.p.start_addr()));

    Bitmap<kMaxResidencyBits> unbacked, swapped;
    unbacked.SetRange(/*index=*/0, 1);
    swapped.SetRange(/*index=*/0, 1);
    residency.SetUnbackedAndSwappedBitmaps(pa.p.start_addr(), unbacked,
                                           swapped);
  }
  ASSERT_EQ(filler_.size(), NHugePages(1));
  TreatHugepageTrackers(/*enable_collapse=*/true,
                        /*enable_release_free_swapped=*/false,
                        /*use_userspace_collapse_heuristics=*/false, &pageflags,
                        &residency);
  for (const auto& pa : p1) {
    EXPECT_TRUE(collapse_.TriedCollapse(pa.p.start_addr()));
    EXPECT_EQ(collapse_.TimesCollapsed(pa.p.start_addr()), 1);
  }

  check_stats(/*expected_eligible=*/1, /*expected_attempted=*/1,
              /*expected_succeeded=*/0, CollapseErrorType::kEInval,
              /*error_count=*/1);

  FakeClock::Advance(absl::Minutes(10));
  collapse_.SetErrorNumber(EBUSY);
  TreatHugepageTrackers(/*enable_collapse=*/true,
                        /*enable_release_free_swapped=*/false,
                        /*use_userspace_collapse_heuristics=*/false, &pageflags,
                        &residency);
  check_stats(/*expected_eligible=*/2, /*expected_attempted=*/2,
              /*expected_succeeded=*/0, CollapseErrorType::kEBusy,
              /*error_count=*/1);

  FakeClock::Advance(absl::Minutes(10));
  collapse_.SetErrorNumber(EAGAIN);
  TreatHugepageTrackers(/*enable_collapse=*/true,
                        /*enable_release_free_swapped=*/false,
                        /*use_userspace_collapse_heuristics=*/false, &pageflags,
                        &residency);
  check_stats(/*expected_eligible=*/3, /*expected_attempted=*/3,
              /*expected_succeeded=*/0, CollapseErrorType::kEAgain,
              /*error_count=*/1);

  FakeClock::Advance(absl::Minutes(10));
  collapse_.SetErrorNumber(0);
  TreatHugepageTrackers(/*enable_collapse=*/true,
                        /*enable_release_free_swapped=*/false,
                        /*use_userspace_collapse_heuristics=*/false, &pageflags,
                        &residency);
  check_stats(/*expected_eligible=*/4, /*expected_attempted=*/4,
              /*expected_succeeded=*/0, CollapseErrorType::kOther,
              /*error_count=*/1);

  FakeClock::Advance(absl::Minutes(10));
  collapse_.SetErrorNumber(ENOMEM);
  TreatHugepageTrackers(/*enable_collapse=*/true,
                        /*enable_release_free_swapped=*/false,
                        /*use_userspace_collapse_heuristics=*/false, &pageflags,
                        &residency);
  check_stats(/*expected_eligible=*/5, /*expected_attempted=*/5,
              /*expected_succeeded=*/0, CollapseErrorType::kENoMem,
              /*error_count=*/1);

  DeleteVector(p1);
}

// Tests that the pages are revisited periodically (after the clock threshold
// expires), if the collapse wasn't successful before.
TEST_P(FillerTest, CollapseClock) {
  const Length kAlloc = kPagesPerHugePage / 2;
  std::vector<PAlloc> p1 = AllocateVector(kAlloc - Length(1));
  ASSERT_TRUE(!p1.empty());
  // Sets up collapse for failure. This is because we want to test the case
  // where we want to try collapse multiple times after the clock expires.
  collapse_.SetSuccess(/*success=*/false);

  auto check_stats = [&](int expected_eligible, int expected_attempted,
                         int expected_succeeded) {
    HugePageTreatmentStats treatment_stats = GetHugePageTreatmentStats();
    EXPECT_EQ(treatment_stats.collapse_eligible, expected_eligible);
    EXPECT_EQ(treatment_stats.collapse_attempted, expected_attempted);
    EXPECT_EQ(treatment_stats.collapse_succeeded, expected_succeeded);
  };

  FakePageFlags pageflags;
  FakeResidency residency;
  for (const auto& pa : p1) {
    pageflags.MarkHugePageBacked(pa.p.start_addr(),
                                 /*is_hugepage_backed=*/false);
    EXPECT_FALSE(pageflags.IsHugepageBacked(pa.p.start_addr()));

    Bitmap<kMaxResidencyBits> unbacked, swapped;
    unbacked.SetRange(/*index=*/0, 1);
    swapped.SetRange(/*index=*/0, 1);
    residency.SetUnbackedAndSwappedBitmaps(pa.p.start_addr(), unbacked,
                                           swapped);
  }
  ASSERT_EQ(filler_.size(), NHugePages(1));
  TreatHugepageTrackers(/*enable_collapse=*/true,
                        /*enable_release_free_swapped=*/false,
                        /*use_userspace_collapse_heuristics=*/false, &pageflags,
                        &residency);
  for (const auto& pa : p1) {
    EXPECT_TRUE(collapse_.TriedCollapse(pa.p.start_addr()));
    EXPECT_EQ(collapse_.TimesCollapsed(pa.p.start_addr()), 1);
  }
  check_stats(/*expected_eligible=*/1, /*expected_attempted=*/1,
              /*expected_succeeded=*/0);

  FakeClock::Advance(absl::Seconds(1));
  for (const auto& pa : p1) {
    pageflags.MarkHugePageBacked(pa.p.start_addr(),
                                 /*is_hugepage_backed=*/false);
    EXPECT_FALSE(pageflags.IsHugepageBacked(pa.p.start_addr()));
  }

  TreatHugepageTrackers(/*enable_collapse=*/true,
                        /*enable_release_free_swapped=*/false,
                        /*use_userspace_collapse_heuristics=*/false, &pageflags,
                        &residency);
  for (const auto& pa : p1) {
    EXPECT_TRUE(collapse_.TriedCollapse(pa.p.start_addr()));
    EXPECT_EQ(collapse_.TimesCollapsed(pa.p.start_addr()), 1);
  }
  check_stats(/*expected_eligible=*/1, /*expected_attempted=*/1,
              /*expected_succeeded=*/0);

  FakeClock::Advance(absl::Minutes(10));
  TreatHugepageTrackers(/*enable_collapse=*/true,
                        /*enable_release_free_swapped=*/false,
                        /*use_userspace_collapse_heuristics=*/false, &pageflags,
                        &residency);
  for (const auto& pa : p1) {
    EXPECT_TRUE(collapse_.TriedCollapse(pa.p.start_addr()));
    EXPECT_EQ(collapse_.TimesCollapsed(pa.p.start_addr()), 2);
  }
  check_stats(/*expected_eligible=*/2, /*expected_attempted=*/2,
              /*expected_succeeded=*/0);

  DeleteVector(p1);
}

// This test makes sure that we continue releasing from regular (non-partial)
// allocs when we enable a feature to release all free pages from partial
// allocs.
TEST_P(FillerTest, ReleaseFromFullAllocs) {
  const Length kAlloc = kPagesPerHugePage / 2;
  // Maintain the object count for the second allocation so that the alloc
  // list remains the same for the two allocations.
  std::vector<PAlloc> p1 = AllocateVector(kAlloc - Length(1));
  ASSERT_TRUE(!p1.empty());
  std::vector<PAlloc> p2 = AllocateVectorWithSpanAllocInfo(
      kAlloc + Length(1), p1.front().span_alloc_info);

  std::vector<PAlloc> p3 = AllocateVector(kAlloc - Length(2));
  ASSERT_TRUE(!p3.empty());
  std::vector<PAlloc> p4 = AllocateVectorWithSpanAllocInfo(
      kAlloc + Length(2), p3.front().span_alloc_info);
  // We have two hugepages, both full: nothing to release.
  ASSERT_EQ(ReleasePartialPages(kMaxValidPages), Length(0));
  DeleteVector(p1);
  DeleteVector(p3);
  // Now we should see the p1 hugepage - emptier - released.
  ASSERT_EQ(ReleasePartialPages(kAlloc - Length(1)), kAlloc - Length(1));
  EXPECT_EQ(filler_.unmapped_pages(), kAlloc - Length(1));
  ASSERT_TRUE(AllReleased(p1));
  ASSERT_FALSE(AllReleased(p3));
  for (const auto& pa : p3) {
    ASSERT_FALSE(pa.from_released);
  }

  // Check subrelease stats.
  SubreleaseStats subrelease = filler_.subrelease_stats();
  EXPECT_EQ(subrelease.num_pages_subreleased, kAlloc - Length(1));
  EXPECT_EQ(subrelease.num_partial_alloc_pages_subreleased, Length(0));

  // We expect to reuse both p1.pt and p3.pt.
  std::vector<PAlloc> p5 = AllocateVectorWithSpanAllocInfo(
      kAlloc - Length(1), p1.front().span_alloc_info);
  for (const auto& pa : p5) {
    ASSERT_TRUE(pa.pt == p1.front().pt || pa.pt == p3.front().pt);
  }

  DeleteVector(p2);
  DeleteVector(p4);
  ASSERT_TRUE(DeleteVector(p5));
}

// This test makes sure that we release all the free pages from partial allocs
// even when we request fewer pages to release. It also confirms that we
// continue to release desired number of pages from the full allocs even when
// release_partial_alloc_pages option is enabled.
TEST_P(FillerTest, ReleaseFreePagesInPartialAllocs) {
  static const Length kAlloc = kPagesPerHugePage / 2;
  static const Length kL1 = kAlloc - Length(1);
  static const Length kL2 = kAlloc + Length(1);

  static const Length kL3 = kAlloc - Length(1);
  static const Length kL4 = kAlloc + Length(1);
  std::vector<PAlloc> p1 = AllocateVector(kL1);
  ASSERT_TRUE(!p1.empty());
  std::vector<PAlloc> p2 =
      AllocateVectorWithSpanAllocInfo(kL2, p1.back().span_alloc_info);
  std::vector<PAlloc> p3 = AllocateVector(kL3);
  ASSERT_TRUE(!p3.empty());
  std::vector<PAlloc> p4 =
      AllocateVectorWithSpanAllocInfo(kL4, p3.back().span_alloc_info);

  // As there are no free pages, we shouldn't be able to release anything.
  EXPECT_EQ(ReleasePartialPages(kMaxValidPages), Length(0));

  DeleteVector(p2);
  DeleteVector(p4);

  // Check subrelease stats.
  SubreleaseStats subrelease = filler_.subrelease_stats();
  EXPECT_EQ(subrelease.num_pages_subreleased, Length(0));
  EXPECT_EQ(subrelease.num_partial_alloc_pages_subreleased, Length(0));

  // As we do not have any pages in partially-released lists, we should
  // continue to release the requested number of pages.
  EXPECT_EQ(filler_.used_pages_in_partial_released(), Length(0));
  EXPECT_EQ(ReleasePartialPages(kL2), kL2);
  EXPECT_EQ(ReleasePartialPages(kL4), kL4);

  // Check subrelease stats.
  subrelease = filler_.subrelease_stats();
  EXPECT_EQ(subrelease.num_pages_subreleased, kL2 + kL4);
  EXPECT_EQ(subrelease.num_partial_alloc_pages_subreleased, Length(0));
  // Now we allocate more.
  static const Length kL5 = kL2 - Length(2);
  static const Length kL6 = kL4 - Length(2);
  std::vector<PAlloc> p5 =
      AllocateVectorWithSpanAllocInfo(kL5, p1.back().span_alloc_info);
  std::vector<PAlloc> p6 =
      AllocateVectorWithSpanAllocInfo(kL6, p3.back().span_alloc_info);
  // When the two hugepages have different densities or both them are sparse,
  // p5 and p6 go to different hugepages.
  const bool p5_and_p6_on_different_hugepages =
      (p1.back().span_alloc_info.density != p3.back().span_alloc_info.density ||
       p3.back().span_alloc_info.density == AccessDensityPrediction::kSparse);
  if (p5_and_p6_on_different_hugepages) {
    EXPECT_EQ(filler_.used_pages_in_released(), kL1 + kL3 + kL5 + kL6);
    EXPECT_EQ(filler_.used_pages_in_partial_released(), Length(0));
  } else {
    EXPECT_EQ(filler_.used_pages_in_released(), kL3 + kL6 - Length(2));
    EXPECT_EQ(filler_.used_pages_in_partial_released(), Length(0));
  }

  DeleteVector(p5);
  DeleteVector(p6);

  if (p5_and_p6_on_different_hugepages) {
    // We have some free pages in partially-released allocs now.
    EXPECT_EQ(filler_.used_pages_in_partial_released(), kL1 + kL3);
    // Because we gradually release free pages from partially-released allocs,
    // we shouldn't be able to release all the k5+k6 free pages at once.
    EXPECT_EQ(ReleasePartialPages(kL5), kL5);
    EXPECT_EQ(ReleasePartialPages(kL6), kL6);

    // Check subrelease stats.
    subrelease = filler_.subrelease_stats();
    EXPECT_EQ(subrelease.num_pages_subreleased, kL5 + kL6);
    EXPECT_EQ(subrelease.num_partial_alloc_pages_subreleased, kL5 + kL6);
  } else {
    // We have some free pages in partially-released allocs now.
    EXPECT_EQ(filler_.used_pages_in_partial_released(), kL1);
    // We should be able to release all the k5+k6 free pages at once.
    EXPECT_EQ(ReleasePartialPages(kL5), kL5 + kL6);
    EXPECT_EQ(ReleasePartialPages(kL6), Length(0));

    // Check subrelease stats.
    subrelease = filler_.subrelease_stats();
    EXPECT_EQ(subrelease.num_pages_subreleased, kL5 + kL6);
    EXPECT_EQ(subrelease.num_partial_alloc_pages_subreleased, kL6 - Length(2));
  }

  DeleteVector(p1);
  DeleteVector(p3);
}

TEST_P(FillerTest, ReleaseFreePagesInPartialAllocs_SpansAllocated) {
  randomize_density_ = false;
  SpanAllocInfo info = {kPagesPerHugePage.raw_num(),
                        AccessDensityPrediction::kDense};
  static const Length kAlloc = kPagesPerHugePage / 2;
  static const Length kL1 = kAlloc - Length(1);
  static const Length kL2 = kAlloc + Length(1);

  static const Length kL3 = kAlloc - Length(1);
  static const Length kL4 = kAlloc + Length(1);
  std::vector<PAlloc> p1 = AllocateVectorWithSpanAllocInfo(kL1, info);
  ASSERT_TRUE(!p1.empty());
  std::vector<PAlloc> p2 =
      AllocateVectorWithSpanAllocInfo(kL2, p1.front().span_alloc_info);
  std::vector<PAlloc> p3 = AllocateVectorWithSpanAllocInfo(kL3, info);
  ASSERT_TRUE(!p3.empty());
  std::vector<PAlloc> p4 =
      AllocateVectorWithSpanAllocInfo(kL4, p3.front().span_alloc_info);

  // As there are no free pages, we shouldn't be able to release anything.
  EXPECT_EQ(ReleasePartialPages(kMaxValidPages), Length(0));

  DeleteVector(p2);
  DeleteVector(p4);

  // Check subrelease stats.
  SubreleaseStats subrelease = filler_.subrelease_stats();
  EXPECT_EQ(subrelease.num_pages_subreleased, Length(0));
  EXPECT_EQ(subrelease.num_partial_alloc_pages_subreleased, Length(0));

  // As we do not have any pages in partially-released lists, we should
  // continue to release the requested number of pages.
  EXPECT_EQ(filler_.used_pages_in_partial_released(), Length(0));
  EXPECT_EQ(ReleasePartialPages(kL2), kL2);
  EXPECT_EQ(ReleasePartialPages(kL4), kL4);

  // Check subrelease stats.
  subrelease = filler_.subrelease_stats();
  EXPECT_EQ(subrelease.num_pages_subreleased, kL2 + kL4);
  EXPECT_EQ(subrelease.num_partial_alloc_pages_subreleased, Length(0));
  // Now we allocate more.
  static const Length kL5 = kL2 - Length(2);
  static const Length kL6 = kL4 - Length(2);
  std::vector<PAlloc> p5 =
      AllocateVectorWithSpanAllocInfo(kL5, p1.front().span_alloc_info);
  std::vector<PAlloc> p6 =
      AllocateVectorWithSpanAllocInfo(kL6, p3.front().span_alloc_info);
  EXPECT_EQ(filler_.used_pages_in_released(), kL3 + kL6 - Length(2));
  EXPECT_EQ(filler_.used_pages_in_partial_released(), Length(0));

  DeleteVector(p5);
  DeleteVector(p6);

  // We have some free pages in partially-released allocs now.
  EXPECT_EQ(filler_.used_pages_in_partial_released(), kL3);
  // Because we gradually release free pages from partially-released allocs,
  // we should be able to release all the k5+k6 free pages when the dense
  // tracker is sorted on spans allocated.
  static const Length kLReleased5 = ReleasePartialPages(kL5);
  static const Length kLReleased6 = ReleasePartialPages(kL6);
  EXPECT_TRUE(kLReleased5 == kL5 + kL6 && kLReleased6 == Length(0));

  // Check subrelease stats.
  subrelease = filler_.subrelease_stats();
  EXPECT_EQ(subrelease.num_pages_subreleased, kL5 + kL6);
  EXPECT_EQ(subrelease.num_partial_alloc_pages_subreleased, kL6 - Length(2));

  DeleteVector(p1);
  DeleteVector(p3);
}

TEST_P(FillerTest, AccountingForUsedPartialReleased) {
  static const Length kAlloc = kPagesPerHugePage / 2;
  static const Length kL1 = kAlloc + Length(3);
  static const Length kL2 = kAlloc + Length(5);
  std::vector<PAlloc> p1 = AllocateVector(kL1);
  ASSERT_TRUE(!p1.empty());
  std::vector<PAlloc> p2 = AllocateVector(kL2);
  ASSERT_TRUE(!p2.empty());
  // We have two hugepages.  They maybe both partially allocated, or one of
  // them is fully allocated and the other partially when the hugepages in the
  // dense tracker are sorted on spans allocated.
  ASSERT_EQ(ReleasePages(kMaxValidPages),
            kPagesPerHugePage - kL1 + kPagesPerHugePage - kL2);
  ASSERT_TRUE(filler_.used_pages_in_released() == kL1 + kL2 ||
              // When the hugepages in the dense tracker are sorted on spans
              // and the two allocations above are both for dense spans.
              filler_.used_pages_in_released() ==
                  kL1 + kL2 - kPagesPerHugePage);
  // Now we allocate more.
  static const Length kL3 = kAlloc - Length(4);
  static const Length kL4 = kAlloc - Length(7);
  // Maintain the object count as above so that same alloc lists are used for
  // the following two allocations.
  std::vector<PAlloc> p3 =
      AllocateVectorWithSpanAllocInfo(kL3, p1.front().span_alloc_info);
  std::vector<PAlloc> p4 =
      AllocateVectorWithSpanAllocInfo(kL4, p2.front().span_alloc_info);
  EXPECT_TRUE(filler_.used_pages_in_released() == kL1 + kL2 + kL3 + kL4 ||
              filler_.used_pages_in_released() ==
                  kL1 + kL2 + kL3 + kL4 - kPagesPerHugePage);
  DeleteVector(p3);
  DeleteVector(p4);
  EXPECT_TRUE(filler_.used_pages_in_partial_released() == kL1 + kL2 ||
              // When the hugepages in the dense tracker are sorted on spans
              // and the two allocations above are both for dense spans.
              filler_.used_pages_in_partial_released() ==
                  kL1 + kL2 - kPagesPerHugePage);
  EXPECT_EQ(filler_.used_pages_in_released(), Length(0));
  DeleteVector(p1);
  DeleteVector(p2);
}

TEST_P(FillerTest, Release) {
  static const Length kAlloc = kPagesPerHugePage / 2;
  // Maintain the object count for the second allocation so that the alloc
  // list
  // remains the same for the two allocations.
  std::vector<PAlloc> p1 = AllocateVector(kAlloc - Length(1));
  ASSERT_TRUE(!p1.empty());
  std::vector<PAlloc> p2 = AllocateVectorWithSpanAllocInfo(
      kAlloc + Length(1), p1.front().span_alloc_info);

  std::vector<PAlloc> p3 = AllocateVector(kAlloc - Length(2));
  ASSERT_TRUE(!p3.empty());
  std::vector<PAlloc> p4 = AllocateVectorWithSpanAllocInfo(
      kAlloc + Length(2), p3.front().span_alloc_info);
  // We have two hugepages, both full: nothing to release.
  ASSERT_EQ(ReleasePages(kMaxValidPages), Length(0));
  DeleteVector(p1);
  DeleteVector(p3);
  // Now we should see the p1 hugepage - emptier - released.
  ASSERT_EQ(ReleasePages(kAlloc - Length(1)), kAlloc - Length(1));
  EXPECT_EQ(filler_.unmapped_pages(), kAlloc - Length(1));
  EXPECT_EQ(filler_.previously_released_huge_pages(), NHugePages(0));
  ASSERT_TRUE(AllReleased(p1));
  for (const auto& pa : p1) {
    ASSERT_FALSE(pa.from_released);
  }
  ASSERT_FALSE(AllReleased(p3));
  for (const auto& pa : p3) {
    ASSERT_FALSE(pa.from_released);
  }

  // We expect to reuse p1.pt.
  std::vector<PAlloc> p5 = AllocateVectorWithSpanAllocInfo(
      kAlloc - Length(1), p1.front().span_alloc_info);
  ASSERT_TRUE(p1.front().pt == p5.front().pt || p3.front().pt == p5.front().pt);

  DeleteVector(p2);
  DeleteVector(p4);
  DeleteVector(p5);
  EXPECT_EQ(filler_.previously_released_huge_pages(), NHugePages(0));
}

TEST_P(FillerTest, ReleaseZero) {
  // Trying to release no pages should not crash.
  EXPECT_EQ(
      ReleasePages(Length(0),
                   SkipSubreleaseIntervals{.peak_interval = absl::Seconds(1)}),
      Length(0));
}

void FillerTest::FragmentationTest() {
  constexpr Length kRequestLimit = Length(32);
  constexpr Length kSizeLimit = Length(512 * 1024);
  constexpr size_t kReps = 1000;

  absl::BitGen rng;
  auto dist = EmpiricalDistribution(kRequestLimit);

  std::vector<std::vector<PAlloc>> allocs;
  std::vector<Length> lengths;
  Length total;
  while (total < kSizeLimit) {
    auto n = Length(dist(rng));
    total += n;
    allocs.push_back(AllocateVector(n));
    lengths.push_back(n);
  }

  double max_slack = 0.0;
  for (size_t i = 0; i < kReps; ++i) {
    auto stats = filler_.stats();
    double slack = static_cast<double>(stats.free_bytes) / stats.system_bytes;

    max_slack = std::max(slack, max_slack);
    if (i % (kReps / 40) == 0) {
      printf("%zu events: %zu allocs totalling %zu slack %f\n", i,
             allocs.size(), total.raw_num(), slack);
    }
    if (absl::Bernoulli(rng, 1.0 / 2)) {
      size_t index = absl::Uniform<int32_t>(rng, 0, allocs.size());
      std::swap(allocs[index], allocs.back());
      std::swap(lengths[index], lengths.back());
      DeleteVector(allocs.back());
      total -= lengths.back();
      allocs.pop_back();
      lengths.pop_back();
    } else {
      auto n = Length(dist(rng));
      allocs.push_back(AllocateVector(n));
      lengths.push_back(n);
      total += n;
    }
  }

  EXPECT_LE(max_slack, 0.06);

  for (auto a : allocs) {
    DeleteVector(a);
  }
}

TEST_P(FillerTest, Fragmentation) { FragmentationTest(); }

TEST_P(FillerTest, PrintFreeRatio) {
  // This test is sensitive to the number of pages per hugepage, as we are
  // printing raw stats.
  if (kPagesPerHugePage != Length(256)) {
    GTEST_SKIP();
  }

  // We prevent randomly choosing the number of objects per span since this
  // test has hardcoded output which will change if the objects per span are
  // chosen at random.
  randomize_density_ = false;
  FakePageFlags pageflags;

  // Allocate two huge pages, release one, verify that we do not get an
  // invalid
  // (>1.) ratio of free : non-fulls.

  // First huge page
  std::vector<PAlloc> a1 = AllocateVector(kPagesPerHugePage / 2);
  ASSERT_TRUE(!a1.empty());
  std::vector<PAlloc> a2 = AllocateVectorWithSpanAllocInfo(
      kPagesPerHugePage / 2, a1.front().span_alloc_info);

  // Second huge page
  constexpr Length kQ = kPagesPerHugePage / 4;

  std::vector<PAlloc> a3 = AllocateVector(kQ);
  ASSERT_TRUE(!a3.empty());
  std::vector<PAlloc> a4 =
      AllocateVectorWithSpanAllocInfo(kQ, a3.front().span_alloc_info);
  std::vector<PAlloc> a5 =
      AllocateVectorWithSpanAllocInfo(kQ, a3.front().span_alloc_info);
  std::vector<PAlloc> a6 =
      AllocateVectorWithSpanAllocInfo(kQ, a3.front().span_alloc_info);

  DeleteVector(a6);
  ReleasePages(kQ);
  DeleteVector(a5);
  std::string buffer(1024 * 1024, '\0');
  {
    PageHeapSpinLockHolder l;
    Printer printer(&*buffer.begin(), buffer.size());
    filler_.Print(printer, /*everything=*/true, pageflags);
    buffer.erase(printer.SpaceRequired());
  }

  EXPECT_THAT(buffer,
              testing::StartsWith(
                  R"(HugePageFiller: densely pack small requests into hugepages
HugePageFiller: Overall, 2 total, 1 full, 0 partial, 1 released (1 partially), 0 quarantined
HugePageFiller: those with sparsely-accessed spans, 2 total, 1 full, 0 partial, 1 released (1 partially), 0 quarantined
HugePageFiller: those with densely-accessed spans, 0 total, 0 full, 0 partial, 0 released (0 partially), 0 quarantined
HugePageFiller: 64 pages free in 2 hugepages, 0.1250 free
HugePageFiller: among non-fulls, 0.2500 free
HugePageFiller: 128 used pages in subreleased hugepages (128 of them in partially released)
HugePageFiller: 1 hugepages partially released, 0.2500 released
HugePageFiller: 0.6667 of used pages hugepageable)"));

  // Cleanup remaining allocs.
  DeleteVector(a1);
  DeleteVector(a2);
  DeleteVector(a3);
  DeleteVector(a4);
}

static double BytesToMiB(size_t bytes) { return bytes / (1024.0 * 1024.0); }

using testing::AnyOf;
using testing::Eq;
using testing::StrEq;

TEST_P(FillerTest, HugePageFrac) {
  // I don't actually care which we get, both are
  // reasonable choices, but don't report a NaN/complain
  // about divide by 0s/ give some bogus number for empty.
  EXPECT_THAT(filler_.hugepage_frac(), AnyOf(Eq(0), Eq(1)));
  static const Length kQ = kPagesPerHugePage / 4;
  // These are all on one page:
  auto a1 = AllocateVector(kQ);
  ASSERT_TRUE(!a1.empty());
  auto a2 = AllocateVectorWithSpanAllocInfo(kQ, a1.front().span_alloc_info);
  auto a3 = AllocateVectorWithSpanAllocInfo(kQ - Length(1),
                                            a1.front().span_alloc_info);
  auto a4 = AllocateVectorWithSpanAllocInfo(kQ + Length(1),
                                            a1.front().span_alloc_info);

  // As are these:
  auto a5 = AllocateVector(kPagesPerHugePage - kQ);
  ASSERT_TRUE(!a5.empty());
  auto a6 = AllocateVectorWithSpanAllocInfo(kQ, a5.front().span_alloc_info);

  EXPECT_EQ(filler_.hugepage_frac(), 1);
  // Free space doesn't affect it...
  DeleteVector(a4);
  DeleteVector(a6);

  EXPECT_EQ(filler_.hugepage_frac(), 1);

  // Releasing the hugepage does.
  ASSERT_EQ(ReleasePages(kQ + Length(1)), kQ + Length(1));
  EXPECT_EQ(filler_.hugepage_frac(),
            (3.0 * kQ.raw_num()) / (6.0 * kQ.raw_num() - 1.0));

  // Check our arithmetic in a couple scenarios.

  // 2 kQs on the release and 3 on the hugepage
  DeleteVector(a2);
  EXPECT_EQ(filler_.hugepage_frac(),
            (3.0 * kQ.raw_num()) / (5.0 * kQ.raw_num() - 1));
  // This releases the free page on the partially released hugepage.
  ASSERT_EQ(ReleasePages(kQ), kQ);
  EXPECT_EQ(filler_.hugepage_frac(),
            (3.0 * kQ.raw_num()) / (5.0 * kQ.raw_num() - 1));

  // just-over-1 kQ on the release and 3 on the hugepage
  DeleteVector(a3);
  EXPECT_EQ(filler_.hugepage_frac(), (3 * kQ.raw_num()) / (4.0 * kQ.raw_num()));
  // This releases the free page on the partially released hugepage.
  ASSERT_EQ(ReleasePages(kQ - Length(1)), kQ - Length(1));
  EXPECT_EQ(filler_.hugepage_frac(), (3 * kQ.raw_num()) / (4.0 * kQ.raw_num()));

  // All huge!
  DeleteVector(a1);
  EXPECT_EQ(filler_.hugepage_frac(), 1);

  DeleteVector(a5);
}

// Repeatedly grow from FLAG_bytes to FLAG_bytes * growth factor, then shrink
// back down by random deletion. Then release partial hugepages until
// pageheap is bounded by some fraction of usage.
// Measure the effective hugepage fraction at peak and baseline usage,
// and the blowup in VSS footprint.
//
// This test is a tool for analyzing parameters -- not intended as an actual
// unit test.
TEST_P(FillerTest, DISABLED_ReleaseFrac) {
  absl::BitGen rng;
  const Length baseline = LengthFromBytes(absl::GetFlag(FLAGS_bytes));
  const Length peak = baseline * absl::GetFlag(FLAGS_growth_factor);
  const Length free_target = baseline * absl::GetFlag(FLAGS_release_until);

  std::vector<PAlloc> allocs;
  while (filler_.used_pages() < baseline) {
    allocs.push_back(Allocate(Length(1)));
  }

  while (true) {
    while (filler_.used_pages() < peak) {
      allocs.push_back(Allocate(Length(1)));
    }
    const double peak_frac = filler_.hugepage_frac();
    // VSS
    const size_t footprint = filler_.size().in_bytes();

    std::shuffle(allocs.begin(), allocs.end(), rng);

    size_t limit = allocs.size();
    while (filler_.used_pages() > baseline) {
      --limit;
      Delete(allocs[limit]);
    }
    allocs.resize(limit);
    while (filler_.free_pages() > free_target) {
      ReleasePages(kMaxValidPages);
    }
    const double baseline_frac = filler_.hugepage_frac();

    printf("%.3f %.3f %6.1f MiB\n", peak_frac, baseline_frac,
           BytesToMiB(footprint));
  }
}

// Make sure we release appropriate number of pages when using
// ReleasePartialPages.
TEST_P(FillerTest, ReleasePagesFromPartialAllocs) {
  const Length N = kPagesPerHugePage;
  auto big = AllocateVector(N - Length(2));
  ASSERT_TRUE(!big.empty());
  auto tiny1 =
      AllocateWithSpanAllocInfo(Length(1), big.front().span_alloc_info);
  auto tiny2 =
      AllocateWithSpanAllocInfo(Length(1), big.front().span_alloc_info);
  auto half1 = AllocateVector(N / 2);
  ASSERT_TRUE(!half1.empty());
  auto half2 =
      AllocateVectorWithSpanAllocInfo(N / 2, half1.front().span_alloc_info);

  DeleteVector(half1);
  DeleteVector(big);

  ASSERT_EQ(filler_.size(), NHugePages(2));

  // We should pick the [empty big][full tiny] hugepage here.
  EXPECT_EQ(ReleasePartialPages(N - Length(2)), N - Length(2));
  EXPECT_EQ(filler_.unmapped_pages(), N - Length(2));
  // This shouldn't trigger a release.
  Delete(tiny1);
  EXPECT_EQ(filler_.unmapped_pages(), N - Length(2));
  // Until we call ReleasePartialPages() again.
  EXPECT_EQ(ReleasePartialPages(Length(1)), Length(1));

  // As should this, but this will drop the whole hugepage.
  Delete(tiny2);
  EXPECT_EQ(filler_.unmapped_pages(), Length(0));
  EXPECT_EQ(filler_.size(), NHugePages(1));

  // We should release tiny2 here.
  EXPECT_EQ(ReleasePartialPages(Length(1)), Length(1));
  EXPECT_EQ(filler_.unmapped_pages(), Length(0));
  EXPECT_EQ(filler_.size(), NHugePages(1));

  // Check subrelease stats.
  EXPECT_EQ(filler_.used_pages(), N / 2);
  EXPECT_EQ(filler_.used_pages_in_any_subreleased(), Length(0));
  EXPECT_EQ(filler_.used_pages_in_partial_released(), Length(0));
  EXPECT_EQ(filler_.used_pages_in_released(), Length(0));

  // Now we pick the half/half hugepage. We should be able to release pages
  // from full allocs with ReleasePartialPages even though partially-released
  // allocs are empty.
  EXPECT_EQ(ReleasePartialPages(kMaxValidPages), N / 2);
  EXPECT_EQ(filler_.unmapped_pages(), N / 2);

  // Check subrelease stats.
  EXPECT_EQ(filler_.used_pages(), N / 2);
  EXPECT_EQ(filler_.used_pages_in_any_subreleased(), N / 2);
  EXPECT_EQ(filler_.used_pages_in_partial_released(), Length(0));
  EXPECT_EQ(filler_.used_pages_in_released(), N / 2);

  DeleteVector(half2);
  EXPECT_EQ(filler_.size(), NHugePages(0));
  EXPECT_EQ(filler_.unmapped_pages(), Length(0));
}

TEST_P(FillerTest, ReleaseAccounting) {
  const Length N = kPagesPerHugePage;
  auto big = AllocateVector(N - Length(2));
  ASSERT_TRUE(!big.empty());
  auto tiny1 =
      AllocateWithSpanAllocInfo(Length(1), big.front().span_alloc_info);
  auto tiny2 =
      AllocateWithSpanAllocInfo(Length(1), big.front().span_alloc_info);
  auto half1 = AllocateVector(N / 2);
  ASSERT_TRUE(!half1.empty());
  auto half2 =
      AllocateVectorWithSpanAllocInfo(N / 2, half1.front().span_alloc_info);
  ASSERT_TRUE(!half2.empty());

  DeleteVector(half1);
  DeleteVector(big);

  ASSERT_EQ(filler_.size(), NHugePages(2));

  // We should pick the [empty big][full tiny] hugepage here.
  EXPECT_EQ(ReleasePages(N - Length(2)), N - Length(2));
  EXPECT_EQ(filler_.unmapped_pages(), N - Length(2));
  // This shouldn't trigger a release
  Delete(tiny1);
  EXPECT_EQ(filler_.unmapped_pages(), N - Length(2));
  // Until we call ReleasePages()
  EXPECT_EQ(ReleasePages(Length(1)), Length(1));
  EXPECT_EQ(filler_.unmapped_pages(), N - Length(1));

  // As should this, but this will drop the whole hugepage
  Delete(tiny2);
  EXPECT_EQ(filler_.unmapped_pages(), Length(0));
  EXPECT_EQ(filler_.size(), NHugePages(1));

  // This shouldn't trigger any release: we just claim credit for the
  // releases we did automatically on tiny2.
  EXPECT_EQ(ReleasePages(Length(1)), Length(1));
  EXPECT_EQ(filler_.unmapped_pages(), Length(0));
  EXPECT_EQ(filler_.size(), NHugePages(1));

  // Check subrelease stats
  EXPECT_EQ(filler_.used_pages(), N / 2);
  EXPECT_EQ(filler_.used_pages_in_any_subreleased(), Length(0));
  EXPECT_EQ(filler_.used_pages_in_partial_released(), Length(0));
  EXPECT_EQ(filler_.used_pages_in_released(), Length(0));

  // Now we pick the half/half hugepage
  EXPECT_EQ(ReleasePages(kMaxValidPages), N / 2);
  EXPECT_EQ(filler_.unmapped_pages(), N / 2);

  // Check subrelease stats
  EXPECT_EQ(filler_.used_pages(), N / 2);
  EXPECT_EQ(filler_.used_pages_in_any_subreleased(), N / 2);
  EXPECT_EQ(filler_.used_pages_in_partial_released(), Length(0));
  EXPECT_EQ(filler_.used_pages_in_released(), N / 2);

  // Check accounting for partially released hugepages with partial rerelease.
  // Allocating and deallocating a small object causes the page to turn from a
  // released hugepage into a partially released hugepage.
  //
  // The number of objects for each allocation is same as that for half2 so to
  // ensure that same alloc list is used.
  auto tiny3 =
      AllocateWithSpanAllocInfo(Length(1), half2.front().span_alloc_info);
  auto tiny4 =
      AllocateWithSpanAllocInfo(Length(1), half2.front().span_alloc_info);
  Delete(tiny4);
  EXPECT_EQ(filler_.used_pages(), N / 2 + Length(1));
  EXPECT_EQ(filler_.used_pages_in_any_subreleased(), N / 2 + Length(1));
  EXPECT_EQ(filler_.used_pages_in_partial_released(), N / 2 + Length(1));
  EXPECT_EQ(filler_.used_pages_in_released(), Length(0));
  Delete(tiny3);

  DeleteVector(half2);
  EXPECT_EQ(filler_.size(), NHugePages(0));
  EXPECT_EQ(filler_.unmapped_pages(), Length(0));
}

TEST_P(FillerTest, ReleaseWithReuse) {
  const Length N = kPagesPerHugePage;
  auto half = AllocateVector(N / 2);
  ASSERT_TRUE(!half.empty());
  auto tiny1 =
      AllocateVectorWithSpanAllocInfo(N / 4, half.front().span_alloc_info);
  auto tiny2 =
      AllocateVectorWithSpanAllocInfo(N / 4, half.front().span_alloc_info);

  DeleteVector(half);
  ASSERT_EQ(filler_.size(), NHugePages(1));

  // We should be able to release the pages from half1.
  EXPECT_EQ(ReleasePages(kMaxValidPages), N / 2);
  EXPECT_EQ(filler_.unmapped_pages(), N / 2);
  EXPECT_EQ(filler_.previously_released_huge_pages(), NHugePages(0));

  // Release tiny1, release more.
  DeleteVector(tiny1);

  EXPECT_EQ(ReleasePages(kMaxValidPages), N / 4);
  EXPECT_EQ(filler_.unmapped_pages(), 3 * N / 4);
  EXPECT_EQ(filler_.previously_released_huge_pages(), NHugePages(0));

  // Repopulate, confirm we can't release anything and unmapped pages goes to
  // 0.
  tiny1 = AllocateVectorWithSpanAllocInfo(N / 4, half.front().span_alloc_info);
  EXPECT_EQ(Length(0), ReleasePages(kMaxValidPages));
  EXPECT_EQ(N / 2, filler_.unmapped_pages());
  EXPECT_EQ(filler_.previously_released_huge_pages(), NHugePages(0));

  // Continue repopulating.
  half = AllocateVectorWithSpanAllocInfo(N / 2, half.front().span_alloc_info);
  EXPECT_EQ(ReleasePages(kMaxValidPages), Length(0));
  EXPECT_EQ(filler_.unmapped_pages(), Length(0));
  EXPECT_EQ(filler_.size(), NHugePages(1));
  EXPECT_EQ(filler_.previously_released_huge_pages(), NHugePages(1));

  // Release everything and cleanup.
  DeleteVector(half);
  DeleteVector(tiny1);
  DeleteVector(tiny2);
  EXPECT_EQ(filler_.size(), NHugePages(0));
  EXPECT_EQ(filler_.unmapped_pages(), Length(0));
  EXPECT_EQ(filler_.previously_released_huge_pages(), NHugePages(0));
}

TEST_P(FillerTest, CheckPreviouslyReleasedStats) {
  FakePageFlags pageflags;
  const Length N = kPagesPerHugePage;
  auto half = AllocateVector(N / 2);
  ASSERT_TRUE(!half.empty());
  auto tiny1 =
      AllocateVectorWithSpanAllocInfo(N / 4, half.front().span_alloc_info);
  auto tiny2 =
      AllocateVectorWithSpanAllocInfo(N / 4, half.front().span_alloc_info);

  DeleteVector(half);
  ASSERT_EQ(filler_.size(), NHugePages(1));

  // We should be able to release the pages from half1.
  EXPECT_EQ(ReleasePages(kMaxValidPages), N / 2);
  EXPECT_EQ(filler_.previously_released_huge_pages(), NHugePages(0));

  std::string buffer(1024 * 1024, '\0');
  {
    PageHeapSpinLockHolder l;
    Printer printer(&*buffer.begin(), buffer.size());
    filler_.Print(printer, true, pageflags);
  }
  buffer.resize(strlen(buffer.c_str()));
  EXPECT_THAT(buffer, testing::HasSubstr(
                          "HugePageFiller: 0 hugepages became full after "
                          "being previously released, "
                          "out of which 0 pages are hugepage backed."));

  // Repopulate.
  ASSERT_TRUE(!tiny1.empty());
  half = AllocateVectorWithSpanAllocInfo(N / 2, tiny1.front().span_alloc_info);
  EXPECT_EQ(ReleasePages(kMaxValidPages), Length(0));
  EXPECT_EQ(filler_.previously_released_huge_pages(), NHugePages(1));
  buffer.resize(1024 * 1024);
  {
    PageHeapSpinLockHolder l;
    Printer printer(&*buffer.begin(), buffer.size());
    filler_.Print(printer, true, pageflags);
  }

  buffer.resize(strlen(buffer.c_str()));
  EXPECT_THAT(buffer,
              testing::HasSubstr("HugePageFiller: 1 hugepages became full "
                                 "after being previously released, "
                                 "out of which 0 pages are hugepage backed."));

  // Release everything and cleanup.
  DeleteVector(half);
  DeleteVector(tiny1);
  DeleteVector(tiny2);
  EXPECT_EQ(filler_.previously_released_huge_pages(), NHugePages(0));
  buffer.resize(1024 * 1024);
  {
    PageHeapSpinLockHolder l;
    Printer printer(&*buffer.begin(), buffer.size());
    filler_.Print(printer, true, pageflags);
  }
  buffer.resize(strlen(buffer.c_str()));
  EXPECT_THAT(buffer,
              testing::HasSubstr("HugePageFiller: 0 hugepages became full "
                                 "after being previously released, "
                                 "out of which 0 pages are hugepage backed."));
}

// Make sure that previously_released_huge_pages stat is correct when a huge
// page toggles from full -> released -> full -> released.
TEST_P(FillerTest, CheckFullReleasedFullReleasedState) {
  FakePageFlags pageflags;
  const Length N = kPagesPerHugePage;
  auto half = AllocateVector(N / 2);
  ASSERT_TRUE(!half.empty());
  ASSERT_EQ(filler_.size(), NHugePages(1));

  // We should be able to release the N/2 pages that are free.
  EXPECT_EQ(ReleasePages(kMaxValidPages), N / 2);
  EXPECT_EQ(filler_.previously_released_huge_pages(), NHugePages(0));

  std::string buffer(1024 * 1024, '\0');
  {
    PageHeapSpinLockHolder l;
    Printer printer(&*buffer.begin(), buffer.size());
    filler_.Print(printer, true, pageflags);
  }
  buffer.resize(strlen(buffer.c_str()));
  EXPECT_THAT(buffer,
              testing::HasSubstr("HugePageFiller: 0 hugepages became full "
                                 "after being previously released, "
                                 "out of which 0 pages are hugepage backed."));

  // Repopulate.
  auto half1 =
      AllocateVectorWithSpanAllocInfo(N / 2, half.front().span_alloc_info);
  EXPECT_EQ(ReleasePages(kMaxValidPages), Length(0));
  EXPECT_EQ(filler_.previously_released_huge_pages(), NHugePages(1));
  buffer.resize(1024 * 1024);
  {
    PageHeapSpinLockHolder l;
    Printer printer(&*buffer.begin(), buffer.size());
    filler_.Print(printer, true, pageflags);
  }

  buffer.resize(strlen(buffer.c_str()));
  EXPECT_THAT(buffer,
              testing::HasSubstr("HugePageFiller: 1 hugepages became full "
                                 "after being previously released, "
                                 "out of which 0 pages are hugepage backed."));

  // Release again.
  DeleteVector(half1);
  EXPECT_EQ(ReleasePages(kMaxValidPages), N / 2);
  EXPECT_EQ(filler_.previously_released_huge_pages(), NHugePages(0));

  buffer.resize(1024 * 1024);
  {
    PageHeapSpinLockHolder l;
    Printer printer(&*buffer.begin(), buffer.size());
    filler_.Print(printer, true, pageflags);
  }
  buffer.resize(strlen(buffer.c_str()));
  EXPECT_THAT(buffer,

              testing::HasSubstr("HugePageFiller: 0 hugepages became full "
                                 "after being previously released, "
                                 "out of which 0 pages are hugepage backed."));

  // Release everything and cleanup.
  DeleteVector(half);
  EXPECT_EQ(filler_.previously_released_huge_pages(), NHugePages(0));
  buffer.resize(1024 * 1024);
  {
    PageHeapSpinLockHolder l;
    Printer printer(&*buffer.begin(), buffer.size());
    filler_.Print(printer, true, pageflags);
  }
  buffer.resize(strlen(buffer.c_str()));
  EXPECT_THAT(buffer,
              testing::HasSubstr("HugePageFiller: 0 hugepages became full "
                                 "after being previously released, "
                                 "out of which 0 pages are hugepage backed."));
}

TEST_P(FillerTest, AvoidArbitraryQuarantineVMGrowth) {
  const Length N = kPagesPerHugePage;
  // Guarantee we have a ton of released pages go empty.
  for (int i = 0; i < 10 * 1000; ++i) {
    auto half1 = AllocateVector(N / 2);
    auto half2 = AllocateVector(N / 2);
    DeleteVector(half1);
    ASSERT_EQ(ReleasePages(N / 2), N / 2);
    DeleteVector(half2);
  }

  auto s = filler_.stats();
  EXPECT_LE(s.system_bytes, 1024 * 1024 * 1024);
}

TEST_P(FillerTest, StronglyPreferNonDonated) {
  // We donate several huge pages of varying fullnesses. Then we make several
  // allocations that would be perfect fits for the donated hugepages, *after*
  // making one allocation that won't fit, to ensure that a huge page is
  // contributed normally. Finally, we verify that we can still get the
  // donated huge pages back. (I.e. they weren't used.)
  std::vector<std::vector<PAlloc>> donated;
  SpanAllocInfo info = {1, AccessDensityPrediction::kSparse};
  ASSERT_GE(kPagesPerHugePage, Length(10));
  for (auto i = Length(1); i <= Length(3); ++i) {
    donated.push_back(AllocateVectorWithSpanAllocInfo(kPagesPerHugePage - i,
                                                      info,
                                                      /*donated=*/true));
  }

  std::vector<std::vector<PAlloc>> regular;
  // Only sparsely-accessed spans are allocated from donated hugepages.  So
  // create a hugepage with a sparsely-accessed span.  The test should prefer
  // this hugepage for sparsely-accessed spans and should allocate a new
  // hugepage for densely-accessed spans.
  regular.push_back(AllocateVectorWithSpanAllocInfo(Length(4), info));

  for (auto i = Length(3); i >= Length(1); --i) {
    regular.push_back(AllocateVector(i));
  }

  for (const std::vector<PAlloc>& alloc : donated) {
    // All the donated huge pages should be freeable.
    EXPECT_TRUE(DeleteVector(alloc));
  }

  for (const std::vector<PAlloc>& alloc : regular) {
    DeleteVector(alloc);
  }
}

TEST_P(FillerTest, SkipPartialAllocSubrelease) {
  // This test is sensitive to the number of pages per hugepage, as we are
  // printing raw stats.
  if (kPagesPerHugePage != Length(256)) {
    GTEST_SKIP();
  }

  FakePageFlags pageflags;
  // Firstly, this test generates a peak (long-term demand peak) and waits for
  // time interval a. Then, it generates a higher peak plus a short-term
  // fluctuation peak, and waits for time interval b. It then generates a
  // trough in demand and tries to subrelease. Finally, it waits for time
  // interval c to generate the highest peak for evaluating subrelease
  // correctness. Skip subrelease selects those demand points using provided
  // time intervals.
  const auto demand_pattern = [&](absl::Duration a, absl::Duration b,
                                  absl::Duration c,
                                  SkipSubreleaseIntervals intervals,
                                  bool expected_subrelease) {
    const Length N = kPagesPerHugePage;
    SpanAllocInfo info = {1, AccessDensityPrediction::kSparse};

    // First peak: min_demand 3/4N, max_demand 1N.
    PAlloc peak1a = AllocateWithSpanAllocInfo(3 * N / 4, info);
    PAlloc peak1b = AllocateWithSpanAllocInfo(N / 4, peak1a.span_alloc_info);
    FakeClock::Advance(a);
    // Second peak: min_demand 0, max_demand 2N.
    Delete(peak1a);
    Delete(peak1b);

    PAlloc half = AllocateWithSpanAllocInfo(N / 2, info);
    PAlloc tiny1 = AllocateWithSpanAllocInfo(N / 4, half.span_alloc_info);
    PAlloc tiny2 = AllocateWithSpanAllocInfo(N / 4, half.span_alloc_info);

    // To force a peak, we allocate 3/4 and 1/4 of a huge page.  This is
    // necessary after we delete `half` below, as a half huge page for the
    // peak would fill into the gap previously occupied by it.
    PAlloc peak2a = AllocateWithSpanAllocInfo(3 * N / 4, info);
    PAlloc peak2b = AllocateWithSpanAllocInfo(N / 4, peak2a.span_alloc_info);
    EXPECT_EQ(filler_.used_pages(), 2 * N);
    Delete(peak2a);
    Delete(peak2b);
    FakeClock::Advance(b);
    Delete(half);
    EXPECT_EQ(filler_.free_pages(), Length(N / 2));
    // The number of released pages is limited to the number of free
    // pages.
    EXPECT_EQ(expected_subrelease ? N / 2 : Length(0),
              ReleasePartialPages(10 * N, intervals));

    FakeClock::Advance(c);
    // Third peak: min_demand 1/2N, max_demand (2+1/2)N.
    PAlloc peak3a = AllocateWithSpanAllocInfo(3 * N / 4, info);
    PAlloc peak3b = AllocateWithSpanAllocInfo(N / 4, peak3a.span_alloc_info);

    PAlloc peak4a = AllocateWithSpanAllocInfo(3 * N / 4, info);
    PAlloc peak4b = AllocateWithSpanAllocInfo(N / 4, peak4a.span_alloc_info);

    Delete(tiny1);
    Delete(tiny2);
    Delete(peak3a);
    Delete(peak3b);
    Delete(peak4a);
    Delete(peak4b);

    EXPECT_EQ(filler_.used_pages(), Length(0));
    EXPECT_EQ(filler_.unmapped_pages(), Length(0));
    EXPECT_EQ(filler_.free_pages(), Length(0));

    EXPECT_EQ(expected_subrelease ? N / 2 : Length(0),
              ReleasePartialPages(10 * N));
  };

  {
    // Uses peak interval for skipping subrelease. We should correctly skip
    // 128 pages.
    SCOPED_TRACE("demand_pattern 1");
    demand_pattern(absl::Minutes(2), absl::Minutes(1), absl::Minutes(3),
                   SkipSubreleaseIntervals{.peak_interval = absl::Minutes(3)},
                   false);
  }

  FakeClock::Advance(absl::Minutes(30));

  {
    // Repeats the "demand_pattern 1" test with additional short-term and
    // long-term intervals, to show that skip-subrelease prioritizes using
    // peak_interval.
    SCOPED_TRACE("demand_pattern 2");
    demand_pattern(
        absl::Minutes(2), absl::Minutes(1), absl::Minutes(3),
        SkipSubreleaseIntervals{.peak_interval = absl::Minutes(3),
                                .short_interval = absl::Milliseconds(10),
                                .long_interval = absl::Milliseconds(20)},
        false);
  }

  FakeClock::Advance(absl::Minutes(30));

  {
    // Uses peak interval for skipping subrelease, subreleasing all free
    // pages. The short-term interval is not used, as we prioritize using
    // demand peak.
    SCOPED_TRACE("demand_pattern 3");
    demand_pattern(absl::Minutes(6), absl::Minutes(3), absl::Minutes(3),
                   SkipSubreleaseIntervals{.peak_interval = absl::Minutes(2),
                                           .short_interval = absl::Minutes(5)},
                   true);
  }

  FakeClock::Advance(absl::Minutes(30));

  {
    // Skip subrelease feature is disabled if all intervals are zero.
    SCOPED_TRACE("demand_pattern 4");
    demand_pattern(absl::Minutes(1), absl::Minutes(1), absl::Minutes(4),
                   SkipSubreleaseIntervals{}, true);
  }

  FakeClock::Advance(absl::Minutes(30));

  {
    // Uses short-term and long-term intervals for skipping subrelease. It
    // incorrectly skips 128 pages.
    SCOPED_TRACE("demand_pattern 5");
    demand_pattern(absl::Minutes(3), absl::Minutes(2), absl::Minutes(7),
                   SkipSubreleaseIntervals{.short_interval = absl::Minutes(3),
                                           .long_interval = absl::Minutes(6)},
                   false);
  }

  FakeClock::Advance(absl::Minutes(30));

  {
    // Uses short-term and long-term intervals for skipping subrelease,
    // subreleasing all free pages.
    SCOPED_TRACE("demand_pattern 6");
    demand_pattern(absl::Minutes(4), absl::Minutes(2), absl::Minutes(3),
                   SkipSubreleaseIntervals{.short_interval = absl::Minutes(1),
                                           .long_interval = absl::Minutes(2)},
                   true);
  }
  FakeClock::Advance(absl::Minutes(30));

  {
    // Uses only short-term interval for skipping subrelease. It correctly
    // skips 128 pages.
    SCOPED_TRACE("demand_pattern 7");
    demand_pattern(absl::Minutes(4), absl::Minutes(2), absl::Minutes(3),
                   SkipSubreleaseIntervals{.short_interval = absl::Minutes(3)},
                   false);
  }

  FakeClock::Advance(absl::Minutes(30));

  {
    // Uses only long-term interval for skipping subrelease, subreleased all
    // free pages.
    SCOPED_TRACE("demand_pattern 8");
    demand_pattern(absl::Minutes(4), absl::Minutes(2), absl::Minutes(3),
                   SkipSubreleaseIntervals{.long_interval = absl::Minutes(2)},
                   true);
  }

  FakeClock::Advance(absl::Minutes(30));

  // This captures a corner case: If we hit another peak immediately after a
  // subrelease decision (in the same time series epoch), do not count this as
  // a correct subrelease decision.
  {
    SCOPED_TRACE("demand_pattern 9");
    demand_pattern(
        absl::Milliseconds(10), absl::Milliseconds(10), absl::Milliseconds(10),
        SkipSubreleaseIntervals{.peak_interval = absl::Minutes(2)}, false);
  }
  // Repeats the "demand_pattern 9" test using short-term and long-term
  // intervals, to show that subrelease decisions are evaluated independently.
  {
    SCOPED_TRACE("demand_pattern 10");
    demand_pattern(absl::Milliseconds(10), absl::Milliseconds(10),
                   absl::Milliseconds(10),
                   SkipSubreleaseIntervals{.short_interval = absl::Minutes(1),
                                           .long_interval = absl::Minutes(2)},
                   false);
  }

  FakeClock::Advance(absl::Minutes(30));

  // Ensure that the tracker is updated.
  auto tiny = Allocate(Length(1));
  Delete(tiny);

  std::string buffer(1024 * 1024, '\0');
  {
    PageHeapSpinLockHolder l;
    Printer printer(&*buffer.begin(), buffer.size());
    filler_.Print(printer, true, pageflags);
  }
  buffer.resize(strlen(buffer.c_str()));

  EXPECT_THAT(buffer, testing::HasSubstr(R"(
HugePageFiller: Since the start of the execution, 6 subreleases (768 pages) were skipped due to either recent (120s) peaks, or the sum of short-term (60s) fluctuations and long-term (120s) trends.
HugePageFiller: 50.0000% of decisions confirmed correct, 0 pending (50.0000% of pages, 0 pending), as per anticipated 300s realized fragmentation.
)"));
}

TEST_P(FillerTest, SkipPartialAllocSubrelease_SpansAllocated) {
  // This test is sensitive to the number of pages per hugepage, as we are
  // printing raw stats.
  if (kPagesPerHugePage != Length(256)) {
    GTEST_SKIP();
  }
  randomize_density_ = false;
  SpanAllocInfo info = {kPagesPerHugePage.raw_num(),
                        AccessDensityPrediction::kDense};

  FakePageFlags pageflags;
  // Firstly, this test generates a peak (long-term demand peak) and waits for
  // time interval a. Then, it generates a higher peak plus a short-term
  // fluctuation peak, and waits for time interval b. It then generates a
  // trough in demand and tries to subrelease. Finally, it waits for time
  // interval c to generate the highest peak for evaluating subrelease
  // correctness. Skip subrelease selects those demand points using provided
  // time intervals.
  const auto demand_pattern = [&](absl::Duration a, absl::Duration b,
                                  absl::Duration c,
                                  SkipSubreleaseIntervals intervals,
                                  bool expected_subrelease) {
    const Length N = kPagesPerHugePage;
    // First peak: min_demand 3/4N, max_demand 1N.
    std::vector<PAlloc> peak1a =
        AllocateVectorWithSpanAllocInfo(3 * N / 4, info);
    ASSERT_TRUE(!peak1a.empty());
    std::vector<PAlloc> peak1b =
        AllocateVectorWithSpanAllocInfo(N / 4, peak1a.front().span_alloc_info);
    FakeClock::Advance(a);
    // Second peak: min_demand 0, max_demand 2N.
    DeleteVector(peak1a);
    DeleteVector(peak1b);

    std::vector<PAlloc> half = AllocateVectorWithSpanAllocInfo(N / 2, info);
    ASSERT_TRUE(!half.empty());
    std::vector<PAlloc> tiny1 =
        AllocateVectorWithSpanAllocInfo(N / 4, half.front().span_alloc_info);
    std::vector<PAlloc> tiny2 =
        AllocateVectorWithSpanAllocInfo(N / 4, half.front().span_alloc_info);

    // To force a peak, we allocate 3/4 and 1/4 of a huge page.  This is
    // necessary after we delete `half` below, as a half huge page for the
    // peak would fill into the gap previously occupied by it.
    std::vector<PAlloc> peak2a =
        AllocateVectorWithSpanAllocInfo(3 * N / 4, info);
    ASSERT_TRUE(!peak2a.empty());
    std::vector<PAlloc> peak2b =
        AllocateVectorWithSpanAllocInfo(N / 4, peak2a.front().span_alloc_info);
    EXPECT_EQ(filler_.used_pages(), 2 * N);
    DeleteVector(peak2a);
    DeleteVector(peak2b);
    FakeClock::Advance(b);
    DeleteVector(half);
    EXPECT_EQ(filler_.free_pages(), Length(N / 2));
    // The number of released pages is limited to the number of free pages.
    EXPECT_EQ(expected_subrelease ? N / 2 : Length(0),
              ReleasePartialPages(10 * N, intervals));

    FakeClock::Advance(c);
    half = AllocateVectorWithSpanAllocInfo(N / 2, half.front().span_alloc_info);
    // Third peak: min_demand 1/2N, max_demand (2+1/2)N.
    std::vector<PAlloc> peak3a =
        AllocateVectorWithSpanAllocInfo(3 * N / 4, info);
    ASSERT_TRUE(!peak3a.empty());
    std::vector<PAlloc> peak3b =
        AllocateVectorWithSpanAllocInfo(N / 4, peak3a.front().span_alloc_info);

    std::vector<PAlloc> peak4a =
        AllocateVectorWithSpanAllocInfo(3 * N / 4, info);
    ASSERT_TRUE(!peak4a.empty());
    std::vector<PAlloc> peak4b =
        AllocateVectorWithSpanAllocInfo(N / 4, peak4a.front().span_alloc_info);

    DeleteVector(half);
    DeleteVector(tiny1);
    DeleteVector(tiny2);
    DeleteVector(peak3a);
    DeleteVector(peak3b);
    DeleteVector(peak4a);
    DeleteVector(peak4b);

    EXPECT_EQ(filler_.used_pages(), Length(0));
    EXPECT_EQ(filler_.unmapped_pages(), Length(0));
    EXPECT_EQ(filler_.free_pages(), Length(0));

    EXPECT_EQ(Length(0), ReleasePartialPages(10 * N));
  };

  {
    // Uses peak interval for skipping subrelease. We should correctly skip
    // 128 pages.
    SCOPED_TRACE("demand_pattern 1");
    demand_pattern(absl::Minutes(2), absl::Minutes(1), absl::Minutes(3),
                   SkipSubreleaseIntervals{.peak_interval = absl::Minutes(3)},
                   false);
  }

  FakeClock::Advance(absl::Minutes(30));

  {
    // Repeats the "demand_pattern 1" test with additional short-term and
    // long-term intervals, to show that skip-subrelease prioritizes using
    // peak_interval.
    SCOPED_TRACE("demand_pattern 2");
    demand_pattern(
        absl::Minutes(2), absl::Minutes(1), absl::Minutes(3),
        SkipSubreleaseIntervals{.peak_interval = absl::Minutes(3),
                                .short_interval = absl::Milliseconds(10),
                                .long_interval = absl::Milliseconds(20)},
        false);
  }

  FakeClock::Advance(absl::Minutes(30));

  {
    // Uses peak interval for skipping subrelease, subreleasing all free
    // pages. The short-term interval is not used, as we prioritize using
    // demand peak.
    SCOPED_TRACE("demand_pattern 3");
    demand_pattern(absl::Minutes(6), absl::Minutes(3), absl::Minutes(3),
                   SkipSubreleaseIntervals{.peak_interval = absl::Minutes(2),
                                           .short_interval = absl::Minutes(5)},
                   true);
  }

  FakeClock::Advance(absl::Minutes(30));

  {
    // Skip subrelease feature is disabled if all intervals are zero.
    SCOPED_TRACE("demand_pattern 4");
    demand_pattern(absl::Minutes(1), absl::Minutes(1), absl::Minutes(4),
                   SkipSubreleaseIntervals{}, true);
  }

  FakeClock::Advance(absl::Minutes(30));

  {
    // Uses short-term and long-term intervals for skipping subrelease. It
    // incorrectly skips 128 pages.
    SCOPED_TRACE("demand_pattern 5");
    demand_pattern(absl::Minutes(3), absl::Minutes(2), absl::Minutes(7),
                   SkipSubreleaseIntervals{.short_interval = absl::Minutes(3),
                                           .long_interval = absl::Minutes(6)},
                   false);
  }

  FakeClock::Advance(absl::Minutes(30));

  {
    // Uses short-term and long-term intervals for skipping subrelease,
    // subreleasing all free pages.
    SCOPED_TRACE("demand_pattern 6");
    demand_pattern(absl::Minutes(4), absl::Minutes(2), absl::Minutes(3),
                   SkipSubreleaseIntervals{.short_interval = absl::Minutes(1),
                                           .long_interval = absl::Minutes(2)},
                   true);
  }
  FakeClock::Advance(absl::Minutes(30));

  {
    // Uses only short-term interval for skipping subrelease. It correctly
    // skips 128 pages.
    SCOPED_TRACE("demand_pattern 7");
    demand_pattern(absl::Minutes(4), absl::Minutes(2), absl::Minutes(3),
                   SkipSubreleaseIntervals{.short_interval = absl::Minutes(3)},
                   false);
  }

  FakeClock::Advance(absl::Minutes(30));

  {
    // Uses only long-term interval for skipping subrelease, subreleased all
    // free pages.
    SCOPED_TRACE("demand_pattern 8");
    demand_pattern(absl::Minutes(4), absl::Minutes(2), absl::Minutes(3),
                   SkipSubreleaseIntervals{.long_interval = absl::Minutes(2)},
                   true);
  }

  FakeClock::Advance(absl::Minutes(30));

  // This captures a corner case: If we hit another peak immediately after a
  // subrelease decision (in the same time series epoch), do not count this as
  // a correct subrelease decision.
  {
    SCOPED_TRACE("demand_pattern 9");
    demand_pattern(
        absl::Milliseconds(10), absl::Milliseconds(10), absl::Milliseconds(10),
        SkipSubreleaseIntervals{.peak_interval = absl::Minutes(2)}, false);
  }
  // Repeats the "demand_pattern 9" test using short-term and long-term
  // intervals, to show that subrelease decisions are evaluated independently.
  {
    SCOPED_TRACE("demand_pattern 10");
    demand_pattern(absl::Milliseconds(10), absl::Milliseconds(10),
                   absl::Milliseconds(10),
                   SkipSubreleaseIntervals{.short_interval = absl::Minutes(1),
                                           .long_interval = absl::Minutes(2)},
                   false);
  }

  FakeClock::Advance(absl::Minutes(30));

  // Ensure that the tracker is updated.
  auto tiny = Allocate(Length(1));
  Delete(tiny);

  std::string buffer(1024 * 1024, '\0');
  {
    PageHeapSpinLockHolder l;
    Printer printer(&*buffer.begin(), buffer.size());
    filler_.Print(printer, true, pageflags);
  }
  buffer.resize(strlen(buffer.c_str()));

  EXPECT_THAT(buffer, testing::HasSubstr(R"(
HugePageFiller: Since the start of the execution, 8 subreleases (1022 pages) were skipped due to either recent (120s) peaks, or the sum of short-term (60s) fluctuations and long-term (120s) trends.
HugePageFiller: 0.0000% of decisions confirmed correct, 0 pending (0.0000% of pages, 0 pending), as per anticipated 300s realized fragmentation.
)"));
}

TEST_P(FillerTest, SkipSubrelease) {
  // This test is sensitive to the number of pages per hugepage, as we are
  // printing raw stats.
  if (kPagesPerHugePage != Length(256)) {
    GTEST_SKIP();
  }

  FakePageFlags pageflags;
  // Firstly, this test generates a peak (long-term demand peak) and waits for
  // time interval a. Then, it generates a higher peak plus a short-term
  // fluctuation peak, and waits for time interval b. It then generates a
  // trough in demand and tries to subrelease. Finally, it waits for time
  // interval c to generate the highest peak for evaluating subrelease
  // correctness. Skip subrelease selects those demand points using provided
  // time intervals.
  const auto demand_pattern = [&](absl::Duration a, absl::Duration b,
                                  absl::Duration c,
                                  SkipSubreleaseIntervals intervals,
                                  bool expected_subrelease) {
    const Length N = kPagesPerHugePage;
    SpanAllocInfo info = {.objects_per_span = 1,
                          .density = AccessDensityPrediction::kSparse};
    // First peak: min_demand 3/4N, max_demand 1N.
    PAlloc peak1a = AllocateWithSpanAllocInfo(3 * N / 4, info);
    PAlloc peak1b = AllocateWithSpanAllocInfo(N / 4, info);
    FakeClock::Advance(a);
    // Second peak: min_demand 0, max_demand 2N.
    Delete(peak1a);
    Delete(peak1b);

    PAlloc half = AllocateWithSpanAllocInfo(N / 2, info);
    PAlloc tiny1 = AllocateWithSpanAllocInfo(N / 4, info);
    PAlloc tiny2 = AllocateWithSpanAllocInfo(N / 4, info);

    // To force a peak, we allocate 3/4 and 1/4 of a huge page.  This is
    // necessary after we delete `half` below, as a half huge page for the
    // peak would fill into the gap previously occupied by it.
    PAlloc peak2a = AllocateWithSpanAllocInfo(3 * N / 4, info);
    PAlloc peak2b = AllocateWithSpanAllocInfo(N / 4, info);
    EXPECT_EQ(filler_.used_pages(), 2 * N);
    Delete(peak2a);
    Delete(peak2b);
    FakeClock::Advance(b);
    Delete(half);
    EXPECT_EQ(filler_.free_pages(), Length(N / 2));
    // The number of released pages is limited to the number of free pages.
    EXPECT_EQ(expected_subrelease ? N / 2 : Length(0),
              ReleasePages(10 * N, intervals));

    FakeClock::Advance(c);
    // Third peak: min_demand 1/2N, max_demand (2+1/2)N.
    PAlloc peak3a = AllocateWithSpanAllocInfo(3 * N / 4, info);
    PAlloc peak3b = AllocateWithSpanAllocInfo(N / 4, info);

    PAlloc peak4a = AllocateWithSpanAllocInfo(3 * N / 4, info);
    PAlloc peak4b = AllocateWithSpanAllocInfo(N / 4, info);

    Delete(tiny1);
    Delete(tiny2);
    Delete(peak3a);
    Delete(peak3b);
    Delete(peak4a);
    Delete(peak4b);

    EXPECT_EQ(filler_.used_pages(), Length(0));
    EXPECT_EQ(filler_.unmapped_pages(), Length(0));
    EXPECT_EQ(filler_.free_pages(), Length(0));
    EXPECT_EQ(expected_subrelease ? N / 2 : Length(0), ReleasePages(10 * N));
  };

  {
    // Uses peak interval for skipping subrelease. We should correctly skip
    // 128 pages.
    SCOPED_TRACE("demand_pattern 1");
    demand_pattern(absl::Minutes(2), absl::Minutes(1), absl::Minutes(3),
                   SkipSubreleaseIntervals{.peak_interval = absl::Minutes(3)},
                   false);
  }

  FakeClock::Advance(absl::Minutes(30));

  {
    // Repeats the "demand_pattern 1" test with additional short-term and
    // long-term intervals, to show that skip-subrelease prioritizes using
    // peak_interval.
    SCOPED_TRACE("demand_pattern 2");
    demand_pattern(
        absl::Minutes(2), absl::Minutes(1), absl::Minutes(3),
        SkipSubreleaseIntervals{.peak_interval = absl::Minutes(3),
                                .short_interval = absl::Milliseconds(10),
                                .long_interval = absl::Milliseconds(20)},
        false);
  }

  FakeClock::Advance(absl::Minutes(30));

  {
    // Uses peak interval for skipping subrelease, subreleasing all free pages
    // . The short-term interval is not used, as we prioritize using demand
    // peak.
    SCOPED_TRACE("demand_pattern 3");
    demand_pattern(absl::Minutes(6), absl::Minutes(3), absl::Minutes(3),
                   SkipSubreleaseIntervals{.peak_interval = absl::Minutes(2),
                                           .short_interval = absl::Minutes(5)},
                   true);
  }

  FakeClock::Advance(absl::Minutes(30));

  {
    // Skip subrelease feature is disabled if all intervals are zero.
    SCOPED_TRACE("demand_pattern 4");
    demand_pattern(absl::Minutes(1), absl::Minutes(1), absl::Minutes(4),
                   SkipSubreleaseIntervals{}, true);
  }

  FakeClock::Advance(absl::Minutes(30));

  {
    // Uses short-term and long-term intervals for skipping subrelease. It
    // incorrectly skips 128 pages.
    SCOPED_TRACE("demand_pattern 5");
    demand_pattern(absl::Minutes(3), absl::Minutes(2), absl::Minutes(7),
                   SkipSubreleaseIntervals{.short_interval = absl::Minutes(3),
                                           .long_interval = absl::Minutes(6)},
                   false);
  }

  FakeClock::Advance(absl::Minutes(30));

  {
    // Uses short-term and long-term intervals for skipping subrelease,
    // subreleasing all free pages.
    SCOPED_TRACE("demand_pattern 6");
    demand_pattern(absl::Minutes(4), absl::Minutes(2), absl::Minutes(3),
                   SkipSubreleaseIntervals{.short_interval = absl::Minutes(1),
                                           .long_interval = absl::Minutes(2)},
                   true);
  }
  FakeClock::Advance(absl::Minutes(30));

  {
    // Uses only short-term interval for skipping subrelease. It correctly
    // skips 128 pages.
    SCOPED_TRACE("demand_pattern 7");
    demand_pattern(absl::Minutes(4), absl::Minutes(2), absl::Minutes(3),
                   SkipSubreleaseIntervals{.short_interval = absl::Minutes(3)},
                   false);
  }

  FakeClock::Advance(absl::Minutes(30));

  {
    // Uses only long-term interval for skipping subrelease, subreleased all
    // free pages.
    SCOPED_TRACE("demand_pattern 8");
    demand_pattern(absl::Minutes(4), absl::Minutes(2), absl::Minutes(3),
                   SkipSubreleaseIntervals{.long_interval = absl::Minutes(2)},
                   true);
  }

  FakeClock::Advance(absl::Minutes(30));

  // This captures a corner case: If we hit another peak immediately after a
  // subrelease decision (in the same time series epoch), do not count this as
  // a correct subrelease decision.
  {
    SCOPED_TRACE("demand_pattern 9");
    demand_pattern(
        absl::Milliseconds(10), absl::Milliseconds(10), absl::Milliseconds(10),
        SkipSubreleaseIntervals{.peak_interval = absl::Minutes(2)}, false);
  }
  // Repeats the "demand_pattern 9" test using short-term and long-term
  // intervals, to show that subrelease decisions are evaluated independently.
  {
    SCOPED_TRACE("demand_pattern 10");
    demand_pattern(absl::Milliseconds(10), absl::Milliseconds(10),
                   absl::Milliseconds(10),
                   SkipSubreleaseIntervals{.short_interval = absl::Minutes(1),
                                           .long_interval = absl::Minutes(2)},
                   false);
  }

  FakeClock::Advance(absl::Minutes(30));

  // Ensure that the tracker is updated.
  auto tiny = Allocate(Length(1));
  Delete(tiny);

  std::string buffer(1024 * 1024, '\0');
  {
    PageHeapSpinLockHolder l;
    Printer printer(&*buffer.begin(), buffer.size());
    filler_.Print(printer, true, pageflags);
  }
  buffer.resize(strlen(buffer.c_str()));

  EXPECT_THAT(buffer, testing::HasSubstr(R"(
HugePageFiller: Since the start of the execution, 6 subreleases (768 pages) were skipped due to either recent (120s) peaks, or the sum of short-term (60s) fluctuations and long-term (120s) trends.
HugePageFiller: 50.0000% of decisions confirmed correct, 0 pending (50.0000% of pages, 0 pending), as per anticipated 300s realized fragmentation.
)"));
}

TEST_P(FillerTest, SkipSubrelease_SpansAllocated) {
  // This test is sensitive to the number of pages per hugepage, as we are
  // printing raw stats.
  if (kPagesPerHugePage != Length(256)) {
    GTEST_SKIP();
  }
  randomize_density_ = false;
  FakePageFlags pageflags;
  SpanAllocInfo info = {kPagesPerHugePage.raw_num(),
                        AccessDensityPrediction::kDense};

  // Firstly, this test generates a peak (long-term demand peak) and waits for
  // time interval a. Then, it generates a higher peak plus a short-term
  // fluctuation peak, and waits for time interval b. It then generates a
  // trough in demand and tries to subrelease. Finally, it waits for time
  // interval c to generate the highest peak for evaluating subrelease
  // correctness. Skip subrelease selects those demand points using provided
  // time intervals.
  const auto demand_pattern = [&](absl::Duration a, absl::Duration b,
                                  absl::Duration c,
                                  SkipSubreleaseIntervals intervals,
                                  bool expected_subrelease) {
    const Length N = kPagesPerHugePage;
    // First peak: min_demand 3/4N, max_demand 1N.
    std::vector<PAlloc> peak1a =
        AllocateVectorWithSpanAllocInfo(3 * N / 4, info);
    ASSERT_TRUE(!peak1a.empty());
    std::vector<PAlloc> peak1b =
        AllocateVectorWithSpanAllocInfo(N / 4, peak1a.front().span_alloc_info);
    FakeClock::Advance(a);
    // Second peak: min_demand 0, max_demand 2N.
    DeleteVector(peak1a);
    DeleteVector(peak1b);

    std::vector<PAlloc> half = AllocateVectorWithSpanAllocInfo(N / 2, info);
    ASSERT_TRUE(!half.empty());
    std::vector<PAlloc> tiny1 =
        AllocateVectorWithSpanAllocInfo(N / 4, half.front().span_alloc_info);
    std::vector<PAlloc> tiny2 =
        AllocateVectorWithSpanAllocInfo(N / 4, half.front().span_alloc_info);

    // To force a peak, we allocate 3/4 and 1/4 of a huge page.  This is
    // necessary after we delete `half` below, as a half huge page for the
    // peak would fill into the gap previously occupied by it.
    std::vector<PAlloc> peak2a =
        AllocateVectorWithSpanAllocInfo(3 * N / 4, info);
    ASSERT_TRUE(!peak2a.empty());
    std::vector<PAlloc> peak2b =
        AllocateVectorWithSpanAllocInfo(N / 4, peak2a.front().span_alloc_info);
    EXPECT_EQ(filler_.used_pages(), 2 * N);
    DeleteVector(peak2a);
    DeleteVector(peak2b);
    FakeClock::Advance(b);
    DeleteVector(half);
    EXPECT_EQ(filler_.free_pages(), Length(N / 2));
    // The number of released pages is limited to the number of free pages.
    EXPECT_EQ(expected_subrelease ? N / 2 : Length(0),
              ReleasePages(10 * N, intervals));

    FakeClock::Advance(c);
    // Third peak: min_demand 1/2N, max_demand (2+1/2)N.
    std::vector<PAlloc> peak3a =
        AllocateVectorWithSpanAllocInfo(3 * N / 4, info);
    ASSERT_TRUE(!peak3a.empty());
    std::vector<PAlloc> peak3b =
        AllocateVectorWithSpanAllocInfo(N / 4, peak3a.front().span_alloc_info);

    std::vector<PAlloc> peak4a =
        AllocateVectorWithSpanAllocInfo(3 * N / 4, info);
    ASSERT_TRUE(!peak4a.empty());
    std::vector<PAlloc> peak4b =
        AllocateVectorWithSpanAllocInfo(N / 4, peak4a.front().span_alloc_info);

    DeleteVector(tiny1);
    DeleteVector(tiny2);
    DeleteVector(peak3a);
    DeleteVector(peak3b);
    DeleteVector(peak4a);
    DeleteVector(peak4b);

    EXPECT_EQ(filler_.used_pages(), Length(0));
    EXPECT_EQ(filler_.unmapped_pages(), Length(0));
    EXPECT_EQ(filler_.free_pages(), Length(0));
    EXPECT_EQ(Length(0), ReleasePages(10 * N));
  };

  {
    // Uses peak interval for skipping subrelease. We should correctly skip
    // 128 pages.
    SCOPED_TRACE("demand_pattern 1");
    demand_pattern(absl::Minutes(2), absl::Minutes(1), absl::Minutes(3),
                   SkipSubreleaseIntervals{.peak_interval = absl::Minutes(3)},
                   false);
  }

  FakeClock::Advance(absl::Minutes(30));

  {
    // Repeats the "demand_pattern 1" test with additional short-term and
    // long-term intervals, to show that skip-subrelease prioritizes using
    // peak_interval.
    SCOPED_TRACE("demand_pattern 2");
    demand_pattern(
        absl::Minutes(2), absl::Minutes(1), absl::Minutes(3),
        SkipSubreleaseIntervals{.peak_interval = absl::Minutes(3),
                                .short_interval = absl::Milliseconds(10),
                                .long_interval = absl::Milliseconds(20)},
        false);
  }

  FakeClock::Advance(absl::Minutes(30));

  {
    // Uses peak interval for skipping subrelease, subreleasing all free
    // pages. The short-term interval is not used, as we prioritize using
    // demand peak.
    SCOPED_TRACE("demand_pattern 3");
    demand_pattern(absl::Minutes(6), absl::Minutes(3), absl::Minutes(3),
                   SkipSubreleaseIntervals{.peak_interval = absl::Minutes(2),
                                           .short_interval = absl::Minutes(5)},
                   true);
  }

  FakeClock::Advance(absl::Minutes(30));

  {
    // Skip subrelease feature is disabled if all intervals are zero.
    SCOPED_TRACE("demand_pattern 4");
    demand_pattern(absl::Minutes(1), absl::Minutes(1), absl::Minutes(4),
                   SkipSubreleaseIntervals{}, true);
  }

  FakeClock::Advance(absl::Minutes(30));

  {
    // Uses short-term and long-term intervals for skipping subrelease. It
    // incorrectly skips 128 pages.
    SCOPED_TRACE("demand_pattern 5");
    demand_pattern(absl::Minutes(3), absl::Minutes(2), absl::Minutes(7),
                   SkipSubreleaseIntervals{.short_interval = absl::Minutes(3),
                                           .long_interval = absl::Minutes(6)},
                   false);
  }

  FakeClock::Advance(absl::Minutes(30));

  {
    // Uses short-term and long-term intervals for skipping subrelease,
    // subreleasing all free pages.
    SCOPED_TRACE("demand_pattern 6");
    demand_pattern(absl::Minutes(4), absl::Minutes(2), absl::Minutes(3),
                   SkipSubreleaseIntervals{.short_interval = absl::Minutes(1),
                                           .long_interval = absl::Minutes(2)},
                   true);
  }
  FakeClock::Advance(absl::Minutes(30));

  {
    // Uses only short-term interval for skipping subrelease. It correctly
    // skips 128 pages.
    SCOPED_TRACE("demand_pattern 7");
    demand_pattern(absl::Minutes(4), absl::Minutes(2), absl::Minutes(3),
                   SkipSubreleaseIntervals{.short_interval = absl::Minutes(3)},
                   false);
  }

  FakeClock::Advance(absl::Minutes(30));

  {
    // Uses only long-term interval for skipping subrelease, subreleased all
    // free pages.
    SCOPED_TRACE("demand_pattern 8");
    demand_pattern(absl::Minutes(4), absl::Minutes(2), absl::Minutes(3),
                   SkipSubreleaseIntervals{.long_interval = absl::Minutes(2)},
                   true);
  }

  FakeClock::Advance(absl::Minutes(30));

  // This captures a corner case: If we hit another peak immediately after a
  // subrelease decision (in the same time series epoch), do not count this as
  // a correct subrelease decision.
  {
    SCOPED_TRACE("demand_pattern 9");
    demand_pattern(
        absl::Milliseconds(10), absl::Milliseconds(10), absl::Milliseconds(10),
        SkipSubreleaseIntervals{.peak_interval = absl::Minutes(2)}, false);
  }
  // Repeats the "demand_pattern 9" test using short-term and long-term
  // intervals, to show that subrelease decisions are evaluated independently.
  {
    SCOPED_TRACE("demand_pattern 10");
    demand_pattern(absl::Milliseconds(10), absl::Milliseconds(10),
                   absl::Milliseconds(10),
                   SkipSubreleaseIntervals{.short_interval = absl::Minutes(1),
                                           .long_interval = absl::Minutes(2)},
                   false);
  }

  FakeClock::Advance(absl::Minutes(30));

  // Ensure that the tracker is updated.
  auto tiny = Allocate(Length(1));
  Delete(tiny);

  std::string buffer(1024 * 1024, '\0');
  {
    PageHeapSpinLockHolder l;
    Printer printer(&*buffer.begin(), buffer.size());
    filler_.Print(printer, true, pageflags);
  }
  buffer.resize(strlen(buffer.c_str()));

  EXPECT_THAT(buffer, testing::HasSubstr(R"(
HugePageFiller: Since the start of the execution, 8 subreleases (1022 pages) were skipped due to either recent (120s) peaks, or the sum of short-term (60s) fluctuations and long-term (120s) trends.
HugePageFiller: 0.0000% of decisions confirmed correct, 0 pending (0.0000% of pages, 0 pending), as per anticipated 300s realized fragmentation.
)"));
}

TEST_P(FillerTest, RecordFeatureVectorTest) {
  SpanAllocInfo info_sparsely_accessed = {2, AccessDensityPrediction::kSparse};
  PAlloc small_alloc =
      AllocateWithSpanAllocInfo(Length(1), info_sparsely_accessed);
  small_alloc.pt->SetTagState({.sampled_for_tagging = true});
  EXPECT_EQ(small_alloc.pt->features().allocations, 0);
  EXPECT_EQ(small_alloc.pt->features().objects, 0);
  EXPECT_EQ(small_alloc.pt->features().allocation_time, 0);
  EXPECT_EQ(small_alloc.pt->features().longest_free_range.raw_num(), 256);
  EXPECT_EQ(small_alloc.pt->features().is_hugepage_backed, false);
  EXPECT_EQ(small_alloc.pt->features().density, false);
  EXPECT_EQ(small_alloc.pt->last_page_allocation_time(), 0);

  FakeClock::Advance(absl::Seconds(5));
  PAlloc small_alloc2 =
      AllocateWithSpanAllocInfo(Length(5), info_sparsely_accessed);
  EXPECT_EQ(small_alloc.pt, small_alloc2.pt);
  EXPECT_EQ(small_alloc.pt->features().allocations, 1);
  EXPECT_EQ(small_alloc.pt->features().objects, 2);
  EXPECT_FLOAT_EQ(small_alloc.pt->features().allocation_time, 0);
  EXPECT_EQ(small_alloc.pt->features().longest_free_range.raw_num(), 255);
  EXPECT_EQ(small_alloc.pt->features().is_hugepage_backed, false);
  EXPECT_EQ(small_alloc.pt->features().density, false);
  EXPECT_EQ(small_alloc.pt->last_page_allocation_time(),
            static_cast<uint64_t>(5 * FakeClock::freq()) + 1234);

  FakeClock::Advance(absl::Seconds(10));
  PAlloc small_alloc3 =
      AllocateWithSpanAllocInfo(Length(10), info_sparsely_accessed);
  EXPECT_EQ(small_alloc.pt, small_alloc3.pt);
  EXPECT_EQ(small_alloc.pt->features().allocations, 2);
  EXPECT_EQ(small_alloc.pt->features().objects, 4);
  EXPECT_FLOAT_EQ(small_alloc.pt->features().allocation_time,
                  5 * FakeClock::freq() + 1234);
  EXPECT_EQ(small_alloc.pt->features().longest_free_range.raw_num(), 250);
  EXPECT_EQ(small_alloc.pt->features().is_hugepage_backed, false);
  EXPECT_EQ(small_alloc.pt->features().density, false);
  EXPECT_EQ(small_alloc.pt->last_page_allocation_time(),
            static_cast<uint64_t>(15 * FakeClock::freq()) + 1234);

  // Test dense spans.
  FakeClock::ResetClock();
  SpanAllocInfo info_densely_accessed = {128, AccessDensityPrediction::kDense};
  PAlloc large_alloc =
      AllocateWithSpanAllocInfo(Length(1), info_densely_accessed);
  large_alloc.pt->SetTagState({.sampled_for_tagging = true});
  EXPECT_NE(large_alloc.pt, small_alloc.pt);
  EXPECT_EQ(large_alloc.pt->features().allocations, 0);
  EXPECT_EQ(large_alloc.pt->features().objects, 0);
  EXPECT_FLOAT_EQ(large_alloc.pt->features().allocation_time, 0);
  EXPECT_EQ(large_alloc.pt->features().longest_free_range.raw_num(), 256);
  EXPECT_EQ(large_alloc.pt->features().is_hugepage_backed, false);
  // Density is false because it defaults to false and lags behind by
  // one allocation.
  EXPECT_EQ(large_alloc.pt->features().density, false);
  EXPECT_EQ(large_alloc.pt->last_page_allocation_time(), 0);

  FakeClock::Advance(absl::Seconds(10));
  std::vector<PAlloc> large_allocs =
      AllocateVectorWithSpanAllocInfo(Length(100), info_densely_accessed);
  EXPECT_EQ(large_alloc.pt->features().allocations, 100);
  EXPECT_EQ(large_alloc.pt->features().objects, 100 * 128);
  EXPECT_FLOAT_EQ(large_alloc.pt->features().allocation_time,
                  10 * FakeClock::freq() + 1234);
  EXPECT_EQ(large_alloc.pt->features().longest_free_range.raw_num(), 156);
  EXPECT_EQ(large_alloc.pt->features().density, true);
  EXPECT_EQ(large_alloc.pt->features().is_hugepage_backed, false);
  EXPECT_EQ(large_alloc.pt->last_page_allocation_time(),
            static_cast<uint64_t>(10 * FakeClock::freq()) + 1234);

  FakeClock::Advance(absl::Seconds(10));
  PAlloc large_alloc2 =
      AllocateWithSpanAllocInfo(Length(1), info_densely_accessed);
  EXPECT_EQ(large_alloc.pt->features().allocations, 101);
  EXPECT_EQ(large_alloc.pt->features().objects, 101 * 128);
  EXPECT_FLOAT_EQ(large_alloc.pt->features().allocation_time,
                  10 * FakeClock::freq() + 1234);
  EXPECT_EQ(large_alloc.pt->features().longest_free_range.raw_num(), 155);
  EXPECT_EQ(large_alloc.pt->features().is_hugepage_backed, false);
  EXPECT_EQ(large_alloc.pt->features().density, true);
  EXPECT_EQ(large_alloc.pt->last_page_allocation_time(),
            static_cast<uint64_t>(20 * FakeClock::freq()) + 1234);

  Delete(small_alloc);
  Delete(small_alloc2);
  Delete(small_alloc3);

  Delete(large_alloc);
  Delete(large_alloc2);
  for (auto& alloc : large_allocs) {
    Delete(alloc);
  }
}

TEST_P(FillerTest, PrintFeatureVectorTest) {
  FakePageFlags pageflags;
  const Length N = kPagesPerHugePage;
  SpanAllocInfo info_sparsely_accessed = {1, AccessDensityPrediction::kSparse};
  SpanAllocInfo info_densely_accessed = {2, AccessDensityPrediction::kDense};
  PAlloc small_alloc = AllocateWithSpanAllocInfo(N / 4, info_sparsely_accessed);
  small_alloc.pt->SetTagState({.sampled_for_tagging = true});
  std::string buffer(1024 * 1024, '\0');
  {
    PageHeapSpinLockHolder l;
    Printer printer(&*buffer.begin(), buffer.size());
    filler_.Print(printer, true, pageflags);
  }
  buffer.resize(strlen(buffer.c_str()));
  EXPECT_THAT(buffer, testing::HasSubstr(R"(
HugePageFiller: Allocations: 0, Longest Free Range: 256, Objects: 0, Is Hugepage Backed?: 0, Density: 0, Reallocation Time: 0.000000
)"));

  FakeClock::Advance(absl::Seconds(100));
  PAlloc small_alloc2 =
      AllocateWithSpanAllocInfo(N / 4, info_sparsely_accessed);
  EXPECT_EQ(small_alloc.pt, small_alloc2.pt);
  EXPECT_EQ(small_alloc.pt->features().allocation_time, 0);
  EXPECT_EQ(small_alloc.pt->last_page_allocation_time(),
            static_cast<uint64_t>(100 * FakeClock::freq()) + 1234);
  EXPECT_EQ(small_alloc.pt->features().allocation_time, 0);
  {
    PageHeapSpinLockHolder l;
    Printer printer(&*buffer.begin(), buffer.size());
    filler_.Print(printer, true, pageflags);
  }
  buffer.resize(strlen(buffer.c_str()));
  EXPECT_THAT(buffer, testing::HasSubstr(R"(
HugePageFiller: Allocations: 1, Longest Free Range: 192, Objects: 1, Is Hugepage Backed?: 0, Density: 0, Reallocation Time: 100.000001
)"));

  FakeClock::ResetClock();
  PAlloc large_alloc =
      AllocateWithSpanAllocInfo(Length(1), info_densely_accessed);
  large_alloc.pt->SetTagState({.sampled_for_tagging = true});
  FakeClock::Advance(absl::Seconds(100));

  PAlloc large_alloc2 =
      AllocateWithSpanAllocInfo(Length(1), info_densely_accessed);
  EXPECT_EQ(large_alloc.pt, large_alloc2.pt);

  pageflags.MarkHugePageBacked(large_alloc.pt->location().start_addr(), true);
  {
    PageHeapSpinLockHolder l;
    Printer printer(&*buffer.begin(), buffer.size());
    filler_.Print(printer, true, pageflags);
  }
  EXPECT_THAT(buffer, testing::HasSubstr(R"(
HugePageFiller: Allocations: 1, Longest Free Range: 255, Objects: 2, Is Hugepage Backed?: 0, Density: 1, Reallocation Time: 100.000001
)"));

  FakeClock::Advance(absl::Seconds(100));
  std::vector<PAlloc> large_allocs =
      AllocateVectorWithSpanAllocInfo(Length(100), info_densely_accessed);
  {
    PageHeapSpinLockHolder l;
    Printer printer(&*buffer.begin(), buffer.size());
    filler_.Print(printer, true, pageflags);
  }
  buffer.resize(strlen(buffer.c_str()));
  // Time delta is 0 here because the clock is not advanced during vector
  // allocation.
  EXPECT_THAT(buffer, testing::HasSubstr(R"(
HugePageFiller: Allocations: 101, Longest Free Range: 155, Objects: 202, Is Hugepage Backed?: 0, Density: 1, Reallocation Time: 0.000000
)"));

  Delete(small_alloc);
  Delete(small_alloc2);
  Delete(large_alloc);
  Delete(large_alloc2);

  for (auto& alloc : large_allocs) {
    Delete(alloc);
  }
}

TEST_P(FillerTest, LiveLifetimeTelemetryTest) {
  // This test is sensitive to the number of pages per hugepage, as we are
  // printing raw stats.
  if (kPagesPerHugePage != Length(256)) {
    GTEST_SKIP();
  }

  FakePageFlags pageflags;
  const Length N = kPagesPerHugePage;
  SpanAllocInfo info_sparsely_accessed = {1, AccessDensityPrediction::kSparse};
  PAlloc small_alloc = AllocateWithSpanAllocInfo(N / 4, info_sparsely_accessed);
  PAlloc large_alloc =
      AllocateWithSpanAllocInfo(3 * N / 4, info_sparsely_accessed);

  std::string buffer(1024 * 1024, '\0');
  {
    PageHeapSpinLockHolder l;
    Printer printer(&*buffer.begin(), buffer.size());
    filler_.Print(printer, true, pageflags);
  }
  buffer.resize(strlen(buffer.c_str()));
  EXPECT_THAT(buffer, testing::HasSubstr(R"(
HugePageFiller: # of sparsely-accessed regular hps with live lifetime a <= # hps < b
HugePageFiller: <   0 ms <=      1 <   1 ms <=      0 <  10 ms <=      0 < 100 ms <=      0 < 1000 ms <=      0 < 10000 ms <=      0
HugePageFiller: < 100000 ms <=      0 < 1000000 ms <=      0

HugePageFiller: # of hps with >= 224 free pages, with different lifetimes.
HugePageFiller: # of sparsely-accessed regular hps with lifetime a <= # hps < b
HugePageFiller: <   0 ms <=      0 <   1 ms <=      0 <  10 ms <=      0 < 100 ms <=      0 < 1000 ms <=      0 < 10000 ms <=      0
HugePageFiller: < 100000 ms <=      0 < 1000000 ms <=      0

HugePageFiller: # of hps with lifetime >= 100000 ms.
HugePageFiller: # of sparsely-accessed regular hps with a <= # of allocations < b
HugePageFiller: <  0<=     0 <  1<=     0 <  2<=     0 <  3<=     0 <  4<=     0 <  5<=     0
HugePageFiller: <  6<=     0 <  7<=     0 <  8<=     0 < 16<=     0 < 32<=     0 < 48<=     0
HugePageFiller: < 64<=     0 < 80<=     0 < 96<=     0 <112<=     0 <128<=     0 <144<=     0
HugePageFiller: <160<=     0 <176<=     0 <192<=     0 <208<=     0 <224<=     0 <240<=     0
HugePageFiller: <248<=     0 <249<=     0 <250<=     0 <251<=     0 <252<=     0 <253<=     0
HugePageFiller: <254<=     0 <255<=     0
)"));

  EXPECT_THAT(buffer, testing::HasSubstr(R"(
HugePageFiller: # of densely-accessed regular hps with live lifetime a <= # hps < b
HugePageFiller: <   0 ms <=      0 <   1 ms <=      0 <  10 ms <=      0 < 100 ms <=      0 < 1000 ms <=      0 < 10000 ms <=      0
HugePageFiller: < 100000 ms <=      0 < 1000000 ms <=      0

HugePageFiller: # of hps with >= 224 free pages, with different lifetimes.
HugePageFiller: # of densely-accessed regular hps with lifetime a <= # hps < b
HugePageFiller: <   0 ms <=      0 <   1 ms <=      0 <  10 ms <=      0 < 100 ms <=      0 < 1000 ms <=      0 < 10000 ms <=      0
HugePageFiller: < 100000 ms <=      0 < 1000000 ms <=      0

HugePageFiller: # of hps with lifetime >= 100000 ms.
HugePageFiller: # of densely-accessed regular hps with a <= # of allocations < b
HugePageFiller: <  0<=     0 <  1<=     0 <  2<=     0 <  3<=     0 <  4<=     0 <  5<=     0
HugePageFiller: <  6<=     0 <  7<=     0 <  8<=     0 < 16<=     0 < 32<=     0 < 48<=     0
HugePageFiller: < 64<=     0 < 80<=     0 < 96<=     0 <112<=     0 <128<=     0 <144<=     0
HugePageFiller: <160<=     0 <176<=     0 <192<=     0 <208<=     0 <224<=     0 <240<=     0
HugePageFiller: <248<=     0 <249<=     0 <250<=     0 <251<=     0 <252<=     0 <253<=     0
HugePageFiller: <254<=     0 <255<=     0
)"));

  EXPECT_THAT(buffer, testing::HasSubstr(R"(
HugePageFiller: # of donated hps with live lifetime a <= # hps < b
HugePageFiller: <   0 ms <=      0 <   1 ms <=      0 <  10 ms <=      0 < 100 ms <=      0 < 1000 ms <=      0 < 10000 ms <=      0
HugePageFiller: < 100000 ms <=      0 < 1000000 ms <=      0

HugePageFiller: # of hps with >= 224 free pages, with different lifetimes.
HugePageFiller: # of donated hps with lifetime a <= # hps < b
HugePageFiller: <   0 ms <=      0 <   1 ms <=      0 <  10 ms <=      0 < 100 ms <=      0 < 1000 ms <=      0 < 10000 ms <=      0
HugePageFiller: < 100000 ms <=      0 < 1000000 ms <=      0

HugePageFiller: # of hps with lifetime >= 100000 ms.
HugePageFiller: # of donated hps with a <= # of allocations < b
HugePageFiller: <  0<=     0 <  1<=     0 <  2<=     0 <  3<=     0 <  4<=     0 <  5<=     0
HugePageFiller: <  6<=     0 <  7<=     0 <  8<=     0 < 16<=     0 < 32<=     0 < 48<=     0
HugePageFiller: < 64<=     0 < 80<=     0 < 96<=     0 <112<=     0 <128<=     0 <144<=     0
HugePageFiller: <160<=     0 <176<=     0 <192<=     0 <208<=     0 <224<=     0 <240<=     0
HugePageFiller: <248<=     0 <249<=     0 <250<=     0 <251<=     0 <252<=     0 <253<=     0
HugePageFiller: <254<=     0 <255<=     0
)"));

  EXPECT_THAT(buffer, testing::HasSubstr(R"(
HugePageFiller: # of sparsely-accessed partial released hps with live lifetime a <= # hps < b
HugePageFiller: <   0 ms <=      0 <   1 ms <=      0 <  10 ms <=      0 < 100 ms <=      0 < 1000 ms <=      0 < 10000 ms <=      0
HugePageFiller: < 100000 ms <=      0 < 1000000 ms <=      0

HugePageFiller: # of hps with >= 224 free pages, with different lifetimes.
HugePageFiller: # of sparsely-accessed partial released hps with lifetime a <= # hps < b
HugePageFiller: <   0 ms <=      0 <   1 ms <=      0 <  10 ms <=      0 < 100 ms <=      0 < 1000 ms <=      0 < 10000 ms <=      0
HugePageFiller: < 100000 ms <=      0 < 1000000 ms <=      0

HugePageFiller: # of hps with lifetime >= 100000 ms.
HugePageFiller: # of sparsely-accessed partial released hps with a <= # of allocations < b
HugePageFiller: <  0<=     0 <  1<=     0 <  2<=     0 <  3<=     0 <  4<=     0 <  5<=     0
HugePageFiller: <  6<=     0 <  7<=     0 <  8<=     0 < 16<=     0 < 32<=     0 < 48<=     0
HugePageFiller: < 64<=     0 < 80<=     0 < 96<=     0 <112<=     0 <128<=     0 <144<=     0
HugePageFiller: <160<=     0 <176<=     0 <192<=     0 <208<=     0 <224<=     0 <240<=     0
HugePageFiller: <248<=     0 <249<=     0 <250<=     0 <251<=     0 <252<=     0 <253<=     0
HugePageFiller: <254<=     0 <255<=     0
)"));

  EXPECT_THAT(buffer, testing::HasSubstr(R"(
HugePageFiller: # of densely-accessed partial released hps with live lifetime a <= # hps < b
HugePageFiller: <   0 ms <=      0 <   1 ms <=      0 <  10 ms <=      0 < 100 ms <=      0 < 1000 ms <=      0 < 10000 ms <=      0
HugePageFiller: < 100000 ms <=      0 < 1000000 ms <=      0

HugePageFiller: # of hps with >= 224 free pages, with different lifetimes.
HugePageFiller: # of densely-accessed partial released hps with lifetime a <= # hps < b
HugePageFiller: <   0 ms <=      0 <   1 ms <=      0 <  10 ms <=      0 < 100 ms <=      0 < 1000 ms <=      0 < 10000 ms <=      0
HugePageFiller: < 100000 ms <=      0 < 1000000 ms <=      0

HugePageFiller: # of hps with lifetime >= 100000 ms.
HugePageFiller: # of densely-accessed partial released hps with a <= # of allocations < b
HugePageFiller: <  0<=     0 <  1<=     0 <  2<=     0 <  3<=     0 <  4<=     0 <  5<=     0
HugePageFiller: <  6<=     0 <  7<=     0 <  8<=     0 < 16<=     0 < 32<=     0 < 48<=     0
HugePageFiller: < 64<=     0 < 80<=     0 < 96<=     0 <112<=     0 <128<=     0 <144<=     0
HugePageFiller: <160<=     0 <176<=     0 <192<=     0 <208<=     0 <224<=     0 <240<=     0
HugePageFiller: <248<=     0 <249<=     0 <250<=     0 <251<=     0 <252<=     0 <253<=     0
HugePageFiller: <254<=     0 <255<=     0
)"));

  EXPECT_THAT(buffer, testing::HasSubstr(R"(
HugePageFiller: # of sparsely-accessed released hps with live lifetime a <= # hps < b
HugePageFiller: <   0 ms <=      0 <   1 ms <=      0 <  10 ms <=      0 < 100 ms <=      0 < 1000 ms <=      0 < 10000 ms <=      0
HugePageFiller: < 100000 ms <=      0 < 1000000 ms <=      0

HugePageFiller: # of hps with >= 224 free pages, with different lifetimes.
HugePageFiller: # of sparsely-accessed released hps with lifetime a <= # hps < b
HugePageFiller: <   0 ms <=      0 <   1 ms <=      0 <  10 ms <=      0 < 100 ms <=      0 < 1000 ms <=      0 < 10000 ms <=      0
HugePageFiller: < 100000 ms <=      0 < 1000000 ms <=      0

HugePageFiller: # of hps with lifetime >= 100000 ms.
HugePageFiller: # of sparsely-accessed released hps with a <= # of allocations < b
HugePageFiller: <  0<=     0 <  1<=     0 <  2<=     0 <  3<=     0 <  4<=     0 <  5<=     0
HugePageFiller: <  6<=     0 <  7<=     0 <  8<=     0 < 16<=     0 < 32<=     0 < 48<=     0
HugePageFiller: < 64<=     0 < 80<=     0 < 96<=     0 <112<=     0 <128<=     0 <144<=     0
HugePageFiller: <160<=     0 <176<=     0 <192<=     0 <208<=     0 <224<=     0 <240<=     0
HugePageFiller: <248<=     0 <249<=     0 <250<=     0 <251<=     0 <252<=     0 <253<=     0
HugePageFiller: <254<=     0 <255<=     0
)"));

  EXPECT_THAT(buffer, testing::HasSubstr(R"(
HugePageFiller: # of densely-accessed released hps with live lifetime a <= # hps < b
HugePageFiller: <   0 ms <=      0 <   1 ms <=      0 <  10 ms <=      0 < 100 ms <=      0 < 1000 ms <=      0 < 10000 ms <=      0
HugePageFiller: < 100000 ms <=      0 < 1000000 ms <=      0

HugePageFiller: # of hps with >= 224 free pages, with different lifetimes.
HugePageFiller: # of densely-accessed released hps with lifetime a <= # hps < b
HugePageFiller: <   0 ms <=      0 <   1 ms <=      0 <  10 ms <=      0 < 100 ms <=      0 < 1000 ms <=      0 < 10000 ms <=      0
HugePageFiller: < 100000 ms <=      0 < 1000000 ms <=      0

HugePageFiller: # of hps with lifetime >= 100000 ms.
HugePageFiller: # of densely-accessed released hps with a <= # of allocations < b
HugePageFiller: <  0<=     0 <  1<=     0 <  2<=     0 <  3<=     0 <  4<=     0 <  5<=     0
HugePageFiller: <  6<=     0 <  7<=     0 <  8<=     0 < 16<=     0 < 32<=     0 < 48<=     0
HugePageFiller: < 64<=     0 < 80<=     0 < 96<=     0 <112<=     0 <128<=     0 <144<=     0
HugePageFiller: <160<=     0 <176<=     0 <192<=     0 <208<=     0 <224<=     0 <240<=     0
HugePageFiller: <248<=     0 <249<=     0 <250<=     0 <251<=     0 <252<=     0 <253<=     0
HugePageFiller: <254<=     0 <255<=     0
)"));

  FakeClock::Advance(absl::Seconds(101));
  {
    PageHeapSpinLockHolder l;
    Printer printer(&*buffer.begin(), buffer.size());
    filler_.Print(printer, true, pageflags);
  }

  EXPECT_THAT(buffer, testing::HasSubstr(R"(
HugePageFiller: # of sparsely-accessed regular hps with live lifetime a <= # hps < b
HugePageFiller: <   0 ms <=      0 <   1 ms <=      0 <  10 ms <=      0 < 100 ms <=      0 < 1000 ms <=      0 < 10000 ms <=      0
HugePageFiller: < 100000 ms <=      1 < 1000000 ms <=      0
)"));

  EXPECT_THAT(buffer, testing::HasSubstr(R"(
HugePageFiller: # of hps with >= 224 free pages, with different lifetimes.
HugePageFiller: # of sparsely-accessed regular hps with lifetime a <= # hps < b
HugePageFiller: <   0 ms <=      0 <   1 ms <=      0 <  10 ms <=      0 < 100 ms <=      0 < 1000 ms <=      0 < 10000 ms <=      0
HugePageFiller: < 100000 ms <=      0 < 1000000 ms <=      0
)"));

  EXPECT_THAT(buffer, testing::HasSubstr(R"(
HugePageFiller: # of hps with lifetime >= 100000 ms.
HugePageFiller: # of sparsely-accessed regular hps with a <= # of allocations < b
HugePageFiller: <  0<=     0 <  1<=     1 <  2<=     0 <  3<=     0 <  4<=     0 <  5<=     0
HugePageFiller: <  6<=     0 <  7<=     0 <  8<=     0 < 16<=     0 < 32<=     0 < 48<=     0
HugePageFiller: < 64<=     0 < 80<=     0 < 96<=     0 <112<=     0 <128<=     0 <144<=     0
HugePageFiller: <160<=     0 <176<=     0 <192<=     0 <208<=     0 <224<=     0 <240<=     0
HugePageFiller: <248<=     0 <249<=     0 <250<=     0 <251<=     0 <252<=     0 <253<=     0
HugePageFiller: <254<=     0 <255<=     0
)"));

  Delete(small_alloc);
  Delete(large_alloc);
}

TEST_P(FillerTest, CompletedLifetimeTelemetryTest) {
  // This test is sensitive to the number of pages per hugepage, as we are
  // printing raw stats.
  if (kPagesPerHugePage != Length(256)) {
    GTEST_SKIP();
  }

  FakePageFlags pageflags;
  const Length N = kPagesPerHugePage;
  SpanAllocInfo info_sparsely_accessed = {1, AccessDensityPrediction::kSparse};
  PAlloc alloc0 = AllocateWithSpanAllocInfo(N / 4, info_sparsely_accessed);
  SpanAllocInfo info_densely_accessed = {1, AccessDensityPrediction::kDense};
  PAlloc alloc1 = AllocateWithSpanAllocInfo(Length(1), info_densely_accessed);

  FakeClock::Advance(absl::Seconds(101));
  Delete(alloc0);
  FakeClock::Advance(absl::Seconds(1001));
  Delete(alloc1);
  std::string buffer(1024 * 1024, '\0');
  {
    PageHeapSpinLockHolder l;
    Printer printer(&*buffer.begin(), buffer.size());
    filler_.Print(printer, true, pageflags);
  }
  EXPECT_THAT(buffer, testing::HasSubstr(R"(
HugePageFiller: # of densely-accessed hps with completed lifetime a <= # hps < b
HugePageFiller: <   0 ms <=      0 <   1 ms <=      0 <  10 ms <=      0 < 100 ms <=      0 < 1000 ms <=      0 < 10000 ms <=      0
HugePageFiller: < 100000 ms <=      0 < 1000000 ms <=      1

HugePageFiller: # of sparsely-accessed hps with completed lifetime a <= # hps < b
HugePageFiller: <   0 ms <=      0 <   1 ms <=      0 <  10 ms <=      0 < 100 ms <=      0 < 1000 ms <=      0 < 10000 ms <=      0
HugePageFiller: < 100000 ms <=      1 < 1000000 ms <=      0
)"));
}

TEST_P(FillerTest, SkipSubreleaseDemandPeak) {
  // Tests that HugePageFiller can cap filler's short-term long-term
  // skip-subrelease mechanism using the demand measured by subrelease
  // intervals.

  const Length N = kPagesPerHugePage;

  // We trigger the demand such that short-term + long-term demand exceeds the
  // peak demand. We should be able to sub-release memory from the HugeFiller
  // up to the peak demand measured in the previous intervals.

  // min_demand = 0.75N, max_demand = 2.5N
  std::vector<PAlloc> peak1a = AllocateVector(3 * N / 4);
  ASSERT_TRUE(!peak1a.empty());
  std::vector<PAlloc> peak1b = AllocateVectorWithSpanAllocInfo(
      3 * N / 4, peak1a.front().span_alloc_info);
  std::vector<PAlloc> half1a =
      AllocateVectorWithSpanAllocInfo(N / 2, peak1a.front().span_alloc_info);
  std::vector<PAlloc> half1b =
      AllocateVectorWithSpanAllocInfo(N / 2, peak1a.front().span_alloc_info);
  EXPECT_EQ(filler_.used_pages(), 2 * N + N / 2);
  FakeClock::Advance(absl::Minutes(1));

  // min_demand = 2N, max_demand = 2.5N
  DeleteVector(half1b);
  std::vector<PAlloc> half1c =
      AllocateVectorWithSpanAllocInfo(N / 2, peak1a.front().span_alloc_info);
  EXPECT_EQ(filler_.used_pages(), 2 * N + N / 2);
  EXPECT_EQ(filler_.free_pages(), N / 2);
  FakeClock::Advance(absl::Minutes(1));

  // At this point, short-term fluctuation, which is the maximum of the
  // difference between max_demand and min_demand in the previous two
  // intervals, is equal to 1.75N. Long-term demand, which is the maximum of
  // min_demand in the previous two intervals, is 2N. As peak demand of 2.5N
  // is lower than 3.75N, we should be able to subrelease 0.5N pages.
  EXPECT_EQ(Length(N / 2),
            ReleasePages(10 * N, SkipSubreleaseIntervals{
                                     .short_interval = absl::Minutes(2),
                                     .long_interval = absl::Minutes(2)}));
  DeleteVector(peak1a);
  DeleteVector(peak1b);
  DeleteVector(half1a);
  DeleteVector(half1c);
}

TEST_P(FillerTest, ReportSkipSubreleases) {
  // Tests that HugePageFiller reports skipped subreleases using demand
  // requirement that is the smaller of two (recent peak and its
  // current capacity). This fix makes evaluating skip subrelease more
  // accurate, which is useful for cross-comparing performance of different
  // skip-subrelease intervals.

  // This test is sensitive to the number of pages per hugepage, as we are
  // printing raw stats.
  if (kPagesPerHugePage != Length(256)) {
    GTEST_SKIP();
  }

  FakePageFlags pageflags;
  const Length N = kPagesPerHugePage;
  // Reports skip subrelease using the recent demand peak (2.5N): it is
  // smaller than the total number of pages (3N) when 0.25N free pages are
  // skipped.  The skipping is correct as the future demand is 2.5N.
  SpanAllocInfo info = {1, AccessDensityPrediction::kSparse};
  PAlloc peak1a = AllocateWithSpanAllocInfo(3 * N / 4, info);
  PAlloc peak1b = AllocateWithSpanAllocInfo(N / 4, info);
  PAlloc peak2a = AllocateWithSpanAllocInfo(3 * N / 4, info);
  PAlloc peak2b = AllocateWithSpanAllocInfo(N / 4, info);
  PAlloc half1 = AllocateWithSpanAllocInfo(N / 2, info);
  FakeClock::Advance(absl::Minutes(2));
  Delete(half1);
  Delete(peak1b);
  Delete(peak2b);
  PAlloc peak3a = AllocateWithSpanAllocInfo(3 * N / 4, info);
  EXPECT_EQ(filler_.free_pages(), 3 * N / 4);
  // Subreleases 0.5N free pages and skips 0.25N free pages.
  EXPECT_EQ(N / 2,
            ReleasePages(10 * N, SkipSubreleaseIntervals{
                                     .peak_interval = absl::Minutes(3)}));
  FakeClock::Advance(absl::Minutes(3));
  PAlloc tiny1 = AllocateWithSpanAllocInfo(N / 4, info);
  EXPECT_EQ(filler_.used_pages(), 2 * N + N / 2);
  EXPECT_EQ(filler_.unmapped_pages(), N / 2);
  EXPECT_EQ(filler_.free_pages(), Length(0));
  Delete(peak1a);
  Delete(peak2a);
  Delete(peak3a);
  Delete(tiny1);
  EXPECT_EQ(filler_.used_pages(), Length(0));
  EXPECT_EQ(filler_.unmapped_pages(), Length(0));
  EXPECT_EQ(filler_.free_pages(), Length(0));
  // Accounts for pages that are eagerly unmapped (unmapping_unaccounted_).
  EXPECT_EQ(N + N / 2, ReleasePages(10 * N));

  FakeClock::Advance(absl::Minutes(30));

  // Reports skip subrelease using HugePageFiller's capacity (N pages): it is
  // smaller than the recent peak (2N) when 0.5N pages are skipped. They are
  // correctly skipped as the future demand is N.
  PAlloc peak4a = AllocateWithSpanAllocInfo(3 * N / 4, info);
  PAlloc peak4b = AllocateWithSpanAllocInfo(N / 4, info);
  PAlloc peak5a = AllocateWithSpanAllocInfo(3 * N / 4, info);
  PAlloc peak5b = AllocateWithSpanAllocInfo(N / 4, info);
  FakeClock::Advance(absl::Minutes(2));
  Delete(peak4a);
  Delete(peak4b);
  Delete(peak5a);
  Delete(peak5b);
  PAlloc half2 = AllocateWithSpanAllocInfo(N / 2, info);
  EXPECT_EQ(Length(0),
            ReleasePages(10 * N, SkipSubreleaseIntervals{
                                     .peak_interval = absl::Minutes(3)}));
  FakeClock::Advance(absl::Minutes(3));
  PAlloc half3 = AllocateWithSpanAllocInfo(N / 2, info);
  Delete(half2);
  Delete(half3);
  EXPECT_EQ(filler_.used_pages(), Length(0));
  EXPECT_EQ(filler_.unmapped_pages(), Length(0));
  EXPECT_EQ(filler_.free_pages(), Length(0));
  EXPECT_EQ(Length(0), ReleasePages(10 * N));
  FakeClock::Advance(absl::Minutes(30));
  //  Ensures that the tracker is updated.
  auto tiny2 = Allocate(Length(1));
  Delete(tiny2);

  std::string buffer(1024 * 1024, '\0');
  {
    PageHeapSpinLockHolder l;
    Printer printer(&*buffer.begin(), buffer.size());
    filler_.Print(printer, true, pageflags);
  }
  buffer.resize(strlen(buffer.c_str()));

  EXPECT_THAT(buffer, testing::HasSubstr(R"(
HugePageFiller: Since the start of the execution, 2 subreleases (192 pages) were skipped due to either recent (180s) peaks, or the sum of short-term (0s) fluctuations and long-term (0s) trends.
HugePageFiller: 100.0000% of decisions confirmed correct, 0 pending (100.0000% of pages, 0 pending), as per anticipated 300s realized fragmentation.
)"));
}

TEST_P(FillerTest, ReportSkipSubreleases_SpansAllocated) {
  // Tests that HugePageFiller reports skipped subreleases using demand
  // requirement that is the smaller of two (recent peak and its
  // current capacity). This fix makes evaluating skip subrelease more
  // accurate, which is useful for cross-comparing performance of different
  // skip-subrelease intervals.

  // This test is sensitive to the number of pages per hugepage, as we are
  // printing raw stats.
  if (kPagesPerHugePage != Length(256)) {
    GTEST_SKIP();
  }
  randomize_density_ = false;
  const Length N = kPagesPerHugePage;
  SpanAllocInfo info = {kPagesPerHugePage.raw_num(),
                        AccessDensityPrediction::kDense};
  // Reports skip subrelease using the recent demand peak (2.5N): it is
  // smaller than the total number of pages (3N) when 0.25N free pages are
  // skipped.  The skipping is correct as the future demand is 2.5N.
  std::vector<PAlloc> peak1a = AllocateVectorWithSpanAllocInfo(3 * N / 4, info);
  ASSERT_TRUE(!peak1a.empty());
  std::vector<PAlloc> peak1b =
      AllocateVectorWithSpanAllocInfo(N / 4, peak1a.front().span_alloc_info);
  std::vector<PAlloc> peak2a = AllocateVectorWithSpanAllocInfo(
      3 * N / 4, peak1a.front().span_alloc_info);
  std::vector<PAlloc> peak2b =
      AllocateVectorWithSpanAllocInfo(N / 4, peak1a.front().span_alloc_info);
  std::vector<PAlloc> half1 =
      AllocateVectorWithSpanAllocInfo(N / 2, peak1a.front().span_alloc_info);
  FakeClock::Advance(absl::Minutes(2));
  DeleteVector(half1);
  DeleteVector(peak1b);
  DeleteVector(peak2b);
  std::vector<PAlloc> peak3a = AllocateVectorWithSpanAllocInfo(
      3 * N / 4, peak1a.front().span_alloc_info);
  EXPECT_EQ(filler_.free_pages(), 3 * N / 4);
  // Subreleases 0.75N free pages.
  EXPECT_EQ(3 * N / 4,
            ReleasePages(10 * N, SkipSubreleaseIntervals{
                                     .peak_interval = absl::Minutes(3)}));
  FakeClock::Advance(absl::Minutes(3));
  std::vector<PAlloc> tiny1 =
      AllocateVectorWithSpanAllocInfo(N / 4, peak1a.front().span_alloc_info);
  EXPECT_EQ(filler_.used_pages(), 2 * N + N / 2);
  EXPECT_EQ(filler_.unmapped_pages(), N / 2);
  EXPECT_EQ(filler_.free_pages(), Length(0));
  DeleteVector(peak1a);
  DeleteVector(peak2a);
  DeleteVector(peak3a);
  DeleteVector(tiny1);
  EXPECT_EQ(filler_.used_pages(), Length(0));
  EXPECT_EQ(filler_.unmapped_pages(), Length(0));
  EXPECT_EQ(filler_.free_pages(), Length(0));
  // Accounts for pages that are eagerly unmapped (unmapping_unaccounted_).
  EXPECT_EQ(N / 2, ReleasePages(10 * N));

  FakeClock::Advance(absl::Minutes(30));

  // Reports skip subrelease using HugePageFiller's capacity (N pages): it is
  // smaller than the recent peak (2N) when 0.5N pages are skipped. They are
  // correctly skipped as the future demand is N.
  std::vector<PAlloc> peak4a = AllocateVectorWithSpanAllocInfo(3 * N / 4, info);
  ASSERT_TRUE(!peak4a.empty());
  std::vector<PAlloc> peak4b =
      AllocateVectorWithSpanAllocInfo(N / 4, peak4a.front().span_alloc_info);
  std::vector<PAlloc> peak5a = AllocateVectorWithSpanAllocInfo(3 * N / 4, info);
  ASSERT_TRUE(!peak5a.empty());
  std::vector<PAlloc> peak5b =
      AllocateVectorWithSpanAllocInfo(N / 4, peak5a.front().span_alloc_info);
  FakeClock::Advance(absl::Minutes(2));
  DeleteVector(peak4a);
  DeleteVector(peak4b);
  DeleteVector(peak5a);
  DeleteVector(peak5b);
  std::vector<PAlloc> half2 = AllocateVectorWithSpanAllocInfo(N / 2, info);
  EXPECT_EQ(Length(0),
            ReleasePages(10 * N, SkipSubreleaseIntervals{
                                     .peak_interval = absl::Minutes(3)}));
  FakeClock::Advance(absl::Minutes(3));
  std::vector<PAlloc> half3 = AllocateVectorWithSpanAllocInfo(N / 2, info);
  DeleteVector(half2);
  DeleteVector(half3);
  EXPECT_EQ(filler_.used_pages(), Length(0));
  EXPECT_EQ(filler_.unmapped_pages(), Length(0));
  EXPECT_EQ(filler_.free_pages(), Length(0));
  EXPECT_EQ(Length(0), ReleasePages(10 * N));
  FakeClock::Advance(absl::Minutes(30));
  //  Ensures that the tracker is updated.
  auto tiny2 = Allocate(Length(1));
  Delete(tiny2);

  std::string buffer(1024 * 1024, '\0');
  FakePageFlags pageflags;
  {
    PageHeapSpinLockHolder l;
    Printer printer(&*buffer.begin(), buffer.size());
    filler_.Print(printer, true, pageflags);
  }
  buffer.resize(strlen(buffer.c_str()));

  EXPECT_THAT(buffer, testing::HasSubstr(R"(
HugePageFiller: Since the start of the execution, 2 subreleases (192 pages) were skipped due to either recent (180s) peaks, or the sum of short-term (0s) fluctuations and long-term (0s) trends.
HugePageFiller: 0.0000% of decisions confirmed correct, 0 pending (0.0000% of pages, 0 pending), as per anticipated 300s realized fragmentation.
)"));
}

std::vector<FillerTest::PAlloc> FillerTest::GenerateInterestingAllocs() {
  SpanAllocInfo info_sparsely_accessed = {1, AccessDensityPrediction::kSparse};
  SpanAllocInfo info_densely_accessed = {kMaxValidPages.raw_num(),
                                         AccessDensityPrediction::kDense};
  PAlloc a = AllocateWithSpanAllocInfo(Length(1), info_sparsely_accessed);
  EXPECT_EQ(ReleasePages(kMaxValidPages), kPagesPerHugePage - Length(1));
  Delete(a);
  // Get the report on the released page
  EXPECT_EQ(ReleasePages(kMaxValidPages), Length(1));

  // Use a maximally-suboptimal pattern to get lots of hugepages into the
  // filler.
  std::vector<PAlloc> result;
  static_assert(kPagesPerHugePage > Length(7),
                "Not enough pages per hugepage!");
  for (auto i = Length(0); i < Length(7); ++i) {
    std::vector<PAlloc> temp = AllocateVectorWithSpanAllocInfo(
        kPagesPerHugePage - i - Length(1), info_sparsely_accessed);
    result.insert(result.end(), temp.begin(), temp.end());
    temp = AllocateVectorWithSpanAllocInfo(kPagesPerHugePage - i - Length(1),
                                           info_densely_accessed);
    result.insert(result.end(), temp.begin(), temp.end());
  }

  // Get released hugepages.
  Length l = ReleasePages(Length(7));
  EXPECT_TRUE(l == Length(7) || l == Length(28));
  l = ReleasePages(Length(7));
  EXPECT_EQ(l, Length(7));
  l = ReleasePages(Length(6));
  EXPECT_EQ(l, Length(6));
  l = ReleasePages(Length(6));
  EXPECT_TRUE(l == Length(6) || l == Length(9));

  // Fill some of the remaining pages with small allocations.
  for (int i = 0; i < 9; ++i) {
    result.push_back(
        AllocateWithSpanAllocInfo(Length(1), info_sparsely_accessed));
    result.push_back(
        AllocateWithSpanAllocInfo(Length(1), info_densely_accessed));
  }

  // Finally, donate one hugepage.
  result.push_back(AllocateWithSpanAllocInfo(Length(1), info_sparsely_accessed,
                                             /*donated=*/true));
  return result;
}

// Testing subrelase stats: ensure that the cumulative number of released
// pages and broken hugepages is no less than those of the last 10 mins
TEST_P(FillerTest, CheckSubreleaseStats) {
  // Get lots of hugepages into the filler.
  FakeClock::Advance(absl::Minutes(1));
  std::vector<std::vector<PAlloc>> result;
  static_assert(kPagesPerHugePage > Length(10),
                "Not enough pages per hugepage!");
  // Fix the object count since very specific statistics are being tested.
  const AccessDensityPrediction kDensity =
      absl::Bernoulli(gen_, 0.5) ? AccessDensityPrediction::kSparse
                                 : AccessDensityPrediction::kDense;
  SCOPED_TRACE(absl::StrCat("AccessDensityPrediction: ", kDensity));
  const size_t kObjects = (1 << absl::Uniform<size_t>(gen_, 0, 8));
  const SpanAllocInfo kAllocInfo = {kObjects, kDensity};

  for (int i = 0; i < 10; ++i) {
    result.push_back(AllocateVectorWithSpanAllocInfo(
        kPagesPerHugePage - Length(i + 1), kAllocInfo));
  }

  if (kDensity == AccessDensityPrediction::kSparse) {
    // Breaking up 2 hugepages, releasing 19 pages due to reaching limit,
    EXPECT_EQ(HardReleasePages(Length(10)), Length(10));
    EXPECT_EQ(HardReleasePages(Length(9)), Length(9));
  } else {
    // Breaking 1 hugepage, releasing 45 pages due to reaching limit,
    EXPECT_EQ(HardReleasePages(Length(10)), Length(55));
    EXPECT_EQ(HardReleasePages(Length(9)), Length(0));
  }

  FakeClock::Advance(absl::Minutes(1));
  SubreleaseStats subrelease = filler_.subrelease_stats();
  EXPECT_EQ(subrelease.total_pages_subreleased, Length(0));
  EXPECT_EQ(subrelease.total_partial_alloc_pages_subreleased, Length(0));
  EXPECT_EQ(subrelease.total_hugepages_broken.raw_num(), 0);
  if (kDensity == AccessDensityPrediction::kSparse) {
    EXPECT_EQ(subrelease.num_pages_subreleased, Length(19));
    EXPECT_EQ(subrelease.num_hugepages_broken.raw_num(), 2);
    EXPECT_EQ(subrelease.total_pages_subreleased_due_to_limit, Length(19));
    EXPECT_EQ(subrelease.total_hugepages_broken_due_to_limit.raw_num(), 2);
  } else {
    EXPECT_EQ(subrelease.num_pages_subreleased, Length(55));
    EXPECT_EQ(subrelease.num_hugepages_broken.raw_num(), 1);
    EXPECT_EQ(subrelease.total_pages_subreleased_due_to_limit, Length(55));
    EXPECT_EQ(subrelease.total_hugepages_broken_due_to_limit.raw_num(), 1);
  }

  // Do some work so that the timeseries updates its stats
  for (int i = 0; i < 5; ++i) {
    result.push_back(AllocateVectorWithSpanAllocInfo(Length(1), kAllocInfo));
  }
  subrelease = filler_.subrelease_stats();
  if (kDensity == AccessDensityPrediction::kSparse) {
    EXPECT_EQ(subrelease.total_pages_subreleased, Length(19));
    EXPECT_EQ(subrelease.total_hugepages_broken.raw_num(), 2);
    EXPECT_EQ(subrelease.total_pages_subreleased_due_to_limit, Length(19));
    EXPECT_EQ(subrelease.total_hugepages_broken_due_to_limit.raw_num(), 2);
  } else {
    EXPECT_EQ(subrelease.total_pages_subreleased, Length(55));
    EXPECT_EQ(subrelease.total_hugepages_broken.raw_num(), 1);
    EXPECT_EQ(subrelease.total_pages_subreleased_due_to_limit, Length(55));
    EXPECT_EQ(subrelease.total_hugepages_broken_due_to_limit.raw_num(), 1);
  }
  EXPECT_EQ(subrelease.total_partial_alloc_pages_subreleased, Length(0));
  EXPECT_EQ(subrelease.num_pages_subreleased, Length(0));
  EXPECT_EQ(subrelease.num_hugepages_broken.raw_num(), 0);

  // Breaking up 3 hugepages, releasing 21 pages (background thread)
  if (kDensity == AccessDensityPrediction::kSparse) {
    EXPECT_EQ(ReleasePages(Length(8)), Length(8));
    EXPECT_EQ(ReleasePages(Length(7)), Length(7));
    EXPECT_EQ(ReleasePages(Length(6)), Length(6));
  } else {
    EXPECT_EQ(ReleasePages(Length(8)), Length(0));
    EXPECT_EQ(ReleasePages(Length(7)), Length(0));
    EXPECT_EQ(ReleasePages(Length(6)), Length(0));
  }

  subrelease = filler_.subrelease_stats();
  if (kDensity == AccessDensityPrediction::kSparse) {
    EXPECT_EQ(subrelease.total_pages_subreleased, Length(19));
    EXPECT_EQ(subrelease.total_partial_alloc_pages_subreleased, Length(0));
    EXPECT_EQ(subrelease.total_hugepages_broken.raw_num(), 2);
    EXPECT_EQ(subrelease.num_pages_subreleased, Length(21));
    EXPECT_EQ(subrelease.num_hugepages_broken.raw_num(), 3);
    EXPECT_EQ(subrelease.total_pages_subreleased_due_to_limit, Length(19));
    EXPECT_EQ(subrelease.total_hugepages_broken_due_to_limit.raw_num(), 2);
  }

  FakeClock::Advance(absl::Minutes(10));  // This forces timeseries to wrap
  // Do some work
  for (int i = 0; i < 5; ++i) {
    result.push_back(AllocateVectorWithSpanAllocInfo(Length(1), kAllocInfo));
  }
  subrelease = filler_.subrelease_stats();
  if (kDensity == AccessDensityPrediction::kSparse) {
    EXPECT_EQ(subrelease.total_pages_subreleased, Length(40));
    EXPECT_EQ(subrelease.total_partial_alloc_pages_subreleased, Length(0));
    EXPECT_EQ(subrelease.total_hugepages_broken.raw_num(), 5);
    EXPECT_EQ(subrelease.num_pages_subreleased, Length(0));
    EXPECT_EQ(subrelease.num_hugepages_broken.raw_num(), 0);
    EXPECT_EQ(subrelease.total_pages_subreleased_due_to_limit, Length(19));
    EXPECT_EQ(subrelease.total_hugepages_broken_due_to_limit.raw_num(), 2);
  } else {
    EXPECT_EQ(subrelease.total_pages_subreleased, Length(55));
    EXPECT_EQ(subrelease.total_partial_alloc_pages_subreleased, Length(0));
    EXPECT_EQ(subrelease.total_hugepages_broken.raw_num(), 1);
    EXPECT_EQ(subrelease.num_pages_subreleased, Length(0));
    EXPECT_EQ(subrelease.num_hugepages_broken.raw_num(), 0);
    EXPECT_EQ(subrelease.total_pages_subreleased_due_to_limit, Length(55));
    EXPECT_EQ(subrelease.total_hugepages_broken_due_to_limit.raw_num(), 1);
  }

  std::string buffer(1024 * 1024, '\0');
  FakePageFlags pageflags;
  {
    PageHeapSpinLockHolder l;
    Printer printer(&*buffer.begin(), buffer.size());
    filler_.Print(printer, /*everything=*/true, pageflags);
    buffer.erase(printer.SpaceRequired());
  }

  if (kDensity == AccessDensityPrediction::kSparse) {
    ASSERT_THAT(buffer,
                testing::HasSubstr(
                    "HugePageFiller: Since startup, 40 pages subreleased, 5 "
                    "hugepages "
                    "broken, (19 pages, 2 hugepages due to reaching tcmalloc "
                    "limit)"));
    ASSERT_THAT(
        buffer,
        testing::EndsWith("HugePageFiller: Subrelease stats last 10 min: total "
                          "21 pages subreleased (0 pages from partial allocs), "
                          "3 hugepages broken\n"));
  }

  for (const auto& alloc : result) {
    DeleteVector(alloc);
  }
}

TEST_P(FillerTest, CheckSubreleaseStats_SpansAllocated) {
  randomize_density_ = false;
  // Get lots of hugepages into the filler.
  FakeClock::Advance(absl::Minutes(1));
  std::vector<std::vector<PAlloc>> result;
  std::vector<std::vector<PAlloc>> temporary;
  static_assert(kPagesPerHugePage > Length(10),
                "Not enough pages per hugepage!");
  // Fix the object count since very specific statistics are being tested.
  const AccessDensityPrediction kDensity =
      absl::Bernoulli(gen_, 0.5) ? AccessDensityPrediction::kSparse
                                 : AccessDensityPrediction::kDense;
  const size_t kObjects = (1 << absl::Uniform<size_t>(gen_, 0, 8));
  const SpanAllocInfo kAllocInfo = {kObjects, kDensity};

  for (int i = 0; i < 10; ++i) {
    result.push_back(AllocateVectorWithSpanAllocInfo(
        kPagesPerHugePage - Length(i + 1), kAllocInfo));
    temporary.push_back(
        AllocateVectorWithSpanAllocInfo(Length(i + 1), kAllocInfo));
  }
  for (const auto& alloc : temporary) {
    DeleteVector(alloc);
  }

  // Breaking up 2 hugepages, releasing 19 pages due to reaching limit,
  EXPECT_EQ(HardReleasePages(Length(10)), Length(10));
  EXPECT_EQ(HardReleasePages(Length(9)), Length(9));

  FakeClock::Advance(absl::Minutes(1));
  SubreleaseStats subrelease = filler_.subrelease_stats();
  EXPECT_EQ(subrelease.total_pages_subreleased, Length(0));
  EXPECT_EQ(subrelease.total_partial_alloc_pages_subreleased, Length(0));
  EXPECT_EQ(subrelease.total_hugepages_broken.raw_num(), 0);
  EXPECT_EQ(subrelease.num_pages_subreleased, Length(19));
  EXPECT_EQ(subrelease.num_hugepages_broken.raw_num(), 2);
  EXPECT_EQ(subrelease.total_pages_subreleased_due_to_limit, Length(19));
  EXPECT_EQ(subrelease.total_hugepages_broken_due_to_limit.raw_num(), 2);

  // Do some work so that the timeseries updates its stats
  for (int i = 0; i < 5; ++i) {
    result.push_back(AllocateVectorWithSpanAllocInfo(Length(1), kAllocInfo));
  }
  subrelease = filler_.subrelease_stats();
  EXPECT_EQ(subrelease.total_pages_subreleased, Length(19));
  EXPECT_EQ(subrelease.total_partial_alloc_pages_subreleased, Length(0));
  EXPECT_EQ(subrelease.total_hugepages_broken.raw_num(), 2);
  EXPECT_EQ(subrelease.num_pages_subreleased, Length(0));
  EXPECT_EQ(subrelease.num_hugepages_broken.raw_num(), 0);
  EXPECT_EQ(subrelease.total_pages_subreleased_due_to_limit, Length(19));
  EXPECT_EQ(subrelease.total_hugepages_broken_due_to_limit.raw_num(), 2);

  // Breaking up 3 hugepages, releasing 21 pages (background thread)
  EXPECT_EQ(ReleasePages(Length(8)), Length(8));
  EXPECT_EQ(ReleasePages(Length(7)), Length(7));
  EXPECT_EQ(ReleasePages(Length(6)), Length(6));

  subrelease = filler_.subrelease_stats();
  EXPECT_EQ(subrelease.total_pages_subreleased, Length(19));
  EXPECT_EQ(subrelease.total_partial_alloc_pages_subreleased, Length(0));
  EXPECT_EQ(subrelease.total_hugepages_broken.raw_num(), 2);
  EXPECT_EQ(subrelease.num_pages_subreleased, Length(21));
  EXPECT_EQ(subrelease.num_hugepages_broken.raw_num(), 3);
  EXPECT_EQ(subrelease.total_pages_subreleased_due_to_limit, Length(19));
  EXPECT_EQ(subrelease.total_hugepages_broken_due_to_limit.raw_num(), 2);

  FakeClock::Advance(absl::Minutes(10));  // This forces timeseries to wrap
  // Do some work
  for (int i = 0; i < 5; ++i) {
    result.push_back(AllocateVectorWithSpanAllocInfo(Length(1), kAllocInfo));
  }
  subrelease = filler_.subrelease_stats();
  EXPECT_EQ(subrelease.total_pages_subreleased, Length(40));
  EXPECT_EQ(subrelease.total_partial_alloc_pages_subreleased, Length(0));
  EXPECT_EQ(subrelease.total_hugepages_broken.raw_num(), 5);
  EXPECT_EQ(subrelease.num_pages_subreleased, Length(0));
  EXPECT_EQ(subrelease.num_hugepages_broken.raw_num(), 0);
  EXPECT_EQ(subrelease.total_pages_subreleased_due_to_limit, Length(19));
  EXPECT_EQ(subrelease.total_hugepages_broken_due_to_limit.raw_num(), 2);

  std::string buffer(1024 * 1024, '\0');
  FakePageFlags pageflags;
  {
    PageHeapSpinLockHolder l;
    Printer printer(&*buffer.begin(), buffer.size());
    filler_.Print(printer, /*everything=*/true, pageflags);
    buffer.erase(printer.SpaceRequired());
  }

  ASSERT_THAT(
      buffer,
      testing::HasSubstr(
          "HugePageFiller: Since startup, 40 pages subreleased, 5 hugepages "
          "broken, (19 pages, 2 hugepages due to reaching tcmalloc "
          "limit)"));
  ASSERT_THAT(buffer, testing::EndsWith(
                          "HugePageFiller: Subrelease stats last 10 min: total "
                          "21 pages subreleased (0 pages from partial allocs), "
                          "3 hugepages broken\n"));

  for (const auto& alloc : result) {
    DeleteVector(alloc);
  }
}

TEST_P(FillerTest, ConstantBrokenHugePages) {
  // Get and Fill up many huge pages
  const HugeLength kHugePages = NHugePages(10 * kPagesPerHugePage.raw_num());

  absl::BitGen rng;
  std::vector<PAlloc> alloc;
  alloc.reserve(kHugePages.raw_num());
  std::vector<PAlloc> dead;
  dead.reserve(kHugePages.raw_num());
  std::vector<PAlloc> alloc_small;
  alloc_small.reserve(kHugePages.raw_num() + 2);

  for (HugeLength i; i < kHugePages; ++i) {
    auto size =
        Length(absl::Uniform<size_t>(rng, 2, kPagesPerHugePage.raw_num() - 1));
    alloc_small.push_back(Allocate(Length(1)));
    SpanAllocInfo info = alloc_small.back().span_alloc_info;
    std::vector<PAlloc> temp =
        AllocateVectorWithSpanAllocInfo(size - Length(1), info);
    alloc.insert(alloc.end(), temp.begin(), temp.end());
    temp = AllocateVectorWithSpanAllocInfo(kPagesPerHugePage - size, info);
    dead.insert(dead.end(), temp.begin(), temp.end());
  }
  ASSERT_EQ(filler_.size(), kHugePages);

  for (int i = 0; i < 2; ++i) {
    for (auto& a : dead) {
      Delete(a);
    }
    ReleasePages(filler_.free_pages());
    ASSERT_EQ(filler_.free_pages(), Length(0));
    alloc_small.push_back(
        Allocate(Length(1)));  // To force subrelease stats to update

    std::string buffer(1024 * 1024, '\0');
    FakePageFlags pageflags;
    {
      PageHeapSpinLockHolder l;
      Printer printer(&*buffer.begin(), buffer.size());
      filler_.Print(printer, /*everything=*/false, pageflags);
      buffer.erase(printer.SpaceRequired());
    }

    ASSERT_THAT(buffer, testing::HasSubstr(absl::StrCat(kHugePages.raw_num(),
                                                        " hugepages broken")));
    if (i == 1) {
      // Number of pages in alloc_small
      ASSERT_THAT(buffer, testing::HasSubstr(absl::StrCat(
                              kHugePages.raw_num() + 2,
                              " used pages in subreleased hugepages")));
      // Sum of pages in alloc and dead
      ASSERT_THAT(buffer,
                  testing::HasSubstr(absl::StrCat(
                      kHugePages.raw_num() * kPagesPerHugePage.raw_num() -
                          kHugePages.raw_num(),
                      " pages subreleased")));
    }

    dead.swap(alloc);
    alloc.clear();
  }

  // Clean up
  for (auto& a : alloc_small) {
    Delete(a);
  }
}

// Confirms that a timeseries that contains every epoch does not exceed the
// expected buffer capacity of 1 MiB.
TEST_P(FillerTest, CheckBufferSize) {
  const int kEpochs = 600;
  const absl::Duration kEpochLength = absl::Seconds(1);
  std::vector<PAlloc> big = AllocateVector(kPagesPerHugePage - Length(4));

  for (int i = 0; i < kEpochs; i += 2) {
    auto tiny = AllocateVector(Length(2));
    FakeClock::Advance(kEpochLength);
    DeleteVector(tiny);
    FakeClock::Advance(kEpochLength);
  }

  DeleteVector(big);

  std::string buffer(1024 * 1024, '\0');
  Printer printer(&*buffer.begin(), buffer.size());
  FakePageFlags pageflags;
  {
    PageHeapSpinLockHolder l;
    PbtxtRegion region(printer, kTop);
    filler_.PrintInPbtxt(region, pageflags);
  }

  // We assume a maximum buffer size of 1 MiB. When increasing this size,
  // ensure that all places processing mallocz protos get updated as well.
  size_t buffer_size = printer.SpaceRequired();
  EXPECT_LE(buffer_size, 1024 * 1024);
}

TEST_P(FillerTest, ReleasePriority) {
  // Fill up many huge pages (>> kPagesPerHugePage).  This relies on an
  // implementation detail of ReleasePages buffering up at most
  // kPagesPerHugePage as potential release candidates.
  const HugeLength kHugePages = NHugePages(10 * kPagesPerHugePage.raw_num());

  // We will ensure that we fill full huge pages, then deallocate some parts
  // of those to provide space for subrelease.
  absl::BitGen rng;
  std::vector<std::vector<PAlloc>> alloc;
  alloc.reserve(kHugePages.raw_num());
  std::vector<std::vector<PAlloc>> dead;
  dead.reserve(kHugePages.raw_num());

  absl::flat_hash_set<PageTracker*> unique_pages;
  unique_pages.reserve(kHugePages.raw_num());

  for (HugeLength i; i < kHugePages; ++i) {
    Length size(absl::Uniform<size_t>(rng, 1, kPagesPerHugePage.raw_num() - 1));

    std::vector<PAlloc> a = AllocateVector(size);
    ASSERT_TRUE(!a.empty());
    for (const auto& pa : a) unique_pages.insert(pa.pt);
    alloc.push_back(a);
    dead.push_back(AllocateVectorWithSpanAllocInfo(kPagesPerHugePage - size,
                                                   a.front().span_alloc_info));
  }

  ASSERT_EQ(filler_.size(), kHugePages);

  for (auto& a : dead) {
    DeleteVector(a);
  }

  // As of 5/2020, our release priority is to subrelease huge pages with the
  // fewest used pages.  Bucket unique_pages by that used_pages().
  std::vector<std::vector<PageTracker*>> ordered(kPagesPerHugePage.raw_num());
  for (auto* pt : unique_pages) {
    // None of these should be released yet.
    EXPECT_FALSE(pt->released());
    ordered[pt->used_pages().raw_num()].push_back(pt);
  }

  // Iteratively release random amounts of free memory--until all free pages
  // become unmapped pages--and validate that we followed the expected release
  // priority.
  Length free_pages;
  while ((free_pages = filler_.free_pages()) > Length(0)) {
    Length to_release(absl::LogUniform<size_t>(rng, 1, free_pages.raw_num()));
    Length released = ReleasePages(to_release);
    ASSERT_LE(released, free_pages);

    // Iterate through each element of ordered.  If any trackers are released,
    // all previous trackers must be released.
    bool previous_all_released = true;
    for (auto l = Length(0); l < kPagesPerHugePage; ++l) {
      bool any_released = false;
      bool all_released = true;

      for (auto* pt : ordered[l.raw_num()]) {
        bool released = pt->released();

        any_released |= released;
        all_released &= released;
      }

      if (any_released) {
        EXPECT_TRUE(previous_all_released) << [&]() {
          // On mismatch, print the bitmap of released states on l-1/l.
          std::vector<bool> before;
          if (l > Length(0)) {
            before.reserve(ordered[l.raw_num() - 1].size());
            for (auto* pt : ordered[l.raw_num() - 1]) {
              before.push_back(pt->released());
            }
          }

          std::vector<bool> after;
          after.reserve(ordered[l.raw_num()].size());
          for (auto* pt : ordered[l.raw_num()]) {
            after.push_back(pt->released());
          }

          return absl::StrCat("before = {", absl::StrJoin(before, ";"),
                              "}\nafter  = {", absl::StrJoin(after, ";"), "}");
        }();
      }

      previous_all_released = all_released;
    }
  }

  // All huge pages should be released.
  for (auto* pt : unique_pages) {
    EXPECT_TRUE(pt->released());
  }

  for (auto& a : alloc) {
    DeleteVector(a);
  }
}

TEST_P(FillerTest, b258965495) {
  // 1 huge page:  2 pages allocated, kPagesPerHugePage-2 free, 0 released
  auto a1 = AllocateVector(Length(2));
  ASSERT_TRUE(!a1.empty());
  EXPECT_EQ(filler_.size(), NHugePages(1));

  ASSERT_TRUE(blocking_unback_.success_);
  // 1 huge page:  2 pages allocated, 0 free, kPagesPerHugePage-2 released
  EXPECT_EQ(HardReleasePages(kPagesPerHugePage), kPagesPerHugePage - Length(2));

  blocking_unback_.success_ = false;
  // 1 huge page:  3 pages allocated, 0 free, kPagesPerHugePage-3 released
  auto a2 = AllocateWithSpanAllocInfo(Length(1), a1.front().span_alloc_info);
  EXPECT_EQ(filler_.size(), NHugePages(1));
  // Even if PartialRerelease::Return, returning a2 fails, so a2's pages stay
  // freed rather than released.
  //
  // 1 huge page:  2 pages allocated, 1 free, kPagesPerHugePage-3 released
  Delete(a2);

  blocking_unback_.success_ = true;
  // During the deallocation of a1 under PartialRerelease::Return, but before
  // we mark the pages as free (PageTracker::MaybeRelease), we have:
  //
  // 1 huge page:  2 pages allocated, 1 free, kPagesPerHugePage-1 released
  //
  // The page appears fully (free_pages() <= released_pages()), rather than
  // partially released, so we look for it on the wrong list.
  DeleteVector(a1);
}

TEST_P(FillerTest, CheckFillerStats) {
  if (kPagesPerHugePage != Length(256)) {
    // The output is hardcoded on this assumption, and dynamically calculating
    // it would be way too much of a pain.
    return;
  }
  // We prevent randomly choosing the number of objects per span since this
  // test has hardcoded output which will change if the objects per span are
  // chosen at random.
  randomize_density_ = false;
  auto allocs = GenerateInterestingAllocs();

  const HugePageFillerStats stats = filler_.GetStats();
  for (int i = 0; i < AccessDensityPrediction::kPredictionCounts; ++i) {
    EXPECT_GE(stats.n_fully_released[i].raw_num(), 0);
  }
  // Check sparsely-accessed filler stats.
  EXPECT_EQ(stats.n_fully_released[AccessDensityPrediction::kSparse].raw_num(),
            4);
  EXPECT_EQ(stats.n_released[AccessDensityPrediction::kSparse].raw_num(), 4);
  EXPECT_EQ(
      stats.n_partial_released[AccessDensityPrediction::kSparse].raw_num(), 0);
  EXPECT_EQ(stats.n_total[AccessDensityPrediction::kSparse].raw_num(), 8);
  EXPECT_EQ(stats.n_full[AccessDensityPrediction::kSparse].raw_num(), 3);
  EXPECT_EQ(stats.n_partial[AccessDensityPrediction::kSparse].raw_num(), 1);

  // Check densely-accessed filler stats.
  EXPECT_EQ(stats.n_fully_released[AccessDensityPrediction::kDense].raw_num(),
            1);
  EXPECT_EQ(stats.n_released[AccessDensityPrediction::kDense].raw_num(), 1);
  EXPECT_EQ(stats.n_partial_released[AccessDensityPrediction::kDense].raw_num(),
            0);
  EXPECT_EQ(stats.n_total[AccessDensityPrediction::kDense].raw_num(), 7);
  EXPECT_EQ(stats.n_full[AccessDensityPrediction::kDense].raw_num(), 6);
  EXPECT_EQ(stats.n_partial[AccessDensityPrediction::kDense].raw_num(), 0);

  // Check total filler stats.
  EXPECT_EQ(stats.n_fully_released[AccessDensityPrediction::kPredictionCounts]
                .raw_num(),
            5);
  EXPECT_EQ(
      stats.n_released[AccessDensityPrediction::kPredictionCounts].raw_num(),
      5);
  EXPECT_EQ(stats.n_partial_released[AccessDensityPrediction::kPredictionCounts]
                .raw_num(),
            0);
  EXPECT_EQ(stats.n_total[AccessDensityPrediction::kPredictionCounts].raw_num(),
            15);
  EXPECT_EQ(stats.n_full[AccessDensityPrediction::kPredictionCounts].raw_num(),
            9);
  EXPECT_EQ(
      stats.n_partial[AccessDensityPrediction::kPredictionCounts].raw_num(), 1);

  for (const auto& alloc : allocs) {
    Delete(alloc);
  }
}

TEST_P(FillerTest, CheckFillerStats_SpansAllocated) {
  if (kPagesPerHugePage != Length(256)) {
    // The output is hardcoded on this assumption, and dynamically calculating
    // it would be way too much of a pain.
    return;
  }
  // We prevent randomly choosing the number of objects per span since this
  // test has hardcoded output which will change if the objects per span are
  // chosen at random.
  randomize_density_ = false;
  auto allocs = GenerateInterestingAllocs();

  const HugePageFillerStats stats = filler_.GetStats();
  for (int i = 0; i < AccessDensityPrediction::kPredictionCounts; ++i) {
    EXPECT_GE(stats.n_fully_released[i].raw_num(), 0);
  }
  // Check sparsely-accessed filler stats.
  EXPECT_EQ(stats.n_fully_released[AccessDensityPrediction::kSparse].raw_num(),
            4);
  EXPECT_EQ(stats.n_released[AccessDensityPrediction::kSparse].raw_num(), 4);
  EXPECT_EQ(
      stats.n_partial_released[AccessDensityPrediction::kSparse].raw_num(), 0);
  EXPECT_EQ(stats.n_total[AccessDensityPrediction::kSparse].raw_num(), 8);
  EXPECT_EQ(stats.n_full[AccessDensityPrediction::kSparse].raw_num(), 3);
  EXPECT_EQ(stats.n_partial[AccessDensityPrediction::kSparse].raw_num(), 1);

  // Check densely-accessed filler stats.
  EXPECT_EQ(stats.n_fully_released[AccessDensityPrediction::kDense].raw_num(),
            1);
  EXPECT_EQ(stats.n_released[AccessDensityPrediction::kDense].raw_num(), 1);
  EXPECT_EQ(stats.n_partial_released[AccessDensityPrediction::kDense].raw_num(),
            0);
  EXPECT_EQ(stats.n_total[AccessDensityPrediction::kDense].raw_num(), 7);
  EXPECT_EQ(stats.n_full[AccessDensityPrediction::kDense].raw_num(), 6);
  EXPECT_EQ(stats.n_partial[AccessDensityPrediction::kDense].raw_num(), 0);

  // Check total filler stats.
  EXPECT_EQ(stats.n_fully_released[AccessDensityPrediction::kPredictionCounts]
                .raw_num(),
            5);
  EXPECT_EQ(
      stats.n_released[AccessDensityPrediction::kPredictionCounts].raw_num(),
      5);
  EXPECT_EQ(stats.n_partial_released[AccessDensityPrediction::kPredictionCounts]
                .raw_num(),
            0);
  EXPECT_EQ(stats.n_total[AccessDensityPrediction::kPredictionCounts].raw_num(),
            15);
  EXPECT_EQ(stats.n_full[AccessDensityPrediction::kPredictionCounts].raw_num(),
            9);
  EXPECT_EQ(
      stats.n_partial[AccessDensityPrediction::kPredictionCounts].raw_num(), 1);

  for (const auto& alloc : allocs) {
    Delete(alloc);
  }
}

TEST_P(FillerTest, ResidencyTelemetry) {
  if (kPagesPerHugePage != Length(256)) {
    // The output is hardcoded on this assumption, and dynamically calculating
    // it would be way too much of a pain.
    return;
  }

  const Length kAlloc = kPagesPerHugePage / 2;

  SpanAllocInfo sparsely_accessed_info = {1, AccessDensityPrediction::kSparse};
  std::vector<PAlloc> p1 = AllocateVectorWithSpanAllocInfo(
      kAlloc - Length(1), sparsely_accessed_info);
  ASSERT_TRUE(!p1.empty());

  FakePageFlags pageflags;
  FakeResidency residency;
  for (const auto& pa : p1) {
    pageflags.MarkHugePageBacked(pa.p.start_addr(),
                                 /*is_hugepage_backed=*/false);
    EXPECT_FALSE(pageflags.IsHugepageBacked(pa.p.start_addr()));

    Bitmap<kMaxResidencyBits> unbacked, swapped;
    unbacked.SetRange(/*index=*/kMaxResidencyBits / 4, kMaxResidencyBits / 2);
    swapped.SetRange(/*index=*/kMaxResidencyBits / 4, kMaxResidencyBits / 2);
    residency.SetUnbackedAndSwappedBitmaps(pa.p.start_addr(), unbacked,
                                           swapped);
  }

  ASSERT_EQ(filler_.size(), NHugePages(1));
  TreatHugepageTrackers(/*enable_collapse=*/true,
                        /*enable_release_free_swapped=*/false,
                        /*use_userspace_collapse_heuristics=*/false, &pageflags,
                        &residency);
  for (const auto& pa : p1) {
    EXPECT_FALSE(collapse_.TriedCollapse(pa.p.start_addr()));
  }

  std::string buffer_text(1024 * 1024, '\0');
  {
    PageHeapSpinLockHolder l;
    Printer printer(&*buffer_text.begin(), buffer_text.size());
    filler_.Print(printer, /*everything=*/true, pageflags);
  }
  buffer_text.resize(strlen(buffer_text.c_str()));
  EXPECT_THAT(buffer_text, testing::HasSubstr(R"(
HugePageFiller: # of sparsely-accessed regular hps with a <= # of swapped < b
HugePageFiller: <  0<=     0 <  1<=     0 <  2<=     0 <  3<=     0 <  4<=     0 <  5<=     0
HugePageFiller: <  6<=     0 <  7<=     0 <  8<=     0 < 40<=     0 < 72<=     0 <104<=     0
HugePageFiller: <136<=     0 <168<=     0 <200<=     0 <232<=     1 <264<=     0 <296<=     0
HugePageFiller: <328<=     0 <360<=     0 <392<=     0 <424<=     0 <456<=     0 <488<=     0
HugePageFiller: <504<=     0 <505<=     0 <506<=     0 <507<=     0 <508<=     0 <509<=     0
HugePageFiller: <510<=     0 <511<=     0
)"));

  EXPECT_THAT(buffer_text, testing::HasSubstr(R"(
HugePageFiller: # of sparsely-accessed regular hps with a <= # of unbacked < b
HugePageFiller: <  0<=     0 <  1<=     0 <  2<=     0 <  3<=     0 <  4<=     0 <  5<=     0
HugePageFiller: <  6<=     0 <  7<=     0 <  8<=     0 < 40<=     0 < 72<=     0 <104<=     0
HugePageFiller: <136<=     0 <168<=     0 <200<=     0 <232<=     1 <264<=     0 <296<=     0
HugePageFiller: <328<=     0 <360<=     0 <392<=     0 <424<=     0 <456<=     0 <488<=     0
HugePageFiller: <504<=     0 <505<=     0 <506<=     0 <507<=     0 <508<=     0 <509<=     0
HugePageFiller: <510<=     0 <511<=     0
)"));

  EXPECT_THAT(buffer_text, testing::HasSubstr(R"(
HugePageFiller: # of sparsely-accessed regular hps with a <= # of free AND unbacked < b
HugePageFiller: <  0<=     0 <  1<=     0 <  2<=     0 <  3<=     0 <  4<=     0 <  5<=     0
HugePageFiller: <  6<=     0 <  7<=     0 <  8<=     0 < 40<=     0 < 72<=     0 <104<=     1
HugePageFiller: <136<=     0 <168<=     0 <200<=     0 <232<=     0 <264<=     0 <296<=     0
HugePageFiller: <328<=     0 <360<=     0 <392<=     0 <424<=     0 <456<=     0 <488<=     0
HugePageFiller: <504<=     0 <505<=     0 <506<=     0 <507<=     0 <508<=     0 <509<=     0
HugePageFiller: <510<=     0 <511<=     0
)"));

  EXPECT_THAT(buffer_text, testing::HasSubstr(R"(
HugePageFiller: # of sparsely-accessed regular hps with a <= # of free AND swapped < b
HugePageFiller: <  0<=     0 <  1<=     0 <  2<=     0 <  3<=     0 <  4<=     0 <  5<=     0
HugePageFiller: <  6<=     0 <  7<=     0 <  8<=     0 < 40<=     0 < 72<=     0 <104<=     1
HugePageFiller: <136<=     0 <168<=     0 <200<=     0 <232<=     0 <264<=     0 <296<=     0
HugePageFiller: <328<=     0 <360<=     0 <392<=     0 <424<=     0 <456<=     0 <488<=     0
HugePageFiller: <504<=     0 <505<=     0 <506<=     0 <507<=     0 <508<=     0 <509<=     0
HugePageFiller: <510<=     0 <511<=     0
)"));

  EXPECT_THAT(buffer_text, testing::HasSubstr(R"(
HugePageFiller: 1 of sparsely-accessed regular pages treated out of 1.
)"));

  std::string buffer_pbtxt(1024 * 1024, '\0');
  Printer printer(&*buffer_pbtxt.begin(), buffer_pbtxt.size());
  {
    PageHeapSpinLockHolder l;
    PbtxtRegion region(printer, kTop);
    filler_.PrintInPbtxt(region, pageflags);
  }
  buffer_pbtxt.erase(printer.SpaceRequired());

  EXPECT_THAT(
      buffer_pbtxt,
      testing::HasSubstr(
          "unbacked_histogram { lower_bound: 232 upper_bound: 263 value: 1}"));
  EXPECT_THAT(
      buffer_pbtxt,
      testing::HasSubstr(
          "swapped_histogram { lower_bound: 232 upper_bound: 263 value: 1}"));
  EXPECT_THAT(buffer_pbtxt,
              testing::HasSubstr("free_unbacked_histogram { lower_bound: 104 "
                                 "upper_bound: 135 value: 1}"));
  EXPECT_THAT(buffer_pbtxt,
              testing::HasSubstr("free_swapped_histogram { lower_bound: 104 "
                                 "upper_bound: 135 value: 1}"));
  EXPECT_THAT(buffer_pbtxt, testing::HasSubstr("num_pages_treated: 1"));
  DeleteVector(p1);
}

TEST_P(FillerTest, PrintHugepageBackedStats) {
  const Length kAlloc = kPagesPerHugePage / 2;
  randomize_density_ = false;
  SpanAllocInfo sparsely_accessed_info = {1, AccessDensityPrediction::kSparse};
  std::vector<PAlloc> p1 = AllocateVectorWithSpanAllocInfo(
      kAlloc - Length(1), sparsely_accessed_info);
  std::vector<PAlloc> p2 = AllocateVectorWithSpanAllocInfo(
      kAlloc + Length(1), sparsely_accessed_info);
  std::vector<PAlloc> p3 = AllocateVectorWithSpanAllocInfo(
      kAlloc - Length(1), sparsely_accessed_info);
  ASSERT_TRUE(!p1.empty());
  ASSERT_TRUE(!p2.empty());
  ASSERT_TRUE(!p3.empty());
  ASSERT_EQ(filler_.size(), NHugePages(2));

  FakePageFlags pageflags;
  for (const auto& pa : p1) {
    pageflags.MarkHugePageBacked(pa.p.start_addr(),
                                 /*is_hugepage_backed=*/true);
    EXPECT_TRUE(pageflags.IsHugepageBacked(pa.p.start_addr()));
  }

  std::string buffer(1024 * 1024, '\0');
  {
    PageHeapSpinLockHolder l;
    Printer printer(&*buffer.begin(), buffer.size());
    filler_.Print(printer, /*everything=*/true, pageflags);
    buffer.erase(printer.SpaceRequired());
  }
  EXPECT_THAT(buffer, testing::HasSubstr(R"(
HugePageFiller: 1 of sparsely-accessed regular pages hugepage backed out of 2.
)"));
  EXPECT_THAT(buffer, testing::HasSubstr(R"(
HugePageFiller: 0 of densely-accessed regular pages hugepage backed out of 0.
)"));
  EXPECT_THAT(buffer, testing::HasSubstr(R"(
HugePageFiller: 0 of donated pages hugepage backed out of 0.
)"));
  EXPECT_THAT(buffer, testing::HasSubstr(R"(
HugePageFiller: 0 of sparsely-accessed partial released pages hugepage backed out of 0.
)"));
  EXPECT_THAT(buffer, testing::HasSubstr(R"(
HugePageFiller: 0 of densely-accessed partial released pages hugepage backed out of 0.
)"));
  EXPECT_THAT(buffer, testing::HasSubstr(R"(
HugePageFiller: 0 of sparsely-accessed released pages hugepage backed out of 0.
)"));
  EXPECT_THAT(buffer, testing::HasSubstr(R"(
HugePageFiller: 0 of densely-accessed released pages hugepage backed out of 0.
)"));
  DeleteVector(p1);
  DeleteVector(p2);
  DeleteVector(p3);
}

// Test the output of Print(). This is something of a change-detector test,
// but that's not all bad in this case.
TEST_P(FillerTest, Print) {
  if (kPagesPerHugePage != Length(256)) {
    // The output is hardcoded on this assumption, and dynamically calculating
    // it would be way too much of a pain.
    return;
  }
  // We prevent randomly choosing the number of objects per span since this
  // test has hardcoded output which will change if the objects per span are
  // chosen at random.
  randomize_density_ = false;
  auto allocs = GenerateInterestingAllocs();

  std::string buffer(1024 * 1024, '\0');
  FakePageFlags pageflags;
  {
    PageHeapSpinLockHolder l;
    Printer printer(&*buffer.begin(), buffer.size());
    filler_.Print(printer, /*everything=*/true, pageflags);
    buffer.erase(printer.SpaceRequired());
  }

  EXPECT_THAT(
      buffer,
      StrEq(R"(HugePageFiller: densely pack small requests into hugepages
HugePageFiller: Overall, 15 total, 9 full, 1 partial, 5 released (0 partially), 0 quarantined
HugePageFiller: those with sparsely-accessed spans, 8 total, 3 full, 1 partial, 4 released (0 partially), 0 quarantined
HugePageFiller: those with densely-accessed spans, 7 total, 6 full, 0 partial, 1 released (0 partially), 0 quarantined
HugePageFiller: 255 pages free in 15 hugepages, 0.0664 free
HugePageFiller: among non-fulls, 0.9961 free
HugePageFiller: 1242 used pages in subreleased hugepages (0 of them in partially released)
HugePageFiller: 5 hugepages partially released, 0.0297 released
HugePageFiller: 0.6498 of used pages hugepageable
HugePageFiller: Since startup, 306 pages subreleased, 6 hugepages broken, (0 pages, 0 hugepages due to reaching tcmalloc limit)
HugePageFiller: Out of 0 eligible hugepages, 0 were attempted, and 0 were collapsed.
HugePageFiller: Of the failed collapse operations, number of operations that failed per error type, ETYPE_NOMEM: 0, ETYPE_BUSY: 0, ETYPE_INVAL: 0, ETYPE_AGAIN: 0, ETYPE_OTHER: 0
HugePageFiller: Latency of collapse operations: 0.000000 ms (total), 0.000000 us (maximum)
HugePageFiller: Backoff delay for collapse currently is 1 interval(s), number of intervals skipped due to backoff is 0
HugePageFiller: In the previous treatment interval, subreleased 0 pages.

HugePageFiller: fullness histograms

HugePageFiller: # of sparsely-accessed regular hps with a<= # of free pages <b
HugePageFiller: <  0<=     3 <  1<=     0 <  2<=     0 <  3<=     0 <  4<=     0 <  5<=     0
HugePageFiller: <  6<=     0 <  7<=     0 <  8<=     0 < 16<=     0 < 32<=     0 < 48<=     0
HugePageFiller: < 64<=     0 < 80<=     0 < 96<=     0 <112<=     0 <128<=     0 <144<=     0
HugePageFiller: <160<=     0 <176<=     0 <192<=     0 <208<=     0 <224<=     0 <240<=     0
HugePageFiller: <248<=     0 <249<=     0 <250<=     0 <251<=     0 <252<=     0 <253<=     0
HugePageFiller: <254<=     0 <255<=     0

HugePageFiller: # of sparsely-accessed regular hps with a<= longest free range <b
HugePageFiller: <  0<=     3 <  1<=     0 <  2<=     0 <  3<=     0 <  4<=     0 <  5<=     0
HugePageFiller: <  6<=     0 <  7<=     0 <  8<=     0 < 16<=     0 < 32<=     0 < 48<=     0
HugePageFiller: < 64<=     0 < 80<=     0 < 96<=     0 <112<=     0 <128<=     0 <144<=     0
HugePageFiller: <160<=     0 <176<=     0 <192<=     0 <208<=     0 <224<=     0 <240<=     0
HugePageFiller: <248<=     0 <249<=     0 <250<=     0 <251<=     0 <252<=     0 <253<=     0
HugePageFiller: <254<=     0 <255<=     0

HugePageFiller: # of sparsely-accessed regular hps with a<= # of allocations <b
HugePageFiller: <  1<=     0 <  2<=     1 <  3<=     1 <  4<=     1 <  5<=     0 <  6<=     0
HugePageFiller: <  7<=     0 <  8<=     0 <  9<=     0 < 17<=     0 < 33<=     0 < 49<=     0
HugePageFiller: < 65<=     0 < 81<=     0 < 97<=     0 <113<=     0 <129<=     0 <145<=     0
HugePageFiller: <161<=     0 <177<=     0 <193<=     0 <209<=     0 <225<=     0 <241<=     0
HugePageFiller: <249<=     0 <250<=     0 <251<=     0 <252<=     0 <253<=     0 <254<=     0
HugePageFiller: <255<=     0 <256<=     0

HugePageFiller: # of sparsely-accessed regular hps with live lifetime a <= # hps < b
HugePageFiller: <   0 ms <=      3 <   1 ms <=      0 <  10 ms <=      0 < 100 ms <=      0 < 1000 ms <=      0 < 10000 ms <=      0
HugePageFiller: < 100000 ms <=      0 < 1000000 ms <=      0

HugePageFiller: # of hps with >= 224 free pages, with different lifetimes.
HugePageFiller: # of sparsely-accessed regular hps with lifetime a <= # hps < b
HugePageFiller: <   0 ms <=      0 <   1 ms <=      0 <  10 ms <=      0 < 100 ms <=      0 < 1000 ms <=      0 < 10000 ms <=      0
HugePageFiller: < 100000 ms <=      0 < 1000000 ms <=      0

HugePageFiller: # of hps with lifetime >= 100000 ms.
HugePageFiller: # of sparsely-accessed regular hps with a <= # of allocations < b
HugePageFiller: <  0<=     0 <  1<=     0 <  2<=     0 <  3<=     0 <  4<=     0 <  5<=     0
HugePageFiller: <  6<=     0 <  7<=     0 <  8<=     0 < 16<=     0 < 32<=     0 < 48<=     0
HugePageFiller: < 64<=     0 < 80<=     0 < 96<=     0 <112<=     0 <128<=     0 <144<=     0
HugePageFiller: <160<=     0 <176<=     0 <192<=     0 <208<=     0 <224<=     0 <240<=     0
HugePageFiller: <248<=     0 <249<=     0 <250<=     0 <251<=     0 <252<=     0 <253<=     0
HugePageFiller: <254<=     0 <255<=     0

HugePageFiller: # of sparsely-accessed regular hps with a <= # of unbacked < b
HugePageFiller: <  0<=     0 <  1<=     0 <  2<=     0 <  3<=     0 <  4<=     0 <  5<=     0
HugePageFiller: <  6<=     0 <  7<=     0 <  8<=     0 < 40<=     0 < 72<=     0 <104<=     0
HugePageFiller: <136<=     0 <168<=     0 <200<=     0 <232<=     0 <264<=     0 <296<=     0
HugePageFiller: <328<=     0 <360<=     0 <392<=     0 <424<=     0 <456<=     0 <488<=     0
HugePageFiller: <504<=     0 <505<=     0 <506<=     0 <507<=     0 <508<=     0 <509<=     0
HugePageFiller: <510<=     0 <511<=     0

HugePageFiller: # of sparsely-accessed regular hps with a <= # of swapped < b
HugePageFiller: <  0<=     0 <  1<=     0 <  2<=     0 <  3<=     0 <  4<=     0 <  5<=     0
HugePageFiller: <  6<=     0 <  7<=     0 <  8<=     0 < 40<=     0 < 72<=     0 <104<=     0
HugePageFiller: <136<=     0 <168<=     0 <200<=     0 <232<=     0 <264<=     0 <296<=     0
HugePageFiller: <328<=     0 <360<=     0 <392<=     0 <424<=     0 <456<=     0 <488<=     0
HugePageFiller: <504<=     0 <505<=     0 <506<=     0 <507<=     0 <508<=     0 <509<=     0
HugePageFiller: <510<=     0 <511<=     0

HugePageFiller: # of sparsely-accessed regular hps with a <= # of free AND unbacked < b
HugePageFiller: <  0<=     0 <  1<=     0 <  2<=     0 <  3<=     0 <  4<=     0 <  5<=     0
HugePageFiller: <  6<=     0 <  7<=     0 <  8<=     0 < 40<=     0 < 72<=     0 <104<=     0
HugePageFiller: <136<=     0 <168<=     0 <200<=     0 <232<=     0 <264<=     0 <296<=     0
HugePageFiller: <328<=     0 <360<=     0 <392<=     0 <424<=     0 <456<=     0 <488<=     0
HugePageFiller: <504<=     0 <505<=     0 <506<=     0 <507<=     0 <508<=     0 <509<=     0
HugePageFiller: <510<=     0 <511<=     0

HugePageFiller: # of sparsely-accessed regular hps with a <= # of free AND swapped < b
HugePageFiller: <  0<=     0 <  1<=     0 <  2<=     0 <  3<=     0 <  4<=     0 <  5<=     0
HugePageFiller: <  6<=     0 <  7<=     0 <  8<=     0 < 40<=     0 < 72<=     0 <104<=     0
HugePageFiller: <136<=     0 <168<=     0 <200<=     0 <232<=     0 <264<=     0 <296<=     0
HugePageFiller: <328<=     0 <360<=     0 <392<=     0 <424<=     0 <456<=     0 <488<=     0
HugePageFiller: <504<=     0 <505<=     0 <506<=     0 <507<=     0 <508<=     0 <509<=     0
HugePageFiller: <510<=     0 <511<=     0

HugePageFiller: 0 of sparsely-accessed regular pages hugepage backed out of 3.
HugePageFiller: 0 of sparsely-accessed regular pages treated out of 3.

HugePageFiller: Sampled Trackers for sparsely-accessed regular pages:

HugePageFiller: # of densely-accessed regular hps with a<= # of free pages <b
HugePageFiller: <  0<=     6 <  1<=     0 <  2<=     0 <  3<=     0 <  4<=     0 <  5<=     0
HugePageFiller: <  6<=     0 <  7<=     0 <  8<=     0 < 16<=     0 < 32<=     0 < 48<=     0
HugePageFiller: < 64<=     0 < 80<=     0 < 96<=     0 <112<=     0 <128<=     0 <144<=     0
HugePageFiller: <160<=     0 <176<=     0 <192<=     0 <208<=     0 <224<=     0 <240<=     0
HugePageFiller: <248<=     0 <249<=     0 <250<=     0 <251<=     0 <252<=     0 <253<=     0
HugePageFiller: <254<=     0 <255<=     0

HugePageFiller: # of densely-accessed regular hps with a<= longest free range <b
HugePageFiller: <  0<=     6 <  1<=     0 <  2<=     0 <  3<=     0 <  4<=     0 <  5<=     0
HugePageFiller: <  6<=     0 <  7<=     0 <  8<=     0 < 16<=     0 < 32<=     0 < 48<=     0
HugePageFiller: < 64<=     0 < 80<=     0 < 96<=     0 <112<=     0 <128<=     0 <144<=     0
HugePageFiller: <160<=     0 <176<=     0 <192<=     0 <208<=     0 <224<=     0 <240<=     0
HugePageFiller: <248<=     0 <249<=     0 <250<=     0 <251<=     0 <252<=     0 <253<=     0
HugePageFiller: <254<=     0 <255<=     0

HugePageFiller: # of densely-accessed regular hps with a<= # of allocations <b
HugePageFiller: <  1<=     0 <  2<=     0 <  3<=     0 <  4<=     0 <  5<=     0 <  6<=     0
HugePageFiller: <  7<=     0 <  8<=     0 <  9<=     0 < 17<=     0 < 33<=     0 < 49<=     0
HugePageFiller: < 65<=     0 < 81<=     0 < 97<=     0 <113<=     0 <129<=     0 <145<=     0
HugePageFiller: <161<=     0 <177<=     0 <193<=     0 <209<=     0 <225<=     0 <241<=     0
HugePageFiller: <249<=     0 <250<=     0 <251<=     0 <252<=     0 <253<=     0 <254<=     0
HugePageFiller: <255<=     0 <256<=     6

HugePageFiller: # of densely-accessed regular hps with live lifetime a <= # hps < b
HugePageFiller: <   0 ms <=      6 <   1 ms <=      0 <  10 ms <=      0 < 100 ms <=      0 < 1000 ms <=      0 < 10000 ms <=      0
HugePageFiller: < 100000 ms <=      0 < 1000000 ms <=      0

HugePageFiller: # of hps with >= 224 free pages, with different lifetimes.
HugePageFiller: # of densely-accessed regular hps with lifetime a <= # hps < b
HugePageFiller: <   0 ms <=      0 <   1 ms <=      0 <  10 ms <=      0 < 100 ms <=      0 < 1000 ms <=      0 < 10000 ms <=      0
HugePageFiller: < 100000 ms <=      0 < 1000000 ms <=      0

HugePageFiller: # of hps with lifetime >= 100000 ms.
HugePageFiller: # of densely-accessed regular hps with a <= # of allocations < b
HugePageFiller: <  0<=     0 <  1<=     0 <  2<=     0 <  3<=     0 <  4<=     0 <  5<=     0
HugePageFiller: <  6<=     0 <  7<=     0 <  8<=     0 < 16<=     0 < 32<=     0 < 48<=     0
HugePageFiller: < 64<=     0 < 80<=     0 < 96<=     0 <112<=     0 <128<=     0 <144<=     0
HugePageFiller: <160<=     0 <176<=     0 <192<=     0 <208<=     0 <224<=     0 <240<=     0
HugePageFiller: <248<=     0 <249<=     0 <250<=     0 <251<=     0 <252<=     0 <253<=     0
HugePageFiller: <254<=     0 <255<=     0

HugePageFiller: # of densely-accessed regular hps with a <= # of unbacked < b
HugePageFiller: <  0<=     0 <  1<=     0 <  2<=     0 <  3<=     0 <  4<=     0 <  5<=     0
HugePageFiller: <  6<=     0 <  7<=     0 <  8<=     0 < 40<=     0 < 72<=     0 <104<=     0
HugePageFiller: <136<=     0 <168<=     0 <200<=     0 <232<=     0 <264<=     0 <296<=     0
HugePageFiller: <328<=     0 <360<=     0 <392<=     0 <424<=     0 <456<=     0 <488<=     0
HugePageFiller: <504<=     0 <505<=     0 <506<=     0 <507<=     0 <508<=     0 <509<=     0
HugePageFiller: <510<=     0 <511<=     0

HugePageFiller: # of densely-accessed regular hps with a <= # of swapped < b
HugePageFiller: <  0<=     0 <  1<=     0 <  2<=     0 <  3<=     0 <  4<=     0 <  5<=     0
HugePageFiller: <  6<=     0 <  7<=     0 <  8<=     0 < 40<=     0 < 72<=     0 <104<=     0
HugePageFiller: <136<=     0 <168<=     0 <200<=     0 <232<=     0 <264<=     0 <296<=     0
HugePageFiller: <328<=     0 <360<=     0 <392<=     0 <424<=     0 <456<=     0 <488<=     0
HugePageFiller: <504<=     0 <505<=     0 <506<=     0 <507<=     0 <508<=     0 <509<=     0
HugePageFiller: <510<=     0 <511<=     0

HugePageFiller: # of densely-accessed regular hps with a <= # of free AND unbacked < b
HugePageFiller: <  0<=     0 <  1<=     0 <  2<=     0 <  3<=     0 <  4<=     0 <  5<=     0
HugePageFiller: <  6<=     0 <  7<=     0 <  8<=     0 < 40<=     0 < 72<=     0 <104<=     0
HugePageFiller: <136<=     0 <168<=     0 <200<=     0 <232<=     0 <264<=     0 <296<=     0
HugePageFiller: <328<=     0 <360<=     0 <392<=     0 <424<=     0 <456<=     0 <488<=     0
HugePageFiller: <504<=     0 <505<=     0 <506<=     0 <507<=     0 <508<=     0 <509<=     0
HugePageFiller: <510<=     0 <511<=     0

HugePageFiller: # of densely-accessed regular hps with a <= # of free AND swapped < b
HugePageFiller: <  0<=     0 <  1<=     0 <  2<=     0 <  3<=     0 <  4<=     0 <  5<=     0
HugePageFiller: <  6<=     0 <  7<=     0 <  8<=     0 < 40<=     0 < 72<=     0 <104<=     0
HugePageFiller: <136<=     0 <168<=     0 <200<=     0 <232<=     0 <264<=     0 <296<=     0
HugePageFiller: <328<=     0 <360<=     0 <392<=     0 <424<=     0 <456<=     0 <488<=     0
HugePageFiller: <504<=     0 <505<=     0 <506<=     0 <507<=     0 <508<=     0 <509<=     0
HugePageFiller: <510<=     0 <511<=     0

HugePageFiller: 0 of densely-accessed regular pages hugepage backed out of 6.
HugePageFiller: 0 of densely-accessed regular pages treated out of 6.

HugePageFiller: Sampled Trackers for densely-accessed regular pages:

HugePageFiller: # of donated hps with a<= # of free pages <b
HugePageFiller: <  0<=     0 <  1<=     0 <  2<=     0 <  3<=     0 <  4<=     0 <  5<=     0
HugePageFiller: <  6<=     0 <  7<=     0 <  8<=     0 < 16<=     0 < 32<=     0 < 48<=     0
HugePageFiller: < 64<=     0 < 80<=     0 < 96<=     0 <112<=     0 <128<=     0 <144<=     0
HugePageFiller: <160<=     0 <176<=     0 <192<=     0 <208<=     0 <224<=     0 <240<=     0
HugePageFiller: <248<=     0 <249<=     0 <250<=     0 <251<=     0 <252<=     0 <253<=     0
HugePageFiller: <254<=     0 <255<=     1

HugePageFiller: # of donated hps with live lifetime a <= # hps < b
HugePageFiller: <   0 ms <=      1 <   1 ms <=      0 <  10 ms <=      0 < 100 ms <=      0 < 1000 ms <=      0 < 10000 ms <=      0
HugePageFiller: < 100000 ms <=      0 < 1000000 ms <=      0

HugePageFiller: # of hps with >= 224 free pages, with different lifetimes.
HugePageFiller: # of donated hps with lifetime a <= # hps < b
HugePageFiller: <   0 ms <=      1 <   1 ms <=      0 <  10 ms <=      0 < 100 ms <=      0 < 1000 ms <=      0 < 10000 ms <=      0
HugePageFiller: < 100000 ms <=      0 < 1000000 ms <=      0

HugePageFiller: # of hps with lifetime >= 100000 ms.
HugePageFiller: # of donated hps with a <= # of allocations < b
HugePageFiller: <  0<=     0 <  1<=     0 <  2<=     0 <  3<=     0 <  4<=     0 <  5<=     0
HugePageFiller: <  6<=     0 <  7<=     0 <  8<=     0 < 16<=     0 < 32<=     0 < 48<=     0
HugePageFiller: < 64<=     0 < 80<=     0 < 96<=     0 <112<=     0 <128<=     0 <144<=     0
HugePageFiller: <160<=     0 <176<=     0 <192<=     0 <208<=     0 <224<=     0 <240<=     0
HugePageFiller: <248<=     0 <249<=     0 <250<=     0 <251<=     0 <252<=     0 <253<=     0
HugePageFiller: <254<=     0 <255<=     0

HugePageFiller: # of donated hps with a <= # of unbacked < b
HugePageFiller: <  0<=     0 <  1<=     0 <  2<=     0 <  3<=     0 <  4<=     0 <  5<=     0
HugePageFiller: <  6<=     0 <  7<=     0 <  8<=     0 < 40<=     0 < 72<=     0 <104<=     0
HugePageFiller: <136<=     0 <168<=     0 <200<=     0 <232<=     0 <264<=     0 <296<=     0
HugePageFiller: <328<=     0 <360<=     0 <392<=     0 <424<=     0 <456<=     0 <488<=     0
HugePageFiller: <504<=     0 <505<=     0 <506<=     0 <507<=     0 <508<=     0 <509<=     0
HugePageFiller: <510<=     0 <511<=     0

HugePageFiller: # of donated hps with a <= # of swapped < b
HugePageFiller: <  0<=     0 <  1<=     0 <  2<=     0 <  3<=     0 <  4<=     0 <  5<=     0
HugePageFiller: <  6<=     0 <  7<=     0 <  8<=     0 < 40<=     0 < 72<=     0 <104<=     0
HugePageFiller: <136<=     0 <168<=     0 <200<=     0 <232<=     0 <264<=     0 <296<=     0
HugePageFiller: <328<=     0 <360<=     0 <392<=     0 <424<=     0 <456<=     0 <488<=     0
HugePageFiller: <504<=     0 <505<=     0 <506<=     0 <507<=     0 <508<=     0 <509<=     0
HugePageFiller: <510<=     0 <511<=     0

HugePageFiller: # of donated hps with a <= # of free AND unbacked < b
HugePageFiller: <  0<=     0 <  1<=     0 <  2<=     0 <  3<=     0 <  4<=     0 <  5<=     0
HugePageFiller: <  6<=     0 <  7<=     0 <  8<=     0 < 40<=     0 < 72<=     0 <104<=     0
HugePageFiller: <136<=     0 <168<=     0 <200<=     0 <232<=     0 <264<=     0 <296<=     0
HugePageFiller: <328<=     0 <360<=     0 <392<=     0 <424<=     0 <456<=     0 <488<=     0
HugePageFiller: <504<=     0 <505<=     0 <506<=     0 <507<=     0 <508<=     0 <509<=     0
HugePageFiller: <510<=     0 <511<=     0

HugePageFiller: # of donated hps with a <= # of free AND swapped < b
HugePageFiller: <  0<=     0 <  1<=     0 <  2<=     0 <  3<=     0 <  4<=     0 <  5<=     0
HugePageFiller: <  6<=     0 <  7<=     0 <  8<=     0 < 40<=     0 < 72<=     0 <104<=     0
HugePageFiller: <136<=     0 <168<=     0 <200<=     0 <232<=     0 <264<=     0 <296<=     0
HugePageFiller: <328<=     0 <360<=     0 <392<=     0 <424<=     0 <456<=     0 <488<=     0
HugePageFiller: <504<=     0 <505<=     0 <506<=     0 <507<=     0 <508<=     0 <509<=     0
HugePageFiller: <510<=     0 <511<=     0

HugePageFiller: 0 of donated pages hugepage backed out of 1.
HugePageFiller: 0 of donated pages treated out of 1.

HugePageFiller: Sampled Trackers for donated pages:

HugePageFiller: # of sparsely-accessed partial released hps with a<= # of free pages <b
HugePageFiller: <  0<=     0 <  1<=     0 <  2<=     0 <  3<=     0 <  4<=     0 <  5<=     0
HugePageFiller: <  6<=     0 <  7<=     0 <  8<=     0 < 16<=     0 < 32<=     0 < 48<=     0
HugePageFiller: < 64<=     0 < 80<=     0 < 96<=     0 <112<=     0 <128<=     0 <144<=     0
HugePageFiller: <160<=     0 <176<=     0 <192<=     0 <208<=     0 <224<=     0 <240<=     0
HugePageFiller: <248<=     0 <249<=     0 <250<=     0 <251<=     0 <252<=     0 <253<=     0
HugePageFiller: <254<=     0 <255<=     0

HugePageFiller: # of sparsely-accessed partial released hps with a<= longest free range <b
HugePageFiller: <  0<=     0 <  1<=     0 <  2<=     0 <  3<=     0 <  4<=     0 <  5<=     0
HugePageFiller: <  6<=     0 <  7<=     0 <  8<=     0 < 16<=     0 < 32<=     0 < 48<=     0
HugePageFiller: < 64<=     0 < 80<=     0 < 96<=     0 <112<=     0 <128<=     0 <144<=     0
HugePageFiller: <160<=     0 <176<=     0 <192<=     0 <208<=     0 <224<=     0 <240<=     0
HugePageFiller: <248<=     0 <249<=     0 <250<=     0 <251<=     0 <252<=     0 <253<=     0
HugePageFiller: <254<=     0 <255<=     0

HugePageFiller: # of sparsely-accessed partial released hps with a<= # of allocations <b
HugePageFiller: <  1<=     0 <  2<=     0 <  3<=     0 <  4<=     0 <  5<=     0 <  6<=     0
HugePageFiller: <  7<=     0 <  8<=     0 <  9<=     0 < 17<=     0 < 33<=     0 < 49<=     0
HugePageFiller: < 65<=     0 < 81<=     0 < 97<=     0 <113<=     0 <129<=     0 <145<=     0
HugePageFiller: <161<=     0 <177<=     0 <193<=     0 <209<=     0 <225<=     0 <241<=     0
HugePageFiller: <249<=     0 <250<=     0 <251<=     0 <252<=     0 <253<=     0 <254<=     0
HugePageFiller: <255<=     0 <256<=     0

HugePageFiller: # of sparsely-accessed partial released hps with live lifetime a <= # hps < b
HugePageFiller: <   0 ms <=      0 <   1 ms <=      0 <  10 ms <=      0 < 100 ms <=      0 < 1000 ms <=      0 < 10000 ms <=      0
HugePageFiller: < 100000 ms <=      0 < 1000000 ms <=      0

HugePageFiller: # of hps with >= 224 free pages, with different lifetimes.
HugePageFiller: # of sparsely-accessed partial released hps with lifetime a <= # hps < b
HugePageFiller: <   0 ms <=      0 <   1 ms <=      0 <  10 ms <=      0 < 100 ms <=      0 < 1000 ms <=      0 < 10000 ms <=      0
HugePageFiller: < 100000 ms <=      0 < 1000000 ms <=      0

HugePageFiller: # of hps with lifetime >= 100000 ms.
HugePageFiller: # of sparsely-accessed partial released hps with a <= # of allocations < b
HugePageFiller: <  0<=     0 <  1<=     0 <  2<=     0 <  3<=     0 <  4<=     0 <  5<=     0
HugePageFiller: <  6<=     0 <  7<=     0 <  8<=     0 < 16<=     0 < 32<=     0 < 48<=     0
HugePageFiller: < 64<=     0 < 80<=     0 < 96<=     0 <112<=     0 <128<=     0 <144<=     0
HugePageFiller: <160<=     0 <176<=     0 <192<=     0 <208<=     0 <224<=     0 <240<=     0
HugePageFiller: <248<=     0 <249<=     0 <250<=     0 <251<=     0 <252<=     0 <253<=     0
HugePageFiller: <254<=     0 <255<=     0

HugePageFiller: # of sparsely-accessed partial released hps with a <= # of unbacked < b
HugePageFiller: <  0<=     0 <  1<=     0 <  2<=     0 <  3<=     0 <  4<=     0 <  5<=     0
HugePageFiller: <  6<=     0 <  7<=     0 <  8<=     0 < 40<=     0 < 72<=     0 <104<=     0
HugePageFiller: <136<=     0 <168<=     0 <200<=     0 <232<=     0 <264<=     0 <296<=     0
HugePageFiller: <328<=     0 <360<=     0 <392<=     0 <424<=     0 <456<=     0 <488<=     0
HugePageFiller: <504<=     0 <505<=     0 <506<=     0 <507<=     0 <508<=     0 <509<=     0
HugePageFiller: <510<=     0 <511<=     0

HugePageFiller: # of sparsely-accessed partial released hps with a <= # of swapped < b
HugePageFiller: <  0<=     0 <  1<=     0 <  2<=     0 <  3<=     0 <  4<=     0 <  5<=     0
HugePageFiller: <  6<=     0 <  7<=     0 <  8<=     0 < 40<=     0 < 72<=     0 <104<=     0
HugePageFiller: <136<=     0 <168<=     0 <200<=     0 <232<=     0 <264<=     0 <296<=     0
HugePageFiller: <328<=     0 <360<=     0 <392<=     0 <424<=     0 <456<=     0 <488<=     0
HugePageFiller: <504<=     0 <505<=     0 <506<=     0 <507<=     0 <508<=     0 <509<=     0
HugePageFiller: <510<=     0 <511<=     0

HugePageFiller: # of sparsely-accessed partial released hps with a <= # of free AND unbacked < b
HugePageFiller: <  0<=     0 <  1<=     0 <  2<=     0 <  3<=     0 <  4<=     0 <  5<=     0
HugePageFiller: <  6<=     0 <  7<=     0 <  8<=     0 < 40<=     0 < 72<=     0 <104<=     0
HugePageFiller: <136<=     0 <168<=     0 <200<=     0 <232<=     0 <264<=     0 <296<=     0
HugePageFiller: <328<=     0 <360<=     0 <392<=     0 <424<=     0 <456<=     0 <488<=     0
HugePageFiller: <504<=     0 <505<=     0 <506<=     0 <507<=     0 <508<=     0 <509<=     0
HugePageFiller: <510<=     0 <511<=     0

HugePageFiller: # of sparsely-accessed partial released hps with a <= # of free AND swapped < b
HugePageFiller: <  0<=     0 <  1<=     0 <  2<=     0 <  3<=     0 <  4<=     0 <  5<=     0
HugePageFiller: <  6<=     0 <  7<=     0 <  8<=     0 < 40<=     0 < 72<=     0 <104<=     0
HugePageFiller: <136<=     0 <168<=     0 <200<=     0 <232<=     0 <264<=     0 <296<=     0
HugePageFiller: <328<=     0 <360<=     0 <392<=     0 <424<=     0 <456<=     0 <488<=     0
HugePageFiller: <504<=     0 <505<=     0 <506<=     0 <507<=     0 <508<=     0 <509<=     0
HugePageFiller: <510<=     0 <511<=     0

HugePageFiller: 0 of sparsely-accessed partial released pages hugepage backed out of 0.
HugePageFiller: 0 of sparsely-accessed partial released pages treated out of 0.

HugePageFiller: Sampled Trackers for sparsely-accessed partial released pages:

HugePageFiller: # of densely-accessed partial released hps with a<= # of free pages <b
HugePageFiller: <  0<=     0 <  1<=     0 <  2<=     0 <  3<=     0 <  4<=     0 <  5<=     0
HugePageFiller: <  6<=     0 <  7<=     0 <  8<=     0 < 16<=     0 < 32<=     0 < 48<=     0
HugePageFiller: < 64<=     0 < 80<=     0 < 96<=     0 <112<=     0 <128<=     0 <144<=     0
HugePageFiller: <160<=     0 <176<=     0 <192<=     0 <208<=     0 <224<=     0 <240<=     0
HugePageFiller: <248<=     0 <249<=     0 <250<=     0 <251<=     0 <252<=     0 <253<=     0
HugePageFiller: <254<=     0 <255<=     0

HugePageFiller: # of densely-accessed partial released hps with a<= longest free range <b
HugePageFiller: <  0<=     0 <  1<=     0 <  2<=     0 <  3<=     0 <  4<=     0 <  5<=     0
HugePageFiller: <  6<=     0 <  7<=     0 <  8<=     0 < 16<=     0 < 32<=     0 < 48<=     0
HugePageFiller: < 64<=     0 < 80<=     0 < 96<=     0 <112<=     0 <128<=     0 <144<=     0
HugePageFiller: <160<=     0 <176<=     0 <192<=     0 <208<=     0 <224<=     0 <240<=     0
HugePageFiller: <248<=     0 <249<=     0 <250<=     0 <251<=     0 <252<=     0 <253<=     0
HugePageFiller: <254<=     0 <255<=     0

HugePageFiller: # of densely-accessed partial released hps with a<= # of allocations <b
HugePageFiller: <  1<=     0 <  2<=     0 <  3<=     0 <  4<=     0 <  5<=     0 <  6<=     0
HugePageFiller: <  7<=     0 <  8<=     0 <  9<=     0 < 17<=     0 < 33<=     0 < 49<=     0
HugePageFiller: < 65<=     0 < 81<=     0 < 97<=     0 <113<=     0 <129<=     0 <145<=     0
HugePageFiller: <161<=     0 <177<=     0 <193<=     0 <209<=     0 <225<=     0 <241<=     0
HugePageFiller: <249<=     0 <250<=     0 <251<=     0 <252<=     0 <253<=     0 <254<=     0
HugePageFiller: <255<=     0 <256<=     0

HugePageFiller: # of densely-accessed partial released hps with live lifetime a <= # hps < b
HugePageFiller: <   0 ms <=      0 <   1 ms <=      0 <  10 ms <=      0 < 100 ms <=      0 < 1000 ms <=      0 < 10000 ms <=      0
HugePageFiller: < 100000 ms <=      0 < 1000000 ms <=      0

HugePageFiller: # of hps with >= 224 free pages, with different lifetimes.
HugePageFiller: # of densely-accessed partial released hps with lifetime a <= # hps < b
HugePageFiller: <   0 ms <=      0 <   1 ms <=      0 <  10 ms <=      0 < 100 ms <=      0 < 1000 ms <=      0 < 10000 ms <=      0
HugePageFiller: < 100000 ms <=      0 < 1000000 ms <=      0

HugePageFiller: # of hps with lifetime >= 100000 ms.
HugePageFiller: # of densely-accessed partial released hps with a <= # of allocations < b
HugePageFiller: <  0<=     0 <  1<=     0 <  2<=     0 <  3<=     0 <  4<=     0 <  5<=     0
HugePageFiller: <  6<=     0 <  7<=     0 <  8<=     0 < 16<=     0 < 32<=     0 < 48<=     0
HugePageFiller: < 64<=     0 < 80<=     0 < 96<=     0 <112<=     0 <128<=     0 <144<=     0
HugePageFiller: <160<=     0 <176<=     0 <192<=     0 <208<=     0 <224<=     0 <240<=     0
HugePageFiller: <248<=     0 <249<=     0 <250<=     0 <251<=     0 <252<=     0 <253<=     0
HugePageFiller: <254<=     0 <255<=     0

HugePageFiller: # of densely-accessed partial released hps with a <= # of unbacked < b
HugePageFiller: <  0<=     0 <  1<=     0 <  2<=     0 <  3<=     0 <  4<=     0 <  5<=     0
HugePageFiller: <  6<=     0 <  7<=     0 <  8<=     0 < 40<=     0 < 72<=     0 <104<=     0
HugePageFiller: <136<=     0 <168<=     0 <200<=     0 <232<=     0 <264<=     0 <296<=     0
HugePageFiller: <328<=     0 <360<=     0 <392<=     0 <424<=     0 <456<=     0 <488<=     0
HugePageFiller: <504<=     0 <505<=     0 <506<=     0 <507<=     0 <508<=     0 <509<=     0
HugePageFiller: <510<=     0 <511<=     0

HugePageFiller: # of densely-accessed partial released hps with a <= # of swapped < b
HugePageFiller: <  0<=     0 <  1<=     0 <  2<=     0 <  3<=     0 <  4<=     0 <  5<=     0
HugePageFiller: <  6<=     0 <  7<=     0 <  8<=     0 < 40<=     0 < 72<=     0 <104<=     0
HugePageFiller: <136<=     0 <168<=     0 <200<=     0 <232<=     0 <264<=     0 <296<=     0
HugePageFiller: <328<=     0 <360<=     0 <392<=     0 <424<=     0 <456<=     0 <488<=     0
HugePageFiller: <504<=     0 <505<=     0 <506<=     0 <507<=     0 <508<=     0 <509<=     0
HugePageFiller: <510<=     0 <511<=     0

HugePageFiller: # of densely-accessed partial released hps with a <= # of free AND unbacked < b
HugePageFiller: <  0<=     0 <  1<=     0 <  2<=     0 <  3<=     0 <  4<=     0 <  5<=     0
HugePageFiller: <  6<=     0 <  7<=     0 <  8<=     0 < 40<=     0 < 72<=     0 <104<=     0
HugePageFiller: <136<=     0 <168<=     0 <200<=     0 <232<=     0 <264<=     0 <296<=     0
HugePageFiller: <328<=     0 <360<=     0 <392<=     0 <424<=     0 <456<=     0 <488<=     0
HugePageFiller: <504<=     0 <505<=     0 <506<=     0 <507<=     0 <508<=     0 <509<=     0
HugePageFiller: <510<=     0 <511<=     0

HugePageFiller: # of densely-accessed partial released hps with a <= # of free AND swapped < b
HugePageFiller: <  0<=     0 <  1<=     0 <  2<=     0 <  3<=     0 <  4<=     0 <  5<=     0
HugePageFiller: <  6<=     0 <  7<=     0 <  8<=     0 < 40<=     0 < 72<=     0 <104<=     0
HugePageFiller: <136<=     0 <168<=     0 <200<=     0 <232<=     0 <264<=     0 <296<=     0
HugePageFiller: <328<=     0 <360<=     0 <392<=     0 <424<=     0 <456<=     0 <488<=     0
HugePageFiller: <504<=     0 <505<=     0 <506<=     0 <507<=     0 <508<=     0 <509<=     0
HugePageFiller: <510<=     0 <511<=     0

HugePageFiller: 0 of densely-accessed partial released pages hugepage backed out of 0.
HugePageFiller: 0 of densely-accessed partial released pages treated out of 0.

HugePageFiller: Sampled Trackers for densely-accessed partial released pages:

HugePageFiller: # of sparsely-accessed released hps with a<= # of free pages <b
HugePageFiller: <  0<=     0 <  1<=     1 <  2<=     0 <  3<=     0 <  4<=     0 <  5<=     1
HugePageFiller: <  6<=     1 <  7<=     1 <  8<=     0 < 16<=     0 < 32<=     0 < 48<=     0
HugePageFiller: < 64<=     0 < 80<=     0 < 96<=     0 <112<=     0 <128<=     0 <144<=     0
HugePageFiller: <160<=     0 <176<=     0 <192<=     0 <208<=     0 <224<=     0 <240<=     0
HugePageFiller: <248<=     0 <249<=     0 <250<=     0 <251<=     0 <252<=     0 <253<=     0
HugePageFiller: <254<=     0 <255<=     0

HugePageFiller: # of sparsely-accessed released hps with a<= longest free range <b
HugePageFiller: <  0<=     0 <  1<=     1 <  2<=     0 <  3<=     0 <  4<=     0 <  5<=     1
HugePageFiller: <  6<=     1 <  7<=     1 <  8<=     0 < 16<=     0 < 32<=     0 < 48<=     0
HugePageFiller: < 64<=     0 < 80<=     0 < 96<=     0 <112<=     0 <128<=     0 <144<=     0
HugePageFiller: <160<=     0 <176<=     0 <192<=     0 <208<=     0 <224<=     0 <240<=     0
HugePageFiller: <248<=     0 <249<=     0 <250<=     0 <251<=     0 <252<=     0 <253<=     0
HugePageFiller: <254<=     0 <255<=     0

HugePageFiller: # of sparsely-accessed released hps with a<= # of allocations <b
HugePageFiller: <  1<=     3 <  2<=     0 <  3<=     0 <  4<=     1 <  5<=     0 <  6<=     0
HugePageFiller: <  7<=     0 <  8<=     0 <  9<=     0 < 17<=     0 < 33<=     0 < 49<=     0
HugePageFiller: < 65<=     0 < 81<=     0 < 97<=     0 <113<=     0 <129<=     0 <145<=     0
HugePageFiller: <161<=     0 <177<=     0 <193<=     0 <209<=     0 <225<=     0 <241<=     0
HugePageFiller: <249<=     0 <250<=     0 <251<=     0 <252<=     0 <253<=     0 <254<=     0
HugePageFiller: <255<=     0 <256<=     0

HugePageFiller: # of sparsely-accessed released hps with live lifetime a <= # hps < b
HugePageFiller: <   0 ms <=      4 <   1 ms <=      0 <  10 ms <=      0 < 100 ms <=      0 < 1000 ms <=      0 < 10000 ms <=      0
HugePageFiller: < 100000 ms <=      0 < 1000000 ms <=      0

HugePageFiller: # of hps with >= 224 free pages, with different lifetimes.
HugePageFiller: # of sparsely-accessed released hps with lifetime a <= # hps < b
HugePageFiller: <   0 ms <=      0 <   1 ms <=      0 <  10 ms <=      0 < 100 ms <=      0 < 1000 ms <=      0 < 10000 ms <=      0
HugePageFiller: < 100000 ms <=      0 < 1000000 ms <=      0

HugePageFiller: # of hps with lifetime >= 100000 ms.
HugePageFiller: # of sparsely-accessed released hps with a <= # of allocations < b
HugePageFiller: <  0<=     0 <  1<=     0 <  2<=     0 <  3<=     0 <  4<=     0 <  5<=     0
HugePageFiller: <  6<=     0 <  7<=     0 <  8<=     0 < 16<=     0 < 32<=     0 < 48<=     0
HugePageFiller: < 64<=     0 < 80<=     0 < 96<=     0 <112<=     0 <128<=     0 <144<=     0
HugePageFiller: <160<=     0 <176<=     0 <192<=     0 <208<=     0 <224<=     0 <240<=     0
HugePageFiller: <248<=     0 <249<=     0 <250<=     0 <251<=     0 <252<=     0 <253<=     0
HugePageFiller: <254<=     0 <255<=     0

HugePageFiller: # of sparsely-accessed released hps with a <= # of unbacked < b
HugePageFiller: <  0<=     0 <  1<=     0 <  2<=     0 <  3<=     0 <  4<=     0 <  5<=     0
HugePageFiller: <  6<=     0 <  7<=     0 <  8<=     0 < 40<=     0 < 72<=     0 <104<=     0
HugePageFiller: <136<=     0 <168<=     0 <200<=     0 <232<=     0 <264<=     0 <296<=     0
HugePageFiller: <328<=     0 <360<=     0 <392<=     0 <424<=     0 <456<=     0 <488<=     0
HugePageFiller: <504<=     0 <505<=     0 <506<=     0 <507<=     0 <508<=     0 <509<=     0
HugePageFiller: <510<=     0 <511<=     0

HugePageFiller: # of sparsely-accessed released hps with a <= # of swapped < b
HugePageFiller: <  0<=     0 <  1<=     0 <  2<=     0 <  3<=     0 <  4<=     0 <  5<=     0
HugePageFiller: <  6<=     0 <  7<=     0 <  8<=     0 < 40<=     0 < 72<=     0 <104<=     0
HugePageFiller: <136<=     0 <168<=     0 <200<=     0 <232<=     0 <264<=     0 <296<=     0
HugePageFiller: <328<=     0 <360<=     0 <392<=     0 <424<=     0 <456<=     0 <488<=     0
HugePageFiller: <504<=     0 <505<=     0 <506<=     0 <507<=     0 <508<=     0 <509<=     0
HugePageFiller: <510<=     0 <511<=     0

HugePageFiller: # of sparsely-accessed released hps with a <= # of free AND unbacked < b
HugePageFiller: <  0<=     0 <  1<=     0 <  2<=     0 <  3<=     0 <  4<=     0 <  5<=     0
HugePageFiller: <  6<=     0 <  7<=     0 <  8<=     0 < 40<=     0 < 72<=     0 <104<=     0
HugePageFiller: <136<=     0 <168<=     0 <200<=     0 <232<=     0 <264<=     0 <296<=     0
HugePageFiller: <328<=     0 <360<=     0 <392<=     0 <424<=     0 <456<=     0 <488<=     0
HugePageFiller: <504<=     0 <505<=     0 <506<=     0 <507<=     0 <508<=     0 <509<=     0
HugePageFiller: <510<=     0 <511<=     0

HugePageFiller: # of sparsely-accessed released hps with a <= # of free AND swapped < b
HugePageFiller: <  0<=     0 <  1<=     0 <  2<=     0 <  3<=     0 <  4<=     0 <  5<=     0
HugePageFiller: <  6<=     0 <  7<=     0 <  8<=     0 < 40<=     0 < 72<=     0 <104<=     0
HugePageFiller: <136<=     0 <168<=     0 <200<=     0 <232<=     0 <264<=     0 <296<=     0
HugePageFiller: <328<=     0 <360<=     0 <392<=     0 <424<=     0 <456<=     0 <488<=     0
HugePageFiller: <504<=     0 <505<=     0 <506<=     0 <507<=     0 <508<=     0 <509<=     0
HugePageFiller: <510<=     0 <511<=     0

HugePageFiller: 0 of sparsely-accessed released pages hugepage backed out of 4.
HugePageFiller: 0 of sparsely-accessed released pages treated out of 4.

HugePageFiller: Sampled Trackers for sparsely-accessed released pages:

HugePageFiller: # of densely-accessed released hps with a<= # of free pages <b
HugePageFiller: <  0<=     0 <  1<=     0 <  2<=     0 <  3<=     0 <  4<=     0 <  5<=     0
HugePageFiller: <  6<=     0 <  7<=     0 <  8<=     0 < 16<=     1 < 32<=     0 < 48<=     0
HugePageFiller: < 64<=     0 < 80<=     0 < 96<=     0 <112<=     0 <128<=     0 <144<=     0
HugePageFiller: <160<=     0 <176<=     0 <192<=     0 <208<=     0 <224<=     0 <240<=     0
HugePageFiller: <248<=     0 <249<=     0 <250<=     0 <251<=     0 <252<=     0 <253<=     0
HugePageFiller: <254<=     0 <255<=     0

HugePageFiller: # of densely-accessed released hps with a<= longest free range <b
HugePageFiller: <  0<=     0 <  1<=     0 <  2<=     0 <  3<=     0 <  4<=     0 <  5<=     0
HugePageFiller: <  6<=     0 <  7<=     0 <  8<=     0 < 16<=     1 < 32<=     0 < 48<=     0
HugePageFiller: < 64<=     0 < 80<=     0 < 96<=     0 <112<=     0 <128<=     0 <144<=     0
HugePageFiller: <160<=     0 <176<=     0 <192<=     0 <208<=     0 <224<=     0 <240<=     0
HugePageFiller: <248<=     0 <249<=     0 <250<=     0 <251<=     0 <252<=     0 <253<=     0
HugePageFiller: <254<=     0 <255<=     0

HugePageFiller: # of densely-accessed released hps with a<= # of allocations <b
HugePageFiller: <  1<=     0 <  2<=     0 <  3<=     0 <  4<=     0 <  5<=     0 <  6<=     0
HugePageFiller: <  7<=     0 <  8<=     0 <  9<=     0 < 17<=     0 < 33<=     0 < 49<=     0
HugePageFiller: < 65<=     0 < 81<=     0 < 97<=     0 <113<=     0 <129<=     0 <145<=     0
HugePageFiller: <161<=     0 <177<=     0 <193<=     0 <209<=     0 <225<=     1 <241<=     0
HugePageFiller: <249<=     0 <250<=     0 <251<=     0 <252<=     0 <253<=     0 <254<=     0
HugePageFiller: <255<=     0 <256<=     0

HugePageFiller: # of densely-accessed released hps with live lifetime a <= # hps < b
HugePageFiller: <   0 ms <=      1 <   1 ms <=      0 <  10 ms <=      0 < 100 ms <=      0 < 1000 ms <=      0 < 10000 ms <=      0
HugePageFiller: < 100000 ms <=      0 < 1000000 ms <=      0

HugePageFiller: # of hps with >= 224 free pages, with different lifetimes.
HugePageFiller: # of densely-accessed released hps with lifetime a <= # hps < b
HugePageFiller: <   0 ms <=      0 <   1 ms <=      0 <  10 ms <=      0 < 100 ms <=      0 < 1000 ms <=      0 < 10000 ms <=      0
HugePageFiller: < 100000 ms <=      0 < 1000000 ms <=      0

HugePageFiller: # of hps with lifetime >= 100000 ms.
HugePageFiller: # of densely-accessed released hps with a <= # of allocations < b
HugePageFiller: <  0<=     0 <  1<=     0 <  2<=     0 <  3<=     0 <  4<=     0 <  5<=     0
HugePageFiller: <  6<=     0 <  7<=     0 <  8<=     0 < 16<=     0 < 32<=     0 < 48<=     0
HugePageFiller: < 64<=     0 < 80<=     0 < 96<=     0 <112<=     0 <128<=     0 <144<=     0
HugePageFiller: <160<=     0 <176<=     0 <192<=     0 <208<=     0 <224<=     0 <240<=     0
HugePageFiller: <248<=     0 <249<=     0 <250<=     0 <251<=     0 <252<=     0 <253<=     0
HugePageFiller: <254<=     0 <255<=     0

HugePageFiller: # of densely-accessed released hps with a <= # of unbacked < b
HugePageFiller: <  0<=     0 <  1<=     0 <  2<=     0 <  3<=     0 <  4<=     0 <  5<=     0
HugePageFiller: <  6<=     0 <  7<=     0 <  8<=     0 < 40<=     0 < 72<=     0 <104<=     0
HugePageFiller: <136<=     0 <168<=     0 <200<=     0 <232<=     0 <264<=     0 <296<=     0
HugePageFiller: <328<=     0 <360<=     0 <392<=     0 <424<=     0 <456<=     0 <488<=     0
HugePageFiller: <504<=     0 <505<=     0 <506<=     0 <507<=     0 <508<=     0 <509<=     0
HugePageFiller: <510<=     0 <511<=     0

HugePageFiller: # of densely-accessed released hps with a <= # of swapped < b
HugePageFiller: <  0<=     0 <  1<=     0 <  2<=     0 <  3<=     0 <  4<=     0 <  5<=     0
HugePageFiller: <  6<=     0 <  7<=     0 <  8<=     0 < 40<=     0 < 72<=     0 <104<=     0
HugePageFiller: <136<=     0 <168<=     0 <200<=     0 <232<=     0 <264<=     0 <296<=     0
HugePageFiller: <328<=     0 <360<=     0 <392<=     0 <424<=     0 <456<=     0 <488<=     0
HugePageFiller: <504<=     0 <505<=     0 <506<=     0 <507<=     0 <508<=     0 <509<=     0
HugePageFiller: <510<=     0 <511<=     0

HugePageFiller: # of densely-accessed released hps with a <= # of free AND unbacked < b
HugePageFiller: <  0<=     0 <  1<=     0 <  2<=     0 <  3<=     0 <  4<=     0 <  5<=     0
HugePageFiller: <  6<=     0 <  7<=     0 <  8<=     0 < 40<=     0 < 72<=     0 <104<=     0
HugePageFiller: <136<=     0 <168<=     0 <200<=     0 <232<=     0 <264<=     0 <296<=     0
HugePageFiller: <328<=     0 <360<=     0 <392<=     0 <424<=     0 <456<=     0 <488<=     0
HugePageFiller: <504<=     0 <505<=     0 <506<=     0 <507<=     0 <508<=     0 <509<=     0
HugePageFiller: <510<=     0 <511<=     0

HugePageFiller: # of densely-accessed released hps with a <= # of free AND swapped < b
HugePageFiller: <  0<=     0 <  1<=     0 <  2<=     0 <  3<=     0 <  4<=     0 <  5<=     0
HugePageFiller: <  6<=     0 <  7<=     0 <  8<=     0 < 40<=     0 < 72<=     0 <104<=     0
HugePageFiller: <136<=     0 <168<=     0 <200<=     0 <232<=     0 <264<=     0 <296<=     0
HugePageFiller: <328<=     0 <360<=     0 <392<=     0 <424<=     0 <456<=     0 <488<=     0
HugePageFiller: <504<=     0 <505<=     0 <506<=     0 <507<=     0 <508<=     0 <509<=     0
HugePageFiller: <510<=     0 <511<=     0

HugePageFiller: 0 of densely-accessed released pages hugepage backed out of 1.
HugePageFiller: 0 of densely-accessed released pages treated out of 1.

HugePageFiller: Sampled Trackers for densely-accessed released pages:

HugePageFiller: 0 hugepages became full after being previously released, out of which 0 pages are hugepage backed.

HugePageFiller: # of densely-accessed hps with completed lifetime a <= # hps < b
HugePageFiller: <   0 ms <=      0 <   1 ms <=      0 <  10 ms <=      0 < 100 ms <=      0 < 1000 ms <=      0 < 10000 ms <=      0
HugePageFiller: < 100000 ms <=      0 < 1000000 ms <=      0

HugePageFiller: # of sparsely-accessed hps with completed lifetime a <= # hps < b
HugePageFiller: <   0 ms <=      1 <   1 ms <=      0 <  10 ms <=      0 < 100 ms <=      0 < 1000 ms <=      0 < 10000 ms <=      0
HugePageFiller: < 100000 ms <=      0 < 1000000 ms <=      0

HugePageFiller: time series over 5 min interval

HugePageFiller: realized fragmentation: 0.0 MiB
HugePageFiller: minimum free pages: 0 (0 backed)
HugePageFiller: at peak demand: 3547 pages (and 255 free, 38 unmapped)
HugePageFiller: at peak demand: 15 hps (9 regular, 1 donated, 0 partial, 5 released)

HugePageFiller: Since the start of the execution, 0 subreleases (0 pages) were skipped due to either recent (0s) peaks, or the sum of short-term (0s) fluctuations and long-term (0s) trends.
HugePageFiller: 0.0000% of decisions confirmed correct, 0 pending (0.0000% of pages, 0 pending), as per anticipated 0s realized fragmentation.
HugePageFiller: Subrelease stats last 10 min: total 306 pages subreleased (0 pages from partial allocs), 6 hugepages broken
)"));

  absl::flat_hash_set<const PageTracker*> expected_pts, actual_pts;
  for (const auto& alloc : allocs) {
    expected_pts.insert(alloc.pt);
  }
  actual_pts.reserve(expected_pts.size());

  bool dupe_seen = false;
  {
    PageHeapSpinLockHolder l;
    filler_.ForEachHugePage([&](const PageTracker& pt) {
      // We are holding the page heap lock, so refrain from allocating
      // (including using Google Test helpers).
      dupe_seen = dupe_seen || actual_pts.contains(&pt);

      if (actual_pts.size() == actual_pts.capacity()) {
        return;
      }

      TC_CHECK(actual_pts.insert(&pt).second);
    });
  }
  EXPECT_FALSE(dupe_seen);
  EXPECT_THAT(actual_pts, Eq(expected_pts));

  for (const auto& alloc : allocs) {
    Delete(alloc);
  }
}

// Test Get and Put operations on the filler work correctly when number of
// objects are provided.  We expect that Get requests with sparsely-accessed
// and densely-accessed spans are satisfied by their respective allocs.
TEST_P(FillerTest, GetsAndPuts) {
  randomize_density_ = false;
  absl::BitGen rng;
  std::vector<PAlloc> sparsely_accessed_allocs;
  std::vector<PAlloc> densely_accessed_allocs;
  SpanAllocInfo sparsely_accessed_info = {1, AccessDensityPrediction::kSparse};
  SpanAllocInfo densely_accessed_info = {kMaxValidPages.raw_num(),
                                         AccessDensityPrediction::kDense};
  static const HugeLength kNumHugePages = NHugePages(64);
  for (auto i = Length(0); i < kNumHugePages.in_pages(); ++i) {
    ASSERT_EQ(filler_.pages_allocated(), i);
    // Randomly select whether the next span should be sparsely-accessed or
    // densely-accessed.
    if (absl::Bernoulli(rng, 0.5)) {
      sparsely_accessed_allocs.push_back(
          AllocateWithSpanAllocInfo(Length(1), sparsely_accessed_info));
      EXPECT_EQ(
          filler_.pages_allocated(AccessDensityPrediction::kSparse).raw_num(),
          sparsely_accessed_allocs.size());
    } else {
      densely_accessed_allocs.push_back(
          AllocateWithSpanAllocInfo(Length(1), densely_accessed_info));
      EXPECT_EQ(
          filler_.pages_allocated(AccessDensityPrediction::kDense).raw_num(),
          densely_accessed_allocs.size());
    }
  }
  EXPECT_GE(filler_.size(), kNumHugePages);
  EXPECT_LE(filler_.size(), kNumHugePages + NHugePages(1));
  // clean up, check for failures
  for (auto a : densely_accessed_allocs) {
    Delete(a);
  }
  ASSERT_EQ(filler_.pages_allocated(AccessDensityPrediction::kDense),
            Length(0));
  for (auto a : sparsely_accessed_allocs) {
    Delete(a);
  }
  ASSERT_EQ(filler_.pages_allocated(AccessDensityPrediction::kSparse),
            Length(0));
  ASSERT_EQ(filler_.pages_allocated(), Length(0));
}

// Test that filler tries to release pages from the sparsely - accessed allocs
// before attempting to release pages from the densely - accessed allocs.
TEST_P(FillerTest, ReleasePrioritySparseAndDenseAllocs) {
  randomize_density_ = false;
  const Length N = kPagesPerHugePage;
  const Length kToBeReleased(4);
  SpanAllocInfo sparsely_accessed_info = {1, AccessDensityPrediction::kSparse};
  auto sparsely_accessed_alloc = AllocateVectorWithSpanAllocInfo(
      N - kToBeReleased, sparsely_accessed_info);
  ASSERT_EQ(sparsely_accessed_alloc.size(), 1);
  SpanAllocInfo densely_accessed_info = {kMaxValidPages.raw_num(),
                                         AccessDensityPrediction::kDense};
  auto densely_accessed_alloc =
      AllocateVectorWithSpanAllocInfo(N - kToBeReleased, densely_accessed_info);
  for (auto a : densely_accessed_alloc) {
    ASSERT_EQ(a.pt, densely_accessed_alloc.front().pt);
  }
  EXPECT_EQ(ReleasePages(Length(1)), kToBeReleased);
  auto get_released_pages = [&](const std::vector<PAlloc>& alloc) {
    return alloc.front().pt->released_pages();
  };
  EXPECT_EQ(get_released_pages(sparsely_accessed_alloc), kToBeReleased);
  EXPECT_EQ(get_released_pages(densely_accessed_alloc), Length(0));
  EXPECT_EQ(ReleasePages(Length(1)), kToBeReleased);
  EXPECT_EQ(get_released_pages(densely_accessed_alloc), kToBeReleased);
  EXPECT_EQ(get_released_pages(sparsely_accessed_alloc), kToBeReleased);
  DeleteVector(sparsely_accessed_alloc);
  DeleteVector(densely_accessed_alloc);
}

// Repeatedly grow from FLAG_bytes to FLAG_bytes * growth factor, then shrink
// back down by random deletion. Then release partial hugepages until
// pageheap is bounded by some fraction of usage.  Measure the blowup in VSS
// footprint.
TEST_P(FillerTest, BoundedVSS) {
  randomize_density_ = false;
  absl::BitGen rng;
  const Length baseline = LengthFromBytes(absl::GetFlag(FLAGS_bytes));
  const Length peak = baseline * absl::GetFlag(FLAGS_growth_factor);

  std::vector<PAlloc> allocs;
  while (filler_.used_pages() < baseline) {
    allocs.push_back(Allocate(Length(1)));
  }
  EXPECT_EQ(filler_.pages_allocated().raw_num(), allocs.size());

  for (int i = 0; i < 10; ++i) {
    while (filler_.used_pages() < peak) {
      allocs.push_back(Allocate(Length(1)));
    }
    std::shuffle(allocs.begin(), allocs.end(), rng);
    size_t limit = allocs.size();
    while (filler_.used_pages() > baseline) {
      --limit;
      Delete(allocs[limit]);
    }
    allocs.resize(limit);
    ReleasePages(kMaxValidPages);
    // Compare the total size of the hugepages in the filler and the allocated
    // pages.
    EXPECT_LE(filler_.size().in_bytes(),
              2 * filler_.pages_allocated().in_bytes());
  }
  while (!allocs.empty()) {
    Delete(allocs.back());
    allocs.pop_back();
  }
}

// In b/265337869, we observed failures in the huge_page_filler due to mixing
// of hugepages between sparsely-accessed and densely-accessed allocs. The
// test below reproduces the buggy situation.
TEST_P(FillerTest, CounterUnderflow) {
  randomize_density_ = false;
  const Length N = kPagesPerHugePage;
  const Length kToBeReleased(kPagesPerHugePage / 2 + Length(1));
  // First allocate a densely-accessed span, then release the remaining pages
  // on the hugepage.  This would move the hugepage to
  // regular_alloc_partial_released_.
  SpanAllocInfo densely_accessed_info = {kMaxValidPages.raw_num(),
                                         AccessDensityPrediction::kDense};
  auto densely_accessed_alloc =
      AllocateVectorWithSpanAllocInfo(N - kToBeReleased, densely_accessed_info);
  EXPECT_EQ(ReleasePages(Length(kToBeReleased)), kToBeReleased);
  // Then allocate a sparsely-accessed objects span.  The previous hugepage
  // should not be used since while allocating a sparsely-accessed objects
  // span, we do not check densely-accessed alloc.
  SpanAllocInfo sparsely_accessed_info = {1, AccessDensityPrediction::kSparse};
  auto sparsely_accessed_alloc = AllocateVectorWithSpanAllocInfo(
      Length(kToBeReleased), sparsely_accessed_info);
  for (const auto& a1 : sparsely_accessed_alloc) {
    for (const auto& a2 : densely_accessed_alloc) {
      EXPECT_NE(a1.pt, a2.pt);
    }
  }
  DeleteVector(sparsely_accessed_alloc);
  DeleteVector(densely_accessed_alloc);
}

// In b/270916852, we observed that the huge_page_filler may fail to release
// memory when densely-accessed alloc is being used.  This is due to the
// presence of partially released and fully released pages in densely-accessed
// alloc.  The comparator in use does not make correct choices in presence of
// such hugepages.  The test below reproduces the buggy situation.
TEST_P(FillerTest, ReleasePagesFromDenseAlloc) {
  randomize_density_ = false;
  constexpr size_t kCandidatesForReleasingMemory =
      HugePageFiller<PageTracker>::kCandidatesForReleasingMemory;
  // Allocate all pages from kCandidate hugepages. We will eventually delete
  // kPagesPerHugepage/2 - 1 pages from them.
  const Length kToBeUsed1(kPagesPerHugePage / 2 + Length(1));
  std::vector<std::vector<PAlloc>> allocs;
  SpanAllocInfo densely_accessed_info = {kMaxValidPages.raw_num(),
                                         AccessDensityPrediction::kDense};
  for (int i = 0; i < kCandidatesForReleasingMemory; ++i) {
    std::vector<PAlloc> temp = AllocateVectorWithSpanAllocInfo(
        kPagesPerHugePage, densely_accessed_info);
    allocs.insert(allocs.end(), temp);
  }
  // Allocate all pages from kCandidate (does not really matter) more
  // hugepages. We will eventually delete kPagesPerHugepage/2 - 2 pages from
  // these. These allocations also need one fresh hugepage each and they use
  // more pages than the previously allocated hugepages.
  const Length kToBeUsed2(kPagesPerHugePage / 2 + Length(2));
  for (int i = 0; i < kCandidatesForReleasingMemory; ++i) {
    std::vector<PAlloc> temp = AllocateVectorWithSpanAllocInfo(
        kPagesPerHugePage, densely_accessed_info);
    allocs.insert(allocs.end(), temp);
  }
  // Delete the allocations that we would like to release.
  for (int i = 0; i < kCandidatesForReleasingMemory; ++i) {
    DeleteRange(allocs[i].begin() + kToBeUsed1.raw_num(), allocs[i].end());
    allocs[i].erase(allocs[i].begin() + kToBeUsed1.raw_num(), allocs[i].end());
  }
  // Release the free portion from these hugepages.
  const Length kExpectedReleased1 =
      kCandidatesForReleasingMemory * (kPagesPerHugePage - kToBeUsed1);
  EXPECT_EQ(ReleasePages(kExpectedReleased1), kExpectedReleased1);
  // Delete the allocations that we would like to release.
  for (int i = kCandidatesForReleasingMemory;
       i < 2 * kCandidatesForReleasingMemory; ++i) {
    DeleteRange(allocs[i].begin() + kToBeUsed2.raw_num(), allocs[i].end());
    allocs[i].erase(allocs[i].begin() + kToBeUsed2.raw_num(), allocs[i].end());
  }
  // Try to release more memory.  We should continue to make progress and
  // return all of the pages we tried to.
  const Length kExpectedReleased2 =
      kCandidatesForReleasingMemory * (kPagesPerHugePage - kToBeUsed2);
  EXPECT_EQ(ReleasePages(kExpectedReleased2), kExpectedReleased2);
  EXPECT_EQ(filler_.free_pages(), Length(0));

  for (auto alloc : allocs) {
    DeleteVector(alloc);
  }
}

TEST_P(FillerTest, ReleasePagesFromDenseAlloc_SpansAllocated) {
  randomize_density_ = false;
  constexpr size_t kCandidatesForReleasingMemory =
      HugePageFiller<PageTracker>::kCandidatesForReleasingMemory;
  // Make kCandidate memory allocations of length kPagesPerHugepage/2 + 1.
  // Note that a fresh hugepage will be used for each alloction.
  const Length kToBeUsed1(kPagesPerHugePage / 2 + Length(1));
  std::vector<PAlloc> allocs;
  std::vector<PAlloc> allocs_to_be_released;
  SpanAllocInfo densely_accessed_info = {kMaxValidPages.raw_num(),
                                         AccessDensityPrediction::kDense};
  for (int i = 0; i < kCandidatesForReleasingMemory; ++i) {
    std::vector<PAlloc> temp =
        AllocateVectorWithSpanAllocInfo(kToBeUsed1, densely_accessed_info);
    allocs.insert(allocs.end(), temp.begin(), temp.end());
    temp = AllocateVectorWithSpanAllocInfo(kPagesPerHugePage - kToBeUsed1,
                                           densely_accessed_info);
    allocs_to_be_released.insert(allocs_to_be_released.end(), temp.begin(),
                                 temp.end());
  }
  // Release the allocs that were made so that the actual we care about are on
  // fresh hugepages.
  DeleteVector(allocs_to_be_released);
  allocs_to_be_released.clear();
  // Release the free portion from these hugepages.
  const Length kExpectedReleased1 =
      kCandidatesForReleasingMemory * (kPagesPerHugePage - kToBeUsed1);
  EXPECT_EQ(ReleasePages(kExpectedReleased1), kExpectedReleased1);
  // Fill up the hugepages again so that subsequent allocations are made on
  // fresh hugepages.
  for (int i = 0; i < kCandidatesForReleasingMemory; ++i) {
    std::vector<PAlloc> temp = AllocateVectorWithSpanAllocInfo(
        kPagesPerHugePage - kToBeUsed1, densely_accessed_info);
    allocs_to_be_released.insert(allocs_to_be_released.end(), temp.begin(),
                                 temp.end());
  }
  //  Allocate kCandidate (does not really matter) more hugepages with
  //  allocations of length kPagesPerHugepage/2 + 2. These allocations also
  //  need one fresh hugepage each and they use more pages than the previously
  //  allocated hugepages.
  std::vector<PAlloc> allocs_to_be_released_2;
  const Length kToBeUsed2(kPagesPerHugePage / 2 + Length(2));
  for (int i = 0; i < kCandidatesForReleasingMemory; ++i) {
    std::vector<PAlloc> temp =
        AllocateVectorWithSpanAllocInfo(kToBeUsed2, densely_accessed_info);
    allocs.insert(allocs.end(), temp.begin(), temp.end());
    temp = AllocateVectorWithSpanAllocInfo(kPagesPerHugePage - kToBeUsed2,
                                           densely_accessed_info);
    allocs_to_be_released_2.insert(allocs_to_be_released_2.end(), temp.begin(),
                                   temp.end());
  }
  // Release the allocs that were made so that the actual we care about are on
  // fresh hugepages.
  DeleteVector(allocs_to_be_released_2);
  allocs_to_be_released_2.clear();
  // Try to release more memory.  We should continue to make progress and
  // return all of the pages we tried to.
  const Length kExpectedReleased2 =
      kCandidatesForReleasingMemory * (kPagesPerHugePage - kToBeUsed2);
  EXPECT_EQ(ReleasePages(kExpectedReleased2), kExpectedReleased2);
  EXPECT_EQ(filler_.free_pages(), Length(0));

  for (auto alloc : allocs) {
    Delete(alloc);
  }
  DeleteVector(allocs_to_be_released);
}

TEST_P(FillerTest, ReleasedPagesStatistics) {
  constexpr Length N = kPagesPerHugePage / 4;

  std::vector<PAlloc> a1 = AllocateVector(N);
  ASSERT_TRUE(!a1.empty());

  const Length released = ReleasePages(kPagesPerHugePage);
  // We should have released some memory.
  EXPECT_NE(released, Length(0));
  // Since we have only a single allocation, its pages should all be used on
  // released pages.
  EXPECT_EQ(filler_.size(), NHugePages(1));
  EXPECT_EQ(filler_.used_pages(), N);
  EXPECT_EQ(filler_.free_pages(), Length(0));
  EXPECT_EQ(filler_.used_pages_in_released(), N);
  EXPECT_EQ(filler_.used_pages_in_any_subreleased(), N);

  // Now differentiate fully released from partially released.  Make an
  // allocation and return it.
  std::vector<PAlloc> a2 =
      AllocateVectorWithSpanAllocInfo(N, a1.front().span_alloc_info);

  // We now have N pages for a1, N pages for a2, and 2N pages
  // released.
  EXPECT_EQ(filler_.size(), NHugePages(1));
  EXPECT_EQ(filler_.used_pages(), 2 * N);
  EXPECT_EQ(filler_.free_pages(), Length(0));
  EXPECT_EQ(filler_.used_pages_in_released(), 2 * N);
  EXPECT_EQ(filler_.used_pages_in_any_subreleased(), 2 * N);

  DeleteVector(a2);

  // We now have N pages for a1, N pages free (but mapped), and 2N pages
  // released.
  EXPECT_EQ(filler_.used_pages(), N);
  EXPECT_EQ(filler_.free_pages(), N);
  EXPECT_EQ(filler_.used_pages_in_released(), Length(0));
  EXPECT_EQ(filler_.used_pages_in_any_subreleased(), N);

  DeleteVector(a1);
}

INSTANTIATE_TEST_SUITE_P(All, FillerTest, ::testing::Bool());

TEST(SkipSubreleaseIntervalsTest, EmptyIsNotEnabled) {
  // When we have a limit hit, we pass SkipSubreleaseIntervals{} to the
  // filler. Make sure it doesn't signal that we should skip the limit.
  EXPECT_FALSE(SkipSubreleaseIntervals{}.SkipSubreleaseEnabled());
}

}  // namespace
}  // namespace tcmalloc_internal
}  // namespace tcmalloc
