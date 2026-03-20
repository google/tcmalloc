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

#ifndef TCMALLOC_INTERNAL_MEMORY_STATS_H_
#define TCMALLOC_INTERNAL_MEMORY_STATS_H_

#include <sys/types.h>

#include <cstddef>
#include <cstdint>

#include "absl/base/macros.h"
#include "absl/functional/function_ref.h"
#include "tcmalloc/internal/config.h"

GOOGLE_MALLOC_SECTION_BEGIN
namespace tcmalloc {
namespace tcmalloc_internal {

struct MemoryStats {
  int64_t vss;
  int64_t rss;
  int64_t shared;
  int64_t code;
  int64_t data;
};

// Memory stats of a process
bool GetMemoryStats(MemoryStats& stats);

ABSL_DEPRECATE_AND_INLINE()
inline bool GetMemoryStats(MemoryStats* stats) {
  return GetMemoryStats(*stats);
}

// For testing
bool GetMemoryStatsFromCallback(
    MemoryStats& stats,
    absl::FunctionRef<ssize_t(char* buf, size_t count)> read);

}  // namespace tcmalloc_internal
}  // namespace tcmalloc
GOOGLE_MALLOC_SECTION_END

#endif  // TCMALLOC_INTERNAL_MEMORY_STATS_H_
