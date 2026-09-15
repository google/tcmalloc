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

#include <variant>
#include <vector>

#include "fuzztest/fuzztest.h"
#include "absl/log/check.h"
#include "absl/strings/str_format.h"
#include "tcmalloc/internal/config.h"
#include "tcmalloc/mock_central_freelist.h"
#include "tcmalloc/mock_transfer_cache.h"
#include "tcmalloc/transfer_cache_internals.h"
#include "tcmalloc/transfer_cache_stats.h"

GOOGLE_MALLOC_SECTION_BEGIN
namespace tcmalloc::tcmalloc_internal {
namespace {

using TransferCache =
    internal_transfer_cache::TransferCache<MockCentralFreeList,
                                           FakeTransferCacheManager>;
using TransferCacheEnv = FakeTransferCacheEnvironment<TransferCache>;

constexpr int kNumObjectsToMove =
    TransferCache::Manager::num_objects_to_move(1);

struct State {
  TransferCacheEnv env;

  State() = default;

  ~State() {
    CheckInvariants();
    Drain();
    CheckInvariants();
    CHECK_EQ(env.transfer_cache().tc_length(), 0);
  }

  void CheckInvariants() {
    const TransferCacheStats stats = env.transfer_cache().GetStats();
    CHECK_GE(stats.used, 0);
    CHECK_LE(stats.used, stats.capacity);
    CHECK_LE(stats.capacity, stats.max_capacity);
    CHECK_EQ(stats.used, env.transfer_cache().tc_length());
    CHECK_EQ(stats.max_capacity, env.transfer_cache().max_capacity());
  }

  void Drain() { env.Drain(); }
};

struct Grow {
  template <typename Sink>
  friend void AbslStringify(Sink& sink, const Grow& g) {
    absl::Format(&sink, "Grow{}");
  }

  void Perform(State& state) const {
    const TransferCacheStats stats = state.env.transfer_cache().GetStats();
    // Confirm that we are always able to grow the cache provided we
    // have sufficient capacity to grow.
    const bool expected =
        stats.capacity + kNumObjectsToMove <= stats.max_capacity;
    CHECK_EQ(state.env.Grow(), expected);
  }
};

struct Shrink {
  template <typename Sink>
  friend void AbslStringify(Sink& sink, const Shrink& s) {
    absl::Format(&sink, "Shrink{}");
  }

  void Perform(State& state) const {
    const TransferCacheStats stats = state.env.transfer_cache().GetStats();
    // Confirm that we are always able to shrink the cache provided we
    // have sufficient capacity to shrink.
    const bool expected = stats.capacity > kNumObjectsToMove;
    CHECK_EQ(state.env.Shrink(), expected);
  }
};

struct TryPlunder {
  template <typename Sink>
  friend void AbslStringify(Sink& sink, const TryPlunder& t) {
    absl::Format(&sink, "TryPlunder{}");
  }

  void Perform(State& state) const { state.env.TryPlunder(); }
};

struct GetStats {
  template <typename Sink>
  friend void AbslStringify(Sink& sink, const GetStats& g) {
    absl::Format(&sink, "GetStats{}");
  }

  void Perform(State& state) const {
    const TransferCacheStats stats = state.env.transfer_cache().GetStats();
    CHECK_GE(stats.used, 0);
    CHECK_LE(stats.used, stats.capacity);
    CHECK_LE(stats.capacity, stats.max_capacity);
  }
};

struct Insert {
  int batch;

  template <typename Sink>
  friend void AbslStringify(Sink& sink, const Insert& i) {
    absl::Format(&sink, "Insert{.batch=%d}", i.batch);
  }

  void Perform(State& state) const {
    if (batch <= 0) {
      return;
    }
    state.env.Insert(batch);
  }
};

struct Remove {
  int batch;

  template <typename Sink>
  friend void AbslStringify(Sink& sink, const Remove& r) {
    absl::Format(&sink, "Remove{.batch=%d}", r.batch);
  }

  void Perform(State& state) const {
    if (batch <= 0) {
      return;
    }
    state.env.Remove(batch);
  }
};

using Instruction =
    std::variant<Grow, Shrink, TryPlunder, GetStats, Insert, Remove>;

void FuzzTransferCache(const std::vector<Instruction>& instructions) {
  // TODO(b/271282540): We should also add a capability to fuzz-test multiple
  // size classes.
  State state;
  for (const auto& instruction : instructions) {
    std::visit([&](const auto& instr) { instr.Perform(state); }, instruction);
    state.CheckInvariants();
  }
}

auto GetInstructionDomain() {
  return fuzztest::OneOf(
      fuzztest::Map([](Grow g) { return Instruction{g}; },
                    fuzztest::Arbitrary<Grow>()),
      fuzztest::Map([](Shrink s) { return Instruction{s}; },
                    fuzztest::Arbitrary<Shrink>()),
      fuzztest::Map([](TryPlunder t) { return Instruction{t}; },
                    fuzztest::Arbitrary<TryPlunder>()),
      fuzztest::Map([](GetStats g) { return Instruction{g}; },
                    fuzztest::Arbitrary<GetStats>()),
      fuzztest::Map([](int batch) { return Instruction{Insert{batch}}; },
                    fuzztest::InRange(0, kNumObjectsToMove)),
      fuzztest::Map([](int batch) { return Instruction{Remove{batch}}; },
                    fuzztest::InRange(0, kNumObjectsToMove)));
}

FUZZ_TEST(TransferCacheTest, FuzzTransferCache)
    .WithDomains(fuzztest::VectorOf(GetInstructionDomain()));

}  // namespace
}  // namespace tcmalloc::tcmalloc_internal
GOOGLE_MALLOC_SECTION_END
