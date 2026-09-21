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
//
// Static variables shared by multiple classes.

#ifndef TCMALLOC_STATIC_VARS_H_
#define TCMALLOC_STATIC_VARS_H_

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <atomic>

#include "absl/base/attributes.h"
#include "absl/base/optimization.h"
#include "absl/base/thread_annotations.h"
#include "tcmalloc/allocation_sample.h"
#include "tcmalloc/arena.h"
#include "tcmalloc/central_freelist.h"
#include "tcmalloc/common.h"
#include "tcmalloc/cpu_cache.h"
#include "tcmalloc/deallocation_profiler.h"
#include "tcmalloc/guarded_page_allocator.h"
#include "tcmalloc/internal/atomic_stats_counter.h"
#include "tcmalloc/internal/config.h"
#include "tcmalloc/internal/gwp_asan_state.h"
#include "tcmalloc/internal/logging.h"
#include "tcmalloc/internal/numa.h"
#include "tcmalloc/internal/sampled_allocation.h"
#include "tcmalloc/internal/sampled_allocation_recorder.h"
#include "tcmalloc/internal/system_allocator.h"
#include "tcmalloc/malloc_hook_invoke.h"
#include "tcmalloc/metadata_object_allocator.h"
#include "tcmalloc/page_allocator.h"
#include "tcmalloc/pagemap.h"
#include "tcmalloc/pages.h"
#include "tcmalloc/parameters.h"
#include "tcmalloc/peak_heap_tracker.h"
#include "tcmalloc/sizemap.h"
#include "tcmalloc/span.h"
#include "tcmalloc/stack_trace_table.h"
#include "tcmalloc/stats.h"
#include "tcmalloc/transfer_cache.h"

GOOGLE_MALLOC_SECTION_BEGIN
namespace tcmalloc {
namespace tcmalloc_internal {

class ThreadCache;

using SampledAllocationRecorder = ::tcmalloc::tcmalloc_internal::SampleRecorder<
    SampledAllocation,
    MetadataObjectAllocator<SampledAllocation, ArenaAlloc::kSampledAllocation>>;

class Static;
extern Static tc_globals;

class Static final {
 public:
  constexpr Static() = default;

  // Non-copyable/movable.
  Static(const Static&) = delete;
  Static(Static&&) = delete;
  Static& operator=(const Static&) = delete;
  Static& operator=(Static&&) = delete;

  // True if InitIfNecessary() has run to completion.
  bool IsInited() const;
  // Must be called before calling any of the accessors below.
  // Safe to call multiple times.
  void InitIfNecessary();

  // Central cache.
  CentralFreeList& central_freelist(int size_class) {
    return transfer_cache().central_freelist(size_class);
  }
  // Central cache -- an array of free-lists, one per size-class.
  // We have a separate lock per free-list to reduce contention.
  TransferCacheManager& transfer_cache() { return transfer_cache_; }

  // A per-cache domain TransferCache.
  ShardedTransferCacheManager& sharded_transfer_cache() {
    return sharded_transfer_cache_;
  }

  SizeMap& sizemap() { return sizemap_; }

  auto& cpu_cache() { return cpu_cache_; }

  PeakHeapTracker& peak_heap_tracker() { return peak_heap_tracker_; }

  NumaTopology<kNumaPartitions, kNumBaseClasses>& numa_topology() {
    return numa_topology_;
  }

  static bool multiple_non_numa_partitions() {
    return Parameters::heap_partitioning_mode() != HeapPartitioningMode::kOff;
  }

  size_t active_partitions() {
    return multiple_non_numa_partitions() ? kNormalPartitions
                                          : numa_topology().active_partitions();
  }

  SystemAllocator<NumaTopology<kNumaPartitions, kNumBaseClasses>,
                  kNormalPartitions>&
  system_allocator() {
    return system_allocator_;
  }

  Arena& arena() { return arena_; }

  // Page-level allocator.
  PageAllocator& page_allocator() {
    return *reinterpret_cast<PageAllocator*>(page_allocator_.memory);
  }

  ProdPageMap& pagemap() { return pagemap_; }

  GuardedPageAllocator& guardedpage_allocator() {
    return guardedpage_allocator_;
  }

  MetadataObjectAllocator<SampledAllocation, ArenaAlloc::kSampledAllocation>&
  sampledallocation_allocator() {
    return sampledallocation_allocator_;
  }

  MetadataObjectAllocator<Span, ArenaAlloc::kSpan>& span_allocator() {
    return span_allocator_;
  }

  MetadataObjectAllocator<ThreadCache, ArenaAlloc::kThreadCache>&
  threadcache_allocator() {
    return threadcache_allocator_;
  }

  SampledAllocationRecorder& sampled_allocation_recorder() {
    return sampled_allocation_recorder_;
  }

  // State kept for sampled allocations (/heapz support).
  tcmalloc_internal::StatsCounter sampled_objects_size_;
  // sampled_internal_fragmentation estimates the amount of memory overhead from
  // allocation sizes being rounded up to size class/page boundaries.
  tcmalloc_internal::StatsCounter sampled_internal_fragmentation_;
  // total_sampled_count_ tracks the total number of allocations that are
  // sampled.
  tcmalloc_internal::StatsCounter total_sampled_count_;

  using PerSizeClassCounts = StatsCounters<kNumClasses>;

  PerSizeClassCounts& per_size_class_counts() { return per_size_class_counts_; }

  AllocationSampleList allocation_samples;

  deallocationz::DeallocationProfilerList deallocation_samples;

  // MallocHook::AllocHandle is a simple 64-bit int, and is not dependent on
  // other data.
  std::atomic<int64_t> sampled_alloc_handle_generator = 0;

  MetadataObjectAllocator<StackTraceTable::LinkedSample,
                          ArenaAlloc::kStackTraceTable>&
  linked_sample_allocator() {
    return linked_sample_allocator_;
  }

  bool ABSL_ATTRIBUTE_ALWAYS_INLINE CpuCacheActive() const {
    return cpu_cache_active_.load(std::memory_order_acquire);
  }
  void ActivateCpuCache() {
    cpu_cache_active_.store(true, std::memory_order_release);
  }

  static bool ABSL_ATTRIBUTE_ALWAYS_INLINE HaveHooks() {
    return
        // These boolean operations do not require short-circuiting from &&.
        // Bitwise AND of booleans triggers -Wbitwise-instead-of-logical, as
        // this can be a common source of bugs.  Suppress this by casting to
        // int first.
        static_cast<int>(!new_hooks_.empty()) |
        static_cast<int>(!delete_hooks_.empty());
  }

  size_t metadata_bytes() ABSL_EXCLUSIVE_LOCKS_REQUIRED(pageheap_lock);

  // The root of the pagemap is potentially a large poorly utilized
  // structure, so figure out how much of it is actually resident.
  size_t pagemap_residence();

  GwpAsanState& gwp_asan_state() { return gwp_asan_state_; }

  static SizeClassConfiguration size_class_configuration();

  static const Span& invalid_span() { return kInvalidSpan; }

 private:
#if defined(__clang__)
  __attribute__((preserve_most))
#endif
  void SlowInitIfNecessary();

  // PageHeap uses a constructor for initialization.  Like the members above,
  // we can't depend on initialization order, so pageheap is new'd
  // into this buffer.
  union PageAllocatorStorage {
    constexpr PageAllocatorStorage() : extra(0) {}

    char memory[sizeof(PageAllocator)];
    uintptr_t extra;  // To force alignment
  };

  // Cacheline-align our SizeMap and CpuCache.  They both have very hot arrays
  // as their first member variables, and aligning them reduces the number of
  // cache lines these arrays use.
  Arena arena_;
  ABSL_CACHELINE_ALIGNED SizeMap sizemap_;
  ABSL_CACHELINE_ALIGNED CpuCache<CpuCacheForwarder<Static, tc_globals>>
      cpu_cache_;
  TransferCacheManager transfer_cache_;
  ShardedTransferCacheManager sharded_transfer_cache_{nullptr, nullptr};
  GuardedPageAllocator guardedpage_allocator_;
  MetadataObjectAllocator<SampledAllocation, ArenaAlloc::kSampledAllocation>
      sampledallocation_allocator_;
  MetadataObjectAllocator<Span, ArenaAlloc::kSpan> span_allocator_;
  MetadataObjectAllocator<ThreadCache, ArenaAlloc::kThreadCache>
      threadcache_allocator_;
  MetadataObjectAllocator<StackTraceTable::LinkedSample,
                          ArenaAlloc::kStackTraceTable>
      linked_sample_allocator_;
  std::atomic<bool> inited_ = false;
  std::atomic<bool> cpu_cache_active_ = false;
  PeakHeapTracker peak_heap_tracker_;
  NumaTopology<kNumaPartitions, kNumBaseClasses> numa_topology_;
  GwpAsanState gwp_asan_state_;
  PerSizeClassCounts per_size_class_counts_;
  PageAllocatorStorage page_allocator_;
  ProdPageMap pagemap_;
  SystemAllocator<NumaTopology<kNumaPartitions, kNumBaseClasses>,
                  kNormalPartitions>
      system_allocator_;
  static ABSL_ATTRIBUTE_SECTION_VARIABLE(.data.rel.ro) const Span kInvalidSpan;
  SampledAllocationRecorder sampled_allocation_recorder_;
};

inline bool Static::IsInited() const {
  return inited_.load(std::memory_order_acquire);
}

inline void Static::InitIfNecessary() {
  if (ABSL_PREDICT_FALSE(!IsInited())) {
    SlowInitIfNecessary();
  }
}

// ConstantRatePageAllocatorReleaser() might release more than the requested
// bytes because the page heap releases at the span granularity, and spans are
// of wildly different sizes. This keeps track of the extra bytes bytes released
// so that the app can periodically call Release() to release memory at a
// constant rate.
class ConstantRatePageAllocatorReleaser {
 public:
  size_t Release(size_t num_bytes, PageReleaseReason reason) {
    const PageHeapSpinLockHolder l;

    if (num_bytes <= extra_bytes_released_) {
      // We released too much on a prior call, so don't release any
      // more this time.
      extra_bytes_released_ -= num_bytes;
      num_bytes = 0;
    } else {
      num_bytes -= extra_bytes_released_;
    }

    const Length num_pages = [&] {
      if (num_bytes > 0) {
        // A sub-page size request may round down to zero.  Assume the caller
        // wants some memory released.
        const Length num_pages = BytesToLengthCeil(num_bytes);
        TC_ASSERT_GT(num_pages, Length(0));

        return num_pages;
      } else {
        return Length(0);
      }
    }();

    const size_t bytes_released = tc_globals.page_allocator()
                                      .ReleaseAtLeastNPages(num_pages, reason)
                                      .in_bytes();
    if (bytes_released > num_bytes) {
      extra_bytes_released_ = bytes_released - num_bytes;

      return num_bytes;
    }

    // The PageHeap wasn't able to release num_bytes.  Don't try to compensate
    // with a big release next time.
    extra_bytes_released_ = 0;

    return bytes_released;
  }

 private:
  size_t extra_bytes_released_ = 0;
};

}  // namespace tcmalloc_internal
}  // namespace tcmalloc
GOOGLE_MALLOC_SECTION_END

#endif  // TCMALLOC_STATIC_VARS_H_
