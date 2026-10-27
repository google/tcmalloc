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
#include <cstdlib>
#include <cstring>
#include <limits>
#include <new>
#include <random>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "gtest/gtest.h"
#include "fuzztest/fuzztest.h"
#include "absl/types/span.h"
#include "tcmalloc/common.h"
#include "tcmalloc/internal/config.h"
#include "tcmalloc/internal/logging.h"
#include "tcmalloc/pages.h"
#include "tcmalloc/sizemap.h"
#include "tcmalloc/span.h"

namespace tcmalloc::tcmalloc_internal {
namespace {

auto AnyLength() {
  return fuzztest::ConstructorOf<Length>(fuzztest::Arbitrary<size_t>());
}

struct State {
  static constexpr uint8_t kSpanPoisonPattern = 0xA5;
  static constexpr uint8_t kCanaryMask = 0x5A;

  size_t object_size;
  Length pages;
  size_t num_to_move;
  size_t objects_per_span = 0;
  Span::BitmapMode bitmap_mode = Span::BitmapMode::kDisabled;
  uint32_t size_reciprocal;
  void* mem = nullptr;
  std::unique_ptr<Span> span;
  std::vector<void*> live_ptrs;
  std::vector<bool> allocated_idx;
  std::vector<void*> batch;
  std::mt19937 rng;
  uint8_t nonempty_index = 0;

  State(bool use_bitmap_for_small, size_t object_size, Length pages,
        size_t num_to_move)
      : object_size(object_size),
        pages(pages),
        num_to_move(num_to_move),
        size_reciprocal(Span::CalcReciprocal(object_size)) {
    const auto info =
        Span::CalcBitmapInfo(use_bitmap_for_small, pages, object_size);
    objects_per_span = info.objects_per_span;
    bitmap_mode = info.mode;
    TC_CHECK_EQ(posix_memalign(&mem, kPageSize, pages.in_bytes()), 0);
    memset(mem, kSpanPoisonPattern, pages.in_bytes());

    span = std::make_unique<Span>(PageIdContaining(mem));
    TC_CHECK_EQ(
        span->BuildFreelist(object_size, objects_per_span, {}, bitmap_mode), 0);

    live_ptrs.reserve(objects_per_span);
    allocated_idx.assign(objects_per_span, false);
    batch.resize(kMaxObjectsToMove);
  }

  std::pair<size_t, uint8_t> ValidateObj(void* p) const {
    char* const start = static_cast<char*>(mem);
    const uintptr_t off = static_cast<char*>(p) - start;
    TC_CHECK_LT(off, objects_per_span * object_size);
    TC_CHECK_EQ(off % object_size, 0);
    const size_t idx = off / object_size;
    const uint8_t pattern = static_cast<uint8_t>((idx & 0xFF) ^ kCanaryMask);
    return {idx, pattern};
  }

  void ValidateAndRecordPop(void* p) {
    const auto [idx, pattern] = ValidateObj(p);
    TC_CHECK(!allocated_idx[idx]);
    allocated_idx[idx] = true;
    const size_t fill = std::min<size_t>(object_size, 64);
    memset(p, pattern, fill);
    memset(static_cast<char*>(p) + object_size - fill, pattern, fill);
  }

  void ValidateAndRecordPush(void* p) {
    const auto [idx, pattern] = ValidateObj(p);
    TC_CHECK(allocated_idx[idx]);
    allocated_idx[idx] = false;
    const auto* bytes = static_cast<const uint8_t*>(p);
    TC_CHECK_EQ(bytes[0], pattern);
    TC_CHECK_EQ(bytes[object_size - 1], pattern);
  }

  ~State() {
    for (size_t i = 0; i < live_ptrs.size();) {
      size_t limit = std::min<size_t>(live_ptrs.size() - i, num_to_move);
      for (size_t j = 0; j < limit; ++j) {
        ValidateAndRecordPush(live_ptrs[i + j]);
      }
      Span::ObjIdx idx[kMaxObjectsToMove];
      Span::ObjectsToIdx({&live_ptrs[i], limit}, span.get(), object_size,
                         size_reciprocal, idx, bitmap_mode);
      (void)span->FreelistPushBatch(absl::MakeSpan(idx, limit), object_size,
                                    bitmap_mode);
      i += limit;
    }
    free(mem);
  }
};

struct Alloc {
  uint8_t count;

  template <typename Sink>
  friend void AbslStringify(Sink& sink, const Alloc& a) {
    absl::Format(&sink, "Alloc{.count=%v}", a.count);
  }

  void Perform(State& state) const {
    if (state.span->FreelistEmpty(state.objects_per_span)) {
      return;
    }
    size_t n = std::min<size_t>(count, state.num_to_move);
    if (n == 0) {
      return;
    }

    size_t popped = state.span->FreelistPopBatch(
        absl::MakeSpan(state.batch.data(), n), state.object_size,
        state.objects_per_span, state.bitmap_mode);
    for (size_t i = 0; i < popped; ++i) {
      state.ValidateAndRecordPop(state.batch[i]);
    }
    state.live_ptrs.insert(state.live_ptrs.end(), state.batch.data(),
                           state.batch.data() + popped);
  }
};

struct Shuffle {
  template <typename Sink>
  friend void AbslStringify(Sink& sink, const Shuffle& s) {
    absl::Format(&sink, "Shuffle{}");
  }

  void Perform(State& state) const {
    std::shuffle(state.live_ptrs.begin(), state.live_ptrs.end(), state.rng);
  }
};

// Pushes objects into the Span using the ObjIdx interface.
struct Dealloc {
  uint8_t count;

  template <typename Sink>
  friend void AbslStringify(Sink& sink, const Dealloc& d) {
    absl::Format(&sink, "Dealloc{.count=%v}", d.count);
  }

  void Perform(State& state) const {
    size_t n = std::min<size_t>(count, state.num_to_move);
    n = std::min(n, state.live_ptrs.size());
    if (n == 0) {
      return;
    }

    absl::Span<void*> ptrs =
        absl::MakeSpan(state.live_ptrs.data() + state.live_ptrs.size() - n, n);
    for (void* p : ptrs) {
      state.ValidateAndRecordPush(p);
    }

    Span::ObjIdx idx[kMaxObjectsToMove];
    Span::ObjectsToIdx(ptrs, state.span.get(), state.object_size,
                       state.size_reciprocal, idx, state.bitmap_mode);

    const bool span_nonempty = state.span->FreelistPushBatch(
        absl::MakeSpan(idx).subspan(0, ptrs.size()), state.object_size,
        state.bitmap_mode);
    state.live_ptrs.resize(state.live_ptrs.size() - n);
    // FreelistPushBatch returns true while allocated objects remain in the
    // span, and false when the last allocated objects are returned (leaving
    // live_ptrs empty).
    TC_CHECK_EQ(span_nonempty, !state.live_ptrs.empty());
    if (!span_nonempty) {
      // When all allocated objects in the span are returned, FreelistPushBatch
      // returns false without pushing the batch onto the freelist or
      // decrementing allocated_ (in CentralFreeList, the empty span would be
      // deallocated to the page heap). Rebuild the freelist so subsequent
      // Alloc instructions in FuzzSpanInstructions can reuse state.span.
      TC_CHECK_EQ(
          state.span->BuildFreelist(state.object_size, state.objects_per_span,
                                    {}, state.bitmap_mode),
          0);
    }
  }
};

struct SetBitpackedAttributes {
  uint8_t nonempty_index;

  template <typename Sink>
  friend void AbslStringify(Sink& sink, const SetBitpackedAttributes& s) {
    absl::Format(&sink, "SetBitpackedAttributes{.nonempty_index=%v}",
                 s.nonempty_index);
  }

  void Perform(State& state) const {
    EXPECT_EQ(state.nonempty_index, state.span->nonempty_index());
    state.nonempty_index = nonempty_index % (1 << Span::kNonemptyIndexBits);
    state.span->set_nonempty_index(state.nonempty_index);
    EXPECT_EQ(state.span->nonempty_index(), state.nonempty_index);
  }
};

struct Prefetch {
  template <typename Sink>
  friend void AbslStringify(Sink& sink, const Prefetch& p) {
    absl::Format(&sink, "Prefetch{}");
  }

  void Perform(State& state) const { state.span->Prefetch(); }
};

using Instruction =
    std::variant<Alloc, Shuffle, Dealloc, SetBitpackedAttributes, Prefetch>;

template <typename Sink>
void AbslStringify(Sink& sink, const Instruction& i) {
  std::visit([&](const auto& arg) { absl::Format(&sink, "%v", arg); }, i);
}

void FuzzSpanInstructions(size_t object_size_direct, Length num_pages_direct,
                          uint8_t num_objects_to_move,
                          const std::vector<Instruction>& instructions,
                          bool use_bitmap_for_small) {
#if ABSL_HAVE_HWADDRESS_SANITIZER
  GTEST_SKIP()
      << "Skipping under HWASan, which uses the top bits of the pointer.";
#endif

  // Truncate ranges to better explore state space.
  const size_t object_size =
      std::max(sizeof(void*), (object_size_direct % kMaxSize) &
                                  ~(static_cast<size_t>(kAlignment) - 1u));
  const size_t num_pages = 1 + (num_pages_direct.raw_num() % 64);
  const size_t num_to_move = 1 + (num_objects_to_move % kMaxObjectsToMove);

  if (!SizeMap::IsValidSizeClass(object_size, Length(num_pages), num_to_move)) {
    return;
  }

  State state(use_bitmap_for_small, object_size, Length(num_pages),
              num_to_move);

  for (const auto& instruction : instructions) {
    std::visit([&](const auto& arg) { arg.Perform(state); }, instruction);
    TC_CHECK_EQ(state.span->Allocated(), state.live_ptrs.size());
  }
}

TEST(SpanTest, FuzzSpanInstructionsFullDealloc) {
  const std::vector<Instruction> instructions = {
      Instruction{Alloc{64}},   Instruction{Dealloc{64}},
      Instruction{Alloc{64}},   Instruction{Alloc{64}},
      Instruction{Shuffle{}},   Instruction{Dealloc{32}},
      Instruction{Dealloc{64}}, Instruction{Dealloc{64}},
      Instruction{Alloc{64}},
  };
  for (size_t size : {8, 16, 24, 64, 128, 512, 4096}) {
    for (bool use_bitmap_for_small : {false, true}) {
      FuzzSpanInstructions(size, Length(0), 63, instructions,
                           use_bitmap_for_small);
    }
  }
}

FUZZ_TEST(SpanTest, FuzzSpanInstructions)
    .WithDomains(fuzztest::Arbitrary<size_t>(), AnyLength(),
                 fuzztest::Arbitrary<uint8_t>(),
                 fuzztest::Arbitrary<std::vector<Instruction>>(),
                 fuzztest::Arbitrary<bool>());

void FuzzSpan(size_t object_size, Length num_pages, size_t num_to_move,
              size_t initial_objects_at_build, bool use_bitmap_for_small) {
#if ABSL_HAVE_HWADDRESS_SANITIZER
  GTEST_SKIP()
      << "Skipping under HWASan, which uses the top bits of the pointer.";
#endif

  if (!SizeMap::IsValidSizeClass(object_size, num_pages, num_to_move)) {
    // Invalid size class configuration, but ValidSizeClass detected that.
    return;
  }

  const auto pages = num_pages;
  const auto [objects_per_span, bitmap_mode] =
      Span::CalcBitmapInfo(use_bitmap_for_small, pages, object_size);
  const size_t initial_objects =
      std::min<size_t>(objects_per_span, initial_objects_at_build);
  const uint32_t size_reciprocal = Span::CalcReciprocal(object_size);

  void* mem;
  int res = posix_memalign(&mem, kPageSize, pages.in_bytes());
  TC_CHECK_EQ(res, 0);

  // Heap allocated, despite not being moved, to aid sanitizers in detecting
  // out-of-bound accesses.
  auto span = std::make_unique<Span>(PageIdContaining(mem));

  std::vector<void*> ptrs;
  ptrs.resize(initial_objects);

  TC_CHECK_EQ(span->BuildFreelist(object_size, objects_per_span,
                                  absl::MakeSpan(ptrs), bitmap_mode),
              initial_objects);
  TC_CHECK_EQ(span->Allocated(), initial_objects);

  ptrs.reserve(objects_per_span);
  while (ptrs.size() < objects_per_span) {
    size_t want = std::min(num_to_move, objects_per_span - ptrs.size());
    TC_CHECK_GT(want, 0);
    void* batch[kMaxObjectsToMove];
    TC_CHECK(!span->FreelistEmpty(objects_per_span));
    size_t n = span->FreelistPopBatch(absl::MakeSpan(batch, want), object_size,
                                      objects_per_span, bitmap_mode);

    TC_CHECK_GT(n, 0);
    TC_CHECK_LE(n, want);
    TC_CHECK_LE(n, kMaxObjectsToMove);
    ptrs.insert(ptrs.end(), batch, batch + n);
  }

  TC_CHECK(span->FreelistEmpty(objects_per_span));
  TC_CHECK_EQ(ptrs.size(), objects_per_span);
  TC_CHECK_EQ(ptrs.size(), span->Allocated());

  for (size_t i = 0, popped = ptrs.size(); i < popped;) {
    const size_t batch_len = std::min(num_to_move, popped - i);
    Span::ObjIdx idx[kMaxObjectsToMove];
    Span::ObjectsToIdx({&ptrs[i], batch_len}, span.get(), object_size,
                       size_reciprocal, idx, bitmap_mode);
    bool ok = span->FreelistPushBatch(absl::MakeSpan(idx, batch_len),
                                      object_size, bitmap_mode);
    i += batch_len;
    TC_CHECK_EQ(ok, i < popped);
    // If the freelist becomes full, then the span does not actually push the
    // element onto the freelist.
    //
    // When all objects are returned in a single batch (popped <= num_to_move),
    // the freelist stays "empty" as a result.
    TC_CHECK(popped <= num_to_move || !span->FreelistEmpty(objects_per_span));
    if (ok) {
      TC_CHECK_EQ(span->Allocated(), popped - i);
    }
  }

  free(mem);
}

TEST(SpanTest, Regression1) { FuzzSpan(2560, Length(40), 6, 16, false); }

TEST(SpanTest, Fuzz6321706670620672) {
  FuzzSpan(262144, Length(32), 32, 1, false);
}

TEST(SpanTest, Crash01d72a40d5815461b92d3f7c0f6377fd441b0034) {
  FuzzSpan(2560, Length(0), 9, 16, false);
}

TEST(SpanTest, Crash32697afd59029eb8356fee8ba568e7f6b58d728f) {
  FuzzSpan(2560, Length(24), 6, 16, false);
}

TEST(SpanTest, Crash42b80edf9551d1095aebb6724c070ee43d490125) {
  FuzzSpan(2560, Length(18), 0, 16, false);
}

TEST(SpanTest, Crash500955af6568b0ed234bd40d6a01af496ba15eb2) {
  FuzzSpan(2560, Length(18), 6, 16, false);
}

TEST(SpanTest, Crash6ef2b6ae2246d1bda0190983b1007df2699e7738) {
  FuzzSpan(41984, Length(2), 39, 60, false);
}

TEST(SpanTest, Crash746940d0368bfe3e4a94b60659eeb6cb87106618) {
  FuzzSpan(0, Length(1), 0, 1, false);
}

TEST(SpanTest, Testcase5877384059617280) {
  FuzzSpan(8, Length(1), 8, 1024, false);
}

FUZZ_TEST(SpanTest, FuzzSpan)
    .WithDomains(fuzztest::InRange<size_t>(0, kMaxSize), AnyLength(),
                 fuzztest::Arbitrary<size_t>(), fuzztest::Arbitrary<size_t>(),
                 fuzztest::Arbitrary<bool>());

}  // namespace
}  // namespace tcmalloc::tcmalloc_internal
