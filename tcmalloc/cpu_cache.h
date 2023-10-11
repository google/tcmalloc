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

#ifndef TCMALLOC_CPU_CACHE_H_
#define TCMALLOC_CPU_CACHE_H_

#include <stddef.h>
#include <stdint.h>
#include <sys/mman.h>

#include <algorithm>
#include <atomic>
#include <new>
#include <tuple>
#include <utility>

#include "absl/algorithm/container.h"
#include "absl/base/attributes.h"
#include "absl/base/call_once.h"
#include "absl/base/internal/cycleclock.h"
#include "absl/base/internal/spinlock.h"
#include "absl/base/internal/sysinfo.h"
#include "absl/base/optimization.h"
#include "absl/base/thread_annotations.h"
#include "absl/container/fixed_array.h"
#include "tcmalloc/common.h"
#include "tcmalloc/internal/allocation_guard.h"
#include "tcmalloc/internal/logging.h"
#include "tcmalloc/internal/optimization.h"
#include "tcmalloc/internal/percpu.h"
#include "tcmalloc/internal/percpu_tcmalloc.h"
#include "tcmalloc/internal/sysinfo.h"
#include "tcmalloc/parameters.h"
#include "tcmalloc/static_vars.h"
#include "tcmalloc/thread_cache.h"

GOOGLE_MALLOC_SECTION_BEGIN
namespace tcmalloc {
namespace tcmalloc_internal {
class CpuCachePeer;

namespace cpu_cache_internal {
template <class CpuCache>
struct DrainHandler;

// Determine number of bits we should use for allocating per-cpu cache.
// The amount of per-cpu cache is 2 ^ per-cpu-shift.
// When dynamic slab size is enabled, we start with kInitialPerCpuShift and
// grow as needed up to kMaxPerCpuShift. When dynamic slab size is disabled,
// we always use kMaxPerCpuShift.
#if defined(TCMALLOC_SMALL_BUT_SLOW)
constexpr inline uint8_t kInitialBasePerCpuShift = 12;
constexpr inline uint8_t kMaxBasePerCpuShift = 12;
#else
constexpr inline uint8_t kInitialBasePerCpuShift = 14;
constexpr inline uint8_t kMaxBasePerCpuShift = 18;
#endif
constexpr inline uint8_t kNumPossiblePerCpuShifts =
    kMaxBasePerCpuShift - kInitialBasePerCpuShift + 1;

// StaticForwarder provides access to the SizeMap and transfer caches.
//
// This is a class, rather than namespaced globals, so that it can be mocked for
// testing.
class StaticForwarder {
 public:
  static void* Alloc(size_t size, std::align_val_t alignment)
      ABSL_LOCKS_EXCLUDED(pageheap_lock) {
    ASSERT(tc_globals.IsInited());
    AllocationGuardSpinLockHolder l(&pageheap_lock);
    return tc_globals.arena().Alloc(size, alignment);
  }
  static void* AllocReportedImpending(size_t size, std::align_val_t alignment)
      ABSL_LOCKS_EXCLUDED(pageheap_lock) {
    ASSERT(tc_globals.IsInited());
    AllocationGuardSpinLockHolder l(&pageheap_lock);
    // Negate previous update to allocated that accounted for this allocation.
    tc_globals.arena().UpdateAllocatedAndNonresident(
        -static_cast<int64_t>(size), 0);
    return tc_globals.arena().Alloc(size, alignment);
  }

  static void Dealloc(void* ptr, size_t size, std::align_val_t alignment) {
    ASSERT(false);
  }

  static void ArenaUpdateAllocatedAndNonresident(int64_t allocated,
                                                 int64_t nonresident)
      ABSL_LOCKS_EXCLUDED(pageheap_lock) {
    ASSERT(tc_globals.IsInited());
    AllocationGuardSpinLockHolder l(&pageheap_lock);
    tc_globals.arena().UpdateAllocatedAndNonresident(allocated, nonresident);
  }

  static void ShrinkToUsageLimit() ABSL_LOCKS_EXCLUDED(pageheap_lock) {
    ASSERT(tc_globals.IsInited());
    AllocationGuardSpinLockHolder l(&pageheap_lock);
    tc_globals.page_allocator().ShrinkToUsageLimit(Length(0));
  }

  static bool per_cpu_caches_dynamic_slab_enabled() {
    return Parameters::per_cpu_caches_dynamic_slab_enabled();
  }

  static bool resize_size_classes_enabled() {
    return Parameters::resize_cpu_cache_size_classes();
  }

  static double per_cpu_caches_dynamic_slab_grow_threshold() {
    return Parameters::per_cpu_caches_dynamic_slab_grow_threshold();
  }

  static double per_cpu_caches_dynamic_slab_shrink_threshold() {
    return Parameters::per_cpu_caches_dynamic_slab_shrink_threshold();
  }

  static size_t class_to_size(int size_class) {
    return tc_globals.sizemap().class_to_size(size_class);
  }

  static absl::Span<const size_t> cold_size_classes() {
    return tc_globals.sizemap().ColdSizeClasses();
  }

  static size_t num_objects_to_move(int size_class) {
    return tc_globals.sizemap().num_objects_to_move(size_class);
  }

  static const NumaTopology<kNumaPartitions, kNumBaseClasses>& numa_topology() {
    return tc_globals.numa_topology();
  }

  static ShardedTransferCacheManager& sharded_transfer_cache() {
    return tc_globals.sharded_transfer_cache();
  }

  static TransferCacheManager& transfer_cache() {
    return tc_globals.transfer_cache();
  }

  static bool UseGenericShardedCache() {
    return tc_globals.sharded_transfer_cache().UseGenericCache();
  }

  static bool UseShardedCacheForLargeClassesOnly() {
    return tc_globals.sharded_transfer_cache().UseCacheForLargeClassesOnly();
  }

  static bool UseWiderSlabs() {
    // We use wider 512KiB slab only when NUMA partitioning is not enabled. NUMA
    // increases shift by 1 by itself, so we can not increase it further.
    return IsExperimentActive(Experiment::TEST_ONLY_TCMALLOC_512K_SLAB) &&
           !numa_topology().numa_aware();
  }
};

template <typename NumaTopology>
uint8_t NumaShift(const NumaTopology& topology) {
  return topology.numa_aware()
             ? absl::bit_ceil(topology.active_partitions() - 1)
             : 0;
}

// Translates from a shift value to the offset of that shift in arrays of
// possible shift values.
inline uint8_t ShiftOffset(uint8_t shift, uint8_t initial_shift) {
  ASSERT(shift >= initial_shift);
  return shift - initial_shift;
}

// Tracks the range of allowed slab shifts.
struct SlabShiftBounds {
  uint8_t initial_shift;
  uint8_t max_shift;
};

struct GetShiftMaxCapacity {
  size_t operator()(size_t size_class) const {
    ASSERT(shift_bounds.max_shift >= shift);
    const uint8_t relative_shift = shift_bounds.max_shift - shift;
    if (relative_shift == 0) return max_capacities[size_class];
    int mc = max_capacities[size_class] >> relative_shift;
    // We decrement by 3 because of (1) cost of per-size-class header, (2) cost
    // of per-size-class padding pointer, (3) there are a lot of empty size
    // classes that have headers and whose max capacities can't be decremented.
    // TODO(b/272085443): try using size_class_to_header_idx array to allow for
    // not having headers for empty size classes.
    // TODO(b/219565872): try not doing prefetching for large size classes to
    // allow for not having padding pointers for large size classes.
    mc = std::max(mc - 3, 0);
    return mc;
  }

  const uint16_t* max_capacities;
  uint8_t shift;
  SlabShiftBounds shift_bounds;
};

template <typename Forwarder>
class CpuCache {
 public:
  struct CpuCacheMissStats {
    size_t underflows = 0;
    size_t overflows = 0;

    CpuCacheMissStats& operator+=(const CpuCacheMissStats rhs) {
      underflows += rhs.underflows;
      overflows += rhs.overflows;
      return *this;
    }
  };

  enum class DynamicSlabResize {
    kNoop = 0,
    kShrink,
    kGrow,
  };

  enum class PerClassMissType {
    // Tracks total number of misses.
    kTotal = 0,
    // Tracks number of misses recorded as of the end of the last per-class
    // resize interval.
    kResize,
    kNumTypes,
  };

  // We track the number of overflows/underflows for each of these cases.
  enum class MissCount {
    // Tracks total number of misses.
    kTotal = 0,
    // Tracks number of misses recorded as of the end of the last shuffle
    // interval.
    kShuffle,
    // Tracks number of misses recorded as of the end of the last resize
    // interval.
    kReclaim,
    // Tracks number of misses recorded as of the end of the last slab resize
    // interval.
    kSlabResize,
    kNumCounts,
  };

  struct SizeClassCapacityStats {
    size_t min_capacity = 0;
    double avg_capacity = 0;
    size_t max_capacity = 0;
    absl::Duration min_last_underflow = absl::InfiniteDuration();
    absl::Duration max_last_underflow;
    absl::Duration min_last_overflow = absl::InfiniteDuration();
    absl::Duration max_last_overflow;
    int min_last_underflow_cpu_id = -1;
    int max_last_underflow_cpu_id = -1;
    int min_last_overflow_cpu_id = -1;
    int max_last_overflow_cpu_id = -1;
  };

  // Sets the lower limit on the capacity that can be stolen from the cpu cache.
  static constexpr double kCacheCapacityThreshold = 0.20;

  constexpr CpuCache() = default;

  // tcmalloc explicitly initializes its global state (to be safe for
  // use in global constructors) so our constructor must be trivial;
  // do all initialization here instead.
  void Activate();

  // For testing
  void Deactivate();

  // Allocate an object of the given size class. When allocation fails
  // (from this cache and after running Refill), Policy::oom_handler(size) is
  // called and its return value is returned from Allocate.
  // Policy::oom_handler is used to parameterize out-of-memory
  // handling (raising exception, returning nullptr, calling
  // new_handler or anything else). "Passing" oom handlers in this way
  // through policies allows Allocate to be used in tail-call position in
  // fast-path, making Allocate use jump (tail-call) to slow path code.
  template <class Policy>
  auto Allocate(size_t size_class);

  // Free an object of the given class.
  void Deallocate(void* ptr, size_t size_class);

  // Give the number of bytes in <cpu>'s cache
  uint64_t UsedBytes(int cpu) const;

  // Give the allocated number of bytes in <cpu>'s cache
  uint64_t Allocated(int cpu) const;

  // Whether <cpu>'s cache has ever been populated with objects
  bool HasPopulated(int cpu) const;

  PerCPUMetadataState MetadataMemoryUsage() const;

  // Give the number of bytes used in all cpu caches.
  uint64_t TotalUsedBytes() const;

  // Give the number of objects of a given class in all cpu caches.
  uint64_t TotalObjectsOfClass(size_t size_class) const;

  // Give the number of bytes unallocated to any sizeclass in <cpu>'s cache.
  uint64_t Unallocated(int cpu) const;

  // Gives the total capacity of <cpu>'s cache in bytes.
  //
  // The total capacity of <cpu>'s cache should be equal to the sum of allocated
  // and unallocated bytes for that cache.
  uint64_t Capacity(int cpu) const;

  // Give the per-cpu limit of cache size.
  uint64_t CacheLimit() const;
  void SetCacheLimit(uint64_t v);

  // Shuffles per-cpu caches using the number of underflows and overflows that
  // occurred in the prior interval. It selects the top per-cpu caches
  // with highest misses as candidates, iterates through the other per-cpu
  // caches to steal capacity from them and adds the stolen bytes to the
  // available capacity of the per-cpu caches. May be called from any processor.
  //
  // TODO(vgogte): There are quite a few knobs that we can play around with in
  // ShuffleCpuCaches.
  void ShuffleCpuCaches();

  // Tries to reclaim inactive per-CPU caches. It iterates through the set of
  // populated cpu caches and reclaims the caches that:
  // (1) had same number of used bytes since the last interval,
  // (2) had no change in the number of misses since the last interval.
  void TryReclaimingCaches();

  // Resize size classes for up to kNumCpuCachesToResize cpu caches per
  // interval.
  static constexpr int kNumCpuCachesToResize = 10;
  // Resizes size classes within up to kNumCpuCachesToResize per-cpu caches per
  // iteration in a round-robin fashion. Per cpu cache, it iterates through the
  // size classes and attempts to grow up to kMaxSizeClassesToResize number of
  // classes by stealing capacity from rest of them. Per iteration, it resizes
  // size classes for up to kNumCpuCachesToResize number of per-cpu caches.
  void ResizeSizeClasses();

  // Empty out the cache on <cpu>; move all objects to the central
  // cache.  (If other threads run concurrently on that cpu, we can't
  // guarantee it will be fully empty on return, but if the cpu is
  // unused, this will eliminate stranded memory.)  Returns the number
  // of bytes we sent back.  This function is thread safe.
  uint64_t Reclaim(int cpu);

  // Reports number of times the size classes were resized for <cpu>.
  uint64_t GetNumResizes(int cpu) const;

  // Reports total number of times size classes were resized.
  uint64_t GetNumResizes() const;

  // Reports number of times the <cpu> has been reclaimed.
  uint64_t GetNumReclaims(int cpu) const;

  // Reports total number of times any CPU has been reclaimed.
  uint64_t GetNumReclaims() const;

  // When dynamic slab size is enabled, checks if there is a need to resize
  // the slab based on miss-counts and resizes if so.
  void ResizeSlabIfNeeded();

  // Reports total cache underflows and overflows for <cpu>.
  CpuCacheMissStats GetTotalCacheMissStats(int cpu) const;

  // Reports total cache underflows and overflows for all CPUs.
  CpuCacheMissStats GetTotalCacheMissStats() const;

  // Reports the cache underflows and overflows for <cpu> that were recorded
  // during the previous interval for <miss_count>.
  CpuCacheMissStats GetIntervalCacheMissStats(int cpu,
                                              MissCount miss_count) const;

  // Records current underflows and overflows in the <miss_count> underflow and
  // overflow stats.
  void UpdateIntervalCacheMissStats(int cpu, MissCount miss_count);

  // Reports the cache underflows and overflows for <cpu> that were recorded
  // during the previous interval for <miss_count>. Records current underflows
  // and overflows in the <miss_count> underflow and overflow stats.
  CpuCacheMissStats GetAndUpdateIntervalCacheMissStats(int cpu,
                                                       MissCount miss_count);

  // Scans through populated per-CPU caches, and reports minimum, average and
  // maximum capacity for size class <size_class>.
  SizeClassCapacityStats GetSizeClassCapacityStats(size_t size_class) const;

  // Reports the number of misses encountered by a <size_class> that were
  // recorded during the previous interval for the miss <type>.
  size_t GetIntervalSizeClassMisses(int cpu, size_t size_class,
                                    PerClassMissType type);

  // Reports if we should use a wider 512KiB slab.
  bool UseWiderSlabs() const;

  // Reports allowed slab shift initial and maximum bounds.
  SlabShiftBounds GetPerCpuSlabShiftBounds() const;

  // Report statistics
  void Print(Printer* out) const;
  void PrintInPbtxt(PbtxtRegion* region) const;

  const Forwarder& forwarder() const { return forwarder_; }

  Forwarder& forwarder() { return forwarder_; }

 private:
  friend struct DrainHandler<CpuCache>;
  friend class ::tcmalloc::tcmalloc_internal::CpuCachePeer;

  using Freelist = subtle::percpu::TcmallocSlab<kNumClasses>;

  // Return a set of objects to be returned to the Transfer Cache.
  static constexpr int kMaxToReturn = 16;
  struct ObjectsToReturn {
    // The number of slots available for storing objects.
    int count = kMaxToReturn;
    // The size class of the returned object. kNumClasses is the
    // largest value that needs to be stored in size_class.
    CompactSizeClass size_class[kMaxToReturn];
    void* obj[kMaxToReturn];
  };

  struct PerClassMissCounts {
    std::atomic<size_t>
        misses[static_cast<size_t>(PerClassMissType::kNumTypes)];

    std::atomic<size_t>& operator[](PerClassMissType type) {
      return misses[static_cast<size_t>(type)];
    }
  };

  // Per-size-class freelist resizing info.
  class PerClassResizeInfo {
   public:
    void Init();
    // Updates info on overflow/underflow.
    // <overflow> says if it's overflow or underflow.
    // <grow> is caller approximation of whether we want to grow capacity.
    // <successive> will contain number of successive overflows/underflows.
    // Returns if capacity needs to be grown aggressively (i.e. by batch size).
    bool Update(bool overflow, bool grow, uint32_t* successive);
    uint32_t Tick();

    // Records a miss. A miss occurs when size class attempts to grow it's
    // capacity on underflow/overflow, but we are already at the maximum
    // configured per-cpu cache capacity limit.
    void RecordMiss();

    // Reports total number of misses recorded for this size class.
    size_t GetTotalMisses();

    // Reports the number of misses encountered by this size class that
    // were recorded during the previous interval for the miss <type>.
    size_t GetIntervalMisses(PerClassMissType type);

    // Records current misses encountered by this size class for the miss
    // <type>.
    void UpdateIntervalMisses(PerClassMissType type);

   private:
    std::atomic<int32_t> state_;
    // state_ layout:
    struct State {
      // last overflow/underflow?
      uint32_t overflow : 1;
      // number of times Steal checked this class since the last grow
      uint32_t quiescent_ticks : 15;
      // number of successive overflows/underflows
      uint32_t successive : 16;
    };
    PerClassMissCounts misses_;
    static_assert(sizeof(State) == sizeof(std::atomic<int32_t>),
                  "size mismatch");
  };

  // Helper type so we don't need to sprinkle `static_cast`s everywhere.
  struct MissCounts {
    std::atomic<size_t> misses[static_cast<size_t>(MissCount::kNumCounts)];

    std::atomic<size_t>& operator[](MissCount miss_count) {
      return misses[static_cast<size_t>(miss_count)];
    }
  };

  struct ABSL_CACHELINE_ALIGNED ResizeInfo {
    // cache space on this CPU we're not using.  Modify atomically;
    // we don't want to lose space.
    std::atomic<size_t> available;
    // this is just a hint
    std::atomic<size_t> last_steal;
    // Track whether we have initialized this CPU.
    absl::once_flag initialized;
    // Track whether we have ever populated this CPU.
    std::atomic<bool> populated;
    // For cross-cpu operations. We can't allocate while holding one of these so
    // please use AllocationGuardSpinLockHolder to hold it.
    absl::base_internal::SpinLock lock ABSL_ACQUIRED_BEFORE(pageheap_lock);
    PerClassResizeInfo per_class[kNumClasses];
    std::atomic<size_t> num_size_class_resizes;
    // Tracks number of underflows on allocate.
    MissCounts underflows;
    // Tracks number of overflows on deallocate.
    MissCounts overflows;
    std::atomic<int64_t> last_miss_cycles[2][kNumClasses];
    // total cache space available on this CPU. This tracks the total
    // allocated and unallocated bytes on this CPU cache.
    std::atomic<size_t> capacity;
    // Used bytes in the cache as of the end of the last resize interval.
    std::atomic<uint64_t> reclaim_used_bytes;
    // Tracks number of times this CPU has been reclaimed.
    std::atomic<size_t> num_reclaims;
    // Tracks last time this CPU was reclaimed.  If last underflow/overflow data
    // appears before this point in time, we ignore the CPU.
    std::atomic<int64_t> last_reclaim;
  };

  struct DynamicSlabInfo {
    std::atomic<size_t> grow_count[kNumPossiblePerCpuShifts];
    std::atomic<size_t> shrink_count[kNumPossiblePerCpuShifts];
    std::atomic<size_t> madvise_failed_bytes;
  };

  // Determines how we distribute memory in the per-cpu cache to the various
  // class sizes.
  size_t MaxCapacity(size_t size_class) const;

  // Gets the max capacity for the size class using the current per-cpu shift.
  uint16_t GetMaxCapacity(int size_class, uint8_t shift) const;

  GetShiftMaxCapacity GetMaxCapacityFunctor(uint8_t shift) const;

  // Fetches objects from backing transfer cache.
  int FetchFromBackingCache(size_t size_class, void** batch, size_t count);

  // Releases free batch of objects to the backing transfer cache.
  void ReleaseToBackingCache(size_t size_class, absl::Span<void*> batch);

  void* Refill(int cpu, size_t size_class);

  // This is called after finding a full freelist when attempting to push <ptr>
  // on the freelist for sizeclass <size_class>.  The last arg should indicate
  // which CPU's list was full.  Returns 1.
  int Overflow(void* ptr, size_t size_class, int cpu);

  // Returns true if we bypass cpu cache for a <size_class>. We may bypass
  // per-cpu cache when we enable certain configurations of sharded transfer
  // cache.
  bool BypassCpuCache(size_t size_class) const;

  // Returns true if we use sharded transfer cache as a backing cache for
  // per-cpu caches. If a sharded transfer cache is used, we fetch/release
  // from/to a sharded transfer cache. Else, we use a legacy transfer cache.
  bool UseBackingShardedTransferCache(size_t size_class) const;

  // Called on <size_class> freelist overflow/underflow on <cpu> to balance
  // cache capacity between size classes. Returns number of objects to
  // return/request from transfer cache. <to_return> will contain objects that
  // need to be freed.
  size_t UpdateCapacity(int cpu, size_t size_class, bool overflow,
                        ObjectsToReturn* to_return);

  // Tries to obtain up to <desired_increase> bytes of freelist space on <cpu>
  // for <size_class> from other <cls>. <to_return> will contain objects that
  // need to be freed.
  void Grow(int cpu, size_t size_class, size_t desired_increase,
            ObjectsToReturn* to_return);

  // Tries to steal <bytes> for <size_class> on <cpu> from other size classes on
  // that CPU. Returns acquired bytes. <to_return> will contain objects that
  // need to be freed.
  size_t Steal(int cpu, size_t size_class, size_t bytes,
               ObjectsToReturn* to_return);

  // Depending on the number of misses that cpu caches encountered in the
  // previous resize interval, returns if slabs should be grown, shrunk or
  // remain the same.
  DynamicSlabResize ShouldResizeSlab();

  // Determine if the <size_class> is a good candidate to be shrunk. We use
  // clock-like algorithm to prioritize size classes for shrinking.
  bool IsGoodCandidateForShrinking(int cpu, size_t size_class);

  // Tries to steal <bytes> for <size_class> on <cpu> from other size classes on
  // that CPU. Returns acquired bytes. <to_return> will contain objects that
  // need to be freed. Unlike Steal, this method may be called from a different
  // cpu.
  size_t StealCapacityForSizeClassWithinCpu(int cpu, size_t size_class,
                                            size_t bytes);

  // Records a cache underflow or overflow on <cpu>, increments underflow or
  // overflow by 1.
  // <is_alloc> determines whether the associated count corresponds to an
  // underflow or overflow.
  void RecordCacheMissStat(int cpu, bool is_alloc);

  static void* NoopUnderflow(int cpu, size_t size_class, void* arg) {
    return nullptr;
  }
  static int NoopOverflow(int cpu, size_t size_class, void* item, void* arg) {
    return -1;
  }

  // Tries to steal <bytes> for the destination <cpu>. It iterates through the
  // the set of populated cpu caches and steals the bytes from them. A cpu is
  // considered a good candidate to steal from if:
  // (1) the cache is populated
  // (2) the numbers of underflows and overflows are both less than 0.8x those
  // of the destination per-cpu cache
  // (3) source cpu is not the same as the destination cpu
  // (4) capacity of the source cpu/size_class is non-zero
  //
  // For a given source cpu, we iterate through the size classes to steal from
  // them. Currently, we use a similar clock-like algorithm from Steal() to
  // identify the size_class to steal from.
  void StealFromOtherCache(int cpu, int max_populated_cpu, size_t bytes);

  // Resizes capacities of up to kMaxSizeClassesToResize size classes for a
  // single <cpu>.
  void ResizeCpuSizeClasses(int cpu);

  // <shift_offset> is the offset of the shift in slabs_by_shift_. Note that we
  // can't calculate this from `shift` directly due to numa shift.
  // Returns the allocated slabs and the number of reused bytes.
  ABSL_MUST_USE_RESULT std::pair<Freelist::Slabs*, size_t> AllocOrReuseSlabs(
      absl::FunctionRef<void*(size_t, std::align_val_t)> alloc,
      subtle::percpu::Shift shift, int num_cpus, uint8_t shift_offset);

  Freelist freelist_;

  // Tracking data for each CPU's cache resizing efforts.
  ResizeInfo* resize_ = nullptr;

  // Tracks initial and maximum slab shift bounds.
  SlabShiftBounds shift_bounds_{};

  // The maximum capacity of each size class within the slab.
  uint16_t max_capacity_[kNumClasses] = {0};

  // Provides a hint to StealFromOtherCache() so that we can steal from the
  // caches in a round-robin fashion.
  std::atomic<int> last_cpu_cache_steal_ = 0;

  // Provides a hint to ResizeSizeClasses() that records the last CPU for which
  // we resized size classes. We use this to resize size classes for CPUs in a
  // round-robin fashion.
  std::atomic<int> last_cpu_size_class_resize_ = 0;

  // Per-core cache limit in bytes.
  std::atomic<uint64_t> max_per_cpu_cache_size_{kMaxCpuCacheSize};

  ABSL_ATTRIBUTE_NO_UNIQUE_ADDRESS Forwarder forwarder_;

  DynamicSlabInfo dynamic_slab_info_{};

  // Pointers to allocations for slabs of each shift value for use in
  // ResizeSlabs. This memory is allocated on the arena, and it is nonresident
  // while not in use.
  Freelist::Slabs* slabs_by_shift_[kNumPossiblePerCpuShifts] = {nullptr};
};

template <class Forwarder>
template <class Policy>
inline ABSL_ATTRIBUTE_ALWAYS_INLINE auto CpuCache<Forwarder>::Allocate(
    size_t size_class) {
  ASSERT(size_class > 0);

  struct Helper {
    static auto ABSL_ATTRIBUTE_NOINLINE Underflow(int cpu, size_t size_class,
                                                  void* arg) {
      CpuCache& cache = *static_cast<CpuCache*>(arg);
      void* ret;
      if (cache.BypassCpuCache(size_class)) {
        ret = cache.forwarder().sharded_transfer_cache().Pop(size_class);
      } else {
        cache.RecordCacheMissStat(cpu, true);
        ret = cache.Refill(cpu, size_class);
      }
      if (ABSL_PREDICT_FALSE(ret == nullptr)) {
        size_t size = cache.forwarder().class_to_size(size_class);
        return Policy::handle_oom(size);
      }
      return Policy::to_pointer(ret, size_class);
    }
  };
  return freelist_.Pop<Policy>(size_class, &Helper::Underflow, this);
}

template <class Forwarder>
inline void ABSL_ATTRIBUTE_ALWAYS_INLINE
CpuCache<Forwarder>::Deallocate(void* ptr, size_t size_class) {
  ASSERT(size_class > 0);

  struct Helper {
    static int ABSL_ATTRIBUTE_NOINLINE Overflow(int cpu, size_t size_class,
                                                void* ptr, void* arg) {
      CpuCache& cache = *static_cast<CpuCache*>(arg);
      if (cache.BypassCpuCache(size_class)) {
        cache.forwarder().sharded_transfer_cache().Push(size_class, ptr);
        return 1;
      }
      cache.RecordCacheMissStat(cpu, false);
      return cache.Overflow(ptr, size_class, cpu);
    }
  };
  freelist_.Push(size_class, ptr, Helper::Overflow, this);
}

static cpu_set_t FillActiveCpuMask() {
  cpu_set_t allowed_cpus;
  if (sched_getaffinity(0, sizeof(allowed_cpus), &allowed_cpus) != 0) {
    CPU_ZERO(&allowed_cpus);
  }

#ifdef PERCPU_USE_RSEQ
  const bool real_cpus = !subtle::percpu::UsingFlatVirtualCpus();
#else
  const bool real_cpus = true;
#endif

  if (real_cpus) {
    return allowed_cpus;
  }

  const int virtual_cpu_count = CPU_COUNT(&allowed_cpus);
  CPU_ZERO(&allowed_cpus);
  for (int cpu = 0; cpu < virtual_cpu_count; ++cpu) {
    CPU_SET(cpu, &allowed_cpus);
  }
  return allowed_cpus;
}

template <class Forwarder>
inline size_t CpuCache<Forwarder>::MaxCapacity(size_t size_class) const {
  // The number of size classes that are commonly used and thus should be
  // allocated more slots in the per-cpu cache.
  static constexpr size_t kNumSmall = 10;

  // When we use wider slabs, we also want to double the maximum capacities for
  // size classes to use that slab.
  const size_t kWiderSlabMultiplier = UseWiderSlabs() ? 2 : 1;

  // The memory used for each per-CPU slab is the sum of:
  //   sizeof(std::atomic<int64_t>) * kNumClasses
  //   sizeof(void*) * (kSmallObjectDepth + 1) * kNumSmall
  //   sizeof(void*) * (kLargeObjectDepth + 1) * kNumLarge
  //
  // Class size 0 has MaxCapacity() == 0, which is the reason for using
  // kNumClasses - 1 above instead of kNumClasses.
  //
  // Each Size class region in the slab is preceded by one padding pointer that
  // points to itself, because prefetch instructions of invalid pointers are
  // slow. That is accounted for by the +1 for object depths.
#if defined(TCMALLOC_SMALL_BUT_SLOW)
  // With SMALL_BUT_SLOW we have 4KiB of per-cpu slab and 46 class sizes we
  // allocate:
  //   == 8 * 46 + 8 * ((16 + 1) * 10 + (6 + 1) * 35) = 4038 bytes of 4096
  static const uint16_t kSmallObjectDepth = 16;
  static const uint16_t kLargeObjectDepth = 6;
#else
  // We allocate 256KiB per-cpu for pointers to cached per-cpu memory.
  // Each 256KiB is a subtle::percpu::TcmallocSlab::Slabs
  // Max(kNumClasses) is 89, so the maximum footprint per CPU for a 256KiB
  // slab is:
  //   89 * 8 + 8 * ((2048 + 1) * 10 + (152 + 1) * 78) = 254 KiB
  // For 512KiB slab, with a multiplier of 2, maximum footprint is:
  //   89 * 8 + 8 * ((4096 + 1) * 10 + (304 + 1) * 78) = 506 KiB
  static const uint16_t kSmallObjectDepth = 2048 * kWiderSlabMultiplier;
  static const uint16_t kLargeObjectDepth = 152 * kWiderSlabMultiplier;
#endif
  if (size_class == 0 || size_class >= kNumClasses) return 0;

  if (BypassCpuCache(size_class)) {
    return 0;
  }

  if (forwarder_.class_to_size(size_class) == 0) {
    return 0;
  }

  if (!IsExpandedSizeClass(size_class) &&
      (size_class % kNumBaseClasses) <= kNumSmall) {
    // Small object sizes are very heavily used and need very deep caches for
    // good performance (well over 90% of malloc calls are for size_class
    // <= 10.)
    return kSmallObjectDepth;
  }

  if (ColdFeatureActive()) {
    // We reduce the number of cached objects for some sizes to fit into the
    // slab.
    static const uint16_t kLargeUninterestingObjectDepth =
        133 * kWiderSlabMultiplier;
    static const uint16_t kLargeInterestingObjectDepth =
        152 * kWiderSlabMultiplier;

    absl::Span<const size_t> cold = forwarder_.cold_size_classes();
    if (absl::c_binary_search(cold, size_class)) {
      return kLargeInterestingObjectDepth;
    } else if (!IsExpandedSizeClass(size_class)) {
      return kLargeUninterestingObjectDepth;
    } else {
      return 0;
    }
  }

  if (IsExpandedSizeClass(size_class)) {
    return 0;
  }

  return kLargeObjectDepth;
}

// Returns estimated bytes required and the bytes available.
inline std::pair<size_t, size_t> EstimateSlabBytes(
    GetShiftMaxCapacity get_shift_capacity) {
  size_t bytes_required = sizeof(std::atomic<int64_t>) * kNumClasses;

  for (int size_class = 0; size_class < kNumClasses; ++size_class) {
    // Each non-empty size class region in the slab is preceded by one padding
    // pointer that points to itself. (We do this because prefetches of invalid
    // pointers are slow.)
    size_t num_pointers = get_shift_capacity(size_class);
    if (num_pointers > 0) ++num_pointers;
    bytes_required += sizeof(void*) * num_pointers;
  }

  const size_t bytes_available = 1 << get_shift_capacity.shift;
  return {bytes_required, bytes_available};
}

template <class Forwarder>
inline uint16_t CpuCache<Forwarder>::GetMaxCapacity(int size_class,
                                                    uint8_t shift) const {
  return GetMaxCapacityFunctor(shift)(size_class);
}

template <class Forwarder>
inline GetShiftMaxCapacity CpuCache<Forwarder>::GetMaxCapacityFunctor(
    uint8_t shift) const {
  return {max_capacity_, shift, shift_bounds_};
}

template <class Forwarder>
inline bool CpuCache<Forwarder>::UseWiderSlabs() const {
  return forwarder_.UseWiderSlabs();
}

template <class Forwarder>
inline SlabShiftBounds CpuCache<Forwarder>::GetPerCpuSlabShiftBounds() const {
  return shift_bounds_;
}

template <class Forwarder>
inline void CpuCache<Forwarder>::Activate() {
  int num_cpus = NumCPUs();

  shift_bounds_.initial_shift = kInitialBasePerCpuShift;
  shift_bounds_.max_shift = kMaxBasePerCpuShift;
  uint8_t per_cpu_shift = forwarder_.per_cpu_caches_dynamic_slab_enabled()
                              ? kInitialBasePerCpuShift
                              : kMaxBasePerCpuShift;

  const auto& topology = forwarder_.numa_topology();
  const uint8_t numa_shift = NumaShift(topology);
  const uint8_t wider_slab_shift = UseWiderSlabs() ? 1 : 0;

  shift_bounds_.initial_shift += numa_shift + wider_slab_shift;
  shift_bounds_.max_shift += numa_shift + wider_slab_shift;
  per_cpu_shift += numa_shift + wider_slab_shift;

  CHECK_CONDITION(shift_bounds_.initial_shift <= shift_bounds_.max_shift);
  CHECK_CONDITION(per_cpu_shift >= shift_bounds_.initial_shift &&
                  per_cpu_shift <= shift_bounds_.max_shift);
  CHECK_CONDITION(shift_bounds_.max_shift - shift_bounds_.initial_shift + 1 ==
                  kNumPossiblePerCpuShifts);

  // Deal with size classes that correspond only to NUMA partitions that are in
  // use. If NUMA awareness is disabled then we may have a smaller shift than
  // would suffice for all of the unused size classes.
  for (int size_class = 0;
       size_class < topology.active_partitions() * kNumBaseClasses;
       ++size_class) {
    max_capacity_[size_class] = MaxCapacity(size_class);
  }

  // Deal with expanded size classes.
  for (int size_class = kExpandedClassesStart; size_class < kNumClasses;
       ++size_class) {
    max_capacity_[size_class] = MaxCapacity(size_class);
  }

  // Verify that all the possible shifts will have valid max capacities.
  for (uint8_t shift = shift_bounds_.initial_shift;
       shift <= shift_bounds_.max_shift; ++shift) {
    const auto [bytes_required, bytes_available] =
        EstimateSlabBytes({max_capacity_, shift, shift_bounds_});
    // We may make certain size classes no-ops by selecting "0" at runtime, so
    // using a compile-time calculation overestimates worst-case memory usage.
    if (ABSL_PREDICT_FALSE(bytes_required > bytes_available)) {
      Crash(kCrash, __FILE__, __LINE__, "per-CPU memory exceeded, have ",
            bytes_available, " need ", bytes_required);
    }
  }

  resize_ = reinterpret_cast<ResizeInfo*>(forwarder_.Alloc(
      sizeof(ResizeInfo) * num_cpus, std::align_val_t{alignof(ResizeInfo)}));

  auto max_cache_size = CacheLimit();

  for (int cpu = 0; cpu < num_cpus; ++cpu) {
    new (&resize_[cpu]) ResizeInfo();

    for (int size_class = 1; size_class < kNumClasses; ++size_class) {
      resize_[cpu].per_class[size_class].Init();
    }
    resize_[cpu].available.store(max_cache_size, std::memory_order_relaxed);
    resize_[cpu].capacity.store(max_cache_size, std::memory_order_relaxed);
    resize_[cpu].last_steal.store(1, std::memory_order_relaxed);
  }

  Freelist::Slabs* slabs =
      AllocOrReuseSlabs(&forwarder_.Alloc,
                        subtle::percpu::ToShiftType(per_cpu_shift), num_cpus,
                        ShiftOffset(per_cpu_shift, shift_bounds_.initial_shift))
          .first;
  freelist_.Init(
      slabs, GetShiftMaxCapacity{max_capacity_, per_cpu_shift, shift_bounds_},
      subtle::percpu::ToShiftType(per_cpu_shift));
}

template <class Forwarder>
inline void CpuCache<Forwarder>::Deactivate() {
  int num_cpus = NumCPUs();
  for (int i = 0; i < num_cpus; i++) {
    Reclaim(i);
  }

  freelist_.Destroy(&forwarder_.Dealloc);
  static_assert(std::is_trivially_destructible<decltype(*resize_)>::value,
                "ResizeInfo is expected to be trivially destructible");
  forwarder_.Dealloc(resize_, sizeof(*resize_) * num_cpus,
                     std::align_val_t{alignof(decltype(*resize_))});
}

template <class Forwarder>
inline int CpuCache<Forwarder>::FetchFromBackingCache(size_t size_class,
                                                      void** batch,
                                                      size_t count) {
  if (UseBackingShardedTransferCache(size_class)) {
    return forwarder_.sharded_transfer_cache().RemoveRange(size_class, batch,
                                                           count);
  }
  return forwarder_.transfer_cache().RemoveRange(size_class, batch, count);
}

template <class Forwarder>
inline void CpuCache<Forwarder>::ReleaseToBackingCache(
    size_t size_class, absl::Span<void*> batch) {
  if (UseBackingShardedTransferCache(size_class)) {
    forwarder_.sharded_transfer_cache().InsertRange(size_class, batch);
    return;
  }

  forwarder_.transfer_cache().InsertRange(size_class, batch);
}

// Fetch more items from the central cache, refill our local cache,
// and try to grow it if necessary.
//
// This is complicated by the fact that we can only tweak the cache on
// our current CPU and we might get migrated whenever (in fact, we
// might already have been migrated since failing to get memory...)
//
// So make sure only to make changes to one CPU's cache; at all times,
// it must be safe to find ourselves migrated (at which point we atomically
// return memory to the correct CPU.)
template <class Forwarder>
inline void* CpuCache<Forwarder>::Refill(int cpu, size_t size_class) {
  // UpdateCapacity can evict objects from other size classes as it tries to
  // increase capacity of this size class. The objects are returned in
  // to_return, we insert them into transfer cache at the end of function
  // (to increase possibility that we stay on the current CPU as we are
  // refilling the list).
  ObjectsToReturn to_return;
  const size_t target = UpdateCapacity(cpu, size_class, false, &to_return);

  // Refill target objects in batch_length batches.
  size_t total = 0;
  size_t got;
  size_t i;
  void* result = nullptr;
  void* batch[kMaxObjectsToMove];

  do {
    const size_t want = std::min(kMaxObjectsToMove, target - total);
    got = FetchFromBackingCache(size_class, batch, want);
    if (got == 0) {
      break;
    }
    total += got;
    i = got;
    if (result == nullptr) {
      i--;
      result = batch[i];
    }
    if (i) {
      i -= freelist_.PushBatch(size_class, batch, i);
      if (i != 0) {
        ReleaseToBackingCache(size_class, {batch, i});
      }
    }
  } while (got == kMaxObjectsToMove && i == 0 && total < target);

  for (int i = to_return.count; i < kMaxToReturn; ++i) {
    ReleaseToBackingCache(to_return.size_class[i], {&(to_return.obj[i]), 1});
  }

  return result;
}

template <class Forwarder>
inline bool CpuCache<Forwarder>::BypassCpuCache(size_t size_class) const {
  // We bypass per-cpu cache when sharded transfer cache is enabled for large
  // size classes (i.e. when we use the traditional configuration of the sharded
  // transfer cache).
  return forwarder_.sharded_transfer_cache().should_use(size_class) &&
         forwarder_.UseShardedCacheForLargeClassesOnly();
}

template <class Forwarder>
inline bool CpuCache<Forwarder>::UseBackingShardedTransferCache(
    size_t size_class) const {
  // Make sure that the thread is registered with rseq.
  ASSERT(subtle::percpu::IsFastNoInit());
  // We enable sharded cache as a backing cache for all size classes when
  // generic configuration is enabled.
  return forwarder_.sharded_transfer_cache().should_use(size_class) &&
         forwarder_.UseGenericShardedCache();
}

// Calculate number of objects to return/request from transfer cache.
inline size_t TargetOverflowRefillCount(size_t capacity, size_t batch_length,
                                        size_t successive) {
  // If the freelist is large and we are hitting a series of overflows or
  // underflows, return/request several batches at once. On the first overflow
  // we return 1 batch, on the second -- 2, on the third -- 4 and so on up to
  // half of the batches we have. We do this to save on the cost of hitting
  // malloc/free slow path, reduce instruction cache pollution, avoid cache
  // misses when accessing transfer/central caches, etc.
  const size_t max = (1 << std::min<uint32_t>(successive, 10)) * batch_length;
  // Aim at returning/refilling roughly half of objects.
  // Round up odd sizes, e.g. if the capacity is 3, we want to refill 2 objects.
  // Also always add 1 to the result to account for the additional object
  // we need to return to the caller on refill, or return on overflow.
  size_t target = std::min((capacity + 1) / 2 + 1, max);
  if (capacity == 1 && successive < 3) {
    // If the capacity is 1, it's generally impossible to avoid bad behavior.
    // Consider refills (but the same stands for overflows): if we fetch an
    // additional object and put it into the cache, and the caller is doing
    // malloc/free in a loop, then we both fetched an unnecessary object and
    // we will immediately hit an overflow on the free. On the other hand
    // if we don't fetch an additional object, and the caller is allocating
    // in a loop, then we also hit underflow again on the next malloc.
    // Currently we fetch/return an additional objects only if we are hitting
    // successive underflows/overflows.
    // But note that this behavior is also easy to compromise: if the caller is
    // allocating 3 objects and then freeing 3 objects in a loop, then we always
    // do the wrong thing.
    target = 1;
  }
  ASSERT(target <= capacity + 1);
  ASSERT(target != 0);
  return target;
}

template <class Forwarder>
inline size_t CpuCache<Forwarder>::UpdateCapacity(int cpu, size_t size_class,
                                                  bool overflow,
                                                  ObjectsToReturn* to_return) {
  // Freelist size balancing strategy:
  //  - We grow a size class only on overflow/underflow.
  //  - We shrink size classes in Steal as it scans all size classes.
  //  - If overflows/underflows happen on a size class, we want to grow its
  //    capacity to at least 2 * batch_length. It enables usage of the
  //    transfer cache and leaves the list half-full after we insert/remove
  //    a batch from the transfer cache.
  //  - We increase capacity beyond 2 * batch_length only when an overflow is
  //    followed by an underflow. That's the only case when we could benefit
  //    from larger capacity -- the overflow and the underflow would collapse.
  //
  // Note: we can't understand when we have a perfectly-sized list, because for
  // a perfectly-sized list we don't hit any slow paths which looks the same as
  // inactive list. Eventually we will shrink a perfectly-sized list a bit and
  // then it will grow back. This won't happen very frequently for the most
  // important small sizes, because we will need several ticks before we shrink
  // it again. Also we will shrink it by 1, but grow by a batch. So we should
  // have lots of time until we need to grow it again.

  // We assert that the return value, target, is non-zero, so starting from an
  // initial capacity of zero means we may be populating this core for the
  // first time.
  absl::base_internal::LowLevelCallOnce(
      &resize_[cpu].initialized,
      [](CpuCache* cache, int cpu) {
        AllocationGuardSpinLockHolder h(&cache->resize_[cpu].lock);
        cache->freelist_.InitCpu(
            cpu, cache->GetMaxCapacityFunctor(cache->freelist_.GetShift()));

        // We update this under the lock so it's guaranteed that the populated
        // CPUs don't change during ResizeSlabs.
        cache->resize_[cpu].populated.store(true, std::memory_order_relaxed);
      },
      this, cpu);
  size_t batch_length = forwarder_.num_objects_to_move(size_class);
  const size_t max_capacity = GetMaxCapacity(size_class, freelist_.GetShift());
  size_t capacity = freelist_.Capacity(cpu, size_class);
  const bool grow_by_one = capacity < 2 * batch_length;
  uint32_t successive = 0;
  ResizeInfo& resize = resize_[cpu];
  const int64_t now = absl::base_internal::CycleClock::Now();
  // TODO(ckennelly): Use a strongly typed enum.
  resize.last_miss_cycles[overflow][size_class].store(
      now, std::memory_order_relaxed);
  bool grow_by_batch =
      resize.per_class[size_class].Update(overflow, grow_by_one, &successive);
  if ((grow_by_one || grow_by_batch) && capacity != max_capacity) {
    size_t increase = 1;
    if (grow_by_batch) {
      increase = std::min(batch_length, max_capacity - capacity);
    } else if (!overflow && capacity < batch_length) {
      // On underflow we want to grow to at least batch size, because that's
      // what we want to request from transfer cache.
      increase = batch_length - capacity;
    }
    Grow(cpu, size_class, increase, to_return);
    capacity = freelist_.Capacity(cpu, size_class);
  }
  return TargetOverflowRefillCount(capacity, batch_length, successive);
}

template <class Forwarder>
inline void CpuCache<Forwarder>::Grow(int cpu, size_t size_class,
                                      size_t desired_increase,
                                      ObjectsToReturn* to_return) {
  const size_t size = forwarder_.class_to_size(size_class);
  const size_t desired_bytes = desired_increase * size;
  size_t acquired_bytes = 0;

  // First, there might be unreserved slack.  Take what we can.
  for (;;) {
    size_t before = resize_[cpu].available.load(std::memory_order_relaxed);
    // Skip atomic RMW if we have less than 6% of one object spare capacity.
    // This number is somewhat arbitrary, the idea is to avoid the RMW cost
    // if the remaining spare capacity is unlikely to help to avoid stealing.
    if (before <= (size / 16)) {
      break;
    }
    size_t can_acquire = std::min(before, desired_bytes);
    if (resize_[cpu].available.compare_exchange_strong(
            before, before - can_acquire, std::memory_order_relaxed)) {
      acquired_bytes = can_acquire;
      break;
    }
  }

  if (acquired_bytes < desired_bytes) {
    resize_[cpu].per_class[size_class].RecordMiss();
    if (!forwarder_.resize_size_classes_enabled()) {
      acquired_bytes +=
          Steal(cpu, size_class, desired_bytes - acquired_bytes, to_return);
    }
  }

  // We have all the memory we could reserve.  Time to actually do the growth.

  // We might have gotten more than we wanted (stealing from larger sizeclasses)
  // so don't grow _too_ much.
  size_t actual_increase = acquired_bytes / size;
  actual_increase = std::min(actual_increase, desired_increase);
  // Remember, Grow may not give us all we ask for.
  size_t increase = freelist_.Grow(
      cpu, size_class, actual_increase,
      [&](uint8_t shift) { return GetMaxCapacity(size_class, shift); });
  size_t increased_bytes = increase * size;
  if (increased_bytes < acquired_bytes) {
    // return whatever we didn't use to the slack.
    size_t unused = acquired_bytes - increased_bytes;
    resize_[cpu].available.fetch_add(unused, std::memory_order_relaxed);
  }
}

template <class Forwarder>
inline void CpuCache<Forwarder>::TryReclaimingCaches() {
  const int num_cpus = NumCPUs();

  for (int cpu = 0; cpu < num_cpus; ++cpu) {
    // Nothing to reclaim if the cpu is not populated.
    if (!HasPopulated(cpu)) {
      continue;
    }

    uint64_t used_bytes = UsedBytes(cpu);
    uint64_t prev_used_bytes =
        resize_[cpu].reclaim_used_bytes.load(std::memory_order_relaxed);

    // Get reclaim miss and used bytes stats that were captured at the end of
    // the previous interval.
    const CpuCacheMissStats miss_stats =
        GetAndUpdateIntervalCacheMissStats(cpu, MissCount::kReclaim);
    uint64_t misses =
        uint64_t{miss_stats.underflows} + uint64_t{miss_stats.overflows};

    // Reclaim the cache if the number of used bytes and total number of misses
    // stayed constant since the last interval.
    if (used_bytes != 0 && used_bytes == prev_used_bytes && misses == 0) {
      Reclaim(cpu);
    }

    // Takes a snapshot of used bytes in the cache at the end of this interval
    // so that we can calculate if cache usage changed in the next interval.
    //
    // Reclaim occurs on a single thread. So, the relaxed store to used_bytes
    // is safe.
    resize_[cpu].reclaim_used_bytes.store(used_bytes,
                                          std::memory_order_relaxed);
  }
}

struct SizeClassMissStat {
  size_t size_class;
  size_t misses;
};

template <class Forwarder>
inline void CpuCache<Forwarder>::ResizeSizeClasses() {
  if (!forwarder_.resize_size_classes_enabled()) return;

  const int num_cpus = NumCPUs();
  // Start resizing from where we left off the last time, and resize size class
  // capacities for up to kNumCpuCachesToResize per-cpu caches.
  int cpu = last_cpu_size_class_resize_.load(std::memory_order_relaxed);
  int num_cpus_resized = 0;

  // Record the cumulative misses for the caches so that we can select the
  // size classes with the highest misses as the candidates to resize.
  for (int cpu_offset = 0; cpu_offset < num_cpus; ++cpu_offset) {
    if (++cpu >= num_cpus) {
      cpu = 0;
    }
    ASSERT(cpu >= 0);
    ASSERT(cpu < num_cpus);

    // Nothing to resize if the cache is not populated.
    if (!HasPopulated(cpu)) {
      continue;
    }

    ResizeCpuSizeClasses(cpu);

    // Record full stats in previous full stat counters so that we can collect
    // stats per interval.
    for (size_t size_class = 1; size_class < kNumClasses; ++size_class) {
      resize_[cpu].per_class[size_class].UpdateIntervalMisses(
          PerClassMissType::kResize);
    }

    if (++num_cpus_resized >= kNumCpuCachesToResize) break;
  }
  // Record the cpu hint for which the size classes were resized so that we
  // can start from the subsequent cpu in the next interval.
  last_cpu_size_class_resize_.store(cpu, std::memory_order_relaxed);
}

template <class Forwarder>
void CpuCache<Forwarder>::ResizeCpuSizeClasses(int cpu) {
  absl::FixedArray<SizeClassMissStat> miss_stats(kNumClasses - 1);
  for (size_t size_class = 1; size_class < kNumClasses; ++size_class) {
    miss_stats[size_class - 1] = SizeClassMissStat{
        .size_class = size_class,
        .misses = resize_[cpu].per_class[size_class].GetIntervalMisses(
            PerClassMissType::kResize)};
  }

  // Resize up to kMaxSizeClassesToResize size classes. Sort the collected
  // stats to record size classes with largest number of misses in the last
  // interval.
  constexpr int kMaxSizeClassesToResize = 5;
  std::partial_sort(
      miss_stats.begin(), miss_stats.begin() + kMaxSizeClassesToResize,
      miss_stats.end(), [](SizeClassMissStat a, SizeClassMissStat b) {
        // In case of a conflict, prefer growing smaller size classes.
        if (a.misses == b.misses) {
          return a.size_class < b.size_class;
        }
        return a.misses > b.misses;
      });

  const auto max_capacity = GetMaxCapacityFunctor(freelist_.GetShift());

  for (int i = 0; i < kMaxSizeClassesToResize; ++i) {
    // If a size class with largest misses is zero, break. Other size classes
    // should also have suffered zero misses as well.
    if (miss_stats[i].misses == 0) break;
    const size_t size_class_to_grow = miss_stats[i].size_class;

    // If we are already at a maximum capacity, nothing to grow.
    const ssize_t can_grow = max_capacity(size_class_to_grow) -
                             freelist_.Capacity(cpu, size_class_to_grow);
    // can_grow can be negative only if slabs were resized,
    // but since we hold resize_[cpu].lock it must not happen.
    ASSERT(can_grow >= 0);
    if (can_grow <= 0) {
      continue;
    }

    resize_[cpu].num_size_class_resizes.fetch_add(1, std::memory_order_relaxed);

    size_t size = forwarder_.class_to_size(size_class_to_grow);
    // Get total bytes to steal from other size classes. We would like to grow
    // the capacity of the size class by a batch size.
    const size_t to_steal_bytes =
        std::min<size_t>(can_grow, Static::sizemap().num_objects_to_move(
                                       size_class_to_grow)) *
        size;

    size_t acquired_bytes = StealCapacityForSizeClassWithinCpu(
        cpu, size_class_to_grow, to_steal_bytes);
    size_t capacity_acquired = acquired_bytes / size;
    size_t actual_increase = 0;
    if (capacity_acquired != 0) {
      AllocationGuardSpinLockHolder h(&resize_[cpu].lock);
      actual_increase = freelist_.GrowOtherCache(
          cpu, size_class_to_grow, capacity_acquired, [&](uint8_t shift) {
            return GetMaxCapacity(size_class_to_grow, shift);
          });
    }

    // We might not have been able to grow the size class's capacity by the
    // amount we stole. Record the leftover in the available capacity of this
    // per-cpu cache. We do not want to lose the total capacity.
    size_t actual_increased_bytes = actual_increase * size;
    if (actual_increased_bytes < acquired_bytes) {
      // return whatever we didn't use to the slack.
      size_t unused = acquired_bytes - actual_increased_bytes;
      resize_[cpu].available.fetch_add(unused, std::memory_order_relaxed);
    }
  }
}

template <class Forwarder>
inline void CpuCache<Forwarder>::ShuffleCpuCaches() {
  // Knobs that we can potentially tune depending on the workloads.
  constexpr double kBytesToStealPercent = 5.0;
  constexpr int kMaxNumStealCpus = 5;

  const int num_cpus = NumCPUs();
  absl::FixedArray<std::pair<int, uint64_t>> misses(num_cpus);

  // Record the cumulative misses for the caches so that we can select the
  // caches with the highest misses as the candidates to steal the cache for.
  int max_populated_cpu = -1;
  int num_populated_cpus = 0;
  for (int cpu = 0; cpu < num_cpus; ++cpu) {
    if (!HasPopulated(cpu)) {
      continue;
    }
    const CpuCacheMissStats miss_stats =
        GetIntervalCacheMissStats(cpu, MissCount::kShuffle);
    misses[num_populated_cpus] = {
        cpu, uint64_t{miss_stats.underflows} + uint64_t{miss_stats.overflows}};
    max_populated_cpu = cpu;
    ++num_populated_cpus;
  }
  if (max_populated_cpu == -1) {
    return;
  }

  // Sorts misses to identify cpus with highest misses.
  //
  // TODO(vgogte): We can potentially sort the entire misses array and use that
  // in StealFromOtherCache to determine cpus to steal from. That is, [0,
  // num_dest_cpus) may be the destination cpus and [num_dest_cpus, num_cpus)
  // may be cpus we may steal from. We can iterate through the array in a
  // descending order to steal from them. The upside of this mechanism is that
  // we would be able to do a more fair stealing, starting with cpus with lowest
  // misses. The downside of this mechanism is that we would have to sort the
  // entire misses array. This might be compute intensive on servers with high
  // number of cpus (eg. Rome, Milan). We need to investigate the compute
  // required to implement this.
  const int num_dest_cpus = std::min(num_populated_cpus, kMaxNumStealCpus);
  std::partial_sort(misses.begin(), misses.begin() + num_dest_cpus,
                    misses.end(),
                    [](std::pair<int, uint64_t> a, std::pair<int, uint64_t> b) {
                      if (a.second == b.second) {
                        return a.first < b.first;
                      }
                      return a.second > b.second;
                    });

  // Try to steal kBytesToStealPercent percentage of max_per_cpu_cache_size for
  // each destination cpu cache.
  size_t to_steal = kBytesToStealPercent / 100.0 * CacheLimit();
  for (int i = 0; i < num_dest_cpus; ++i) {
    StealFromOtherCache(misses[i].first, max_populated_cpu, to_steal);
  }

  // Takes a snapshot of underflows and overflows at the end of this interval
  // so that we can calculate the misses that occurred in the next interval.
  for (int cpu = 0; cpu < num_cpus; ++cpu) {
    UpdateIntervalCacheMissStats(cpu, MissCount::kShuffle);
  }
}

template <class Forwarder>
inline void CpuCache<Forwarder>::StealFromOtherCache(int cpu,
                                                     int max_populated_cpu,
                                                     size_t bytes) {
  constexpr double kCacheMissThreshold = 0.80;

  const CpuCacheMissStats dest_misses =
      GetIntervalCacheMissStats(cpu, MissCount::kShuffle);

  // If both underflows and overflows are 0, we should not need to steal.
  if (dest_misses.underflows == 0 && dest_misses.overflows == 0) return;

  size_t acquired = 0;

  // We use last_cpu_cache_steal_ as a hint to start our search for cpu ids to
  // steal from so that we can iterate through the cpus in a nice round-robin
  // fashion.
  int src_cpu = std::min(last_cpu_cache_steal_.load(std::memory_order_relaxed),
                         max_populated_cpu);

  // We iterate through max_populate_cpus number of cpus to steal from.
  // max_populate_cpus records the max cpu id that has been populated. Note
  // that, any intermediate changes since the max_populated_cpus was measured
  // may have populated higher cpu ids, but we do not include those in the
  // search. The approximation prevents us from doing another pass through the
  // cpus to just find the latest populated cpu id.
  //
  // We break from the loop once we iterate through all the cpus once, or if the
  // total number of acquired bytes is higher than or equal to the desired bytes
  // we want to steal.
  for (int cpu_offset = 1; cpu_offset <= max_populated_cpu && acquired < bytes;
       ++cpu_offset) {
    if (--src_cpu < 0) {
      src_cpu = max_populated_cpu;
    }
    ASSERT(0 <= src_cpu);
    ASSERT(src_cpu <= max_populated_cpu);

    // We do not steal from the same CPU. Maybe we can explore combining this
    // with stealing from the same CPU later.
    if (src_cpu == cpu) continue;

    // We do not steal from the cache that hasn't been populated yet.
    if (!HasPopulated(src_cpu)) continue;

    // We do not steal from cache that has capacity less than our lower
    // capacity threshold.
    if (Capacity(src_cpu) < kCacheCapacityThreshold * CacheLimit()) continue;

    const CpuCacheMissStats src_misses =
        GetIntervalCacheMissStats(src_cpu, MissCount::kShuffle);

    // If underflows and overflows from the source cpu are higher, we do not
    // steal from that cache. We consider the cache as a candidate to steal from
    // only when its misses are lower than 0.8x that of the dest cache.
    if (src_misses.underflows > kCacheMissThreshold * dest_misses.underflows ||
        src_misses.overflows > kCacheMissThreshold * dest_misses.overflows)
      continue;

    size_t start_size_class =
        resize_[src_cpu].last_steal.load(std::memory_order_relaxed);

    ASSERT(start_size_class < kNumClasses);
    ASSERT(0 < start_size_class);
    size_t source_size_class = start_size_class;
    for (size_t offset = 1; offset < kNumClasses; ++offset) {
      source_size_class = start_size_class + offset;
      if (source_size_class >= kNumClasses) {
        source_size_class -= kNumClasses - 1;
      }
      ASSERT(0 < source_size_class);
      ASSERT(source_size_class < kNumClasses);

      const size_t capacity = freelist_.Capacity(src_cpu, source_size_class);
      if (capacity == 0) {
        // Nothing to steal.
        continue;
      }
      const size_t length = freelist_.Length(src_cpu, source_size_class);

      // TODO(vgogte): Currently, scoring is similar to stealing from the
      // same cpu in CpuCache<Forwarder>::Steal(). Revisit this later to tune
      // the knobs.
      const size_t batch_length =
          forwarder_.num_objects_to_move(source_size_class);
      size_t size = forwarder_.class_to_size(source_size_class);

      // Clock-like algorithm to prioritize size classes for shrinking.
      //
      // Each size class has quiescent ticks counter which is incremented as we
      // pass it, the counter is reset to 0 in UpdateCapacity on grow.
      // If the counter value is 0, then we've just tried to grow the size
      // class, so it makes little sense to shrink it back. The higher counter
      // value the longer ago we grew the list and the more probable it is that
      // the full capacity is unused.
      //
      // Then, we calculate "shrinking score", the higher the score the less we
      // we want to shrink this size class. The score is considerably skewed
      // towards larger size classes: smaller classes are usually used more
      // actively and we also benefit less from shrinking smaller classes (steal
      // less capacity). Then, we also avoid shrinking full freelists as we will
      // need to evict an object and then go to the central freelist to return
      // it. Then, we also avoid shrinking freelists that are just above batch
      // size, because shrinking them will disable transfer cache.
      //
      // Finally, we shrink if the ticks counter is >= the score.
      uint32_t qticks = resize_[src_cpu].per_class[source_size_class].Tick();
      uint32_t score = 0;
      // Note: the following numbers are based solely on intuition, common sense
      // and benchmarking results.
      if (size <= 144) {
        score = 2 + (length >= capacity) +
                (length >= batch_length && length < 2 * batch_length);
      } else if (size <= 1024) {
        score = 1 + (length >= capacity) +
                (length >= batch_length && length < 2 * batch_length);
      } else if (size <= (64 << 10)) {
        score = (length >= capacity);
      }
      if (score > qticks) {
        continue;
      }

      // Finally, try to shrink (can fail if we were migrated).
      // We always shrink by 1 object. The idea is that inactive lists will be
      // shrunk to zero eventually anyway (or they just would not grow in the
      // first place), but for active lists it does not make sense to
      // aggressively shuffle capacity all the time.
      //
      // If the list is full, ShrinkOtherCache first tries to pop enough items
      // to make space and then shrinks the capacity.
      // TODO(vgogte): Maybe we can steal more from a single list to avoid
      // frequent locking overhead.
      {
        AllocationGuardSpinLockHolder h(&resize_[src_cpu].lock);
        if (freelist_.ShrinkOtherCache(
                src_cpu, source_size_class, 1,
                [this](size_t size_class, void** batch, size_t count) {
                  const size_t batch_length =
                      forwarder_.num_objects_to_move(size_class);
                  for (size_t i = 0; i < count; i += batch_length) {
                    size_t n = std::min(batch_length, count - i);
                    ReleaseToBackingCache(size_class,
                                          absl::Span<void*>(batch + i, n));
                  }
                }) == 1) {
          acquired += size;
          resize_[src_cpu].capacity.fetch_sub(size, std::memory_order_relaxed);
        }
      }

      if (acquired >= bytes) {
        break;
      }
    }
    resize_[cpu].last_steal.store(source_size_class, std::memory_order_relaxed);
  }
  // Record the last cpu id we stole from, which would provide a hint to the
  // next time we iterate through the cpus for stealing.
  last_cpu_cache_steal_.store(src_cpu, std::memory_order_relaxed);

  // Increment the capacity of the destination cpu cache by the amount of bytes
  // acquired from source caches.
  if (acquired) {
    resize_[cpu].available.fetch_add(acquired, std::memory_order_relaxed);
    resize_[cpu].capacity.fetch_add(acquired, std::memory_order_relaxed);
  }
}

template <class Forwarder>
inline bool CpuCache<Forwarder>::IsGoodCandidateForShrinking(
    int cpu, size_t size_class) {
  const size_t capacity = freelist_.Capacity(cpu, size_class);
  if (capacity == 0) {
    // Nothing to steal.
    return false;
  }
  const size_t length = freelist_.Length(cpu, size_class);
  const size_t batch_length = forwarder_.num_objects_to_move(size_class);
  size_t size = forwarder_.class_to_size(size_class);

  // Clock-like algorithm to prioritize size classes for shrinking.
  //
  // Each size class has quiescent ticks counter which is incremented as we
  // pass it, the counter is reset to 0 in UpdateCapacity on grow.
  // If the counter value is 0, then we've just tried to grow the size class,
  // so it makes little sense to shrink it back. The higher counter value
  // the longer ago we grew the list and the more probable it is that
  // the full capacity is unused.
  //
  // Then, we calculate "shrinking score", the higher the score the less we
  // we want to shrink this size class. The score is considerably skewed
  // towards larger size classes: smaller classes are usually used more
  // actively and we also benefit less from shrinking smaller classes (steal
  // less capacity). Then, we also avoid shrinking full freelists as we will
  // need to evict an object and then go to the central freelist to return it.
  // Then, we also avoid shrinking freelists that are just above batch size,
  // because shrinking them will disable transfer cache.
  //
  // Finally, we shrink if the ticks counter is >= the score.
  uint32_t qticks = resize_[cpu].per_class[size_class].Tick();
  uint32_t score = 0;
  // Note: the following numbers are based solely on intuition, common sense
  // and benchmarking results.
  if (size <= 144) {
    score = 2 + (length >= capacity) +
            (length >= batch_length && length < 2 * batch_length);
  } else if (size <= 1024) {
    score = 1 + (length >= capacity) +
            (length >= batch_length && length < 2 * batch_length);
  } else if (size <= (64 << 10)) {
    score = (length >= capacity);
  }
  return (score <= qticks);
}

// TODO(vgogte): There is a lot of repetition between
// StealCapacityForSizeClassWithinCpu, Steal and other resize methods. Combine
// the logic and reduce that redundancy. Also, deprecate Steal once we make lazy
// resize a default.
template <class Forwarder>
inline size_t CpuCache<Forwarder>::StealCapacityForSizeClassWithinCpu(
    int cpu, size_t dest_size_class, size_t bytes) {
  // Steal from other sizeclasses.  Try to go in a nice circle.
  // Complicated by sizeclasses actually being 1-indexed.
  size_t acquired = 0;
  size_t start = resize_[cpu].last_steal.load(std::memory_order_relaxed);
  ASSERT(start < kNumClasses);
  ASSERT(0 < start);
  size_t source_size_class = start;
  for (size_t offset = 1; offset < kNumClasses; ++offset) {
    source_size_class = start + offset;
    if (source_size_class >= kNumClasses) {
      source_size_class -= kNumClasses - 1;
    }
    ASSERT(0 < source_size_class);
    ASSERT(source_size_class < kNumClasses);
    // Decide if we want to steal source_size_class.
    if (source_size_class == dest_size_class) {
      // First, no sense in picking your own pocket.
      continue;
    }

    if (!IsGoodCandidateForShrinking(cpu, source_size_class)) continue;
    size_t size = forwarder_.class_to_size(source_size_class);
    // Finally, try to shrink.
    // We always shrink by 1 object. The idea is that inactive lists will be
    // shrunk to zero eventually anyway (or they just would not grow in the
    // first place), but for active lists it does not make sense to aggressively
    // shuffle capacity all the time.
    {
      AllocationGuardSpinLockHolder h(&resize_[cpu].lock);
      if (freelist_.ShrinkOtherCache(
              cpu, source_size_class, 1,
              [this](size_t size_class, void** batch, size_t count) {
                ASSERT(count > 0);
                ReleaseToBackingCache(size_class,
                                      absl::Span<void*>(batch, count));
              }) == 1) {
        acquired += size;
      }
    }

    if (acquired >= bytes) {
      // can't steal any more or don't need to
      break;
    }
  }
  // update the hint
  resize_[cpu].last_steal.store(source_size_class, std::memory_order_relaxed);
  return acquired;
}
// There are rather a lot of policy knobs we could tweak here.
template <class Forwarder>
inline size_t CpuCache<Forwarder>::Steal(int cpu, size_t dest_size_class,
                                         size_t bytes,
                                         ObjectsToReturn* to_return) {
  // Steal from other sizeclasses.  Try to go in a nice circle.
  // Complicated by sizeclasses actually being 1-indexed.
  size_t acquired = 0;
  size_t start = resize_[cpu].last_steal.load(std::memory_order_relaxed);
  ASSERT(start < kNumClasses);
  ASSERT(0 < start);
  size_t source_size_class = start;
  for (size_t offset = 1; offset < kNumClasses; ++offset) {
    source_size_class = start + offset;
    if (source_size_class >= kNumClasses) {
      source_size_class -= kNumClasses - 1;
    }
    ASSERT(0 < source_size_class);
    ASSERT(source_size_class < kNumClasses);
    // Decide if we want to steal source_size_class.
    if (source_size_class == dest_size_class) {
      // First, no sense in picking your own pocket.
      continue;
    }
    if (!IsGoodCandidateForShrinking(cpu, source_size_class)) continue;
    size_t size = forwarder_.class_to_size(source_size_class);
    const size_t capacity = freelist_.Capacity(cpu, source_size_class);
    const size_t length = freelist_.Length(cpu, source_size_class);

    if (length >= capacity) {
      // The list is full, need to evict an object to shrink it.
      if (to_return == nullptr) {
        continue;
      }
      if (to_return->count == 0) {
        // Can't steal any more because the to_return set is full.
        break;
      }
      void* obj = freelist_.Pop(source_size_class, NoopUnderflow, nullptr);
      if (obj) {
        --to_return->count;
        to_return->size_class[to_return->count] = source_size_class;
        to_return->obj[to_return->count] = obj;
      }
    }

    // Finally, try to shrink (can fail if we were migrated).
    // We always shrink by 1 object. The idea is that inactive lists will be
    // shrunk to zero eventually anyway (or they just would not grow in the
    // first place), but for active lists it does not make sense to aggressively
    // shuffle capacity all the time.
    if (freelist_.Shrink(cpu, source_size_class, 1) == 1) {
      acquired += size;
    }

    if (cpu != freelist_.GetCurrentVirtualCpuUnsafe() || acquired >= bytes) {
      // can't steal any more or don't need to
      break;
    }
  }
  // update the hint
  resize_[cpu].last_steal.store(source_size_class, std::memory_order_relaxed);
  return acquired;
}

template <class Forwarder>
inline int CpuCache<Forwarder>::Overflow(void* ptr, size_t size_class,
                                         int cpu) {
  const size_t target = UpdateCapacity(cpu, size_class, true, nullptr);
  size_t total = 0;
  size_t count = 1;
  void* batch[kMaxObjectsToMove];
  batch[0] = ptr;
  do {
    size_t want = std::min(kMaxObjectsToMove, target - total);
    if (count < want) {
      count += freelist_.PopBatch(size_class, batch + count, want - count);
    }
    if (!count) break;

    total += count;
    ReleaseToBackingCache(size_class, absl::Span<void*>(batch, count));
    if (count != kMaxObjectsToMove) break;
    count = 0;
  } while (total < target);
  return 1;
}

template <class Forwarder>
inline uint64_t CpuCache<Forwarder>::Allocated(int target_cpu) const {
  ASSERT(target_cpu >= 0);
  if (!HasPopulated(target_cpu)) {
    return 0;
  }

  uint64_t total = 0;
  for (int size_class = 1; size_class < kNumClasses; size_class++) {
    int size = forwarder_.class_to_size(size_class);
    total += size * freelist_.Capacity(target_cpu, size_class);
  }
  return total;
}

template <class Forwarder>
inline uint64_t CpuCache<Forwarder>::UsedBytes(int target_cpu) const {
  ASSERT(target_cpu >= 0);
  if (!HasPopulated(target_cpu)) {
    return 0;
  }

  uint64_t total = 0;
  for (int size_class = 1; size_class < kNumClasses; size_class++) {
    int size = forwarder_.class_to_size(size_class);
    total += size * freelist_.Length(target_cpu, size_class);
  }
  return total;
}

template <class Forwarder>
inline bool CpuCache<Forwarder>::HasPopulated(int target_cpu) const {
  ASSERT(target_cpu >= 0);
  return resize_[target_cpu].populated.load(std::memory_order_relaxed);
}

template <class Forwarder>
inline PerCPUMetadataState CpuCache<Forwarder>::MetadataMemoryUsage() const {
  return freelist_.MetadataMemoryUsage();
}

template <class Forwarder>
inline uint64_t CpuCache<Forwarder>::TotalUsedBytes() const {
  uint64_t total = 0;
  for (int cpu = 0, num_cpus = NumCPUs(); cpu < num_cpus; ++cpu) {
    total += UsedBytes(cpu);
  }
  return total;
}

template <class Forwarder>
inline uint64_t CpuCache<Forwarder>::TotalObjectsOfClass(
    size_t size_class) const {
  ASSERT(size_class < kNumClasses);
  uint64_t total_objects = 0;
  if (size_class > 0) {
    for (int cpu = 0, n = NumCPUs(); cpu < n; cpu++) {
      if (!HasPopulated(cpu)) {
        continue;
      }
      total_objects += freelist_.Length(cpu, size_class);
    }
  }
  return total_objects;
}

template <class Forwarder>
inline uint64_t CpuCache<Forwarder>::Unallocated(int cpu) const {
  return resize_[cpu].available.load(std::memory_order_relaxed);
}

template <class Forwarder>
inline uint64_t CpuCache<Forwarder>::Capacity(int cpu) const {
  return resize_[cpu].capacity.load(std::memory_order_relaxed);
}

template <class Forwarder>
inline uint64_t CpuCache<Forwarder>::CacheLimit() const {
  return max_per_cpu_cache_size_.load(std::memory_order_relaxed);
}

template <class Forwarder>
inline void CpuCache<Forwarder>::SetCacheLimit(uint64_t v) {
  // TODO(b/179516472): Drain cores as required.
  max_per_cpu_cache_size_.store(v, std::memory_order_relaxed);
}

template <class CpuCache>
struct DrainHandler {
  void operator()(int cpu, size_t size_class, void** batch, size_t count,
                  size_t cap) const {
    const size_t size = cache->forwarder_.class_to_size(size_class);
    const size_t batch_length =
        cache->forwarder_.num_objects_to_move(size_class);
    if (bytes != nullptr) *bytes += count * size;
    // Drain resets capacity to 0, so return the allocated capacity to that
    // CPU's slack.
    cache->resize_[cpu].available.fetch_add(cap * size,
                                            std::memory_order_relaxed);
    for (size_t i = 0; i < count; i += batch_length) {
      size_t n = std::min(batch_length, count - i);
      cache->ReleaseToBackingCache(size_class, absl::Span<void*>(batch + i, n));
    }
  }

  // `cache` must be non-null.
  CpuCache* cache;
  uint64_t* bytes;
};

template <class Forwarder>
inline uint64_t CpuCache<Forwarder>::Reclaim(int cpu) {
  AllocationGuardSpinLockHolder h(&resize_[cpu].lock);

  // If we haven't populated this core, freelist_.Drain() will touch the memory
  // (for writing) as part of its locking process.  Avoid faulting new pages as
  // part of a release process.
  if (!HasPopulated(cpu)) {
    return 0;
  }

  uint64_t bytes = 0;
  freelist_.Drain(cpu, DrainHandler<CpuCache>{this, &bytes});

  // Record that the reclaim occurred for this CPU.
  resize_[cpu].num_reclaims.store(
      resize_[cpu].num_reclaims.load(std::memory_order_relaxed) + 1,
      std::memory_order_relaxed);
  resize_[cpu].last_reclaim.store(absl::base_internal::CycleClock::Now(),
                                  std::memory_order_relaxed);

  return bytes;
}
template <class Forwarder>
inline uint64_t CpuCache<Forwarder>::GetNumResizes(int cpu) const {
  return resize_[cpu].num_size_class_resizes.load(std::memory_order_relaxed);
}

template <class Forwarder>
inline uint64_t CpuCache<Forwarder>::GetNumResizes() const {
  uint64_t resizes = 0;
  const int num_cpus = NumCPUs();
  for (int cpu = 0; cpu < num_cpus; ++cpu)
    resizes +=
        resize_[cpu].num_size_class_resizes.load(std::memory_order_relaxed);
  return resizes;
}

template <class Forwarder>
inline uint64_t CpuCache<Forwarder>::GetNumReclaims(int cpu) const {
  return resize_[cpu].num_reclaims.load(std::memory_order_relaxed);
}

template <class Forwarder>
inline uint64_t CpuCache<Forwarder>::GetNumReclaims() const {
  uint64_t reclaims = 0;
  const int num_cpus = NumCPUs();
  for (int cpu = 0; cpu < num_cpus; ++cpu)
    reclaims += resize_[cpu].num_reclaims.load(std::memory_order_relaxed);
  return reclaims;
}

template <class Forwarder>
inline auto CpuCache<Forwarder>::AllocOrReuseSlabs(
    absl::FunctionRef<void*(size_t, std::align_val_t)> alloc,
    subtle::percpu::Shift shift, int num_cpus, uint8_t shift_offset)
    -> std::pair<Freelist::Slabs*, size_t> {
  Freelist::Slabs*& reused_slabs = slabs_by_shift_[shift_offset];
  const size_t size = GetSlabsAllocSize(shift, num_cpus);
  const bool can_reuse = reused_slabs != nullptr;
  if (can_reuse) {
    // Enable huge pages for reused slabs.
    // TODO(b/214241843): we should be able to remove this once the kernel
    // enables huge zero pages.
    ErrnoRestorer errno_restorer;
    madvise(reused_slabs, size, MADV_HUGEPAGE);
  } else {
    reused_slabs = static_cast<Freelist::Slabs*>(
        alloc(size, subtle::percpu::kPhysicalPageAlign));
    // MSan does not see writes in assembly.
    ANNOTATE_MEMORY_IS_INITIALIZED(reused_slabs, size);
  }
  return {reused_slabs, can_reuse ? size : 0};
}

template <class Forwarder>
inline typename CpuCache<Forwarder>::DynamicSlabResize
CpuCache<Forwarder>::ShouldResizeSlab() {
  const int num_cpus = NumCPUs();
  CpuCacheMissStats total_misses{};
  DynamicSlabResize resize = DynamicSlabResize::kNoop;
  const bool wider_slabs_enabled = UseWiderSlabs();
  for (int cpu = 0; cpu < num_cpus; ++cpu) {
    CpuCacheMissStats misses =
        GetAndUpdateIntervalCacheMissStats(cpu, MissCount::kSlabResize);
    total_misses += misses;

    if (misses.overflows >
        misses.underflows *
            forwarder_.per_cpu_caches_dynamic_slab_grow_threshold()) {
      resize = DynamicSlabResize::kGrow;
    }
  }

  // When wider slabs featuee is enabled, we try to grow slabs when the
  // condition for at least one cpu cache is met. Else, we use total misses to
  // figure out whether to grow the slab, shrink it, or do nothing.
  if (wider_slabs_enabled && resize == DynamicSlabResize::kGrow) {
    return resize;
  }

  // As a simple heuristic, we decide to grow if the total number of overflows
  // is large compared to total number of underflows during the growth period.
  // If the slab size was infinite, we would expect 0 overflows. If the slab
  // size was 0, we would expect approximately equal numbers of underflows and
  // overflows.
  if (total_misses.overflows >
      total_misses.underflows *
          forwarder_.per_cpu_caches_dynamic_slab_grow_threshold()) {
    return DynamicSlabResize::kGrow;
  } else if (total_misses.overflows <
             total_misses.underflows *
                 forwarder_.per_cpu_caches_dynamic_slab_shrink_threshold()) {
    return DynamicSlabResize::kShrink;
  }

  return DynamicSlabResize::kNoop;
}

template <class Forwarder>
void CpuCache<Forwarder>::ResizeSlabIfNeeded() ABSL_NO_THREAD_SAFETY_ANALYSIS {
  uint8_t per_cpu_shift = freelist_.GetShift();

  const int num_cpus = NumCPUs();
  const DynamicSlabResize resize = ShouldResizeSlab();

  if (resize == DynamicSlabResize::kGrow) {
    if (per_cpu_shift == shift_bounds_.max_shift) return;
    ++per_cpu_shift;
    dynamic_slab_info_
        .grow_count[ShiftOffset(per_cpu_shift, shift_bounds_.initial_shift)]
        .fetch_add(1, std::memory_order_relaxed);
  } else if (resize == DynamicSlabResize::kShrink) {
    if (per_cpu_shift == shift_bounds_.initial_shift) return;
    --per_cpu_shift;
    dynamic_slab_info_
        .shrink_count[ShiftOffset(per_cpu_shift, shift_bounds_.initial_shift)]
        .fetch_add(1, std::memory_order_relaxed);
  } else {
    return;
  }

  const auto new_shift = subtle::percpu::ToShiftType(per_cpu_shift);
  const int64_t new_slabs_size =
      subtle::percpu::GetSlabsAllocSize(new_shift, num_cpus);
  // Account for impending allocation/reusing of new slab so that we can avoid
  // going over memory limit.
  forwarder_.ArenaUpdateAllocatedAndNonresident(new_slabs_size, 0);
  forwarder_.ShrinkToUsageLimit();

  for (int cpu = 0; cpu < num_cpus; ++cpu) resize_[cpu].lock.Lock();
  ResizeSlabsInfo info;
  int64_t reused_bytes;
  {
    // We can't allocate while holding the per-cpu spinlocks.
    AllocationGuard enforce_no_alloc;

    Freelist::Slabs* new_slabs;
    std::tie(new_slabs, reused_bytes) = AllocOrReuseSlabs(
        [&](size_t size, std::align_val_t align) {
          return forwarder_.AllocReportedImpending(size, align);
        },
        new_shift, num_cpus,
        ShiftOffset(per_cpu_shift, shift_bounds_.initial_shift));
    info = freelist_.ResizeSlabs(
        new_shift, new_slabs, &forwarder_.Alloc,
        GetShiftMaxCapacity{max_capacity_, per_cpu_shift, shift_bounds_},
        [this](int cpu) { return HasPopulated(cpu); },
        DrainHandler<CpuCache>{this, nullptr});
  }
  for (int cpu = 0; cpu < num_cpus; ++cpu) resize_[cpu].lock.Unlock();

  // madvise away the old slabs memory.  It is important that we do not
  // MADV_REMOVE the memory, since file-backed pages may SIGSEGV/SIGBUS if
  // another thread sees the previous slab after this point and reads it.
  //
  // TODO(b/214241843): we should be able to remove MADV_NOHUGEPAGE once the
  // kernel enables huge zero pages.
  // Note: we use bitwise OR to avoid short-circuiting.
  ErrnoRestorer errno_restorer;
  const bool madvise_failed =
      madvise(info.old_slabs, info.old_slabs_size, MADV_NOHUGEPAGE) |
      madvise(info.old_slabs, info.old_slabs_size, MADV_DONTNEED);
  if (madvise_failed) {
    dynamic_slab_info_.madvise_failed_bytes.fetch_add(
        info.old_slabs_size, std::memory_order_relaxed);
  }
  const int64_t old_slabs_size = info.old_slabs_size;
  forwarder_.ArenaUpdateAllocatedAndNonresident(-old_slabs_size,
                                                old_slabs_size - reused_bytes);
}

template <class Forwarder>
inline void CpuCache<Forwarder>::RecordCacheMissStat(const int cpu,
                                                     const bool is_alloc) {
  MissCounts& misses =
      is_alloc ? resize_[cpu].underflows : resize_[cpu].overflows;
  auto& c = misses[MissCount::kTotal];
  c.store(c.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
}

template <class Forwarder>
inline typename CpuCache<Forwarder>::CpuCacheMissStats
CpuCache<Forwarder>::GetTotalCacheMissStats(int cpu) const {
  CpuCacheMissStats stats;
  stats.underflows = resize_[cpu].underflows[MissCount::kTotal].load(
      std::memory_order_relaxed);
  stats.overflows =
      resize_[cpu].overflows[MissCount::kTotal].load(std::memory_order_relaxed);
  return stats;
}

template <class Forwarder>
inline typename CpuCache<Forwarder>::CpuCacheMissStats
CpuCache<Forwarder>::GetTotalCacheMissStats() const {
  CpuCacheMissStats stats;
  const int num_cpus = NumCPUs();
  for (int cpu = 0; cpu < num_cpus; ++cpu) stats += GetTotalCacheMissStats(cpu);
  return stats;
}

template <class Forwarder>
inline typename CpuCache<Forwarder>::CpuCacheMissStats
CpuCache<Forwarder>::GetIntervalCacheMissStats(int cpu,
                                               MissCount miss_count) const {
  ASSERT(miss_count != MissCount::kTotal);
  ASSERT(miss_count < MissCount::kNumCounts);
  const auto get_safe_miss_diff = [miss_count](MissCounts& misses) {
    const size_t total_misses =
        misses[MissCount::kTotal].load(std::memory_order_relaxed);
    const size_t interval_misses =
        misses[miss_count].load(std::memory_order_relaxed);
    // In case of a size_t overflow, we wrap around to 0.
    return total_misses > interval_misses ? total_misses - interval_misses : 0;
  };
  return {get_safe_miss_diff(resize_[cpu].underflows),
          get_safe_miss_diff(resize_[cpu].overflows)};
}

template <class Forwarder>
void CpuCache<Forwarder>::UpdateIntervalCacheMissStats(int cpu,
                                                       MissCount miss_count) {
  CpuCacheMissStats total_stats = GetTotalCacheMissStats(cpu);
  // Takes a snapshot of misses at the end of this interval so that we can
  // calculate the misses that occurred in the next interval.
  //
  // Interval updates occur on a single thread so relaxed stores to interval
  // miss stats are safe.
  resize_[cpu].underflows[miss_count].store(total_stats.underflows,
                                            std::memory_order_relaxed);
  resize_[cpu].overflows[miss_count].store(total_stats.overflows,
                                           std::memory_order_relaxed);
}

template <class Forwarder>
inline typename CpuCache<Forwarder>::CpuCacheMissStats
CpuCache<Forwarder>::GetAndUpdateIntervalCacheMissStats(int cpu,
                                                        MissCount miss_count) {
  // Note: it's possible for cache misses to occur between these two calls, but
  // there's likely to be few of them so we don't handle them specially.
  CpuCacheMissStats interval_stats = GetIntervalCacheMissStats(cpu, miss_count);
  UpdateIntervalCacheMissStats(cpu, miss_count);
  return interval_stats;
}

template <class Forwarder>
size_t CpuCache<Forwarder>::GetIntervalSizeClassMisses(int cpu,
                                                       size_t size_class,
                                                       PerClassMissType type) {
  return resize_[cpu].per_class[size_class].GetIntervalMisses(type);
}

template <class Forwarder>
inline typename CpuCache<Forwarder>::SizeClassCapacityStats
CpuCache<Forwarder>::GetSizeClassCapacityStats(size_t size_class) const {
  SizeClassCapacityStats stats;
  int num_populated = 0;
  // We use a local variable here, instead of directly updating min_capacity in
  // SizeClassCapacityStats struct to make sure we do not end up with SIZE_MAX
  // in stats.min_capacity when num_populated is equal to zero.
  size_t min_capacity = SIZE_MAX;
  const double now = absl::base_internal::CycleClock::Now();
  const double frequency = absl::base_internal::CycleClock::Frequency();

  // Scan through all per-CPU caches and calculate minimum, average and maximum
  // capacities for the size class <size_class> across all the populated caches.
  for (int cpu = 0, num_cpus = NumCPUs(); cpu < num_cpus; ++cpu) {
    // We do not include stats for non-populated cpus in our average.
    if (!HasPopulated(cpu)) {
      continue;
    }

    ++num_populated;

    const auto last_reclaim =
        resize_[cpu].last_reclaim.load(std::memory_order_relaxed);

    const auto last_underflow_cycles =
        resize_[cpu].last_miss_cycles[0][size_class].load(
            std::memory_order_relaxed);
    const auto last_overflow_cycles =
        resize_[cpu].last_miss_cycles[1][size_class].load(
            std::memory_order_relaxed);

    size_t cap = freelist_.Capacity(cpu, size_class);
    stats.max_capacity = std::max(stats.max_capacity, cap);
    min_capacity = std::min(min_capacity, cap);
    stats.avg_capacity += cap;

    if (last_reclaim >= last_underflow_cycles ||
        last_reclaim >= last_overflow_cycles) {
      // Don't consider the underflow/overflow time on this CPU if we have
      // recently reclaimed.
      continue;
    }

    if (cap == 0) {
      // Or if the capacity is empty.  We may simply not be allocating this size
      // class.
      continue;
    }

    const absl::Duration last_underflow =
        absl::Seconds((now - last_underflow_cycles) / frequency);
    const absl::Duration last_overflow =
        absl::Seconds((now - last_overflow_cycles) / frequency);

    if (last_overflow < stats.min_last_overflow) {
      stats.min_last_overflow = last_overflow;
      stats.min_last_overflow_cpu_id = cpu;
    }
    if (last_overflow > stats.max_last_overflow) {
      stats.max_last_overflow = last_overflow;
      stats.max_last_overflow_cpu_id = cpu;
    }
    if (last_underflow < stats.min_last_underflow) {
      stats.min_last_underflow = last_underflow;
      stats.min_last_underflow_cpu_id = cpu;
    }
    if (last_underflow > stats.max_last_underflow) {
      stats.max_last_underflow = last_underflow;
      stats.max_last_underflow_cpu_id = cpu;
    }
  }
  if (num_populated > 0) {
    stats.avg_capacity /= num_populated;
    stats.min_capacity = min_capacity;
  }
  return stats;
}

template <class Forwarder>
inline void CpuCache<Forwarder>::Print(Printer* out) const {
  out->printf("------------------------------------------------\n");
  out->printf("Bytes in per-CPU caches (per cpu limit: %u bytes)\n",
              CacheLimit());
  out->printf("------------------------------------------------\n");

  const cpu_set_t allowed_cpus = FillActiveCpuMask();
  const int num_cpus = NumCPUs();

  for (int cpu = 0; cpu < num_cpus; ++cpu) {
    static constexpr double MiB = 1048576.0;

    uint64_t rbytes = UsedBytes(cpu);
    bool populated = HasPopulated(cpu);
    uint64_t unallocated = Unallocated(cpu);
    out->printf(
        "cpu %3d: %12u"
        " bytes (%7.1f MiB) with"
        "%12u bytes unallocated %s%s\n",
        cpu, rbytes, rbytes / MiB, unallocated,
        CPU_ISSET(cpu, &allowed_cpus) ? " active" : "",
        populated ? " populated" : "");
  }

  out->printf("------------------------------------------------\n");
  out->printf("Size class capacity statistics in per-cpu caches\n");
  out->printf("------------------------------------------------\n");

  for (int size_class = 0; size_class < kNumClasses; ++size_class) {
    SizeClassCapacityStats stats = GetSizeClassCapacityStats(size_class);
    out->printf(
        "class %3d [ %8zu bytes ] : "
        "%6zu (minimum), %7.1f (average), %6zu (maximum), %6zu maximum allowed "
        "capacity (underflow: [%d us CPU %d, %d us CPU %d]; "
        "overflow [%d us CPU %d, %d us CPU %d]\n",
        size_class, forwarder_.class_to_size(size_class), stats.min_capacity,
        stats.avg_capacity, stats.max_capacity,
        GetMaxCapacity(size_class, freelist_.GetShift()),
        absl::ToInt64Microseconds(stats.min_last_underflow),
        stats.min_last_underflow_cpu_id,
        absl::ToInt64Microseconds(stats.max_last_underflow),
        stats.max_last_underflow_cpu_id,
        absl::ToInt64Microseconds(stats.min_last_overflow),
        stats.min_last_overflow_cpu_id,
        absl::ToInt64Microseconds(stats.max_last_overflow),
        stats.max_last_overflow_cpu_id);
  }

  out->printf("------------------------------------------------\n");
  out->printf("Number of per-CPU cache underflows, overflows, and reclaims\n");
  out->printf("------------------------------------------------\n");
  const auto print_miss_stats = [out](CpuCacheMissStats miss_stats,
                                      uint64_t reclaims, uint64_t resizes) {
    out->printf(
        "%12u underflows,"
        "%12u overflows, overflows / underflows: %5.2f, "
        "%12u reclaims,"
        "%12u resizes\n",
        miss_stats.underflows, miss_stats.overflows,
        safe_div(miss_stats.overflows, miss_stats.underflows), reclaims,
        resizes);
  };
  out->printf("Total  :");
  print_miss_stats(GetTotalCacheMissStats(), GetNumReclaims(), GetNumResizes());
  for (int cpu = 0; cpu < num_cpus; ++cpu) {
    out->printf("cpu %3d:", cpu);
    print_miss_stats(GetTotalCacheMissStats(cpu), GetNumReclaims(cpu),
                     GetNumResizes(cpu));
  }

  out->printf("------------------------------------------------\n");
  out->printf("Per-CPU cache slab resizing info:\n");
  out->printf("------------------------------------------------\n");
  uint8_t current_shift = freelist_.GetShift();
  out->printf("Current shift: %3d (slab size: %4d KiB)\n", current_shift,
              (1 << current_shift) / 1024);
  for (int shift = 0; shift < kNumPossiblePerCpuShifts; ++shift) {
    out->printf("shift %3d:", shift + shift_bounds_.initial_shift);
    out->printf(
        "%12u growths, %12u shrinkages\n",
        dynamic_slab_info_.grow_count[shift].load(std::memory_order_relaxed),
        dynamic_slab_info_.shrink_count[shift].load(std::memory_order_relaxed));
  }
  out->printf(
      "%12u bytes for which MADVISE_DONTNEED failed\n",
      dynamic_slab_info_.madvise_failed_bytes.load(std::memory_order_relaxed));
}

template <class Forwarder>
inline void CpuCache<Forwarder>::PrintInPbtxt(PbtxtRegion* region) const {
  const cpu_set_t allowed_cpus = FillActiveCpuMask();

  for (int cpu = 0, num_cpus = NumCPUs(); cpu < num_cpus; ++cpu) {
    PbtxtRegion entry = region->CreateSubRegion("cpu_cache");
    uint64_t rbytes = UsedBytes(cpu);
    bool populated = HasPopulated(cpu);
    uint64_t unallocated = Unallocated(cpu);
    CpuCacheMissStats miss_stats = GetTotalCacheMissStats(cpu);
    uint64_t reclaims = GetNumReclaims(cpu);
    uint64_t resizes = GetNumResizes(cpu);
    entry.PrintI64("cpu", cpu);
    entry.PrintI64("used", rbytes);
    entry.PrintI64("unused", unallocated);
    entry.PrintBool("active", CPU_ISSET(cpu, &allowed_cpus));
    entry.PrintBool("populated", populated);
    entry.PrintI64("underflows", miss_stats.underflows);
    entry.PrintI64("overflows", miss_stats.overflows);
    entry.PrintI64("reclaims", reclaims);
    entry.PrintI64("size_class_resizes", resizes);
  }

  // Record size class capacity statistics.
  for (int size_class = 0; size_class < kNumClasses; ++size_class) {
    SizeClassCapacityStats stats = GetSizeClassCapacityStats(size_class);
    PbtxtRegion entry = region->CreateSubRegion("size_class_capacity");
    entry.PrintI64("sizeclass", forwarder_.class_to_size(size_class));
    entry.PrintI64("min_capacity", stats.min_capacity);
    entry.PrintDouble("avg_capacity", stats.avg_capacity);
    entry.PrintI64("max_capacity", stats.max_capacity);
    entry.PrintI64("max_allowed_capacity",
                   GetMaxCapacity(size_class, freelist_.GetShift()));

    entry.PrintI64("min_last_underflow_ns",
                   absl::ToInt64Nanoseconds(stats.min_last_underflow));
    entry.PrintI64("max_last_underflow_ns",
                   absl::ToInt64Nanoseconds(stats.max_last_underflow));
    entry.PrintI64("min_last_overflow_ns",
                   absl::ToInt64Nanoseconds(stats.min_last_overflow));
    entry.PrintI64("max_last_overflow_ns",
                   absl::ToInt64Nanoseconds(stats.max_last_overflow));
  }

  // Record dynamic slab statistics.
  region->PrintI64("dynamic_per_cpu_slab_size", 1 << freelist_.GetShift());
  for (int shift = 0; shift < kNumPossiblePerCpuShifts; ++shift) {
    PbtxtRegion entry = region->CreateSubRegion("dynamic_slab");
    entry.PrintI64("shift", shift + shift_bounds_.initial_shift);
    entry.PrintI64("grow_count", dynamic_slab_info_.grow_count[shift].load(
                                     std::memory_order_relaxed));
    entry.PrintI64("shrink_count", dynamic_slab_info_.shrink_count[shift].load(
                                       std::memory_order_relaxed));
  }
  region->PrintI64(
      "dynamic_slab_madvise_failed_bytes",
      dynamic_slab_info_.madvise_failed_bytes.load(std::memory_order_relaxed));
}

template <class Forwarder>
inline void CpuCache<Forwarder>::PerClassResizeInfo::Init() {
  state_.store(0, std::memory_order_relaxed);
}

template <class Forwarder>
inline bool CpuCache<Forwarder>::PerClassResizeInfo::Update(
    bool overflow, bool grow, uint32_t* successive) {
  int32_t raw = state_.load(std::memory_order_relaxed);
  State state;
  memcpy(&state, &raw, sizeof(state));
  const bool overflow_then_underflow = !overflow && state.overflow;
  grow |= overflow_then_underflow;
  // Reset quiescent ticks for Steal clock algorithm if we are going to grow.
  State new_state;
  new_state.overflow = overflow;
  new_state.quiescent_ticks = grow ? 0 : state.quiescent_ticks;
  new_state.successive = overflow == state.overflow ? state.successive + 1 : 0;
  memcpy(&raw, &new_state, sizeof(raw));
  state_.store(raw, std::memory_order_relaxed);
  *successive = new_state.successive;
  return overflow_then_underflow;
}

template <class Forwarder>
inline uint32_t CpuCache<Forwarder>::PerClassResizeInfo::Tick() {
  int32_t raw = state_.load(std::memory_order_relaxed);
  State state;
  memcpy(&state, &raw, sizeof(state));
  state.quiescent_ticks++;
  memcpy(&raw, &state, sizeof(raw));
  state_.store(raw, std::memory_order_relaxed);
  return state.quiescent_ticks - 1;
}

template <class Forwarder>
inline void CpuCache<Forwarder>::PerClassResizeInfo::RecordMiss() {
  auto& c = misses_[PerClassMissType::kTotal];
  c.store(c.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
}

template <class Forwarder>
inline size_t CpuCache<Forwarder>::PerClassResizeInfo::GetTotalMisses() {
  return misses_[PerClassMissType::kTotal].load(std::memory_order_relaxed);
}

template <class Forwarder>
inline size_t CpuCache<Forwarder>::PerClassResizeInfo::GetIntervalMisses(
    PerClassMissType type) {
  ASSERT(type != PerClassMissType::kTotal);
  ASSERT(type < PerClassMissType::kNumTypes);

  const size_t total_misses =
      misses_[PerClassMissType::kTotal].load(std::memory_order_relaxed);
  const size_t interval_misses = misses_[type].load(std::memory_order_relaxed);
  // In case of a size_t overflow, we wrap around to 0.
  return total_misses > interval_misses ? total_misses - interval_misses : 0;
}

template <class Forwarder>
void CpuCache<Forwarder>::PerClassResizeInfo::UpdateIntervalMisses(
    PerClassMissType type) {
  const size_t total_misses = GetTotalMisses();
  // Takes a snapshot of misses at the end of this interval so that we can
  // calculate the misses that occurred in the next interval.
  //
  // Interval updates occur on a single thread so relaxed stores to interval
  // miss stats are safe.
  misses_[type].store(total_misses, std::memory_order_relaxed);
}

}  // namespace cpu_cache_internal

// Static forward declares CpuCache to avoid a cycle in headers.  Make
// "CpuCache" be non-templated to avoid breaking that forward declaration.
class CpuCache final
    : public cpu_cache_internal::CpuCache<cpu_cache_internal::StaticForwarder> {
};

template <typename State>
inline bool UsePerCpuCache(State& state) {
  // We expect a fast path of per-CPU caches being active and the thread being
  // registered with rseq.
  if (ABSL_PREDICT_FALSE(!state.CpuCacheActive())) {
    return false;
  }

  if (ABSL_PREDICT_TRUE(subtle::percpu::IsFastNoInit())) {
    return true;
  }

  // When rseq is not registered, use this transition edge to shutdown the
  // thread cache for this thread.
  //
  // We call IsFast() on every non-fastpath'd malloc or free since IsFast() has
  // the side-effect of initializing the per-thread state needed for "unsafe"
  // per-cpu operations in case this is the first time a new thread is calling
  // into tcmalloc.
  //
  // If the per-CPU cache for a thread is not initialized, we push ourselves
  // onto the slow path until this occurs.  See fast_alloc's use of
  // TryRecordAllocationFast.
  if (ABSL_PREDICT_TRUE(subtle::percpu::IsFast())) {
    ThreadCache::BecomeIdle();
    return true;
  }

  return false;
}

}  // namespace tcmalloc_internal
}  // namespace tcmalloc
GOOGLE_MALLOC_SECTION_END
#endif  // TCMALLOC_CPU_CACHE_H_
