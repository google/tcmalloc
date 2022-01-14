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

#include "absl/debugging/stacktrace.h"

GOOGLE_MALLOC_SECTION_BEGIN
namespace tcmalloc {
namespace tcmalloc_internal {

void SampledAllocation::PrepareForSampling() {
  // Get the current stack trace and reset all the other fields.
  sampled_stack.depth = absl::GetStackTrace(sampled_stack.stack, kMaxStackDepth,
                                            /* skip_count= */ 0);
  sampled_stack.proxy = nullptr;
  sampled_stack.requested_size = 0;
  sampled_stack.requested_alignment = 0;
  sampled_stack.allocated_size = 0;
  sampled_stack.access_hint = 0;
  sampled_stack.cold_allocated = false;
  sampled_stack.weight = 0;
}

}  // namespace tcmalloc_internal
}  // namespace tcmalloc
GOOGLE_MALLOC_SECTION_END
