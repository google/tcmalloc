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
      if (kSizeClasses[i].size >= SizeMap::kMinAllocSizeForCold) {
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

        if (Span::IsNonIntrusive(cold_candidate)) {
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
              size_map.SizeClass(CppPolicy().AccessAsHot(), request_size) +
                  (tc_globals.numa_topology().GetCurrentPartition() == 0
                       ? kExpandedClassesStart
                       : kNumBaseClasses));
  }
  EXPECT_THAT(size_map.ColdSizeClasses(),
              ElementsAreArray(expected_cold_size_classes));
}

TEST_P(ColdSizeClassTest, VerifyAllocationFullRange) {
  if (kPageShift <= 12) {
    GTEST_SKIP() << "cold size classes are not activated on the small page";
  }

  SizeMap size_map;
  size_map.Init(kSizeClasses, use_extended_size_class_for_cold());

  size_t size_before_min_alloc_for_cold = 0;
  if (use_extended_size_class_for_cold()) {
    auto it = std::lower_bound(kSizeClasses.begin(), kSizeClasses.end(),
                               SizeMap::kMinAllocSizeForCold,
                               [](const SizeClassInfo& lhs, const size_t rhs) {
                                 return lhs.size < rhs;
                               });
    ASSERT_NE(it, kSizeClasses.begin());
    size_before_min_alloc_for_cold = (--it)->size;
  } else {
    static constexpr size_t kColdCandidates[] = {
        2048,  4096,  6144,  7168,  8192,   16384,
        20480, 32768, 40960, 65536, 131072, 262144,
    };
    for (const size_t cold_candidate : kColdCandidates) {
      // Due to a bug in the existing code, the first eligible size is never
      // used for cold, which makes it the size_before_min_alloc_for_cold.
      if (Span::IsNonIntrusive(cold_candidate)) {
        size_before_min_alloc_for_cold = cold_candidate;
        break;
      }
    }
  }

  // Confirm that small sizes are allocated as "hot".
  for (int request_size = 0; request_size <= size_before_min_alloc_for_cold;
       ++request_size) {
    // Cold allocation is not numa-aware. They always point to the first
    // partition.
    EXPECT_EQ(size_map.SizeClass(CppPolicy().AccessAsCold(), request_size),
              size_map.SizeClass(CppPolicy().AccessAsHot(), request_size) -
                  (tc_globals.numa_topology().GetCurrentPartition() == 0
                       ? 0
                       : kNumBaseClasses))
        << request_size;
  }

  // Confirm that large sizes are allocated as cold as requested.
  size_t max_size = kSizeClasses[kSizeClasses.size() - 1].size;
  for (int request_size = size_before_min_alloc_for_cold + 1;
       request_size <= max_size; ++request_size) {
    if (use_extended_size_class_for_cold()) {
      EXPECT_EQ(size_map.SizeClass(CppPolicy().AccessAsCold(), request_size),
                size_map.SizeClass(CppPolicy().AccessAsHot(), request_size) +
                    (tc_globals.numa_topology().GetCurrentPartition() == 0
                         ? kExpandedClassesStart
                         : kNumBaseClasses))
          << request_size;
    } else {
      // When using hardcoded cold size classes, the mapping may not be
      // continous. For example, in 8k page, size class 5376 will be used for
      // hot requests in (4736, 5376]. But size class 6144 (the immediate size
      // class after 5376) will be used for cold requests in the same range. So
      // the real mappping from hot to cold in this case should be
      // `hot_size_class_index` + 1 + `numa_offset`. The exact offset into size
      // class index depends on how many "holes" between the adjacent cold size
      // classes. For simplicity, we use greater-or-equal for comparison.
      EXPECT_GE(size_map.SizeClass(CppPolicy().AccessAsCold(), request_size),
                size_map.SizeClass(CppPolicy().AccessAsHot(), request_size) +
                    (tc_globals.numa_topology().GetCurrentPartition() == 0
                         ? kExpandedClassesStart
                         : kNumBaseClasses))
          << request_size;
    }
  }
}

}  // namespace tcmalloc::tcmalloc_internal
