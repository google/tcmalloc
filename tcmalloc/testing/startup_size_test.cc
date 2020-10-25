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

#include <stddef.h>
#include <sys/mman.h>

#include <map>
#include <string>
#include <utility>

#include "absl/base/internal/sysinfo.h"
#include "gtest/gtest.h"
#include "tcmalloc/internal/logging.h"
#include "tcmalloc/malloc_extension.h"

namespace tcmalloc {
namespace {

typedef std::map<std::string, MallocExtension::Property> PropertyMap;

static size_t Property(const PropertyMap& map, const char* name) {
  const PropertyMap::const_iterator iter = map.find(name);
  if (iter == map.end()) {
    tcmalloc::Crash(tcmalloc::kCrash, __FILE__, __LINE__, "name not found",
                    name);
  }
  return iter->second.value;
}

TEST(StartupSizeTest, Basic) {
  static const size_t MiB = 1024 * 1024;
  PropertyMap map = MallocExtension::GetProperties();
  ASSERT_NE(map.count("tcmalloc.metadata_bytes"), 0)
      << "couldn't run - no tcmalloc data. Check your malloc configuration.";
  size_t percpu = Property(map, "tcmalloc.cpu_free");
#ifdef __powerpc64__
  size_t metadata_limit = 35 * MiB;
#else
  size_t metadata_limit = 10.1 * MiB;
#endif
  // Check whether per-cpu is active
  if (percpu > 0) {
    // Account for 256KiB per cpu slab
    metadata_limit += absl::base_internal::NumCPUs() * 0.25 * MiB;
  }
  size_t meta = Property(map, "tcmalloc.metadata_bytes");
  size_t physical = Property(map, "generic.physical_memory_used");
  EXPECT_LE(meta, metadata_limit);
  // Allow 20% more total physical memory than the virtual memory
  // reserved for the metadata.
  EXPECT_LE(physical, metadata_limit * 1.2);
}

}  // namespace
}  // namespace tcmalloc
