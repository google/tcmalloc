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

#include <errno.h>

#include <algorithm>

#include "absl/base/internal/sysinfo.h"
#include "absl/time/clock.h"
#include "absl/time/time.h"
#include "tcmalloc/cpu_cache.h"
#include "tcmalloc/internal/logging.h"
#include "tcmalloc/internal/percpu.h"
#include "tcmalloc/internal_malloc_extension.h"
#include "tcmalloc/malloc_extension.h"
#include "tcmalloc/parameters.h"
#include "tcmalloc/static_vars.h"

// Release memory to the system at a constant rate.
void MallocExtension_Internal_ProcessBackgroundActions() {
  using ::tcmalloc::tcmalloc_internal::Parameters;
  using ::tcmalloc::tcmalloc_internal::tc_globals;

  tcmalloc::MallocExtension::MarkThreadIdle();

  absl::Time prev_time = absl::Now();
  const absl::Duration kSleepTime =
      tcmalloc::MallocExtension::GetBackgroundProcessSleepInterval();

  // Reclaim inactive per-cpu caches once per kCpuCacheReclaimPeriod.
  //
  // We use a longer 30 sec reclaim period to make sure that caches are indeed
  // idle. Reclaim drains entire cache, as opposed to cache shuffle for instance
  // that only shrinks a cache by a few objects at a time. So, we might have
  // larger performance degradation if we use a shorter reclaim interval and
  // drain caches that weren't supposed to.
  const absl::Duration kCpuCacheReclaimPeriod = 30 * kSleepTime;
  absl::Time last_reclaim = absl::Now();

  // Shuffle per-cpu caches once per kCpuCacheShufflePeriod.
  const absl::Duration kCpuCacheShufflePeriod = 5 * kSleepTime;
  absl::Time last_shuffle = absl::Now();

  const absl::Duration kSizeClassResizePeriod = 2 * kSleepTime;
  absl::Time last_size_class_resize = absl::Now();

  // See if we should resize the slab once per kCpuCacheSlabResizePeriod. This
  // period is coprime to kCpuCacheShufflePeriod and kCpuCacheReclaimPeriod.
  const absl::Duration kCpuCacheSlabResizePeriod = 29 * kSleepTime;
  absl::Time last_slab_resize_check = absl::Now();

#ifndef TCMALLOC_SMALL_BUT_SLOW
  // We reclaim unused objects from the transfer caches once per
  // kTransferCacheResizePeriod.
  const absl::Duration kTransferCachePlunderPeriod = 5 * kSleepTime;
  absl::Time last_transfer_cache_plunder_check = absl::Now();

  // Resize transfer caches once per kTransferCacheResizePeriod.
  const absl::Duration kTransferCacheResizePeriod = 2 * kSleepTime;
  absl::Time last_transfer_cache_resize_check = absl::Now();
#endif

  while (tcmalloc::MallocExtension::GetBackgroundProcessActionsEnabled()) {
    absl::Time now = absl::Now();

    // We follow the cache hierarchy in TCMalloc from outermost (per-CPU) to
    // innermost (the page heap).  Freeing up objects at one layer can help aid
    // memory coalescing for inner caches.

    if (tcmalloc::MallocExtension::PerCpuCachesActive()) {
      // Accelerate fences as part of this operation by registering this thread
      // with rseq.  While this is not strictly required to succeed, we do not
      // expect an inconsistent state for rseq (some threads registered and some
      // threads unable to).
      CHECK_CONDITION(tcmalloc::tcmalloc_internal::subtle::percpu::IsFast());

      // Try to reclaim per-cpu caches once every kCpuCacheReclaimPeriod
      // when enabled.
      if (now - last_reclaim >= kCpuCacheReclaimPeriod) {
        tc_globals.cpu_cache().TryReclaimingCaches();
        last_reclaim = now;
      }

      if (now - last_shuffle >= kCpuCacheShufflePeriod) {
        tc_globals.cpu_cache().ShuffleCpuCaches();
        last_shuffle = now;
      }

      if (Parameters::resize_cpu_cache_size_classes() &&
          now - last_size_class_resize >= kSizeClassResizePeriod) {
        tc_globals.cpu_cache().ResizeSizeClasses();
        last_size_class_resize = now;
      }

      // See if we need to grow the slab once every kCpuCacheSlabResizePeriod
      // when enabled.
      if (Parameters::per_cpu_caches_dynamic_slab_enabled() &&
          now - last_slab_resize_check >= kCpuCacheSlabResizePeriod) {
        tc_globals.cpu_cache().ResizeSlabIfNeeded();
        last_slab_resize_check = now;
      }
    }

    tc_globals.sharded_transfer_cache().Plunder();

#ifndef TCMALLOC_SMALL_BUT_SLOW
    // Try to plunder and reclaim unused objects from transfer caches.
    if (now - last_transfer_cache_plunder_check >=
        kTransferCachePlunderPeriod) {
      tc_globals.transfer_cache().TryPlunder();
      last_transfer_cache_plunder_check = now;
    }

    if (now - last_transfer_cache_resize_check >= kTransferCacheResizePeriod) {
      tc_globals.transfer_cache().TryResizingCaches();
      last_transfer_cache_resize_check = now;
    }
#endif

    // If time goes backwards, we would like to cap the release rate at 0.
    ssize_t bytes_to_release =
        static_cast<size_t>(Parameters::background_release_rate()) *
        absl::ToDoubleSeconds(now - prev_time);
    bytes_to_release = std::max<ssize_t>(bytes_to_release, 0);

    // If release rate is set to 0, do not release memory to system. However, if
    // we want to release free and backed hugepages from HugeRegion,
    // ReleaseMemoryToSystem should be able to release those pages to the
    // system even with bytes_to_release = 0.
    if (bytes_to_release > 0 || Parameters::release_pages_from_huge_region()) {
      tcmalloc::MallocExtension::ReleaseMemoryToSystem(bytes_to_release);
    }

    prev_time = now;
    absl::SleepFor(kSleepTime);
  }
}
