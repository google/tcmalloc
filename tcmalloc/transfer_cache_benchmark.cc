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

#include "benchmark/benchmark.h"
#include "tcmalloc/central_freelist.h"
#include "tcmalloc/common.h"
#include "tcmalloc/mock_central_freelist.h"
#include "tcmalloc/mock_transfer_cache.h"
#include "tcmalloc/transfer_cache_internals.h"

namespace tcmalloc {
namespace {

using FakeTransferCacheEnv =
    FakeTransferCacheEnvironment<internal_transfer_cache::TransferCache<
        MinimalFakeCentralFreeList, FakeTransferCacheManager>>;

using FakeLockFreeTransferCacheEnv =
    FakeTransferCacheEnvironment<internal_transfer_cache::LockFreeTransferCache<
        MinimalFakeCentralFreeList, FakeTransferCacheManager>>;

template <typename Env>
void BM_CrossThreadDraining(benchmark::State& state) {
  using Manager = typename Env::Manager;
  using Cache = typename Env::TransferCache;
  const int kBatchSize = Env::kBatchSize;
  const int kMaxObjectsToMove = Env::kMaxObjectsToMove;
  void* batch[kMaxObjectsToMove];

  struct CrossThreadState {
    CrossThreadState() : m{}, c{Cache(&m), Cache(&m)} {
      c[0].Init(1);
      c[1].Init(1);
    }
    Manager m;
    Cache c[2];
  };
  static CrossThreadState* s = nullptr;
  if (state.thread_index == 0) {
    s = new CrossThreadState();
    for (int i = 0; i < Env::kInitialCapacityInBatches / 2; ++i) {
      for (int j : {0, 1}) {
        s->c[j].freelist().AllocateBatch(batch, kBatchSize);
        s->c[j].InsertRange(batch, kBatchSize);
      }
    }
  }

  int src = state.thread_index % 2;
  int dst = (src + 1) % 2;

  for (auto iter : state) {
    benchmark::DoNotOptimize(batch);
    s->c[src].RemoveRange(batch, kBatchSize);
    benchmark::DoNotOptimize(batch);
    s->c[dst].InsertRange(batch, kBatchSize);
    benchmark::DoNotOptimize(batch);
  }

  if (state.thread_index == 0) {
    delete s;
    s = nullptr;
  }
}

template <typename Env>
void BM_WithinThreadBalance(benchmark::State& state) {
  using Manager = typename Env::Manager;
  using Cache = typename Env::TransferCache;
  const int kBatchSize = Env::kBatchSize;
  const int kMaxObjectsToMove = Env::kMaxObjectsToMove;
  void* batch[kMaxObjectsToMove];

  struct CrossThreadState {
    CrossThreadState() : m(), c(&m) { c.Init(1); }
    Manager m;
    Cache c;
  };
  static CrossThreadState* s = nullptr;
  if (state.thread_index == 0) {
    s = new CrossThreadState();
    for (int i = 0; i < Env::kInitialCapacityInBatches / 2; ++i) {
      s->c.freelist().AllocateBatch(batch, kBatchSize);
      s->c.InsertRange(batch, kBatchSize);
    }
  }

  for (auto iter : state) {
    benchmark::DoNotOptimize(batch);
    s->c.RemoveRange(batch, kBatchSize);
    benchmark::DoNotOptimize(batch);
    s->c.InsertRange(batch, kBatchSize);
    benchmark::DoNotOptimize(batch);
  }

  if (state.thread_index == 0) {
    delete s;
    s = nullptr;
  }
}

void BM_CrossThreadDraining_TransferCache(benchmark::State& state) {
  BM_CrossThreadDraining<FakeTransferCacheEnv>(state);
}

void BM_CrossThreadDraining_LockFreeTransferCache(benchmark::State& state) {
  BM_CrossThreadDraining<FakeLockFreeTransferCacheEnv>(state);
}

void BM_WithinThreadBalance_TransferCache(benchmark::State& state) {
  BM_WithinThreadBalance<FakeTransferCacheEnv>(state);
}

void BM_WithinThreadBalance_LockFreeTransferCache(benchmark::State& state) {
  BM_WithinThreadBalance<FakeLockFreeTransferCacheEnv>(state);
}

BENCHMARK(BM_CrossThreadDraining_TransferCache)->ThreadRange(2, 128);
BENCHMARK(BM_CrossThreadDraining_LockFreeTransferCache)->ThreadRange(2, 128);
BENCHMARK(BM_WithinThreadBalance_TransferCache)->ThreadRange(1, 128);
BENCHMARK(BM_WithinThreadBalance_LockFreeTransferCache)->ThreadRange(1, 128);

}  // namespace
}  // namespace tcmalloc
