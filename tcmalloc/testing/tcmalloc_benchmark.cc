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

#include <malloc.h>

#include <new>
#include <vector>

#include "absl/random/random.h"
#include "benchmark/benchmark.h"
#include "tcmalloc/internal/declarations.h"
#include "tcmalloc/malloc_extension.h"

namespace tcmalloc {
namespace {

static void BM_new_delete(benchmark::State& state) {
  const int arg = state.range(0);

  for (auto s : state) {
    void* ptr = ::operator new(arg);
    ::operator delete(ptr);
  }
}
BENCHMARK(BM_new_delete)->Range(1, 1 << 20);

static void BM_new_sized_delete(benchmark::State& state) {
  const int arg = state.range(0);

  for (auto s : state) {
    void* ptr = ::operator new(arg);
    ::operator delete(ptr, arg);
  }
}
BENCHMARK(BM_new_sized_delete)->Range(1, 1 << 20);

static void BM_size_returning_new_delete(benchmark::State& state) {
  const int arg = state.range(0);

  for (auto s : state) {
    sized_ptr_t res = tcmalloc_size_returning_operator_new(arg);
    ::operator delete(res.p, res.n);
  }
}
BENCHMARK(BM_size_returning_new_delete)->Range(1, 1 << 20);

static void BM_aligned_new(benchmark::State& state) {
  const int size = state.range(0);
  const int alignment = state.range(1);

  for (auto s : state) {
    void* ptr = operator new(size, static_cast<std::align_val_t>(alignment));
    operator delete(ptr, size, static_cast<std::align_val_t>(alignment));
  }
}
BENCHMARK(BM_aligned_new)->RangePair(1, 1 << 20, 8, 64);

static void* malloc_pages(size_t pages) {
#if defined(TCMALLOC_256K_PAGES)
  static const size_t kPageSize = 256 * 1024;
#elif defined(TCMALLOC_LARGE_PAGES)
  static const size_t kPageSize = 32 * 1024;
#elif defined(TCMALLOC_SMALL_BUT_SLOW)
  static const size_t kPageSize = 4096;
#else
  static const size_t kPageSize = 8192;
#endif
  size_t size = pages * kPageSize;
  void* ptr = memalign(kPageSize, size);
  return ptr;
}

static void BM_malloc_pages(benchmark::State& state) {
  const int arg = state.range(0);

  size_t pages = arg;
  for (auto s : state) {
    void* ptr = malloc_pages(pages);
    benchmark::DoNotOptimize(ptr);
    free(ptr);
  }
}

BENCHMARK(BM_malloc_pages)
    ->Range(1, 1024)
    ->Arg(511)
    ->Arg(513)
    ->Arg(1023)
    ->Arg(1 + 20 * 1024 * 1024 / (8 * 1024))
    ->Arg(256);

static void BM_random_malloc_pages(benchmark::State& state) {
  const int kMaxOnHeap = 5000;
  const int kMaxRequestSizePages = 127;

  // We don't want random number generation to be a large part of
  // what we measure, so create a table of numbers now.
  absl::BitGen rand;
  const int kRandomTableSize = 98765;
  std::vector<size_t> random_index(kRandomTableSize);
  std::vector<size_t> random_request_size(kRandomTableSize);
  for (int i = 0; i < kRandomTableSize; i++) {
    random_index[i] = absl::Uniform<int32_t>(rand, 0, kMaxOnHeap);
    random_request_size[i] =
        absl::Uniform<int32_t>(rand, 0, kMaxRequestSizePages) + 1;
  }
  void* v[kMaxOnHeap];
  memset(v, 0, sizeof(v));
  size_t r = 0;
  for (int i = 0; i < kMaxOnHeap; ++i) {
    v[i] = malloc_pages(random_request_size[r]);
    if (++r == kRandomTableSize) {
      r = 0;
    }
  }

  for (auto s : state) {
    size_t index = random_index[r];
    free(v[index]);
    v[index] = malloc_pages(random_request_size[r]);
    if (++r == kRandomTableSize) {
      r = 0;
    }
  }

  for (int j = 0; j < kMaxOnHeap; j++) {
    free(v[j]);
  }
}

BENCHMARK(BM_random_malloc_pages);

static void BM_random_new_delete(benchmark::State& state) {
  const int kMaxOnHeap = 5000;
  const int kMaxRequestSize = 5000;

  // We don't want random number generation to be a large part of
  // what we measure, so create a table of numbers now.
  absl::BitGen rand;
  const int kRandomTableSize = 98765;
  std::vector<int> random_index(kRandomTableSize);
  std::vector<int> random_request_size(kRandomTableSize);
  for (int i = 0; i < kRandomTableSize; i++) {
    random_index[i] = absl::Uniform<int32_t>(rand, 0, kMaxOnHeap);
    random_request_size[i] =
        absl::Uniform<int32_t>(rand, 0, kMaxRequestSize) + 1;
  }
  void* v[kMaxOnHeap];
  memset(v, 0, sizeof(v));

  int r = 0;
  for (auto s : state) {
    int index = random_index[r];
    ::operator delete(v[index]);
    v[index] = ::operator new(random_request_size[r]);
    if (++r == kRandomTableSize) {
      r = 0;
    }
  }
  for (int j = 0; j < kMaxOnHeap; j++) {
    ::operator delete(v[j]);
  }
}
BENCHMARK(BM_random_new_delete);

}  // namespace
}  // namespace tcmalloc
