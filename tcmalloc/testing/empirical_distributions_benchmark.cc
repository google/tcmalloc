// Copyright 2019 The TCMalloc Authors
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

#include "absl/random/random.h"
#include "absl/strings/str_cat.h"
#include "benchmark/benchmark.h"
#include "tcmalloc/testing/empirical_distributions.h"
#include "tcmalloc/testing/testutil.h"

namespace tcmalloc {
namespace {

void* alloc(size_t s) { return ::operator new(s); }

// Not really a test, just summary stats for our distributions.

void BM_Real(benchmark::State& state) {
  const int mib = state.range(0);
  size_t bytes = static_cast<size_t>(mib) * 1024 * 1024;
  EmpiricalData data(12345, empirical_distributions::Merced(), bytes, alloc,
                     sized_delete);
  const size_t before = data.total_num_allocated();
  const size_t before_bytes = data.total_bytes_allocated();
  // Run benchmark loop under timer.
  for (auto s : state) {
    data.Next();
  }
  state.SetItemsProcessed(data.total_num_allocated() - before);
  state.SetBytesProcessed(data.total_bytes_allocated() - before_bytes);
}

BENCHMARK(BM_Real)->UseRealTime()->Range(64, 16 * 1024)->ThreadRange(1, 128);

void BM_Create(benchmark::State& state) {
  const int mib = state.range(0);
  const size_t bytes = static_cast<size_t>(mib) * 1024 * 1024;
  size_t total_items = 0;
  size_t total_bytes = 0;
  for (auto s : state) {
    EmpiricalData data(12345, empirical_distributions::Merced(), bytes, alloc,
                       sized_delete);
    total_items += data.total_num_allocated();
    total_bytes += data.total_bytes_allocated();
  }

  state.SetBytesProcessed(total_bytes);
  state.SetItemsProcessed(total_items);
}

BENCHMARK(BM_Create)->UseRealTime()->Range(64, 16 * 1024);

void BM_DistributionAdjustableSampler(benchmark::State& state) {
  std::vector<double> weights;
  for (const auto& e : empirical_distributions::Merced()) {
    weights.push_back(e.alloc_rate);
  }

  absl::BitGen rng;
  AdjustableSampler alloc(weights);
  // Run benchmark loop under timer.
  for (auto s : state) {
    benchmark::DoNotOptimize(alloc(rng));
  }
  state.SetItemsProcessed(state.iterations());
}

BENCHMARK(BM_DistributionAdjustableSampler);

}  // namespace
}  // namespace tcmalloc
