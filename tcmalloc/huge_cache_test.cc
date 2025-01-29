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

#include "tcmalloc/huge_cache.h"

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#include <algorithm>
#include <cstring>
#include <memory>
#include <random>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "absl/memory/memory.h"
#include "absl/random/random.h"
#include "absl/strings/str_cat.h"
#include "absl/time/time.h"
#include "tcmalloc/huge_page_subrelease.h"
#include "tcmalloc/huge_pages.h"
#include "tcmalloc/internal/clock.h"
#include "tcmalloc/internal/config.h"
#include "tcmalloc/internal/logging.h"
#include "tcmalloc/mock_metadata_allocator.h"
#include "tcmalloc/mock_virtual_allocator.h"
#include "tcmalloc/pages.h"
#include "tcmalloc/stats.h"

namespace tcmalloc {
namespace tcmalloc_internal {
namespace {

using testing::Return;

class HugeCacheTest
    : public testing::TestWithParam<std::tuple<absl::Duration, bool>> {
 private:
  // Allow tests to modify the clock used by the cache.
  static int64_t clock_;

  static int64_t FakeClock() { return clock_; }

  static double GetFakeClockFrequency() {
    return absl::ToDoubleNanoseconds(absl::Seconds(2));
  }

  static void ResetClock() { clock_ = 1234; }

  class MockBackingInterface : public MemoryModifyFunction {
   public:
    MOCK_METHOD(bool, Unback, (PageId p, Length len), ());

    bool operator()(Range r) override { return Unback(r.p, r.n); }
  };

 protected:
  testing::NiceMock<MockBackingInterface> mock_unback_;

  HugeCacheTest() {
    // We don't use the first few bytes, because things might get weird
    // given zero pointers.
    vm_allocator_.backing_.resize(1024);
    ResetClock();
  }

  static void Advance(absl::Duration d) {
    clock_ += absl::ToDoubleSeconds(d) * GetFakeClockFrequency();
  }

  absl::Duration GetCacheTime() { return std::get<0>(GetParam()); }
  bool GetDemandBasedRelease() { return std::get<1>(GetParam()); }
  void Release(HugeRange r) { cache_.Release(r, GetDemandBasedRelease()); }

  FakeVirtualAllocator vm_allocator_;
  FakeMetadataAllocator metadata_allocator_;
  HugeAllocator alloc_{vm_allocator_, metadata_allocator_};
  HugeCache cache_{&alloc_, metadata_allocator_, mock_unback_, GetCacheTime(),
                   Clock{.now = FakeClock, .freq = GetFakeClockFrequency}};
};

int64_t HugeCacheTest::clock_{1234};

TEST_P(HugeCacheTest, Basic) {
  bool from;
  for (int i = 0; i < 100 * 1000; ++i) {
    Release(cache_.Get(NHugePages(1), &from));
  }
}

TEST_P(HugeCacheTest, Backing) {
  bool from;
  Release(cache_.Get(NHugePages(4), &from));
  EXPECT_TRUE(from);
  // We should be able to split up a large range...
  HugeRange r1 = cache_.Get(NHugePages(3), &from);
  EXPECT_FALSE(from);
  HugeRange r2 = cache_.Get(NHugePages(1), &from);
  EXPECT_FALSE(from);

  // and then merge it back.
  Release(r1);
  Release(r2);
  HugeRange r = cache_.Get(NHugePages(4), &from);
  EXPECT_FALSE(from);
  Release(r);
}

TEST_P(HugeCacheTest, Release) {
  bool from;
  const HugeLength one = NHugePages(1);
  Release(cache_.Get(NHugePages(5), &from));
  HugeRange r1, r2, r3, r4, r5;
  r1 = cache_.Get(one, &from);
  r2 = cache_.Get(one, &from);
  r3 = cache_.Get(one, &from);
  r4 = cache_.Get(one, &from);
  r5 = cache_.Get(one, &from);
  Release(r1);
  Release(r2);
  Release(r3);
  Release(r4);
  Release(r5);

  r1 = cache_.Get(one, &from);
  ASSERT_EQ(false, from);
  r2 = cache_.Get(one, &from);
  ASSERT_EQ(false, from);
  r3 = cache_.Get(one, &from);
  ASSERT_EQ(false, from);
  r4 = cache_.Get(one, &from);
  ASSERT_EQ(false, from);
  r5 = cache_.Get(one, &from);
  ASSERT_EQ(false, from);
  Release(r1);
  Release(r2);
  Release(r5);

  ASSERT_EQ(NHugePages(3), cache_.size());
  EXPECT_CALL(mock_unback_, Unback(r5.start().first_page(), kPagesPerHugePage))
      .WillOnce(Return(true));
  EXPECT_EQ(NHugePages(1), cache_.ReleaseCachedPages(NHugePages(1)));
  Release(r3);
  Release(r4);

  EXPECT_CALL(mock_unback_,
              Unback(r1.start().first_page(), 4 * kPagesPerHugePage))
      .WillOnce(Return(true));
  EXPECT_EQ(NHugePages(4), cache_.ReleaseCachedPages(NHugePages(200)));
}

TEST_P(HugeCacheTest, ReleaseFailure) {
  bool from;
  const HugeLength one = NHugePages(1);
  Release(cache_.Get(NHugePages(5), &from));
  HugeRange r1, r2, r3, r4, r5;
  r1 = cache_.Get(one, &from);
  r2 = cache_.Get(one, &from);
  r3 = cache_.Get(one, &from);
  r4 = cache_.Get(one, &from);
  r5 = cache_.Get(one, &from);
  Release(r1);
  Release(r2);
  Release(r3);
  Release(r4);
  Release(r5);

  r1 = cache_.Get(one, &from);
  ASSERT_EQ(false, from);
  r2 = cache_.Get(one, &from);
  ASSERT_EQ(false, from);
  r3 = cache_.Get(one, &from);
  ASSERT_EQ(false, from);
  r4 = cache_.Get(one, &from);
  ASSERT_EQ(false, from);
  r5 = cache_.Get(one, &from);
  ASSERT_EQ(false, from);
  Release(r1);
  Release(r2);
  Release(r5);

  ASSERT_EQ(NHugePages(3), cache_.size());
  EXPECT_CALL(mock_unback_,
              Unback(r5.start().first_page(), 1 * kPagesPerHugePage))
      .WillOnce(Return(false));
  EXPECT_EQ(NHugePages(0), cache_.ReleaseCachedPages(NHugePages(1)));
  Release(r3);
  Release(r4);

  EXPECT_CALL(mock_unback_,
              Unback(r1.start().first_page(), 5 * kPagesPerHugePage))
      .WillOnce(Return(false));
  EXPECT_EQ(NHugePages(0), cache_.ReleaseCachedPages(NHugePages(200)));
}

TEST_P(HugeCacheTest, Stats) {
  bool from;
  HugeRange r = cache_.Get(NHugePages(1 + 1 + 2 + 1 + 3), &from);
  HugeRange r1, r2, r3, spacer1, spacer2;
  std::tie(r1, spacer1) = Split(r, NHugePages(1));
  std::tie(spacer1, r2) = Split(spacer1, NHugePages(1));
  std::tie(r2, spacer2) = Split(r2, NHugePages(2));
  std::tie(spacer2, r3) = Split(spacer2, NHugePages(1));
  Release(r1);
  Release(r2);
  Release(r3);

  ASSERT_EQ(NHugePages(6), cache_.size());
  r1 = cache_.Get(NHugePages(1), &from);
  ASSERT_EQ(false, from);
  r2 = cache_.Get(NHugePages(2), &from);
  ASSERT_EQ(false, from);
  r3 = cache_.Get(NHugePages(3), &from);
  ASSERT_EQ(false, from);

  struct Helper {
    static void Stat(const HugeCache& cache, size_t* spans,
                     Length* pages_backed, Length* pages_unbacked) {
      LargeSpanStats large;
      cache.AddSpanStats(nullptr, &large);

      *spans = large.spans;
      *pages_backed = large.normal_pages;
      *pages_unbacked = large.returned_pages;
    }
  };

  size_t spans;
  Length pages_backed;
  Length pages_unbacked;

  Release(r1);
  Helper::Stat(cache_, &spans, &pages_backed, &pages_unbacked);
  EXPECT_EQ(Length(0), pages_unbacked);
  EXPECT_EQ(1, spans);
  EXPECT_EQ(NHugePages(1).in_pages(), pages_backed);

  Release(r2);
  Helper::Stat(cache_, &spans, &pages_backed, &pages_unbacked);
  EXPECT_EQ(Length(0), pages_unbacked);
  EXPECT_EQ(2, spans);
  EXPECT_EQ(NHugePages(3).in_pages(), pages_backed);

  Release(r3);
  Helper::Stat(cache_, &spans, &pages_backed, &pages_unbacked);
  EXPECT_EQ(Length(0), pages_unbacked);
  EXPECT_EQ(3, spans);
  EXPECT_EQ(NHugePages(6).in_pages(), pages_backed);
}

static double Frac(HugeLength num, HugeLength denom) {
  return static_cast<double>(num.raw_num()) / denom.raw_num();
}

// Tests that the cache can grow to fit a working set. The two cache shrinking
// mechanisms, demand-based release and limit-based release, use two different
// paths to shrink the cache (ReleaseCachedPagesByDemand vs. Release). We
// test both paths here.
TEST_P(HugeCacheTest, Growth) {
  EXPECT_CALL(mock_unback_, Unback(testing::_, testing::_))
      .WillRepeatedly(Return(true));

  bool released;
  absl::BitGen rng;
  // fragmentation is a bit of a challenge
  std::uniform_int_distribution<size_t> sizes(1, 5);
  // fragment the cache badly.
  std::vector<HugeRange> keep;
  std::vector<HugeRange> drop;
  for (int i = 0; i < 1000; ++i) {
    auto& l = std::bernoulli_distribution()(rng) ? keep : drop;
    l.push_back(cache_.Get(NHugePages(sizes(rng)), &released));
  }

  for (auto r : drop) {
    Release(r);
  }

  // See the TODO in HugeCache::MaybeGrowCache; without this delay,
  // the above fragmentation plays merry havoc with our instrumentation.
  Advance(absl::Seconds(30));
  // Requests a best-effort demand-based release to shrink the cache.
  if (GetDemandBasedRelease()) {
    cache_.ReleaseCachedPagesByDemand(
        cache_.size(),
        SkipSubreleaseIntervals{.short_interval = absl::Seconds(10),
                                .long_interval = absl::Seconds(10)},
        /*hit_limit=*/false);
  }
  // Test that our cache can grow to fit a working set.
  HugeLength hot_set_sizes[] = {NHugePages(5), NHugePages(10), NHugePages(100),
                                NHugePages(10000)};

  for (const HugeLength hot : hot_set_sizes) {
    SCOPED_TRACE(absl::StrCat("cache size = ", hot.in_bytes() / 1024.0 / 1024.0,
                              " MiB"));
    // Exercise the cache allocating about <hot> worth of data. After
    // a brief warmup phase, we should do this without needing to back much.
    auto alloc = [&]() -> std::pair<HugeLength, HugeLength> {
      HugeLength got = NHugePages(0);
      HugeLength needed_backing = NHugePages(0);
      std::vector<HugeRange> items;
      while (got < hot) {
        HugeLength rest = hot - got;
        HugeLength l = std::min(rest, NHugePages(sizes(rng)));
        got += l;
        items.push_back(cache_.Get(l, &released));
        if (released) needed_backing += l;
      }
      for (auto r : items) {
        Release(r);
      }
      // Requests a demand-based release. The target will increase to
      // kFractionToReleaseFromCache of the cache, and that is enough to trim
      // the fragmentation.
      if (GetDemandBasedRelease()) {
        cache_.ReleaseCachedPagesByDemand(
            NHugePages(0),
            SkipSubreleaseIntervals{.short_interval = absl::Seconds(1),
                                    .long_interval = absl::Seconds(1)},
            /*hit_limit=*/false);
      }
      return {needed_backing, got};
    };

    // warmup - we're allowed to incur misses and be too big.
    for (int i = 0; i < 2; ++i) {
      alloc();
    }

    HugeLength needed_backing = NHugePages(0);
    HugeLength total = NHugePages(0);
    for (int i = 0; i < 16; ++i) {
      auto r = alloc();
      needed_backing += r.first;
      total += r.second;
      // Cache shouldn't have just grown arbitrarily
      const HugeLength cached = cache_.size();
      // Allow us 10% slop, but don't get out of bed for tiny caches anyway.
      const double ratio = Frac(cached, hot);
      SCOPED_TRACE(
          absl::StrCat(cached.raw_num(), "hps ", Frac(r.first, r.second)));
      if (ratio > 1 && cached > NHugePages(16)) {
        EXPECT_LE(ratio, 1.1);
      }
    }
    // approximately, given the randomized sizing...

    const double ratio = Frac(needed_backing, total);
    EXPECT_LE(ratio, 0.3);
  }
}

// If we repeatedly grow and shrink, but do so very slowly, we should *not*
// cache the large variation.
TEST_P(HugeCacheTest, SlowGrowthUncached) {
  // This test expects the cache to stay small when using unbacking with
  // Release(). Hence we skip it when demand-based release is enabled.
  if (GetDemandBasedRelease()) {
    GTEST_SKIP();
  }
  EXPECT_CALL(mock_unback_, Unback(testing::_, testing::_))
      .WillRepeatedly(Return(true));
  absl::Duration cache_time = GetCacheTime();

  absl::BitGen rng;
  std::uniform_int_distribution<size_t> sizes(1, 10);
  for (int i = 0; i < 20; ++i) {
    std::vector<HugeRange> rs;
    for (int j = 0; j < 20; ++j) {
      Advance(cache_time);
      bool released;
      rs.push_back(cache_.Get(NHugePages(sizes(rng)), &released));
    }
    HugeLength max_cached = NHugePages(0);
    for (auto r : rs) {
      Advance(cache_time);
      Release(r);
      max_cached = std::max(max_cached, cache_.size());
    }
    EXPECT_GE(NHugePages(10), max_cached);
  }
}

// If very rarely we have a huge increase in usage, it shouldn't be cached.
TEST_P(HugeCacheTest, SpikesUncached) {
  // This test expects the cache to stay small when using unbacking with
  // Release(). Hence we skip it when demand-based release is enabled.
  if (GetDemandBasedRelease()) {
    GTEST_SKIP();
  }
  EXPECT_CALL(mock_unback_, Unback(testing::_, testing::_))
      .WillRepeatedly(Return(true));
  absl::Duration cache_time = GetCacheTime();
  absl::BitGen rng;
  std::uniform_int_distribution<size_t> sizes(1, 10);
  for (int i = 0; i < 20; ++i) {
    std::vector<HugeRange> rs;
    for (int j = 0; j < 2000; ++j) {
      bool released;
      rs.push_back(cache_.Get(NHugePages(sizes(rng)), &released));
    }
    HugeLength max_cached = NHugePages(0);
    for (auto r : rs) {
      Release(r);
      max_cached = std::max(max_cached, cache_.size());
    }
    EXPECT_GE(NHugePages(10), max_cached);
    Advance(10 * cache_time);
  }
}

// If we allocate a spike occasionally but having demand-based release enabled,
// all freed hugepages will be cached even though the cache limit is low. This
// is because the cache shrinking mechanism in Release() is bypassed when
// demand-based release is enabled.
TEST_P(HugeCacheTest, SpikesCachedNoUnback) {
  // This test expects no cache shirking in Release(). Hence we skip it when
  // demand-based release is disabled.
  if (!GetDemandBasedRelease()) {
    GTEST_SKIP();
  }
  absl::Duration cache_time = GetCacheTime();
  for (int i = 0; i < 20; ++i) {
    std::vector<HugeRange> rs;
    for (int j = 0; j < 200; ++j) {
      bool released;
      rs.push_back(cache_.Get(NHugePages(5), &released));
    }
    HugeLength max_cached = NHugePages(0);
    for (auto r : rs) {
      Release(r);
      max_cached = std::max(max_cached, cache_.size());
    }
    EXPECT_EQ(max_cached, NHugePages(1000));
    // The limit never changed as the growth mechanism sees no value in
    // preparing for occasional peaks (i.e., shrink and grow in cache_time
    // are not balanced).
    EXPECT_EQ(cache_.limit(), NHugePages(10));
    Advance(10 * cache_time);
  }
}

// If very rarely we have a huge *decrease* in usage, it *should* be cached.
TEST_P(HugeCacheTest, DipsCached) {
  absl::BitGen rng;
  std::uniform_int_distribution<size_t> sizes(1, 10);
  absl::Duration cache_time = GetCacheTime();
  for (int i = 0; i < 20; ++i) {
    std::vector<HugeRange> rs;
    HugeLength got = NHugePages(0);
    HugeLength uncached = NHugePages(0);
    for (int j = 0; j < 2000; ++j) {
      bool released;
      HugeLength n = NHugePages(sizes(rng));
      rs.push_back(cache_.Get(n, &released));
      got += n;
      if (released) uncached += n;
    }
    // Most of our time is at high usage...
    Advance(10 * cache_time);
    // Now immediately release and reallocate.
    for (auto r : rs) {
      Release(r);
    }

    // warmup
    if (i >= 2) {
      EXPECT_GE(0.07, Frac(uncached, got));
    }
  }
}

// Suppose in a previous era of behavior we needed a giant cache,
// but now we don't.  Do we figure this out promptly?
TEST_P(HugeCacheTest, Shrink) {
  // This test expects the cache to shrink in Release() after the working set
  // size is reduced. Hence we skip it when demand-based release is enabled.
  if (GetDemandBasedRelease()) {
    GTEST_SKIP();
  }
  EXPECT_CALL(mock_unback_, Unback(testing::_, testing::_))
      .WillRepeatedly(Return(true));
  absl::BitGen rng;
  std::uniform_int_distribution<size_t> sizes(1, 10);
  absl::Duration cache_time = GetCacheTime();
  for (int i = 0; i < 20; ++i) {
    std::vector<HugeRange> rs;
    for (int j = 0; j < 2000; ++j) {
      HugeLength n = NHugePages(sizes(rng));
      bool released;
      rs.push_back(cache_.Get(n, &released));
    }
    for (auto r : rs) {
      Release(r);
    }
  }

  ASSERT_LE(NHugePages(10000), cache_.size());

  for (int i = 0; i < 30; ++i) {
    // New working set <= 20 pages, arranging the allocation rounds happen in
    // different cache limit updating windows (> cache_time * 2) so we can
    // shrink the cache gradually in each round.
    Advance(cache_time * 3);

    // And do some work.
    for (int j = 0; j < 100; ++j) {
      bool released;
      HugeRange r1 = cache_.Get(NHugePages(sizes(rng)), &released);
      HugeRange r2 = cache_.Get(NHugePages(sizes(rng)), &released);
      Release(r1);
      Release(r2);
    }
  }
  // The cache should have shrunk to the working set size.
  ASSERT_GE(NHugePages(25), cache_.size());
  ASSERT_GE(NHugePages(25), cache_.limit());
}

// In demand-based release, we want to release as much as possible when the
// hit_limit is set.
TEST_P(HugeCacheTest, ReleaseByDemandHardRelease) {
  // This test is sensitive to the number of pages per hugepage, as we are
  // printing raw stats.
  if (!GetDemandBasedRelease() || (kPagesPerHugePage != Length(256))) {
    GTEST_SKIP();
  }
  EXPECT_CALL(mock_unback_, Unback(testing::_, testing::_))
      .WillRepeatedly(Return(true));
  bool released;
  HugeRange r = cache_.Get(NHugePages(1000), &released);
  Release(r);
  ASSERT_EQ(cache_.size(), NHugePages(1000));
  // Releases half of the cache with hit_limit set.
  HugeLength unbacked_1 = cache_.ReleaseCachedPagesByDemand(
      NHugePages(500), SkipSubreleaseIntervals{}, /*hit_limit=*/true);
  EXPECT_EQ(unbacked_1, NHugePages(500));
  //  Releases the remaining using invalid intervals.
  HugeLength unbacked_2 = cache_.ReleaseCachedPagesByDemand(
      NHugePages(1000), SkipSubreleaseIntervals{}, /*hit_limit=*/false);
  EXPECT_EQ(unbacked_2, NHugePages(500));
  std::string buffer(1024 * 1024, '\0');
  {
    Printer printer(&*buffer.begin(), buffer.size());
    cache_.Print(printer);
  }
  buffer.resize(strlen(buffer.c_str()));

  EXPECT_THAT(buffer, testing::HasSubstr(R"(
HugeCache: 0 MiB fast unbacked, 2000 MiB periodic
)"));
  EXPECT_THAT(buffer, testing::HasSubstr(R"(
HugeCache: Since startup, 1000 hugepages released, (500 hugepages due to reaching tcmalloc limit)
)"));
  // The skip-subrelease mechanism is bypassed for both requests.
  EXPECT_THAT(buffer, testing::HasSubstr(R"(
HugeCache: Since the start of the execution, 0 subreleases (0 pages) were skipped due to the sum of short-term (0s) fluctuations and long-term (0s) trends.
HugeCache: 0.0000% of decisions confirmed correct, 0 pending (0.0000% of pages, 0 pending).
HugeCache: Subrelease stats last 10 min: total 256000 pages subreleased (0 pages from partial allocs), 0 hugepages broken
)"));
}

// Tests that we can increase the release target to a fraction
// (kFractionToReleaseFromCache) of HugeCache. This can happen regardless of the
// initial value of the target.
TEST_P(HugeCacheTest, ReleaseByDemandIncreaseTarget) {
  if (!GetDemandBasedRelease()) {
    GTEST_SKIP();
  }
  EXPECT_CALL(mock_unback_, Unback(testing::_, testing::_))
      .WillRepeatedly(Return(true));
  bool released;
  // (Current - 3 min) Max: 60 hps, Min: 50 hps.
  HugeRange peak_1a = cache_.Get(NHugePages(50), &released);
  HugeRange peak_1b = cache_.Get(NHugePages(10), &released);
  Advance(absl::Minutes(1));

  // (Current - 2 min) Max: 170 hps, Min: 70 hps.
  HugeRange peak_2a = cache_.Get(NHugePages(100), &released);
  HugeRange peak_2b = cache_.Get(NHugePages(10), &released);
  Release(peak_2a);
  Advance(absl::Minutes(1));

  // (Current - 1 minute) Max: 20 hps, Min: 10 hps.
  Release(peak_1a);
  Release(peak_2b);
  Advance(absl::Minutes(1));

  // (Current) Max: 0 hps, Min: 0 hps.
  Release(peak_1b);
  EXPECT_EQ(cache_.size(), NHugePages(170));
  EXPECT_EQ(cache_.usage(), NHugePages(0));

  // The past demand is 80 hps (short 10 hps + long 70 hps), and we can unback
  // 34 hps (170 hps * kFractionToReleaseFromCache), more than the release
  // target (0 hps).
  HugeLength unbacked_1 = cache_.ReleaseCachedPagesByDemand(
      NHugePages(0),
      SkipSubreleaseIntervals{.short_interval = absl::Seconds(120),
                              .long_interval = absl::Seconds(180)},
      /*hit_limit=*/false);
  EXPECT_EQ(unbacked_1, NHugePages(34));
  // Repeats the test using a non-zero target.
  EXPECT_EQ(cache_.size(), NHugePages(136));
  HugeLength unbacked_2 = cache_.ReleaseCachedPagesByDemand(
      NHugePages(10),
      SkipSubreleaseIntervals{.short_interval = absl::Seconds(120),
                              .long_interval = absl::Seconds(180)},
      /*hit_limit=*/false);
  EXPECT_EQ(unbacked_2, NHugePages(28));

  // Tests that we always manage to protect the cache limit (10 hps) while
  // increasing the target. First, force the cache close to the limit using a
  // crafted target.
  HugeLength unbacked_3 = cache_.ReleaseCachedPagesByDemand(
      NHugePages(97), SkipSubreleaseIntervals{}, /*hit_limit=*/true);
  EXPECT_EQ(unbacked_3, NHugePages(97));
  EXPECT_EQ(cache_.size(), NHugePages(11));
  // Then, ask for release using target zero.
  HugeLength unbacked_4 = cache_.ReleaseCachedPagesByDemand(
      NHugePages(0), SkipSubreleaseIntervals{}, /*hit_limit=*/true);
  EXPECT_EQ(unbacked_4, NHugePages(1));
  EXPECT_EQ(cache_.size(), NHugePages(10));
  // Now the cache is at the limit. Checks if that can be protected.
  HugeLength unbacked_5 = cache_.ReleaseCachedPagesByDemand(
      NHugePages(0), SkipSubreleaseIntervals{}, /*hit_limit=*/true);
  EXPECT_EQ(unbacked_5, NHugePages(0));

  // Finally, show that we can release the limit if requested. There has been no
  // demand in the past 10s so we can release the rest of the cache.
  HugeLength unbacked_6 = cache_.ReleaseCachedPagesByDemand(
      NHugePages(100),
      SkipSubreleaseIntervals{.short_interval = absl::Seconds(10),
                              .long_interval = absl::Seconds(10)},
      /*hit_limit=*/false);
  EXPECT_EQ(unbacked_6, NHugePages(10));
}

// Tests releasing zero pages when the cache size and demand are both zero.
TEST_P(HugeCacheTest, ReleaseByDemandReleaseZero) {
  if (!GetDemandBasedRelease()) {
    GTEST_SKIP();
  }
  EXPECT_EQ(cache_.ReleaseCachedPagesByDemand(
                NHugePages(0),
                SkipSubreleaseIntervals{.short_interval = absl::Seconds(1),
                                        .long_interval = absl::Seconds(1)},
                /*hit_limit=*/false),
            NHugePages(0));
}

// Tests that releasing target is not affected if the demand history is empty.
TEST_P(HugeCacheTest, ReleaseByDemandNoHistory) {
  if (!GetDemandBasedRelease()) {
    GTEST_SKIP();
  }
  EXPECT_CALL(mock_unback_, Unback(testing::_, testing::_))
      .WillRepeatedly(Return(true));
  // First we make sure that the cache is not empty.
  bool released;
  Release(cache_.Get(NHugePages(10), &released));
  EXPECT_EQ(cache_.size(), NHugePages(10));
  // Then we advance the time to make sure that the demand history is empty.
  Advance(absl::Minutes(30));
  EXPECT_EQ(cache_.ReleaseCachedPagesByDemand(
                NHugePages(10),
                SkipSubreleaseIntervals{.short_interval = absl::Seconds(1),
                                        .long_interval = absl::Seconds(1)},
                /*hit_limit=*/false),
            NHugePages(10));
}

// Tests that the demand is capped by peak within the default interval (5 mins).
TEST_P(HugeCacheTest, ReleaseByDemandCappedByDemandPeak) {
  if (!GetDemandBasedRelease()) {
    GTEST_SKIP();
  }
  EXPECT_CALL(mock_unback_, Unback(testing::_, testing::_))
      .WillRepeatedly(Return(true));
  // Generates a demand pattern that can cause the sum-of-peak issue.
  bool released;
  // The diff peak: 20 hps - 1 hps = 19 hps.
  HugeRange diff_a = cache_.Get(NHugePages(1), &released);
  HugeRange diff_b = cache_.Get(NHugePages(20), &released);
  Release(diff_a);
  Release(diff_b);
  Advance(absl::Minutes(5));
  // The long-term demand peak: 15 hps.
  HugeRange peak = cache_.Get(NHugePages(15), &released);
  Advance(absl::Minutes(1));
  Release(peak);
  EXPECT_EQ(cache_.size(), NHugePages(21));
  // Releases partial of the cache as the demand is capped by the 5-mins' peak
  // (15 hps).
  EXPECT_EQ(cache_.ReleaseCachedPagesByDemand(
                NHugePages(100),
                SkipSubreleaseIntervals{.short_interval = absl::Minutes(10),
                                        .long_interval = absl::Minutes(10)},
                /*hit_limit=*/false),
            NHugePages(6));
  // Releases the rest of the cache.
  EXPECT_EQ(cache_.ReleaseCachedPagesByDemand(NHugePages(100),
                                              SkipSubreleaseIntervals{},
                                              /*hit_limit=*/false),
            NHugePages(15));
}

// Tests demand-based skip release. The test is a modified version of the
// FillerTest.SkipSubrelease test by removing parts designed particularly for
// subrelease.
TEST_P(HugeCacheTest, ReleaseByDemandSkipRelease) {
  // This test is sensitive to the number of pages per hugepage, as we are
  // printing raw stats.
  if (!GetDemandBasedRelease() || (kPagesPerHugePage != Length(256))) {
    GTEST_SKIP();
  }

  EXPECT_CALL(mock_unback_, Unback(testing::_, testing::_))
      .WillRepeatedly(Return(true));
  // First it generates a peak (the long-term demand peak) and waits for
  // time_interval(a). Then, it generates a higher peak that contains the
  // short-term fluctuation peak, and waits for time_interval(b). It then
  // generates a trough in demand and asks to release. Finally, it waits for
  // time_interval(c) to generate the highest peak which is used for evaluating
  // release correctness.
  const auto demand_pattern =
      [&](absl::Duration a, absl::Duration b, absl::Duration c,
          SkipSubreleaseIntervals intervals, bool expected_release) {
        bool released;
        // First peak: min_demand 10 hps , max_demand 15 hps, diff 10 hps.
        HugeRange peak_1a = cache_.Get(NHugePages(10), &released);
        HugeRange peak_1b = cache_.Get(NHugePages(5), &released);
        Advance(a);
        // Second peak: min_demand 0 hps, max_demand 20 hps, diff 20 hps.
        Release(peak_1a);
        Release(peak_1b);
        HugeRange peak_2a = cache_.Get(NHugePages(15), &released);
        HugeRange peak_2b = cache_.Get(NHugePages(5), &released);
        EXPECT_EQ(cache_.usage(), NHugePages(20));
        EXPECT_EQ(cache_.size(), NHugePages(0));
        Advance(b);
        // Trough: min_demand 5 hps, max_demand 5 hps, diff 0 hps.
        Release(peak_2a);
        EXPECT_EQ(cache_.usage(), NHugePages(5));
        EXPECT_EQ(cache_.size(), NHugePages(15));
        // Release is capped by the cache size.
        EXPECT_EQ(cache_.ReleaseCachedPagesByDemand(NHugePages(100), intervals,
                                                    /*hit_limit=*/false),
                  expected_release ? NHugePages(15) : NHugePages(0));
        Advance(c);
        // Third peak: min_demand 25 hps, max_demand 30 hps, diff 5 hps.
        // Note, skip-subrelease evaluates the correctness of skipped releases
        // using the first demand update recorded in an epoch (25 hps for this
        // case).
        HugeRange peak_3a = cache_.Get(NHugePages(20), &released);
        HugeRange peak_3b = cache_.Get(NHugePages(5), &released);
        EXPECT_EQ(cache_.usage(), NHugePages(30));
        Release(peak_2b);
        Release(peak_3a);
        Release(peak_3b);
        // If the previous release is skipped, the cache size is larger due to
        // fragmentation.
        EXPECT_EQ(cache_.ReleaseCachedPagesByDemand(NHugePages(100),
                                                    SkipSubreleaseIntervals{},
                                                    /*hit_limit=*/false),
                  expected_release ? NHugePages(30) : NHugePages(40));
        Advance(absl::Minutes(30));
      };
  {
    // Skip release feature is disabled if all intervals are zero.
    SCOPED_TRACE("demand_pattern 1");
    demand_pattern(absl::Minutes(1), absl::Minutes(1), absl::Minutes(4),
                   SkipSubreleaseIntervals{}, /*expected_release=*/true);
  }
  {
    // Uses short-term and long-term intervals (combined demand is 30 hps but
    // capped by maximum demand in 10 mins, 20 hps), incorrectly skipped 15 hps.
    SCOPED_TRACE("demand_pattern 2");
    demand_pattern(absl::Minutes(3), absl::Minutes(2), absl::Minutes(7),
                   SkipSubreleaseIntervals{.short_interval = absl::Minutes(3),
                                           .long_interval = absl::Minutes(6)},
                   /*expected_release=*/false);
  }
  {
    // Uses short-term and long-term intervals (combined demand 5 hps), released
    // all free hps.
    SCOPED_TRACE("demand_pattern 3");
    demand_pattern(absl::Minutes(4), absl::Minutes(2), absl::Minutes(3),
                   SkipSubreleaseIntervals{.short_interval = absl::Minutes(1),
                                           .long_interval = absl::Minutes(2)},
                   /*expected_release=*/true);
  }
  {
    // Uses only short-term interval (demand 20 hps), correctly skipped 15 hps.
    SCOPED_TRACE("demand_pattern 4");
    demand_pattern(absl::Minutes(4), absl::Minutes(2), absl::Minutes(3),
                   SkipSubreleaseIntervals{.short_interval = absl::Minutes(3)},
                   /*expected_release=*/false);
  }
  {
    // Uses only long-term interval (demand 5 hps), released all free pages.
    SCOPED_TRACE("demand_pattern 5");
    demand_pattern(absl::Minutes(4), absl::Minutes(2), absl::Minutes(3),
                   SkipSubreleaseIntervals{.long_interval = absl::Minutes(2)},
                   /*expected_release=*/true);
  }
  // This captures a corner case: If we hit another peak immediately after a
  // release decision (recorded in the same epoch), do not count this as
  // a correct release decision.
  {
    SCOPED_TRACE("demand_pattern 6");
    demand_pattern(absl::Milliseconds(10), absl::Milliseconds(10),
                   absl::Milliseconds(10),
                   SkipSubreleaseIntervals{.short_interval = absl::Minutes(1),
                                           .long_interval = absl::Minutes(2)},
                   /*expected_release=*/false);
  }
  // Ensure that the tracker is updated.
  bool released;
  HugeRange tiny = cache_.Get(NHugePages(1), &released);
  Release(tiny);
  std::string buffer(1024 * 1024, '\0');
  {
    Printer printer(&*buffer.begin(), buffer.size());
    cache_.Print(printer);
  }
  buffer.resize(strlen(buffer.c_str()));

  EXPECT_THAT(buffer, testing::HasSubstr(R"(
HugeCache: Since the start of the execution, 3 subreleases (11520 pages) were skipped due to the sum of short-term (60s) fluctuations and long-term (120s) trends.
HugeCache: 33.3333% of decisions confirmed correct, 0 pending (33.3333% of pages, 0 pending).
HugeCache: Subrelease stats last 10 min: total 0 pages subreleased (0 pages from partial allocs), 0 hugepages broken
)"));
}

// Tests the skipping decisions are reported correctly, particularly for the
// demand peaks used in correctness evaluation.
TEST_P(HugeCacheTest, ReleaseByDemandSkipReleaseReport) {
  // This test is sensitive to the number of pages per hugepage, as we are
  // printing raw stats.
  if (!GetDemandBasedRelease() || (kPagesPerHugePage != Length(256))) {
    GTEST_SKIP();
  }
  EXPECT_CALL(mock_unback_, Unback(testing::_, testing::_))
      .WillRepeatedly(Return(true));

  // Reports skip release using the recent demand peak (23 hps): it is
  // smaller than the current capacity (33 hps) when 8 hps are skipped.
  // The skipping is correct as the future demand is 25 hps.
  bool released;
  HugeRange peak_1a = cache_.Get(NHugePages(10), &released);
  HugeRange peak_1b = cache_.Get(NHugePages(8), &released);
  Advance(absl::Minutes(2));
  Release(peak_1a);
  HugeRange peak_2a = cache_.Get(NHugePages(15), &released);
  Release(peak_1b);
  EXPECT_EQ(cache_.usage(), NHugePages(15));
  EXPECT_EQ(cache_.size(), NHugePages(18));
  EXPECT_EQ(cache_.ReleaseCachedPagesByDemand(
                NHugePages(30),
                SkipSubreleaseIntervals{.short_interval = absl::Minutes(3),
                                        .long_interval = absl::Minutes(3)},
                /*hit_limit=*/false),
            NHugePages(10));
  Advance(absl::Minutes(3));
  HugeRange peak_3a = cache_.Get(NHugePages(10), &released);
  Release(peak_2a);
  Release(peak_3a);
  EXPECT_EQ(cache_.ReleaseCachedPagesByDemand(NHugePages(100),
                                              SkipSubreleaseIntervals{},
                                              /*hit_limit=*/false),
            NHugePages(33));
  Advance(absl::Minutes(30));

  // Reports skip release using the current capacity (15 hps): it
  // is smaller than the recent peak (20 hps) when 10 hps are skipped. They are
  // correctly skipped as the future demand is 18 hps.
  HugeRange peak_4a = cache_.Get(NHugePages(10), &released);
  HugeRange peak_4b = cache_.Get(NHugePages(10), &released);
  Release(peak_4a);
  EXPECT_EQ(cache_.ReleaseCachedPagesByDemand(NHugePages(10),
                                              SkipSubreleaseIntervals{}, false),
            NHugePages(10));
  Advance(absl::Minutes(2));
  HugeRange peak_5a = cache_.Get(NHugePages(5), &released);
  Release(peak_4b);
  EXPECT_EQ(cache_.ReleaseCachedPagesByDemand(
                NHugePages(10),
                SkipSubreleaseIntervals{.short_interval = absl::Minutes(3),
                                        .long_interval = absl::Minutes(3)},
                /*hit_limit=*/false),
            NHugePages(0));
  Advance(absl::Minutes(3));
  HugeRange peak_6a = cache_.Get(NHugePages(10), &released);
  HugeRange peak_6b = cache_.Get(NHugePages(3), &released);
  Release(peak_5a);
  Release(peak_6a);
  Release(peak_6b);
  EXPECT_EQ(cache_.ReleaseCachedPagesByDemand(NHugePages(100),
                                              SkipSubreleaseIntervals{},
                                              /*hit_limit=*/false),
            NHugePages(18));
  Advance(absl::Minutes(30));

  std::string buffer(1024 * 1024, '\0');
  {
    Printer printer(&*buffer.begin(), buffer.size());
    cache_.Print(printer);
  }
  buffer.resize(strlen(buffer.c_str()));

  EXPECT_THAT(buffer, testing::HasSubstr(R"(
HugeCache: Since the start of the execution, 2 subreleases (4608 pages) were skipped due to the sum of short-term (180s) fluctuations and long-term (180s) trends.
HugeCache: 100.0000% of decisions confirmed correct, 0 pending (100.0000% of pages, 0 pending).
)"));
}

TEST_P(HugeCacheTest, Usage) {
  bool released;

  auto r1 = cache_.Get(NHugePages(10), &released);
  EXPECT_EQ(NHugePages(10), cache_.usage());

  auto r2 = cache_.Get(NHugePages(100), &released);
  EXPECT_EQ(NHugePages(110), cache_.usage());

  Release(r1);
  EXPECT_EQ(NHugePages(100), cache_.usage());

  // Pretend we unbacked this.
  cache_.ReleaseUnbacked(r2);
  EXPECT_EQ(NHugePages(0), cache_.usage());
}

class MinMaxTrackerTest : public testing::Test {
 protected:
  void Advance(absl::Duration d) {
    clock_ += absl::ToDoubleSeconds(d) * GetFakeClockFrequency();
  }

  static int64_t FakeClock() { return clock_; }

  static double GetFakeClockFrequency() {
    return absl::ToDoubleNanoseconds(absl::Seconds(2));
  }

 private:
  static int64_t clock_;
};

int64_t MinMaxTrackerTest::clock_{0};

TEST_F(MinMaxTrackerTest, Works) {
  const absl::Duration kDuration = absl::Seconds(2);
  MinMaxTracker<> tracker{
      Clock{.now = FakeClock, .freq = GetFakeClockFrequency}, kDuration};

  tracker.Report(NHugePages(0));
  EXPECT_EQ(NHugePages(0), tracker.MaxOverTime(kDuration));
  EXPECT_EQ(NHugePages(0), tracker.MinOverTime(kDuration));

  tracker.Report(NHugePages(10));
  EXPECT_EQ(NHugePages(10), tracker.MaxOverTime(kDuration));
  EXPECT_EQ(NHugePages(0), tracker.MinOverTime(kDuration));

  tracker.Report(NHugePages(5));
  EXPECT_EQ(NHugePages(10), tracker.MaxOverTime(kDuration));
  EXPECT_EQ(NHugePages(0), tracker.MinOverTime(kDuration));

  tracker.Report(NHugePages(100));
  EXPECT_EQ(NHugePages(100), tracker.MaxOverTime(kDuration));
  EXPECT_EQ(NHugePages(0), tracker.MinOverTime(kDuration));

  // Some tests for advancing time
  Advance(kDuration / 3);
  tracker.Report(NHugePages(2));
  EXPECT_EQ(NHugePages(2), tracker.MaxOverTime(absl::Nanoseconds(1)));
  EXPECT_EQ(NHugePages(100), tracker.MaxOverTime(kDuration / 2));
  EXPECT_EQ(NHugePages(100), tracker.MaxOverTime(kDuration));
  EXPECT_EQ(NHugePages(2), tracker.MinOverTime(absl::Nanoseconds(1)));
  EXPECT_EQ(NHugePages(0), tracker.MinOverTime(kDuration / 2));
  EXPECT_EQ(NHugePages(0), tracker.MinOverTime(kDuration));

  Advance(kDuration / 3);
  tracker.Report(NHugePages(5));
  EXPECT_EQ(NHugePages(5), tracker.MaxOverTime(absl::Nanoseconds(1)));
  EXPECT_EQ(NHugePages(5), tracker.MaxOverTime(kDuration / 2));
  EXPECT_EQ(NHugePages(100), tracker.MaxOverTime(kDuration));
  EXPECT_EQ(NHugePages(5), tracker.MinOverTime(absl::Nanoseconds(1)));
  EXPECT_EQ(NHugePages(2), tracker.MinOverTime(kDuration / 2));
  EXPECT_EQ(NHugePages(0), tracker.MinOverTime(kDuration));

  // This should annihilate everything.
  Advance(kDuration * 2);
  tracker.Report(NHugePages(1));
  EXPECT_EQ(NHugePages(1), tracker.MaxOverTime(absl::Nanoseconds(1)));
  EXPECT_EQ(NHugePages(1), tracker.MinOverTime(absl::Nanoseconds(1)));
  EXPECT_EQ(NHugePages(1), tracker.MaxOverTime(kDuration));
  EXPECT_EQ(NHugePages(1), tracker.MinOverTime(kDuration));
}

INSTANTIATE_TEST_SUITE_P(
    All, HugeCacheTest,
    testing::Combine(testing::Values(absl::Seconds(1), absl::Seconds(30)),
                     testing::Bool()),
    [](const testing::TestParamInfo<HugeCacheTest::ParamType> info) {
      return "Cachetime_" + absl::FormatDuration(std::get<0>(info.param)) +
             "DemandBasedRelease_" +
             (std::get<1>(info.param) ? "Enabled" : "Disabled");
    });

}  // namespace
}  // namespace tcmalloc_internal
}  // namespace tcmalloc
