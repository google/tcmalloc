// Copyright 2021 The TCMalloc Authors
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

#include "tcmalloc/sampled_allocation.h"

#include "gmock/gmock.h"

namespace tcmalloc {
namespace tcmalloc_internal {
namespace {

TEST(SampledAllocationTest, PrepareForSampling) {
  // PrepareForSampling() invoked in the constructor.
  SampledAllocation sampled_allocation;
  absl::base_internal::SpinLockHolder sample_lock(&sampled_allocation.lock);

  // Now verify some fields.
  EXPECT_GT(sampled_allocation.sampled_stack.depth, 0);
  EXPECT_EQ(sampled_allocation.allocated_size.load(), 0);

  // Set them to different values.
  sampled_allocation.sampled_stack.depth = 0;
  sampled_allocation.allocated_size.store(1, std::memory_order_relaxed);

  // Call PrepareForSampling() again and check the fields.
  sampled_allocation.PrepareForSampling();
  EXPECT_GT(sampled_allocation.sampled_stack.depth, 0);
  EXPECT_EQ(sampled_allocation.allocated_size.load(), 0);
}

}  // namespace
}  // namespace tcmalloc_internal
}  // namespace tcmalloc
