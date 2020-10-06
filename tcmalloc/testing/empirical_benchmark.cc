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

#include <algorithm>
#include <new>
#include <string>
#include <tuple>

#include "benchmark/benchmark.h"
#include "tcmalloc/testing/empirical.h"
#include "tcmalloc/testing/testutil.h"

namespace tcmalloc {
namespace {

void *alloc(size_t s) { return ::operator new(s); }

void BM_EmpiricalTrivial(benchmark::State &state) {
  std::vector<EmpiricalData::Entry> triv = {{1024, 1, 1}, {2048, 1, 1}};
  EmpiricalData d(0, triv, 1024 * 1024 * 1024, alloc, sized_delete);

  for (auto s : state) {
    d.Next();
  }
}

BENCHMARK(BM_EmpiricalTrivial);

}  // namespace
}  // namespace tcmalloc
