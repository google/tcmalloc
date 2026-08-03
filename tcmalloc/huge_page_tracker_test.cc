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

#include "tcmalloc/huge_page_tracker.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdint>
#include <iterator>
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
#include "tcmalloc/testing/testutil.h"

using tcmalloc::tcmalloc_internal::Length;

ABSL_FLAG(Length, page_tracker_defrag_lim, Length(32),
          "Max allocation size for defrag test");

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
      TC_CHECK_LT(actual_index_, std::size(actual_));
      actual_[actual_index_].r = r;
      TC_CHECK_LT(actual_index_, std::size(expected_));
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
    return PageStats{};
  }

  PageFlagsBitmaps GetSinglePageBitmaps(const void* addr) override {
    PageFlagsBitmaps ret;
    ret.stale.SetBit(0);
    ret.status = absl::StatusCode::kOk;
    return ret;
  }

  void MarkHugePageBacked(void* addr, bool is_hugepage_backed) {
    PageId p = PageIdContaining(addr);
    HugePage hp = HugePageContaining(p);
    is_hugepage_backed_[hp.start_addr()] = is_hugepage_backed;
  }

  void MarkHugePageBackedUnknown(void* addr) {
    PageId p = PageIdContaining(addr);
    HugePage hp = HugePageContaining(p);
    is_hugepage_backed_[hp.start_addr()] = std::nullopt;
  }

  std::optional<bool> IsHugepageBacked(const void* addr) override {
    PageId p = PageIdContaining(addr);
    HugePage hp = HugePageContaining(p);
    auto it = is_hugepage_backed_.find(hp.start_addr());
    if (it == is_hugepage_backed_.end()) {
      return false;
    }
    return it->second;
  }

 private:
  absl::flat_hash_map<const void*, std::optional<bool>> is_hugepage_backed_;
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

  const size_t kHardwarePagesInHugePage = kHugePageSize / kPageSize;
  size_t GetHardwarePagesInHugePage() const override {
    return kHardwarePagesInHugePage;
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

  static const size_t kReps = 25 * 1000;

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

}  // namespace
}  // namespace tcmalloc_internal
}  // namespace tcmalloc
