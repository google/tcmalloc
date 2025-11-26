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

#include <setjmp.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <new>
#include <random>
#include <type_traits>
#include <variant>
#include <vector>

#include "gtest/gtest.h"
#include "fuzztest/fuzztest.h"
#include "absl/types/span.h"
#include "tcmalloc/common.h"
#include "tcmalloc/internal/logging.h"
#include "tcmalloc/pages.h"
#include "tcmalloc/span.h"
#include "tcmalloc/testing/testutil.h"

namespace tcmalloc::tcmalloc_internal {
namespace {

struct Alloc {
  uint8_t count;

  template <typename Sink>
  friend void AbslStringify(Sink& sink, const Alloc& a) {
    absl::Format(&sink, "Alloc(%d)", a.count);
  }
};

struct Shuffle {
  template <typename Sink>
  friend void AbslStringify(Sink& sink, const Shuffle& s) {
    absl::Format(&sink, "Shuffle");
  }
};

struct Dealloc {
  uint8_t count;

  template <typename Sink>
  friend void AbslStringify(Sink& sink, const Dealloc& d) {
    absl::Format(&sink, "Dealloc(%d)", d.count);
  }
};

struct DeallocNoRemove {
  uint8_t count;

  template <typename Sink>
  friend void AbslStringify(Sink& sink, const DeallocNoRemove& d) {
    absl::Format(&sink, "DeallocNoRemove(%d)", d.count);
  }
};

using Instruction = std::variant<Alloc, Shuffle, Dealloc, DeallocNoRemove>;

void FuzzSpanInstructions(size_t object_size_direct, size_t num_pages_direct,
                          uint8_t num_objects_to_move,
                          std::vector<Instruction> instructions) {
  GTEST_SKIP() << "Skipping";
  std::vector<void*> live_ptrs;
  std::vector<void*> batch;
  std::mt19937 rng;

  LongJmpScope scope;
  if (setjmp(scope.buf_)) {
    return;
  }

  // Truncate ranges to better explore state space.
  const size_t object_size =
      std::max(sizeof(void*), (object_size_direct % kMaxSize) &
                                  ~(static_cast<size_t>(kAlignment) - 1u));
  const size_t num_pages = 1 + (num_pages_direct % 64);
  const size_t num_to_move = 1 + (num_objects_to_move % kMaxObjectsToMove);

  if (!SizeMap::IsValidSizeClass(object_size, num_pages, num_to_move)) {
    return;
  }

  const auto pages = Length(num_pages);
  const size_t objects_per_span = pages.in_bytes() / object_size;
  const uint32_t size_reciprocal = Span::CalcReciprocal(object_size);

  void* mem;
  int res = posix_memalign(&mem, kPageSize, pages.in_bytes());
  TC_CHECK_EQ(res, 0);

  auto span = std::make_unique<Span>(Range(PageIdContaining(mem), pages));

  TC_CHECK_EQ(span->BuildFreelist(object_size, objects_per_span, {},
                                  /*alloc_time=*/0),
              0);

  live_ptrs.reserve(objects_per_span);
  batch.resize(kMaxObjectsToMove);
  bool did_double_free = false;

  for (const auto& instruction : instructions) {
    std::visit(
        [&](auto&& arg) {
          using T = std::decay_t<decltype(arg)>;
          if constexpr (std::is_same_v<T, Alloc>) {
            size_t n = std::min<size_t>(arg.count, num_to_move);
            if (span->FreelistEmpty(object_size)) {
              n = 0;
            }
            if (n == 0) {
              return;
            }

            size_t popped = span->FreelistPopBatch(
                absl::MakeSpan(batch.data(), n), object_size);
            live_ptrs.insert(live_ptrs.end(), batch.data(),
                             batch.data() + popped);
          } else if constexpr (std::is_same_v<T, Shuffle>) {
            std::shuffle(live_ptrs.begin(), live_ptrs.end(), rng);
          } else if constexpr (std::is_same_v<T, Dealloc> ||
                               std::is_same_v<T, DeallocNoRemove>) {
            size_t n = std::min<size_t>(arg.count, num_to_move);
            n = std::min(n, live_ptrs.size());
            if (n == 0) {
              return;
            }

            (void)span->FreelistPushBatch(
                {live_ptrs.data() + live_ptrs.size() - n, n}, object_size,
                size_reciprocal);

            if constexpr (!std::is_same_v<T, DeallocNoRemove>) {
              live_ptrs.resize(live_ptrs.size() - n);
            } else {
              // double free: don't remove from live_ptrs
              did_double_free = true;

              // TODO(b/457842787): Detect the double free immediately.
            }
          }
        },
        instruction);
  }

  for (int i = 0; i < live_ptrs.size();) {
    size_t limit = std::min<size_t>(live_ptrs.size() - i, num_objects_to_move);

    (void)span->FreelistPushBatch({live_ptrs.data() + i, limit}, object_size,
                                  size_reciprocal);

    i += limit;
  }

  free(mem);

  // We expect to have crashed when draining `live_ptrs` if there was a double
  // free.
  EXPECT_FALSE(did_double_free);
}

FUZZ_TEST(SpanTest, FuzzSpanInstructions);

TEST(SpanTest, FuzzSpanInstructionsDoubleFreeDuringFullDrain) {
  FuzzSpanInstructions(0, 0, 1, {Alloc{1}, DeallocNoRemove{58}, Dealloc{3}});
}

void FuzzSpan(const std::string& s) {
  // Extract fuzz input into 6 integers.
  //
  // TODO(b/271282540): Strongly type input.
  size_t state[6];
  if (s.size() != sizeof(state)) {
    return;
  }
  memcpy(state, s.data(), sizeof(state));

  const size_t object_size = state[0];
  const size_t num_pages = state[1];
  const size_t num_to_move = state[2];

  if (!SizeMap::IsValidSizeClass(object_size, num_pages, num_to_move)) {
    // Invalid size class configuration, but ValidSizeClass detected that.
    return;
  }

  const auto pages = Length(num_pages);
  const size_t objects_per_span = pages.in_bytes() / object_size;
  const size_t initial_objects_at_build =
      std::min(objects_per_span, state[3] >> 4);

  // state[4] reserved.
  const uint64_t alloc_time = state[5];
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
    TC_CHECK(!span->FreelistEmpty(object_size));
    size_t n = span->FreelistPopBatch(absl::MakeSpan(batch, want), object_size);

    TC_CHECK_GT(n, 0);
    TC_CHECK_LE(n, want);
    TC_CHECK_LE(n, kMaxObjectsToMove);
    ptrs.insert(ptrs.end(), batch, batch + n);
  }

  TC_CHECK(span->FreelistEmpty(object_size));
  TC_CHECK_EQ(ptrs.size(), objects_per_span);
  TC_CHECK_EQ(ptrs.size(), span->Allocated());

  for (size_t i = 0, popped = ptrs.size(); i < popped; ++i) {
    bool ok =
        span->FreelistPushBatch({&ptrs[i], 1}, object_size, size_reciprocal);
    TC_CHECK_EQ(ok, i != popped - 1);
    // If the freelist becomes full, then the span does not actually push the
    // element onto the freelist.
    //
    // For single object spans, the freelist always stays "empty" as a result.
    TC_CHECK(popped == 1 || !span->FreelistEmpty(object_size));
  }

  TC_CHECK_EQ(span->AllocTime(), alloc_time);

  free(mem);
}

FUZZ_TEST(SpanTest, FuzzSpan)
    ;

}  // namespace
}  // namespace tcmalloc::tcmalloc_internal
