// Copyright 2026 The TCMalloc Authors
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
#include "gtest/gtest.h"

namespace tcmalloc {
namespace {

TEST(Benchmarks, RunAll) {
  char arg0[] = "benchmark";
  char arg1[] = "--benchmark_min_time=10x";
  char* argv[] = {arg0, arg1, nullptr};
  int argc = 2;
  benchmark::Initialize(&argc, argv);
  benchmark::RunSpecifiedBenchmarks("all");
}

}  // namespace
}  // namespace tcmalloc
