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

#include <algorithm>
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
using ::testing::Pair;

TEST(ColdSizeClassTest, ColdFeatureActivation) {
  if (kPageShift > 12) {
    ASSERT_TRUE(ColdFeatureActive());
  } else {
    ASSERT_TRUE(!ColdFeatureActive());
  }
}

TEST(ColdSizeClassTest, ColdSizeClasses) {
  if (kPageShift <= 12) {
    GTEST_SKIP() << "cold size classes are not activated on the small page";
  }

  // TODO(ckennelly): Parameterize across size class sets.
  const auto& classes = kSizeClasses.classes;
  std::vector<size_t> allowed_alloc_size;
  std::vector<size_t> expected_cold_size_classes;
  for (int i = 1; i < classes.size(); ++i) {
    allowed_alloc_size.push_back(classes[i].size);
    expected_cold_size_classes.push_back(i + kExpandedClassesStart);
  }

  SizeMap size_map;
  EXPECT_TRUE(size_map.Init(classes));
  for (const size_t request_size : allowed_alloc_size) {
    EXPECT_EQ(size_map.SizeClass(CppPolicy().AccessAsCold(), request_size),
              size_map.SizeClass(CppPolicy().AccessAsHot(), request_size) +
                  (tc_globals.numa_topology().GetCurrentPartition() == 0
                       ? kExpandedClassesStart
                       : kNumBaseClasses));
  }
  EXPECT_THAT(size_map.ColdSizeClasses(),
              ElementsAreArray(expected_cold_size_classes));
}

TEST(ColdSizeClassTest, VerifyAllocationFullRange) {
  if (kPageShift <= 12) {
    GTEST_SKIP() << "cold size classes are not activated on the small page";
  }

  SizeMap size_map;
  const auto& classes = kSizeClasses.classes;
  EXPECT_TRUE(size_map.Init(classes));

  // Confirm that sizes are allocated as cold as requested.
  size_t max_size = classes[classes.size() - 1].size;
  for (int request_size = 1; request_size <= max_size; ++request_size) {
    EXPECT_EQ(size_map.SizeClass(CppPolicy().AccessAsCold(), request_size),
              size_map.SizeClass(CppPolicy().AccessAsHot(), request_size) +
                  (tc_globals.numa_topology().GetCurrentPartition() == 0
                       ? kExpandedClassesStart
                       : kNumBaseClasses))
        << request_size;
  }
}

TEST(SizeMapTest, ClassToSizeRange) {
  SizeMap size_map;
  const auto& classes = kSizeClasses.classes;
  EXPECT_TRUE(size_map.Init(classes));

  // A few simple ones to spot check.
  EXPECT_THAT(size_map.class_to_size_range(0), Pair(0, 0));
  EXPECT_THAT(size_map.class_to_size_range(1), Pair(1, 8));
  EXPECT_THAT(size_map.class_to_size_range(2), Pair(9, 16));
  if (kPageShift > 12) {
    // Check that the size ranges of the cold classes mirror the
    // base classes.
    EXPECT_THAT(size_map.class_to_size_range(kExpandedClassesStart),
                Pair(0, 0));
    EXPECT_THAT(size_map.class_to_size_range(kExpandedClassesStart + 1),
                Pair(1, 8));
    EXPECT_THAT(size_map.class_to_size_range(kExpandedClassesStart + 2),
                Pair(9, 16));
  }
}

}  // namespace tcmalloc::tcmalloc_internal
