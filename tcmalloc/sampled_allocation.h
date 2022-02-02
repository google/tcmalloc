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

#ifndef TCMALLOC_SAMPLED_ALLOCATION_H_
#define TCMALLOC_SAMPLED_ALLOCATION_H_

#include "tcmalloc/internal/logging.h"
#include "tcmalloc/sampled_allocation_recorder.h"

GOOGLE_MALLOC_SECTION_BEGIN
namespace tcmalloc {
namespace tcmalloc_internal {

// Stores information about the sampled allocation.
struct SampledAllocation : public tcmalloc_internal::Sample<SampledAllocation> {
  constexpr SampledAllocation() = default;

  explicit SampledAllocation(const StackTrace& stack_trace) {
    PrepareForSampling(stack_trace);
  }

  SampledAllocation(const SampledAllocation&) = delete;
  SampledAllocation& operator=(const SampledAllocation&) = delete;

  SampledAllocation(SampledAllocation&&) = delete;
  SampledAllocation& operator=(SampledAllocation&&) = delete;

  // Puts the object into a clean state and blocks any readers currently
  // sampling the object.
  void PrepareForSampling(const StackTrace& stack_trace)
      ABSL_EXCLUSIVE_LOCKS_REQUIRED(lock);

  // The stack trace of the sampled allocation.
  StackTrace sampled_stack = {};
};

}  // namespace tcmalloc_internal
}  // namespace tcmalloc
GOOGLE_MALLOC_SECTION_END

#endif  // TCMALLOC_SAMPLED_ALLOCATION_H_
