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

#include <stdio.h>
#include <string.h>

#include <cstdint>
#include <iterator>
#include <memory>
#include <new>
#include <random>
#include <string>
#include <thread>  // NOLINT(build/c++11)
#include <utility>
#include <vector>

#include "benchmark/benchmark.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "absl/algorithm/container.h"
#include "absl/base/internal/sysinfo.h"
#include "absl/base/thread_annotations.h"
#include "absl/container/flat_hash_map.h"
#include "absl/container/flat_hash_set.h"
#include "absl/flags/flag.h"
#include "absl/memory/memory.h"
#include "absl/random/bernoulli_distribution.h"
#include "absl/random/random.h"
#include "absl/synchronization/blocking_counter.h"
#include "absl/synchronization/mutex.h"
#include "absl/time/clock.h"
#include "absl/time/time.h"
#include "tcmalloc/common.h"
#include "tcmalloc/internal/logging.h"
#include "tcmalloc/stats.h"

ABSL_FLAG(uint64_t, page_tracker_defrag_lim, 32,
          "Max allocation size for defrag test");

ABSL_FLAG(uint64_t, frag_req_limit, 32, "request size limit for frag test");
ABSL_FLAG(uint64_t, frag_size, 512 * 1024,
          "target number of pages for frag test");
ABSL_FLAG(uint64_t, frag_iters, 10 * 1000 * 1000, "iterations for frag test");

ABSL_FLAG(double, release_until, 0.01,
          "fraction of used we target in pageheap");
ABSL_FLAG(uint64_t, bytes, 1024 * 1024 * 1024, "baseline usage");
ABSL_FLAG(double, growth_factor, 2.0, "growth over baseline");

namespace tcmalloc {
namespace {

// This is an arbitrary distribution taken from page requests from
// an empirical driver test.  It seems realistic enough. We trim it to
// [1, last].
//
std::discrete_distribution<int> EmpiricalDistribution(Length last) {
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

  auto j = page_counts.size() > lim ? i + (lim + 1) : page_counts.end();

  return std::discrete_distribution<int>(i, j);
}

class PageTrackerTest : public testing::Test {
 protected:
  PageTrackerTest()
      :  // an unlikely magic page
        huge_(HugePageContaining(reinterpret_cast<void *>(0x1abcde200000))),
        tracker_(huge_, absl::base_internal::CycleClock::Now()) {}

  ~PageTrackerTest() override { mock_.VerifyAndClear(); }

  struct PAlloc {
    PageID p;
    Length n;
  };

  void Mark(PAlloc a, size_t mark) {
    EXPECT_LE(huge_.first_page(), a.p);
    size_t index = a.p - huge_.first_page();
    size_t end = index + a.n;
    EXPECT_LE(end, kPagesPerHugePage);
    for (; index < end; ++index) {
      marks_[index] = mark;
    }
  }

  class MockUnbackInterface {
   public:
    void Unback(void *p, size_t len) {
      CHECK_CONDITION(actual_index_ < kMaxCalls);
      actual_[actual_index_] = {p, len};
      ++actual_index_;
    }

    void Expect(void *p, size_t len) {
      CHECK_CONDITION(expected_index_ < kMaxCalls);
      expected_[expected_index_] = {p, len};
      ++expected_index_;
    }

    void VerifyAndClear() {
      EXPECT_EQ(expected_index_, actual_index_);
      for (size_t i = 0; i < expected_index_; ++i) {
        EXPECT_EQ(expected_[i].ptr, actual_[i].ptr);
        EXPECT_EQ(expected_[i].len, actual_[i].len);
      }
      memset(expected_, 0, sizeof(expected_));
      memset(actual_, 0, sizeof(actual_));
      expected_index_ = 0;
      actual_index_ = 0;
    }

   private:
    struct CallArgs {
      void *ptr{nullptr};
      size_t len{0};
    };

    static constexpr size_t kMaxCalls = 10;
    CallArgs expected_[kMaxCalls] = {};
    CallArgs actual_[kMaxCalls] = {};
    size_t expected_index_{0};
    size_t actual_index_{0};
  };

  static void MockUnback(void *p, size_t len);

  typedef PageTracker<MockUnback> TestPageTracker;

  // strict because release calls should only happen when we ask
  static MockUnbackInterface mock_;

  void Check(PAlloc a, size_t mark) {
    EXPECT_LE(huge_.first_page(), a.p);
    size_t index = a.p - huge_.first_page();
    size_t end = index + a.n;
    EXPECT_LE(end, kPagesPerHugePage);
    for (; index < end; ++index) {
      EXPECT_EQ(mark, marks_[index]);
    }
  }
  size_t marks_[kPagesPerHugePage];
  HugePage huge_;
  TestPageTracker tracker_;

  void ExpectPages(PAlloc a) {
    void *ptr = reinterpret_cast<void *>(a.p << kPageShift);
    size_t bytes = a.n << kPageShift;
    mock_.Expect(ptr, bytes);
  }

  PAlloc Get(Length n) {
    absl::base_internal::SpinLockHolder l(&pageheap_lock);
    PageID p = tracker_.Get(n).page;
    return {p, n};
  }

  void Put(PAlloc a) {
    absl::base_internal::SpinLockHolder l(&pageheap_lock);
    tracker_.Put(a.p, a.n);
  }

  size_t ReleaseFree() {
    absl::base_internal::SpinLockHolder l(&pageheap_lock);
    return tracker_.ReleaseFree();
  }

  void MaybeRelease(PAlloc a) {
    absl::base_internal::SpinLockHolder l(&pageheap_lock);
    tracker_.MaybeRelease(a.p, a.n);
  }
};

void PageTrackerTest::MockUnback(void *p, size_t len) { mock_.Unback(p, len); }

PageTrackerTest::MockUnbackInterface PageTrackerTest::mock_;

TEST_F(PageTrackerTest, AllocSane) {
  Length free = kPagesPerHugePage;
  Length n = 1;
  std::vector<PAlloc> allocs;
  // This should work without fragmentation.
  while (n <= free) {
    ASSERT_LE(n, tracker_.longest_free_range());
    EXPECT_EQ(kPagesPerHugePage - free, tracker_.used_pages());
    EXPECT_EQ(free, tracker_.free_pages());
    PAlloc a = Get(n);
    Mark(a, n);
    allocs.push_back(a);
    free -= n;
    n++;
  }

  // All should be distinct
  for (auto alloc : allocs) {
    Check(alloc, alloc.n);
  }
}

TEST_F(PageTrackerTest, ReleasingReturn) {
  static const size_t kAllocSize = kPagesPerHugePage / 4;
  PAlloc a1 = Get(kAllocSize - 3);
  PAlloc a2 = Get(kAllocSize);
  PAlloc a3 = Get(kAllocSize + 1);
  PAlloc a4 = Get(kAllocSize + 2);

  Put(a2);
  Put(a4);
  // We now have a hugepage that looks like [alloced] [free] [alloced] [free].
  // The free parts should be released when we mark the hugepage as such,
  // but not the allocated parts.
  ExpectPages(a2);
  ExpectPages(a4);
  ReleaseFree();
  mock_.VerifyAndClear();

  // Now we return the other parts, and they *should* get released.
  ExpectPages(a1);
  ExpectPages(a3);

  MaybeRelease(a1);
  Put(a1);

  MaybeRelease(a3);
  Put(a3);
}

TEST_F(PageTrackerTest, ReleasingRetain) {
  static const size_t kAllocSize = kPagesPerHugePage / 4;
  PAlloc a1 = Get(kAllocSize - 3);
  PAlloc a2 = Get(kAllocSize);
  PAlloc a3 = Get(kAllocSize + 1);
  PAlloc a4 = Get(kAllocSize + 2);

  Put(a2);
  Put(a4);
  // We now have a hugepage that looks like [alloced] [free] [alloced] [free].
  // The free parts should be released when we mark the hugepage as such,
  // but not the allocated parts.
  ExpectPages(a2);
  ExpectPages(a4);
  ReleaseFree();
  mock_.VerifyAndClear();

  // Now we return the other parts, and they shouldn't get released.
  Put(a1);
  Put(a3);

  mock_.VerifyAndClear();

  // But they will if we ReleaseFree.
  ExpectPages(a1);
  ExpectPages(a3);
  ReleaseFree();
  mock_.VerifyAndClear();
}

TEST_F(PageTrackerTest, Defrag) {
  absl::BitGen rng;
  const Length N = absl::GetFlag(FLAGS_page_tracker_defrag_lim);
  auto dist = EmpiricalDistribution(N);

  std::vector<PAlloc> allocs;

  std::vector<PAlloc> doomed;
  while (tracker_.longest_free_range() > 0) {
    Length n;
    do {
      n = dist(rng);
    } while (n > tracker_.longest_free_range());
    PAlloc a = Get(n);
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
    double frag = free > 0 ? static_cast<double>(longest) / free : 1;

    if (i % (kReps / 25) == 0) {
      printf("free = %zu longest = %zu frag = %f\n", free, longest, frag);
    }
    frag_samples.push_back(frag);
    longest_free_samples.push_back(longest);

    // Randomly grow or shrink (picking the only safe option when we're either
    // full or empty.)
    if (tracker_.longest_free_range() == 0 ||
        (absl::Bernoulli(rng, 1.0 / 2) && !allocs.empty())) {
      size_t index = absl::Uniform<int32_t>(rng, 0, allocs.size());
      std::swap(allocs[index], allocs.back());
      Put(allocs.back());
      allocs.pop_back();
    } else {
      Length n;
      do {
        n = dist(rng);
      } while (n > tracker_.longest_free_range());
      allocs.push_back(Get(n));
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
    // The situation is worse on ppc with larger huge pages:
    // pass rate for test is ~50% at 0.20. Reducing from 0.2 to 0.07.
    // TODO(b/127466107) figure out a better solution.
    EXPECT_GE(p25, 0.07);
  }

  {
    const Length p10 = longest_free_samples[kReps * 10 / 100];
    const Length p25 = longest_free_samples[kReps * 25 / 100];
    const Length p50 = longest_free_samples[kReps * 50 / 100];
    const Length p75 = longest_free_samples[kReps * 75 / 100];
    const Length p90 = longest_free_samples[kReps * 90 / 100];
    printf("Longest free quantiles:\n");
    printf("p10: %zu p25: %zu p50: %zu p75: %zu p90: %zu\n", p10, p25, p50, p75,
           p90);
    // Similarly, we'd really like for there usually (p25) to be a space
    // for a large allocation (N - note that we've cooked the books so that
    // the page tracker is going to be something like half empty (ish) and N
    // is small, so that should be doable.)
    // ...but, of course, it isn't.
    EXPECT_GE(p25, 4);
  }

  for (auto a : allocs) {
    Put(a);
  }
}

TEST_F(PageTrackerTest, Stats) {
  struct Helper {
    static void Stat(const TestPageTracker &tracker,
                     std::vector<Length> *small_backed,
                     std::vector<Length> *small_unbacked, LargeSpanStats *large,
                     double *avg_age_backed, double *avg_age_unbacked) {
      SmallSpanStats small;
      memset(&small, 0, sizeof(small));
      memset(large, 0, sizeof(*large));
      PageAgeHistograms ages(absl::base_internal::CycleClock::Now());
      tracker.AddSpanStats(&small, large, &ages);
      small_backed->clear();
      small_unbacked->clear();
      for (int i = 0; i < kMaxPages; ++i) {
        for (int j = 0; j < small.normal_length[i]; ++j) {
          small_backed->push_back(i);
        }

        for (int j = 0; j < small.returned_length[i]; ++j) {
          small_unbacked->push_back(i);
        }
      }

      *avg_age_backed = ages.GetTotalHistogram(false)->avg_age();
      *avg_age_unbacked = ages.GetTotalHistogram(true)->avg_age();
    }
  };

  LargeSpanStats large;
  std::vector<Length> small_backed, small_unbacked;
  double avg_age_backed, avg_age_unbacked;

  const PageID p = Get(kPagesPerHugePage).p;
  const PageID end = p + kPagesPerHugePage;
  PageID next = p;
  Put({next, kMaxPages + 1});
  next += kMaxPages + 1;

  absl::SleepFor(absl::Milliseconds(10));
  Helper::Stat(tracker_, &small_backed, &small_unbacked, &large,
               &avg_age_backed, &avg_age_unbacked);
  EXPECT_THAT(small_backed, testing::ElementsAre());
  EXPECT_THAT(small_unbacked, testing::ElementsAre());
  EXPECT_EQ(1, large.spans);
  EXPECT_EQ(kMaxPages + 1, large.normal_pages);
  EXPECT_EQ(0, large.returned_pages);
  EXPECT_LE(0.01, avg_age_backed);

  next++;
  Put({next, 1});
  next += 1;
  absl::SleepFor(absl::Milliseconds(20));
  Helper::Stat(tracker_, &small_backed, &small_unbacked, &large,
               &avg_age_backed, &avg_age_unbacked);
  EXPECT_THAT(small_backed, testing::ElementsAre(1));
  EXPECT_THAT(small_unbacked, testing::ElementsAre());
  EXPECT_EQ(1, large.spans);
  EXPECT_EQ(kMaxPages + 1, large.normal_pages);
  EXPECT_EQ(0, large.returned_pages);
  EXPECT_LE(((kMaxPages + 1) * 0.03 + 1 * 0.02) / (kMaxPages + 2),
            avg_age_backed);
  EXPECT_EQ(0, avg_age_unbacked);

  next++;
  Put({next, 2});
  next += 2;
  absl::SleepFor(absl::Milliseconds(30));
  Helper::Stat(tracker_, &small_backed, &small_unbacked, &large,
               &avg_age_backed, &avg_age_unbacked);
  EXPECT_THAT(small_backed, testing::ElementsAre(1, 2));
  EXPECT_THAT(small_unbacked, testing::ElementsAre());
  EXPECT_EQ(1, large.spans);
  EXPECT_EQ(kMaxPages + 1, large.normal_pages);
  EXPECT_EQ(0, large.returned_pages);
  EXPECT_LE(((kMaxPages + 1) * 0.06 + 1 * 0.05 + 2 * 0.03) / (kMaxPages + 4),
            avg_age_backed);
  EXPECT_EQ(0, avg_age_unbacked);

  next++;
  Put({next, 3});
  next += 3;
  ASSERT_LE(next, end);
  absl::SleepFor(absl::Milliseconds(40));
  Helper::Stat(tracker_, &small_backed, &small_unbacked, &large,
               &avg_age_backed, &avg_age_unbacked);
  EXPECT_THAT(small_backed, testing::ElementsAre(1, 2, 3));
  EXPECT_THAT(small_unbacked, testing::ElementsAre());
  EXPECT_EQ(1, large.spans);
  EXPECT_EQ(kMaxPages + 1, large.normal_pages);
  EXPECT_EQ(0, large.returned_pages);
  EXPECT_LE(((kMaxPages + 1) * 0.10 + 1 * 0.09 + 2 * 0.07 + 3 * 0.04) /
                (kMaxPages + 7),
            avg_age_backed);
  EXPECT_EQ(0, avg_age_unbacked);

  ExpectPages({p, kMaxPages + 1});
  ExpectPages({p + kMaxPages + 2, 1});
  ExpectPages({p + kMaxPages + 4, 2});
  ExpectPages({p + kMaxPages + 7, 3});
  EXPECT_EQ(kMaxPages + 7, ReleaseFree());
  absl::SleepFor(absl::Milliseconds(100));
  Helper::Stat(tracker_, &small_backed, &small_unbacked, &large,
               &avg_age_backed, &avg_age_unbacked);
  EXPECT_THAT(small_backed, testing::ElementsAre());
  EXPECT_THAT(small_unbacked, testing::ElementsAre(1, 2, 3));
  EXPECT_EQ(1, large.spans);
  EXPECT_EQ(0, large.normal_pages);
  EXPECT_EQ(kMaxPages + 1, large.returned_pages);
  EXPECT_EQ(0, avg_age_backed);
  EXPECT_LE(0.1, avg_age_unbacked);
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
  allocs.reserve(kPagesPerHugePage);
  for (int i = 0; i < kPagesPerHugePage; i++) {
    allocs.push_back(Get(1));
  }

  std::sort(allocs.begin(), allocs.end(),
            [](const PAlloc &a, const PAlloc &b) { return a.p < b.p; });

  Put(allocs.back());
  allocs.erase(allocs.begin() + allocs.size() - 1);

  ASSERT_EQ(tracker_.used_pages(), kPagesPerHugePage - 1);

  SmallSpanStats small;
  memset(&small, 0, sizeof(small));
  LargeSpanStats large;
  memset(&large, 0, sizeof(large));
  PageAgeHistograms ages(absl::base_internal::CycleClock::Now());

  tracker_.AddSpanStats(&small, &large, &ages);

  EXPECT_EQ(small.normal_length[1], 1);
  EXPECT_THAT(0, testing::AllOfArray(&small.normal_length[2],
                                     &small.normal_length[kMaxPages]));
}

class BlockingUnback {
 public:
  static void Unback(void *p, size_t len) {
    if (!mu_) {
      return;
    }

    if (counter) {
      counter->DecrementCount();
    }

    mu_->Lock();
    mu_->Unlock();
  }

  static void set_lock(absl::Mutex *mu) { mu_ = mu; }

  static absl::BlockingCounter *counter;

 private:
  static thread_local absl::Mutex *mu_;
};

thread_local absl::Mutex *BlockingUnback::mu_ = nullptr;
absl::BlockingCounter *BlockingUnback::counter = nullptr;

class FillerTest : public testing::TestWithParam<FillerPartialRerelease> {
 protected:
  // Allow tests to modify the clock used by the cache.
  static int64_t FakeClock() { return clock_; }
  static void Advance(absl::Duration d) { clock_ += ToInt64Nanoseconds(d); }
  static int64_t clock_;

  static void Unback(void *p, size_t len) {}

  // Our templating approach lets us directly override certain functions
  // and have mocks without virtualization.  It's a bit funky but works.
  typedef PageTracker<BlockingUnback::Unback> FakeTracker;

  // We have backing of one word per (normal-sized) page for our "hugepages".
  std::vector<size_t> backing_;
  // This is space efficient enough that we won't bother recycling pages.
  HugePage GetBacking() {
    intptr_t i = backing_.size();
    backing_.resize(i + kPagesPerHugePage);
    intptr_t addr = i << kPageShift;
    CHECK_CONDITION(addr % kHugePageSize == 0);
    return HugePageContaining(reinterpret_cast<void *>(addr));
  }

  size_t *GetFakePage(PageID p) { return &backing_[p]; }

  void MarkRange(PageID p, Length n, size_t mark) {
    for (Length i = 0; i < n; ++i) {
      *GetFakePage(p + i) = mark;
    }
  }

  void CheckRange(PageID p, Length n, size_t mark) {
    for (Length i = 0; i < n; ++i) {
      EXPECT_EQ(mark, *GetFakePage(p + i));
    }
  }

  HugePageFiller<FakeTracker> filler_;

  FillerTest() : filler_(GetParam(), FakeClock) {}

  ~FillerTest() override {
    EXPECT_EQ(NHugePages(0), filler_.size());
  }

  struct PAlloc {
    FakeTracker *pt;
    PageID p;
    Length n;
    size_t mark;
  };

  void Mark(const PAlloc &alloc) { MarkRange(alloc.p, alloc.n, alloc.mark); }

  void Check(const PAlloc &alloc) { CheckRange(alloc.p, alloc.n, alloc.mark); }

  size_t next_mark_{0};

  HugeLength hp_contained_{NHugePages(0)};
  Length total_allocated_{0};

  absl::InsecureBitGen gen_;

  void CheckStats() {
    EXPECT_EQ(hp_contained_, filler_.size());
    auto stats = filler_.stats();
    const uint64_t freelist_bytes = stats.free_bytes + stats.unmapped_bytes;
    const uint64_t used_bytes = stats.system_bytes - freelist_bytes;
    EXPECT_EQ(total_allocated_ * kPageSize, used_bytes);
    EXPECT_EQ((hp_contained_.in_pages() - total_allocated_) * kPageSize,
              freelist_bytes);
  }
  PAlloc AllocateRaw(Length n, bool donated = false) {
    PAlloc ret;
    ret.n = n;
    ret.mark = ++next_mark_;
    bool success = false;
    if (!donated) {  // Donated means always create a new hugepage
      absl::base_internal::SpinLockHolder l(&pageheap_lock);
      success = filler_.TryGet(n, &ret.pt, &ret.p);
    }
    if (!success) {
      ret.pt =
          new FakeTracker(GetBacking(), absl::base_internal::CycleClock::Now());
      {
        absl::base_internal::SpinLockHolder l(&pageheap_lock);
        ret.p = ret.pt->Get(n).page;
      }
      filler_.Contribute(ret.pt, donated);
      ++hp_contained_;
    }

    total_allocated_ += n;
    return ret;
  }

  PAlloc Allocate(Length n, bool donated = false) {
    CHECK_CONDITION(n <= kPagesPerHugePage);
    PAlloc ret = AllocateRaw(n, donated);
    ret.n = n;
    Mark(ret);
    CheckStats();
    return ret;
  }

  // Returns true iff the filler returned an empty hugepage.
  bool DeleteRaw(const PAlloc &p) {
    FakeTracker *pt;
    {
      absl::base_internal::SpinLockHolder l(&pageheap_lock);
      pt = filler_.Put(p.pt, p.p, p.n);
    }
    total_allocated_ -= p.n;
    if (pt != nullptr) {
      EXPECT_EQ(kPagesPerHugePage, pt->longest_free_range());
      EXPECT_TRUE(pt->empty());
      --hp_contained_;
      delete pt;
      return true;
    }

    return false;
  }

  // Returns true iff the filler returned an empty hugepage
  bool Delete(const PAlloc &p) {
    Check(p);
    bool r = DeleteRaw(p);
    CheckStats();
    return r;
  }

  Length ReleasePages(Length desired) {
    absl::base_internal::SpinLockHolder l(&pageheap_lock);
    return filler_.ReleasePages(desired);
  }

  // Generates an "interesting" pattern of allocations that highlights all the
  // various features of our stats.
  std::vector<PAlloc> GenerateInterestingAllocs();
};

int64_t FillerTest::clock_{1234};

TEST_P(FillerTest, Density) {
  absl::BitGen rng;
  // Start with a really annoying setup: some hugepages half
  // empty (randomly)
  std::vector<PAlloc> allocs;
  std::vector<PAlloc> doomed_allocs;
  static const HugeLength kNumHugePages = NHugePages(64);
  for (Length i = 0; i < kNumHugePages.in_pages(); ++i) {
    ASSERT_EQ(i, filler_.pages_allocated());
    if (absl::Bernoulli(rng, 1.0 / 2)) {
      allocs.push_back(Allocate(1));
    } else {
      doomed_allocs.push_back(Allocate(1));
    }
  }
  for (auto d : doomed_allocs) {
    Delete(d);
  }
  EXPECT_EQ(kNumHugePages, filler_.size());
  // We want a good chance of touching ~every allocation.
  size_t n = allocs.size();
  // Now, randomly add and delete to the allocations.
  // We should converge to full and empty pages.
  for (int j = 0; j < 6; j++) {
    absl::c_shuffle(allocs, rng);

    for (int i = 0; i < n; ++i) {
      Delete(allocs[i]);
      allocs[i] = Allocate(1);
      ASSERT_EQ(n, filler_.pages_allocated());
    }
  }

  EXPECT_GE(allocs.size() / kPagesPerHugePage + 1, filler_.size().raw_num());

  // clean up, check for failures
  for (auto a : allocs) {
    Delete(a);
    ASSERT_EQ(--n, filler_.pages_allocated());
  }
}

TEST_P(FillerTest, Release) {
  static const size_t kAlloc = kPagesPerHugePage / 2;
  PAlloc p1 = Allocate(kAlloc - 1);
  PAlloc p2 = Allocate(kAlloc + 1);

  PAlloc p3 = Allocate(kAlloc - 2);
  PAlloc p4 = Allocate(kAlloc + 2);
  // We have two hugepages, both full: nothing to release.
  ASSERT_EQ(0, ReleasePages(kMaxValidPages));
  Delete(p1);
  Delete(p3);
  // Now we should see the p1 hugepage - emptier - released.
  ASSERT_EQ(kAlloc - 1, ReleasePages(kAlloc - 1));
  EXPECT_EQ(kAlloc - 1, filler_.unmapped_pages());
  ASSERT_TRUE(p1.pt->released());
  ASSERT_FALSE(p3.pt->released());

  // We expect to reuse p1.pt.
  PAlloc p5 = Allocate(kAlloc - 1);
  ASSERT_TRUE(p1.pt == p5.pt || p3.pt == p5.pt);

  Delete(p2);
  Delete(p4);
  Delete(p5);
}

TEST_P(FillerTest, Fragmentation) {
  absl::BitGen rng;
  auto dist = EmpiricalDistribution(absl::GetFlag(FLAGS_frag_req_limit));

  std::vector<PAlloc> allocs;
  Length total = 0;
  while (total < absl::GetFlag(FLAGS_frag_size)) {
    Length n = dist(rng);
    total += n;
    allocs.push_back(AllocateRaw(n));
  }

  double max_slack = 0.0;
  const size_t kReps = absl::GetFlag(FLAGS_frag_iters);
  for (size_t i = 0; i < kReps; ++i) {
    auto stats = filler_.stats();
    double slack = static_cast<double>(stats.free_bytes) / stats.system_bytes;

    max_slack = std::max(slack, max_slack);
    if (i % (kReps / 40) == 0) {
      printf("%zu events: %zu allocs totalling %zu slack %f\n", i,
             allocs.size(), total, slack);
    }
    if (absl::Bernoulli(rng, 1.0 / 2)) {
      size_t index = absl::Uniform<int32_t>(rng, 0, allocs.size());
      std::swap(allocs[index], allocs.back());
      DeleteRaw(allocs.back());
      total -= allocs.back().n;
      allocs.pop_back();
    } else {
      Length n = dist(rng);
      allocs.push_back(AllocateRaw(n));
      total += n;
    }
  }

  EXPECT_LE(max_slack, 0.05);

  for (auto a : allocs) {
    DeleteRaw(a);
  }
}

TEST_P(FillerTest, PrintFreeRatio) {
  // This test is sensitive to the number of pages per hugepage, as we are
  // printing raw stats.
  if (kPagesPerHugePage != 256) {
    GTEST_SKIP();
  }

  // Allocate two huge pages, release one, verify that we do not get an invalid
  // (>1.) ratio of free : non-fulls.
  PAlloc a1 = Allocate(kPagesPerHugePage);

  constexpr Length kQ = kPagesPerHugePage / 4;

  PAlloc a2 = Allocate(kQ);
  PAlloc a3 = Allocate(kQ);
  PAlloc a4 = Allocate(kQ);
  PAlloc a5 = Allocate(kQ);

  Delete(a5);

  ReleasePages(kQ);

  Delete(a4);

  std::string buffer(1024 * 1024, '\0');
  {
    TCMalloc_Printer printer(&*buffer.begin(), buffer.size());
    filler_.Print(&printer, /*everything=*/true);
  }
  // Find the \0 that got added.
  buffer.resize(strlen(buffer.c_str()));

  if (GetParam() == FillerPartialRerelease::Retain) {
    EXPECT_THAT(
        buffer,
        testing::StartsWith(
            R"(HugePageFiller: densely pack small requests into hugepages
HugePageFiller: 2 total, 1 full, 0 partial, 1 released (1 partially), 0 quarantined
HugePageFiller: 64 pages free in 2 hugepages, 0.1250 free
HugePageFiller: among non-fulls, 0.2500 free
HugePageFiller: 128 used pages in subreleased hugepages (128 of them in partially released)
HugePageFiller: 1 hugepages partially released, 0.2500 released
HugePageFiller: 0.6667 of used pages hugepageable)"));
  } else {
    EXPECT_THAT(
        buffer,
        testing::StartsWith(
            R"(HugePageFiller: densely pack small requests into hugepages
HugePageFiller: 2 total, 1 full, 0 partial, 1 released (0 partially), 0 quarantined
HugePageFiller: 0 pages free in 2 hugepages, 0.0000 free
HugePageFiller: among non-fulls, 0.0000 free
HugePageFiller: 128 used pages in subreleased hugepages (0 of them in partially released)
HugePageFiller: 1 hugepages partially released, 0.5000 released
HugePageFiller: 0.6667 of used pages hugepageable)"));
  }

  // Cleanup remaining allocs.
  Delete(a1);
  Delete(a2);
  Delete(a3);
}

static double BytesToMiB(size_t bytes) { return bytes / (1024.0 * 1024.0); }

using testing::AnyOf;
using testing::Eq;

TEST_P(FillerTest, HugePageFrac) {
  // I don't actually care which we get, both are
  // reasonable choices, but don't report a NaN/complain
  // about divide by 0s/ give some bogus number for empty.
  EXPECT_THAT(filler_.hugepage_frac(), AnyOf(Eq(0), Eq(1)));
  static const Length kQ = kPagesPerHugePage / 4;
  // These are all on one page:
  auto a1 = Allocate(kQ);
  auto a2 = Allocate(kQ);
  auto a3 = Allocate(kQ - 1);
  auto a4 = Allocate(kQ + 1);

  // As are these:
  auto a5 = Allocate(kPagesPerHugePage - kQ);
  auto a6 = Allocate(kQ);

  EXPECT_EQ(1, filler_.hugepage_frac());
  // Free space doesn't affect it...
  Delete(a4);
  Delete(a6);

  EXPECT_EQ(1, filler_.hugepage_frac());

  // Releasing the hugepage does.
  ASSERT_EQ(kQ + 1, ReleasePages(kQ + 1));
  EXPECT_EQ((3.0 * kQ) / (6.0 * kQ - 1.0), filler_.hugepage_frac());

  // Check our arithmetic in a couple scenarios.

  // 2 kQs on the release and 3 on the hugepage
  Delete(a2);
  EXPECT_EQ((3.0 * kQ) / (5.0 * kQ - 1), filler_.hugepage_frac());
  // This releases the free page on the partially released hugepage.
  ASSERT_EQ(kQ, ReleasePages(kQ));
  EXPECT_EQ((3.0 * kQ) / (5.0 * kQ - 1), filler_.hugepage_frac());

  // just-over-1 kQ on the release and 3 on the hugepage
  Delete(a3);
  EXPECT_EQ((3 * kQ) / (4.0 * kQ), filler_.hugepage_frac());
  // This releases the free page on the partially released hugepage.
  ASSERT_EQ(kQ - 1, ReleasePages(kQ - 1));
  EXPECT_EQ((3 * kQ) / (4.0 * kQ), filler_.hugepage_frac());

  // All huge!
  Delete(a1);
  EXPECT_EQ(1, filler_.hugepage_frac());

  Delete(a5);
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
  const Length baseline = absl::GetFlag(FLAGS_bytes) / kPageSize;
  const Length peak = baseline * absl::GetFlag(FLAGS_growth_factor);
  const Length free_target = baseline * absl::GetFlag(FLAGS_release_until);

  std::vector<PAlloc> allocs;
  while (filler_.used_pages() < baseline) {
    allocs.push_back(AllocateRaw(1));
  }

  while (true) {
    while (filler_.used_pages() < peak) {
      allocs.push_back(AllocateRaw(1));
    }
    const double peak_frac = filler_.hugepage_frac();
    // VSS
    const size_t footprint = filler_.size().in_bytes();

    std::shuffle(allocs.begin(), allocs.end(), rng);

    size_t limit = allocs.size();
    while (filler_.used_pages() > baseline) {
      --limit;
      DeleteRaw(allocs[limit]);
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

TEST_P(FillerTest, ReleaseAccounting) {
  const Length N = kPagesPerHugePage;
  auto big = Allocate(N - 2);
  auto tiny1 = Allocate(1);
  auto tiny2 = Allocate(1);
  auto half1 = Allocate(N / 2);
  auto half2 = Allocate(N / 2);

  Delete(half1);
  Delete(big);

  ASSERT_EQ(NHugePages(2), filler_.size());

  // We should pick the [empty big][full tiny] hugepage here.
  EXPECT_EQ(N - 2, ReleasePages(N - 2));
  EXPECT_EQ(N - 2, filler_.unmapped_pages());
  // This shouldn't trigger a release
  Delete(tiny1);
  if (GetParam() == FillerPartialRerelease::Retain) {
    EXPECT_EQ(N - 2, filler_.unmapped_pages());
    // Until we call ReleasePages()
    EXPECT_EQ(1, ReleasePages(1));
  }
  EXPECT_EQ(N - 1, filler_.unmapped_pages());

  // As should this, but this will drop the whole hugepage
  Delete(tiny2);
  EXPECT_EQ(0, filler_.unmapped_pages());
  EXPECT_EQ(NHugePages(1), filler_.size());

  // This shouldn't trigger any release: we just claim credit for the
  // releases we did automatically on tiny2.
  if (GetParam() == FillerPartialRerelease::Retain) {
    EXPECT_EQ(1, ReleasePages(1));
  } else {
    EXPECT_EQ(2, ReleasePages(2));
  }
  EXPECT_EQ(0, filler_.unmapped_pages());
  EXPECT_EQ(NHugePages(1), filler_.size());

  // Check subrelease stats
  EXPECT_EQ(N / 2, filler_.used_pages());
  EXPECT_EQ(0, filler_.used_pages_in_any_subreleased());
  EXPECT_EQ(0, filler_.used_pages_in_partial_released());
  EXPECT_EQ(0, filler_.used_pages_in_released());

  // Now we pick the half/half hugepage
  EXPECT_EQ(N / 2, ReleasePages(kMaxValidPages));
  EXPECT_EQ(N / 2, filler_.unmapped_pages());

  // Check subrelease stats
  EXPECT_EQ(N / 2, filler_.used_pages());
  EXPECT_EQ(N / 2, filler_.used_pages_in_any_subreleased());
  EXPECT_EQ(0, filler_.used_pages_in_partial_released());
  EXPECT_EQ(N / 2, filler_.used_pages_in_released());

  // Check accounting for partially released hugepages with partial rerelease
  if (GetParam() == FillerPartialRerelease::Retain) {
    // Allocating and deallocating a small object causes the page to turn from
    // a released hugepage into a partially released hugepage.
    auto tiny3 = Allocate(1);
    auto tiny4 = Allocate(1);
    Delete(tiny4);
    EXPECT_EQ(N / 2 + 1, filler_.used_pages());
    EXPECT_EQ(N / 2 + 1, filler_.used_pages_in_any_subreleased());
    EXPECT_EQ(N / 2 + 1, filler_.used_pages_in_partial_released());
    EXPECT_EQ(0, filler_.used_pages_in_released());
    Delete(tiny3);
  }

  Delete(half2);
  EXPECT_EQ(NHugePages(0), filler_.size());
  EXPECT_EQ(0, filler_.unmapped_pages());
}

TEST_P(FillerTest, ReleaseWithReuse) {
  const Length N = kPagesPerHugePage;
  auto half = Allocate(N / 2);
  auto tiny1 = Allocate(N / 4);
  auto tiny2 = Allocate(N / 4);

  Delete(half);

  ASSERT_EQ(NHugePages(1), filler_.size());

  // We should be able to release the pages from half1.
  EXPECT_EQ(N / 2, ReleasePages(kMaxValidPages));
  EXPECT_EQ(N / 2, filler_.unmapped_pages());

  // Release tiny1, release more.
  Delete(tiny1);

  EXPECT_EQ(N / 4, ReleasePages(kMaxValidPages));
  EXPECT_EQ(3 * N / 4, filler_.unmapped_pages());

  // Repopulate, confirm we can't release anything and unmapped pages goes to 0.
  tiny1 = Allocate(N / 4);
  EXPECT_EQ(0, ReleasePages(kMaxValidPages));
  EXPECT_EQ(N / 2, filler_.unmapped_pages());

  // Continue repopulating.
  half = Allocate(N / 2);
  EXPECT_EQ(0, ReleasePages(kMaxValidPages));
  EXPECT_EQ(0, filler_.unmapped_pages());
  EXPECT_EQ(NHugePages(1), filler_.size());

  // Release everything and cleanup.
  Delete(half);
  Delete(tiny1);
  Delete(tiny2);
  EXPECT_EQ(NHugePages(0), filler_.size());
  EXPECT_EQ(0, filler_.unmapped_pages());
}

TEST_P(FillerTest, AvoidArbitraryQuarantineVMGrowth) {
  const Length N = kPagesPerHugePage;
  // Guarantee we have a ton of released pages go empty.
  for (int i = 0; i < 10 * 1000; ++i) {
    auto half1 = Allocate(N / 2);
    auto half2 = Allocate(N / 2);
    Delete(half1);
    ASSERT_EQ(N / 2, ReleasePages(N / 2));
    Delete(half2);
  }

  auto s = filler_.stats();
  EXPECT_GE(1024 * 1024 * 1024, s.system_bytes);
}

TEST_P(FillerTest, StronglyPreferNonDonated) {
  // We donate several huge pages of varying fullnesses. Then we make several
  // allocations that would be perfect fits for the donated hugepages, *after*
  // making one allocation that won't fit, to ensure that a huge page is
  // contributed normally. Finally, we verify that we can still get the
  // donated huge pages back. (I.e. they weren't used.)
  std::vector<PAlloc> donated;
  ASSERT_GE(kPagesPerHugePage, 10);
  for (int i = 1; i <= 3; ++i) {
    donated.push_back(Allocate(kPagesPerHugePage - i, /*donated=*/true));
  }

  std::vector<PAlloc> regular;
  for (int i = 4; i >= 1; --i) {
    regular.push_back(Allocate(i));
  }

  for (const PAlloc &alloc : donated) {
    // All the donated huge pages should be freeable.
    EXPECT_TRUE(Delete(alloc));
  }

  for (const PAlloc &alloc : regular) {
    Delete(alloc);
  }
}

TEST_P(FillerTest, ParallelUnlockingSubrelease) {
  if (GetParam() == FillerPartialRerelease::Retain) {
    // When rerelease happens without going to Unback(), this test
    // (intentionally) deadlocks, as we never receive the call.
    return;
  }

  // Verify that we can deallocate a partial huge page and successfully unlock
  // the pageheap_lock without introducing race conditions around the metadata
  // for PageTracker::released_.
  //
  // Currently, HPAA unbacks *all* subsequent deallocations to a huge page once
  // we have broken up *any* part of it.
  //
  // If multiple deallocations are in-flight, we need to leave sufficient
  // breadcrumbs to ourselves (PageTracker::releasing_ is a Length, not a bool)
  // so that one deallocation completing does not have us "forget" that another
  // deallocation is about to unback other parts of the hugepage.
  //
  // If PageTracker::releasing_ were a bool, the completion of "t1" and
  // subsequent reallocation of "a2" in this test would mark the entirety of the
  // page as full, so we would choose to *not* unback a2 (when deallocated) or
  // a3 (when deallocated by t3).
  constexpr Length N = kPagesPerHugePage;

  auto a1 = AllocateRaw(N / 2);
  auto a2 = AllocateRaw(1);
  auto a3 = AllocateRaw(1);

  // Trigger subrelease.  The filler now has a partial hugepage, so subsequent
  // calls to Delete() will cause us to unback the remainder of it.
  EXPECT_GT(ReleasePages(kMaxValidPages), 0);

  auto m1 = absl::make_unique<absl::Mutex>();
  auto m2 = absl::make_unique<absl::Mutex>();

  m1->Lock();
  m2->Lock();

  absl::BlockingCounter counter(2);
  BlockingUnback::counter = &counter;

  std::thread t1([&]() {
    BlockingUnback::set_lock(m1.get());

    DeleteRaw(a2);
  });

  std::thread t2([&]() {
    BlockingUnback::set_lock(m2.get());

    DeleteRaw(a3);
  });

  // Wait for t1 and t2 to block.
  counter.Wait();

  // At this point, t1 and t2 are blocked (as if they were on a long-running
  // syscall) on "unback" (m1 and m2, respectively).  pageheap_lock is not held.
  //
  // Allocating a4 will complete the hugepage, but we have on-going releaser
  // threads.
  auto a4 = AllocateRaw((N / 2) - 2);
  EXPECT_EQ(NHugePages(1), filler_.size());

  // Let one of the threads proceed.  The huge page consists of:
  // * a1 (N/2  ):  Allocated
  // * a2 (    1):  Unbacked
  // * a3 (    1):  Unbacking (blocked on m2)
  // * a4 (N/2-2):  Allocated
  m1->Unlock();
  t1.join();

  // Reallocate a2.  We should still consider the huge page partially backed for
  // purposes of subreleasing.
  a2 = AllocateRaw(1);
  EXPECT_EQ(NHugePages(1), filler_.size());
  DeleteRaw(a2);

  // Let the other thread proceed.  The huge page consists of:
  // * a1 (N/2  ):  Allocated
  // * a2 (    1):  Unbacked
  // * a3 (    1):  Unbacked
  // * a4 (N/2-2):  Allocated
  m2->Unlock();
  t2.join();

  EXPECT_EQ(filler_.used_pages(), N - 2);
  EXPECT_EQ(filler_.unmapped_pages(), 2);
  EXPECT_EQ(filler_.free_pages(), 0);

  // Clean up.
  DeleteRaw(a1);
  DeleteRaw(a4);

  BlockingUnback::counter = nullptr;
}

class FillerStatsTrackerTest : public testing::Test {
 private:
  static int64_t clock_;
  static int64_t FakeClock() { return clock_; }

 protected:
  static constexpr absl::Duration kWindow = absl::Minutes(10);

  tcmalloc::FillerStatsTracker<16> tracker_{FakeClock, kWindow,
                                            absl::Minutes(5)};

  void Advance(absl::Duration d) { clock_ += ToInt64Nanoseconds(d); }

  // Generates four data points for the tracker that represent "interesting"
  // points (i.e., min/max pages demand, min/max hugepages).
  void GenerateInterestingPoints(size_t num_pages, size_t num_hugepages,
                                 size_t num_free_pages);
};

int64_t FillerStatsTrackerTest::clock_{0};

void FillerStatsTrackerTest::GenerateInterestingPoints(size_t num_pages,
                                                       size_t num_hugepages,
                                                       size_t num_free_pages) {
  for (size_t i = 0; i <= 1; i++) {
    for (size_t j = 0; j <= 1; j++) {
      tracker_.Report({.num_pages = num_pages + ((i == 0) ? 4 : 8 * j),
                       .free_pages = num_free_pages + 10 * i + j,
                       .unmapped_pages = 10,
                       .used_pages_in_subreleased_huge_pages = num_pages,
                       .huge_pages = {num_hugepages + ((i == 1) ? 4 : 8 * j),
                                      num_hugepages, i, j}});
    }
  }
}

// Tests that the tracker aggregates all data correctly. The output is tested by
// comparing the text output of the tracker. While this is a bit verbose, it is
// much cleaner than extracting and comparing all data manually.
TEST_F(FillerStatsTrackerTest, Works) {
  // Ensure that the beginning (when free pages are 0) is outside the 5-min
  // window the instrumentation is recording.
  GenerateInterestingPoints(1, 1, 1);
  Advance(absl::Minutes(5));

  GenerateInterestingPoints(100, 5, 200);

  Advance(absl::Minutes(1));

  GenerateInterestingPoints(200, 10, 100);

  Advance(absl::Minutes(1));

  // Test text output (time series summary).
  {
    std::string buffer(1024 * 1024, '\0');
    {
      TCMalloc_Printer printer(&*buffer.begin(), buffer.size());
      PbtxtRegion region(&printer, kTop, /*indent=*/0);
      tracker_.Print(&printer);
    }
    buffer.resize(strlen(buffer.c_str()));

    EXPECT_EQ(buffer, R"(HugePageFiller: time series over 5 min interval

HugePageFiller: minimum free pages: 110 (100 backed)
HugePageFiller: at peak demand: 208 pages (and 111 free, 10 unmapped)
HugePageFiller: at peak demand: 26 hps (14 regular, 10 donated, 1 partial, 1 released)
HugePageFiller: at peak hps: 208 pages (and 111 free, 10 unmapped)
HugePageFiller: at peak hps: 26 hps (14 regular, 10 donated, 1 partial, 1 released)
)");
  }

  // Test pbtxt output (full time series).
  {
    std::string buffer(1024 * 1024, '\0');
    {
      TCMalloc_Printer printer(&*buffer.begin(), buffer.size());
      PbtxtRegion region(&printer, kTop, /*indent=*/0);
      tracker_.PrintInPbtxt(&region);
    }
    buffer.resize(strlen(buffer.c_str()));

    EXPECT_EQ(buffer, R"(
  filler_stats_timeseries {
    window_ms: 37500
    epochs: 16
    min_free_pages_interval_ms: 300000
    min_free_pages: 110
    min_free_backed_pages: 100
    measurements {
      epoch: 6
      timestamp_ms: 0
      min_free_pages: 11
      min_free_backed_pages: 1
      at_minimum_demand {
        num_pages: 1
        regular_huge_pages: 5
        donated_huge_pages: 1
        partial_released_huge_pages: 1
        released_huge_pages: 0
        used_pages_in_subreleased_huge_pages: 1
      }
      at_maximum_demand {
        num_pages: 9
        regular_huge_pages: 5
        donated_huge_pages: 1
        partial_released_huge_pages: 1
        released_huge_pages: 1
        used_pages_in_subreleased_huge_pages: 1
      }
      at_minimum_huge_pages {
        num_pages: 5
        regular_huge_pages: 1
        donated_huge_pages: 1
        partial_released_huge_pages: 0
        released_huge_pages: 0
        used_pages_in_subreleased_huge_pages: 1
      }
      at_maximum_huge_pages {
        num_pages: 5
        regular_huge_pages: 9
        donated_huge_pages: 1
        partial_released_huge_pages: 0
        released_huge_pages: 1
        used_pages_in_subreleased_huge_pages: 1
      }
    }
    measurements {
      epoch: 14
      timestamp_ms: 300000
      min_free_pages: 210
      min_free_backed_pages: 200
      at_minimum_demand {
        num_pages: 100
        regular_huge_pages: 9
        donated_huge_pages: 5
        partial_released_huge_pages: 1
        released_huge_pages: 0
        used_pages_in_subreleased_huge_pages: 100
      }
      at_maximum_demand {
        num_pages: 108
        regular_huge_pages: 9
        donated_huge_pages: 5
        partial_released_huge_pages: 1
        released_huge_pages: 1
        used_pages_in_subreleased_huge_pages: 100
      }
      at_minimum_huge_pages {
        num_pages: 104
        regular_huge_pages: 5
        donated_huge_pages: 5
        partial_released_huge_pages: 0
        released_huge_pages: 0
        used_pages_in_subreleased_huge_pages: 100
      }
      at_maximum_huge_pages {
        num_pages: 104
        regular_huge_pages: 13
        donated_huge_pages: 5
        partial_released_huge_pages: 0
        released_huge_pages: 1
        used_pages_in_subreleased_huge_pages: 100
      }
    }
    measurements {
      epoch: 15
      timestamp_ms: 337500
      min_free_pages: 110
      min_free_backed_pages: 100
      at_minimum_demand {
        num_pages: 200
        regular_huge_pages: 14
        donated_huge_pages: 10
        partial_released_huge_pages: 1
        released_huge_pages: 0
        used_pages_in_subreleased_huge_pages: 200
      }
      at_maximum_demand {
        num_pages: 208
        regular_huge_pages: 14
        donated_huge_pages: 10
        partial_released_huge_pages: 1
        released_huge_pages: 1
        used_pages_in_subreleased_huge_pages: 200
      }
      at_minimum_huge_pages {
        num_pages: 204
        regular_huge_pages: 10
        donated_huge_pages: 10
        partial_released_huge_pages: 0
        released_huge_pages: 0
        used_pages_in_subreleased_huge_pages: 200
      }
      at_maximum_huge_pages {
        num_pages: 204
        regular_huge_pages: 18
        donated_huge_pages: 10
        partial_released_huge_pages: 0
        released_huge_pages: 1
        used_pages_in_subreleased_huge_pages: 200
      }
    }
  }
)");
  }
}

TEST_F(FillerStatsTrackerTest, InvalidDurations) {
  // These should not crash.
  tracker_.min_free_pages(absl::InfiniteDuration());
  tracker_.min_free_pages(kWindow + absl::Seconds(1));
  tracker_.min_free_pages(-(kWindow + absl::Seconds(1)));
  tracker_.min_free_pages(-absl::InfiniteDuration());
}

std::vector<FillerTest::PAlloc> FillerTest::GenerateInterestingAllocs() {
  PAlloc a = Allocate(1);
  EXPECT_EQ(ReleasePages(kMaxValidPages), kPagesPerHugePage - 1);
  Delete(a);
  // Get the report on the released page
  EXPECT_EQ(ReleasePages(kMaxValidPages), 1);

  // Use a maximally-suboptimal pattern to get lots of hugepages into the
  // filler.
  std::vector<PAlloc> result;
  static_assert(kPagesPerHugePage > 7, "Not enough pages per hugepage!");
  for (int i = 0; i < 7; ++i) {
    result.push_back(Allocate(kPagesPerHugePage - i - 1));
  }

  // Get two released hugepages.
  EXPECT_EQ(ReleasePages(7), 7);
  EXPECT_EQ(ReleasePages(6), 6);

  // Fill some of the remaining pages with small allocations.
  for (int i = 0; i < 9; ++i) {
    result.push_back(Allocate(1));
  }

  // Finally, donate one hugepage.
  result.push_back(Allocate(1, /*donated=*/true));
  return result;
}

// Test the output of Print(). This is something of a change-detector test,
// but that's not all bad in this case.
TEST_P(FillerTest, Print) {
  if (kPagesPerHugePage != 256) {
    // The output is hardcoded on this assumption, and dynamically calculating
    // it would be way too much of a pain.
    return;
  }
  auto allocs = GenerateInterestingAllocs();

  std::string buffer(1024 * 1024, '\0');
  {
    TCMalloc_Printer printer(&*buffer.begin(), buffer.size());
    filler_.Print(&printer, /*everything=*/true);
  }
  // Find the \0 that got added.
  buffer.resize(strlen(buffer.c_str()));

  EXPECT_EQ(buffer,
            R"(HugePageFiller: densely pack small requests into hugepages
HugePageFiller: 8 total, 3 full, 3 partial, 2 released (0 partially), 0 quarantined
HugePageFiller: 261 pages free in 8 hugepages, 0.1274 free
HugePageFiller: among non-fulls, 0.3398 free
HugePageFiller: 499 used pages in subreleased hugepages (0 of them in partially released)
HugePageFiller: 2 hugepages partially released, 0.0254 released
HugePageFiller: 0.7187 of used pages hugepageable

HugePageFiller: fullness histograms

HugePageFiller: # of regular hps with a<= # of free pages <b
HugePageFiller: <  0<=     3 <  1<=     1 <  2<=     0 <  3<=     0 <  4<=     1 < 16<=     0
HugePageFiller: < 32<=     0 < 48<=     0 < 64<=     0 < 80<=     0 < 96<=     0 <112<=     0
HugePageFiller: <128<=     0 <144<=     0 <160<=     0 <176<=     0 <192<=     0 <208<=     0
HugePageFiller: <224<=     0 <240<=     0 <252<=     0 <253<=     0 <254<=     0 <255<=     0

HugePageFiller: # of donated hps with a<= # of free pages <b
HugePageFiller: <  0<=     0 <  1<=     0 <  2<=     0 <  3<=     0 <  4<=     0 < 16<=     0
HugePageFiller: < 32<=     0 < 48<=     0 < 64<=     0 < 80<=     0 < 96<=     0 <112<=     0
HugePageFiller: <128<=     0 <144<=     0 <160<=     0 <176<=     0 <192<=     0 <208<=     0
HugePageFiller: <224<=     0 <240<=     0 <252<=     0 <253<=     0 <254<=     0 <255<=     1

HugePageFiller: # of partial released hps with a<= # of free pages <b
HugePageFiller: <  0<=     0 <  1<=     0 <  2<=     0 <  3<=     0 <  4<=     0 < 16<=     0
HugePageFiller: < 32<=     0 < 48<=     0 < 64<=     0 < 80<=     0 < 96<=     0 <112<=     0
HugePageFiller: <128<=     0 <144<=     0 <160<=     0 <176<=     0 <192<=     0 <208<=     0
HugePageFiller: <224<=     0 <240<=     0 <252<=     0 <253<=     0 <254<=     0 <255<=     0

HugePageFiller: # of released hps with a<= # of free pages <b
HugePageFiller: <  0<=     0 <  1<=     0 <  2<=     0 <  3<=     0 <  4<=     2 < 16<=     0
HugePageFiller: < 32<=     0 < 48<=     0 < 64<=     0 < 80<=     0 < 96<=     0 <112<=     0
HugePageFiller: <128<=     0 <144<=     0 <160<=     0 <176<=     0 <192<=     0 <208<=     0
HugePageFiller: <224<=     0 <240<=     0 <252<=     0 <253<=     0 <254<=     0 <255<=     0

HugePageFiller: # of regular hps with a<= longest free range <b
HugePageFiller: <  0<=     3 <  1<=     1 <  2<=     0 <  3<=     0 <  4<=     1 < 16<=     0
HugePageFiller: < 32<=     0 < 48<=     0 < 64<=     0 < 80<=     0 < 96<=     0 <112<=     0
HugePageFiller: <128<=     0 <144<=     0 <160<=     0 <176<=     0 <192<=     0 <208<=     0
HugePageFiller: <224<=     0 <240<=     0 <252<=     0 <253<=     0 <254<=     0 <255<=     0

HugePageFiller: # of partial released hps with a<= longest free range <b
HugePageFiller: <  0<=     0 <  1<=     0 <  2<=     0 <  3<=     0 <  4<=     0 < 16<=     0
HugePageFiller: < 32<=     0 < 48<=     0 < 64<=     0 < 80<=     0 < 96<=     0 <112<=     0
HugePageFiller: <128<=     0 <144<=     0 <160<=     0 <176<=     0 <192<=     0 <208<=     0
HugePageFiller: <224<=     0 <240<=     0 <252<=     0 <253<=     0 <254<=     0 <255<=     0

HugePageFiller: # of released hps with a<= longest free range <b
HugePageFiller: <  0<=     0 <  1<=     0 <  2<=     0 <  3<=     0 <  4<=     2 < 16<=     0
HugePageFiller: < 32<=     0 < 48<=     0 < 64<=     0 < 80<=     0 < 96<=     0 <112<=     0
HugePageFiller: <128<=     0 <144<=     0 <160<=     0 <176<=     0 <192<=     0 <208<=     0
HugePageFiller: <224<=     0 <240<=     0 <252<=     0 <253<=     0 <254<=     0 <255<=     0

HugePageFiller: # of regular hps with a<= # of allocations <b
HugePageFiller: <  1<=     1 <  2<=     1 <  3<=     1 <  4<=     2 <  5<=     0 < 17<=     0
HugePageFiller: < 33<=     0 < 49<=     0 < 65<=     0 < 81<=     0 < 97<=     0 <113<=     0
HugePageFiller: <129<=     0 <145<=     0 <161<=     0 <177<=     0 <193<=     0 <209<=     0
HugePageFiller: <225<=     0 <241<=     0 <253<=     0 <254<=     0 <255<=     0 <256<=     0

HugePageFiller: # of partial released hps with a<= # of allocations <b
HugePageFiller: <  1<=     0 <  2<=     0 <  3<=     0 <  4<=     0 <  5<=     0 < 17<=     0
HugePageFiller: < 33<=     0 < 49<=     0 < 65<=     0 < 81<=     0 < 97<=     0 <113<=     0
HugePageFiller: <129<=     0 <145<=     0 <161<=     0 <177<=     0 <193<=     0 <209<=     0
HugePageFiller: <225<=     0 <241<=     0 <253<=     0 <254<=     0 <255<=     0 <256<=     0

HugePageFiller: # of released hps with a<= # of allocations <b
HugePageFiller: <  1<=     2 <  2<=     0 <  3<=     0 <  4<=     0 <  5<=     0 < 17<=     0
HugePageFiller: < 33<=     0 < 49<=     0 < 65<=     0 < 81<=     0 < 97<=     0 <113<=     0
HugePageFiller: <129<=     0 <145<=     0 <161<=     0 <177<=     0 <193<=     0 <209<=     0
HugePageFiller: <225<=     0 <241<=     0 <253<=     0 <254<=     0 <255<=     0 <256<=     0

HugePageFiller: time series over 5 min interval

HugePageFiller: minimum free pages: 0 (0 backed)
HugePageFiller: at peak demand: 1774 pages (and 261 free, 13 unmapped)
HugePageFiller: at peak demand: 8 hps (5 regular, 1 donated, 0 partial, 2 released)
HugePageFiller: at peak hps: 1774 pages (and 261 free, 13 unmapped)
HugePageFiller: at peak hps: 8 hps (5 regular, 1 donated, 0 partial, 2 released)
)");
  for (const auto &alloc : allocs) {
    Delete(alloc);
  }
}

// Test the output of PrintInPbtxt(). This is something of a change-detector
// test, but that's not all bad in this case.
TEST_P(FillerTest, PrintInPbtxt) {
  if (kPagesPerHugePage != 256) {
    // The output is hardcoded on this assumption, and dynamically calculating
    // it would be way too much of a pain.
    return;
  }
  auto allocs = GenerateInterestingAllocs();

  std::string buffer(1024 * 1024, '\0');
  {
    TCMalloc_Printer printer(&*buffer.begin(), buffer.size());
    PbtxtRegion region(&printer, kTop, /*indent=*/0);
    // Hardcoding the effective page size, to avoid depending on it in the
    // output.
    filler_.PrintInPbtxt(&region, filler_.used_pages() * 8192);
  }
  // Find the \0 that got added.
  buffer.resize(strlen(buffer.c_str()));

  EXPECT_EQ(buffer, R"(
  filler_full_huge_pages: 3
  filler_partial_huge_pages: 3
  filler_released_huge_pages: 2
  filler_partially_released_huge_pages: 0
  filler_free_pages: 261
  filler_used_pages_in_subreleased: 499
  filler_used_pages_in_partial_released: 0
  filler_unmapped_bytes: 0
  filler_hugepageable_used_bytes: 10444800
  filler_tracker {
    type: REGULAR
    free_pages_histogram {
      lower_bound: 0
      upper_bound: 0
      value: 3
    }
    free_pages_histogram {
      lower_bound: 1
      upper_bound: 1
      value: 1
    }
    free_pages_histogram {
      lower_bound: 2
      upper_bound: 2
      value: 0
    }
    free_pages_histogram {
      lower_bound: 3
      upper_bound: 3
      value: 0
    }
    free_pages_histogram {
      lower_bound: 4
      upper_bound: 15
      value: 1
    }
    free_pages_histogram {
      lower_bound: 16
      upper_bound: 31
      value: 0
    }
    free_pages_histogram {
      lower_bound: 32
      upper_bound: 47
      value: 0
    }
    free_pages_histogram {
      lower_bound: 48
      upper_bound: 63
      value: 0
    }
    free_pages_histogram {
      lower_bound: 64
      upper_bound: 79
      value: 0
    }
    free_pages_histogram {
      lower_bound: 80
      upper_bound: 95
      value: 0
    }
    free_pages_histogram {
      lower_bound: 96
      upper_bound: 111
      value: 0
    }
    free_pages_histogram {
      lower_bound: 112
      upper_bound: 127
      value: 0
    }
    free_pages_histogram {
      lower_bound: 128
      upper_bound: 143
      value: 0
    }
    free_pages_histogram {
      lower_bound: 144
      upper_bound: 159
      value: 0
    }
    free_pages_histogram {
      lower_bound: 160
      upper_bound: 175
      value: 0
    }
    free_pages_histogram {
      lower_bound: 176
      upper_bound: 191
      value: 0
    }
    free_pages_histogram {
      lower_bound: 192
      upper_bound: 207
      value: 0
    }
    free_pages_histogram {
      lower_bound: 208
      upper_bound: 223
      value: 0
    }
    free_pages_histogram {
      lower_bound: 224
      upper_bound: 239
      value: 0
    }
    free_pages_histogram {
      lower_bound: 240
      upper_bound: 251
      value: 0
    }
    free_pages_histogram {
      lower_bound: 252
      upper_bound: 252
      value: 0
    }
    free_pages_histogram {
      lower_bound: 253
      upper_bound: 253
      value: 0
    }
    free_pages_histogram {
      lower_bound: 254
      upper_bound: 254
      value: 0
    }
    free_pages_histogram {
      lower_bound: 255
      upper_bound: 255
      value: 0
    }
    longest_free_range_histogram {
      lower_bound: 0
      upper_bound: 0
      value: 3
    }
    longest_free_range_histogram {
      lower_bound: 1
      upper_bound: 1
      value: 1
    }
    longest_free_range_histogram {
      lower_bound: 2
      upper_bound: 2
      value: 0
    }
    longest_free_range_histogram {
      lower_bound: 3
      upper_bound: 3
      value: 0
    }
    longest_free_range_histogram {
      lower_bound: 4
      upper_bound: 15
      value: 1
    }
    longest_free_range_histogram {
      lower_bound: 16
      upper_bound: 31
      value: 0
    }
    longest_free_range_histogram {
      lower_bound: 32
      upper_bound: 47
      value: 0
    }
    longest_free_range_histogram {
      lower_bound: 48
      upper_bound: 63
      value: 0
    }
    longest_free_range_histogram {
      lower_bound: 64
      upper_bound: 79
      value: 0
    }
    longest_free_range_histogram {
      lower_bound: 80
      upper_bound: 95
      value: 0
    }
    longest_free_range_histogram {
      lower_bound: 96
      upper_bound: 111
      value: 0
    }
    longest_free_range_histogram {
      lower_bound: 112
      upper_bound: 127
      value: 0
    }
    longest_free_range_histogram {
      lower_bound: 128
      upper_bound: 143
      value: 0
    }
    longest_free_range_histogram {
      lower_bound: 144
      upper_bound: 159
      value: 0
    }
    longest_free_range_histogram {
      lower_bound: 160
      upper_bound: 175
      value: 0
    }
    longest_free_range_histogram {
      lower_bound: 176
      upper_bound: 191
      value: 0
    }
    longest_free_range_histogram {
      lower_bound: 192
      upper_bound: 207
      value: 0
    }
    longest_free_range_histogram {
      lower_bound: 208
      upper_bound: 223
      value: 0
    }
    longest_free_range_histogram {
      lower_bound: 224
      upper_bound: 239
      value: 0
    }
    longest_free_range_histogram {
      lower_bound: 240
      upper_bound: 251
      value: 0
    }
    longest_free_range_histogram {
      lower_bound: 252
      upper_bound: 252
      value: 0
    }
    longest_free_range_histogram {
      lower_bound: 253
      upper_bound: 253
      value: 0
    }
    longest_free_range_histogram {
      lower_bound: 254
      upper_bound: 254
      value: 0
    }
    longest_free_range_histogram {
      lower_bound: 255
      upper_bound: 255
      value: 0
    }
    allocations_histogram {
      lower_bound: 1
      upper_bound: 1
      value: 1
    }
    allocations_histogram {
      lower_bound: 2
      upper_bound: 2
      value: 1
    }
    allocations_histogram {
      lower_bound: 3
      upper_bound: 3
      value: 1
    }
    allocations_histogram {
      lower_bound: 4
      upper_bound: 4
      value: 2
    }
    allocations_histogram {
      lower_bound: 5
      upper_bound: 16
      value: 0
    }
    allocations_histogram {
      lower_bound: 17
      upper_bound: 32
      value: 0
    }
    allocations_histogram {
      lower_bound: 33
      upper_bound: 48
      value: 0
    }
    allocations_histogram {
      lower_bound: 49
      upper_bound: 64
      value: 0
    }
    allocations_histogram {
      lower_bound: 65
      upper_bound: 80
      value: 0
    }
    allocations_histogram {
      lower_bound: 81
      upper_bound: 96
      value: 0
    }
    allocations_histogram {
      lower_bound: 97
      upper_bound: 112
      value: 0
    }
    allocations_histogram {
      lower_bound: 113
      upper_bound: 128
      value: 0
    }
    allocations_histogram {
      lower_bound: 129
      upper_bound: 144
      value: 0
    }
    allocations_histogram {
      lower_bound: 145
      upper_bound: 160
      value: 0
    }
    allocations_histogram {
      lower_bound: 161
      upper_bound: 176
      value: 0
    }
    allocations_histogram {
      lower_bound: 177
      upper_bound: 192
      value: 0
    }
    allocations_histogram {
      lower_bound: 193
      upper_bound: 208
      value: 0
    }
    allocations_histogram {
      lower_bound: 209
      upper_bound: 224
      value: 0
    }
    allocations_histogram {
      lower_bound: 225
      upper_bound: 240
      value: 0
    }
    allocations_histogram {
      lower_bound: 241
      upper_bound: 252
      value: 0
    }
    allocations_histogram {
      lower_bound: 253
      upper_bound: 253
      value: 0
    }
    allocations_histogram {
      lower_bound: 254
      upper_bound: 254
      value: 0
    }
    allocations_histogram {
      lower_bound: 255
      upper_bound: 255
      value: 0
    }
    allocations_histogram {
      lower_bound: 256
      upper_bound: 256
      value: 0
    }
  }
  filler_tracker {
    type: DONATED
    free_pages_histogram {
      lower_bound: 0
      upper_bound: 0
      value: 0
    }
    free_pages_histogram {
      lower_bound: 1
      upper_bound: 1
      value: 0
    }
    free_pages_histogram {
      lower_bound: 2
      upper_bound: 2
      value: 0
    }
    free_pages_histogram {
      lower_bound: 3
      upper_bound: 3
      value: 0
    }
    free_pages_histogram {
      lower_bound: 4
      upper_bound: 15
      value: 0
    }
    free_pages_histogram {
      lower_bound: 16
      upper_bound: 31
      value: 0
    }
    free_pages_histogram {
      lower_bound: 32
      upper_bound: 47
      value: 0
    }
    free_pages_histogram {
      lower_bound: 48
      upper_bound: 63
      value: 0
    }
    free_pages_histogram {
      lower_bound: 64
      upper_bound: 79
      value: 0
    }
    free_pages_histogram {
      lower_bound: 80
      upper_bound: 95
      value: 0
    }
    free_pages_histogram {
      lower_bound: 96
      upper_bound: 111
      value: 0
    }
    free_pages_histogram {
      lower_bound: 112
      upper_bound: 127
      value: 0
    }
    free_pages_histogram {
      lower_bound: 128
      upper_bound: 143
      value: 0
    }
    free_pages_histogram {
      lower_bound: 144
      upper_bound: 159
      value: 0
    }
    free_pages_histogram {
      lower_bound: 160
      upper_bound: 175
      value: 0
    }
    free_pages_histogram {
      lower_bound: 176
      upper_bound: 191
      value: 0
    }
    free_pages_histogram {
      lower_bound: 192
      upper_bound: 207
      value: 0
    }
    free_pages_histogram {
      lower_bound: 208
      upper_bound: 223
      value: 0
    }
    free_pages_histogram {
      lower_bound: 224
      upper_bound: 239
      value: 0
    }
    free_pages_histogram {
      lower_bound: 240
      upper_bound: 251
      value: 0
    }
    free_pages_histogram {
      lower_bound: 252
      upper_bound: 252
      value: 0
    }
    free_pages_histogram {
      lower_bound: 253
      upper_bound: 253
      value: 0
    }
    free_pages_histogram {
      lower_bound: 254
      upper_bound: 254
      value: 0
    }
    free_pages_histogram {
      lower_bound: 255
      upper_bound: 255
      value: 1
    }
    longest_free_range_histogram {
      lower_bound: 0
      upper_bound: 0
      value: 0
    }
    longest_free_range_histogram {
      lower_bound: 1
      upper_bound: 1
      value: 0
    }
    longest_free_range_histogram {
      lower_bound: 2
      upper_bound: 2
      value: 0
    }
    longest_free_range_histogram {
      lower_bound: 3
      upper_bound: 3
      value: 0
    }
    longest_free_range_histogram {
      lower_bound: 4
      upper_bound: 15
      value: 0
    }
    longest_free_range_histogram {
      lower_bound: 16
      upper_bound: 31
      value: 0
    }
    longest_free_range_histogram {
      lower_bound: 32
      upper_bound: 47
      value: 0
    }
    longest_free_range_histogram {
      lower_bound: 48
      upper_bound: 63
      value: 0
    }
    longest_free_range_histogram {
      lower_bound: 64
      upper_bound: 79
      value: 0
    }
    longest_free_range_histogram {
      lower_bound: 80
      upper_bound: 95
      value: 0
    }
    longest_free_range_histogram {
      lower_bound: 96
      upper_bound: 111
      value: 0
    }
    longest_free_range_histogram {
      lower_bound: 112
      upper_bound: 127
      value: 0
    }
    longest_free_range_histogram {
      lower_bound: 128
      upper_bound: 143
      value: 0
    }
    longest_free_range_histogram {
      lower_bound: 144
      upper_bound: 159
      value: 0
    }
    longest_free_range_histogram {
      lower_bound: 160
      upper_bound: 175
      value: 0
    }
    longest_free_range_histogram {
      lower_bound: 176
      upper_bound: 191
      value: 0
    }
    longest_free_range_histogram {
      lower_bound: 192
      upper_bound: 207
      value: 0
    }
    longest_free_range_histogram {
      lower_bound: 208
      upper_bound: 223
      value: 0
    }
    longest_free_range_histogram {
      lower_bound: 224
      upper_bound: 239
      value: 0
    }
    longest_free_range_histogram {
      lower_bound: 240
      upper_bound: 251
      value: 0
    }
    longest_free_range_histogram {
      lower_bound: 252
      upper_bound: 252
      value: 0
    }
    longest_free_range_histogram {
      lower_bound: 253
      upper_bound: 253
      value: 0
    }
    longest_free_range_histogram {
      lower_bound: 254
      upper_bound: 254
      value: 0
    }
    longest_free_range_histogram {
      lower_bound: 255
      upper_bound: 255
      value: 1
    }
    allocations_histogram {
      lower_bound: 1
      upper_bound: 1
      value: 1
    }
    allocations_histogram {
      lower_bound: 2
      upper_bound: 2
      value: 0
    }
    allocations_histogram {
      lower_bound: 3
      upper_bound: 3
      value: 0
    }
    allocations_histogram {
      lower_bound: 4
      upper_bound: 4
      value: 0
    }
    allocations_histogram {
      lower_bound: 5
      upper_bound: 16
      value: 0
    }
    allocations_histogram {
      lower_bound: 17
      upper_bound: 32
      value: 0
    }
    allocations_histogram {
      lower_bound: 33
      upper_bound: 48
      value: 0
    }
    allocations_histogram {
      lower_bound: 49
      upper_bound: 64
      value: 0
    }
    allocations_histogram {
      lower_bound: 65
      upper_bound: 80
      value: 0
    }
    allocations_histogram {
      lower_bound: 81
      upper_bound: 96
      value: 0
    }
    allocations_histogram {
      lower_bound: 97
      upper_bound: 112
      value: 0
    }
    allocations_histogram {
      lower_bound: 113
      upper_bound: 128
      value: 0
    }
    allocations_histogram {
      lower_bound: 129
      upper_bound: 144
      value: 0
    }
    allocations_histogram {
      lower_bound: 145
      upper_bound: 160
      value: 0
    }
    allocations_histogram {
      lower_bound: 161
      upper_bound: 176
      value: 0
    }
    allocations_histogram {
      lower_bound: 177
      upper_bound: 192
      value: 0
    }
    allocations_histogram {
      lower_bound: 193
      upper_bound: 208
      value: 0
    }
    allocations_histogram {
      lower_bound: 209
      upper_bound: 224
      value: 0
    }
    allocations_histogram {
      lower_bound: 225
      upper_bound: 240
      value: 0
    }
    allocations_histogram {
      lower_bound: 241
      upper_bound: 252
      value: 0
    }
    allocations_histogram {
      lower_bound: 253
      upper_bound: 253
      value: 0
    }
    allocations_histogram {
      lower_bound: 254
      upper_bound: 254
      value: 0
    }
    allocations_histogram {
      lower_bound: 255
      upper_bound: 255
      value: 0
    }
    allocations_histogram {
      lower_bound: 256
      upper_bound: 256
      value: 0
    }
  }
  filler_tracker {
    type: PARTIAL
    free_pages_histogram {
      lower_bound: 0
      upper_bound: 0
      value: 0
    }
    free_pages_histogram {
      lower_bound: 1
      upper_bound: 1
      value: 0
    }
    free_pages_histogram {
      lower_bound: 2
      upper_bound: 2
      value: 0
    }
    free_pages_histogram {
      lower_bound: 3
      upper_bound: 3
      value: 0
    }
    free_pages_histogram {
      lower_bound: 4
      upper_bound: 15
      value: 0
    }
    free_pages_histogram {
      lower_bound: 16
      upper_bound: 31
      value: 0
    }
    free_pages_histogram {
      lower_bound: 32
      upper_bound: 47
      value: 0
    }
    free_pages_histogram {
      lower_bound: 48
      upper_bound: 63
      value: 0
    }
    free_pages_histogram {
      lower_bound: 64
      upper_bound: 79
      value: 0
    }
    free_pages_histogram {
      lower_bound: 80
      upper_bound: 95
      value: 0
    }
    free_pages_histogram {
      lower_bound: 96
      upper_bound: 111
      value: 0
    }
    free_pages_histogram {
      lower_bound: 112
      upper_bound: 127
      value: 0
    }
    free_pages_histogram {
      lower_bound: 128
      upper_bound: 143
      value: 0
    }
    free_pages_histogram {
      lower_bound: 144
      upper_bound: 159
      value: 0
    }
    free_pages_histogram {
      lower_bound: 160
      upper_bound: 175
      value: 0
    }
    free_pages_histogram {
      lower_bound: 176
      upper_bound: 191
      value: 0
    }
    free_pages_histogram {
      lower_bound: 192
      upper_bound: 207
      value: 0
    }
    free_pages_histogram {
      lower_bound: 208
      upper_bound: 223
      value: 0
    }
    free_pages_histogram {
      lower_bound: 224
      upper_bound: 239
      value: 0
    }
    free_pages_histogram {
      lower_bound: 240
      upper_bound: 251
      value: 0
    }
    free_pages_histogram {
      lower_bound: 252
      upper_bound: 252
      value: 0
    }
    free_pages_histogram {
      lower_bound: 253
      upper_bound: 253
      value: 0
    }
    free_pages_histogram {
      lower_bound: 254
      upper_bound: 254
      value: 0
    }
    free_pages_histogram {
      lower_bound: 255
      upper_bound: 255
      value: 0
    }
    longest_free_range_histogram {
      lower_bound: 0
      upper_bound: 0
      value: 0
    }
    longest_free_range_histogram {
      lower_bound: 1
      upper_bound: 1
      value: 0
    }
    longest_free_range_histogram {
      lower_bound: 2
      upper_bound: 2
      value: 0
    }
    longest_free_range_histogram {
      lower_bound: 3
      upper_bound: 3
      value: 0
    }
    longest_free_range_histogram {
      lower_bound: 4
      upper_bound: 15
      value: 0
    }
    longest_free_range_histogram {
      lower_bound: 16
      upper_bound: 31
      value: 0
    }
    longest_free_range_histogram {
      lower_bound: 32
      upper_bound: 47
      value: 0
    }
    longest_free_range_histogram {
      lower_bound: 48
      upper_bound: 63
      value: 0
    }
    longest_free_range_histogram {
      lower_bound: 64
      upper_bound: 79
      value: 0
    }
    longest_free_range_histogram {
      lower_bound: 80
      upper_bound: 95
      value: 0
    }
    longest_free_range_histogram {
      lower_bound: 96
      upper_bound: 111
      value: 0
    }
    longest_free_range_histogram {
      lower_bound: 112
      upper_bound: 127
      value: 0
    }
    longest_free_range_histogram {
      lower_bound: 128
      upper_bound: 143
      value: 0
    }
    longest_free_range_histogram {
      lower_bound: 144
      upper_bound: 159
      value: 0
    }
    longest_free_range_histogram {
      lower_bound: 160
      upper_bound: 175
      value: 0
    }
    longest_free_range_histogram {
      lower_bound: 176
      upper_bound: 191
      value: 0
    }
    longest_free_range_histogram {
      lower_bound: 192
      upper_bound: 207
      value: 0
    }
    longest_free_range_histogram {
      lower_bound: 208
      upper_bound: 223
      value: 0
    }
    longest_free_range_histogram {
      lower_bound: 224
      upper_bound: 239
      value: 0
    }
    longest_free_range_histogram {
      lower_bound: 240
      upper_bound: 251
      value: 0
    }
    longest_free_range_histogram {
      lower_bound: 252
      upper_bound: 252
      value: 0
    }
    longest_free_range_histogram {
      lower_bound: 253
      upper_bound: 253
      value: 0
    }
    longest_free_range_histogram {
      lower_bound: 254
      upper_bound: 254
      value: 0
    }
    longest_free_range_histogram {
      lower_bound: 255
      upper_bound: 255
      value: 0
    }
    allocations_histogram {
      lower_bound: 1
      upper_bound: 1
      value: 0
    }
    allocations_histogram {
      lower_bound: 2
      upper_bound: 2
      value: 0
    }
    allocations_histogram {
      lower_bound: 3
      upper_bound: 3
      value: 0
    }
    allocations_histogram {
      lower_bound: 4
      upper_bound: 4
      value: 0
    }
    allocations_histogram {
      lower_bound: 5
      upper_bound: 16
      value: 0
    }
    allocations_histogram {
      lower_bound: 17
      upper_bound: 32
      value: 0
    }
    allocations_histogram {
      lower_bound: 33
      upper_bound: 48
      value: 0
    }
    allocations_histogram {
      lower_bound: 49
      upper_bound: 64
      value: 0
    }
    allocations_histogram {
      lower_bound: 65
      upper_bound: 80
      value: 0
    }
    allocations_histogram {
      lower_bound: 81
      upper_bound: 96
      value: 0
    }
    allocations_histogram {
      lower_bound: 97
      upper_bound: 112
      value: 0
    }
    allocations_histogram {
      lower_bound: 113
      upper_bound: 128
      value: 0
    }
    allocations_histogram {
      lower_bound: 129
      upper_bound: 144
      value: 0
    }
    allocations_histogram {
      lower_bound: 145
      upper_bound: 160
      value: 0
    }
    allocations_histogram {
      lower_bound: 161
      upper_bound: 176
      value: 0
    }
    allocations_histogram {
      lower_bound: 177
      upper_bound: 192
      value: 0
    }
    allocations_histogram {
      lower_bound: 193
      upper_bound: 208
      value: 0
    }
    allocations_histogram {
      lower_bound: 209
      upper_bound: 224
      value: 0
    }
    allocations_histogram {
      lower_bound: 225
      upper_bound: 240
      value: 0
    }
    allocations_histogram {
      lower_bound: 241
      upper_bound: 252
      value: 0
    }
    allocations_histogram {
      lower_bound: 253
      upper_bound: 253
      value: 0
    }
    allocations_histogram {
      lower_bound: 254
      upper_bound: 254
      value: 0
    }
    allocations_histogram {
      lower_bound: 255
      upper_bound: 255
      value: 0
    }
    allocations_histogram {
      lower_bound: 256
      upper_bound: 256
      value: 0
    }
  }
  filler_tracker {
    type: RELEASED
    free_pages_histogram {
      lower_bound: 0
      upper_bound: 0
      value: 0
    }
    free_pages_histogram {
      lower_bound: 1
      upper_bound: 1
      value: 0
    }
    free_pages_histogram {
      lower_bound: 2
      upper_bound: 2
      value: 0
    }
    free_pages_histogram {
      lower_bound: 3
      upper_bound: 3
      value: 0
    }
    free_pages_histogram {
      lower_bound: 4
      upper_bound: 15
      value: 2
    }
    free_pages_histogram {
      lower_bound: 16
      upper_bound: 31
      value: 0
    }
    free_pages_histogram {
      lower_bound: 32
      upper_bound: 47
      value: 0
    }
    free_pages_histogram {
      lower_bound: 48
      upper_bound: 63
      value: 0
    }
    free_pages_histogram {
      lower_bound: 64
      upper_bound: 79
      value: 0
    }
    free_pages_histogram {
      lower_bound: 80
      upper_bound: 95
      value: 0
    }
    free_pages_histogram {
      lower_bound: 96
      upper_bound: 111
      value: 0
    }
    free_pages_histogram {
      lower_bound: 112
      upper_bound: 127
      value: 0
    }
    free_pages_histogram {
      lower_bound: 128
      upper_bound: 143
      value: 0
    }
    free_pages_histogram {
      lower_bound: 144
      upper_bound: 159
      value: 0
    }
    free_pages_histogram {
      lower_bound: 160
      upper_bound: 175
      value: 0
    }
    free_pages_histogram {
      lower_bound: 176
      upper_bound: 191
      value: 0
    }
    free_pages_histogram {
      lower_bound: 192
      upper_bound: 207
      value: 0
    }
    free_pages_histogram {
      lower_bound: 208
      upper_bound: 223
      value: 0
    }
    free_pages_histogram {
      lower_bound: 224
      upper_bound: 239
      value: 0
    }
    free_pages_histogram {
      lower_bound: 240
      upper_bound: 251
      value: 0
    }
    free_pages_histogram {
      lower_bound: 252
      upper_bound: 252
      value: 0
    }
    free_pages_histogram {
      lower_bound: 253
      upper_bound: 253
      value: 0
    }
    free_pages_histogram {
      lower_bound: 254
      upper_bound: 254
      value: 0
    }
    free_pages_histogram {
      lower_bound: 255
      upper_bound: 255
      value: 0
    }
    longest_free_range_histogram {
      lower_bound: 0
      upper_bound: 0
      value: 0
    }
    longest_free_range_histogram {
      lower_bound: 1
      upper_bound: 1
      value: 0
    }
    longest_free_range_histogram {
      lower_bound: 2
      upper_bound: 2
      value: 0
    }
    longest_free_range_histogram {
      lower_bound: 3
      upper_bound: 3
      value: 0
    }
    longest_free_range_histogram {
      lower_bound: 4
      upper_bound: 15
      value: 2
    }
    longest_free_range_histogram {
      lower_bound: 16
      upper_bound: 31
      value: 0
    }
    longest_free_range_histogram {
      lower_bound: 32
      upper_bound: 47
      value: 0
    }
    longest_free_range_histogram {
      lower_bound: 48
      upper_bound: 63
      value: 0
    }
    longest_free_range_histogram {
      lower_bound: 64
      upper_bound: 79
      value: 0
    }
    longest_free_range_histogram {
      lower_bound: 80
      upper_bound: 95
      value: 0
    }
    longest_free_range_histogram {
      lower_bound: 96
      upper_bound: 111
      value: 0
    }
    longest_free_range_histogram {
      lower_bound: 112
      upper_bound: 127
      value: 0
    }
    longest_free_range_histogram {
      lower_bound: 128
      upper_bound: 143
      value: 0
    }
    longest_free_range_histogram {
      lower_bound: 144
      upper_bound: 159
      value: 0
    }
    longest_free_range_histogram {
      lower_bound: 160
      upper_bound: 175
      value: 0
    }
    longest_free_range_histogram {
      lower_bound: 176
      upper_bound: 191
      value: 0
    }
    longest_free_range_histogram {
      lower_bound: 192
      upper_bound: 207
      value: 0
    }
    longest_free_range_histogram {
      lower_bound: 208
      upper_bound: 223
      value: 0
    }
    longest_free_range_histogram {
      lower_bound: 224
      upper_bound: 239
      value: 0
    }
    longest_free_range_histogram {
      lower_bound: 240
      upper_bound: 251
      value: 0
    }
    longest_free_range_histogram {
      lower_bound: 252
      upper_bound: 252
      value: 0
    }
    longest_free_range_histogram {
      lower_bound: 253
      upper_bound: 253
      value: 0
    }
    longest_free_range_histogram {
      lower_bound: 254
      upper_bound: 254
      value: 0
    }
    longest_free_range_histogram {
      lower_bound: 255
      upper_bound: 255
      value: 0
    }
    allocations_histogram {
      lower_bound: 1
      upper_bound: 1
      value: 2
    }
    allocations_histogram {
      lower_bound: 2
      upper_bound: 2
      value: 0
    }
    allocations_histogram {
      lower_bound: 3
      upper_bound: 3
      value: 0
    }
    allocations_histogram {
      lower_bound: 4
      upper_bound: 4
      value: 0
    }
    allocations_histogram {
      lower_bound: 5
      upper_bound: 16
      value: 0
    }
    allocations_histogram {
      lower_bound: 17
      upper_bound: 32
      value: 0
    }
    allocations_histogram {
      lower_bound: 33
      upper_bound: 48
      value: 0
    }
    allocations_histogram {
      lower_bound: 49
      upper_bound: 64
      value: 0
    }
    allocations_histogram {
      lower_bound: 65
      upper_bound: 80
      value: 0
    }
    allocations_histogram {
      lower_bound: 81
      upper_bound: 96
      value: 0
    }
    allocations_histogram {
      lower_bound: 97
      upper_bound: 112
      value: 0
    }
    allocations_histogram {
      lower_bound: 113
      upper_bound: 128
      value: 0
    }
    allocations_histogram {
      lower_bound: 129
      upper_bound: 144
      value: 0
    }
    allocations_histogram {
      lower_bound: 145
      upper_bound: 160
      value: 0
    }
    allocations_histogram {
      lower_bound: 161
      upper_bound: 176
      value: 0
    }
    allocations_histogram {
      lower_bound: 177
      upper_bound: 192
      value: 0
    }
    allocations_histogram {
      lower_bound: 193
      upper_bound: 208
      value: 0
    }
    allocations_histogram {
      lower_bound: 209
      upper_bound: 224
      value: 0
    }
    allocations_histogram {
      lower_bound: 225
      upper_bound: 240
      value: 0
    }
    allocations_histogram {
      lower_bound: 241
      upper_bound: 252
      value: 0
    }
    allocations_histogram {
      lower_bound: 253
      upper_bound: 253
      value: 0
    }
    allocations_histogram {
      lower_bound: 254
      upper_bound: 254
      value: 0
    }
    allocations_histogram {
      lower_bound: 255
      upper_bound: 255
      value: 0
    }
    allocations_histogram {
      lower_bound: 256
      upper_bound: 256
      value: 0
    }
  }
  filler_stats_timeseries {
    window_ms: 1000
    epochs: 600
    min_free_pages_interval_ms: 300000
    min_free_pages: 0
    min_free_backed_pages: 0
    measurements {
      epoch: 599
      timestamp_ms: 0
      min_free_pages: 0
      min_free_backed_pages: 0
      at_minimum_demand {
        num_pages: 0
        regular_huge_pages: 0
        donated_huge_pages: 0
        partial_released_huge_pages: 0
        released_huge_pages: 0
        used_pages_in_subreleased_huge_pages: 0
      }
      at_maximum_demand {
        num_pages: 1774
        regular_huge_pages: 5
        donated_huge_pages: 1
        partial_released_huge_pages: 0
        released_huge_pages: 2
        used_pages_in_subreleased_huge_pages: 499
      }
      at_minimum_huge_pages {
        num_pages: 0
        regular_huge_pages: 0
        donated_huge_pages: 0
        partial_released_huge_pages: 0
        released_huge_pages: 0
        used_pages_in_subreleased_huge_pages: 0
      }
      at_maximum_huge_pages {
        num_pages: 1774
        regular_huge_pages: 5
        donated_huge_pages: 1
        partial_released_huge_pages: 0
        released_huge_pages: 2
        used_pages_in_subreleased_huge_pages: 499
      }
    }
  }
)");
  for (const auto &alloc : allocs) {
    Delete(alloc);
  }
}

// Confirms that a timeseries that contains every epoch does not exceed the
// expected buffer capacity of 1 MiB.
TEST_P(FillerTest, CheckBufferSize) {
  const int kEpochs = 600;
  const absl::Duration kEpochLength = absl::Seconds(1);

  PAlloc big = Allocate(kPagesPerHugePage - 4);

  for (int i = 0; i < kEpochs; i += 2) {
    auto tiny = Allocate(2);
    Advance(kEpochLength);
    Delete(tiny);
    Advance(kEpochLength);
  }

  Delete(big);

  std::string buffer(1024 * 1024, '\0');
  {
    TCMalloc_Printer printer(&*buffer.begin(), buffer.size());
    PbtxtRegion region(&printer, kTop, /*indent=*/0);
    filler_.PrintInPbtxt(&region, filler_.used_pages() * 8192);
  }

  // We assume a maximum buffer size of 1 MiB. When increasing this size, ensure
  // that all places processing mallocz protos get updated as well.
  size_t buffer_size = strlen(buffer.c_str());
  printf("HugePageFiller buffer size: %zu\n", buffer_size);
  EXPECT_LE(buffer_size, 1024 * 1024);
}

INSTANTIATE_TEST_SUITE_P(All, FillerTest,
                         testing::Values(FillerPartialRerelease::Return,
                                         FillerPartialRerelease::Retain));

}  // namespace
}  // namespace tcmalloc
