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

#include <stdlib.h>

#include <vector>

#include "benchmark/benchmark.h"

int main(int argc, char* argv[]) {
  benchmark::MaybeReenterWithoutASLR(argc, argv);

  // Default to a short dry run (10 iterations) when running under bazel
  // test or ctest (TEST_TMPDIR is set).
  std::vector<char*> args;
  char min_time_arg[] = "--benchmark_min_time=10x";
  if (getenv("TEST_TMPDIR") != nullptr) {
    args.reserve(argc + 2);
    args.push_back(argv[0]);
    args.push_back(min_time_arg);
    args.insert(args.end(), argv + 1, argv + argc);
    args.push_back(nullptr);
    argc = static_cast<int>(args.size()) - 1;
    argv = args.data();
  }

  // Consume benchmark-specific flags first so InitGoogle does not reject them.
  benchmark::Initialize(&argc, argv);

  if (benchmark::ReportUnrecognizedArguments(argc, argv)) return 1;

  benchmark::RunSpecifiedBenchmarks();
  benchmark::Shutdown();
  return 0;
}
