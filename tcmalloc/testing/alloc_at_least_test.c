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

#include <errno.h>
#include <stdalign.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char** argv) {
  int exit_code = EXIT_SUCCESS;

  alloc_result_t result = alloc_at_least(127);
  if (result.ptr == NULL || result.size < 127) {
    exit_code = EXIT_FAILURE;
  }
  if (result.ptr != NULL) {
    memset(result.ptr, 0x42, result.size);
    free(result.ptr);
  }

  const size_t alignments[] = {1, 2, 4, 8, 16, 32, 64,
                               alignof(max_align_t) * 2};
  const size_t sizes[] = {1, 3, 5, 7, 15, 31, 63, 127};
  const size_t num_alignments = sizeof(alignments) / sizeof(alignments[0]);
  const size_t num_sizes = sizeof(sizes) / sizeof(sizes[0]);

  for (size_t i = 0; i < num_alignments; ++i) {
    for (size_t j = 0; j < num_sizes; ++j) {
      size_t align = alignments[i];
      size_t size = sizes[j];
      result = aligned_alloc_at_least(align, size);
      if (result.ptr == NULL || result.size < size) {
        exit_code = EXIT_FAILURE;
      }
      if (result.ptr != NULL) {
        if (((uintptr_t)result.ptr & (align - 1)) != 0) {
          exit_code = EXIT_FAILURE;
        }
        memset(result.ptr, 0x42, result.size);
        free(result.ptr);
      }
    }
  }

  const size_t invalid_alignments[] = {0, 3, 5, 6, 7, 9, 10, 12};
  const size_t num_invalid =
      sizeof(invalid_alignments) / sizeof(invalid_alignments[0]);
  for (size_t i = 0; i < num_invalid; ++i) {
    errno = 0;
    result = aligned_alloc_at_least(invalid_alignments[i], 64);
    if (result.ptr != NULL || result.size != 0 || errno != EINVAL) {
      exit_code = EXIT_FAILURE;
    }
  }

  errno = 0;
  result = aligned_alloc_at_least(8, SIZE_MAX - 1);
  if (result.ptr != NULL || result.size != 0 || errno != ENOMEM) {
    exit_code = EXIT_FAILURE;
  }

  return exit_code;
}
