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
#include <string>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "gtest/gtest.h"
#include "fuzztest/fuzztest.h"
#include "absl/base/attributes.h"
#include "absl/log/check.h"
#include "absl/strings/str_format.h"
#include "absl/time/time.h"
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

class MockUnback final : public MemoryModifyFunction {
 public:
  [[nodiscard]] MemoryModifyStatus operator()(Range r) override {
    if (!unback_success_) {
      has_failed_ = true;
      return {.success = false, .error_number = ENOMEM};
    }
    return {.success = true, .error_number = 0};
  }

  bool unback_success_ = true;
  mutable bool has_failed_ = false;
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

  explicit State(absl::Duration cache_time)
      : vm_allocator(),
        metadata_allocator(),
        alloc(vm_allocator, metadata_allocator),
        unback(),
        cache(alloc, metadata_allocator, unback,
              std::clamp(cache_time, absl::Milliseconds(10), absl::Minutes(10)),
              Clock{.now = FakeClockNow, .freq = FakeClockFreq}) {
    vm_allocator.backing_.resize(1024);
    output_buffer.resize(1 << 20);
  }

  ~State() {
    unback.unback_success_ = true;
    // Release all outstanding ranges so memory is reclaimed cleanly.
    for (HugeRange r : live_ranges) {
      cache.Release(r);
    }
    live_ranges.clear();
    outstanding_usage = NHugePages(0);

    cache.ReleaseCachedPages(cache.size());
    CheckInvariants();
    TC_CHECK_EQ(cache.size(), NHugePages(0));
    TC_CHECK_EQ(cache.usage(), NHugePages(0));
    TC_CHECK_EQ(alloc.size(), alloc.system());
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
  }
};

struct Get {
  size_t count;

  template <typename Sink>
  friend void AbslStringify(Sink& sink, const Get& g) {
    absl::Format(&sink, "Get{.count=%v}", g.count);
  }

  void Perform(State& state) const {
    const HugeLength n = NHugePages(std::max<size_t>(1, count % 1024));
    bool from_released = false;
    HugeRange r = state.cache.Get(n, &from_released);
    if (r.valid()) {
      state.live_ranges.push_back(r);
      state.outstanding_usage += r.len();
    }
  }
};

struct Release {
  size_t index;

  template <typename Sink>
  friend void AbslStringify(Sink& sink, const Release& r) {
    absl::Format(&sink, "Release{.index=%v}", r.index);
  }

  void Perform(State& state) const {
    if (state.live_ranges.empty()) {
      return;
    }
    const size_t idx = index % state.live_ranges.size();
    HugeRange r = state.live_ranges[idx];
    std::swap(state.live_ranges[idx], state.live_ranges.back());
    state.live_ranges.pop_back();
    state.outstanding_usage -= r.len();
    state.cache.Release(r);
  }
};

struct ReleaseUnbacked {
  size_t index;

  template <typename Sink>
  friend void AbslStringify(Sink& sink, const ReleaseUnbacked& r) {
    absl::Format(&sink, "ReleaseUnbacked{.index=%v}", r.index);
  }

  void Perform(State& state) const {
    if (state.live_ranges.empty()) {
      return;
    }
    const size_t idx = index % state.live_ranges.size();
    HugeRange r = state.live_ranges[idx];
    std::swap(state.live_ranges[idx], state.live_ranges.back());
    state.live_ranges.pop_back();
    state.outstanding_usage -= r.len();
    state.cache.ReleaseUnbacked(r);
  }
};

struct ReleaseCachedPages {
  size_t count;

  template <typename Sink>
  friend void AbslStringify(Sink& sink, const ReleaseCachedPages& r) {
    absl::Format(&sink, "ReleaseCachedPages{.count=%v}", r.count);
  }

  void Perform(State& state) const {
    const HugeLength n = NHugePages(count % 1024);
    const HugeLength previous_size = state.cache.size();
    const HugeLength released = state.cache.ReleaseCachedPages(n);
    EXPECT_LE(released, previous_size);
  }
};

struct AdvanceClock {
  absl::Duration duration;

  template <typename Sink>
  friend void AbslStringify(Sink& sink, const AdvanceClock& a) {
    absl::Format(&sink, "AdvanceClock{.duration=absl::Nanoseconds(%v)}",
                 absl::ToInt64Nanoseconds(a.duration));
  }

  void Perform(State& state) const {
    fake_clock_ticks += absl::ToInt64Nanoseconds(
        std::clamp(duration, absl::ZeroDuration(), absl::Hours(1)));
  }
};

struct AddSpanStats {
  template <typename Sink>
  friend void AbslStringify(Sink& sink, const AddSpanStats&) {
    sink.Append("AddSpanStats{}");
  }

  void Perform(State& state) const {
    SmallSpanStats small;
    LargeSpanStats large;
    state.cache.AddSpanStats(&small, &large);
    TC_CHECK_EQ(large.normal_pages, state.cache.size().in_pages());
    TC_CHECK_EQ(large.returned_pages, Length(0));
  }
};

struct PrintStats {
  template <typename Sink>
  friend void AbslStringify(Sink& sink, const PrintStats&) {
    sink.Append("PrintStats{}");
  }

  void Perform(State& state) const {
    Printer printer(&state.output_buffer[0], state.output_buffer.size());
    state.cache.Print(printer);
    Printer pbtxt_printer(&state.output_buffer[0], state.output_buffer.size());
    PbtxtRegion pbtxt(pbtxt_printer, kTop);
    state.cache.PrintInPbtxt(pbtxt);
  }
};

struct SetUnbackSuccess {
  bool success;

  template <typename Sink>
  friend void AbslStringify(Sink& sink, const SetUnbackSuccess& s) {
    absl::Format(&sink, "SetUnbackSuccess{.success=%v}", s.success);
  }

  void Perform(State& state) const { state.unback.unback_success_ = success; }
};

using Instruction =
    std::variant<Get, Release, ReleaseUnbacked, ReleaseCachedPages,
                 AdvanceClock, AddSpanStats, PrintStats, SetUnbackSuccess>;

template <typename Sink>
void AbslStringify(Sink& sink, const Instruction& i) {
  std::visit([&](auto&& arg) { absl::Format(&sink, "%v", arg); }, i);
}

void FuzzHugeCache(const std::vector<Instruction>& instructions,
                   absl::Duration cache_time) {
  fake_clock_ticks = 1234;

  State state(cache_time);

  for (const auto& inst : instructions) {
    std::visit([&](auto&& arg) { arg.Perform(state); }, inst);
    state.CheckInvariants();
  }
}

auto ArbitraryDurationDomain() {
  return fuzztest::Map([](int64_t ns) { return absl::Nanoseconds(ns); },
                       fuzztest::Arbitrary<int64_t>());
}

auto CacheTimeDomain() {
  return fuzztest::Map([](int64_t ms) { return absl::Milliseconds(ms); },
                       fuzztest::InRange<int64_t>(10, 60000));
}

fuzztest::Domain<Instruction> GetInstructionDomain() {
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

FUZZ_TEST(HugeCacheTest, FuzzHugeCache)
    .WithDomains(fuzztest::VectorOf(GetInstructionDomain()), CacheTimeDomain());

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

}  // namespace
}  // namespace tcmalloc::tcmalloc_internal
GOOGLE_MALLOC_SECTION_END
