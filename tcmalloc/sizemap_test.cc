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

#include "tcmalloc/sizemap.h"

#include <cstddef>
#include <vector>

#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "tcmalloc/common.h"
#include "tcmalloc/size_class_info.h"
#include "tcmalloc/span.h"
#include "tcmalloc/static_vars.h"
#include "tcmalloc/tcmalloc_policy.h"

namespace tcmalloc::tcmalloc_internal {

using ::testing::ElementsAreArray;

class ColdSizeClassTest : public testing::TestWithParam<
                              bool /* use_extended_size_class_for_cold */> {
 public:
  bool use_extended_size_class_for_cold() { return GetParam(); }
};

INSTANTIATE_TEST_SUITE_P(TestSizeClassForColdAllocations, ColdSizeClassTest,
                         testing::Bool());

TEST_P(ColdSizeClassTest, ColdFeatureActivation) {
  if (kPageShift > 12) {
    ASSERT_TRUE(ColdFeatureActive());
  } else {
    ASSERT_TRUE(!ColdFeatureActive());
  }
}

TEST_P(ColdSizeClassTest, ColdSizeClasses) {
  if (kPageShift <= 12) {
    GTEST_SKIP() << "cold size classes are not activated on the small page";
  }

  std::vector<size_t> allowed_alloc_size;
  std::vector<size_t> expected_cold_size_classes;
  if (use_extended_size_class_for_cold()) {
    for (int i = 0; i < kSizeClasses.size(); ++i) {
      if (kSizeClasses[i].size >= kBitmapMinObjectSize &&
          kSizeClasses[i].size >= SizeMap::kMinAllocSizeForCold) {
        allowed_alloc_size.push_back(kSizeClasses[i].size);
        expected_cold_size_classes.push_back(i + kExpandedClassesStart);
      }
    }
  } else {
    static constexpr size_t kColdCandidates[] = {
        2048,  4096,  6144,  7168,  8192,   16384,
        20480, 32768, 40960, 65536, 131072, 262144,
    };
    bool first = true;
    for (const size_t cold_candidate : kColdCandidates) {
      for (int i = 0; i < kSizeClasses.size(); ++i) {
        if (kSizeClasses[i].size != cold_candidate) continue;

        if (kSizeClasses[i].pages * kPageSize / cold_candidate <=
            Span::kCacheSize) {
          // Due to a bug in the code, the smallest allowed size class is not
          // recorded in the final `class_array_`, but it's recorded in the
          // `cold_sizes_` array.
          if (first) {
            first = false;
          } else {
            allowed_alloc_size.push_back(cold_candidate);
          }
          expected_cold_size_classes.push_back(i + kExpandedClassesStart);
          break;
        }
      }
    }
  }

  SizeMap size_map;
  size_map.Init(kSizeClasses, use_extended_size_class_for_cold());
  for (const size_t request_size : allowed_alloc_size) {
    EXPECT_EQ(size_map.SizeClass(CppPolicy().AccessAsCold(), request_size),
              size_map.SizeClass(CppPolicy(), request_size) +
                  (tc_globals.numa_topology().GetCurrentPartition() == 0
                       ? kExpandedClassesStart
                       : kNumBaseClasses));
  }
  EXPECT_THAT(size_map.ColdSizeClasses(),
              ElementsAreArray(expected_cold_size_classes));
}

}  // namespace tcmalloc::tcmalloc_internal
