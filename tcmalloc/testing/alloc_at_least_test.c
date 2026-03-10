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

// Simple source file which verifies `tcmalloc/alloc_at_least.h` is compatible
// with C.

#include "tcmalloc/alloc_at_least.h"

#include <stdalign.h>
#include <stddef.h>
#include <stdlib.h>

int main(int argc, char** argv) {
  alloc_result_t result = alloc_at_least(127);
  if (result.ptr != NULL) {
    free(result.ptr);
  }
  result = aligned_alloc_at_least(alignof(max_align_t) * 2,
                                  alignof(max_align_t) * 4);
  if (result.ptr != NULL) {
    free(result.ptr);
  }
  return EXIT_SUCCESS;
}
