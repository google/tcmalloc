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

#include "absl/base/attributes.h"
#include "absl/base/internal/cycleclock.h"
#include "absl/base/nullability.h"
#include "absl/base/optimization.h"
#include "absl/base/thread_annotations.h"
#include "absl/strings/string_view.h"
#include "absl/types/span.h"
#include "tcmalloc/arena.h"
#include "tcmalloc/central_freelist_options.h"
#include "tcmalloc/common.h"
#include "tcmalloc/error_reporting.h"
#include "tcmalloc/experiment.h"
#include "tcmalloc/experiment_config.h"
#include "tcmalloc/huge_pages.h"
#include "tcmalloc/internal/central_freelist_hooks.h"
#include "tcmalloc/internal/clock.h"
#include "tcmalloc/internal/config.h"
#include "tcmalloc/internal/logging.h"
#include "tcmalloc/internal/memory_tag.h"
#include "tcmalloc/internal/numa.h"
#include "tcmalloc/internal/prefetch.h"
#include "tcmalloc/internal/system_allocator.h"
#include "tcmalloc/page_allocator_interface.h"
#include "tcmalloc/pages.h"
#include "tcmalloc/parameters.h"
#include "tcmalloc/sizemap.h"
#include "tcmalloc/span.h"

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
  using Parameters::enable_unfiltered_collapse;
  using Parameters::filler_skip_subrelease_long_interval;
  using Parameters::filler_skip_subrelease_short_interval;
  using Parameters::hpaa_subrelease;
  using Parameters::huge_region_adaptive_release;
  using Parameters::madvise_cold_regions_nohugepage;
  using Parameters::per_cpu_caches_dynamic_slab_enabled;
  using Parameters::per_cpu_caches_dynamic_slab_grow_threshold;
  using Parameters::per_cpu_caches_dynamic_slab_shrink_threshold;
  using Parameters::release_drained_slab_metadata;
  using Parameters::release_max_cold_pages;
  using Parameters::release_max_filler_pages;
  using Parameters::release_partial_alloc_pages;
  using Parameters::release_stale_pages;
  using Parameters::subrelease_unbacked_hugepages;

  static size_t class_to_size(int size_class) {
    return state.sizemap().class_to_size(size_class);
  }

  static size_t num_objects_to_move(int size_class) {
    return state.sizemap().num_objects_to_move(size_class);
  }

  static Length class_to_pages(int size_class) {
    return Length(state.sizemap().class_to_pages(size_class));
  }

  static bool reuse_size_classes() {
    return state.size_class_configuration() ==
           SizeClassConfiguration::kReuseRelaxedBelow64;
  }

  // Allocates metadata from the arena, accounted to `tag`.  This does not take
  // pageheap_lock, so it may be called with pageheap_lock held.
  [[nodiscard]] static void* absl_nonnull Alloc(ArenaAlloc tag, size_t size,
                                                std::align_val_t alignment) {
    return state.arena().Alloc(tag, size, alignment);
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

  static const NumaTopology<kNumaPartitions, kNumBaseClasses>& numa_topology() {
    return state.numa_topology();
  }

  // These return types are deduced, as transfer_cache.h depends on this header.
  static auto& sharded_transfer_cache() {
    return state.sharded_transfer_cache();
  }

  static auto& transfer_cache() { return state.transfer_cache(); }

  static bool UseGenericShardedCache() {
    return IsExperimentActive(Experiment::TCMALLOC_SHARDED_TC_ABLATION) &&
           !IsExperimentActive(
               Experiment::TEST_ONLY_TCMALLOC_SHARDED_TRANSFER_CACHE);
  }

  static bool UseShardedCacheForLargeClassesOnly() {
    // Traditionally, we enable sharded transfer cache for large size
    // classes alone.
    return IsExperimentActive(
        Experiment::TEST_ONLY_TCMALLOC_SHARDED_TRANSFER_CACHE);
  }

  static bool UseWiderSlabs() {
    // We use wider 512KiB slab only when partitioning is not enabled. NUMA
    // and security partitions increase shift by 1 by itself, so we can not
    // increase it further.
    return state.active_partitions() == 1;
  }

  static bool HaveHooks() { return state.HaveHooks(); }

  static auto active_partitions() { return state.active_partitions(); }

  static bool multiple_non_numa_partitions() {
    return state.multiple_non_numa_partitions();
  }

  static void InvokeInsertRangeHook(size_t size_class,
                                    absl::Span<void*> batch) {
    if (ABSL_PREDICT_TRUE(central_freelist_insert_range_hooks.empty())) {
      return;
    }
    InvokeInsertRangeHookSlow(size_class, batch);
  }

  static void InvokeRemoveRangeHook(size_t size_class,
                                    absl::Span<void*> batch) {
    if (ABSL_PREDICT_TRUE(central_freelist_remove_range_hooks.empty())) {
      return;
    }
    InvokeRemoveRangeHookSlow(size_class, batch);
  }

  static uint64_t clock_now() { return absl::base_internal::CycleClock::Now(); }
  static double clock_frequency() {
    return absl::base_internal::CycleClock::Frequency();
  }

  static void MapObjectsToSpans(absl::Span<void*> batch,
                                Span** absl_nonnull spans,
                                int expected_size_class) {
    // Prefetch Span objects to reduce cache misses.
    for (int i = 0; i < batch.size(); ++i) {
      void* ptr = batch[i];
      const PageId p = PageIdContaining(ptr);
      auto [span, page_size_class] =
          state.pagemap().GetDescriptorAndSizeClass(p);
      // If we have a missing span/invalid span, we expect to retrieve
      // page_size_class=0 causing us to take this overloaded branch since
      // expected_size_class>0.
      if (ABSL_PREDICT_FALSE(page_size_class != expected_size_class)) {
        HandleDetectedUB(ptr, span, page_size_class, expected_size_class);
      }
      span->Prefetch();
      spans[i] = span;
    }
  }

  [[nodiscard]] static Span* absl_nullable AllocateSpan(int size_class,
                                                        size_t objects_per_span,
                                                        Length pages_per_span)
      ABSL_LOCKS_EXCLUDED(pageheap_lock) {
    const MemoryTag tag = MemoryTagFromSizeClass(size_class);
    const AccessDensityPrediction density = AccessDensity(objects_per_span);

    SpanAllocInfo span_alloc_info = {.objects_per_span = objects_per_span,
                                     .density = density};
    TC_ASSERT(density == AccessDensityPrediction::kSparse ||
              (density == AccessDensityPrediction::kDense &&
               pages_per_span == Length(1)));
    Span* span =
        state.page_allocator().New(pages_per_span, span_alloc_info, tag);
    if (ABSL_PREDICT_FALSE(span == nullptr)) {
      return nullptr;
    }
    TC_ASSERT_EQ(tag, GetMemoryTag(span->start_address()));
    TC_ASSERT_EQ(span->num_pages(), pages_per_span);

    state.pagemap().RegisterSizeClass(span, size_class);
    return span;
  }

  static void DeallocateSpans(size_t objects_per_span,
                              absl::Span<Span*> free_spans)
      ABSL_LOCKS_EXCLUDED(pageheap_lock) {
    TC_ASSERT_NE(free_spans.size(), 0);
    TC_ASSERT_LE(free_spans.size(), kMaxObjectsToMove);
    const MemoryTag tag = GetMemoryTag(free_spans[0]->start_address());
    // Unregister size class doesn't require holding any locks.
    for (Span* const free_span : free_spans) {
      TC_ASSERT_EQ(GetMemoryTag(free_span->start_address()), tag);
      TC_ASSERT(!IsSampledMemory(free_span->start_address()));
      state.pagemap().UnregisterSizeClass(free_span);

      // Before taking pageheap_lock, prefetch the PageTrackers these spans are
      // on.
      const PageId p = free_span->first_page();

      // In huge_page_filler.h, we static_assert that PageTracker's key elements
      // for deallocation are within the first two cachelines.
      void* pt = state.pagemap().GetHugepage(p);
      // Prefetch for writing, as we will issue stores to the PageTracker
      // instance.
      PrefetchW(pt);
      PrefetchW(reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(pt) +
                                        ABSL_CACHELINE_SIZE));
    }

#ifdef TCMALLOC_INTERNAL_LEGACY_LOCKING
    ReturnSpansToPageHeap(tag, free_spans, objects_per_span);
#else
    PageAllocatorInterface::AllocationState allocs[kMaxObjectsToMove];
    for (int i = 0, n = free_spans.size(); i < n; ++i) {
      Span* s = free_spans[i];
      TC_ASSERT_EQ(tag, GetMemoryTag(s->start_address()));
      allocs[i].r = Range(s->first_page(), s->num_pages());
      allocs[i].donated = s->donated();
      Span::Delete(s);
    }
    const AccessDensityPrediction density = AccessDensity(objects_per_span);
    SpanAllocInfo span_alloc_info = {.objects_per_span = objects_per_span,
                                     .density = density};
    ReturnAllocsToPageHeap(tag, absl::MakeSpan(allocs, free_spans.size()),
                           span_alloc_info);
#endif
  }

  // Clock consumed by the filler and huge cache.  Tests substitute a fake so
  // they can advance time deterministically.
  static Clock clock() { return Clock{}; }

  // Arena state.
  static Arena& arena() { return state.arena(); }

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
  static void* GetHugepage(HugePage p) {
    return state.pagemap().GetHugepage(p.first_page());
  }
  [[nodiscard]] static bool Ensure(Range r)
      ABSL_EXCLUSIVE_LOCKS_REQUIRED(pageheap_lock) {
    return state.pagemap().Ensure(r);
  }
  static void ClearSpan(PageId page) {
    state.pagemap().Set(page, const_cast<Span*>(&state.invalid_span()));
  }
  static void SetSpan(PageId page, Span* absl_nonnull span) {
    state.pagemap().Set(page, span);
  }
  static void SetHugepage(HugePage p, void* pt) {
    state.pagemap().SetHugepage(p.first_page(), pt);
  }

  // SpanAllocator state.
  static Span* NewSpan(Range r)
#ifdef TCMALLOC_INTERNAL_LEGACY_LOCKING
      ABSL_EXCLUSIVE_LOCKS_REQUIRED(pageheap_lock)
#else
      ABSL_LOCKS_EXCLUDED(pageheap_lock)
#endif  // TCMALLOC_INTERNAL_LEGACY_LOCKING
          ABSL_ATTRIBUTE_RETURNS_NONNULL {
    // TODO(b/134687001):  Delete this when span_allocator moves.
    return Span::New(r);
  }
  static void DeleteSpan(Span* span)
#ifdef TCMALLOC_INTERNAL_LEGACY_LOCKING
      ABSL_EXCLUSIVE_LOCKS_REQUIRED(pageheap_lock)
#endif  // TCMALLOC_INTERNAL_LEGACY_LOCKING
          ABSL_ATTRIBUTE_NONNULL() {
    Span::Delete(span);
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
  static bool BackAllocations() { return back_small_allocations(); }
  static int32_t BackSizeThresholdBytes() {
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

 private:
  ABSL_ATTRIBUTE_NOINLINE static void InvokeInsertRangeHookSlow(
      size_t size_class, absl::Span<void*> batch) {
    central_freelist_insert_range_hooks.Invoke(size_class, batch);
  }

  ABSL_ATTRIBUTE_NOINLINE static void InvokeRemoveRangeHookSlow(
      size_t size_class, absl::Span<void*> batch) {
    central_freelist_remove_range_hooks.Invoke(size_class, batch);
  }

  static MemoryTag MemoryTagFromSizeClass(size_t size_class) {
    if (IsColdSizeClass(size_class)) {
      return MemoryTag::kCold;
    }
    if (state.active_partitions() == 1) {
      return MemoryTag::kNormal;
    }
    return MultiNormalTag(size_class / kNumBaseClasses);
  }

  static AccessDensityPrediction AccessDensity(int objects_per_span) {
    // Use number of objects per span as a proxy for estimating access density
    // of the span. If number of objects per span is higher than
    // kFewObjectsAllocMaxLimit threshold, we assume that the span would be
    // long-lived.
    return objects_per_span >
                   central_freelist_internal::kFewObjectsAllocMaxLimit
               ? AccessDensityPrediction::kDense
               : AccessDensityPrediction::kSparse;
  }

  [[noreturn]] ABSL_ATTRIBUTE_NOINLINE static void HandleDetectedUB(
      void* ptr, Span* span, int page_size_class, int expected_size_class) {
    if (span == nullptr) {
      tcmalloc_internal::ReportCorruptedFree(state, ptr);
    } else if (span == &state.invalid_span()) {
      tcmalloc_internal::ReportDoubleFree(state, ptr);
    }
    tcmalloc_internal::ReportMismatchedSizeClass(state, ptr, page_size_class,
                                                 expected_size_class);
  }

#ifdef TCMALLOC_INTERNAL_LEGACY_LOCKING
  static void ReturnSpansToPageHeap(MemoryTag tag, absl::Span<Span*> free_spans,
                                    size_t objects_per_span)
      ABSL_LOCKS_EXCLUDED(pageheap_lock) {
    PageHeapSpinLockHolder l;
    for (Span* const free_span : free_spans) {
      TC_ASSERT_EQ(tag, GetMemoryTag(free_span->start_address()));
      state.page_allocator().Delete(free_span, tag,
                                    {.objects_per_span = objects_per_span});
    }
  }
#endif  // TCMALLOC_INTERNAL_LEGACY_LOCKING

  static void ReturnAllocsToPageHeap(
      MemoryTag tag,
      absl::Span<PageAllocatorInterface::AllocationState> free_allocs,
      SpanAllocInfo span_alloc_info) ABSL_LOCKS_EXCLUDED(pageheap_lock) {
    PageHeapSpinLockHolder l;
    for (const auto& alloc : free_allocs) {
      state.page_allocator().Delete(alloc, tag, span_alloc_info);
    }
  }
};

}  // namespace tcmalloc_internal
}  // namespace tcmalloc
GOOGLE_MALLOC_SECTION_END

#endif  // TCMALLOC_STATIC_FORWARDER_H_
