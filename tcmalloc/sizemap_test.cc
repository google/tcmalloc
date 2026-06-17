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
#include "tcmalloc/internal/parameter_accessors.h"
#include "tcmalloc/internal/size_class_info.h"
#include "tcmalloc/malloc_extension.h"
#include "tcmalloc/parameters.h"
#include "tcmalloc/span.h"
#include "tcmalloc/static_vars.h"
#include "tcmalloc/tcmalloc_policy.h"

namespace tcmalloc::tcmalloc_internal {

using ::testing::ElementsAreArray;
using ::testing::Pair;

static void VerifyColdSizeClassRelations(const SizeMap& size_map,
                                         size_t request_size) {
  const HeapPartitioningMode mode = Parameters::heap_partitioning_mode();

  {
    auto policy = CppPolicy().InPartition(0).WithSecurityToken<TokenId{0}>();
    size_t cold = size_map.SizeClass(policy.AccessAsCold(), request_size);
    size_t hot = size_map.SizeClass(policy.AccessAsHot(), request_size);
    if (mode == HeapPartitioningMode::kLight) {
      // In kLight mode, all C++ allocations are routed to Hot P1 or Cold P0.
      EXPECT_EQ(cold, hot - kNumBaseClasses + kExpandedClassesStart)
          << request_size;
    } else {
      EXPECT_EQ(cold, hot + kExpandedClassesStart) << request_size;
    }
  }

  {
    auto policy =
        CppPolicy().InPartition(0).WithSecurityToken<TokenId::kAllocToken1>();
    size_t cold = size_map.SizeClass(policy.AccessAsCold(), request_size);
    size_t hot = size_map.SizeClass(policy.AccessAsHot(), request_size);
    if (mode == HeapPartitioningMode::kLight) {
      EXPECT_EQ(cold, hot - kNumBaseClasses + kExpandedClassesStart)
          << request_size;
    } else if (mode == HeapPartitioningMode::kFull) {
      // In kFull mode, alloc-token 1 Cold allocations map to Hot P1.
      EXPECT_EQ(cold, hot) << request_size;
    } else {
      EXPECT_EQ(cold, hot + kExpandedClassesStart) << request_size;
    }
  }

  if (kNumaPartitions > 1) {
    auto policy = CppPolicy().InPartition(1).WithSecurityToken<TokenId{0}>();
    size_t cold = size_map.SizeClass(policy.AccessAsCold(), request_size);
    size_t hot = size_map.SizeClass(policy.AccessAsHot(), request_size);
    EXPECT_EQ(cold, hot - kNumBaseClasses + kExpandedClassesStart)
        << request_size;
  }
}

TEST(ColdSizeClassTest, ColdFeatureActivation) {
  if (kPageShift > 12) {
    ASSERT_TRUE(ColdFeatureActive());
  } else {
    ASSERT_TRUE(!ColdFeatureActive());
  }
}

const SizeClasses* const kAllSizeClassesConfigs[] = {
    &kSizeClasses,
    &kLegacySizeClasses,
    &kReuseRelaxedBelow64SizeClasses,
    &kExperimentalPow2SizeClasses,
};

TEST(ColdSizeClassTest, ColdSizeClasses) {
  if (kPageShift <= 12) {
    GTEST_SKIP() << "cold size classes are not activated on the small page";
  }

  for (const SizeClasses* sc : kAllSizeClassesConfigs) {
    const auto& classes = sc->classes;
    std::vector<size_t> allowed_alloc_size;
    std::vector<size_t> expected_cold_size_classes;
    for (int i = 1; i < classes.size(); ++i) {
      allowed_alloc_size.push_back(classes[i].size);
      expected_cold_size_classes.push_back(i + kExpandedClassesStart);
    }

    SizeMap size_map;
    EXPECT_TRUE(size_map.Init(classes));
    for (const size_t request_size : allowed_alloc_size) {
      VerifyColdSizeClassRelations(size_map, request_size);
    }
    EXPECT_THAT(size_map.ColdSizeClasses(),
                ElementsAreArray(expected_cold_size_classes));
  }
}

TEST(ColdSizeClassTest, VerifyAllocationFullRange) {
  if (kPageShift <= 12) {
    GTEST_SKIP() << "cold size classes are not activated on the small page";
  }

  for (const SizeClasses* sc : kAllSizeClassesConfigs) {
    SizeMap size_map;
    const auto& classes = sc->classes;
    EXPECT_TRUE(size_map.Init(classes));

    // Confirm that sizes are allocated as cold as requested.
    size_t max_size = classes[classes.size() - 1].size;
    for (int request_size = 1; request_size <= max_size; ++request_size) {
      VerifyColdSizeClassRelations(size_map, request_size);
    }
  }
}

TEST(SizeMapTest, ClassToSizeRange) {
  for (const SizeClasses* sc : kAllSizeClassesConfigs) {
    SizeMap size_map;
    const auto& classes = sc->classes;
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
}

TEST(SizeMapTest, HeapPartitioning) {
  if (kSecurityPartitions == 1) {
    GTEST_SKIP() << "Heap partitioning is not compiled in.";
  }

  for (const SizeClasses* sc : kAllSizeClassesConfigs) {
    const auto& classes = sc->classes;

    SizeMap size_map;
    EXPECT_TRUE(size_map.Init(classes));

    size_t offset =
        Parameters::heap_partitioning_mode() == HeapPartitioningMode::kFull
            ? kNumBaseClasses
            : 0;

    for (size_t i = 1; i < classes.size(); ++i) {
      EXPECT_EQ(size_map.SizeClass(CppPolicy().WithSecurityToken<TokenId{0}>(),
                                   classes[i].size) +
                    offset,
                size_map.SizeClass(
                    CppPolicy().WithSecurityToken<TokenId::kAllocToken1>(),
                    classes[i].size));
    }
  }
}

TEST(SizeMapTest, PointerPartitionNoCold) {
  if (kPageShift <= 12) {
    GTEST_SKIP() << "cold size classes are not activated on the small page";
  }
  if (MallocExtension::GetNumericProperty(
          "tcmalloc.security_partitioning_active")
          .value_or(0) == 0) {
    GTEST_SKIP() << "security partition not active";
  }

  for (const SizeClasses* sc : kAllSizeClassesConfigs) {
    const auto& classes = sc->classes;
    std::vector<size_t> allowed_alloc_size;
    std::vector<size_t> expected_cold_size_classes;
    for (int i = 1; i < classes.size(); ++i) {
      allowed_alloc_size.push_back(classes[i].size);
      expected_cold_size_classes.push_back(i + kExpandedClassesStart);
    }

    SizeMap size_map;
    EXPECT_TRUE(size_map.Init(classes));
    for (const size_t request_size : allowed_alloc_size) {
      if (Parameters::heap_partitioning_mode() == HeapPartitioningMode::kFull) {
        EXPECT_EQ(
            size_map.SizeClass(CppPolicy()
                                   .WithSecurityToken<TokenId::kAllocToken1>()
                                   .AccessAsCold(),
                               request_size),
            size_map.SizeClass(CppPolicy()
                                   .WithSecurityToken<TokenId::kAllocToken1>()
                                   .AccessAsHot(),
                               request_size));
      } else {
        EXPECT_EQ(
            size_map.SizeClass(CppPolicy()
                                   .WithSecurityToken<TokenId::kAllocToken1>()
                                   .AccessAsCold(),
                               request_size),
            size_map.SizeClass(
                CppPolicy().WithSecurityToken<TokenId{0}>().AccessAsCold(),
                request_size));
      }
    }
    EXPECT_THAT(size_map.ColdSizeClasses(),
                ElementsAreArray(expected_cold_size_classes));
  }
}

TEST(SizeMapTest, HeapPartitioningSizeZero) {
  if (!tc_globals.multiple_non_numa_partitions()) {
    GTEST_SKIP() << "Heap partitioning not active";
  }

  for (const SizeClasses* sc : kAllSizeClassesConfigs) {
    const auto& classes = sc->classes;
    SizeMap size_map;
    EXPECT_TRUE(size_map.Init(classes));
    // Map Partition 0 Size 0.
    size_t base_c =
        size_map.SizeClass(CppPolicy().WithSecurityToken<TokenId{0}>(), 0);
    EXPECT_GT(base_c, 0);
    EXPECT_GT(size_map.class_to_size(base_c), 0);
    // Map Partition 1 Size 0.
    size_t part1_c = size_map.SizeClass(
        CppPolicy().WithSecurityToken<TokenId::kAllocToken1>(), 0);
    EXPECT_GE(part1_c, kNumBaseClasses)
        << "Size 0 must belong to Partition 1's range";
    EXPECT_NE(part1_c, kNumBaseClasses)
        << "Size 0 must not map to the dummy class";
    EXPECT_EQ(part1_c, base_c + (Parameters::heap_partitioning_mode() ==
                                         HeapPartitioningMode::kLight
                                     ? 0
                                     : kNumBaseClasses));
  }
}

TEST(SizeMapTest, SpecificClassRanges) {
  // Verify kReuseRelaxedBelow64SizeClasses (with 24/48)
  {
    SizeMap size_map;
    ASSERT_TRUE(size_map.Init(kReuseRelaxedBelow64SizeClasses.classes));
    EXPECT_THAT(size_map.class_to_size_range(1), Pair(1, 8));
    EXPECT_THAT(size_map.class_to_size_range(2), Pair(9, 16));
#if defined(__cpp_aligned_new) && __STDCPP_DEFAULT_NEW_ALIGNMENT__ <= 8
    EXPECT_THAT(size_map.class_to_size_range(3), Pair(17, 24));
    EXPECT_THAT(size_map.class_to_size_range(4), Pair(25, 32));
    EXPECT_THAT(size_map.class_to_size_range(5), Pair(33, 48));
    EXPECT_THAT(size_map.class_to_size_range(6), Pair(49, 64));
#else
    EXPECT_THAT(size_map.class_to_size_range(3), Pair(17, 32));
    EXPECT_THAT(size_map.class_to_size_range(4), Pair(33, 48));
    EXPECT_THAT(size_map.class_to_size_range(5), Pair(49, 64));
#endif
  }

  // Verify kSizeClasses (default, power of two below 64)
  {
    SizeMap size_map;
    ASSERT_TRUE(size_map.Init(kSizeClasses.classes));
    EXPECT_THAT(size_map.class_to_size_range(1), Pair(1, 8));
    EXPECT_THAT(size_map.class_to_size_range(2), Pair(9, 16));
    EXPECT_THAT(size_map.class_to_size_range(3), Pair(17, 32));
    EXPECT_THAT(size_map.class_to_size_range(4), Pair(33, 64));
  }
}

}  // namespace tcmalloc::tcmalloc_internal
