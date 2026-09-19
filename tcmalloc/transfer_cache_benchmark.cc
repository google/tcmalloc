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

#include <array>
#include <cstddef>
#include <optional>
#include <vector>

#include "absl/random/distributions.h"
#include "absl/random/random.h"
#include "benchmark/benchmark.h"
#include "tcmalloc/internal/config.h"
#include "tcmalloc/mock_central_freelist.h"
#include "tcmalloc/mock_transfer_cache.h"
#include "tcmalloc/transfer_cache_internals.h"
#include "tcmalloc/transfer_cache_stats.h"

GOOGLE_MALLOC_SECTION_BEGIN
namespace tcmalloc {
namespace tcmalloc_internal {
namespace {

using TransferCacheWithRealCFLEnv =
    FakeTransferCacheEnvironment<internal_transfer_cache::TransferCache<
        RealCentralFreeListForTesting, FakeTransferCacheManager>>;
using TransferCacheEnv =
    FakeTransferCacheEnvironment<internal_transfer_cache::TransferCache<
        MinimalFakeCentralFreeList, FakeTransferCacheManager>>;
static constexpr int kSizeClass = 0;

template <typename Env>
void BM_CrossThread(benchmark::State& state) {
  using Cache = typename Env::TransferCache;
  const int kBatchSize = Env::kBatchSize;
  const int kMaxObjectsToMove = Env::kMaxObjectsToMove;
  void* batch[kMaxObjectsToMove];

  struct CrossThreadState {
    CrossThreadState() : m{}, c{Cache(&m, 1), Cache(&m, 1)} {}
    FakeTransferCacheManager m;
    Cache c[2];
  };

  static CrossThreadState* s = nullptr;
  if (state.thread_index() == 0) {
    s = new CrossThreadState();
    for (int i = 0; i < ::tcmalloc::tcmalloc_internal::internal_transfer_cache::
                                kInitialCapacityInBatches /
                            2;
         ++i) {
      for (Cache& c : s->c) {
        c.freelist().AllocateBatch({batch, kBatchSize});
        c.InsertRange(kSizeClass, {batch, kBatchSize});
      }
    }
  }

  int src = state.thread_index() % 2;
  int dst = (src + 1) % 2;
  for (auto iter : state) {
    benchmark::DoNotOptimize(batch);
    (void)s->c[src].RemoveRange(kSizeClass, {batch, kBatchSize});
    benchmark::DoNotOptimize(batch);
    s->c[dst].InsertRange(kSizeClass, {batch, kBatchSize});
    benchmark::DoNotOptimize(batch);
  }
  if (state.thread_index() == 0) {
    TransferCacheStats stats{};
    for (Cache& c : s->c) {
      TransferCacheStats other = c.GetStats();
      stats.insert_hits += other.insert_hits;
      stats.insert_misses += other.insert_misses;
      stats.remove_hits += other.remove_hits;
      stats.remove_misses += other.remove_misses;
    }

    state.counters["insert_hit_ratio"] =
        static_cast<double>(stats.insert_hits) /
        (stats.insert_hits + stats.insert_misses);
    state.counters["remove_hit_ratio"] =
        static_cast<double>(stats.remove_hits) /
        (stats.remove_hits + stats.remove_misses);
    delete s;
    s = nullptr;
  }
}

template <typename Env>
void BM_InsertRange(benchmark::State& state) {
  const int kBatchSize = Env::kBatchSize;
  const int kMaxObjectsToMove = Env::kMaxObjectsToMove;
  constexpr int kBatches = 16;

  Env e;
  void* batches[kBatches][kMaxObjectsToMove];
  for (int i = 0; i < kBatches; ++i) {
    e.central_freelist().AllocateBatch(
        {batches[i], static_cast<size_t>(kBatchSize)});
  }

  while (state.KeepRunningBatch(kBatches)) {
    for (int i = 0; i < kBatches; ++i) {
      e.transfer_cache().InsertRange(
          kSizeClass, {batches[i], static_cast<size_t>(kBatchSize)});
    }
    state.PauseTiming();
    for (int i = 0; i < kBatches; ++i) {
      (void)e.transfer_cache().RemoveRange(
          kSizeClass, {batches[i], static_cast<size_t>(kBatchSize)});
    }
    state.ResumeTiming();
  }
}

template <typename Env>
void BM_RemoveRange(benchmark::State& state) {
  const int kBatchSize = Env::kBatchSize;
  const int kMaxObjectsToMove = Env::kMaxObjectsToMove;
  constexpr int kBatches = 16;

  Env e;
  void* batches[kBatches][kMaxObjectsToMove];
  for (int i = 0; i < kBatches; ++i) {
    e.central_freelist().AllocateBatch(
        {batches[i], static_cast<size_t>(kBatchSize)});
  }

  while (state.KeepRunningBatch(kBatches)) {
    state.PauseTiming();
    for (int i = 0; i < kBatches; ++i) {
      e.transfer_cache().InsertRange(
          kSizeClass, {batches[i], static_cast<size_t>(kBatchSize)});
    }
    state.ResumeTiming();
    for (int i = 0; i < kBatches; ++i) {
      (void)e.transfer_cache().RemoveRange(
          kSizeClass, {batches[i], static_cast<size_t>(kBatchSize)});
      benchmark::DoNotOptimize(batches[i]);
    }
  }
}

template <typename Env>
void BM_RealisticBatchNonBatchMutations(benchmark::State& state) {
  const int kBatchSize = Env::kBatchSize;

  Env e;
  absl::BitGen gen;
  constexpr size_t kNumChoices = 4096;
  std::array<double, kNumChoices> choices;
  for (double& choice : choices) {
    choice = absl::Uniform(gen, 0.0, 1.0);
  }

  size_t idx = 0;
  for (auto iter : state) {
    const double choice = choices[idx++ % kNumChoices];

    // These numbers have been determined by looking at production data.
    if (choice < 0.424) {
      e.Insert(kBatchSize);
    } else if (choice < 0.471) {
      e.Insert(1);
    } else if (choice < 0.959) {
      e.Remove(kBatchSize);
    } else {
      e.Remove(1);
    }
  }

  const TransferCacheStats stats = e.transfer_cache().GetStats();
  state.counters["insert_hit_ratio"] =
      static_cast<double>(stats.insert_hits) /
      (stats.insert_hits + stats.insert_misses);
  state.counters["remove_hit_ratio"] =
      static_cast<double>(stats.remove_hits) /
      (stats.remove_hits + stats.remove_misses);
}

template <typename Env>
void BM_RealisticHitRate(benchmark::State& state) {
  const int kBatchSize = Env::kBatchSize;

  Env e;
  absl::BitGen gen;
  // We switch between insert-heavy and remove-heavy access pattern every 5k
  // iterations. kBias specifies the fraction of insert (or remove) operations
  // during insert-heavy (or remove-heavy) phase of the microbenchmark. These
  // constants have been determined through experimentation so that the
  // resulting insert and remove miss rate matches that of the production.
  constexpr int kInterval = 5000;
  constexpr double kBias = 0.85;

  struct Op {
    bool insert;
    int count;
  };
  std::vector<Op> ops;
  ops.reserve(2 * kInterval);
  for (bool insert_heavy : {true, false}) {
    for (int i = 0; i < kInterval; ++i) {
      const double partial = absl::Uniform(gen, 0.0, 1.0);
      const bool insert = absl::Bernoulli(gen, kBias) == insert_heavy;
      if (insert) {
        ops.push_back({true, partial < 0.65 ? kBatchSize : 1});
      } else {
        ops.push_back({false, partial < 0.99 ? kBatchSize : 1});
      }
    }
  }

  size_t idx = 0;
  for (auto iter : state) {
    const Op& op = ops[idx++ % ops.size()];
    if (op.insert) {
      e.Insert(op.count);
    } else {
      e.Remove(op.count);
    }
  }

  const TransferCacheStats stats = e.transfer_cache().GetStats();
  const size_t total_inserts = stats.insert_hits + stats.insert_misses;
  state.counters["insert_aggregate_miss_ratio"] =
      static_cast<double>(stats.insert_misses) / total_inserts;

  const size_t total_removes = stats.remove_hits + stats.remove_misses;
  state.counters["remove_aggregate_miss_ratio"] =
      static_cast<double>(stats.remove_misses) / total_removes;
}

BENCHMARK_TEMPLATE(BM_CrossThread, TransferCacheEnv)->ThreadRange(2, 64);
BENCHMARK_TEMPLATE(BM_InsertRange, TransferCacheEnv);
BENCHMARK_TEMPLATE(BM_RemoveRange, TransferCacheEnv);
BENCHMARK_TEMPLATE(BM_RealisticBatchNonBatchMutations, TransferCacheEnv);
BENCHMARK_TEMPLATE(BM_RealisticHitRate, TransferCacheEnv);
BENCHMARK_TEMPLATE(BM_RealisticHitRate, TransferCacheWithRealCFLEnv);

}  // namespace
}  // namespace tcmalloc_internal
}  // namespace tcmalloc
GOOGLE_MALLOC_SECTION_END
