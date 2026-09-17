// Copyright 2026 The TCMalloc Authors
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
#include <utility>
#include <variant>
#include <vector>

#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "fuzztest/fuzztest.h"
#include "absl/strings/str_format.h"
#include "absl/types/span.h"
#include "tcmalloc/internal/logging.h"
#include "tcmalloc/internal/range_tracker.h"

namespace tcmalloc::tcmalloc_internal {
namespace {

void FuzzBitmapPopBatch(const std::array<bool, 128>& bits_to_set,
                        size_t limit) {
  constexpr size_t N = 128;
  Bitmap<N> map1;
  Bitmap<N> map2;

  for (size_t i = 0; i < N; ++i) {
    if (bits_to_set[i]) {
      map1.SetBit(i);
      map2.SetBit(i);
    }
  }

  std::vector<size_t> offsets1, offsets2;
  size_t popped1 =
      map1.PopBatch([&](size_t v) { offsets1.push_back(v); }, limit);
  EXPECT_EQ(offsets1.size(), popped1);

  while (!map2.IsZero() && offsets2.size() < limit) {
    size_t offset = map2.FindSet(0);
    offsets2.push_back(offset);
    map2.ClearBit(offset);
  }

  EXPECT_THAT(offsets1, testing::Eq(offsets2));

  for (size_t i = 0; i < N; ++i) {
    EXPECT_EQ(map1.GetBit(i), map2.GetBit(i));
  }
  EXPECT_EQ(map1.IsZero(), map2.IsZero());
}

FUZZ_TEST(BitmapFuzzTest, FuzzBitmapPopBatch)
    .WithDomains(fuzztest::Arbitrary<std::array<bool, 128>>(),
                 fuzztest::InRange<size_t>(0, 128));

void FuzzBitmapCountBits(const std::array<bool, 253>& bits_to_set, size_t start,
                         size_t length) {
  constexpr size_t N = 253;
  if (start > N || start + length > N) {
    return;
  }

  Bitmap<N> map;
  for (size_t i = 0; i < N; ++i) {
    if (bits_to_set[i]) {
      map.SetBit(i);
    }
  }

  size_t expected = 0;
  for (size_t j = 0; j < length; j++) {
    size_t idx = start + j;
    if (bits_to_set[idx]) {
      expected++;
    }
  }

  EXPECT_EQ(expected, map.CountBits(start, length));
}

FUZZ_TEST(BitmapFuzzTest, FuzzBitmapCountBits)
    .WithDomains(fuzztest::Arbitrary<std::array<bool, 253>>(),
                 fuzztest::InRange<size_t>(0, 253),
                 fuzztest::InRange<size_t>(0, 253));

void FuzzBitmapCopyBits(const std::array<bool, 256>& src_bits,
                        const std::array<bool, 256>& dst_initial_bits,
                        size_t src_offset, size_t dst_offset, size_t length) {
  constexpr size_t N = 256;
  if (src_offset > N || src_offset + length > N || dst_offset > N ||
      dst_offset + length > N) {
    return;
  }

  Bitmap<N> src_map;
  Bitmap<N> dst_map;
  std::array<bool, N> expected_dst = dst_initial_bits;

  for (size_t i = 0; i < N; ++i) {
    if (src_bits[i]) {
      src_map.SetBit(i);
    }
    if (dst_initial_bits[i]) {
      dst_map.SetBit(i);
    }
  }

  for (size_t i = 0; i < length; ++i) {
    expected_dst[dst_offset + i] = src_bits[src_offset + i];
  }

  CopyBits(dst_map, dst_offset, src_map, src_offset, length);

  for (size_t i = 0; i < N; ++i) {
    EXPECT_EQ(dst_map.GetBit(i), expected_dst[i]);
  }
}

FUZZ_TEST(BitmapFuzzTest, FuzzBitmapCopyBits)
    .WithDomains(fuzztest::Arbitrary<std::array<bool, 256>>(),
                 fuzztest::Arbitrary<std::array<bool, 256>>(),
                 fuzztest::InRange<size_t>(0, 256),
                 fuzztest::InRange<size_t>(0, 256),
                 fuzztest::InRange<size_t>(0, 256));

void FuzzBitmapCopyBitsDifferentSizes(
    const std::array<bool, 127>& src_bits,
    const std::array<bool, 300>& dst_initial_bits, size_t src_offset,
    size_t dst_offset, size_t length) {
  constexpr size_t SrcN = 127;
  constexpr size_t DstN = 300;
  if (src_offset > SrcN || src_offset + length > SrcN || dst_offset > DstN ||
      dst_offset + length > DstN) {
    return;
  }

  Bitmap<SrcN> src_map;
  Bitmap<DstN> dst_map;
  std::array<bool, DstN> expected_dst = dst_initial_bits;

  for (size_t i = 0; i < SrcN; ++i) {
    if (src_bits[i]) {
      src_map.SetBit(i);
    }
  }
  for (size_t i = 0; i < DstN; ++i) {
    if (dst_initial_bits[i]) {
      dst_map.SetBit(i);
    }
  }

  for (size_t i = 0; i < length; ++i) {
    expected_dst[dst_offset + i] = src_bits[src_offset + i];
  }

  CopyBits(dst_map, dst_offset, src_map, src_offset, length);

  for (size_t i = 0; i < DstN; ++i) {
    EXPECT_EQ(dst_map.GetBit(i), expected_dst[i]);
  }
}

FUZZ_TEST(BitmapFuzzTest, FuzzBitmapCopyBitsDifferentSizes)
    .WithDomains(fuzztest::Arbitrary<std::array<bool, 127>>(),
                 fuzztest::Arbitrary<std::array<bool, 300>>(),
                 fuzztest::InRange<size_t>(0, 127),
                 fuzztest::InRange<size_t>(0, 300),
                 fuzztest::InRange<size_t>(0, 300));

void FuzzBitmapCopyBitsContractingSizes(
    const std::array<bool, 300>& src_bits,
    const std::array<bool, 127>& dst_initial_bits, size_t src_offset,
    size_t dst_offset, size_t length) {
  constexpr size_t SrcN = 300;
  constexpr size_t DstN = 127;
  if (src_offset > SrcN || src_offset + length > SrcN || dst_offset > DstN ||
      dst_offset + length > DstN) {
    return;
  }

  Bitmap<SrcN> src_map;
  Bitmap<DstN> dst_map;
  std::array<bool, DstN> expected_dst = dst_initial_bits;

  for (size_t i = 0; i < SrcN; ++i) {
    if (src_bits[i]) {
      src_map.SetBit(i);
    }
  }
  for (size_t i = 0; i < DstN; ++i) {
    if (dst_initial_bits[i]) {
      dst_map.SetBit(i);
    }
  }

  for (size_t i = 0; i < length; ++i) {
    expected_dst[dst_offset + i] = src_bits[src_offset + i];
  }

  CopyBits(dst_map, dst_offset, src_map, src_offset, length);

  for (size_t i = 0; i < DstN; ++i) {
    EXPECT_EQ(dst_map.GetBit(i), expected_dst[i]);
  }
}

FUZZ_TEST(BitmapFuzzTest, FuzzBitmapCopyBitsContractingSizes)
    .WithDomains(fuzztest::Arbitrary<std::array<bool, 300>>(),
                 fuzztest::Arbitrary<std::array<bool, 127>>(),
                 fuzztest::InRange<size_t>(0, 300),
                 fuzztest::InRange<size_t>(0, 127),
                 fuzztest::InRange<size_t>(0, 127));

// Odd and larger than three words so ranges straddle word boundaries and the
// final partial word.
constexpr size_t kTrackerBits = 253;

struct TrackerState {
  RangeTracker<kTrackerBits> tracker;
  // Reference copy of the tracker's bits.
  std::vector<bool> model = std::vector<bool>(kTrackerBits, false);
  // Ranges handed out by FindAndMark/Mark that have not been unmarked yet.
  std::vector<std::pair<size_t, size_t>> live;

  // Free runs of the model as (index, length) in increasing index order.
  std::vector<std::pair<size_t, size_t>> FreeRuns() const {
    std::vector<std::pair<size_t, size_t>> runs;
    for (size_t i = 0; i < kTrackerBits;) {
      if (model[i]) {
        ++i;
        continue;
      }
      size_t j = i;
      while (j < kTrackerBits && !model[j]) ++j;
      runs.emplace_back(i, j - i);
      i = j;
    }
    return runs;
  }

  void CheckInvariants() const {
    size_t used = 0;
    for (size_t i = 0; i < kTrackerBits; ++i) {
      TC_CHECK_EQ(tracker.bits().GetBit(i), model[i]);
      used += model[i];
    }
    TC_CHECK_EQ(tracker.used(), used);
    TC_CHECK_EQ(tracker.allocs(), live.size());

    const std::vector<std::pair<size_t, size_t>> runs = FreeRuns();
    size_t longest = 0;
    for (const auto& [index, length] : runs) {
      longest = std::max(longest, length);
    }
    TC_CHECK_EQ(tracker.longest_free(), longest);

    // Walking NextFreeRange from the start enumerates exactly the free runs.
    size_t start = 0;
    for (const auto& [index, length] : runs) {
      size_t found_index, found_length;
      TC_CHECK(tracker.NextFreeRange(start, &found_index, &found_length));
      TC_CHECK_EQ(found_index, index);
      TC_CHECK_EQ(found_length, length);
      start = index + length;
    }
    size_t unused_index, unused_length;
    TC_CHECK(!tracker.NextFreeRange(start, &unused_index, &unused_length));
  }
};

struct FindAndMark {
  size_t n;

  template <typename Sink>
  friend void AbslStringify(Sink& sink, const FindAndMark& f) {
    absl::Format(&sink, "FindAndMark{.n=%v}", f.n);
  }

  void Perform(TrackerState& state) const {
    const size_t len = 1 + n % kTrackerBits;
    if (len > state.tracker.longest_free()) {
      return;
    }
    // Best fit: the first free run of the smallest length that still fits.
    size_t expected_index = kTrackerBits;
    size_t expected_length = 2 * kTrackerBits;
    for (const auto& [index, length] : state.FreeRuns()) {
      if (length >= len && length < expected_length) {
        expected_index = index;
        expected_length = length;
      }
    }
    TC_CHECK_LT(expected_index, kTrackerBits);

    const size_t index = state.tracker.FindAndMark(len);
    TC_CHECK_EQ(index, expected_index);
    std::fill(state.model.begin() + index, state.model.begin() + index + len,
              true);
    state.live.emplace_back(index, len);
  }
};

struct Mark {
  size_t index;
  size_t n;

  template <typename Sink>
  friend void AbslStringify(Sink& sink, const Mark& m) {
    absl::Format(&sink, "Mark{.index=%v, .n=%v}", m.index, m.n);
  }

  void Perform(TrackerState& state) const {
    const size_t start = index % kTrackerBits;
    const size_t len = 1 + n % (kTrackerBits - start);
    // Mark requires the range to be entirely free.
    for (size_t i = start; i < start + len; ++i) {
      if (state.model[i]) {
        return;
      }
    }
    state.tracker.Mark(start, len);
    std::fill(state.model.begin() + start, state.model.begin() + start + len,
              true);
    state.live.emplace_back(start, len);
  }
};

struct Unmark {
  size_t which;

  template <typename Sink>
  friend void AbslStringify(Sink& sink, const Unmark& u) {
    absl::Format(&sink, "Unmark{.which=%v}", u.which);
  }

  void Perform(TrackerState& state) const {
    if (state.live.empty()) {
      return;
    }
    const size_t i = which % state.live.size();
    const auto [index, len] = state.live[i];
    std::swap(state.live[i], state.live.back());
    state.live.pop_back();
    state.tracker.Unmark(index, len);
    std::fill(state.model.begin() + index, state.model.begin() + index + len,
              false);
  }
};

struct NextFreeRangeFrom {
  size_t start;

  template <typename Sink>
  friend void AbslStringify(Sink& sink, const NextFreeRangeFrom& n) {
    absl::Format(&sink, "NextFreeRangeFrom{.start=%v}", n.start);
  }

  void Perform(TrackerState& state) const {
    // Include kTrackerBits itself to exercise the out-of-range start.
    const size_t from = start % (kTrackerBits + 1);
    size_t expected_index = from;
    while (expected_index < kTrackerBits && state.model[expected_index]) {
      ++expected_index;
    }
    size_t index, length;
    const bool found = state.tracker.NextFreeRange(from, &index, &length);
    if (expected_index >= kTrackerBits) {
      TC_CHECK(!found);
      return;
    }
    TC_CHECK(found);
    // A start inside a free run yields the remainder of that run.
    size_t expected_end = expected_index;
    while (expected_end < kTrackerBits && !state.model[expected_end]) {
      ++expected_end;
    }
    TC_CHECK_EQ(index, expected_index);
    TC_CHECK_EQ(length, expected_end - expected_index);
  }
};

struct Clear {
  template <typename Sink>
  friend void AbslStringify(Sink& sink, const Clear&) {
    sink.Append("Clear{}");
  }

  void Perform(TrackerState& state) const {
    state.tracker.Clear();
    std::fill(state.model.begin(), state.model.end(), false);
    state.live.clear();
  }
};

using Instruction =
    std::variant<FindAndMark, Mark, Unmark, NextFreeRangeFrom, Clear>;

template <typename Sink>
void AbslStringify(Sink& sink, const Instruction& i) {
  std::visit([&](auto&& arg) { absl::Format(&sink, "%v", arg); }, i);
}

void FuzzRangeTracker(const std::vector<Instruction>& instructions) {
  TrackerState state;
  state.CheckInvariants();
  for (const auto& inst : instructions) {
    std::visit([&](auto&& arg) { arg.Perform(state); }, inst);
    state.CheckInvariants();
  }
}

fuzztest::Domain<Instruction> GetInstructionDomain() {
  return fuzztest::OneOf(
      fuzztest::Map([](FindAndMark f) { return Instruction{f}; },
                    fuzztest::Arbitrary<FindAndMark>()),
      fuzztest::Map([](Mark m) { return Instruction{m}; },
                    fuzztest::Arbitrary<Mark>()),
      fuzztest::Map([](Unmark u) { return Instruction{u}; },
                    fuzztest::Arbitrary<Unmark>()),
      fuzztest::Map([](NextFreeRangeFrom n) { return Instruction{n}; },
                    fuzztest::Arbitrary<NextFreeRangeFrom>()),
      fuzztest::Map([](Clear c) { return Instruction{c}; },
                    fuzztest::Arbitrary<Clear>()));
}

FUZZ_TEST(RangeTrackerFuzzTest, FuzzRangeTracker)
    .WithDomains(fuzztest::VectorOf(GetInstructionDomain()));

TEST(RangeTrackerFuzzTest, BestFitPrefersShortestRun) {
  // Two runs of free bits remain after the unmark; the shorter must win.
  FuzzRangeTracker({
      FindAndMark{.n = 99},
      FindAndMark{.n = 9},
      FindAndMark{.n = 19},
      Unmark{.which = 1},
      FindAndMark{.n = 4},
      Unmark{.which = 0},
      NextFreeRangeFrom{.start = 5},
      Clear{},
  });
}

}  // namespace
}  // namespace tcmalloc::tcmalloc_internal
