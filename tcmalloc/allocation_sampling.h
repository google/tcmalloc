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

#ifndef TCMALLOC_ALLOCATION_SAMPLING_H_
#define TCMALLOC_ALLOCATION_SAMPLING_H_

#include <cstddef>
#include <memory>
#include <optional>

#include "absl/base/attributes.h"
#include "absl/base/nullability.h"
#include "tcmalloc/common.h"
#include "tcmalloc/internal/config.h"
#include "tcmalloc/internal/percpu.h"
#include "tcmalloc/malloc_extension.h"
#include "tcmalloc/pagemap.h"
#include "tcmalloc/pages.h"
#include "tcmalloc/sampler.h"
#include "tcmalloc/tcmalloc_policy.h"

GOOGLE_MALLOC_SECTION_BEGIN
namespace tcmalloc::tcmalloc_internal {

class Static;

[[nodiscard]] std::unique_ptr<const ProfileBase> DumpHeapProfile(Static& state);
#if !TCMALLOC_INTERNAL_PERCPU_USE_RSEQ
// For RSEQ enabled builds, we declare the sampler in percpu.h so that we can
// reference its address in percpu_tcmalloc.h without creating a circular
// dependency with the Sampler definition. Otherwise, we declare it here to
// avoid relying on the percpu code.
extern "C" ABSL_CONST_INIT thread_local Sampler tcmalloc_sampler
    ABSL_ATTRIBUTE_INITIAL_EXEC;
ABSL_CONST_INIT ABSL_ATTRIBUTE_WEAK thread_local Sampler tcmalloc_sampler
    ABSL_ATTRIBUTE_INITIAL_EXEC;
#endif

[[nodiscard]] inline Sampler& GetThreadSampler() {
  static_assert(sizeof(Sampler) == TCMALLOC_SAMPLER_SIZE,
                "update TCMALLOC_SAMPLER_SIZE");
  static_assert(alignof(Sampler) == TCMALLOC_SAMPLER_ALIGN,
                "update TCMALLOC_SAMPLER_ALIGN");
  static_assert(Sampler::HotDataOffset() == TCMALLOC_SAMPLER_HOT_OFFSET,
                "update TCMALLOC_SAMPLER_HOT_OFFSET");
#if TCMALLOC_INTERNAL_PERCPU_USE_RSEQ
  return *static_cast<Sampler*>(subtle::percpu::GetThreadSamplerAddress());
#else
  return tcmalloc_sampler;
#endif
}

// Performs sampling for already occurred allocation of object.
//
// For small object sizes, we allocate a new object in a sampled span.
//
// For large objects (i.e. allocated with do_malloc_pages) they are
// also fully reused and their span is marked as sampled.
//
// Note that do_free_with_size assumes sampled objects have
// page-aligned addresses. Please change both functions if need to
// invalidate the assumption.
//
// Note that size_class might not match requested_size in case of
// memalign. I.e. when larger than requested allocation is done to
// satisfy alignment constraint.
//
// In case of out-of-memory condition when allocating span or
// stacktrace struct, this function simply cheats and returns original
// object. As if no sampling was requested.
[[nodiscard]] sized_ptr_t SampleifyAllocation(Static& state,
                                              DynamicPolicy policy,
                                              size_t requested_size,
                                              size_t weight, size_t size_class,
                                              Range r, bool donated);

void MaybeUnsampleAllocation(Static& state, DynamicPolicy policy,
                             void* absl_nonnull ptr, std::optional<size_t> size,
                             PageId p, PageMeta meta);

}  // namespace tcmalloc::tcmalloc_internal
GOOGLE_MALLOC_SECTION_END

#endif  // TCMALLOC_ALLOCATION_SAMPLING_H_
