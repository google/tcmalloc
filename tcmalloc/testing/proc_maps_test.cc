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

#include "tcmalloc/internal/proc_maps.h"

#include <stdint.h>
#include <sys/types.h>

#include <string>
#include <vector>

#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "absl/container/flat_hash_set.h"
#include "absl/strings/match.h"
#include "tcmalloc/common.h"
#include "tcmalloc/internal/config.h"
#include "tcmalloc/malloc_extension.h"
#include "tcmalloc/static_vars.h"
#include "tcmalloc/testing/testutil.h"

namespace tcmalloc {
namespace tcmalloc_internal {
namespace {

using ::testing::AnyOf;
using ::testing::Contains;
using ::testing::IsSupersetOf;

TEST(ProcMapsTest, InspectMappings) {
  const bool heap_partitioning_active =
      tcmalloc::MallocExtension::GetNumericProperty(
          "tcmalloc.security_partitioning_active")
          .value_or(0);
  const bool numa_aware = tc_globals.numa_topology().numa_aware();

  std::vector<void*> ptrs;

  // Allocate something to ensure NORMAL region.
  {
    ScopedNeverSample never_sample;
    ptrs.push_back(::operator new(10 << 20));
  }

  {
    // Allocate something to ensure SAMPLED region.
    ScopedAlwaysSample always_sample;
    ptrs.push_back(::operator new(10 << 10));
  }

  if (ColdFeatureActive()) {
    // Allocate something to ensure COLD region.
    ScopedNeverSample never_sample;
    ptrs.push_back(::operator new(1 << 20, tcmalloc::hot_cold_t{0}));
  }

  ProcMapsIterator::Buffer buffer;
  ProcMapsIterator it(&buffer);
  ASSERT_TRUE(it.Valid());

  absl::flat_hash_set<std::string> tcmalloc_regions;

  uint64_t start, end, offset;
  int64_t inode;
  char* flags;
  char* filename;
  dev_t dev;

  while (it.NextExt(&start, &end, &flags, &offset, &inode, &filename, &dev)) {
    if (absl::StrContains(filename, "[anon:")) {
      tcmalloc_regions.insert(filename);
    }
  }

  // Clean up.
  for (void* ptr : ptrs) {
    ::operator delete(ptr);
  }

  // We include [anon:absl] as part of the bootstrap allocator for the
  // SystemAllocator.
  absl::flat_hash_set<std::string> expected = {
      "[anon:absl]",
      "[anon:tcmalloc_region_METADATA]",
      "[anon:tcmalloc_region_SAMPLED]",
  };

  const bool numa_or_partitioned =
      numa_aware || MallocExtension::GetNumericProperty(
                        "tcmalloc.security_partitioning_active")
                            .value_or(0) > 0;

  if (!numa_or_partitioned) {
    expected.insert("[anon:tcmalloc_region_NORMAL]");
  }

  if (ColdFeatureActive() && !heap_partitioning_active) {
    expected.insert("[anon:tcmalloc_region_COLD]");
  }

  if (kSanitizerPresent || !tcmalloc::NamedVMAsSupported()) {
    GTEST_SKIP()
        << "Skipping under sanitizers or if named VMAs are not supported";
  }

  EXPECT_THAT(tcmalloc_regions, IsSupersetOf(expected));
  if (numa_or_partitioned) {
    EXPECT_THAT(tcmalloc_regions,
                AnyOf(Contains("[anon:tcmalloc_region_NORMAL]"),
                      Contains("[anon:tcmalloc_region_NORMAL_P1]")));
  }
}

}  // namespace
}  // namespace tcmalloc_internal
}  // namespace tcmalloc
