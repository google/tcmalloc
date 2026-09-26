// Copyright 2022 The TCMalloc Authors
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

#include <algorithm>
#include <bitset>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "gtest/gtest.h"
#include "fuzztest/fuzztest.h"
#include "absl/base/attributes.h"
#include "absl/log/check.h"
#include "absl/strings/str_format.h"
#include "absl/strings/str_join.h"
#include "absl/strings/string_view.h"
#include "absl/types/span.h"
#include "tcmalloc/common.h"
#include "tcmalloc/huge_cache.h"
#include "tcmalloc/huge_pages.h"
#include "tcmalloc/huge_region.h"
#include "tcmalloc/internal/logging.h"
#include "tcmalloc/internal/memory_tag.h"
#include "tcmalloc/internal/range_tracker.h"
#include "tcmalloc/internal/system_allocator.h"
#include "tcmalloc/pages.h"
#include "tcmalloc/stats.h"

namespace tcmalloc::tcmalloc_internal {
namespace {

void* MakeTaggedAddress(MemoryTag tag) {
  return reinterpret_cast<void*>(uintptr_t{static_cast<uint8_t>(tag)}
                                 << kTagShift);
}

class NilMemoryTagFunction final : public MemoryTagFunction {
 public:
  void operator()(Range r, std::optional<absl::string_view> name) override {}
};

class MockUnback final : public MemoryModifyFunction {
 public:
  [[nodiscard]] MemoryModifyStatus operator()(Range r) override {
    // While release_callback_ runs, r is backed but every page of it is
    // marked in the region's tracker.
    in_flight_.push_back(r);
    if (release_callback_) {
      release_callback_();
    }
    in_flight_.pop_back();

    if (!unback_success_) {
      return {.success = false, .error_number = 0};
    }

    if (unbacked_callback_) {
      unbacked_callback_(r);
    }
    return {.success = true, .error_number = 0};
  }

  // Is hp currently in the middle of being unbacked?
  bool InFlight(HugePage hp) const {
    const PageId p = hp.first_page();
    for (const Range& r : in_flight_) {
      if (r.p <= p && p < r.p + r.n) {
        return true;
      }
    }
    return false;
  }

  bool unback_success_ = true;
  // Invoked before the unback happens.
  std::function<void()> release_callback_;
  // Invoked with the range after it has been successfully unbacked.
  std::function<void(Range)> unbacked_callback_;

 private:
  std::vector<Range> in_flight_;
};

// Per-hugepage state of a single region, tracked by the harness independently
// of the region's own bookkeeping.
class RegionOracle {
 public:
  explicit RegionOracle(HugePage start)
      : start_(start), live_(HugeRegion::kNumHugePages) {
    unbacked_.set();
  }

  HugePage start() const { return start_; }

  bool contains(PageId p) const {
    return start_.first_page() <= p &&
           p < (start_ + HugeRegion::size()).first_page();
  }

  bool unbacked(size_t i) const { return unbacked_[i]; }
  size_t unbacked_count() const { return unbacked_.count(); }
  bool fully_free(size_t i) const { return live_[i].IsZero(); }

  // True iff any hugepage spanned by r is unbacked, i.e. what MaybeGet must
  // report in *from_released for r.
  bool AnyUnbacked(Range r) const {
    bool any = false;
    ForEachHugePage(r, [&](size_t i, size_t, size_t) { any |= unbacked_[i]; });
    return any;
  }

  void OnAllocate(Range r) {
    ForEachHugePage(r, [&](size_t i, size_t offset, size_t len) {
      unbacked_.reset(i);
      live_[i].SetRange(offset, len);
    });
  }

  void OnFree(Range r) {
    ForEachHugePage(r, [&](size_t i, size_t offset, size_t len) {
      live_[i].ClearRange(offset, len);
    });
  }

  void OnUnback(Range r) {
    ForEachHugePage(r, [&](size_t i, size_t offset, size_t len) {
      EXPECT_EQ(offset, size_t{0});
      EXPECT_EQ(len, kPagesPerHugePage.raw_num());
      EXPECT_TRUE(live_[i].IsZero());
      EXPECT_FALSE(unbacked_[i]);
      unbacked_.set(i);
    });
  }

  // Invokes f(hugepage index) for every hugepage spanned by r.
  template <typename F>
  void ForEachHugePage(Range r, F f) const {
    PageId p = r.p;
    Length n = r.n;
    while (n > Length(0)) {
      const HugePage hp = HugePageContaining(p);
      const Length offset = p - hp.first_page();
      const Length here = std::min(n, kPagesPerHugePage - offset);
      f((hp - start_) / NHugePages(1), offset.raw_num(), here.raw_num());
      p += here;
      n -= here;
    }
  }

  // Verifies query(hp, pages) for every hugepage of the region: it must
  // succeed and return exactly the live pages, none for an unbacked hugepage,
  // and all of them while hp is in the middle of being unbacked.
  template <typename Query>
  void CheckPages(Query query, const MockUnback& unback) const {
    PageBitmap actual;
    for (size_t i = 0; i < HugeRegion::kNumHugePages; ++i) {
      const HugePage hp = start_ + NHugePages(i);
      ASSERT_TRUE(query(hp, actual)) << i;
      PageBitmap expected;
      if (unbacked_[i]) {
        ASSERT_TRUE(live_[i].IsZero()) << i;
      } else if (unback.InFlight(hp)) {
        expected.SetRange(0, kPagesPerHugePage.raw_num());
      } else {
        expected = live_[i];
      }
      ASSERT_TRUE((actual ^ expected).IsZero()) << i;
    }
  }

  // Verifies that query rejects the hugepages adjacent to the region.
  template <typename Query>
  void CheckNeighbors(Query query) const {
    PageBitmap pages;
    EXPECT_FALSE(query(start_ - NHugePages(1), pages));
    EXPECT_FALSE(query(start_ + HugeRegion::size(), pages));
  }

 private:
  HugePage start_;
  std::bitset<HugeRegion::kNumHugePages> unbacked_;
  std::vector<PageBitmap> live_;
};

struct State;

struct Allocate {
  uint32_t length;

  template <typename Sink>
  friend void AbslStringify(Sink& sink, const Allocate& a) {
    absl::Format(&sink, "Allocate{.length=%d}", a.length);
  }

  void Perform(State& state) const;
};

struct Deallocate {
  uint32_t index;
  bool release;

  template <typename Sink>
  friend void AbslStringify(Sink& sink, const Deallocate& d) {
    absl::Format(&sink, "Deallocate{.index=%d, .release=%v}", d.index,
                 d.release);
  }

  void Perform(State& state) const;
};

struct Release {
  uint32_t length;
  bool adaptive_release;

  template <typename Sink>
  friend void AbslStringify(Sink& sink, const Release& r) {
    absl::Format(&sink, "Release{.length=%d, .adaptive_release=%v}", r.length,
                 r.adaptive_release);
  }

  void Perform(State& state) const;
};

struct Stats {
  template <typename Sink>
  friend void AbslStringify(Sink& sink, const Stats&) {
    sink.Append("Stats{}");
  }

  void Perform(State& state) const;
};

struct Toggle {
  template <typename Sink>
  friend void AbslStringify(Sink& sink, const Toggle&) {
    sink.Append("Toggle{}");
  }

  void Perform(State& state) const;
};

struct SetUnbackSuccess {
  bool success;

  template <typename Sink>
  friend void AbslStringify(Sink& sink, const SetUnbackSuccess& s) {
    absl::Format(&sink, "SetUnbackSuccess{.success=%v}", s.success);
  }

  void Perform(State& state) const;
};

struct Reentrant;

struct GatherStatsPbtxt {
  template <typename Sink>
  friend void AbslStringify(Sink& sink, const GatherStatsPbtxt&) {
    sink.Append("GatherStatsPbtxt{}");
  }

  void Perform(State& state) const;
};

struct PrintStats {
  template <typename Sink>
  friend void AbslStringify(Sink& sink, const PrintStats&) {
    sink.Append("PrintStats{}");
  }

  void Perform(State& state) const;
};

using Instruction =
    std::variant<Allocate, Deallocate, Release, Stats, Toggle, SetUnbackSuccess,
                 Reentrant, GatherStatsPbtxt, PrintStats>;

struct Reentrant {
  std::vector<Instruction> subprogram;

  void Perform(State& state) const;
};

template <typename Sink>
void AbslStringify(Sink& sink, const Instruction& i) {
  std::visit([&](const auto& arg) { absl::Format(&sink, "%v", arg); }, i);
}

template <typename Sink>
void AbslStringify(Sink& sink, const Reentrant& r) {
  absl::Format(&sink, "Reentrant{.subprogram={%s}}",
               absl::StrJoin(r.subprogram, ", ",
                             [](std::string* out, const Instruction& i) {
                               absl::StrAppend(out, i);
                             }));
}

struct State {
  bool reentrant_release;
  const HugePage start;
  MockUnback unback;
  NilMemoryTagFunction nil_set_anon_vma_name;
  HugeRegion region;
  RegionOracle oracle;

  std::vector<Range> allocs;
  std::vector<absl::Span<const Instruction>> reentrant_stack;
  std::string output;
  int depth = 0;

  explicit State(bool reentrant_release)
      : reentrant_release(reentrant_release),
        start(HugePageContaining(MakeTaggedAddress(MemoryTag::kNormal))),
        region({start, HugeRegion::size()}, unback, nil_set_anon_vma_name),
        oracle(start) {
    output.resize(1 << 20);

    unback.unbacked_callback_ = [this](Range r) { oracle.OnUnback(r); };
    unback.release_callback_ = [this]() {
      if (!this->reentrant_release) return;
      if (reentrant_stack.empty()) return;
      if (depth >= 5) return;

      auto prog = std::move(reentrant_stack.back());
      reentrant_stack.pop_back();

      depth++;
      Execute(prog);
      depth--;
    };
  }

  ~State() {
    reentrant_stack.clear();
    for (const auto& alloc : allocs) {
      oracle.OnFree(alloc);
      region.Put(alloc, false);
    }
    allocs.clear();
    EXPECT_EQ(region.used_pages(), Length(0));
    CheckInvariants();
  }

  void Execute(absl::Span<const Instruction> instructions) {
    for (const auto& inst : instructions) {
      std::visit([&](const auto& arg) { arg.Perform(*this); }, inst);
      CheckInvariants();
    }
  }

  void CheckInvariants() {
    SmallSpanStats small;
    LargeSpanStats large;
    region.AddSpanStats(&small, &large);
    ASSERT_LE(region.free_backed(), region.backed());
    ASSERT_LE(region.backed(), region.size());
    BackingStats stats = region.stats();
    EXPECT_EQ(stats.system_bytes, HugeRegion::size().in_bytes());
    EXPECT_EQ(stats.free_bytes, region.free_pages().in_bytes());
    EXPECT_EQ(stats.unmapped_bytes, region.unmapped_pages().in_bytes());
    EXPECT_EQ(
        region.used_pages() + region.free_pages() + region.unmapped_pages(),
        HugeRegion::size().in_pages());
    EXPECT_EQ(NHugePages(oracle.unbacked_count()),
              HugeRegion::size() - region.backed());

    const auto query = [&](HugePage hp, PageBitmap& pages) {
      return region.GetPageAllocationStatus(hp, pages);
    };
    oracle.CheckPages(query, unback);
    oracle.CheckNeighbors(query);
  }
};

void Allocate::Perform(State& state) const {
  const Length n = Length(std::max<size_t>(length % (1 << 18), 1));
  PageId p;
  bool from_released;
  if (!state.region.MaybeGet(n, &p, &from_released)) {
    return;
  }
  EXPECT_TRUE(state.region.contains(p));
  EXPECT_TRUE(state.region.contains(p + n - Length(1)));
  const Range r(p, n);
  EXPECT_EQ(from_released, state.oracle.AnyUnbacked(r));
  state.oracle.OnAllocate(r);
  state.allocs.push_back(r);
}

void Deallocate::Perform(State& state) const {
  if (state.allocs.empty()) {
    return;
  }
  const int target_index = index % state.allocs.size();
  const Range alloc = state.allocs[target_index];
  using std::swap;
  swap(state.allocs[target_index], state.allocs.back());
  state.allocs.pop_back();
  state.oracle.OnFree(alloc);
  state.region.Put(alloc, release);
  if (!release || !state.unback.unback_success_ || state.reentrant_release) {
    return;
  }
  // Every hugepage emptied by this Put has been unbacked.
  state.oracle.ForEachHugePage(alloc, [&](size_t i, size_t, size_t) {
    if (state.oracle.fully_free(i)) {
      EXPECT_TRUE(state.oracle.unbacked(i)) << i;
    }
  });
}

void Release::Perform(State& state) const {
  const Length len = Length(length % (1 << 18));
  const HugeLength max_expected =
      std::min(state.region.free_backed(), HLFromPages(len));
  const HugeLength actual = state.region.Release(len, adaptive_release);
  if (!state.unback.unback_success_) {
    TC_CHECK_EQ(actual, NHugePages(0));
    return;
  }

  if (max_expected > NHugePages(0) && len > Length(0)) {
    TC_CHECK_GT(actual, NHugePages(0));
  }
  TC_CHECK_LE(actual, max_expected);
}

void Stats::Perform(State& state) const {
  SmallSpanStats small;
  LargeSpanStats large;
  state.region.AddSpanStats(&small, &large);

  Length small_normal_pages;
  Length small_returned_pages;
  for (size_t i = 0; i < kMaxPages.raw_num(); ++i) {
    small_normal_pages += Length(i * small.normal_length[i]);
    small_returned_pages += Length(i * small.returned_length[i]);
  }

  EXPECT_EQ(small_normal_pages + large.normal_pages, state.region.free_pages());
  EXPECT_EQ(small_returned_pages + large.returned_pages,
            state.region.unmapped_pages());

  BackingStats stats = state.region.stats();
  EXPECT_EQ(stats.system_bytes, HugeRegion::size().in_bytes());
  EXPECT_EQ(stats.free_bytes, state.region.free_pages().in_bytes());
  EXPECT_EQ(stats.unmapped_bytes, state.region.unmapped_pages().in_bytes());
  EXPECT_EQ(state.region.used_pages() + state.region.free_pages() +
                state.region.unmapped_pages(),
            HugeRegion::size().in_pages());
  EXPECT_LE(state.region.free_backed(), state.region.backed());
  EXPECT_LE(state.region.backed(), state.region.size());
}

void Toggle::Perform(State& state) const {
  state.unback.unback_success_ = !state.unback.unback_success_;
}

void SetUnbackSuccess::Perform(State& state) const {
  state.unback.unback_success_ = success;
}

void Reentrant::Perform(State& state) const {
  state.reentrant_stack.push_back(subprogram);
}

void GatherStatsPbtxt::Perform(State& state) const {
  Printer p(&state.output[0], state.output.size());
  {
    PbtxtRegion r(p, kTop);
    state.region.PrintInPbtxt(r);
  }
  CHECK_LE(p.SpaceRequired(), state.output.size());
}

void PrintStats::Perform(State& state) const {
  Printer p(&state.output[0], state.output.size());
  state.region.Print(p);
  ASSERT_LE(p.SpaceRequired(), state.output.size());
}

void FuzzRegion(const std::vector<Instruction>& instructions,
                bool reentrant_release) {
  State state(reentrant_release);
  state.Execute(instructions);
}

fuzztest::Domain<Instruction> GetInstructionDomain(int depth);

auto GetFlatInstructionDomain() {
  return fuzztest::OneOf(
      fuzztest::Map([](Allocate a) -> Instruction { return Instruction{a}; },
                    fuzztest::Arbitrary<Allocate>()),
      fuzztest::Map([](Deallocate d) -> Instruction { return Instruction{d}; },
                    fuzztest::Arbitrary<Deallocate>()),
      fuzztest::Map([](Release r) -> Instruction { return Instruction{r}; },
                    fuzztest::Arbitrary<Release>()),
      fuzztest::Map([](Stats s) -> Instruction { return Instruction{s}; },
                    fuzztest::Arbitrary<Stats>()),
      fuzztest::Map([](Toggle t) -> Instruction { return Instruction{t}; },
                    fuzztest::Arbitrary<Toggle>()),
      fuzztest::Map(
          [](SetUnbackSuccess s) -> Instruction { return Instruction{s}; },
          fuzztest::Arbitrary<SetUnbackSuccess>()),
      fuzztest::Map(
          [](GatherStatsPbtxt g) -> Instruction { return Instruction{g}; },
          fuzztest::Arbitrary<GatherStatsPbtxt>()),
      fuzztest::Map([](PrintStats p) -> Instruction { return Instruction{p}; },
                    fuzztest::Arbitrary<PrintStats>()));
}

fuzztest::Domain<Instruction> GetInstructionDomain(int depth) {
  if (depth <= 0) {
    return fuzztest::OneOf(
        GetFlatInstructionDomain(),
        fuzztest::Map(
            [](std::vector<Instruction> sub) -> Instruction {
              return Instruction{Reentrant{sub}};
            },
            fuzztest::VectorOf(fuzztest::Just(Instruction{Allocate{1}}))
                .WithSize(0)));
  } else {
    return fuzztest::OneOf(
        GetFlatInstructionDomain(),
        fuzztest::Map(
            [](std::vector<Instruction> sub) -> Instruction {
              return Instruction{Reentrant{sub}};
            },
            fuzztest::VectorOf(GetInstructionDomain(depth - 1))));
  }
}

FUZZ_TEST(HugeRegionTest, FuzzRegion)
    .WithDomains(fuzztest::VectorOf(GetInstructionDomain(5)),
                 fuzztest::Arbitrary<bool>());

TEST(HugeRegionTest, b339521569) {
  std::vector<Instruction> p = {
      Allocate{0},
  };

  FuzzRegion(p, false);
}

// Drives a HugeRegionSet<HugeRegion> over up to kMaxRegions regions that are
// contributed lazily.
namespace region_set {

constexpr size_t kMaxRegions = 4;

struct State;

struct Contribute {
  template <typename Sink>
  friend void AbslStringify(Sink& sink, const Contribute&) {
    sink.Append("Contribute{}");
  }

  void Perform(State& state) const;
};

struct Get {
  uint32_t length;

  template <typename Sink>
  friend void AbslStringify(Sink& sink, const Get& g) {
    absl::Format(&sink, "Get{.length=%d}", g.length);
  }

  void Perform(State& state) const;
};

struct Put {
  uint32_t index;

  template <typename Sink>
  friend void AbslStringify(Sink& sink, const Put& p) {
    absl::Format(&sink, "Put{.index=%d}", p.index);
  }

  void Perform(State& state) const;
};

struct ReleasePages {
  uint32_t length;
  bool use_adaptive;
  bool hit_limit;

  template <typename Sink>
  friend void AbslStringify(Sink& sink, const ReleasePages& r) {
    absl::Format(&sink,
                 "ReleasePages{.length=%d, .use_adaptive=%v, .hit_limit=%v}",
                 r.length, r.use_adaptive, r.hit_limit);
  }

  void Perform(State& state) const;
};

struct SpanStats {
  template <typename Sink>
  friend void AbslStringify(Sink& sink, const SpanStats&) {
    sink.Append("SpanStats{}");
  }

  void Perform(State& state) const;
};

struct Print {
  template <typename Sink>
  friend void AbslStringify(Sink& sink, const Print&) {
    sink.Append("Print{}");
  }

  void Perform(State& state) const;
};

struct PrintInPbtxt {
  template <typename Sink>
  friend void AbslStringify(Sink& sink, const PrintInPbtxt&) {
    sink.Append("PrintInPbtxt{}");
  }

  void Perform(State& state) const;
};

struct SetUnbackSuccess {
  bool success;

  template <typename Sink>
  friend void AbslStringify(Sink& sink, const SetUnbackSuccess& s) {
    absl::Format(&sink, "SetUnbackSuccess{.success=%v}", s.success);
  }

  void Perform(State& state) const;
};

using Instruction = std::variant<Contribute, Get, Put, ReleasePages, SpanStats,
                                 Print, PrintInPbtxt, SetUnbackSuccess>;

template <typename Sink>
void AbslStringify(Sink& sink, const Instruction& i) {
  std::visit([&](const auto& arg) { absl::Format(&sink, "%v", arg); }, i);
}

struct State {
  const HugePage start;
  MockUnback unback;
  NilMemoryTagFunction nil_set_anon_vma_name;
  HugeRegionSet<HugeRegion> set;

  std::vector<std::unique_ptr<HugeRegion>> regions;
  std::vector<RegionOracle> oracles;
  std::vector<Range> allocs;
  // The set's low-water mark of free-but-backed hugepages, which bounds an
  // adaptive release: the minimum observed after each successful MaybeGet,
  // reset by ReleasePages.
  HugeLength lowater = NHugePages(0);
  std::string output;

  explicit State(bool use_huge_region_more_often)
      : start(HugePageContaining(MakeTaggedAddress(MemoryTag::kNormal))),
        set(use_huge_region_more_often
                ? HugeRegionUsageOption::kUseForAllLargeAllocs
                : HugeRegionUsageOption::kDefault) {
    output.resize(1 << 20);
    unback.unbacked_callback_ = [this](Range r) {
      const size_t i = RegionIndex(r.p);
      ASSERT_LT(i, oracles.size());
      oracles[i].OnUnback(r);
    };
  }

  ~State() {
    for (const Range& alloc : allocs) {
      const size_t i = RegionIndex(alloc.p);
      if (i == oracles.size()) {
        ADD_FAILURE() << "allocation outside every region";
        continue;
      }
      oracles[i].OnFree(alloc);
      EXPECT_TRUE(set.MaybePut(alloc));
    }
    allocs.clear();
    for (const auto& region : regions) {
      EXPECT_EQ(region->used_pages(), Length(0));
    }
    CheckInvariants();
  }

  // Returns the index of the region containing p, or regions.size() if none.
  size_t RegionIndex(PageId p) const {
    size_t found = regions.size();
    for (size_t i = 0; i < regions.size(); ++i) {
      if (!regions[i]->contains(p)) continue;
      EXPECT_EQ(found, regions.size());
      found = i;
    }
    return found;
  }

  void Execute(absl::Span<const Instruction> instructions) {
    for (const auto& inst : instructions) {
      std::visit([&](const auto& arg) { arg.Perform(*this); }, inst);
      CheckInvariants();
    }
  }

  void CheckInvariants() {
    EXPECT_EQ(set.ActiveRegions(), regions.size());

    const auto region_query = [&](HugePage hp, PageBitmap& pages) {
      return set.GetPageAllocationStatus(hp, pages);
    };
    Length used, free, unmapped;
    HugeLength free_backed = NHugePages(0);
    for (size_t i = 0; i < regions.size(); ++i) {
      const HugeRegion& region = *regions[i];
      used += region.used_pages();
      free += region.free_pages();
      unmapped += region.unmapped_pages();
      free_backed += region.free_backed();
      EXPECT_EQ(NHugePages(oracles[i].unbacked_count()),
                HugeRegion::size() - region.backed());
      oracles[i].CheckPages(region_query, unback);
      oracles[i].CheckNeighbors([&](HugePage hp, PageBitmap& pages) {
        return region.GetPageAllocationStatus(hp, pages);
      });
    }

    const BackingStats stats = set.stats();
    EXPECT_EQ(stats.system_bytes,
              (HugeRegion::size() * regions.size()).in_bytes());
    EXPECT_EQ(stats.free_bytes, free.in_bytes());
    EXPECT_EQ(stats.unmapped_bytes, unmapped.in_bytes());
    EXPECT_EQ(set.free_backed(), free_backed);

    Length live;
    for (const Range& alloc : allocs) {
      live += alloc.n;
    }
    EXPECT_EQ(used, live);

    // Neither the hugepage before the first region nor the first hugepage
    // past the contributed regions belongs to the set.
    PageBitmap pages;
    EXPECT_FALSE(set.GetPageAllocationStatus(start - NHugePages(1), pages));
    EXPECT_FALSE(set.GetPageAllocationStatus(
        start + HugeRegion::size() * regions.size(), pages));
  }
};

void Contribute::Perform(State& state) const {
  if (state.regions.size() == kMaxRegions) {
    return;
  }
  const HugePage start =
      state.start + HugeRegion::size() * state.regions.size();
  state.regions.push_back(
      std::make_unique<HugeRegion>(HugeRange(start, HugeRegion::size()),
                                   state.unback, state.nil_set_anon_vma_name));
  state.oracles.emplace_back(start);
  state.set.Contribute(state.regions.back().get());
  EXPECT_EQ(state.set.ActiveRegions(), state.regions.size());
}

void Get::Perform(State& state) const {
  const Length n = Length(std::max<size_t>(length % (1 << 18), 1));

  PageId p;
  bool from_released;
  if (!state.set.MaybeGet(n, &p, &from_released)) {
    return;
  }

  const size_t i = state.RegionIndex(p);
  ASSERT_LT(i, state.regions.size());
  EXPECT_TRUE(state.regions[i]->contains(p + n - Length(1)));

  const Range r(p, n);
  EXPECT_EQ(from_released, state.oracles[i].AnyUnbacked(r));
  state.oracles[i].OnAllocate(r);
  state.allocs.push_back(r);
  state.lowater = std::min(state.lowater, state.set.free_backed());
}

void Put::Perform(State& state) const {
  if (state.allocs.empty()) {
    // A range outside every region is not accepted.
    const PageId outside = (state.start - NHugePages(1)).first_page();
    EXPECT_FALSE(state.set.MaybePut(Range(outside, Length(1))));
    return;
  }
  const size_t target_index = index % state.allocs.size();
  const Range alloc = state.allocs[target_index];
  using std::swap;
  swap(state.allocs[target_index], state.allocs.back());
  state.allocs.pop_back();

  const size_t i = state.RegionIndex(alloc.p);
  ASSERT_LT(i, state.regions.size());
  RegionOracle& oracle = state.oracles[i];
  oracle.OnFree(alloc);
  EXPECT_TRUE(state.set.MaybePut(alloc));
  if (state.set.UseHugeRegionMoreOften() || !state.unback.unback_success_) {
    return;
  }
  // Without huge-region-more-often, MaybePut unbacks the hugepages it empties.
  oracle.ForEachHugePage(alloc, [&](size_t hp, size_t, size_t) {
    if (oracle.fully_free(hp)) {
      EXPECT_TRUE(oracle.unbacked(hp)) << hp;
    }
  });
}

void ReleasePages::Perform(State& state) const {
  const Length desired = Length(length % (1 << 20));
  const HugeLength before = state.set.free_backed();
  Length to_release;
  if (hit_limit) {
    to_release = desired;
  } else if (use_adaptive) {
    to_release = state.lowater.in_pages();
  } else {
    // HugeRegionSet::kFractionToReleaseFromRegion.
    to_release = Length(static_cast<size_t>(before.in_pages().raw_num() * 0.1));
  }
  // The set releases whole hugepages until it has covered its target or run
  // out of free-but-backed hugepages.
  const Length cap = std::min(to_release, before.in_pages());

  const Length released =
      state.set.ReleasePages(desired, use_adaptive, hit_limit);
  if (!state.unback.unback_success_) {
    EXPECT_EQ(released, Length(0));
  } else {
    EXPECT_GE(released, cap);
    EXPECT_LE(released, HLFromPages(cap).in_pages());
  }
  EXPECT_EQ(released % kPagesPerHugePage, Length(0));
  EXPECT_EQ(state.set.free_backed(), before - HLFromPages(released));
  state.lowater = state.set.free_backed();
}

void SpanStats::Perform(State& state) const {
  SmallSpanStats small;
  LargeSpanStats large;
  state.set.AddSpanStats(&small, &large);

  Length small_normal_pages;
  Length small_returned_pages;
  for (size_t i = 0; i < kMaxPages.raw_num(); ++i) {
    small_normal_pages += Length(i * small.normal_length[i]);
    small_returned_pages += Length(i * small.returned_length[i]);
  }

  Length free, unmapped;
  for (const auto& region : state.regions) {
    free += region->free_pages();
    unmapped += region->unmapped_pages();
  }
  EXPECT_EQ(small_normal_pages + large.normal_pages, free);
  EXPECT_EQ(small_returned_pages + large.returned_pages, unmapped);
}

void Print::Perform(State& state) const {
  Printer p(&state.output[0], state.output.size());
  state.set.Print(p);
  ASSERT_LE(p.SpaceRequired(), state.output.size());
}

void PrintInPbtxt::Perform(State& state) const {
  Printer p(&state.output[0], state.output.size());
  {
    PbtxtRegion r(p, kTop);
    state.set.PrintInPbtxt(r);
  }
  CHECK_LE(p.SpaceRequired(), state.output.size());
}

void SetUnbackSuccess::Perform(State& state) const {
  state.unback.unback_success_ = success;
}

void FuzzRegionSet(const std::vector<Instruction>& instructions,
                   bool use_huge_region_more_often) {
  State state(use_huge_region_more_often);
  state.Execute(instructions);
}

FUZZ_TEST(HugeRegionTest, FuzzRegionSet)
    .WithDomains(fuzztest::VectorOf(fuzztest::Arbitrary<Instruction>()),
                 fuzztest::Arbitrary<bool>());

TEST(HugeRegionTest, RegionSetSmoke) {
  for (bool use_huge_region_more_often : {false, true}) {
    FuzzRegionSet(
        {
            Get{.length = 1},
            Put{.index = 0},
            Contribute{},
            Get{.length = 1},
            Contribute{},
            // Only the empty second region fits a whole region.
            Get{.length = 1 << 17},
            Get{.length = 513},
            SpanStats{},
            Print{},
            PrintInPbtxt{},
            Put{.index = 0},
            ReleasePages{
                .length = 1024, .use_adaptive = false, .hit_limit = true},
            SetUnbackSuccess{.success = false},
            Put{.index = 0},
            ReleasePages{
                .length = 100, .use_adaptive = true, .hit_limit = false},
            SetUnbackSuccess{.success = true},
            Put{.index = 0},
            ReleasePages{
                .length = 1 << 20, .use_adaptive = false, .hit_limit = false},
            ReleasePages{
                .length = 1 << 20, .use_adaptive = true, .hit_limit = false},
            PrintInPbtxt{},
        },
        use_huge_region_more_often);
  }
}

}  // namespace region_set
}  // namespace
}  // namespace tcmalloc::tcmalloc_internal
