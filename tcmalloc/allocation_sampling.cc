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

#include "tcmalloc/allocation_sampling.h"

#include <cmath>
#include <cstddef>
#include <memory>
#include <optional>
#include <utility>

#include "absl/base/attributes.h"
#include "absl/base/optimization.h"
#include "absl/debugging/stacktrace.h"
#include "absl/time/clock.h"
#include "absl/time/time.h"
#include "absl/types/span.h"
#include "tcmalloc/common.h"
#include "tcmalloc/cpu_cache.h"
#include "tcmalloc/error_reporting.h"
#include "tcmalloc/guarded_allocations.h"
#include "tcmalloc/internal/config.h"
#include "tcmalloc/internal/exponential_biased.h"
#include "tcmalloc/internal/logging.h"
#include "tcmalloc/internal/sampled_allocation.h"
#include "tcmalloc/malloc_extension.h"
#include "tcmalloc/malloc_hook.h"
#include "tcmalloc/pagemap.h"
#include "tcmalloc/pages.h"
#include "tcmalloc/parameters.h"
#include "tcmalloc/span.h"
#include "tcmalloc/stack_trace_table.h"
#include "tcmalloc/static_vars.h"
#include "tcmalloc/tcmalloc_policy.h"
#include "tcmalloc/thread_cache.h"

GOOGLE_MALLOC_SECTION_BEGIN
namespace tcmalloc::tcmalloc_internal {

std::unique_ptr<const ProfileBase> DumpHeapProfile(Static& state) {
  auto profile = std::make_unique<StackTraceTable>(ProfileType::kHeap);
  profile->SetStartTime(absl::Now());
  state.sampled_allocation_recorder().Iterate(
      [&](const SampledAllocation& sampled_allocation) {
        profile->AddTrace(1.0, sampled_allocation.sampled_stack);
      });
  return profile;
}

}  // namespace tcmalloc::tcmalloc_internal
GOOGLE_MALLOC_SECTION_END
