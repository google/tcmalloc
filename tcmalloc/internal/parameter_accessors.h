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

#ifndef TCMALLOC_INTERNAL_PARAMETER_ACCESSORS_H_
#define TCMALLOC_INTERNAL_PARAMETER_ACCESSORS_H_

#include <cstddef>
#include <cstdint>
#include <vector>

#include "absl/base/attributes.h"
#include "absl/base/nullability.h"
#include "absl/time/time.h"
#include "tcmalloc/malloc_extension.h"

namespace tcmalloc {
namespace tcmalloc_internal {

struct PageAllocationStatus;

struct TracerSizeClassInfo {
  size_t size;
  size_t span_size_in_bytes;
  size_t num_objects_to_move;
};

enum class MadviseSampledAllocations : bool { kDisabled, kEnabled };

}  // namespace tcmalloc_internal
}  // namespace tcmalloc

extern "C" {

ABSL_ATTRIBUTE_WEAK void TCMalloc_Internal_SetBackgroundReleaseRate(
    size_t value);
[[nodiscard]] ABSL_ATTRIBUTE_WEAK uint64_t
TCMalloc_Internal_GetHeapSizeHardLimit();
[[nodiscard]] ABSL_ATTRIBUTE_WEAK bool TCMalloc_Internal_GetHPAASubrelease();
ABSL_ATTRIBUTE_WEAK void
TCMalloc_Internal_GetHugePageFillerSkipSubreleaseShortInterval(
    absl::Duration* v);
ABSL_ATTRIBUTE_WEAK void
TCMalloc_Internal_GetHugePageFillerSkipSubreleaseLongInterval(
    absl::Duration* v);
[[nodiscard]] ABSL_ATTRIBUTE_WEAK bool
TCMalloc_Internal_GetReleasePartialAllocPagesEnabled();
[[nodiscard]] ABSL_ATTRIBUTE_WEAK bool
TCMalloc_Internal_GetUsermodeHugepageCollapse();
[[nodiscard]] ABSL_ATTRIBUTE_WEAK bool
TCMalloc_Internal_GetReleasePagesFromHugeRegionEnabled();
[[nodiscard]] ABSL_ATTRIBUTE_WEAK bool
TCMalloc_Internal_GetResizeSizeClassMaxCapacityEnabled();
[[nodiscard]] ABSL_ATTRIBUTE_WEAK bool
TCMalloc_Internal_GetPrioritizeSpansEnabled();
[[nodiscard]] ABSL_ATTRIBUTE_WEAK double
TCMalloc_Internal_GetPeakSamplingHeapGrowthFraction();
[[nodiscard]] ABSL_ATTRIBUTE_WEAK bool
TCMalloc_Internal_GetPerCpuCachesEnabled();
[[nodiscard]] ABSL_ATTRIBUTE_WEAK size_t TCMalloc_Internal_GetStats(
    char* buffer, size_t buffer_length, bool include_hugepage_fragmentation);
ABSL_ATTRIBUTE_WEAK void TCMalloc_Internal_SetGuardedSamplingInterval(
    int64_t v);
ABSL_ATTRIBUTE_WEAK void TCMalloc_Internal_SetHeapSizeHardLimit(uint64_t v);
ABSL_ATTRIBUTE_WEAK void TCMalloc_Internal_SetHPAASubrelease(bool v);
ABSL_ATTRIBUTE_WEAK void TCMalloc_Internal_SetReleasePartialAllocPagesEnabled(
    bool v);
ABSL_ATTRIBUTE_WEAK void TCMalloc_Internal_SetUsermodeHugepageCollapse(bool v);
ABSL_ATTRIBUTE_WEAK void TCMalloc_Internal_SetReleasePagesFromHugeRegionEnabled(
    bool v);
ABSL_ATTRIBUTE_WEAK void TCMalloc_Internal_SetResizeSizeClassMaxCapacityEnabled(
    bool v);
ABSL_ATTRIBUTE_WEAK void TCMalloc_Internal_SetPrioritizeSpansEnabled(bool v);
ABSL_ATTRIBUTE_WEAK void TCMalloc_Internal_SetMaxPerCpuCacheSize(int32_t v);
ABSL_ATTRIBUTE_WEAK void TCMalloc_Internal_SetMaxTotalThreadCacheBytes(
    int64_t v);
ABSL_ATTRIBUTE_WEAK void TCMalloc_Internal_SetPeakSamplingHeapGrowthFraction(
    double v);
ABSL_ATTRIBUTE_WEAK void TCMalloc_Internal_SetPerCpuCachesEnabled(bool v);
ABSL_ATTRIBUTE_WEAK void
TCMalloc_Internal_SetPerCpuCachesEnabledNoBuildRequirement(bool v);
ABSL_ATTRIBUTE_WEAK void TCMalloc_Internal_SetProfileSamplingInterval(
    int64_t v);
ABSL_ATTRIBUTE_WEAK void
TCMalloc_Internal_SetHugePageFillerSkipSubreleaseShortInterval(
    absl::Duration v);
ABSL_ATTRIBUTE_WEAK void
TCMalloc_Internal_SetHugePageFillerSkipSubreleaseLongInterval(absl::Duration v);
[[nodiscard]] ABSL_ATTRIBUTE_WEAK bool
TCMalloc_Internal_GetMadviseColdRegionsNoHugepage();
ABSL_ATTRIBUTE_WEAK void TCMalloc_Internal_SetMadviseColdRegionsNoHugepage(
    bool v);
[[nodiscard]] ABSL_ATTRIBUTE_WEAK
    tcmalloc::tcmalloc_internal::MadviseSampledAllocations
    TCMalloc_Internal_GetMadviseSampledAllocations();
ABSL_ATTRIBUTE_WEAK void TCMalloc_Internal_SetMadviseSampledAllocations(
    tcmalloc::tcmalloc_internal::MadviseSampledAllocations v);
[[nodiscard]] ABSL_ATTRIBUTE_WEAK int64_t
TCMalloc_Internal_GetEventTraceMemoryLimit();
ABSL_ATTRIBUTE_WEAK void TCMalloc_Internal_SetEventTraceMemoryLimit(int64_t v);
[[nodiscard]] ABSL_ATTRIBUTE_WEAK uint8_t
TCMalloc_Internal_GetMinHotAccessHint();
ABSL_ATTRIBUTE_WEAK void TCMalloc_Internal_SetMinHotAccessHint(uint8_t v);
[[nodiscard]] [[maybe_unused]] ABSL_ATTRIBUTE_WEAK bool
TCMalloc_Internal_PossiblyCold(const void* ptr);
[[nodiscard]] ABSL_ATTRIBUTE_WEAK bool
TCMalloc_Internal_GetPerCpuCachesDynamicSlabEnabled();
ABSL_ATTRIBUTE_WEAK void TCMalloc_Internal_SetPerCpuCachesDynamicSlabEnabled(
    bool v);

[[nodiscard]] ABSL_ATTRIBUTE_WEAK tcmalloc::tcmalloc_internal::MadvisePreference
TCMalloc_Internal_GetMadvise();
ABSL_ATTRIBUTE_WEAK void TCMalloc_Internal_SetMadvise(
    tcmalloc::tcmalloc_internal::MadvisePreference v);
[[nodiscard]] ABSL_ATTRIBUTE_WEAK bool
TCMalloc_Internal_GetReleaseFreeSwapped();
ABSL_ATTRIBUTE_WEAK void TCMalloc_Internal_SetReleaseFreeSwapped(bool v);

[[nodiscard]] ABSL_ATTRIBUTE_WEAK bool
TCMalloc_Internal_GetBackSmallAllocations();
ABSL_ATTRIBUTE_WEAK void TCMalloc_Internal_SetBackSmallAllocations(bool v);
[[nodiscard]] ABSL_ATTRIBUTE_WEAK int32_t
TCMalloc_Internal_GetBackSizeThresholdBytes();
ABSL_ATTRIBUTE_WEAK void TCMalloc_Internal_SetBackSizeThresholdBytes(int32_t v);
[[nodiscard]] ABSL_ATTRIBUTE_WEAK bool
TCMalloc_Internal_GetEnableUnfilteredCollapse();
ABSL_ATTRIBUTE_WEAK void TCMalloc_Internal_SetEnableUnfilteredCollapse(bool v);
[[nodiscard]] ABSL_ATTRIBUTE_WEAK bool
TCMalloc_Internal_GetHugeRegionAdaptiveReleaseEnabled();
ABSL_ATTRIBUTE_WEAK void TCMalloc_Internal_SetHugeRegionAdaptiveReleaseEnabled(
    bool v);
[[nodiscard]] ABSL_ATTRIBUTE_WEAK bool
TCMalloc_Internal_GetReleaseMaxColdPages();
ABSL_ATTRIBUTE_WEAK void TCMalloc_Internal_SetReleaseMaxColdPages(bool v);
[[nodiscard]] ABSL_ATTRIBUTE_WEAK bool
TCMalloc_Internal_GetReleaseMaxFillerPages();
ABSL_ATTRIBUTE_WEAK void TCMalloc_Internal_SetReleaseMaxFillerPages(bool v);
[[nodiscard]] ABSL_ATTRIBUTE_WEAK bool
TCMalloc_Internal_GetReleaseMaxSampledPages();
ABSL_ATTRIBUTE_WEAK void TCMalloc_Internal_SetReleaseMaxSampledPages(bool v);

ABSL_ATTRIBUTE_WEAK void TCMalloc_Internal_GetSizeClasses(
    std::vector<tcmalloc::tcmalloc_internal::TracerSizeClassInfo>* absl_nonnull
        size_classes);
[[nodiscard]] ABSL_ATTRIBUTE_WEAK size_t TCMalloc_Internal_GetPageSize();
[[nodiscard]] ABSL_ATTRIBUTE_WEAK bool
TCMalloc_Internal_GetReleaseDrainedSlabMetadata();
ABSL_ATTRIBUTE_WEAK void TCMalloc_Internal_SetReleaseDrainedSlabMetadata(
    bool v);
[[nodiscard]] ABSL_ATTRIBUTE_WEAK bool
TCMalloc_Internal_GetPageAllocationStatus(
    const void* ptr,
    tcmalloc::tcmalloc_internal::PageAllocationStatus* absl_nonnull status);
}

#endif  // TCMALLOC_INTERNAL_PARAMETER_ACCESSORS_H_
