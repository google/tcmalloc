// Copyright 2022 The TCMalloc Authors
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

#include "gtest/gtest.h"
#include "absl/container/flat_hash_set.h"
#include "tcmalloc/common.h"
#include "tcmalloc/experiment.h"

namespace tcmalloc::tcmalloc_internal {
namespace {

TEST(SizeMap, ReducedBelow64SizeClass) {
  ASSERT_TRUE(IsExperimentActive(
      Experiment::TEST_ONLY_TCMALLOC_REDUCED_BELOW64_SIZECLASS));
  SizeMap size_map;
  size_map.Init();
  for (size_t i = 0; i < kNumClasses; ++i) {
    EXPECT_TRUE(
        size_map_internal::IsReducedBelow64SizeClass(size_map.class_to_size(i)))
        << i << " " << size_map.class_to_size(i);
  }
}

TEST(SizeMap, IsReducedBelow64SizeClass) {
  const absl::flat_hash_set<int> allowed = {1, 2, 4, 8, 16, 32, 48, 64};
  for (size_t i = 1; i <= 10000; ++i) {
    EXPECT_EQ(size_map_internal::IsReducedBelow64SizeClass(i),
              allowed.contains(i) || i > 64)
        << i;
  }
}

}  // namespace
}  // namespace tcmalloc::tcmalloc_internal
