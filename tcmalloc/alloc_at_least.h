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

#ifndef TCMALLOC_ALLOC_AT_LEAST_H_
#define TCMALLOC_ALLOC_AT_LEAST_H_

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(__cplusplus) && defined(__GLIBC__)
#define ALLOC_AT_LEAST_NOEXCEPT noexcept
#else
#define ALLOC_AT_LEAST_NOEXCEPT
#endif

typedef struct {
  void* ptr;
  size_t size;
} alloc_result_t;

alloc_result_t alloc_at_least(size_t min_size) ALLOC_AT_LEAST_NOEXCEPT;

alloc_result_t aligned_alloc_at_least(size_t alignment,
                                      size_t min_size) ALLOC_AT_LEAST_NOEXCEPT;

#ifdef __cplusplus
}
#endif

#endif  // TCMALLOC_ALLOC_AT_LEAST_H_
