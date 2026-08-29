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
    release_callback_();

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
  std::function<void()> release_callback_;
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
