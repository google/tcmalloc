// Copyright 2024 The TCMalloc Authors
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

#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "absl/status/status.h"
#include "absl/status/status_matchers.h"
#include "tcmalloc/internal/profile_builder.h"
#include "tcmalloc/malloc_extension.h"

namespace tcmalloc {
namespace tcmalloc_internal {
namespace {

using ::absl_testing::StatusIs;
using ::testing::HasSubstr;

TEST(ProfileBuilderNoTCMallocTest, StatusErrorTest) {
  auto profile_st =
      MakeProfileProto(MallocExtension::SnapshotCurrent(ProfileType::kHeap));
#if defined(ABSL_HAVE_ADDRESS_SANITIZER) || \
    defined(ABSL_HAVE_LEAK_SANITIZER) ||    \
    defined(ABSL_HAVE_MEMORY_SANITIZER) || defined(ABSL_HAVE_THREAD_SANITIZER)
  EXPECT_THAT(profile_st,
              StatusIs(absl::StatusCode::kUnimplemented,
                       HasSubstr("Program was built with sanitizers enabled")));
#else
  EXPECT_THAT(profile_st, StatusIs(absl::StatusCode::kInvalidArgument,
                                   HasSubstr("Empty heap profile")));
#endif
}

}  // namespace
}  // namespace tcmalloc_internal
}  // namespace tcmalloc
