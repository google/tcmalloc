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

#include "absl/base/attributes.h"
#include "absl/time/time.h"

extern "C" {

[[maybe_unused]] ABSL_ATTRIBUTE_WEAK void
TCMalloc_Internal_SetBackgroundReleaseRate(size_t value);
[[maybe_unused]] ABSL_ATTRIBUTE_WEAK uint64_t
TCMalloc_Internal_GetHeapSizeHardLimit();
[[maybe_unused]] ABSL_ATTRIBUTE_WEAK bool TCMalloc_Internal_GetHPAASubrelease();
[[maybe_unused]] ABSL_ATTRIBUTE_WEAK void
TCMalloc_Internal_GetHugePageFillerSkipSubreleaseInterval(absl::Duration* v);
[[maybe_unused]] ABSL_ATTRIBUTE_WEAK bool
TCMalloc_Internal_GetShufflePerCpuCachesEnabled();
[[maybe_unused]] ABSL_ATTRIBUTE_WEAK bool
TCMalloc_Internal_GetPrioritizeSpansEnabled();
[[maybe_unused]] ABSL_ATTRIBUTE_WEAK bool
TCMalloc_Internal_GetPartialTransferCacheEnabled();
[[maybe_unused]] ABSL_ATTRIBUTE_WEAK double
TCMalloc_Internal_GetPeakSamplingHeapGrowthFraction();
[[maybe_unused]] ABSL_ATTRIBUTE_WEAK bool
TCMalloc_Internal_GetPerCpuCachesEnabled();
[[maybe_unused]] ABSL_ATTRIBUTE_WEAK size_t
TCMalloc_Internal_GetStats(char* buffer, size_t buffer_length);
[[maybe_unused]] ABSL_ATTRIBUTE_WEAK void
TCMalloc_Internal_SetGuardedSamplingRate(int64_t v);
[[maybe_unused]] ABSL_ATTRIBUTE_WEAK void
TCMalloc_Internal_SetHeapSizeHardLimit(uint64_t v);
[[maybe_unused]] ABSL_ATTRIBUTE_WEAK void TCMalloc_Internal_SetHPAASubrelease(
    bool v);
[[maybe_unused]] ABSL_ATTRIBUTE_WEAK void
TCMalloc_Internal_SetShufflePerCpuCachesEnabled(bool v);
[[maybe_unused]] ABSL_ATTRIBUTE_WEAK void
TCMalloc_Internal_SetPrioritizeSpansEnabled(bool v);
[[maybe_unused]] ABSL_ATTRIBUTE_WEAK void
TCMalloc_Internal_SetPartialTransferCacheEnabled(bool v);
[[maybe_unused]] ABSL_ATTRIBUTE_WEAK void
TCMalloc_Internal_SetMaxPerCpuCacheSize(int32_t v);
[[maybe_unused]] ABSL_ATTRIBUTE_WEAK void
TCMalloc_Internal_SetMaxTotalThreadCacheBytes(int64_t v);
[[maybe_unused]] ABSL_ATTRIBUTE_WEAK void
TCMalloc_Internal_SetPeakSamplingHeapGrowthFraction(double v);
[[maybe_unused]] ABSL_ATTRIBUTE_WEAK void
TCMalloc_Internal_SetPerCpuCachesEnabled(bool v);
[[maybe_unused]] ABSL_ATTRIBUTE_WEAK void
TCMalloc_Internal_SetProfileSamplingRate(int64_t v);
[[maybe_unused]] ABSL_ATTRIBUTE_WEAK void
TCMalloc_Internal_SetHugePageFillerSkipSubreleaseInterval(absl::Duration v);
[[maybe_unused]] ABSL_ATTRIBUTE_WEAK void
TCMalloc_Internal_SetLifetimeAllocatorOptions(absl::string_view s);
[[maybe_unused]] ABSL_ATTRIBUTE_WEAK int32_t
TCMalloc_Internal_GetLinearSearchLengthTrackerList();
[[maybe_unused]] ABSL_ATTRIBUTE_WEAK void
TCMalloc_Internal_SetLinearSearchLengthTrackerList(int32_t v);
[[maybe_unused]] ABSL_ATTRIBUTE_WEAK bool
TCMalloc_Internal_GetMadviseColdRegionsNoHugepage();
[[maybe_unused]] ABSL_ATTRIBUTE_WEAK void
TCMalloc_Internal_SetMadviseColdRegionsNoHugepage(bool v);
[[maybe_unused]] ABSL_ATTRIBUTE_WEAK bool TCMalloc_Internal_PossiblyCold(
    const void* ptr);
[[maybe_unused]] ABSL_ATTRIBUTE_WEAK bool
TCMalloc_Internal_GetPerCpuCachesDynamicSlabEnabled();
[[maybe_unused]] ABSL_ATTRIBUTE_WEAK void
TCMalloc_Internal_SetPerCpuCachesDynamicSlabEnabled(bool v);
[[maybe_unused]] ABSL_ATTRIBUTE_WEAK double
TCMalloc_Internal_GetPerCpuCachesDynamicSlabGrowThreshold();
[[maybe_unused]] ABSL_ATTRIBUTE_WEAK void
TCMalloc_Internal_SetPerCpuCachesDynamicSlabGrowThreshold(double v);
[[maybe_unused]] ABSL_ATTRIBUTE_WEAK double
TCMalloc_Internal_GetPerCpuCachesDynamicSlabShrinkThreshold();
[[maybe_unused]] ABSL_ATTRIBUTE_WEAK void
TCMalloc_Internal_SetPerCpuCachesDynamicSlabShrinkThreshold(double v);

[[maybe_unused]] ABSL_ATTRIBUTE_WEAK bool
TCMalloc_Internal_GetPassSpanObjectCountToPageheap();
[[maybe_unused]] ABSL_ATTRIBUTE_WEAK void
TCMalloc_Internal_SetPassSpanObjectCountToPageheap(bool v);

[[maybe_unused]] ABSL_ATTRIBUTE_WEAK bool
TCMalloc_Internal_GetUseNewResidencyApi();
[[maybe_unused]] ABSL_ATTRIBUTE_WEAK void
TCMalloc_Internal_SetUseNewResidencyApi(bool v);
}

#endif  // TCMALLOC_INTERNAL_PARAMETER_ACCESSORS_H_
