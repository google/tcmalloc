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
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
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
    begin_unback_(r);
    release_callback_();
    const bool success = unback_success_;
    end_unback_(r, success);
    return {.success = success, .error_number = 0};
  }

  bool unback_success_ = true;
  std::function<void()> release_callback_;
  // Bracket each unback so the model can mirror HugeRegion::UnbackHugepages,
  // which treats the hugepages as fully used while the unback is in flight.
  std::function<void(Range)> begin_unback_;
  std::function<void(Range, bool)> end_unback_;
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

  std::vector<Range> allocs;
  std::vector<absl::Span<const Instruction>> reentrant_stack;
  std::string output;
  int depth = 0;

  // Per-hugepage mirror of the region's bookkeeping.
  struct HugePageModel {
    Length used;
    bool backed = false;
  };
  std::array<HugePageModel, HugeRegion::kNumHugePages> model{};
  // Hugepages whose unback started / succeeded, indexed by the reentrancy
  // depth of the operation that triggered it.
  static constexpr int kMaxDepth = 8;
  std::array<HugeLength, kMaxDepth> begun_by_depth{};
  std::array<HugeLength, kMaxDepth> released_by_depth{};

  explicit State(bool reentrant_release)
      : reentrant_release(reentrant_release),
        start(HugePageContaining(MakeTaggedAddress(MemoryTag::kNormal))),
        region({start, HugeRegion::size()}, unback, nil_set_anon_vma_name) {
    output.resize(1 << 20);

    unback.begin_unback_ = [this](Range r) {
      ForEachHugePage(r, [&](size_t i, Length here) {
        TC_CHECK_EQ(here, kPagesPerHugePage);
        TC_CHECK(model[i].backed);
        TC_CHECK_EQ(model[i].used, Length(0));
        model[i].used = kPagesPerHugePage;
        ++begun_by_depth[depth];
      });
    };
    unback.end_unback_ = [this](Range r, bool success) {
      ForEachHugePage(r, [&](size_t i, Length here) {
        model[i].used = Length(0);
        if (success) {
          model[i].backed = false;
          ++released_by_depth[depth];
        }
      });
    };

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
      Put(alloc, false);
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

  size_t IndexOf(PageId p) const {
    return (HugePageContaining(p) - start) / NHugePages(1);
  }

  // Invokes f(index, pages) for each hugepage r overlaps.
  template <typename F>
  void ForEachHugePage(Range r, F f) const {
    PageId p = r.p;
    const PageId end = r.p + r.n;
    while (p != end) {
      const size_t i = IndexOf(p);
      const PageId lim = (start + NHugePages(i + 1)).first_page();
      const Length here = std::min(end - p, lim - p);
      f(i, here);
      p += here;
    }
  }

  // Returns r to the region, checking that exactly the hugepages it empties
  // are unbacked, and only when release is requested.
  void Put(Range r, bool release) {
    HugeLength emptied;
    ForEachHugePage(r, [&](size_t i, Length here) {
      TC_CHECK(model[i].backed);
      TC_CHECK_GE(model[i].used, here);
      model[i].used -= here;
      if (model[i].used == Length(0)) {
        ++emptied;
      }
    });
    begun_by_depth[depth] = NHugePages(0);
    region.Put(r, release);
    TC_CHECK_EQ(begun_by_depth[depth], release ? emptied : NHugePages(0));
  }

  void CheckInvariants() {
    Length used;
    HugeLength backed;
    HugeLength free_backed;
    for (const HugePageModel& hp : model) {
      used += hp.used;
      if (hp.backed) {
        ++backed;
        if (hp.used == Length(0)) {
          ++free_backed;
        }
      }
    }
    TC_CHECK_EQ(region.used_pages(), used);
    TC_CHECK_EQ(region.backed(), backed);
    TC_CHECK_EQ(region.free_backed(), free_backed);
    TC_CHECK_EQ(region.unmapped_pages(),
                (HugeRegion::size() - backed).in_pages());

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
  // The range is disjoint from every live allocation.
  for (const Range& live : state.allocs) {
    TC_CHECK(!(p < live.p + live.n && live.p < p + n));
  }
  state.allocs.emplace_back(p, n);

  // from_released is set iff the range touched an unbacked hugepage, which is
  // backed afterwards.
  bool expected_from_released = false;
  state.ForEachHugePage({p, n}, [&](size_t i, Length here) {
    auto& hp = state.model[i];
    if (!hp.backed) {
      TC_CHECK_EQ(hp.used, Length(0));
      hp.backed = true;
      expected_from_released = true;
    }
    hp.used += here;
    TC_CHECK_LE(hp.used, kPagesPerHugePage);
  });
  TC_CHECK_EQ(from_released, expected_from_released);
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
  state.Put(alloc, release);
}

void Release::Perform(State& state) const {
  const Length len = Length(length % (1 << 18));
  const HugeLength max_expected =
      std::min(state.region.free_backed(), HLFromPages(len));

  // Mirror HugeRegion::Release's candidate selection: walk the hugepages in
  // allocation order (or reverse when adaptive) taking every free, backed
  // hugepage until at least min(desired, free_backed) pages are covered.
  HugeLength expected_begun;
  if (len > Length(0)) {
    const Length to_release =
        std::min(len, state.region.free_backed().in_pages());
    const int n = HugeRegion::kNumHugePages;
    for (int k = 0; k < n; ++k) {
      const int i = adaptive_release ? n - 1 - k : k;
      if (state.model[i].backed && state.model[i].used == Length(0)) {
        ++expected_begun;
      }
      if (expected_begun.in_pages() >= to_release) break;
    }
  }

  state.begun_by_depth[state.depth] = NHugePages(0);
  state.released_by_depth[state.depth] = NHugePages(0);
  const HugeLength actual = state.region.Release(len, adaptive_release);
  TC_CHECK_EQ(state.begun_by_depth[state.depth], expected_begun);
  // Release reports exactly the hugepages whose unback succeeded.
  TC_CHECK_EQ(actual, state.released_by_depth[state.depth]);

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

}  // namespace
}  // namespace tcmalloc::tcmalloc_internal
