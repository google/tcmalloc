// Copyright 2025 The TCMalloc Authors
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

#include "tcmalloc/alloc_at_least.h"

#include <cstddef>
#include <cstdlib>

#include "absl/base/attributes.h"

extern "C" ABSL_ATTRIBUTE_WEAK alloc_result_t alloc_at_least(size_t min_size)
    ALLOC_AT_LEAST_NOEXCEPT {
  alloc_result_t result;
  result.ptr = std::malloc(min_size);
  result.size = result.ptr != nullptr ? min_size : 0;
  return result;
}

extern "C" ABSL_ATTRIBUTE_WEAK alloc_result_t aligned_alloc_at_least(
    size_t alignment, size_t min_size) ALLOC_AT_LEAST_NOEXCEPT {
  alloc_result_t result;
  result.ptr = std::aligned_alloc(alignment, min_size);
  result.size = result.ptr != nullptr ? min_size : 0;
  return result;
}
