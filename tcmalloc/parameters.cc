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
#include "tcmalloc/parameters.h"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>

#include "absl/base/attributes.h"
#include "absl/base/call_once.h"
#include "absl/base/internal/spinlock.h"
#include "absl/time/time.h"
#include "tcmalloc/central_freelist.h"
#include "tcmalloc/common.h"
#include "tcmalloc/cpu_cache.h"
#include "tcmalloc/experiment.h"
#include "tcmalloc/experiment_config.h"
#include "tcmalloc/huge_page_options.h"
#include "tcmalloc/huge_pages.h"
#include "tcmalloc/internal/allocation_guard.h"
#include "tcmalloc/internal/config.h"
#include "tcmalloc/internal/environment.h"
#include "tcmalloc/internal/logging.h"
#include "tcmalloc/internal/parameter_accessors.h"
#include "tcmalloc/internal_malloc_extension.h"
#include "tcmalloc/malloc_extension.h"
#include "tcmalloc/page_allocator.h"
#include "tcmalloc/static_vars.h"
#include "tcmalloc/thread_cache.h"

GOOGLE_MALLOC_SECTION_BEGIN
namespace tcmalloc {
namespace tcmalloc_internal {

template <typename T>
static constexpr T DefaultOrDebugValue(T default_val, T debug_val) {
#ifdef NDEBUG
  return default_val;
#else
  return debug_val;
#endif
}

extern "C" ABSL_ATTRIBUTE_WEAK void
tcmalloc_flag_enable_heap_partitioning_default_on();

ABSL_ATTRIBUTE_WEAK int default_subrelease();

static bool decide_subrelease() {
  const char* e = thread_safe_getenv("TCMALLOC_HPAA_CONTROL");
  if (e) {
    switch (e[0]) {
      case '0':
        // If we're forcing HPAA on, we want to converge towards our default
        // of subrelease on, rather than off (where it is moot without HPAA).
        break;
      case '1':
        return false;
      case '2':
        return true;
      default:
        TC_BUG("bad env var '%s'", e);
    }
  }

  if (default_subrelease != nullptr) {
    const int decision = default_subrelease();
    if (decision != 0) {
      return decision > 0;
    }
  }

  return true;
}

extern "C" ABSL_ATTRIBUTE_WEAK bool
default_want_disable_huge_region_more_often();

static bool use_huge_region_more_often() {
  // Disable huge regions more often feature if built against an opt-out.
  if (default_want_disable_huge_region_more_often != nullptr) {
    return false;
  }

  // TODO(b/296281171): Remove this opt-out.
  const char* e =
      thread_safe_getenv("TCMALLOC_USE_HUGE_REGION_MORE_OFTEN_DISABLE");
  if (e) {
    switch (e[0]) {
      case '0':
        return true;
      case '1':
        return false;
      default:
        TC_BUG("bad env var '%s'", e);
    }
  }

  return true;
}

// As decide_subrelease() is determined at runtime, we cannot require constant
// initialization for the atomic.  This avoids an initialization order fiasco.
static std::atomic<bool>& hpaa_subrelease_ptr() {
  ABSL_CONST_INIT static absl::once_flag flag;
  ABSL_CONST_INIT static std::atomic<bool> v{false};
  absl::base_internal::LowLevelCallOnce(&flag, [&]() {
    v.store(decide_subrelease(), std::memory_order_relaxed);
  });

  return v;
}

void Parameters::InitializeFromExperiments() {
#if !defined(TCMALLOC_INTERNAL_SMALL_BUT_SLOW)
  if (IsExperimentActive(Experiment::TCMALLOC_DEMAND_CYCLE_120S)) {
    skip_subrelease_short_interval_ns_.store(
        absl::ToInt64Nanoseconds(absl::Seconds(60)), std::memory_order_relaxed);
    skip_subrelease_long_interval_ns_.store(
        absl::ToInt64Nanoseconds(absl::Seconds(300)),
        std::memory_order_relaxed);
    skip_subrelease_cold_short_interval_ns_.store(
        absl::ToInt64Nanoseconds(absl::Seconds(60)), std::memory_order_relaxed);
    skip_subrelease_cold_long_interval_ns_.store(
        absl::ToInt64Nanoseconds(absl::Seconds(300)),
        std::memory_order_relaxed);
  }
#endif

  if (IsExperimentActive(Experiment::TCMALLOC_SONIC_TRACK_BROKEN_HUGEPAGES)) {
    track_broken_hugepages_.store(TrackBrokenHugepages::kEnabled,
                                  std::memory_order_relaxed);
  }

  if (IsExperimentActive(
          Experiment::TCMALLOC_SONIC_PRESERVE_DONATED_ON_COLLAPSE)) {
    preserve_donated_on_collapse_.store(PreserveDonatedOnCollapse::kEnabled,
                                        std::memory_order_relaxed);
  }

  if (IsExperimentActive(
          Experiment::TCMALLOC_SONIC_COLLAPSE_RELEASED_HUGEPAGES)) {
    collapse_released_hugepages_.store(CollapseReleasedHugePages::kEnabled,
                                       std::memory_order_relaxed);
  }

  if (IsExperimentActive(
          Experiment::TCMALLOC_SONIC_PREFER_BACKED_FILLER_PAGES)) {
    prefer_backed_filler_pages_.store(PreferBackedPages::kEnabled,
                                      std::memory_order_relaxed);
  }

  if (IsExperimentActive(Experiment::TCMALLOC_SONIC_MADV_NOHUGEPAGE_REGIONS)) {
    madvise_cold_regions_nohugepage_.store(MadviseRegionsNoHugepage::kEnabled,
                                           std::memory_order_relaxed);
  }

  if (IsExperimentActive(Experiment::TCMALLOC_RELEASE_FREE_STALE)) {
    release_stale_pages_.store(ReleaseStalePages::kEnabled,
                               std::memory_order_relaxed);
  }

  if (IsExperimentActive(
          Experiment::TCMALLOC_RELEASE_DRAINED_METADATA_ABLATION)) {
    release_drained_slab_metadata_.store(false, std::memory_order_relaxed);
  }

  if (IsExperimentActive(Experiment::TCMALLOC_PGHO_EXPERIMENT)
  ) {
    min_hot_access_hint_.store(static_cast<tcmalloc::hot_cold_t>(2),
                               std::memory_order_relaxed);
  }
}

uint64_t Parameters::heap_size_hard_limit() {
  return tc_globals.page_allocator().limit(PageAllocator::kHard);
}

void Parameters::set_heap_size_hard_limit(uint64_t value) {
  TCMalloc_Internal_SetHeapSizeHardLimit(value);
}

bool Parameters::hpaa_subrelease() {
  return hpaa_subrelease_ptr().load(std::memory_order_relaxed);
}

void Parameters::set_hpaa_subrelease(bool value) {
  TCMalloc_Internal_SetHPAASubrelease(value);
}
extern "C" ABSL_ATTRIBUTE_WEAK bool
default_want_disable_subrelease_unbacked_hugepages();

static SubreleaseUnbackedMode want_subrelease_unbacked_hugepages() {
  if (default_want_disable_subrelease_unbacked_hugepages != nullptr) {
    return SubreleaseUnbackedMode::kDisabled;
  }
  const char* e =
      thread_safe_getenv("TCMALLOC_DISABLE_SUBRELEASE_UNBACKED_HUGEPAGES");
  if (e) {
    switch (e[0]) {
      case '0':
        return SubreleaseUnbackedMode::kEnabled;
      case '1':
        return SubreleaseUnbackedMode::kDisabled;
      default:
        TC_BUG("bad env var '%s'", e);
    }
  }
  return SubreleaseUnbackedMode::kEnabled;
}

SubreleaseUnbackedMode Parameters::subrelease_unbacked_hugepages() {
  ABSL_CONST_INIT static absl::once_flag flag;
  ABSL_CONST_INIT static std::atomic<SubreleaseUnbackedMode> v{
      SubreleaseUnbackedMode::kDisabled};
  absl::base_internal::LowLevelCallOnce(&flag, [&]() {
    v.store(want_subrelease_unbacked_hugepages(), std::memory_order_relaxed);
  });
  return v.load(std::memory_order_relaxed);
}

ABSL_CONST_INIT std::atomic<MallocExtension::BytesPerSecond>
    Parameters::background_release_rate_{MallocExtension::BytesPerSecond{
        0
    }};

void Parameters::set_background_release_rate(
    MallocExtension::BytesPerSecond value) {
  TCMalloc_Internal_SetBackgroundReleaseRate(static_cast<size_t>(value));
}

ABSL_CONST_INIT std::atomic<int64_t> Parameters::guarded_sampling_interval_(
    DefaultOrDebugValue(/*default_val=*/50, /*debug_val=*/5) *
    kDefaultProfileSamplingInterval);
// TODO(b/285379004):  Remove this opt-out.
ABSL_CONST_INIT std::atomic<ReleasePartialAllocPages>
    Parameters::release_partial_alloc_pages_(
        ReleasePartialAllocPages::kEnabled);
// TODO(b/123345734): Remove the flag when experimentation is done.
ABSL_CONST_INIT std::atomic<bool> Parameters::resize_size_class_max_capacity_(
    true);
// TODO(b/199203282):  Remove this opt-out.
ABSL_CONST_INIT std::atomic<bool> Parameters::release_pages_from_huge_region_(
    true);
ABSL_CONST_INIT std::atomic<int64_t> Parameters::max_total_thread_cache_bytes_(
    kDefaultOverallThreadCacheSize);
ABSL_CONST_INIT std::atomic<double>
    Parameters::peak_sampling_heap_growth_fraction_(1.1);
ABSL_CONST_INIT std::atomic<bool> Parameters::per_cpu_caches_enabled_(
#if defined(TCMALLOC_DEPRECATED_PERTHREAD)
    false
#else
    true
#endif
);
ABSL_CONST_INIT std::atomic<bool> Parameters::per_cpu_caches_dynamic_slab_(
    true);
ABSL_CONST_INIT std::atomic<tcmalloc::hot_cold_t>
    Parameters::min_hot_access_hint_(kDefaultMinHotAccessHint);
ABSL_CONST_INIT std::atomic<double>
    Parameters::per_cpu_caches_dynamic_slab_grow_threshold_(0.9);
ABSL_CONST_INIT std::atomic<double>
    Parameters::per_cpu_caches_dynamic_slab_shrink_threshold_(0.4);

ABSL_CONST_INIT std::atomic<int64_t> Parameters::profile_sampling_interval_(
    kDefaultProfileSamplingInterval);

ABSL_CONST_INIT std::atomic<CollapseReleasedHugePages>
    Parameters::collapse_released_hugepages_(
        CollapseReleasedHugePages::kDisabled);

// TODO: b/134694141 - Remove this opt out.
ABSL_CONST_INIT std::atomic<bool> Parameters::back_small_allocations_(false);
ABSL_CONST_INIT std::atomic<int32_t> Parameters::back_size_threshold_bytes_(
    kPageSize);
ABSL_CONST_INIT std::atomic<EnableUnfilteredCollapse>
    Parameters::enable_unfiltered_collapse_(
        EnableUnfilteredCollapse::kDisabled);
ABSL_CONST_INIT std::atomic<MadviseSampledAllocations>
    Parameters::madvise_sampled_allocations_(
        MadviseSampledAllocations::kDisabled);
ABSL_CONST_INIT std::atomic<size_t> Parameters::event_trace_memory_limit_(
    16 << 20);
ABSL_CONST_INIT std::atomic<HugeRegionAdaptiveRelease>
    Parameters::huge_region_adaptive_release_(
        HugeRegionAdaptiveRelease::kEnabled);
ABSL_CONST_INIT std::atomic<ReleaseStalePages> Parameters::release_stale_pages_(
    ReleaseStalePages::kDisabled);

ABSL_CONST_INIT std::atomic<bool> Parameters::release_drained_slab_metadata_(
    true);

// Configures short and long intervals to zero by default. We expect to set them
// to the non-zero durations once the feature is no longer experimental.
ABSL_CONST_INIT std::atomic<int64_t>
    Parameters::skip_subrelease_short_interval_ns_(
#if defined(TCMALLOC_INTERNAL_SMALL_BUT_SLOW)
        0
#else
        absl::ToInt64Nanoseconds(absl::Seconds(10))
#endif
    );
ABSL_CONST_INIT std::atomic<int64_t>
    Parameters::skip_subrelease_long_interval_ns_(
#if defined(TCMALLOC_INTERNAL_SMALL_BUT_SLOW)
        0
#else
        absl::ToInt64Nanoseconds(absl::Seconds(120))
#endif
    );
ABSL_CONST_INIT std::atomic<int64_t>
    Parameters::skip_subrelease_cold_short_interval_ns_(
#if defined(TCMALLOC_INTERNAL_SMALL_BUT_SLOW)
        0
#else
        absl::ToInt64Nanoseconds(absl::Seconds(10))
#endif
    );
ABSL_CONST_INIT std::atomic<int64_t>
    Parameters::skip_subrelease_cold_long_interval_ns_(
#if defined(TCMALLOC_INTERNAL_SMALL_BUT_SLOW)
        0
#else
        absl::ToInt64Nanoseconds(absl::Seconds(120))
#endif
    );
ABSL_CONST_INIT std::atomic<bool> Parameters::release_max_filler_pages_(false);
ABSL_CONST_INIT std::atomic<TrackBrokenHugepages>
    Parameters::track_broken_hugepages_(TrackBrokenHugepages::kDisabled);
ABSL_CONST_INIT std::atomic<PreserveDonatedOnCollapse>
    Parameters::preserve_donated_on_collapse_(
        PreserveDonatedOnCollapse::kDisabled);
ABSL_CONST_INIT std::atomic<PreferBackedPages>
    Parameters::prefer_backed_filler_pages_(PreferBackedPages::kDisabled);
ABSL_CONST_INIT std::atomic<MadviseRegionsNoHugepage>
    Parameters::madvise_cold_regions_nohugepage_(
        MadviseRegionsNoHugepage::kDisabled);

static std::atomic<HeapPartitioningMode>& heap_partitioning_mode_ptr() {
  ABSL_CONST_INIT static absl::once_flag flag;
  ABSL_CONST_INIT static std::atomic<HeapPartitioningMode> v{
#ifdef __SANITIZE_ALLOC_TOKEN__
      kSecurityPartitions > 1 ? HeapPartitioningMode::kLight :
#endif
                              HeapPartitioningMode::kOff};
  absl::base_internal::LowLevelCallOnce(&flag, [&]() {
    if (kSecurityPartitions == 1) {
      return;
    }
    const char* hp = thread_safe_getenv("TCMALLOC_HEAP_PARTITIONING");
    if (hp == nullptr &&
        tcmalloc_flag_enable_heap_partitioning_default_on != nullptr) {
      v.store(HeapPartitioningMode::kFull, std::memory_order_relaxed);
    }
    if (hp != nullptr) {
      if (strcasecmp(hp, "false") == 0 || std::strcmp(hp, "0") == 0) {
        v.store(HeapPartitioningMode::kOff, std::memory_order_relaxed);
      } else if (std::strcmp(hp, "2") == 0 || strcasecmp(hp, "light") == 0) {
        v.store(HeapPartitioningMode::kLight, std::memory_order_relaxed);
      } else {
        v.store(HeapPartitioningMode::kFull, std::memory_order_relaxed);
      }
    }
  });
  return v;
}

ABSL_CONST_INIT std::atomic<EnableCollapse>
    Parameters::usermode_hugepage_collapse_enabled_{
        // This feature causes very long delays in the tail in non-optimized
        // builds.
        //
        // TODO(b/287498389): remove this divergence.
        DefaultOrDebugValue(EnableCollapse::kEnabled,
                            EnableCollapse::kDisabled),
    };

ABSL_CONST_INIT std::atomic<bool>
    Parameters::background_process_actions_enabled_(true);
ABSL_CONST_INIT std::atomic<int64_t>
    Parameters::background_process_sleep_interval_ns_(
        absl::ToInt64Nanoseconds(absl::Seconds(1)));

EnableCollapse Parameters::usermode_hugepage_collapse() {
  return usermode_hugepage_collapse_enabled_.load(std::memory_order_relaxed);
}

HugeRegionUsageOption Parameters::huge_region_option() {
  // By default, we use slack to determine when to use HugeRegion. When slack is
  // greater than 64MB (to ignore small binaries), and greater than the number
  // of small allocations, we allocate large allocations from HugeRegion.
  //
  // When huge-region-more-often feature is enabled, we use number of abandoned
  // pages in addition to slack to make a decision. If the size of abandoned
  // pages plus slack exceeds 64MB (to ignore small binaries), we use HugeRegion
  // for large allocations. This results in using HugeRegions for all the large
  // allocations once the size exceeds 64MB.
  return use_huge_region_more_often()
             ? HugeRegionUsageOption::kUseForAllLargeAllocs
             : HugeRegionUsageOption::kDefault;
}

HeapPartitioningMode Parameters::heap_partitioning_mode() {
  return heap_partitioning_mode_ptr().load(std::memory_order_relaxed);
}

extern "C" ABSL_ATTRIBUTE_WEAK bool
default_want_disable_cfl_subbucket_prioritization();
static central_freelist_internal::CflSubbucketPrioritization
want_cfl_subbucket_prioritization() {
  if (default_want_disable_cfl_subbucket_prioritization != nullptr) {
    return central_freelist_internal::CflSubbucketPrioritization::kDisabled;
  }
  const char* e =
      thread_safe_getenv("TCMALLOC_DISABLE_CFL_SUBBUCKET_PRIORITIZATION");
  if (e) {
    switch (e[0]) {
      case '0':
        return central_freelist_internal::CflSubbucketPrioritization::kEnabled;
      case '1':
        return central_freelist_internal::CflSubbucketPrioritization::kDisabled;
      default:
        TC_BUG("bad env var '%s'", e);
    }
  }
  if (IsExperimentActive(Experiment::TCMALLOC_CFL_PRIORITIZATION_ABLATION)) {
    return central_freelist_internal::CflSubbucketPrioritization::kDisabled;
  }
  return central_freelist_internal::CflSubbucketPrioritization::kEnabled;
}

central_freelist_internal::CflSubbucketPrioritization
Parameters::cfl_subbucket_prioritization() {
  ABSL_CONST_INIT static absl::once_flag flag;
  ABSL_CONST_INIT static std::atomic<
      central_freelist_internal::CflSubbucketPrioritization>
      v{central_freelist_internal::CflSubbucketPrioritization::kDisabled};
  absl::base_internal::LowLevelCallOnce(&flag, [&]() {
    v.store(want_cfl_subbucket_prioritization(), std::memory_order_relaxed);
  });
  return v.load(std::memory_order_relaxed);
}

int32_t Parameters::max_per_cpu_cache_size() {
  tc_globals.InitIfNecessary();
  return tc_globals.cpu_cache().CacheLimit();
}

int ABSL_ATTRIBUTE_WEAK default_want_disable_dynamic_slabs();

// TODO(b/271475288): remove the default_want_disable_dynamic_slabs opt-out
// some time after 2023-02-01.
static bool want_disable_dynamic_slabs() {
  if (default_want_disable_dynamic_slabs == nullptr) return false;
  return default_want_disable_dynamic_slabs() > 0;
}

}  // namespace tcmalloc_internal
}  // namespace tcmalloc

using tcmalloc::tcmalloc_internal::EnableCollapse;
using tcmalloc::tcmalloc_internal::EnableUnfilteredCollapse;
using tcmalloc::tcmalloc_internal::HugeRegionAdaptiveRelease;
using tcmalloc::tcmalloc_internal::MadviseSampledAllocations;
using tcmalloc::tcmalloc_internal::Parameters;
using tcmalloc::tcmalloc_internal::ReleasePartialAllocPages;
using tcmalloc::tcmalloc_internal::tc_globals;

extern "C" {

int64_t MallocExtension_Internal_GetProfileSamplingInterval() {
  return Parameters::profile_sampling_interval();
}

void MallocExtension_Internal_SetProfileSamplingInterval(int64_t value) {
  Parameters::set_profile_sampling_interval(value);
}

int64_t MallocExtension_Internal_GetGuardedSamplingInterval() {
  return Parameters::guarded_sampling_interval();
}

void MallocExtension_Internal_SetGuardedSamplingInterval(int64_t value) {
  Parameters::set_guarded_sampling_interval(value);
}

int64_t MallocExtension_Internal_GetMaxTotalThreadCacheBytes() {
  return Parameters::max_total_thread_cache_bytes();
}

void MallocExtension_Internal_SetMaxTotalThreadCacheBytes(int64_t value) {
  Parameters::set_max_total_thread_cache_bytes(value);
}

bool MallocExtension_Internal_GetBackgroundProcessActionsEnabled() {
  return Parameters::background_process_actions_enabled();
}

void MallocExtension_Internal_GetBackgroundProcessSleepInterval(
    absl::Duration* ret) {
  *ret = Parameters::background_process_sleep_interval();
}

void MallocExtension_Internal_SetBackgroundProcessSleepInterval(
    absl::Duration value) {
  Parameters::set_background_process_sleep_interval(value);
}

void MallocExtension_Internal_GetSkipSubreleaseShortInterval(
    absl::Duration* ret) {
  tc_globals.InitIfNecessary();
  *ret = Parameters::filler_skip_subrelease_short_interval();
}

void MallocExtension_Internal_SetSkipSubreleaseShortInterval(
    absl::Duration value) {
  Parameters::set_filler_skip_subrelease_short_interval(value);
}

void MallocExtension_Internal_GetSkipSubreleaseLongInterval(
    absl::Duration* ret) {
  tc_globals.InitIfNecessary();
  *ret = Parameters::filler_skip_subrelease_long_interval();
}

void MallocExtension_Internal_SetSkipSubreleaseLongInterval(
    absl::Duration value) {
  Parameters::set_filler_skip_subrelease_long_interval(value);
}

tcmalloc::MallocExtension::BytesPerSecond
MallocExtension_Internal_GetBackgroundReleaseRate() {
  tc_globals.InitIfNecessary();
  return Parameters::background_release_rate();
}

void MallocExtension_Internal_SetBackgroundReleaseRate(
    tcmalloc::MallocExtension::BytesPerSecond rate) {
  Parameters::set_background_release_rate(rate);
}

void TCMalloc_Internal_SetBackgroundReleaseRate(size_t value) {
  tc_globals.InitIfNecessary();
  Parameters::background_release_rate_.store(
      static_cast<tcmalloc::MallocExtension::BytesPerSecond>(value));
}

uint64_t TCMalloc_Internal_GetHeapSizeHardLimit() {
  // Under ASan we could get here before globals have been initialized.
  tc_globals.InitIfNecessary();
  return Parameters::heap_size_hard_limit();
}

bool TCMalloc_Internal_GetHPAASubrelease() {
  return Parameters::hpaa_subrelease();
}

bool TCMalloc_Internal_GetReleasePartialAllocPagesEnabled() {
  return Parameters::release_partial_alloc_pages() ==
         ReleasePartialAllocPages::kEnabled;
}

bool TCMalloc_Internal_GetBackSmallAllocations() {
  return Parameters::back_small_allocations();
}

int32_t TCMalloc_Internal_GetBackSizeThresholdBytes() {
  return Parameters::back_size_threshold_bytes();
}

bool TCMalloc_Internal_GetReleasePagesFromHugeRegionEnabled() {
  return Parameters::release_pages_from_huge_region();
}

bool TCMalloc_Internal_GetUsermodeHugepageCollapse() {
  return Parameters::usermode_hugepage_collapse() == EnableCollapse::kEnabled;
}

bool TCMalloc_Internal_GetResizeSizeClassMaxCapacityEnabled() {
  return Parameters::resize_size_class_max_capacity();
}

double TCMalloc_Internal_GetPeakSamplingHeapGrowthFraction() {
  return Parameters::peak_sampling_heap_growth_fraction();
}

bool TCMalloc_Internal_GetPerCpuCachesEnabled() {
  return Parameters::per_cpu_caches();
}

void TCMalloc_Internal_SetGuardedSamplingInterval(int64_t v) {
  Parameters::guarded_sampling_interval_.store(v, std::memory_order_relaxed);
}

// update_lock guards changes via SetHeapSizeHardLimit.
ABSL_CONST_INIT static absl::base_internal::SpinLock update_lock(
    absl::base_internal::SCHEDULE_KERNEL_ONLY);

void TCMalloc_Internal_SetHeapSizeHardLimit(uint64_t value) {
  // limit == 0 implies no limit.
  value = value > 0 ? value : std::numeric_limits<size_t>::max();
  // Ensure that page allocator is set up.
  tc_globals.InitIfNecessary();

  tcmalloc::tcmalloc_internal::AllocationGuardSpinLockHolder l(update_lock);

  using tcmalloc::tcmalloc_internal::PageAllocator;
  const size_t old_limit =
      tc_globals.page_allocator().limit(PageAllocator::kHard);
  tc_globals.page_allocator().set_limit(value, PageAllocator::kHard);
  if (value != old_limit) {
    TC_LOG("[tcmalloc] set page heap hard limit to %v bytes", value);
  }
}

void TCMalloc_Internal_SetHPAASubrelease(bool v) {
  tcmalloc::tcmalloc_internal::hpaa_subrelease_ptr().store(
      v, std::memory_order_relaxed);
}

void TCMalloc_Internal_SetReleasePartialAllocPagesEnabled(bool v) {
  Parameters::release_partial_alloc_pages_.store(
      v ? ReleasePartialAllocPages::kEnabled
        : ReleasePartialAllocPages::kDisabled,
      std::memory_order_relaxed);
}

void TCMalloc_Internal_SetUsermodeHugepageCollapse(bool v) {
  Parameters::usermode_hugepage_collapse_enabled_.store(
      v ? EnableCollapse::kEnabled : EnableCollapse::kDisabled,
      std::memory_order_relaxed);
}

void TCMalloc_Internal_SetReleasePagesFromHugeRegionEnabled(bool v) {
  Parameters::release_pages_from_huge_region_.store(v,
                                                    std::memory_order_relaxed);
}

void TCMalloc_Internal_SetResizeSizeClassMaxCapacityEnabled(bool v) {
  Parameters::resize_size_class_max_capacity_.store(v,
                                                    std::memory_order_relaxed);
}

void TCMalloc_Internal_SetMaxPerCpuCacheSize(int32_t v) {
  tcmalloc::tcmalloc_internal::tc_globals.InitIfNecessary();
  tcmalloc::tcmalloc_internal::tc_globals.cpu_cache().SetCacheLimit(
      std::max<int32_t>(0, v));
}

void TCMalloc_Internal_SetMaxTotalThreadCacheBytes(int64_t v) {
  Parameters::max_total_thread_cache_bytes_.store(v, std::memory_order_relaxed);
  tcmalloc::tcmalloc_internal::ThreadCache::set_overall_thread_cache_size(v);
}

void TCMalloc_Internal_SetPeakSamplingHeapGrowthFraction(double v) {
  Parameters::peak_sampling_heap_growth_fraction_.store(
      v, std::memory_order_relaxed);
}

void TCMalloc_Internal_SetPerCpuCachesEnabled(bool v) {
#if !defined(TCMALLOC_DEPRECATED_PERTHREAD)
  if (!v) {
    TC_LOG(
        "Using per-thread caches requires linking against "
        ":tcmalloc_deprecated_perthread.");
    return;
  }
#endif  // !TCMALLOC_DEPRECATED_PERTHREAD

  TCMalloc_Internal_SetPerCpuCachesEnabledNoBuildRequirement(v);
}

void TCMalloc_Internal_SetPerCpuCachesEnabledNoBuildRequirement(bool v) {
  Parameters::per_cpu_caches_enabled_.store(v, std::memory_order_relaxed);
}

void TCMalloc_Internal_SetProfileSamplingInterval(int64_t v) {
  Parameters::profile_sampling_interval_.store(v, std::memory_order_relaxed);
}

void TCMalloc_Internal_GetHugePageFillerSkipSubreleaseShortInterval(
    absl::Duration* v) {
  tc_globals.InitIfNecessary();
  *v = Parameters::filler_skip_subrelease_short_interval();
}

void TCMalloc_Internal_SetHugePageFillerSkipSubreleaseShortInterval(
    absl::Duration v) {
  tc_globals.InitIfNecessary();
  Parameters::skip_subrelease_short_interval_ns_.store(
      absl::ToInt64Nanoseconds(v), std::memory_order_relaxed);
}

void TCMalloc_Internal_GetHugePageFillerSkipSubreleaseLongInterval(
    absl::Duration* v) {
  tc_globals.InitIfNecessary();
  *v = Parameters::filler_skip_subrelease_long_interval();
}

void TCMalloc_Internal_SetHugePageFillerSkipSubreleaseLongInterval(
    absl::Duration v) {
  tc_globals.InitIfNecessary();
  Parameters::skip_subrelease_long_interval_ns_.store(
      absl::ToInt64Nanoseconds(v), std::memory_order_relaxed);
}

void TCMalloc_Internal_GetHugePageFillerSkipSubreleaseColdShortInterval(
    absl::Duration* v) {
  tc_globals.InitIfNecessary();
  *v = Parameters::filler_skip_subrelease_cold_short_interval();
}

void TCMalloc_Internal_SetHugePageFillerSkipSubreleaseColdShortInterval(
    absl::Duration v) {
  tc_globals.InitIfNecessary();
  Parameters::skip_subrelease_cold_short_interval_ns_.store(
      absl::ToInt64Nanoseconds(v), std::memory_order_relaxed);
}

void TCMalloc_Internal_GetHugePageFillerSkipSubreleaseColdLongInterval(
    absl::Duration* v) {
  tc_globals.InitIfNecessary();
  *v = Parameters::filler_skip_subrelease_cold_long_interval();
}

void TCMalloc_Internal_SetHugePageFillerSkipSubreleaseColdLongInterval(
    absl::Duration v) {
  tc_globals.InitIfNecessary();
  Parameters::skip_subrelease_cold_long_interval_ns_.store(
      absl::ToInt64Nanoseconds(v), std::memory_order_relaxed);
}

bool TCMalloc_Internal_GetPerCpuCachesDynamicSlabEnabled() {
  return Parameters::per_cpu_caches_dynamic_slab_enabled();
}

void TCMalloc_Internal_SetPerCpuCachesDynamicSlabEnabled(bool v) {
  // We only allow disabling dynamic slabs using both the flag and
  // want_disable_dynamic_slabs.
  if (!v && !tcmalloc::tcmalloc_internal::want_disable_dynamic_slabs()) return;
  Parameters::per_cpu_caches_dynamic_slab_.store(v, std::memory_order_relaxed);
}

uint8_t TCMalloc_Internal_GetMinHotAccessHint() {
  tc_globals.InitIfNecessary();
  return static_cast<uint8_t>(Parameters::min_hot_access_hint());
}

void TCMalloc_Internal_SetMinHotAccessHint(uint8_t v) {
  tc_globals.InitIfNecessary();
  Parameters::min_hot_access_hint_.store(static_cast<tcmalloc::hot_cold_t>(v),
                                         std::memory_order_relaxed);
}

void TCMalloc_Internal_SetBackSmallAllocations(bool v) {
  Parameters::back_small_allocations_.store(v, std::memory_order_relaxed);
}

void TCMalloc_Internal_SetBackSizeThresholdBytes(int32_t v) {
  Parameters::back_size_threshold_bytes_.store(v, std::memory_order_relaxed);
}

bool TCMalloc_Internal_GetEnableUnfilteredCollapse() {
  return Parameters::enable_unfiltered_collapse() ==
         EnableUnfilteredCollapse::kEnabled;
}

void TCMalloc_Internal_SetEnableUnfilteredCollapse(bool v) {
  Parameters::enable_unfiltered_collapse_.store(
      v ? EnableUnfilteredCollapse::kEnabled
        : EnableUnfilteredCollapse::kDisabled,
      std::memory_order_relaxed);
}

bool TCMalloc_Internal_GetHugeRegionAdaptiveReleaseEnabled() {
  return Parameters::huge_region_adaptive_release() ==
         HugeRegionAdaptiveRelease::kEnabled;
}

void TCMalloc_Internal_SetHugeRegionAdaptiveReleaseEnabled(bool v) {
  Parameters::huge_region_adaptive_release_.store(
      v ? HugeRegionAdaptiveRelease::kEnabled
        : HugeRegionAdaptiveRelease::kDisabled,
      std::memory_order_relaxed);
}

bool TCMalloc_Internal_GetReleaseMaxFillerPages() {
  tc_globals.InitIfNecessary();
  return Parameters::release_max_filler_pages();
}

void TCMalloc_Internal_SetReleaseMaxFillerPages(bool v) {
  tc_globals.InitIfNecessary();
  Parameters::release_max_filler_pages_.store(v, std::memory_order_relaxed);
}

bool TCMalloc_Internal_GetTrackBrokenHugepages() {
  tc_globals.InitIfNecessary();
  return Parameters::track_broken_hugepages() ==
         tcmalloc::tcmalloc_internal::TrackBrokenHugepages::kEnabled;
}

void TCMalloc_Internal_SetTrackBrokenHugepages(bool v) {
  tc_globals.InitIfNecessary();
  Parameters::track_broken_hugepages_.store(
      v ? tcmalloc::tcmalloc_internal::TrackBrokenHugepages::kEnabled
        : tcmalloc::tcmalloc_internal::TrackBrokenHugepages::kDisabled,
      std::memory_order_relaxed);
}

bool TCMalloc_Internal_GetPreserveDonatedOnCollapse() {
  tc_globals.InitIfNecessary();
  return Parameters::preserve_donated_on_collapse() ==
         tcmalloc::tcmalloc_internal::PreserveDonatedOnCollapse::kEnabled;
}

void TCMalloc_Internal_SetPreserveDonatedOnCollapse(bool v) {
  tc_globals.InitIfNecessary();
  Parameters::preserve_donated_on_collapse_.store(
      v ? tcmalloc::tcmalloc_internal::PreserveDonatedOnCollapse::kEnabled
        : tcmalloc::tcmalloc_internal::PreserveDonatedOnCollapse::kDisabled,
      std::memory_order_relaxed);
}
bool TCMalloc_Internal_GetCollapseReleasedHugepages() {
  tc_globals.InitIfNecessary();
  return Parameters::collapse_released_hugepages() ==
         tcmalloc::tcmalloc_internal::CollapseReleasedHugePages::kEnabled;
}

void TCMalloc_Internal_SetCollapseReleasedHugepages(bool v) {
  tc_globals.InitIfNecessary();
  Parameters::collapse_released_hugepages_.store(
      v ? tcmalloc::tcmalloc_internal::CollapseReleasedHugePages::kEnabled
        : tcmalloc::tcmalloc_internal::CollapseReleasedHugePages::kDisabled,
      std::memory_order_relaxed);
}

bool TCMalloc_Internal_GetPreferBackedFillerPages() {
  tc_globals.InitIfNecessary();
  return Parameters::prefer_backed_filler_pages() ==
         tcmalloc::tcmalloc_internal::PreferBackedPages::kEnabled;
}

void TCMalloc_Internal_SetPreferBackedFillerPages(bool v) {
  tc_globals.InitIfNecessary();
  Parameters::prefer_backed_filler_pages_.store(
      v ? tcmalloc::tcmalloc_internal::PreferBackedPages::kEnabled
        : tcmalloc::tcmalloc_internal::PreferBackedPages::kDisabled,
      std::memory_order_relaxed);
}

bool TCMalloc_Internal_GetMadviseColdRegionsNoHugepage() {
  tc_globals.InitIfNecessary();
  return Parameters::madvise_cold_regions_nohugepage() ==
         tcmalloc::tcmalloc_internal::MadviseRegionsNoHugepage::kEnabled;
}

void TCMalloc_Internal_SetMadviseColdRegionsNoHugepage(bool v) {
  tc_globals.InitIfNecessary();
  Parameters::madvise_cold_regions_nohugepage_.store(
      v ? tcmalloc::tcmalloc_internal::MadviseRegionsNoHugepage::kEnabled
        : tcmalloc::tcmalloc_internal::MadviseRegionsNoHugepage::kDisabled,
      std::memory_order_relaxed);
}

MadviseSampledAllocations TCMalloc_Internal_GetMadviseSampledAllocations() {
  return Parameters::madvise_sampled_allocations();
}

void TCMalloc_Internal_SetMadviseSampledAllocations(
    MadviseSampledAllocations v) {
  Parameters::madvise_sampled_allocations_.store(v, std::memory_order_relaxed);
}

size_t TCMalloc_Internal_GetEventTraceMemoryLimit() {
  return Parameters::event_trace_memory_limit();
}

void TCMalloc_Internal_SetEventTraceMemoryLimit(size_t v) {
  Parameters::event_trace_memory_limit_.store(v, std::memory_order_relaxed);
}

bool TCMalloc_Internal_GetReleaseDrainedSlabMetadata() {
  tc_globals.InitIfNecessary();
  return Parameters::release_drained_slab_metadata();
}

void TCMalloc_Internal_SetReleaseDrainedSlabMetadata(bool v) {
  tc_globals.InitIfNecessary();
  Parameters::release_drained_slab_metadata_.store(v,
                                                   std::memory_order_relaxed);
}
}  // extern "C"

GOOGLE_MALLOC_SECTION_END
