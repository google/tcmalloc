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
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "fuzztest/fuzztest.h"
#include "absl/log/check.h"
#include "absl/strings/str_format.h"
#include "tcmalloc/common.h"
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

constexpr size_t kNumObjectsToMove =
    TransferCache::Manager::num_objects_to_move(1);

struct State {
  TransferCacheEnv env;
  // Reference model of TransferCache::low_water_mark_: the minimum of `used`
  // observed since the last TryPlunder.
  size_t low_water_mark = 0;
  // Object misses already returned by FetchCommitIntervalMisses.
  size_t committed_object_misses = 0;

  State() = default;

  ~State() {
    CheckInvariants();
    Drain();
    CheckInvariants();
    CHECK_EQ(env.transfer_cache().tc_length(), 0);
  }

  TransferCacheStats GetStats() { return env.transfer_cache().GetStats(); }

  void CheckInvariants() {
    const TransferCacheStats stats = GetStats();
    CHECK_GE(stats.used, 0);
    CHECK_LE(stats.used, stats.capacity);
    CHECK_LE(stats.capacity, stats.max_capacity);
    CHECK_EQ(stats.used, env.transfer_cache().tc_length());
    CHECK_EQ(stats.max_capacity, env.transfer_cache().max_capacity());
    CHECK_EQ(env.transfer_cache().HasSpareCapacity(kSizeClass),
             stats.capacity - stats.used >= kNumObjectsToMove);
    CHECK_EQ(env.transfer_cache().CanIncreaseCapacity(kSizeClass),
             stats.max_capacity - stats.capacity >= kNumObjectsToMove);

    // The low water mark only ever decreases between plunders.
    low_water_mark = std::min(low_water_mark, stats.used);
  }

  void Drain() { env.Drain(); }
};

// Checks that the hit/miss counters not touched by an operation are unchanged.
void ExpectInsertStatsUnchanged(const TransferCacheStats& before,
                                const TransferCacheStats& after) {
  CHECK_EQ(after.insert_hits, before.insert_hits);
  CHECK_EQ(after.insert_misses, before.insert_misses);
  CHECK_EQ(after.insert_object_misses, before.insert_object_misses);
}

void ExpectRemoveStatsUnchanged(const TransferCacheStats& before,
                                const TransferCacheStats& after) {
  CHECK_EQ(after.remove_hits, before.remove_hits);
  CHECK_EQ(after.remove_object_hits, before.remove_object_hits);
  CHECK_EQ(after.remove_misses, before.remove_misses);
  CHECK_EQ(after.remove_object_misses, before.remove_object_misses);
}

struct Grow {
  template <typename Sink>
  friend void AbslStringify(Sink& sink, const Grow& g) {
    absl::Format(&sink, "Grow{}");
  }

  void Perform(State& state) const {
    const TransferCacheStats before = state.GetStats();
    // Confirm that we are always able to grow the cache provided we
    // have sufficient capacity to grow.
    const bool expected =
        before.capacity + kNumObjectsToMove <= before.max_capacity;
    CHECK_EQ(state.env.Grow(), expected);

    const TransferCacheStats after = state.GetStats();
    CHECK_EQ(after.capacity,
             before.capacity + (expected ? kNumObjectsToMove : 0));
    CHECK_EQ(after.used, before.used);
    ExpectInsertStatsUnchanged(before, after);
    ExpectRemoveStatsUnchanged(before, after);
  }
};

struct Shrink {
  template <typename Sink>
  friend void AbslStringify(Sink& sink, const Shrink& s) {
    absl::Format(&sink, "Shrink{}");
  }

  void Perform(State& state) const {
    const TransferCacheStats before = state.GetStats();
    // Confirm that we are always able to shrink the cache provided we
    // have sufficient capacity to shrink.
    const bool expected = before.capacity > kNumObjectsToMove;
    CHECK_EQ(state.env.Shrink(), expected);

    // Shrinking evicts only the objects that no longer fit in the reduced
    // capacity; evictions go to the freelist without counting as misses.
    const TransferCacheStats after = state.GetStats();
    if (expected) {
      CHECK_EQ(after.capacity, before.capacity - kNumObjectsToMove);
      CHECK_EQ(after.used, std::min(before.used, after.capacity));
    } else {
      CHECK_EQ(after.capacity, before.capacity);
      CHECK_EQ(after.used, before.used);
    }
    ExpectInsertStatsUnchanged(before, after);
    ExpectRemoveStatsUnchanged(before, after);
  }
};

struct TryPlunder {
  template <typename Sink>
  friend void AbslStringify(Sink& sink, const TryPlunder& t) {
    absl::Format(&sink, "TryPlunder{}");
  }

  void Perform(State& state) const {
    const TransferCacheStats before = state.GetStats();
    state.env.TryPlunder();

    // Plundering returns exactly the objects that sat unused since the last
    // plunder (the low water mark) and resets the mark to the new occupancy.
    const TransferCacheStats after = state.GetStats();
    CHECK_EQ(after.used, before.used - state.low_water_mark);
    CHECK_EQ(after.capacity, before.capacity);
    ExpectInsertStatsUnchanged(before, after);
    ExpectRemoveStatsUnchanged(before, after);
    state.low_water_mark = after.used;
  }
};

struct GetStats {
  template <typename Sink>
  friend void AbslStringify(Sink& sink, const GetStats& g) {
    absl::Format(&sink, "GetStats{}");
  }

  void Perform(State& state) const {
    const TransferCacheStats stats = state.GetStats();
    CHECK_GE(stats.used, 0);
    CHECK_LE(stats.used, stats.capacity);
    CHECK_LE(stats.capacity, stats.max_capacity);
  }
};

struct CommitMisses {
  template <typename Sink>
  friend void AbslStringify(Sink& sink, const CommitMisses& c) {
    absl::Format(&sink, "CommitMisses{}");
  }

  void Perform(State& state) const {
    const TransferCacheStats stats = state.GetStats();
    const size_t total =
        stats.insert_object_misses + stats.remove_object_misses;
    // Each object miss is reported by exactly one interval.
    CHECK_EQ(state.env.transfer_cache().FetchCommitIntervalMisses(),
             total - state.committed_object_misses);
    state.committed_object_misses = total;
    CHECK_EQ(state.env.transfer_cache().FetchCommitIntervalMisses(), 0);
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
    const size_t n = batch;
    const TransferCacheStats before = state.GetStats();
    state.env.Insert(batch);

    // Objects are cached up to the spare capacity; any remainder is a single
    // miss that spills the rest of the batch to the freelist.
    const TransferCacheStats after = state.GetStats();
    const size_t cached = std::min(n, before.capacity - before.used);
    CHECK_EQ(after.used, before.used + cached);
    CHECK_EQ(after.capacity, before.capacity);
    CHECK_EQ(after.insert_hits - before.insert_hits,
             static_cast<size_t>(cached > 0));
    CHECK_EQ(after.insert_misses - before.insert_misses,
             static_cast<size_t>(cached < n));
    CHECK_EQ(after.insert_object_misses - before.insert_object_misses,
             n - cached);
    ExpectRemoveStatsUnchanged(before, after);
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
    const size_t n = batch;
    const TransferCacheStats before = state.GetStats();
    state.env.Remove(batch);

    // Cached objects are served first as a single hit; the environment then
    // fetches any shortfall from the (now empty) cache, which is one miss.
    const TransferCacheStats after = state.GetStats();
    const size_t from_cache = std::min(n, before.used);
    CHECK_EQ(after.used, before.used - from_cache);
    CHECK_EQ(after.capacity, before.capacity);
    CHECK_EQ(after.remove_hits - before.remove_hits,
             static_cast<size_t>(from_cache > 0));
    CHECK_EQ(after.remove_object_hits - before.remove_object_hits, from_cache);
    CHECK_EQ(after.remove_misses - before.remove_misses,
             static_cast<size_t>(from_cache < n));
    CHECK_EQ(after.remove_object_misses - before.remove_object_misses,
             n - from_cache);
    ExpectInsertStatsUnchanged(before, after);
  }
};

using Instruction = std::variant<Grow, Shrink, TryPlunder, GetStats,
                                 CommitMisses, Insert, Remove>;

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
      fuzztest::Map([](CommitMisses c) { return Instruction{c}; },
                    fuzztest::Arbitrary<CommitMisses>()),
      fuzztest::Map([](int batch) { return Instruction{Insert{batch}}; },
                    fuzztest::InRange<int>(0, kNumObjectsToMove)),
      fuzztest::Map([](int batch) { return Instruction{Remove{batch}}; },
                    fuzztest::InRange<int>(0, kNumObjectsToMove)));
}

FUZZ_TEST(TransferCacheTest, FuzzTransferCache)
    .WithDomains(fuzztest::VectorOf(GetInstructionDomain()));

}  // namespace
}  // namespace tcmalloc::tcmalloc_internal
GOOGLE_MALLOC_SECTION_END
