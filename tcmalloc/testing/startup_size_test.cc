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
//
// Test that the memory used by tcmalloc after the first few malloc
// calls is below a known limit to make sure no huge regression in
// startup size occurs due to a change.
//
// We intentionally do not measure RSS since that is very noisy.  For
// example, if the physical memory is not fragmented much, touching a
// single byte might map in a 2MB huge page instead of 4K, which will
// cause wide variations in RSS measurements based on environmental
// conditions.

#include <errno.h>
#include <stddef.h>
#include <sys/mman.h>

#include <map>
#include <string>
#include <utility>

#include "gtest/gtest.h"
#include "tcmalloc/internal/logging.h"
#include "tcmalloc/internal/sysinfo.h"
#include "tcmalloc/malloc_extension.h"

namespace tcmalloc {
namespace {

static size_t Property(const MallocExtension::PropertyMap& map,
                       const char* name) {
  const auto iter = map.find(name);
  TC_CHECK(iter != map.end(), "name=%s", name);
  return iter->second.value;
}

TEST(StartupSizeTest, Basic) {

  static const size_t MiB = 1024 * 1024;
  const auto map = MallocExtension::GetProperties();
  ASSERT_NE(map.count("tcmalloc.metadata_bytes"), 0)
      << "couldn't run - no tcmalloc data. Check your malloc configuration.";
  bool percpu_active = false;
  const auto percpu_active_iter = map.find("tcmalloc.per_cpu_caches_active");
  if (percpu_active_iter != map.end() && percpu_active_iter->second.value > 0) {
    percpu_active = true;
  }

  bool partitioning_active = false;
  const auto partitioning_active_iter =
      map.find("tcmalloc.security_partitioning_active");
  if (partitioning_active_iter != map.end() &&
      partitioning_active_iter->second.value > 0) {
    partitioning_active = true;
  }

  bool sharded_cache_active = false;
  const auto sharded_cache_active_iter =
      map.find("tcmalloc.experiment.TEST_ONLY_TCMALLOC_SHARDED_TRANSFER_CACHE");
  if (sharded_cache_active_iter != map.end() &&
      sharded_cache_active_iter->second.value > 0) {
    sharded_cache_active = true;
  }

#ifdef __powerpc64__
  size_t metadata_limit = 36.5 * MiB;
#else
  size_t metadata_limit = 28 * MiB;
#endif

  if (partitioning_active) {
    // Extra HugePageAwareAllocator (~815 KiB) and some slots
    metadata_limit += 1 * MiB;
  }

  // Check whether per-cpu is active
  size_t upper_percpu_limit = 0;
  if (percpu_active) {
    // Slabs: 32 KiB per CPU.
    // ResizeInfo: ~5.3 KiB per CPU.
    size_t bytes_per_cpu = 32 * 1024 + 5500;

    if (sharded_cache_active) {
      // Shards: up to NumCPUs / 8 shards, each ~320 KiB.
      // So on average ~40 KiB per CPU.
      bytes_per_cpu += 40 * 1024;
    }

    upper_percpu_limit = tcmalloc_internal::NumCPUs() * bytes_per_cpu;
  }
  size_t meta = Property(map, "tcmalloc.metadata_bytes");
  size_t physical = Property(map, "generic.physical_memory_used");
  EXPECT_LE(meta, metadata_limit + upper_percpu_limit);
  // Allow 20% more total physical memory than the virtual memory
  // reserved for the metadata.
  EXPECT_LE(physical, (metadata_limit + upper_percpu_limit) * 1.2);
}

}  // namespace
}  // namespace tcmalloc
