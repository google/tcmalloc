// Copyright 2021 The TCMalloc Authors
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

// This is a trivial program.  When run with a virtual address size rlimit,
// TCMalloc should crash cleanly, rather than hang.

#if defined(__linux__)
#include <sys/resource.h>

#include "tcmalloc/internal/memory_stats.h"

// This runs after the process image has been loaded, so the new rlimit will
// apply only to any subsequent mmap() calls done through tcmalloc. Leave some
// headroom so RandomMmapHint's single-page seed mmap() succeeds while failing
// SystemAllocator's region reservations.
#if !defined(__clang__) && defined(__GNUC__)
#pragma GCC diagnostic ignored "-Wprio-ctor-dtor"
#endif
__attribute__((constructor(99))) void LowerRlimitAs() {
  tcmalloc::tcmalloc_internal::MemoryStats stats;
  if (tcmalloc::tcmalloc_internal::GetMemoryStats(stats)) {
    const rlim_t limit = stats.vss + (2 << 20) - 1;
    struct rlimit rlim = {limit, limit};
    setrlimit(RLIMIT_AS, &rlim);
  }
}
#endif  // __linux__

int main() { return 0; }
