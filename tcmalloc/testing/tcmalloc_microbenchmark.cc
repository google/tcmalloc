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

#include "tcmalloc/testing/tcmalloc_microbenchmark.h"

#include <malloc.h>
#include <sys/stat.h>

#include <cstddef>
#include <cstdlib>
#include <optional>
#include <random>
#include <vector>

#include "absl/log/check.h"
#include "third_party/benchmark/include/benchmark/benchmark.h"
#include "tcmalloc/malloc_extension.h"

namespace tcmalloc {
namespace {

size_t GetProp(absl::string_view name) {
  std::optional<size_t> x =
      ::tcmalloc::MallocExtension::GetNumericProperty(name);
  // If we are running under a sanitizer, we may not get every property exposed
  // to us.
#if !(defined(ABSL_HAVE_ADDRESS_SANITIZER) || \
      defined(ABSL_HAVE_MEMORY_SANITIZER) ||  \
      defined(ABSL_HAVE_THREAD_SANITIZER) ||  \
      defined(UNDEFINED_BEHAVIOR_SANITIZER))
  CHECK(x.has_value());
#endif
  return x.value_or(0);
}

void RecordStateCounters(benchmark::State &state) {
  // Collects interested counters
  // NOTE: All counters below are estimations of per-iteration and per-thread.
  size_t in_use = GetProp("generic.current_allocated_bytes");
  size_t local = GetProp("tcmalloc.local_bytes");
  size_t pageheap = GetProp("tcmalloc.pageheap_free_bytes");
  size_t released = GetProp("tcmalloc.pageheap_unmapped_bytes");
  size_t waste = GetProp("tcmalloc.external_fragmentation_bytes");
  size_t central = waste - local - pageheap;
  state.counters["AllocatedBytes"] = benchmark::Counter(
      in_use, benchmark::Counter::kDefaults, benchmark::Counter::OneK::kIs1024);
  state.counters["WasteTotal"] = benchmark::Counter(
      waste, benchmark::Counter::kDefaults, benchmark::Counter::OneK::kIs1024);
  state.counters["WasteLocal"] = benchmark::Counter(
      local, benchmark::Counter::kDefaults, benchmark::Counter::OneK::kIs1024);
  state.counters["WasteCentral"] =
      benchmark::Counter(central, benchmark::Counter::kDefaults,
                         benchmark::Counter::OneK::kIs1024);
  state.counters["WastePageheap"] =
      benchmark::Counter(pageheap, benchmark::Counter::kDefaults,
                         benchmark::Counter::OneK::kIs1024);
  state.counters["SpaceReleased"] =
      benchmark::Counter(released, benchmark::Counter::kDefaults,
                         benchmark::Counter::OneK::kIs1024);
}

static void BM_MallocThenFree(benchmark::State &state) {
  const size_t kSize = state.range(0);

  for (auto s : state) {
    void *ptr = malloc(kSize);
    benchmark::DoNotOptimize(ptr);
    free(ptr);
  }

  RecordStateCounters(state);
}
BENCHMARK(BM_MallocThenFree)->Range(1, 1 << 20);

static void BM_Throughput(benchmark::State &state) {
  size_t size = 32;

  for (auto s : state) {
    for (int i = 0; i < kIterations; ++i) {
      void *p = malloc(size);
      benchmark::DoNotOptimize(p);
      free(p);
      // This makes next iteration use different free list. So
      // subsequent iterations may actually overlap in time.
      size = (size & 511) + 16;
    }
  }

  RecordStateCounters(state);
}
BENCHMARK(BM_Throughput);

void BM_ThroughputSmall(benchmark::State &state) {
  size_t size = 32;

  for (auto s : state) {
    for (int i = 0; i < kIterations; ++i) {
      void *p = malloc(size);
      benchmark::DoNotOptimize(p);
      free(p);
      // This makes next iteration use different free list. So
      // subsequent iterations may actually overlap in time.
      size = (size & 127) + 32;
    }
  }

  RecordStateCounters(state);
}
BENCHMARK(BM_ThroughputSmall);

void BM_SizedDelete(benchmark::State &state) {
  const std::vector<size_t> sizes{32, 64, 96, 128, 160, 192, 224, 256};

  for (auto s : state) {
    for (const auto &size : sizes) {
      void *ptr = ::operator new(size);
      ::operator delete(ptr, size);
    }
  }

  RecordStateCounters(state);
}
BENCHMARK(BM_SizedDelete);

std::vector<size_t> &GetSizes() {
  static std::vector<size_t> *sizes = new std::vector<size_t>(kIterations);
  return *sizes;
}

std::vector<void *> &GetBlocks() {
  static std::vector<void *> *blocks = new std::vector<void *>(kIterations);
  return *blocks;
}

std::vector<size_t> &GetFreeIndices() {
  static std::vector<size_t> *free_indices =
      new std::vector<size_t>(kIterations);
  return *free_indices;
}

void BM_Gauss_Setup(const benchmark::State &state) {
  std::vector<size_t> &sizes = GetSizes();
  std::vector<size_t> &free_indices = GetFreeIndices();

  std::random_device rd;
  std::default_random_engine re(rd());

  std::vector<double> intervals{16, 64, 256, 512};
  std::vector<double> weights{9, 0, 1};
  std::piecewise_constant_distribution<> dist(intervals.begin(),
                                              intervals.end(), weights.begin());
  for (size_t i = 0; i < kIterations; ++i) {
    sizes[i] = dist(re);
  }

  std::default_random_engine re2(rd());
  std::uniform_int_distribution<size_t> dist2(0, kIterations - 1);
  for (size_t i = 0; i < kIterations; ++i) {
    free_indices[i] = static_cast<size_t>(dist2(re2)) % (i + 1);
  }
}

static void BM_Gauss(benchmark::State &state) {
  std::vector<size_t> &sizes = GetSizes();
  std::vector<void *> &blocks = GetBlocks();

  for (auto s : state) {
    for (size_t i = 0; i < kIterations; ++i) {
      void *p = malloc(sizes[i]);
      benchmark::DoNotOptimize(p);
      if (!p) {
        abort();
      }
      blocks[i] = p;
    }
  }

  RecordStateCounters(state);
}
BENCHMARK(BM_Gauss)->Setup(BM_Gauss_Setup);

static void BM_GaussFree(benchmark::State &state) {
  std::vector<size_t> &sizes = GetSizes();
  std::vector<void *> &blocks = GetBlocks();
  std::vector<size_t> &free_indices = GetFreeIndices();

  for (auto s : state) {
    for (size_t i = 0; i < kIterations; ++i) {
      void *p = malloc(sizes[i]);
      benchmark::DoNotOptimize(p);
      if (!p) {
        abort();
      }
      blocks[i] = p;

      size_t to_free = free_indices[i];
      assert(to_free <= i);
      if (blocks[to_free] != nullptr) {
        free(blocks[to_free]);
        blocks[to_free] = nullptr;
      }
    }
  }

  RecordStateCounters(state);
}
BENCHMARK(BM_GaussFree)->Setup(BM_Gauss_Setup);

}  // namespace
}  // namespace tcmalloc
