// Copyright 2026 The TCMalloc Authors
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

#ifndef TCMALLOC_STATIC_FORWARDER_H_
#define TCMALLOC_STATIC_FORWARDER_H_

#include <stddef.h>
#include <stdint.h>

#include <new>
#include <optional>

#include "absl/base/nullability.h"
#include "absl/base/thread_annotations.h"
#include "absl/strings/string_view.h"
#include "tcmalloc/arena.h"
#include "tcmalloc/common.h"
#include "tcmalloc/error_reporting.h"
#include "tcmalloc/experiment.h"
#include "tcmalloc/experiment_config.h"
#include "tcmalloc/huge_pages.h"
#include "tcmalloc/internal/clock.h"
#include "tcmalloc/internal/config.h"
#include "tcmalloc/internal/logging.h"
#include "tcmalloc/internal/memory_tag.h"
#include "tcmalloc/internal/numa.h"
#include "tcmalloc/internal/system_allocator.h"
#include "tcmalloc/pages.h"
#include "tcmalloc/parameters.h"
#include "tcmalloc/sizemap.h"

GOOGLE_MALLOC_SECTION_BEGIN
namespace tcmalloc {
namespace tcmalloc_internal {

class Static;
extern Static tc_globals;

// StaticForwarder provides the caches and page allocator with access to the
// global allocator state, `state`.
//
// This is a class, rather than namespaced globals, so that it can be mocked for
// testing.  It is templated on `state` so that its members can be defined
// inline here, despite Static containing the caches that use it:  member
// definitions are only instantiated where they are used, by which point Static
// is complete.
template <typename State, State& state>
class StaticForwarder : private Parameters {
 public:
  using Parameters::background_release_rate;
  using Parameters::enable_unfiltered_collapse;
  using Parameters::filler_skip_subrelease_cold_long_interval;
  using Parameters::filler_skip_subrelease_cold_short_interval;
  using Parameters::filler_skip_subrelease_long_interval;
  using Parameters::filler_skip_subrelease_short_interval;
  using Parameters::hpaa_subrelease;
  using Parameters::huge_region_adaptive_release;
  using Parameters::madvise_cold_regions_nohugepage;
  using Parameters::per_cpu_caches_dynamic_slab_enabled;
  using Parameters::per_cpu_caches_dynamic_slab_grow_threshold;
  using Parameters::per_cpu_caches_dynamic_slab_shrink_threshold;
  using Parameters::release_drained_slab_metadata;
  using Parameters::release_max_filler_pages;
  using Parameters::release_partial_alloc_pages;
  using Parameters::release_stale_pages;
  using Parameters::subrelease_unbacked_hugepages;

  [[nodiscard]] static size_t class_to_size(int size_class) {
    return state.sizemap().class_to_size(size_class);
  }

  [[nodiscard]] static size_t num_objects_to_move(int size_class) {
    return state.sizemap().num_objects_to_move(size_class);
  }

  [[nodiscard]] static bool reuse_size_classes() {
    return state.size_class_configuration() ==
           SizeClassConfiguration::kReuseRelaxedBelow64;
  }

  // Allocates metadata from the arena, accounted to `tag`.  This does not take
  // pageheap_lock, so it may be called with pageheap_lock held.
  [[nodiscard]] static void* absl_nonnull Alloc(ArenaAlloc tag, size_t size,
                                                std::align_val_t alignment) {
    return state.arena().Alloc(tag, size, alignment);
  }

  [[nodiscard]] static void* absl_nonnull AllocCpuCache(
      size_t size, std::align_val_t alignment)
      ABSL_LOCKS_EXCLUDED(pageheap_lock) {
    TC_ASSERT(state.IsInited());
    // TODO(b/373944374): Arena is thread-safe, but we take the pageheap_lock to
    // present a consistent view of memory usage.
    PageHeapSpinLockHolder l;
    return state.arena().Alloc(ArenaAlloc::kCpuCache, size, alignment);
  }
  [[nodiscard]] static void* absl_nonnull AllocReportedImpending(
      size_t size, std::align_val_t alignment)
      ABSL_LOCKS_EXCLUDED(pageheap_lock) {
    TC_ASSERT(state.IsInited());
    // TODO(b/373944374): Arena is thread-safe, but we take the pageheap_lock to
    // present a consistent view of memory usage.
    PageHeapSpinLockHolder l;
    // Negate previous update to allocated that accounted for this allocation.
    state.arena().UpdateAllocatedAndNonresident(-static_cast<int64_t>(size), 0);
    return state.arena().Alloc(ArenaAlloc::kCpuCache, size, alignment);
  }

  static void Dealloc(void* ptr, size_t size, std::align_val_t alignment) {
    TC_ASSERT(false);
  }

  static void ArenaUpdateAllocatedAndNonresident(int64_t allocated,
                                                 int64_t nonresident)
      ABSL_LOCKS_EXCLUDED(pageheap_lock) {
    TC_ASSERT(state.IsInited());
    // TODO(b/373944374): Arena is thread-safe, but we take the pageheap_lock to
    // present a consistent view of memory usage.
    PageHeapSpinLockHolder l;
    if (allocated > 0) {
      state.page_allocator().ShrinkToUsageLimit(
          BytesToLengthCeil(allocated),
          /*may_have_grown=*/allocated > nonresident);
    }
    state.arena().UpdateAllocatedAndNonresident(allocated, nonresident);
  }

  static void SetAnonVmaName(void* ptr, size_t size,
                             std::optional<absl::string_view> name) {
    TC_ASSERT_EQ(reinterpret_cast<uintptr_t>(ptr) % kHugePageSize, 0);
    TC_ASSERT_EQ(size % kHugePageSize, 0);
    state.system_allocator().SetAnonVmaName(ptr, size, name);
  }

  [[nodiscard]] static const NumaTopology<kNumaPartitions, kNumBaseClasses>&
  numa_topology() {
    return state.numa_topology();
  }

  // These return types are deduced, as transfer_cache.h depends on this header.
  [[nodiscard]] static auto& sharded_transfer_cache() {
    return state.sharded_transfer_cache();
  }

  [[nodiscard]] static auto& transfer_cache() { return state.transfer_cache(); }

  [[nodiscard]] static bool UseGenericShardedCache() {
    return IsExperimentActive(
               Experiment::TCMALLOC_SONIC_SHARDED_TRANSFER_CACHE) &&
           !IsExperimentActive(
               Experiment::TEST_ONLY_TCMALLOC_SHARDED_TRANSFER_CACHE);
  }

  [[nodiscard]] static bool UseShardedCacheForLargeClassesOnly() {
    // Traditionally, we enable sharded transfer cache for large size
    // classes alone.
    return IsExperimentActive(
        Experiment::TEST_ONLY_TCMALLOC_SHARDED_TRANSFER_CACHE);
  }

  [[nodiscard]] static bool UseWiderSlabs() {
    // We use wider 512KiB slab only when partitioning is not enabled. NUMA
    // and security partitions increase shift by 1 by itself, so we can not
    // increase it further.
    return state.active_partitions() == 1;
  }

  [[nodiscard]] static bool HaveHooks() { return state.HaveHooks(); }

  [[nodiscard]] static auto active_partitions() {
    return state.active_partitions();
  }

  [[nodiscard]] static bool multiple_non_numa_partitions() {
    return state.multiple_non_numa_partitions();
  }

  // Clock consumed by the filler and huge cache.  Tests substitute a fake so
  // they can advance time deterministically.
  [[nodiscard]] static Clock clock() { return Clock{}; }

  // Arena state.
  [[nodiscard]] static Arena& arena() { return state.arena(); }

  // PageAllocator state.

  // Check page heap memory limit.  `n` indicates the size of the allocation
  // currently being made, which will not be included in the sampled memory heap
  // for realized fragmentation estimation.
  //
  // `may_have_grown` provides a hint to elide statistics checks when the heap
  // did not grow above previously accounted for memory usage (for example,
  // reusing part of an already mapped hugepage, etc.).  This argument is always
  // checked in debug builds, and incorrect values in optimized builds may break
  // limit enforcement.
  static void ShrinkToUsageLimit(Length n, bool may_have_grown)
      ABSL_EXCLUSIVE_LOCKS_REQUIRED(pageheap_lock) {
    state.page_allocator().ShrinkToUsageLimit(n, may_have_grown);
  }

  // PageMap state.
  [[nodiscard]] static void* GetHugepage(HugePage p) {
    return state.pagemap().GetHugepage(p.first_page());
  }
  [[nodiscard]] static bool Ensure(Range r)
      ABSL_EXCLUSIVE_LOCKS_REQUIRED(pageheap_lock) {
    return state.pagemap().Ensure(r);
  }
  static void SetHugepage(HugePage p, void* pt) {
    state.pagemap().SetHugepage(p.first_page(), pt);
  }

  // Error reporting
  [[noreturn]] static void ReportDoubleFree(void* ptr) {
    tcmalloc_internal::ReportDoubleFree(state, ptr);
  }

  // SystemAlloc state.
  [[nodiscard]] static AddressRange AllocatePages(size_t bytes, size_t align,
                                                  MemoryTag tag) {
    return state.system_allocator().Allocate(bytes, align, tag);
  }
  [[nodiscard]] static bool BackAllocations() {
    return back_small_allocations();
  }
  [[nodiscard]] static int32_t BackSizeThresholdBytes() {
    return back_size_threshold_bytes();
  }
  static void Back(Range r) {
    state.system_allocator().Back(r.start_addr(), r.in_bytes());
  }
  [[nodiscard]] static MemoryModifyStatus ReleasePages(Range r) {
    return state.system_allocator().Release(r.start_addr(), r.in_bytes());
  }
  [[nodiscard]] static MemoryModifyStatus CollapsePages(Range r) {
    return state.system_allocator().Collapse(r.start_addr(), r.in_bytes());
  }
  static void SetAnonVmaName(Range r, std::optional<absl::string_view> name) {
    state.system_allocator().SetAnonVmaName(r.start_addr(), r.in_bytes(), name);
  }
};

}  // namespace tcmalloc_internal
}  // namespace tcmalloc
GOOGLE_MALLOC_SECTION_END

#endif  // TCMALLOC_STATIC_FORWARDER_H_
