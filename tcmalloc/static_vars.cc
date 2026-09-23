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

#include "tcmalloc/static_vars.h"

#include <stddef.h>

#include <atomic>
#include <cstring>

#include "absl/base/attributes.h"
#include "absl/base/internal/spinlock.h"
#include "absl/base/optimization.h"
#include "absl/types/span.h"
#include "tcmalloc/arena.h"
#include "tcmalloc/common.h"
#include "tcmalloc/experiment.h"
#include "tcmalloc/experiment_config.h"
#include "tcmalloc/guarded_page_allocator.h"
#include "tcmalloc/internal/cache_topology.h"
#include "tcmalloc/internal/config.h"
#include "tcmalloc/internal/environment.h"
#include "tcmalloc/internal/logging.h"
#include "tcmalloc/internal/mincore.h"
#include "tcmalloc/internal/numa.h"
#include "tcmalloc/internal/parameter_accessors.h"
#include "tcmalloc/internal/percpu.h"
#include "tcmalloc/internal/percpu_state.h"
#include "tcmalloc/internal/size_class_info.h"
#include "tcmalloc/internal/sysinfo.h"
#include "tcmalloc/malloc_extension.h"
#include "tcmalloc/page_allocator.h"
#include "tcmalloc/parameters.h"
#include "tcmalloc/sizemap.h"
#include "tcmalloc/span.h"
#include "tcmalloc/transfer_cache.h"

GOOGLE_MALLOC_SECTION_BEGIN
namespace tcmalloc {
namespace tcmalloc_internal {
ABSL_CONST_INIT absl::base_internal::SpinLock pageheap_lock(
    absl::base_internal::SCHEDULE_KERNEL_ONLY);
// Force kInvalidSpan to be read-protected.  Span contains a std::atomic, and
// libc++'s std::atomic implementation contains a mutable field in one of its
// implementation details.  This prevents Span from being placed in a read-only
// section automatically, even though we will never mutate this particular
// instance.
ABSL_ATTRIBUTE_SECTION_VARIABLE(.data.rel.ro)
constexpr Span Static::kInvalidSpan;

// We expect tc_globals to be in a zero-initialized section (.bss). This is
// important to keep binary size smaller. But there is no easy way to enforce
// this during compilation. ABSL_ATTRIBUTE_SECTION_VARIABLE(.bss) does it,
// but it places the variable in .bss section, while we want it to be in
// google_malloc_bss. And compiler does not like
// ABSL_ATTRIBUTE_SECTION_VARIABLE(google_malloc_bss). So instead,
// we check this in arena_test.cc (a random test) by declaring another
// global Static variable with ABSL_ATTRIBUTE_SECTION_VARIABLE(.bss).
TCMALLOC_ATTRIBUTE_NO_DESTROY ABSL_CONST_INIT Static tc_globals;

size_t Static::metadata_bytes() {
  const size_t internal_dependencies_size =
      sizeof(pageheap_lock) + sizeof(kInvalidSpan) +
      sizeof(CacheTopology::Instance()) + sizeof(PerCpuState::state());

  const size_t allocated =
#ifdef TCMALLOC_INTERNAL_LEGACY_LOCKING
      arena().stats().bytes_allocated
#else
      arena().allocated()
#endif
      + AddressRegionFactory::InternalBytesAllocated();
  return sizeof(*this) + allocated + internal_dependencies_size;
}

size_t Static::pagemap_residence() {
  // Determine residence of the root node of the pagemap.
  return MInCore::residence(&pagemap_, sizeof(pagemap_));
}

SizeClassConfiguration Static::size_class_configuration() {
  if (IsExperimentActive(Experiment::TEST_ONLY_TCMALLOC_POW2_SIZECLASS)) {
    return SizeClassConfiguration::kPow2Only;
  }
  if (IsExperimentActive(Experiment::TCMALLOC_RD_0_0)) {
    return SizeClassConfiguration::kReuseRelaxedBelow64Rd00;
  }
  if (IsExperimentActive(Experiment::TCMALLOC_RD_0_1)) {
    return SizeClassConfiguration::kReuseRelaxedBelow64Rd01;
  }
  if (IsExperimentActive(Experiment::TCMALLOC_RD_0_2)) {
    return SizeClassConfiguration::kReuseRelaxedBelow64Rd02;
  }
  return SizeClassConfiguration::kReuseRelaxedBelow64;
}

ABSL_ATTRIBUTE_COLD ABSL_ATTRIBUTE_NOINLINE void Static::SlowInitIfNecessary() {
  PageHeapSpinLockHolder l;

  // double-checked locking
  if (inited_.load(std::memory_order_acquire)) {
    return;
  }

  TC_CHECK(sizemap_.Init(SizeMap::CurrentClasses().classes));
  sampledallocation_allocator_.Init(arena_);
  span_allocator_.Init(arena_);
  threadcache_allocator_.Init(arena_);
  linked_sample_allocator_.Init(arena_);
  sampled_allocation_recorder_.Init(sampledallocation_allocator_);
  peak_heap_tracker_.Init(sampledallocation_allocator_);
  system_allocator_.Init(numa_topology_, kMinMmapAlloc);

  // Verify we can determine the number of CPUs now, since we will need it
  // later for per-CPU caches and initializing the cache topology.
  if (ABSL_PREDICT_FALSE(!NumCPUsMaybe().has_value())) {
    TCMalloc_Internal_SetPerCpuCachesEnabledNoBuildRequirement(false);
  }
  (void)subtle::percpu::IsFast();
  PerCpuState::state().Init();
  numa_topology_.Init();
  CacheTopology::Instance().Init();
  cpu_cache_.Init();

  if (IsExperimentActive(Experiment::TCMALLOC_PGHO_EXPERIMENT)
  ) {
    TCMalloc_Internal_SetMinHotAccessHint(/*v=*/2);
  }

  // Do a bit of sanitizing: make sure central_cache is aligned properly
  TC_CHECK_EQ((sizeof(transfer_cache_) % ABSL_CACHELINE_SIZE), 0);
  transfer_cache_.Init();
  // The constructor of the sharded transfer cache leaves it in a disabled
  // state.
  sharded_transfer_cache_.Init();
  new (page_allocator_.memory) PageAllocator;
  guardedpage_allocator_.Init(/*max_allocated_pages=*/64,
                              /*total_pages=*/128);

  inited_.store(true, std::memory_order_release);
}

}  // namespace tcmalloc_internal
}  // namespace tcmalloc
GOOGLE_MALLOC_SECTION_END
