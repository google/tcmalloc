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

#ifndef TCMALLOC_TESTING_TCMALLOC_MICROBENCHMARK_H_
#define TCMALLOC_TESTING_TCMALLOC_MICROBENCHMARK_H_

#include <cstdlib>

#ifndef NATIVE_FACTOR
#define NATIVE_FACTOR 1
#endif

namespace tcmalloc {

// Defines the number of iterations the benchmark should run.
constexpr size_t kBaseIterations = 1000000;
constexpr size_t kIterations = kBaseIterations * NATIVE_FACTOR;

struct SizedPtr {
  void* ptr;
  size_t size;
};

}  // namespace tcmalloc
#endif  // TCMALLOC_TESTING_TCMALLOC_MICROBENCHMARK_H_
