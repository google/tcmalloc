// Copyright 2020 The TCMalloc Authors
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
#include <cstring>
#include <random>
#include <type_traits>
#include <variant>
#include <vector>

#include "fuzztest/fuzztest.h"
#include "absl/log/check.h"
#include "absl/strings/str_format.h"
#include "absl/time/time.h"
#include "absl/types/span.h"
#include "tcmalloc/central_freelist.h"
#include "tcmalloc/common.h"
#include "tcmalloc/internal/config.h"
#include "tcmalloc/internal/logging.h"
#include "tcmalloc/mock_static_forwarder.h"
#include "tcmalloc/pages.h"
#include "tcmalloc/sizemap.h"
#include "tcmalloc/span_stats.h"

GOOGLE_MALLOC_SECTION_BEGIN
namespace tcmalloc::tcmalloc_internal {
namespace {

using CentralFreeList = central_freelist_internal::CentralFreeList<
    tcmalloc_internal::MockStaticForwarder>;
using CentralFreelistEnv = FakeCentralFreeListEnvironment<CentralFreeList>;

auto AnyLength() {
  return fuzztest::ConstructorOf<Length>(fuzztest::Arbitrary<size_t>());
}

struct State {
  CentralFreelistEnv env;
  std::vector<void*> objects;

  State(size_t object_size, Length num_pages, size_t num_objects_to_move,
        central_freelist_internal::CflSubbucketPrioritization
            cfl_subbucket_prioritization)
      : env(object_size, Bytes(num_pages.in_bytes()), num_objects_to_move,
            cfl_subbucket_prioritization) {}

  ~State();

  void CheckInvariants();
};

void State::CheckInvariants() {
  if (env.objects_per_span() == 1) {
    return;
  }

  const tcmalloc_internal::SpanStats stats =
      env.central_freelist().GetSpanStats();
  TC_CHECK_EQ(env.central_freelist().length() + objects.size(),
              stats.obj_capacity);
  if (objects.empty()) {
    TC_CHECK_EQ(stats.num_live_spans(), 0);
  } else {
    TC_CHECK_GT(stats.num_live_spans(), 0);
  }
}

State::~State() {
  const size_t allocated = objects.size();
  size_t returned = 0;
  while (returned < allocated) {
    const size_t to_return =
        std::min(allocated - returned, static_cast<size_t>(kMaxObjectsToMove));
    env.central_freelist().InsertRange({&objects[returned], to_return});
    returned += to_return;
  }
  objects.clear();

  CheckInvariants();
}

struct Allocate {
  uint8_t num_objects;

  template <typename Sink>
  friend void AbslStringify(Sink& sink, const Allocate& a) {
    absl::Format(&sink, "Allocate{.num_objects=%v}", a.num_objects);
  }

  void Perform(State& state) const {
    void* batch[kMaxObjectsToMove];
    const size_t n = num_objects;
    int allocated =
        state.env.central_freelist().RemoveRange(absl::MakeSpan(batch, n));
    state.objects.insert(state.objects.end(), batch, batch + allocated);
  }
};

struct Deallocate {
  uint8_t num_objects;

  template <typename Sink>
  friend void AbslStringify(Sink& sink, const Deallocate& d) {
    absl::Format(&sink, "Deallocate{.num_objects=%v}", d.num_objects);
  }

  void Perform(State& state) const {
    if (state.objects.empty()) return;

    const size_t n = std::min<size_t>(num_objects, state.objects.size());
    state.env.central_freelist().InsertRange(
        {&state.objects[state.objects.size() - n], n});
    state.objects.resize(state.objects.size() - n);
  }
};

struct Shuffle {
  int seed;

  template <typename Sink>
  friend void AbslStringify(Sink& sink, const Shuffle& s) {
    absl::Format(&sink, "Shuffle{.seed=%v}", s.seed);
  }

  void Perform(State& state) const {
    std::mt19937 rng(seed);
    constexpr int kMaxToShuffle = 10 * kMaxObjectsToMove;
    if (state.objects.size() <= kMaxToShuffle) {
      std::shuffle(state.objects.begin(), state.objects.end(), rng);
    } else {
      std::shuffle(state.objects.end() - kMaxToShuffle, state.objects.end(),
                   rng);
    }
  }
};

struct CheckStats {
  template <typename Sink>
  friend void AbslStringify(Sink& sink, const CheckStats&) {
    absl::Format(&sink, "CheckStats{}");
  }

  void Perform(State& state) const { state.CheckInvariants(); }
};

struct PrintStats {
  template <typename Sink>
  friend void AbslStringify(Sink& sink, const PrintStats&) {
    absl::Format(&sink, "PrintStats{}");
  }

  void Perform(State& state) const {
    std::string s;
    s.resize(1 << 20);
    Printer p(&s[0], s.size());
    state.env.central_freelist().PrintSpanUtilStats(p);
    state.env.central_freelist().PrintSpanLifetimeStats(p);

    PbtxtRegion region(p, kTop);
    state.env.central_freelist().PrintSpanUtilStatsInPbtxt(region);
    state.env.central_freelist().PrintSpanLifetimeStatsInPbtxt(region);
  }
};

struct AdvanceClock {
  int32_t value;

  template <typename Sink>
  friend void AbslStringify(Sink& sink, const AdvanceClock& a) {
    absl::Format(&sink, "AdvanceClock{.value=%v}", a.value);
  }

  void Perform(State& state) const {
    state.env.forwarder().AdvanceClock(absl::Milliseconds(value));
  }
};

using Instruction = std::variant<Allocate, Deallocate, Shuffle, CheckStats,
                                 PrintStats, AdvanceClock>;

template <typename Sink>
void AbslStringify(Sink& sink, const Instruction& i) {
  std::visit([&](const auto& arg) { absl::Format(&sink, "%v", arg); }, i);
}

void FuzzCFL(size_t object_size, Length num_pages, size_t num_objects_to_move,
             const std::vector<Instruction>& instructions,
             central_freelist_internal::CflSubbucketPrioritization
                 cfl_subbucket_prioritization) {
  // TODO(271282540): Add support for multiple size classes for fuzzing.
  if (!SizeMap::IsValidSizeClass(object_size, num_pages, num_objects_to_move)) {
    return;
  }
  State state(object_size, num_pages, num_objects_to_move,
              cfl_subbucket_prioritization);

  for (const auto& instruction : instructions) {
    std::visit([&](const auto& arg) { arg.Perform(state); }, instruction);
    state.CheckInvariants();
  }
}

auto GetInstructionDomain() {
  return fuzztest::OneOf(
      fuzztest::Map([](uint8_t n) { return Instruction{Allocate{n}}; },
                    fuzztest::InRange<uint8_t>(1, kMaxObjectsToMove)),
      fuzztest::Map([](uint8_t n) { return Instruction{Deallocate{n}}; },
                    fuzztest::InRange<uint8_t>(1, kMaxObjectsToMove)),
      fuzztest::Map([](int s) { return Instruction{Shuffle{s}}; },
                    fuzztest::Arbitrary<int>()),
      fuzztest::Map([](CheckStats c) { return Instruction{c}; },
                    fuzztest::Arbitrary<CheckStats>()),
      fuzztest::Map([](PrintStats p) { return Instruction{p}; },
                    fuzztest::Arbitrary<PrintStats>()),
      fuzztest::Map([](int32_t v) { return Instruction{AdvanceClock{v}}; },
                    fuzztest::Arbitrary<int32_t>()));
}

FUZZ_TEST(CentralFreeListTest, FuzzCFL)
    .WithDomains(fuzztest::InRange<size_t>(0, kMaxSize), AnyLength(),
                 fuzztest::Arbitrary<size_t>(),
                 fuzztest::VectorOf(GetInstructionDomain()),
                 fuzztest::Arbitrary<
                     central_freelist_internal::CflSubbucketPrioritization>());

}  // namespace
}  // namespace tcmalloc::tcmalloc_internal
GOOGLE_MALLOC_SECTION_END
