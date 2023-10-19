// Copyright 2023 The TCMalloc Authors
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


#include "tcmalloc/flags.h"
#include "gtest/gtest.h"
#include "absl/base/attributes.h"
#include "absl/flags/flag.h"
#include "tcmalloc/parameters.h"

namespace tcmalloc {
namespace tcmalloc_internal {

int ABSL_ATTRIBUTE_WEAK default_want_disable_laze_size_class_resize();

namespace {

TEST(DisableLazySizeClassResizeTest, Sanity) {
  ASSERT_NE(default_want_disable_laze_size_class_resize, nullptr);
  EXPECT_EQ(default_want_disable_laze_size_class_resize(), 1);

  absl::SetFlag(&FLAGS_tcmalloc_resize_cpu_cache_size_classes, true);
  EXPECT_TRUE(Parameters::resize_cpu_cache_size_classes());

  absl::SetFlag(&FLAGS_tcmalloc_resize_cpu_cache_size_classes, false);
  EXPECT_FALSE(Parameters::resize_cpu_cache_size_classes());
}

}  // namespace
}  // namespace tcmalloc_internal
}  // namespace tcmalloc
