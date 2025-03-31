// Copyright 2025 The TCMalloc Authors
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

#ifndef TCMALLOC_ERROR_REPORTING_H_
#define TCMALLOC_ERROR_REPORTING_H_

#include <new>

#include "tcmalloc/internal/config.h"
#include "tcmalloc/static_vars.h"

GOOGLE_MALLOC_SECTION_BEGIN
namespace tcmalloc::tcmalloc_internal {

[[noreturn]] ABSL_ATTRIBUTE_NOINLINE void ReportMismatchedDelete(
    Static& state, const SampledAllocation& alloc, size_t size,
    size_t requested_size, std::optional<size_t> allocated_size);

[[noreturn]] ABSL_ATTRIBUTE_NOINLINE void ReportMismatchedDelete(
    Static& state, void* ptr, size_t size, size_t minimum_size,
    size_t maximum_size);

[[noreturn]]
ABSL_ATTRIBUTE_NOINLINE void ReportMismatchedSizeClass(Static& state,
                                                       void* object,
                                                       int page_size_class,
                                                       int object_size_class);

[[noreturn]]
ABSL_ATTRIBUTE_NOINLINE void ReportDoubleFree(Static& state, void* ptr);

[[noreturn]]
ABSL_ATTRIBUTE_NOINLINE void ReportCorruptedFree(
    Static& state, std::align_val_t expected_alignment, void* ptr);

}  // namespace tcmalloc::tcmalloc_internal
GOOGLE_MALLOC_SECTION_END

#endif  // TCMALLOC_ERROR_REPORTING_H_
