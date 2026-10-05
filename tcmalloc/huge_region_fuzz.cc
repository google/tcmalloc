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
#include "absl/container/flat_hash_set.h"
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
    release_callback_(r);

    if (!unback_success_) {
      return {.success = false, .error_number = 0};
    }

    PageId end = r.p + r.n;
    for (; r.p != end; ++r.p) {
      released_.insert(r.p);
    }

    return {.success = true, .error_number = 0};
  }

  absl::flat_hash_set<PageId> released_;
  bool unback_success_ = true;
  // Runs before each unback with the range being unbacked.
  std::function<void(Range)> release_callback_;
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

// Shared by FuzzRegion and FuzzRegionSet, whose states both own a MockUnback.
struct SetUnbackSuccess {
  bool success;

  template <typename Sink>
  friend void AbslStringify(Sink& sink, const SetUnbackSuccess& s) {
    absl::Format(&sink, "SetUnbackSuccess{.success=%v}", s.success);
  }

  template <typename State>
  void Perform(State& state) const {
    state.unback.unback_success_ = success;
  }
};

// Stringifies either fuzzer's instruction variant.
template <typename Sink, typename... Ts>
void AbslStringify(Sink& sink, const std::variant<Ts...>& i) {
  std::visit([&](const auto& arg) { absl::Format(&sink, "%v", arg); }, i);
}

// Checks AddSpanStats output against the free and unmapped pages it covers.
void ExpectSpanStats(const SmallSpanStats& small, const LargeSpanStats& large,
                     Length free, Length unmapped) {
  Length small_normal_pages;
  Length small_returned_pages;
  for (size_t i = 0; i < kMaxPages.raw_num(); ++i) {
    small_normal_pages += Length(i * small.normal_length[i]);
    small_returned_pages += Length(i * small.returned_length[i]);
  }
  EXPECT_EQ(small_normal_pages + large.normal_pages, free);
  EXPECT_EQ(small_returned_pages + large.returned_pages, unmapped);
}

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

  std::vector<Range> allocs;
  std::vector<absl::Span<const Instruction>> reentrant_stack;
  // Ranges being unbacked, innermost last.
  std::vector<Range> in_flight;
  std::string output;
  int depth = 0;

  explicit State(bool reentrant_release)
      : reentrant_release(reentrant_release),
        start(HugePageContaining(MakeTaggedAddress(MemoryTag::kNormal))),
        region({start, HugeRegion::size()}, unback, nil_set_anon_vma_name) {
    unback.released_.reserve(HugeRegion::size().in_pages().raw_num());
    for (PageId p = start.first_page(), end = p + HugeRegion::size().in_pages();
         p != end; ++p) {
      unback.released_.insert(p);
    }
    output.resize(1 << 20);

    unback.release_callback_ = [this](Range r) {
      // HugeRegion::Release will drop pageheap_lock around the unback
      // (b/73749855), so other threads may observe the region here.
      in_flight.push_back(r);
      CheckInvariants();
      RunReentrant();
      // Whatever ran meanwhile left the range alone.
      CheckInvariants();
      in_flight.pop_back();
    };
  }

  ~State() {
    reentrant_stack.clear();
    for (const auto& alloc : allocs) {
      region.Put(alloc, false);
    }
    allocs.clear();
    EXPECT_EQ(region.used_pages(), Length(0));
    CheckInvariants();
  }

  void RunReentrant() {
    if (!reentrant_release) return;
    if (reentrant_stack.empty()) return;
    if (depth >= 5) return;

    auto prog = std::move(reentrant_stack.back());
    reentrant_stack.pop_back();

    depth++;
    Execute(prog);
    depth--;
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
    // Free-and-backed hugepages are entirely free.  A range being unbacked is
    // neither.
    ASSERT_LE(region.free_backed().in_pages(), region.free_pages());
    // The per-hugepage backed bits agree with the backed count.
    EXPECT_EQ(region.backed().in_pages() + region.unmapped_pages(),
              HugeRegion::size().in_pages());
    BackingStats stats = region.stats();
    EXPECT_EQ(stats.system_bytes, HugeRegion::size().in_bytes());
    EXPECT_EQ(stats.free_bytes, region.free_pages().in_bytes());
    EXPECT_EQ(stats.unmapped_bytes, region.unmapped_pages().in_bytes());
    EXPECT_EQ(
        region.used_pages() + region.free_pages() + region.unmapped_pages(),
        HugeRegion::size().in_pages());
    for (const Range& r : in_flight) {
      CheckInFlight(r);
    }
  }

  // r is being unbacked.  It must consist of whole hugepages that the region
  // reports as allocated, so that no one can allocate it, and no live
  // allocation may overlap it.
  void CheckInFlight(Range r) {
    ASSERT_EQ(r.p, HugePageContaining(r.p).first_page());
    ASSERT_EQ(r.n.raw_num() % kPagesPerHugePage.raw_num(), 0);
    for (Length offset; offset < r.n; offset += kPagesPerHugePage) {
      PageBitmap pages;
      ASSERT_TRUE(region.GetPageAllocationStatus(
          HugePageContaining(r.p + offset), pages));
      ASSERT_EQ(pages.CountBits(0, kPagesPerHugePage.raw_num()),
                kPagesPerHugePage.raw_num());
    }
    for (const Range& a : allocs) {
      ASSERT_TRUE(a.p + a.n <= r.p || r.p + r.n <= a.p)
          << "allocation " << a.p.index() << "+" << a.n.raw_num()
          << " overlaps unback " << r.p.index() << "+" << r.n.raw_num();
    }
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
  state.allocs.emplace_back(p, n);
  if (!from_released) {
    return;
  }
  bool did_release = false;
  for (PageId q = p, end = p + n; q != end; ++q) {
    auto it = state.unback.released_.find(q);
    if (it != state.unback.released_.end()) {
      state.unback.released_.erase(it);
      did_release = true;
    }
  }
  CHECK(did_release);
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
  state.region.Put(alloc, release);
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
  ExpectSpanStats(small, large, state.region.free_pages(),
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

using Instruction = std::variant<Contribute, Get, Put, ReleasePages, SpanStats,
                                 Print, PrintInPbtxt, SetUnbackSuccess>;

struct State {
  const HugePage start;
  MockUnback unback;
  NilMemoryTagFunction nil_set_anon_vma_name;
  HugeRegionSet<HugeRegion> set;

  std::vector<std::unique_ptr<HugeRegion>> regions;
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
    unback.release_callback_ = [](Range) {};
  }

  ~State() {
    for (const Range& alloc : allocs) {
      EXPECT_TRUE(set.MaybePut(alloc));
    }
    allocs.clear();
    for (const auto& region : regions) {
      EXPECT_EQ(region->used_pages(), Length(0));
    }
    CheckInvariants();
  }

  // Returns the index of the region containing p, or regions.size() if none.
  [[nodiscard]] size_t RegionIndex(PageId p) const {
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

    Length used, free, unmapped;
    HugeLength free_backed = NHugePages(0);
    for (const auto& region : regions) {
      used += region->used_pages();
      free += region->free_pages();
      unmapped += region->unmapped_pages();
      free_backed += region->free_backed();
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
      EXPECT_LT(RegionIndex(alloc.p), regions.size());
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
  state.set.Contribute(state.regions.back().get());
  EXPECT_EQ(state.set.ActiveRegions(), state.regions.size());
}

void Get::Perform(State& state) const {
  const Length n = Length(std::max<size_t>(length % (1 << 18), 1));
  std::vector<Length> longest_free;
  longest_free.reserve(state.regions.size());
  for (const auto& region : state.regions) {
    longest_free.push_back(region->longest_free());
  }

  PageId p;
  bool from_released;
  if (!state.set.MaybeGet(n, &p, &from_released)) {
    for (Length longest : longest_free) {
      EXPECT_LT(longest, n);
    }
    return;
  }

  const size_t i = state.RegionIndex(p);
  ASSERT_LT(i, state.regions.size());
  EXPECT_TRUE(state.regions[i]->contains(p + n - Length(1)));
  // The set allocates from the most fragmented region that fits: the one with
  // the shortest longest-free range that is still at least n.
  EXPECT_GE(longest_free[i], n);
  for (Length longest : longest_free) {
    if (longest < n) continue;
    EXPECT_GE(longest, longest_free[i]);
  }

  state.allocs.push_back(Range(p, n));
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

  EXPECT_TRUE(state.set.MaybePut(alloc));
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

  Length free, unmapped;
  for (const auto& region : state.regions) {
    free += region->free_pages();
    unmapped += region->unmapped_pages();
  }
  ExpectSpanStats(small, large, free, unmapped);
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
