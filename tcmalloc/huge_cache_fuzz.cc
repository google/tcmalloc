// Copyright 2024 The TCMalloc Authors
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
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "gtest/gtest.h"
#include "fuzztest/fuzztest.h"
#include "absl/algorithm/container.h"
#include "absl/base/attributes.h"
#include "absl/log/check.h"
#include "absl/strings/match.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_format.h"
#include "absl/strings/str_join.h"
#include "absl/strings/string_view.h"
#include "absl/time/time.h"
#include "absl/types/span.h"
#include "tcmalloc/huge_allocator.h"
#include "tcmalloc/huge_cache.h"
#include "tcmalloc/huge_pages.h"
#include "tcmalloc/internal/clock.h"
#include "tcmalloc/internal/config.h"
#include "tcmalloc/internal/logging.h"
#include "tcmalloc/internal/mock_metadata_allocator.h"
#include "tcmalloc/mock_virtual_allocator.h"
#include "tcmalloc/pages.h"
#include "tcmalloc/stats.h"

GOOGLE_MALLOC_SECTION_BEGIN
namespace tcmalloc::tcmalloc_internal {
namespace {

ABSL_CONST_INIT static int64_t fake_clock_ticks = 1234;

int64_t FakeClockNow() { return fake_clock_ticks; }

double FakeClockFreq() { return absl::ToDoubleNanoseconds(absl::Seconds(1)); }

// FakeVirtualAllocator hands out hugepage number backing_.size() next, so the
// first kBackingOffset hugepage numbers are never given to HugeAllocator.
constexpr size_t kBackingOffset = 1024;

// Where the oracle believes each hugepage of the fake address space is.
enum class PageState : uint8_t {
  // Never handed out by the virtual allocator: on neither free list.
  kOutside,
  // Free and unbacked: on HugeAllocator's free list.
  kUnbacked,
  // Free and backed: on HugeCache's free list.
  kCached,
  // Returned by Get and not yet released: on neither free list.
  kLive,
  // Taken off HugeCache's free list and inside the unback callback, not yet
  // handed to HugeAllocator.  Production drops pageheap_lock here.
  kUnbacking,
};
constexpr size_t kNumPageStates = 5;

struct State;

// HugeCache::ShrinkCache calls this with pageheap_lock dropped in production
// (HugePageAwareAllocator::UnbackWithoutLock), so other threads can operate on
// the cache in the middle of a shrink.  Run a queued reentrant subprogram here
// to model them.
class MockUnback final : public MemoryModifyFunction {
 public:
  explicit MockUnback(State& state) : state_(state) {}
  [[nodiscard]] MemoryModifyStatus operator()(Range r) override;

  bool unback_success_ = true;
  mutable bool has_failed_ = false;

 private:
  State& state_;
};

struct Get {
  size_t count;

  template <typename Sink>
  friend void AbslStringify(Sink& sink, const Get& g) {
    absl::Format(&sink, "Get{.count=%v}", g.count);
  }

  void Perform(State& state) const;
};

struct Release {
  size_t index;

  template <typename Sink>
  friend void AbslStringify(Sink& sink, const Release& r) {
    absl::Format(&sink, "Release{.index=%v}", r.index);
  }

  void Perform(State& state) const;
};

struct ReleaseUnbacked {
  size_t index;

  template <typename Sink>
  friend void AbslStringify(Sink& sink, const ReleaseUnbacked& r) {
    absl::Format(&sink, "ReleaseUnbacked{.index=%v}", r.index);
  }

  void Perform(State& state) const;
};

struct ReleaseCachedPages {
  size_t count;

  template <typename Sink>
  friend void AbslStringify(Sink& sink, const ReleaseCachedPages& r) {
    absl::Format(&sink, "ReleaseCachedPages{.count=%v}", r.count);
  }

  void Perform(State& state) const;
};

struct AdvanceClock {
  absl::Duration duration;

  template <typename Sink>
  friend void AbslStringify(Sink& sink, const AdvanceClock& a) {
    absl::Format(&sink, "AdvanceClock{.duration=absl::Nanoseconds(%v)}",
                 absl::ToInt64Nanoseconds(a.duration));
  }

  void Perform(State& state) const;
};

struct AddSpanStats {
  template <typename Sink>
  friend void AbslStringify(Sink& sink, const AddSpanStats&) {
    sink.Append("AddSpanStats{}");
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

struct SetUnbackSuccess {
  bool success;

  template <typename Sink>
  friend void AbslStringify(Sink& sink, const SetUnbackSuccess& s) {
    absl::Format(&sink, "SetUnbackSuccess{.success=%v}", s.success);
  }

  void Perform(State& state) const;
};

struct Reentrant;

using Instruction = std::variant<Get, Release, ReleaseUnbacked,
                                 ReleaseCachedPages, AdvanceClock, AddSpanStats,
                                 PrintStats, SetUnbackSuccess, Reentrant>;

template <typename Sink>
void AbslStringify(Sink& sink, const Instruction& i) {
  std::visit([&](auto&& arg) { absl::Format(&sink, "%v", arg); }, i);
}

// Queues a subprogram to run the next time the cache unbacks memory.
struct Reentrant {
  std::vector<Instruction> subprogram;

  template <typename Sink>
  friend void AbslStringify(Sink& sink, const Reentrant& r) {
    absl::Format(&sink, "Reentrant{.subprogram={%s}}",
                 absl::StrJoin(r.subprogram, ", ",
                               [](std::string* out, const Instruction& i) {
                                 absl::StrAppend(out, i);
                               }));
  }

  void Perform(State& state) const;
};

struct State {
  FakeVirtualAllocator vm_allocator;
  FakeMetadataAllocator metadata_allocator;
  HugeAllocator alloc;
  MockUnback unback;
  HugeCache cache;

  std::vector<HugeRange> live_ranges;
  HugeLength outstanding_usage = NHugePages(0);
  std::string output_buffer;

  // Oracle for every hugepage, indexed by HugePage::index().  kOutside pages
  // are not counted.
  std::vector<PageState> model;
  size_t model_counts[kNumPageStates] = {};

  std::vector<absl::Span<const Instruction>> reentrant_stack;
  int depth = 0;
  // Bumped whenever a reentrant subprogram runs, so an operation can tell
  // whether other instructions interleaved with it.
  size_t reentrant_runs = 0;

  explicit State(absl::Duration cache_time)
      : vm_allocator(),
        metadata_allocator(),
        alloc(vm_allocator, metadata_allocator),
        unback(*this),
        cache(alloc, metadata_allocator, unback,
              std::clamp(cache_time, absl::Milliseconds(10), absl::Minutes(10)),
              Clock{.now = FakeClockNow, .freq = FakeClockFreq}) {
    vm_allocator.backing_.resize(kBackingOffset);
    output_buffer.resize(1 << 20);
  }

  ~State() {
    // Releasing below unbacks; do not run subprograms that would add to
    // live_ranges while we drain it.
    reentrant_stack.clear();
    unback.unback_success_ = true;
    // Release all outstanding ranges so memory is reclaimed cleanly.
    while (!live_ranges.empty()) {
      const HugeRange r = live_ranges.back();
      live_ranges.pop_back();
      outstanding_usage -= r.len();
      Transition(r, {PageState::kLive}, PageState::kCached);
      cache.Release(r);
    }

    cache.ReleaseCachedPages(cache.size());
    CheckInvariants();
    TC_CHECK_EQ(cache.size(), NHugePages(0));
    TC_CHECK_EQ(cache.usage(), NHugePages(0));
    TC_CHECK_EQ(alloc.size(), alloc.system());
  }

  void RunInstructions(absl::Span<const Instruction> instrs) {
    for (const auto& inst : instrs) {
      std::visit([&](auto&& arg) { arg.Perform(*this); }, inst);
      // A shrink in progress has already taken the range being unbacked out
      // of size(), so only check between top-level instructions.
      if (depth == 0) {
        CheckInvariants();
      }
    }
  }

  [[nodiscard]] size_t Count(PageState s) const {
    return model_counts[static_cast<size_t>(s)];
  }

  // Moves every hugepage of r to `to`, checking each was in one of `from`.
  void Transition(HugeRange r, std::initializer_list<PageState> from,
                  PageState to) {
    const size_t begin = r.start().index();
    const size_t end = begin + r.len().raw_num();
    if (model.size() < end) {
      model.resize(end, PageState::kOutside);
    }
    for (size_t i = begin; i < end; ++i) {
      PageState& s = model[i];
      TC_CHECK(absl::c_linear_search(from, s), "hugepage %v is in state %v",
               HugePage{i}, static_cast<int>(s));
      if (s != PageState::kOutside) {
        --model_counts[static_cast<size_t>(s)];
      }
      ++model_counts[static_cast<size_t>(to)];
      s = to;
    }
  }

  // Hugepages the fake virtual allocator handed out since the last call go
  // straight onto HugeAllocator's free list.
  void AbsorbNewSystemPages(size_t backing_before) {
    const size_t backing_after = vm_allocator.backing_.size();
    if (backing_after == backing_before) {
      return;
    }
    Transition(HugeRange::Make(HugePage{backing_before},
                               NHugePages(backing_after - backing_before)),
               {PageState::kOutside}, PageState::kUnbacked);
  }

  void CheckContains(HugePage p) const {
    const PageState s =
        p.index() < model.size() ? model[p.index()] : PageState::kOutside;
    TC_CHECK_EQ(cache.Contains(p), s == PageState::kCached, "hugepage %v", p);
    TC_CHECK_EQ(alloc.Contains(p), s == PageState::kUnbacked, "hugepage %v", p);
  }

  void CheckContains(HugeRange r) const {
    for (size_t i = r.start().index(), end = i + r.len().raw_num(); i < end;
         ++i) {
      CheckContains(HugePage{i});
    }
  }

  void OnLockDropped() {
    if (reentrant_stack.empty()) {
      return;
    }
    if (depth >= 5) {
      return;
    }

    absl::Span<const Instruction> ops = reentrant_stack.back();
    reentrant_stack.pop_back();

    depth++;
    reentrant_runs++;
    RunInstructions(ops);
    depth--;
  }

  void CheckInvariants() const {
    if (cache.size() <= cache.limit()) {
      unback.has_failed_ = false;
    }
    TC_CHECK(cache.size() <= cache.limit() || unback.has_failed_);
    TC_CHECK_GE(cache.limit(), NHugePages(10));
    TC_CHECK_EQ(cache.usage(), outstanding_usage);
    BackingStats stats = cache.stats();
    TC_CHECK_EQ(stats.system_bytes, (cache.usage() + cache.size()).in_bytes());
    TC_CHECK_EQ(stats.free_bytes, cache.size().in_bytes());
    TC_CHECK_EQ(stats.unmapped_bytes, 0);

    const BackingStats alloc_stats = alloc.stats();
    TC_CHECK_EQ(alloc_stats.system_bytes, alloc.system().in_bytes());
    TC_CHECK_EQ(alloc_stats.free_bytes, 0);
    TC_CHECK_EQ(alloc_stats.unmapped_bytes, alloc.size().in_bytes());

    // The oracle partitions every hugepage the system handed out.
    TC_CHECK_EQ(alloc.system().raw_num(),
                vm_allocator.backing_.size() - kBackingOffset);
    TC_CHECK_EQ(Count(PageState::kLive), outstanding_usage.raw_num());
    TC_CHECK_EQ(Count(PageState::kCached), cache.size().raw_num());
    TC_CHECK_EQ(Count(PageState::kUnbacked), alloc.size().raw_num());
    TC_CHECK_EQ(Count(PageState::kLive) + Count(PageState::kCached) +
                    Count(PageState::kUnbacked) + Count(PageState::kUnbacking),
                alloc.system().raw_num());

    // Addresses below and above everything handed out.
    CheckContains(HugePage{0});
    CheckContains(HugePage{vm_allocator.backing_.size()});
  }
};

MemoryModifyStatus MockUnback::operator()(Range r) {
  // The cache only unbacks whole hugepages.
  TC_CHECK_EQ(HugePageContaining(r.p).first_page(), r.p);
  TC_CHECK_EQ(r.n % kPagesPerHugePage, Length(0));
  const HugeRange hr =
      HugeRange::Make(HugePageContaining(r.p), HLFromPages(r.n));
  // ShrinkCache has removed hr from the cache but not yet handed it to
  // HugeAllocator, so it is on neither free list.
  state_.Transition(hr, {PageState::kCached}, PageState::kUnbacking);
  state_.CheckContains(hr);

  state_.OnLockDropped();
  // Read after the subprogram, which may have flipped it.
  if (!unback_success_) {
    has_failed_ = true;
    // ShrinkCache puts the range back on its free list.
    state_.Transition(hr, {PageState::kUnbacking}, PageState::kCached);
    return {.success = false, .error_number = ENOMEM};
  }
  // ShrinkCache hands the range to HugeAllocator next.
  state_.Transition(hr, {PageState::kUnbacking}, PageState::kUnbacked);
  return {.success = true, .error_number = 0};
}

void Get::Perform(State& state) const {
  const HugeLength n = NHugePages(std::max<size_t>(1, count % 1024));
  const HugeLength size_before = state.cache.size();
  const HugeLength limit_before = state.cache.limit();
  const size_t backing_before = state.vm_allocator.backing_.size();
  bool from_released = false;
  HugeRange r = state.cache.Get(n, &from_released);
  if (!r.valid()) {
    // Only the backing allocator can fail, leaving the cache untouched.
    TC_CHECK(!from_released);
    TC_CHECK_EQ(state.cache.size(), size_before);
    TC_CHECK_EQ(state.cache.limit(), limit_before);
    TC_CHECK_EQ(state.vm_allocator.backing_.size(), backing_before);
    return;
  }
  TC_CHECK_EQ(r.len(), n);
  for (HugeRange live : state.live_ranges) {
    TC_CHECK(!r.intersects(live));
  }
  if (from_released) {
    // A miss leaves the cached ranges alone and can only grow the limit.
    TC_CHECK_EQ(state.cache.size(), size_before);
    TC_CHECK_GE(state.cache.limit(), limit_before);
    state.AbsorbNewSystemPages(backing_before);
    state.Transition(r, {PageState::kUnbacked}, PageState::kLive);
  } else {
    // A hit is carved out of the cached ranges and never moves the limit.
    TC_CHECK_GE(size_before, n);
    TC_CHECK_EQ(state.cache.size(), size_before - n);
    TC_CHECK_EQ(state.cache.limit(), limit_before);
    TC_CHECK_EQ(state.vm_allocator.backing_.size(), backing_before);
    state.Transition(r, {PageState::kCached}, PageState::kLive);
  }
  state.live_ranges.push_back(r);
  state.outstanding_usage += r.len();
  state.CheckContains(r);
}

void Release::Perform(State& state) const {
  if (state.live_ranges.empty()) {
    return;
  }
  const size_t idx = index % state.live_ranges.size();
  HugeRange r = state.live_ranges[idx];
  std::swap(state.live_ranges[idx], state.live_ranges.back());
  state.live_ranges.pop_back();
  state.outstanding_usage -= r.len();
  const HugeLength size_before = state.cache.size();
  const HugeLength limit_before = state.cache.limit();
  const bool unback_success = state.unback.unback_success_;
  const size_t runs_before = state.reentrant_runs;
  // The range enters the cache before ShrinkCache may unback parts of it.
  state.Transition(r, {PageState::kLive}, PageState::kCached);
  state.cache.Release(r);
  state.CheckContains(r);
  if (runs_before != state.reentrant_runs) {
    // Other operations interleaved with the shrink; State::CheckInvariants
    // still holds, but the exact size and limit are no longer predictable.
    return;
  }
  // Releasing can only shrink the limit.  Everything above the resulting
  // limit is unbacked, exactly, unless unbacking fails part way.
  TC_CHECK_LE(state.cache.limit(), limit_before);
  const HugeLength fits = std::min(size_before + r.len(), state.cache.limit());
  if (unback_success) {
    TC_CHECK_EQ(state.cache.size(), fits);
  } else {
    TC_CHECK_GE(state.cache.size(), fits);
    TC_CHECK_LE(state.cache.size(), size_before + r.len());
  }
}

void ReleaseUnbacked::Perform(State& state) const {
  if (state.live_ranges.empty()) {
    return;
  }
  const size_t idx = index % state.live_ranges.size();
  HugeRange r = state.live_ranges[idx];
  std::swap(state.live_ranges[idx], state.live_ranges.back());
  state.live_ranges.pop_back();
  state.outstanding_usage -= r.len();
  const HugeLength size_before = state.cache.size();
  const HugeLength limit_before = state.cache.limit();
  state.Transition(r, {PageState::kLive}, PageState::kUnbacked);
  state.cache.ReleaseUnbacked(r);
  // The range bypasses the cache entirely.
  TC_CHECK_EQ(state.cache.size(), size_before);
  TC_CHECK_EQ(state.cache.limit(), limit_before);
  state.CheckContains(r);
}

void ReleaseCachedPages::Perform(State& state) const {
  const HugeLength n = NHugePages(count % 1024);
  const HugeLength previous_size = state.cache.size();
  const HugeLength limit_before = state.cache.limit();
  const bool unback_success = state.unback.unback_success_;
  const size_t runs_before = state.reentrant_runs;
  const HugeLength released = state.cache.ReleaseCachedPages(n);
  if (runs_before != state.reentrant_runs) {
    // Interleaved operations can add to or take from the cache mid-shrink.
    return;
  }
  EXPECT_LE(released, previous_size);
  // Every released hugepage left the cache; failed unbacks stay cached.
  TC_CHECK_EQ(released, previous_size - state.cache.size());
  TC_CHECK_LE(state.cache.limit(), limit_before);
  if (unback_success) {
    // At least n pages are released, capped by what was cached; shrinking
    // the limit can release more.
    const HugeLength requested = std::min(n, previous_size);
    TC_CHECK_GE(released, requested);
  }
}

void AdvanceClock::Perform(State& state) const {
  fake_clock_ticks += absl::ToInt64Nanoseconds(
      std::clamp(duration, absl::ZeroDuration(), absl::Hours(1)));
}

void AddSpanStats::Perform(State& state) const {
  SmallSpanStats small;
  LargeSpanStats large;
  state.cache.AddSpanStats(&small, &large);
  TC_CHECK_EQ(large.normal_pages, state.cache.size().in_pages());
  TC_CHECK_EQ(large.returned_pages, Length(0));
  const size_t cache_spans = large.spans;
  TC_CHECK_LE(cache_spans, state.cache.size().raw_num());
  TC_CHECK_EQ(cache_spans == 0, state.cache.size() == NHugePages(0));

  // HugeAllocator reports its free list as returned pages on top of the
  // cache's.
  state.alloc.AddSpanStats(&small, &large);
  TC_CHECK_EQ(large.normal_pages, state.cache.size().in_pages());
  TC_CHECK_EQ(large.returned_pages, state.alloc.size().in_pages());
  const size_t alloc_spans = large.spans - cache_spans;
  TC_CHECK_LE(alloc_spans, state.alloc.size().raw_num());
  TC_CHECK_EQ(alloc_spans == 0, state.alloc.size() == NHugePages(0));
}

void PrintStats::Perform(State& state) const {
  {
    Printer printer(&state.output_buffer[0], state.output_buffer.size());
    state.cache.Print(printer);
    state.alloc.Print(printer);
    TC_CHECK_LE(printer.SpaceRequired(), state.output_buffer.size());
    const absl::string_view text(&state.output_buffer[0],
                                 printer.SpaceRequired());
    TC_CHECK(absl::StrContains(
        text, absl::StrFormat("HugeCache: %zu / %zu hugepages cached",
                              state.cache.size().raw_num(),
                              state.cache.limit().raw_num())));
    TC_CHECK(absl::StrContains(
        text, absl::StrFormat(
                  "HugeAllocator: %zu requested - %zu in use = %zu hugepages",
                  state.alloc.system().raw_num(),
                  (state.alloc.system() - state.alloc.size()).raw_num(),
                  state.alloc.size().raw_num())));
  }
  {
    Printer printer(&state.output_buffer[0], state.output_buffer.size());
    {
      PbtxtRegion pbtxt(printer, kTop);
      state.cache.PrintInPbtxt(pbtxt);
      state.alloc.PrintInPbtxt(pbtxt);
    }
    TC_CHECK_LE(printer.SpaceRequired(), state.output_buffer.size());
    const absl::string_view text(&state.output_buffer[0],
                                 printer.SpaceRequired());
    TC_CHECK(absl::StrContains(
        text, absl::StrFormat("num_total_requested_huge_pages: %zu",
                              state.alloc.system().raw_num())));
    TC_CHECK(absl::StrContains(
        text, absl::StrFormat(
                  "num_in_use_huge_pages: %zu",
                  (state.alloc.system() - state.alloc.size()).raw_num())));
  }
}

void SetUnbackSuccess::Perform(State& state) const {
  state.unback.unback_success_ = success;
}

// Queued at any depth; State::OnLockDropped bounds the nesting.
void Reentrant::Perform(State& state) const {
  if (subprogram.empty()) {
    return;
  }
  state.reentrant_stack.push_back(subprogram);
}

void FuzzHugeCache(const std::vector<Instruction>& instructions,
                   absl::Duration cache_time) {
  fake_clock_ticks = 1234;

  State state(cache_time);
  state.RunInstructions(instructions);
}

auto ArbitraryDurationDomain() {
  return fuzztest::Map([](int64_t ns) { return absl::Nanoseconds(ns); },
                       fuzztest::Arbitrary<int64_t>());
}

auto CacheTimeDomain() {
  return fuzztest::Map([](int64_t ms) { return absl::Milliseconds(ms); },
                       fuzztest::InRange<int64_t>(10, 60000));
}

auto GetFlatInstructionDomain() {
  return fuzztest::OneOf(
      fuzztest::Map([](Get g) -> Instruction { return Instruction{g}; },
                    fuzztest::Arbitrary<Get>()),
      fuzztest::Map([](Release r) -> Instruction { return Instruction{r}; },
                    fuzztest::Arbitrary<Release>()),
      fuzztest::Map(
          [](ReleaseUnbacked r) -> Instruction { return Instruction{r}; },
          fuzztest::Arbitrary<ReleaseUnbacked>()),
      fuzztest::Map(
          [](ReleaseCachedPages r) -> Instruction { return Instruction{r}; },
          fuzztest::Arbitrary<ReleaseCachedPages>()),
      fuzztest::Map([](absl::Duration d)
                        -> Instruction { return Instruction{AdvanceClock{d}}; },
                    ArbitraryDurationDomain()),
      fuzztest::Map(
          [](AddSpanStats a) -> Instruction { return Instruction{a}; },
          fuzztest::Arbitrary<AddSpanStats>()),
      fuzztest::Map([](PrintStats p) -> Instruction { return Instruction{p}; },
                    fuzztest::Arbitrary<PrintStats>()),
      fuzztest::Map(
          [](SetUnbackSuccess s) -> Instruction { return Instruction{s}; },
          fuzztest::Arbitrary<SetUnbackSuccess>()));
}

fuzztest::Domain<Instruction> GetInstructionDomain(int depth) {
  if (depth <= 0) {
    return GetFlatInstructionDomain();
  }
  return fuzztest::OneOf(
      GetFlatInstructionDomain(),
      fuzztest::Map(
          [](std::vector<Instruction> sub) -> Instruction {
            return Instruction{Reentrant{std::move(sub)}};
          },
          fuzztest::VectorOf(GetInstructionDomain(depth - 1))));
}

FUZZ_TEST(HugeCacheTest, FuzzHugeCache)
    .WithDomains(fuzztest::VectorOf(GetInstructionDomain(/*depth=*/5)),
                 CacheTimeDomain());

// ShrinkCache removes a range from the cache, unbacks it with the lock
// dropped, and then keeps shrinking towards its original target.  A Get and
// Release interleaved at the unback must leave the cache consistent.
TEST(HugeCacheTest, ReentrantGetAndReleaseDuringShrink) {
  FuzzHugeCache(
      {
          Get{.count = 4},
          Release{.index = 0},
          Reentrant{.subprogram = {Get{.count = 2}, Release{.index = 0}}},
          ReleaseCachedPages{.count = 4},
      },
      absl::Seconds(1));
}

TEST(HugeCacheTest, Regression) {
  FuzzHugeCache(
      {
          Get{.count = 1},
          Get{.count = 5},
          Release{.index = 0},
          AdvanceClock{.duration = absl::Seconds(1)},
          ReleaseCachedPages{.count = 1},
          AddSpanStats{},
          PrintStats{},
          ReleaseUnbacked{.index = 0},
      },
      absl::Seconds(1));
}

TEST(HugeCacheTest, FailingUnbackRegression) {
  FuzzHugeCache(
      {
          SetUnbackSuccess{.success = false},
          Get{.count = 18446744073709551615ULL},
          PrintStats{},
          Release{.index = 18446744073709551615ULL},
          Release{.index = 0},
      },
      absl::Seconds(37) + absl::Nanoseconds(822000000));
}

// ShrinkCache unbacks one node per iteration.  Two cached nodes and a target
// below both make it loop twice; the second unback fails from inside the
// callback, so the first node is unbacked and the second returns to the
// cache.  Every step checks Contains against the per-hugepage model.
TEST(HugeCacheTest, UnbackFailsMidShrinkContains) {
  FuzzHugeCache(
      {
          Get{.count = 4},
          Get{.count = 4},
          Get{.count = 4},
          Get{.count = 4},
          // Release the first and last ranges: two non-adjacent cached
          // nodes of 4 hugepages, under the 10 hugepage limit.
          Release{.index = 0},
          Release{.index = 0},
          // Subprograms run LIFO, one per unback callback.
          Reentrant{.subprogram = {SetUnbackSuccess{.success = false}}},
          Reentrant{.subprogram = {AddSpanStats{}, PrintStats{}}},
          // Target 2: the first node is unbacked whole, the second is split
          // and its unback fails.
          ReleaseCachedPages{.count = 6},
          SetUnbackSuccess{.success = true},
          // Hit carved from the surviving node.
          Get{.count = 3},
          ReleaseUnbacked{.index = 0},
          AddSpanStats{},
          PrintStats{},
          Release{.index = 0},
          ReleaseCachedPages{.count = 1023},
      },
      absl::Seconds(1));
}

}  // namespace
}  // namespace tcmalloc::tcmalloc_internal
GOOGLE_MALLOC_SECTION_END
