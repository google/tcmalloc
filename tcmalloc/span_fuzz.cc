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
#include <variant>
#include <vector>

#include "gtest/gtest.h"
#include "fuzztest/fuzztest.h"
#include "absl/types/span.h"
#include "tcmalloc/common.h"
#include "tcmalloc/internal/config.h"
#include "tcmalloc/internal/logging.h"
#include "tcmalloc/internal/sampled_allocation.h"
#include "tcmalloc/pages.h"
#include "tcmalloc/sizemap.h"
#include "tcmalloc/span.h"

namespace tcmalloc::tcmalloc_internal {
namespace {

auto AnyLength() {
  return fuzztest::ConstructorOf<Length>(fuzztest::Arbitrary<size_t>());
}

auto AnyPositiveLength() {
  return fuzztest::ConstructorOf<Length>(
      fuzztest::InRange<size_t>(1, std::numeric_limits<size_t>::max()));
}

auto AnyPageId() {
  return fuzztest::ConstructorOf<PageId>(fuzztest::InRange<size_t>(
      1, (size_t{1} << (kAddressBits - kPageShift)) - 1));
}

struct State {
  size_t object_size;
  Length pages;
  size_t num_to_move;
  size_t objects_per_span;
  uint32_t size_reciprocal;
  void* mem = nullptr;
  std::unique_ptr<Span> span;
  std::vector<void*> live_ptrs;
  std::vector<void*> batch;
  std::mt19937 rng;

  State(size_t object_size, Length pages, size_t num_to_move)
      : object_size(object_size),
        pages(pages),
        num_to_move(num_to_move),
        objects_per_span(pages.in_bytes() / object_size),
        size_reciprocal(Span::CalcReciprocal(object_size)) {
    TC_CHECK_EQ(posix_memalign(&mem, kPageSize, pages.in_bytes()), 0);

    span = std::make_unique<Span>(Range(PageIdContaining(mem), pages));
    TC_CHECK_EQ(span->BuildFreelist(object_size, objects_per_span, {},
                                    /*alloc_time=*/0),
                0);

    live_ptrs.reserve(objects_per_span);
    batch.resize(kMaxObjectsToMove);
  }

  ~State() {
    for (size_t i = 0; i < live_ptrs.size();) {
      size_t limit = std::min<size_t>(live_ptrs.size() - i, num_to_move);
      (void)span->FreelistPushBatch(absl::MakeSpan(live_ptrs.data() + i, limit),
                                    object_size, size_reciprocal);
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
    if (state.span->FreelistEmpty(state.object_size, state.objects_per_span)) {
      return;
    }
    size_t n = std::min<size_t>(count, state.num_to_move);
    if (n == 0) {
      return;
    }

    size_t popped = state.span->FreelistPopBatch(
        absl::MakeSpan(state.batch.data(), n), state.object_size);
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
    (void)state.span->FreelistPushBatch(ptrs, state.object_size,
                                        state.size_reciprocal);
    state.live_ptrs.resize(state.live_ptrs.size() - n);
  }
};

// Pushes objects into the Span using the ObjIdx interface.
struct DeallocIndex {
  uint8_t count;

  template <typename Sink>
  friend void AbslStringify(Sink& sink, const DeallocIndex& d) {
    absl::Format(&sink, "DeallocIndex{.count=%v}", d.count);
  }

  void Perform(State& state) const {
    size_t n = std::min<size_t>(count, state.num_to_move);
    n = std::min(n, state.live_ptrs.size());
    if (n == 0) {
      return;
    }

    absl::Span<void*> ptrs =
        absl::MakeSpan(state.live_ptrs.data() + state.live_ptrs.size() - n, n);

    Span::ObjIdx idx[kMaxObjectsToMove];
    if (Span::UseBitmapForSize(state.object_size)) {
      for (size_t i = 0; i < ptrs.size(); ++i) {
        idx[i] = state.span->BitmapPtrToIdx(ptrs[i], state.object_size,
                                            state.size_reciprocal);
      }
    } else {
      for (size_t i = 0; i < ptrs.size(); ++i) {
        idx[i] = state.span->PtrToIdx(ptrs[i], state.object_size);
      }
    }

    (void)state.span->FreelistPushBatch(
        absl::MakeSpan(idx).subspan(0, ptrs.size()), state.object_size,
        state.size_reciprocal);
    state.live_ptrs.resize(state.live_ptrs.size() - n);
  }
};

using Instruction = std::variant<Alloc, Shuffle, Dealloc, DeallocIndex>;

template <typename Sink>
void AbslStringify(Sink& sink, const Instruction& i) {
  std::visit([&](const auto& arg) { absl::Format(&sink, "%v", arg); }, i);
}

void FuzzSpanInstructions(size_t object_size_direct, Length num_pages_direct,
                          uint8_t num_objects_to_move,
                          const std::vector<Instruction>& instructions) {
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

  State state(object_size, Length(num_pages), num_to_move);

  for (const auto& instruction : instructions) {
    std::visit([&](const auto& arg) { arg.Perform(state); }, instruction);
  }
}

FUZZ_TEST(SpanTest, FuzzSpanInstructions)
    .WithDomains(fuzztest::Arbitrary<size_t>(), AnyLength(),
                 fuzztest::Arbitrary<uint8_t>(),
                 fuzztest::Arbitrary<std::vector<Instruction>>());

void FuzzSpan(size_t object_size, Length num_pages, size_t num_to_move,
              size_t initial_objects_at_build, uint64_t alloc_time) {
#if ABSL_HAVE_HWADDRESS_SANITIZER
  GTEST_SKIP()
      << "Skipping under HWASan, which uses the top bits of the pointer.";
#endif

  if (!SizeMap::IsValidSizeClass(object_size, num_pages, num_to_move)) {
    // Invalid size class configuration, but ValidSizeClass detected that.
    return;
  }

  const auto pages = num_pages;
  const size_t objects_per_span = pages.in_bytes() / object_size;
  initial_objects_at_build =
      std::min(objects_per_span, initial_objects_at_build);
  const uint32_t size_reciprocal = Span::CalcReciprocal(object_size);

  void* mem;
  int res = posix_memalign(&mem, kPageSize, pages.in_bytes());
  TC_CHECK_EQ(res, 0);

  // Heap allocated, despite not being moved, to aid sanitizers in detecting
  // out-of-bound accesses.
  auto span = std::make_unique<Span>(Range(PageIdContaining(mem), pages));

  std::vector<void*> ptrs;
  ptrs.resize(initial_objects_at_build);

  TC_CHECK_EQ(span->BuildFreelist(object_size, objects_per_span,
                                  absl::MakeSpan(ptrs), alloc_time),
              initial_objects_at_build);
  TC_CHECK_EQ(span->Allocated(), initial_objects_at_build);

  ptrs.reserve(objects_per_span);
  while (ptrs.size() < objects_per_span) {
    size_t want = std::min(num_to_move, objects_per_span - ptrs.size());
    TC_CHECK_GT(want, 0);
    void* batch[kMaxObjectsToMove];
    TC_CHECK(!span->FreelistEmpty(object_size, objects_per_span));
    size_t n = span->FreelistPopBatch(absl::MakeSpan(batch, want), object_size);

    TC_CHECK_GT(n, 0);
    TC_CHECK_LE(n, want);
    TC_CHECK_LE(n, kMaxObjectsToMove);
    ptrs.insert(ptrs.end(), batch, batch + n);
  }

  TC_CHECK(span->FreelistEmpty(object_size, objects_per_span));
  TC_CHECK_EQ(ptrs.size(), objects_per_span);
  TC_CHECK_EQ(ptrs.size(), span->Allocated());

  for (size_t i = 0, popped = ptrs.size(); i < popped; ++i) {
    bool ok = span->FreelistPushBatch(absl::MakeSpan(&ptrs[i], 1), object_size,
                                      size_reciprocal);
    TC_CHECK_EQ(ok, i != popped - 1);
    // If the freelist becomes full, then the span does not actually push the
    // element onto the freelist.
    //
    // For single object spans, the freelist always stays "empty" as a result.
    TC_CHECK(popped == 1 ||
             !span->FreelistEmpty(object_size, objects_per_span));
  }

  // We bitpack alloc time and do not store the full value.  We are willing to
  // tolerate a small amount of imprecision in the least significant bits
  // because a few nanoseconds should not make or break any decisions we make
  // with it.
#ifdef TCMALLOC_INTERNAL_LEGACY_LOCKING
  constexpr uint64_t kMask = ~uint64_t{0x0};
#else
  constexpr uint64_t kMask = ~uint64_t{0xFF};
#endif
  TC_CHECK_EQ(span->AllocTime() & kMask, alloc_time & kMask);

  free(mem);
}

TEST(SpanTest, Regression1) { FuzzSpan(2560, Length(40), 6, 16, 0); }

TEST(SpanTest, Fuzz6321706670620672) { FuzzSpan(262144, Length(32), 32, 1, 0); }

TEST(SpanTest, Crash01d72a40d5815461b92d3f7c0f6377fd441b0034) {
  FuzzSpan(2560, Length(0), 9, 16, 0);
}

TEST(SpanTest, Crash32697afd59029eb8356fee8ba568e7f6b58d728f) {
  FuzzSpan(2560, Length(24), 6, 16, 0);
}

TEST(SpanTest, Crash42b80edf9551d1095aebb6724c070ee43d490125) {
  FuzzSpan(2560, Length(18), 0, 16, 0);
}

TEST(SpanTest, Crash500955af6568b0ed234bd40d6a01af496ba15eb2) {
  FuzzSpan(2560, Length(18), 6, 16, 0);
}

TEST(SpanTest, Crash6ef2b6ae2246d1bda0190983b1007df2699e7738) {
  FuzzSpan(41984, Length(2), 39, 60, 0);
}

TEST(SpanTest, Crash746940d0368bfe3e4a94b60659eeb6cb87106618) {
  FuzzSpan(0, Length(1), 0, 1, 0);
}

TEST(SpanTest, Testcase5877384059617280) {
  FuzzSpan(8, Length(1), 8, 1024, 13683181415406439436ull);
}

FUZZ_TEST(SpanTest, FuzzSpan)
    .WithDomains(fuzztest::InRange<size_t>(0, kMaxSize), AnyLength(),
                 fuzztest::Arbitrary<size_t>(), fuzztest::Arbitrary<size_t>(),
                 fuzztest::Arbitrary<uint64_t>());

void FuzzSpanSampling(PageId start, Length num_pages) {
  if (num_pages.raw_num() >=
      std::numeric_limits<size_t>::max() - start.index()) {
    GTEST_SKIP() << "Skipping overflow range";
  }

  // FuzzSpanSampling is a property-based test to ensure sampling does not
  // impact other parts of the span state.
  Span span(Range(start, num_pages));

  EXPECT_EQ(span.first_page(), start);
  EXPECT_EQ(span.num_pages(), num_pages);
  EXPECT_EQ(span.last_page() + Length(1), start + num_pages);
  EXPECT_FALSE(span.sampled());

  SampledAllocation alloc;

  span.Sample(&alloc);

  EXPECT_EQ(span.first_page(), start);
  EXPECT_EQ(span.num_pages(), num_pages);
  EXPECT_EQ(span.last_page() + Length(1), start + num_pages);
  EXPECT_TRUE(span.sampled());

  SampledAllocation* ptr = span.Unsample();

  EXPECT_EQ(ptr, &alloc);
  EXPECT_EQ(span.first_page(), start);
  EXPECT_EQ(span.num_pages(), num_pages);
  EXPECT_EQ(span.last_page() + Length(1), start + num_pages);
  EXPECT_FALSE(span.sampled());

  // Unsampling again should not produce the pointer again.
  ptr = span.Unsample();

  EXPECT_EQ(ptr, nullptr);
  EXPECT_EQ(span.first_page(), start);
  EXPECT_EQ(span.num_pages(), num_pages);
  EXPECT_EQ(span.last_page() + Length(1), start + num_pages);
  EXPECT_FALSE(span.sampled());
}

FUZZ_TEST(SpanTest, FuzzSpanSampling)
    .WithDomains(AnyPageId(), AnyPositiveLength());

TEST(SpanTest, FuzzSpanSamplingRegression) {
  FuzzSpanSampling(PageId(34359738367), Length(1));
  FuzzSpanSampling(PageId(1), Length(18446744073709551614ull));
}

}  // namespace
}  // namespace tcmalloc::tcmalloc_internal
