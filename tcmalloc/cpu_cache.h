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

#include <algorithm>
#include <atomic>

#include "absl/algorithm/container.h"
#include "absl/base/attributes.h"
#include "absl/base/call_once.h"
#include "absl/base/internal/spinlock.h"
#include "absl/base/internal/sysinfo.h"
#include "absl/base/optimization.h"
#include "absl/container/fixed_array.h"
#include "tcmalloc/common.h"
#include "tcmalloc/internal/logging.h"
#include "tcmalloc/internal/optimization.h"
#include "tcmalloc/internal/percpu.h"
#include "tcmalloc/internal/percpu_tcmalloc.h"
#include "tcmalloc/parameters.h"
#include "tcmalloc/static_vars.h"
#include "tcmalloc/thread_cache.h"
#include "tcmalloc/tracking.h"

GOOGLE_MALLOC_SECTION_BEGIN
namespace tcmalloc {
namespace tcmalloc_internal {
namespace cpu_cache_internal {

// StaticForwarder provides access to the SizeMap and transfer caches.
//
// This is a class, rather than namespaced globals, so that it can be mocked for
// testing.
class StaticForwarder {
 public:
  static size_t class_to_size(int size_class) {
    return Static::sizemap().class_to_size(size_class);
  }

  static bool lazy_per_cpu_caches() {
    return Parameters::lazy_per_cpu_caches();
  }

  static size_t max_per_cpu_cache_size() {
    return Parameters::max_per_cpu_cache_size();
  }

  static size_t num_objects_to_move(int size_class) {
    return Static::sizemap().num_objects_to_move(size_class);
  }

  static const NumaTopology<kNumaPartitions, kNumBaseClasses>& numa_topology() {
    return Static::numa_topology();
  }

  static ShardedTransferCacheManager& sharded_transfer_cache() {
    return Static::sharded_transfer_cache();
  }

  static TransferCacheManager& transfer_cache() {
    return Static::transfer_cache();
  }
};

template <typename Forwarder>
class CPUCache {
 public:
  constexpr CPUCache() = default;

  enum class ActivationMode {
    FastPathOn,
    FastPathOffTestOnly,
  };

  // tcmalloc explicitly initializes its global state (to be safe for
  // use in global constructors) so our constructor must be trivial;
  // do all initialization here instead.
  void Activate(ActivationMode mode);

  // Allocate an object of the given size class. When allocation fails
  // (from this cache and after running Refill), OOMHandler(size) is
  // called and its return value is returned from
  // Allocate. OOMHandler is used to parameterize out-of-memory
  // handling (raising exception, returning nullptr, calling
  // new_handler or anything else). "Passing" OOMHandler in this way
  // allows Allocate to be used in tail-call position in fast-path,
  // making Allocate use jump (tail-call) to slow path code.
  template <void* OOMHandler(size_t)>
  void* Allocate(size_t cl);

  // Free an object of the given class.
  void Deallocate(void* ptr, size_t cl);

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
  uint64_t TotalObjectsOfClass(size_t cl) const;

  // Give the number of bytes unallocated to any sizeclass in <cpu>'s cache.
  uint64_t Unallocated(int cpu) const;

  // Gives the total capacity of <cpu>'s cache in bytes.
  //
  // The total capacity of <cpu>'s cache should be equal to the sum of allocated
  // and unallocated bytes for that cache.
  uint64_t Capacity(int cpu) const;

  // Give the per-cpu limit of cache size.
  uint64_t CacheLimit() const;

  // Shuffles per-cpu caches using the number of underflows and overflows that
  // occurred in the prior interval. It selects the top per-cpu caches
  // with highest misses as candidates, iterates through the other per-cpu
  // caches to steal capacity from them and adds the stolen bytes to the
  // available capacity of the per-cpu caches. May be called from any processor.
  //
  // TODO(vgogte): There are quite a few knobs that we can play around with in
  // ShuffleCpuCaches.
  void ShuffleCpuCaches();

  // Sets the lower limit on the capacity that can be stolen from the cpu cache.
  static constexpr double kCacheCapacityThreshold = 0.20;

  // Tries to steal <bytes> for the destination <cpu>. It iterates through the
  // the set of populated cpu caches and steals the bytes from them. A cpu is
  // considered a good candidate to steal from if:
  // (1) the cache is populated
  // (2) the numbers of underflows and overflows are both less than 0.8x those
  // of the destination per-cpu cache
  // (3) source cpu is not the same as the destination cpu
  // (4) capacity of the source cpu/cl is non-zero
  //
  // For a given source cpu, we iterate through the size classes to steal from
  // them. Currently, we use a similar clock-like algorithm from Steal() to
  // identify the cl to steal from.
  void StealFromOtherCache(int cpu, int max_populated_cpu, size_t bytes);

  // Tries to reclaim inactive per-CPU caches. It iterates through the set of
  // populated cpu caches and reclaims the caches that:
  // (1) had same number of used bytes since the last interval,
  // (2) had no change in the number of misses since the last interval.
  void TryReclaimingCaches();

  // Empty out the cache on <cpu>; move all objects to the central
  // cache.  (If other threads run concurrently on that cpu, we can't
  // guarantee it will be fully empty on return, but if the cpu is
  // unused, this will eliminate stranded memory.)  Returns the number
  // of bytes we sent back.  This function is thread safe.
  uint64_t Reclaim(int cpu);

  // Reports number of times the <cpu> has been reclaimed.
  uint64_t GetNumReclaims(int cpu) const;

  // Determine number of bits we should use for allocating per-cpu cache
  // The amount of per-cpu cache is 2 ^ kPerCpuShift
#if defined(TCMALLOC_SMALL_BUT_SLOW)
  static const size_t kPerCpuShift = 12;
#else
  static constexpr size_t kPerCpuShift = 18;
#endif

  struct CpuCacheMissStats {
    size_t underflows;
    size_t overflows;
  };

  // Reports total cache underflows and overflows for <cpu>.
  CpuCacheMissStats GetTotalCacheMissStats(int cpu) const;

  // Reports the cache underflows and overflows for <cpu> that were recorded at
  // the end of the previous interval. It also records current underflows and
  // overflows in the reclaim underflow and overflow stats.
  CpuCacheMissStats GetReclaimCacheMissStats(int cpu) const;

  // Reports cache underflows and overflows for <cpu> this interval.
  CpuCacheMissStats GetIntervalCacheMissStats(int cpu) const;

  struct SizeClassCapacityStats {
    size_t min_capacity = 0;
    double avg_capacity = 0;
    size_t max_capacity = 0;
  };

  // Scans through populated per-CPU caches, and reports minimum, average and
  // maximum capacity for size class <cl>.
  //
  // TODO(b/205829372): Change cl to size_class here and in rest of TCMalloc to
  // adhere to C++ style guide.
  SizeClassCapacityStats GetSizeClassCapacityStats(size_t cl) const;

  // Report statistics
  void Print(Printer* out) const;
  void PrintInPbtxt(PbtxtRegion* region) const;

  const Forwarder& forwarder() const { return forwarder_; }

  Forwarder& forwarder() { return forwarder_; }

 private:
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
    static_assert(sizeof(State) == sizeof(std::atomic<int32_t>),
                  "size mismatch");
  };

  subtle::percpu::TcmallocSlab<kNumClasses> freelist_;

  struct ResizeInfoUnpadded {
    // cache space on this CPU we're not using.  Modify atomically;
    // we don't want to lose space.
    std::atomic<size_t> available;
    // this is just a hint
    std::atomic<size_t> last_steal;
    // Track whether we have initialized this CPU.
    absl::once_flag initialized;
    // Track whether we have ever populated this CPU.
    std::atomic<bool> populated;
    // For cross-cpu operations.
    absl::base_internal::SpinLock lock;
    PerClassResizeInfo per_class[kNumClasses];
    // tracks number of underflows on allocate.
    std::atomic<size_t> total_underflows;
    // tracks number of overflows on deallocate.
    std::atomic<size_t> total_overflows;
    // tracks number of underflows recorded as of the end of the last shuffle
    // interval.
    std::atomic<size_t> shuffle_underflows;
    // tracks number of overflows recorded as of the end of the last shuffle
    // interval.
    std::atomic<size_t> shuffle_overflows;
    // total cache space available on this CPU. This tracks the total
    // allocated and unallocated bytes on this CPU cache.
    std::atomic<size_t> capacity;
    // Number of underflows as of the end of the last resize interval.
    std::atomic<size_t> reclaim_underflows;
    // Number of overflows as of the end of the last resize interval.
    std::atomic<size_t> reclaim_overflows;
    // Used bytes in the cache as of the end of the last resize interval.
    std::atomic<uint64_t> reclaim_used_bytes;
    // Tracks number of times this CPU has been reclaimed.
    std::atomic<size_t> num_reclaims;
  };
  struct ResizeInfo : ResizeInfoUnpadded {
    char pad[ABSL_CACHELINE_SIZE -
             sizeof(ResizeInfoUnpadded) % ABSL_CACHELINE_SIZE];
  };
  // Tracking data for each CPU's cache resizing efforts.
  ResizeInfo* resize_ = nullptr;

  // Track whether we are lazily initializing slabs.  We cannot use the latest
  // value in Parameters, as it can change after initialization.
  bool lazy_slabs_ = false;
  // The maximum capacity of each size class within the slab.
  uint16_t max_capacity_[kNumClasses] = {0};

  // Provides a hint to StealFromOtherCache() so that we can steal from the
  // caches in a round-robin fashion.
  std::atomic<int> last_cpu_cache_steal_ = 0;

  TCMALLOC_NO_UNIQUE_ADDRESS Forwarder forwarder_;

  // Return a set of objects to be returned to the Transfer Cache.
  static constexpr int kMaxToReturn = 16;
  struct ObjectsToReturn {
    // The number of slots available for storing objects.
    int count = kMaxToReturn;
    // The size class of the returned object. kNumClasses is the
    // largest value that needs to be stored in cl.
    CompactSizeClass cl[kMaxToReturn];
    void* obj[kMaxToReturn];
  };

  static size_t MaxCapacityHelper(size_t cl) {
    CPUCache& cpu_cache = Static::cpu_cache();
    // Heuristic that the CPUCache has been activated.
    ASSERT(cpu_cache.resize_ != nullptr);
    return cpu_cache.max_capacity_[cl];
  }

  size_t MaxCapacity(size_t cl);

  void* Refill(int cpu, size_t cl);

  // This is called after finding a full freelist when attempting to push <ptr>
  // on the freelist for sizeclass <cl>.  The last arg should indicate which
  // CPU's list was full.  Returns 1.
  int Overflow(void* ptr, size_t cl, int cpu);

  // Called on <cl> freelist overflow/underflow on <cpu> to balance cache
  // capacity between size classes. Returns number of objects to return/request
  // from transfer cache. <to_return> will contain objects that need to be
  // freed.
  size_t UpdateCapacity(int cpu, size_t cl, size_t batch_length, bool overflow,
                        ObjectsToReturn* to_return);

  // Tries to obtain up to <desired_increase> bytes of freelist space on <cpu>
  // for <cl> from other <cls>. <to_return> will contain objects that need to be
  // freed.
  void Grow(int cpu, size_t cl, size_t desired_increase,
            ObjectsToReturn* to_return);

  // Tries to steal <bytes> for <cl> on <cpu> from other size classes on that
  // CPU. Returns acquired bytes. <to_return> will contain objects that need to
  // be freed.
  size_t Steal(int cpu, size_t cl, size_t bytes, ObjectsToReturn* to_return);

  // Records a cache underflow or overflow on <cpu>, increments underflow or
  // overflow by 1.
  // <is_malloc> determines whether the associated count corresponds to an
  // underflow or overflow.
  void RecordCacheMissStat(const int cpu, const bool is_malloc);

  static void* NoopUnderflow(int cpu, size_t cl) { return nullptr; }
  static int NoopOverflow(int cpu, size_t cl, void* item) { return -1; }
};

template <class Forwarder>
template <void* OOMHandler(size_t)>
inline void* ABSL_ATTRIBUTE_ALWAYS_INLINE
CPUCache<Forwarder>::Allocate(size_t cl) {
  ASSERT(cl > 0);

  tracking::Report(kMallocHit, cl, 1);
  struct Helper {
    static void* ABSL_ATTRIBUTE_NOINLINE Underflow(int cpu, size_t cl) {
      // we've optimistically reported hit in Allocate, lets undo it and
      // report miss instead.
      tracking::Report(kMallocHit, cl, -1);
      void* ret = nullptr;
      CPUCache& cache = Static::cpu_cache();
      if (Static::sharded_transfer_cache().should_use(cl)) {
        ret = Static::sharded_transfer_cache().Pop(cl);
      } else {
        tracking::Report(kMallocMiss, cl, 1);
        cache.RecordCacheMissStat(cpu, true);
        ret = cache.Refill(cpu, cl);
      }
      if (ABSL_PREDICT_FALSE(ret == nullptr)) {
        size_t size = cache.forwarder().class_to_size(cl);
        return OOMHandler(size);
      }
      return ret;
    }
  };
  return freelist_.Pop(cl, &Helper::Underflow);
}

template <class Forwarder>
inline void ABSL_ATTRIBUTE_ALWAYS_INLINE
CPUCache<Forwarder>::Deallocate(void* ptr, size_t cl) {
  ASSERT(cl > 0);
  tracking::Report(kFreeHit, cl, 1);  // Be optimistic; correct later if needed.

  struct Helper {
    static int ABSL_ATTRIBUTE_NOINLINE Overflow(int cpu, size_t cl, void* ptr) {
      // When we reach here we've already optimistically bumped FreeHits.
      // Fix that.
      tracking::Report(kFreeHit, cl, -1);
      if (Static::sharded_transfer_cache().should_use(cl)) {
        Static::sharded_transfer_cache().Push(cl, ptr);
        return 1;
      }
      tracking::Report(kFreeMiss, cl, 1);
      CPUCache& cache = Static::cpu_cache();
      cache.RecordCacheMissStat(cpu, false);
      return cache.Overflow(ptr, cl, cpu);
    }
  };
  freelist_.Push(cl, ptr, Helper::Overflow);
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

// MaxCapacity() determines how we distribute memory in the per-cpu cache
// to the various class sizes.
template <class Forwarder>
inline size_t CPUCache<Forwarder>::MaxCapacity(size_t cl) {
  // The number of size classes that are commonly used and thus should be
  // allocated more slots in the per-cpu cache.
  static constexpr size_t kNumSmall = 10;

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
  // Max(kNumClasses) is 89, so the maximum footprint per CPU is:
  //   89 * 8 + 8 * ((2048 + 1) * 10 + (152 + 1) * 78 + 88) = 254 KiB
  static const uint16_t kSmallObjectDepth = 2048;
  static const uint16_t kLargeObjectDepth = 152;
#endif
  if (cl == 0 || cl >= kNumClasses) return 0;

  if (forwarder_.sharded_transfer_cache().should_use(cl)) {
    return 0;
  }

  if (forwarder_.class_to_size(cl) == 0) {
    return 0;
  }

  if (!IsExpandedSizeClass(cl) && (cl % kNumBaseClasses) <= kNumSmall) {
    // Small object sizes are very heavily used and need very deep caches for
    // good performance (well over 90% of malloc calls are for cl <= 10.)
    return kSmallObjectDepth;
  }

  if (ColdExperimentActive()) {
    // We reduce the number of cached objects for some sizes to fit into the
    // slab.
    static constexpr uint16_t kLargeUninterestingObjectDepth = 133;
    static constexpr uint16_t kLargeInterestingObjectDepth = 152;

    absl::Span<const size_t> cold = Static::sizemap().ColdSizeClasses();
    ASSERT(!cold.empty());
    if (absl::c_binary_search(cold, cl)) {
      return kLargeInterestingObjectDepth;
    } else if (!IsExpandedSizeClass(cl)) {
      return kLargeUninterestingObjectDepth;
    } else {
      return 0;
    }
  }

  if (IsExpandedSizeClass(cl)) {
    return 0;
  }

  return kLargeObjectDepth;
}

static void* SlabAlloc(size_t size)
    ABSL_EXCLUSIVE_LOCKS_REQUIRED(pageheap_lock) {
  return Static::arena().Alloc(size);
}

template <class Forwarder>
inline void CPUCache<Forwarder>::Activate(ActivationMode mode) {
  ASSERT(Static::IsInited());
  int num_cpus = absl::base_internal::NumCPUs();

  size_t per_cpu_shift = kPerCpuShift;
  const auto& topology = forwarder_.numa_topology();
  if (topology.numa_aware()) {
    per_cpu_shift += absl::bit_ceil(topology.active_partitions() - 1);
  }

  const size_t kBytesAvailable = (1 << per_cpu_shift);
  size_t bytes_required = sizeof(std::atomic<int64_t>) * kNumClasses;

  // Deal with size classes that correspond only to NUMA partitions that are in
  // use. If NUMA awareness is disabled then we may have a smaller shift than
  // would suffice for all of the unused size classes.
  for (int cl = 0; cl < topology.active_partitions() * kNumBaseClasses; ++cl) {
    const uint16_t mc = MaxCapacity(cl);
    max_capacity_[cl] = mc;
    bytes_required += sizeof(void*) * mc;
  }

  // Deal with expanded size classes.
  for (int cl = kExpandedClassesStart; cl < kNumClasses; ++cl) {
    const uint16_t mc = MaxCapacity(cl);
    max_capacity_[cl] = mc;
    bytes_required += sizeof(void*) * mc;
  }

  // As we may make certain size classes no-ops by selecting "0" at runtime,
  // using a compile-time calculation overestimates the worst-case memory usage.
  if (ABSL_PREDICT_FALSE(bytes_required > kBytesAvailable)) {
    Crash(kCrash, __FILE__, __LINE__, "per-CPU memory exceeded, have ",
          kBytesAvailable, " need ", bytes_required);
  }

  absl::base_internal::SpinLockHolder h(&pageheap_lock);

  resize_ = reinterpret_cast<ResizeInfo*>(
      Static::arena().Alloc(sizeof(ResizeInfo) * num_cpus));
  lazy_slabs_ = forwarder_.lazy_per_cpu_caches();

  auto max_cache_size = forwarder_.max_per_cpu_cache_size();

  for (int cpu = 0; cpu < num_cpus; ++cpu) {
    for (int cl = 1; cl < kNumClasses; ++cl) {
      resize_[cpu].per_class[cl].Init();
    }
    resize_[cpu].available.store(max_cache_size, std::memory_order_relaxed);
    resize_[cpu].capacity.store(max_cache_size, std::memory_order_relaxed);
    resize_[cpu].last_steal.store(1, std::memory_order_relaxed);
  }

  freelist_.Init(SlabAlloc, MaxCapacityHelper, lazy_slabs_, per_cpu_shift);
  if (mode == ActivationMode::FastPathOn) {
    Static::ActivateCPUCache();
  }
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
inline void* CPUCache<Forwarder>::Refill(int cpu, size_t cl) {
  const size_t batch_length = forwarder_.num_objects_to_move(cl);

  // UpdateCapacity can evict objects from other size classes as it tries to
  // increase capacity of this size class. The objects are returned in
  // to_return, we insert them into transfer cache at the end of function
  // (to increase possibility that we stay on the current CPU as we are
  // refilling the list).
  ObjectsToReturn to_return;
  const size_t target =
      UpdateCapacity(cpu, cl, batch_length, false, &to_return);

  // Refill target objects in batch_length batches.
  size_t total = 0;
  size_t got;
  size_t i;
  void* result = nullptr;
  void* batch[kMaxObjectsToMove];

  do {
    const size_t want = std::min(batch_length, target - total);
    got = forwarder_.transfer_cache().RemoveRange(cl, batch, want);
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
      i -= freelist_.PushBatch(cl, batch, i);
      if (i != 0) {
        static_assert(ABSL_ARRAYSIZE(batch) >= kMaxObjectsToMove,
                      "not enough space in batch");
        forwarder_.transfer_cache().InsertRange(cl,
                                                absl::Span<void*>(batch, i));
      }
    }
  } while (got == batch_length && i == 0 && total < target &&
           cpu == freelist_.GetCurrentVirtualCpuUnsafe());

  for (int i = to_return.count; i < kMaxToReturn; ++i) {
    forwarder_.transfer_cache().InsertRange(
        to_return.cl[i], absl::Span<void*>(&(to_return.obj[i]), 1));
  }

  return result;
}

template <class Forwarder>
inline size_t CPUCache<Forwarder>::UpdateCapacity(int cpu, size_t cl,
                                                  size_t batch_length,
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

  const size_t max_capacity = max_capacity_[cl];
  size_t capacity = freelist_.Capacity(cpu, cl);
  // We assert that the return value, target, is non-zero, so starting from an
  // initial capacity of zero means we may be populating this core for the
  // first time.
  absl::base_internal::LowLevelCallOnce(
      &resize_[cpu].initialized,
      [](CPUCache* cache, int cpu) {
        if (cache->lazy_slabs_) {
          absl::base_internal::SpinLockHolder h(&cache->resize_[cpu].lock);
          cache->freelist_.InitCPU(cpu, MaxCapacityHelper);
        }

        // While we could unconditionally store, a lazy slab population
        // implementation will require evaluating a branch.
        cache->resize_[cpu].populated.store(true, std::memory_order_relaxed);
      },
      this, cpu);
  const bool grow_by_one = capacity < 2 * batch_length;
  uint32_t successive = 0;
  bool grow_by_batch =
      resize_[cpu].per_class[cl].Update(overflow, grow_by_one, &successive);
  if ((grow_by_one || grow_by_batch) && capacity != max_capacity) {
    size_t increase = 1;
    if (grow_by_batch) {
      increase = std::min(batch_length, max_capacity - capacity);
    } else if (!overflow && capacity < batch_length) {
      // On underflow we want to grow to at least batch size, because that's
      // what we want to request from transfer cache.
      increase = batch_length - capacity;
    }
    Grow(cpu, cl, increase, to_return);
    capacity = freelist_.Capacity(cpu, cl);
  }
  // Calculate number of objects to return/request from transfer cache.
  // Generally we prefer to transfer a single batch, because transfer cache
  // handles it efficiently. Except for 2 special cases:
  size_t target = batch_length;
  // "capacity + 1" because on overflow we already have one object from caller,
  // so we can return a whole batch even if capacity is one less. Similarly,
  // on underflow we need to return one object to caller, so we can request
  // a whole batch even if capacity is one less.
  if ((capacity + 1) < batch_length) {
    // If we don't have a full batch, return/request just half. We are missing
    // transfer cache anyway, and cost of insertion into central freelist is
    // ~O(number of objects).
    target = std::max<size_t>(1, (capacity + 1) / 2);
  } else if (successive > 0 && capacity >= 3 * batch_length) {
    // If the freelist is large and we are hitting series of overflows or
    // underflows, return/request several batches at once. On the first overflow
    // we return 1 batch, on the second -- 2, on the third -- 4 and so on up to
    // half of the batches we have. We do this to save on the cost of hitting
    // malloc/free slow path, reduce instruction cache pollution, avoid cache
    // misses when accessing transfer/central caches, etc.
    size_t num_batches =
        std::min<size_t>(1 << std::min<uint32_t>(successive, 10),
                         ((capacity / batch_length) + 1) / 2);
    target = num_batches * batch_length;
  }
  ASSERT(target != 0);
  return target;
}

template <class Forwarder>
inline void CPUCache<Forwarder>::Grow(int cpu, size_t cl,
                                      size_t desired_increase,
                                      ObjectsToReturn* to_return) {
  const size_t size = forwarder_.class_to_size(cl);
  const size_t desired_bytes = desired_increase * size;
  size_t acquired_bytes;

  // First, there might be unreserved slack.  Take what we can.
  size_t before, after;
  do {
    before = resize_[cpu].available.load(std::memory_order_relaxed);
    acquired_bytes = std::min(before, desired_bytes);
    after = before - acquired_bytes;
  } while (!resize_[cpu].available.compare_exchange_strong(
      before, after, std::memory_order_relaxed, std::memory_order_relaxed));

  if (acquired_bytes < desired_bytes) {
    acquired_bytes += Steal(cpu, cl, desired_bytes - acquired_bytes, to_return);
  }

  // We have all the memory we could reserve.  Time to actually do the growth.

  // We might have gotten more than we wanted (stealing from larger sizeclasses)
  // so don't grow _too_ much.
  size_t actual_increase = acquired_bytes / size;
  actual_increase = std::min(actual_increase, desired_increase);
  // Remember, Grow may not give us all we ask for.
  size_t increase = freelist_.Grow(cpu, cl, actual_increase, max_capacity_[cl]);
  size_t increased_bytes = increase * size;
  if (increased_bytes < acquired_bytes) {
    // return whatever we didn't use to the slack.
    size_t unused = acquired_bytes - increased_bytes;
    resize_[cpu].available.fetch_add(unused, std::memory_order_relaxed);
  }
}

template <class Forwarder>
inline void CPUCache<Forwarder>::TryReclaimingCaches() {
  const int num_cpus = absl::base_internal::NumCPUs();

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
    const CpuCacheMissStats miss_stats = GetReclaimCacheMissStats(cpu);
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

template <class Forwarder>
inline void CPUCache<Forwarder>::ShuffleCpuCaches() {
  // Knobs that we can potentially tune depending on the workloads.
  constexpr double kBytesToStealPercent = 5.0;
  constexpr int kMaxNumStealCpus = 5;

  const int num_cpus = absl::base_internal::NumCPUs();
  absl::FixedArray<std::pair<int, uint64_t>> misses(num_cpus);

  // Record the cumulative misses for the caches so that we can select the
  // caches with the highest misses as the candidates to steal the cache for.
  int max_populated_cpu = -1;
  int num_populated_cpus = 0;
  for (int cpu = 0; cpu < num_cpus; ++cpu) {
    if (!HasPopulated(cpu)) {
      continue;
    }
    const CpuCacheMissStats miss_stats = GetIntervalCacheMissStats(cpu);
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
  size_t to_steal =
      kBytesToStealPercent / 100.0 * forwarder_.max_per_cpu_cache_size();
  for (int i = 0; i < num_dest_cpus; ++i) {
    StealFromOtherCache(misses[i].first, max_populated_cpu, to_steal);
  }

  // Takes a snapshot of underflows and overflows at the end of this interval
  // so that we can calculate the misses that occurred in the next interval.
  for (int cpu = 0; cpu < num_cpus; ++cpu) {
    size_t underflows =
        resize_[cpu].total_underflows.load(std::memory_order_relaxed);
    size_t overflows =
        resize_[cpu].total_overflows.load(std::memory_order_relaxed);

    // Shuffle occurs on a single thread. So, the relaxed stores to
    // prev_underflow and pre_overflow counters are safe.
    resize_[cpu].shuffle_underflows.store(underflows,
                                          std::memory_order_relaxed);
    resize_[cpu].shuffle_overflows.store(overflows, std::memory_order_relaxed);
  }
}

template <typename Forwarder>
static void ShrinkHandler(void* arg, size_t cl, void** batch, size_t count) {
  Forwarder& forwarder = *static_cast<Forwarder*>(arg);

  const size_t batch_length = forwarder.num_objects_to_move(cl);
  auto& transfer_cache = forwarder.transfer_cache();
  for (size_t i = 0; i < count; i += batch_length) {
    size_t n = std::min(batch_length, count - i);
    transfer_cache.InsertRange(cl, absl::Span<void*>(batch + i, n));
  }
}

template <class Forwarder>
inline void CPUCache<Forwarder>::StealFromOtherCache(int cpu,
                                                     int max_populated_cpu,
                                                     size_t bytes) {
  constexpr double kCacheMissThreshold = 0.80;

  const CpuCacheMissStats dest_misses = GetIntervalCacheMissStats(cpu);

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
    if (Capacity(src_cpu) <
        kCacheCapacityThreshold * forwarder_.max_per_cpu_cache_size())
      continue;

    const CpuCacheMissStats src_misses = GetIntervalCacheMissStats(src_cpu);

    // If underflows and overflows from the source cpu are higher, we do not
    // steal from that cache. We consider the cache as a candidate to steal from
    // only when its misses are lower than 0.8x that of the dest cache.
    if (src_misses.underflows > kCacheMissThreshold * dest_misses.underflows ||
        src_misses.overflows > kCacheMissThreshold * dest_misses.overflows)
      continue;

    size_t start_cl =
        resize_[src_cpu].last_steal.load(std::memory_order_relaxed);

    ASSERT(start_cl < kNumClasses);
    ASSERT(0 < start_cl);
    size_t source_cl = start_cl;
    for (size_t offset = 1; offset < kNumClasses; ++offset) {
      source_cl = start_cl + offset;
      if (source_cl >= kNumClasses) {
        source_cl -= kNumClasses - 1;
      }
      ASSERT(0 < source_cl);
      ASSERT(source_cl < kNumClasses);

      const size_t capacity = freelist_.Capacity(src_cpu, source_cl);
      if (capacity == 0) {
        // Nothing to steal.
        continue;
      }
      const size_t length = freelist_.Length(src_cpu, source_cl);

      // TODO(vgogte): Currently, scoring is similar to stealing from the
      // same cpu in CPUCache<Forwarder>::Steal(). Revisit this later to tune
      // the knobs.
      const size_t batch_length = forwarder_.num_objects_to_move(source_cl);
      size_t size = forwarder_.class_to_size(source_cl);

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
      uint32_t qticks = resize_[src_cpu].per_class[source_cl].Tick();
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
        absl::base_internal::SpinLockHolder h(&resize_[src_cpu].lock);
        if (freelist_.ShrinkOtherCache(src_cpu, source_cl, 1, &forwarder_,
                                       ShrinkHandler<Forwarder>) == 1) {
          acquired += size;
          resize_[src_cpu].capacity.fetch_sub(size, std::memory_order_relaxed);
        }
      }

      if (acquired >= bytes) {
        break;
      }
    }
    resize_[cpu].last_steal.store(source_cl, std::memory_order_relaxed);
  }
  // Record the last cpu id we stole from, which would provide a hint to the
  // next time we iterate through the cpus for stealing.
  last_cpu_cache_steal_.store(src_cpu, std::memory_order_relaxed);

  // Increment the capacity of the destination cpu cache by the amount of bytes
  // acquired from source caches.
  if (acquired) {
    size_t before = resize_[cpu].available.load(std::memory_order_relaxed);
    size_t bytes_with_stolen;
    do {
      bytes_with_stolen = before + acquired;
    } while (!resize_[cpu].available.compare_exchange_weak(
        before, bytes_with_stolen, std::memory_order_relaxed,
        std::memory_order_relaxed));
    resize_[cpu].capacity.fetch_add(acquired, std::memory_order_relaxed);
  }
}

// There are rather a lot of policy knobs we could tweak here.
template <class Forwarder>
inline size_t CPUCache<Forwarder>::Steal(int cpu, size_t dest_cl, size_t bytes,
                                         ObjectsToReturn* to_return) {
  // Steal from other sizeclasses.  Try to go in a nice circle.
  // Complicated by sizeclasses actually being 1-indexed.
  size_t acquired = 0;
  size_t start = resize_[cpu].last_steal.load(std::memory_order_relaxed);
  ASSERT(start < kNumClasses);
  ASSERT(0 < start);
  size_t source_cl = start;
  for (size_t offset = 1; offset < kNumClasses; ++offset) {
    source_cl = start + offset;
    if (source_cl >= kNumClasses) {
      source_cl -= kNumClasses - 1;
    }
    ASSERT(0 < source_cl);
    ASSERT(source_cl < kNumClasses);
    // Decide if we want to steal source_cl.
    if (source_cl == dest_cl) {
      // First, no sense in picking your own pocket.
      continue;
    }
    const size_t capacity = freelist_.Capacity(cpu, source_cl);
    if (capacity == 0) {
      // Nothing to steal.
      continue;
    }
    const size_t length = freelist_.Length(cpu, source_cl);
    const size_t batch_length = forwarder_.num_objects_to_move(source_cl);
    size_t size = forwarder_.class_to_size(source_cl);

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
    uint32_t qticks = resize_[cpu].per_class[source_cl].Tick();
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

    if (length >= capacity) {
      // The list is full, need to evict an object to shrink it.
      if (to_return == nullptr) {
        continue;
      }
      if (to_return->count == 0) {
        // Can't steal any more because the to_return set is full.
        break;
      }
      void* obj = freelist_.Pop(source_cl, NoopUnderflow);
      if (obj) {
        --to_return->count;
        to_return->cl[to_return->count] = source_cl;
        to_return->obj[to_return->count] = obj;
      }
    }

    // Finally, try to shrink (can fail if we were migrated).
    // We always shrink by 1 object. The idea is that inactive lists will be
    // shrunk to zero eventually anyway (or they just would not grow in the
    // first place), but for active lists it does not make sense to aggressively
    // shuffle capacity all the time.
    if (freelist_.Shrink(cpu, source_cl, 1) == 1) {
      acquired += size;
    }

    if (cpu != freelist_.GetCurrentVirtualCpuUnsafe() || acquired >= bytes) {
      // can't steal any more or don't need to
      break;
    }
  }
  // update the hint
  resize_[cpu].last_steal.store(source_cl, std::memory_order_relaxed);
  return acquired;
}

template <class Forwarder>
inline int CPUCache<Forwarder>::Overflow(void* ptr, size_t cl, int cpu) {
  const size_t batch_length = forwarder_.num_objects_to_move(cl);
  const size_t target = UpdateCapacity(cpu, cl, batch_length, true, nullptr);
  // Return target objects in batch_length batches.
  size_t total = 0;
  size_t count = 1;
  void* batch[kMaxObjectsToMove];
  batch[0] = ptr;
  do {
    size_t want = std::min(batch_length, target - total);
    if (count < want) {
      count += freelist_.PopBatch(cl, batch + count, want - count);
    }
    if (!count) break;

    total += count;
    static_assert(ABSL_ARRAYSIZE(batch) >= kMaxObjectsToMove,
                  "not enough space in batch");
    forwarder_.transfer_cache().InsertRange(cl,
                                            absl::Span<void*>(batch, count));
    if (count != batch_length) break;
    count = 0;
  } while (total < target && cpu == freelist_.GetCurrentVirtualCpuUnsafe());
  tracking::Report(kFreeTruncations, cl, 1);
  return 1;
}

template <class Forwarder>
inline uint64_t CPUCache<Forwarder>::Allocated(int target_cpu) const {
  ASSERT(target_cpu >= 0);
  if (!HasPopulated(target_cpu)) {
    return 0;
  }

  uint64_t total = 0;
  for (int cl = 1; cl < kNumClasses; cl++) {
    int size = forwarder_.class_to_size(cl);
    total += size * freelist_.Capacity(target_cpu, cl);
  }
  return total;
}

template <class Forwarder>
inline uint64_t CPUCache<Forwarder>::UsedBytes(int target_cpu) const {
  ASSERT(target_cpu >= 0);
  if (!HasPopulated(target_cpu)) {
    return 0;
  }

  uint64_t total = 0;
  for (int cl = 1; cl < kNumClasses; cl++) {
    int size = forwarder_.class_to_size(cl);
    total += size * freelist_.Length(target_cpu, cl);
  }
  return total;
}

template <class Forwarder>
inline bool CPUCache<Forwarder>::HasPopulated(int target_cpu) const {
  ASSERT(target_cpu >= 0);
  return resize_[target_cpu].populated.load(std::memory_order_relaxed);
}

template <class Forwarder>
inline PerCPUMetadataState CPUCache<Forwarder>::MetadataMemoryUsage() const {
  return freelist_.MetadataMemoryUsage();
}

template <class Forwarder>
inline uint64_t CPUCache<Forwarder>::TotalUsedBytes() const {
  uint64_t total = 0;
  for (int cpu = 0, num_cpus = absl::base_internal::NumCPUs(); cpu < num_cpus;
       ++cpu) {
    total += UsedBytes(cpu);
  }
  return total;
}

template <class Forwarder>
inline uint64_t CPUCache<Forwarder>::TotalObjectsOfClass(size_t cl) const {
  ASSERT(cl < kNumClasses);
  uint64_t total_objects = 0;
  if (cl > 0) {
    for (int cpu = 0, n = absl::base_internal::NumCPUs(); cpu < n; cpu++) {
      if (!HasPopulated(cpu)) {
        continue;
      }
      total_objects += freelist_.Length(cpu, cl);
    }
  }
  return total_objects;
}

template <class Forwarder>
inline uint64_t CPUCache<Forwarder>::Unallocated(int cpu) const {
  return resize_[cpu].available.load(std::memory_order_relaxed);
}

template <class Forwarder>
inline uint64_t CPUCache<Forwarder>::Capacity(int cpu) const {
  return resize_[cpu].capacity.load(std::memory_order_relaxed);
}

template <class Forwarder>
inline uint64_t CPUCache<Forwarder>::CacheLimit() const {
  return forwarder_.max_per_cpu_cache_size();
}

template <class Forwarder>
struct DrainContext {
  Forwarder* forwarder;
  std::atomic<size_t>* available;
  uint64_t bytes;
};

template <class Forwarder>
static void DrainHandler(void* drain_ctx, size_t cl, void** batch, size_t count,
                         size_t cap) {
  DrainContext<Forwarder>* ctx =
      static_cast<DrainContext<Forwarder>*>(drain_ctx);
  const size_t size = ctx->forwarder->class_to_size(cl);
  const size_t batch_length = ctx->forwarder->num_objects_to_move(cl);
  ctx->bytes += count * size;
  // Drain resets capacity to 0, so return the allocated capacity to that
  // CPU's slack.
  ctx->available->fetch_add(cap * size, std::memory_order_relaxed);
  auto& transfer_cache = ctx->forwarder->transfer_cache();
  for (size_t i = 0; i < count; i += batch_length) {
    size_t n = std::min(batch_length, count - i);
    transfer_cache.InsertRange(cl, absl::Span<void*>(batch + i, n));
  }
}

template <class Forwarder>
inline uint64_t CPUCache<Forwarder>::Reclaim(int cpu) {
  absl::base_internal::SpinLockHolder h(&resize_[cpu].lock);

  // If we haven't populated this core, freelist_.Drain() will touch the memory
  // (for writing) as part of its locking process.  Avoid faulting new pages as
  // part of a release process.
  if (!resize_[cpu].populated.load(std::memory_order_relaxed)) {
    return 0;
  }

  DrainContext<Forwarder> ctx{&forwarder_, &resize_[cpu].available, 0};
  freelist_.Drain(cpu, &ctx, DrainHandler<Forwarder>);

  // Record that the reclaim occurred for this CPU.
  resize_[cpu].num_reclaims.store(
      resize_[cpu].num_reclaims.load(std::memory_order_relaxed) + 1,
      std::memory_order_relaxed);
  return ctx.bytes;
}

template <class Forwarder>
inline uint64_t CPUCache<Forwarder>::GetNumReclaims(int cpu) const {
  return resize_[cpu].num_reclaims.load(std::memory_order_relaxed);
}

template <class Forwarder>
inline void CPUCache<Forwarder>::RecordCacheMissStat(const int cpu,
                                                     const bool is_malloc) {
  if (is_malloc) {
    resize_[cpu].total_underflows.fetch_add(1, std::memory_order_relaxed);
  } else {
    resize_[cpu].total_overflows.fetch_add(1, std::memory_order_relaxed);
  }
}

template <class Forwarder>
inline typename CPUCache<Forwarder>::CpuCacheMissStats
CPUCache<Forwarder>::GetReclaimCacheMissStats(int cpu) const {
  CpuCacheMissStats stats;
  size_t total_underflows =
      resize_[cpu].total_underflows.load(std::memory_order_relaxed);
  size_t prev_reclaim_underflows =
      resize_[cpu].reclaim_underflows.load(std::memory_order_relaxed);
  // Takes a snapshot of underflows at the end of this interval so that we can
  // calculate the misses that occurred in the next interval.
  //
  // Reclaim occurs on a single thread. So, a relaxed store to the reclaim
  // underflow stat is safe.
  resize_[cpu].reclaim_underflows.store(total_underflows,
                                        std::memory_order_relaxed);

  // In case of a size_t overflow, we wrap around to 0.
  stats.underflows = total_underflows > prev_reclaim_underflows
                         ? total_underflows - prev_reclaim_underflows
                         : 0;

  size_t total_overflows =
      resize_[cpu].total_overflows.load(std::memory_order_relaxed);
  size_t prev_reclaim_overflows =
      resize_[cpu].reclaim_overflows.load(std::memory_order_relaxed);
  // Takes a snapshot of overflows at the end of this interval so that we can
  // calculate the misses that occurred in the next interval.
  //
  // Reclaim occurs on a single thread. So, a relaxed store to the reclaim
  // overflow stat is safe.
  resize_[cpu].reclaim_overflows.store(total_overflows,
                                       std::memory_order_relaxed);

  // In case of a size_t overflow, we wrap around to 0.
  stats.overflows = total_overflows > prev_reclaim_overflows
                        ? total_overflows - prev_reclaim_overflows
                        : 0;

  return stats;
}

template <class Forwarder>
inline typename CPUCache<Forwarder>::CpuCacheMissStats
CPUCache<Forwarder>::GetIntervalCacheMissStats(int cpu) const {
  CpuCacheMissStats stats;
  size_t total_underflows =
      resize_[cpu].total_underflows.load(std::memory_order_relaxed);
  size_t shuffle_underflows =
      resize_[cpu].shuffle_underflows.load(std::memory_order_relaxed);
  // In case of a size_t overflow, we wrap around to 0.
  stats.underflows = total_underflows > shuffle_underflows
                         ? total_underflows - shuffle_underflows
                         : 0;

  size_t total_overflows =
      resize_[cpu].total_overflows.load(std::memory_order_relaxed);
  size_t shuffle_overflows =
      resize_[cpu].shuffle_overflows.load(std::memory_order_relaxed);
  // In case of a size_t overflow, we wrap around to 0.
  stats.overflows = total_overflows > shuffle_overflows
                        ? total_overflows - shuffle_overflows
                        : 0;

  return stats;
}

template <class Forwarder>
inline typename CPUCache<Forwarder>::CpuCacheMissStats
CPUCache<Forwarder>::GetTotalCacheMissStats(int cpu) const {
  CpuCacheMissStats stats;
  stats.underflows =
      resize_[cpu].total_underflows.load(std::memory_order_relaxed);
  stats.overflows =
      resize_[cpu].total_overflows.load(std::memory_order_relaxed);
  return stats;
}

template <class Forwarder>
inline typename CPUCache<Forwarder>::SizeClassCapacityStats
CPUCache<Forwarder>::GetSizeClassCapacityStats(size_t cl) const {
  SizeClassCapacityStats stats;
  int num_populated = 0;
  // We use a local variable here, instead of directly updating min_capacity in
  // SizeClassCapacityStats struct to make sure we do not end up with SIZE_MAX
  // in stats.min_capacity when num_populated is equal to zero.
  size_t min_capacity = SIZE_MAX;

  // Scan through all per-CPU caches and calculate minimum, average and maximum
  // capacities for the size class <cl> across all the populated caches.
  for (int cpu = 0, num_cpus = absl::base_internal::NumCPUs(); cpu < num_cpus;
       ++cpu) {
    // We do not include stats for non-populated cpus in our average.
    if (!HasPopulated(cpu)) {
      continue;
    }

    size_t cap = freelist_.Capacity(cpu, cl);
    stats.max_capacity = std::max(stats.max_capacity, cap);
    min_capacity = std::min(min_capacity, cap);
    stats.avg_capacity += cap;
    ++num_populated;
  }
  if (num_populated > 0) {
    stats.avg_capacity /= num_populated;
    stats.min_capacity = min_capacity;
  }
  return stats;
}

template <class Forwarder>
inline void CPUCache<Forwarder>::Print(Printer* out) const {
  out->printf("------------------------------------------------\n");
  out->printf("Bytes in per-CPU caches (per cpu limit: %" PRIu64 " bytes)\n",
              CacheLimit());
  out->printf("------------------------------------------------\n");

  const cpu_set_t allowed_cpus = FillActiveCpuMask();

  for (int cpu = 0, num_cpus = absl::base_internal::NumCPUs(); cpu < num_cpus;
       ++cpu) {
    static constexpr double MiB = 1048576.0;

    uint64_t rbytes = UsedBytes(cpu);
    bool populated = HasPopulated(cpu);
    uint64_t unallocated = Unallocated(cpu);
    out->printf("cpu %3d: %12" PRIu64
                " bytes (%7.1f MiB) with"
                "%12" PRIu64 " bytes unallocated %s%s\n",
                cpu, rbytes, rbytes / MiB, unallocated,
                CPU_ISSET(cpu, &allowed_cpus) ? " active" : "",
                populated ? " populated" : "");
  }

  out->printf("------------------------------------------------\n");
  out->printf("Size class capacity statistics in per-cpu caches\n");
  out->printf("------------------------------------------------\n");

  for (int cl = 0; cl < kNumClasses; ++cl) {
    SizeClassCapacityStats stats = GetSizeClassCapacityStats(cl);
    out->printf(
        "class %3d [ %8zu bytes ] : "
        "%6zu (minimum),"
        "%7.1f (average),"
        "%6zu (maximum),"
        "%6zu maximum allowed capacity\n",
        cl, forwarder_.class_to_size(cl), stats.min_capacity,
        stats.avg_capacity, stats.max_capacity, max_capacity_[cl]);
  }

  out->printf("------------------------------------------------\n");
  out->printf("Number of per-CPU cache underflows, overflows and reclaims\n");
  out->printf("------------------------------------------------\n");
  for (int cpu = 0, num_cpus = absl::base_internal::NumCPUs(); cpu < num_cpus;
       ++cpu) {
    CpuCacheMissStats miss_stats = GetTotalCacheMissStats(cpu);
    uint64_t reclaims = GetNumReclaims(cpu);
    out->printf(
        "cpu %3d:"
        "%12" PRIu64
        " underflows,"
        "%12" PRIu64
        " overflows,"
        "%12" PRIu64 " reclaims\n",
        cpu, miss_stats.underflows, miss_stats.overflows, reclaims);
  }
}

template <class Forwarder>
inline void CPUCache<Forwarder>::PrintInPbtxt(PbtxtRegion* region) const {
  const cpu_set_t allowed_cpus = FillActiveCpuMask();

  for (int cpu = 0, num_cpus = absl::base_internal::NumCPUs(); cpu < num_cpus;
       ++cpu) {
    PbtxtRegion entry = region->CreateSubRegion("cpu_cache");
    uint64_t rbytes = UsedBytes(cpu);
    bool populated = HasPopulated(cpu);
    uint64_t unallocated = Unallocated(cpu);
    CpuCacheMissStats miss_stats = GetTotalCacheMissStats(cpu);
    uint64_t reclaims = GetNumReclaims(cpu);
    entry.PrintI64("cpu", uint64_t(cpu));
    entry.PrintI64("used", rbytes);
    entry.PrintI64("unused", unallocated);
    entry.PrintBool("active", CPU_ISSET(cpu, &allowed_cpus));
    entry.PrintBool("populated", populated);
    entry.PrintI64("underflows", miss_stats.underflows);
    entry.PrintI64("overflows", miss_stats.overflows);
    entry.PrintI64("reclaims", reclaims);
  }

  // Record size class capacity statistics.
  for (int cl = 0; cl < kNumClasses; ++cl) {
    SizeClassCapacityStats stats = GetSizeClassCapacityStats(cl);
    PbtxtRegion entry = region->CreateSubRegion("size_class_capacity");
    entry.PrintI64("min_capacity", stats.min_capacity);
    entry.PrintDouble("avg_capacity", stats.avg_capacity);
    entry.PrintI64("max_capacity", stats.max_capacity);
    entry.PrintI64("max_allowed_capacity", max_capacity_[cl]);
  }
}

template <class Forwarder>
inline void CPUCache<Forwarder>::PerClassResizeInfo::Init() {
  state_.store(0, std::memory_order_relaxed);
}

template <class Forwarder>
inline bool CPUCache<Forwarder>::PerClassResizeInfo::Update(
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
inline uint32_t CPUCache<Forwarder>::PerClassResizeInfo::Tick() {
  int32_t raw = state_.load(std::memory_order_relaxed);
  State state;
  memcpy(&state, &raw, sizeof(state));
  state.quiescent_ticks++;
  memcpy(&raw, &state, sizeof(raw));
  state_.store(raw, std::memory_order_relaxed);
  return state.quiescent_ticks - 1;
}

}  // namespace cpu_cache_internal

// Static forward declares CPUCache to avoid a cycle in headers.  Make
// "CPUCache" be non-templated to avoid breaking that forward declaration.
class CPUCache final
    : public cpu_cache_internal::CPUCache<cpu_cache_internal::StaticForwarder> {
};

inline bool UsePerCpuCache() {
  // We expect a fast path of per-CPU caches being active and the thread being
  // registered with rseq.
  if (ABSL_PREDICT_FALSE(!Static::CPUCacheActive())) {
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
  // onto the slow path (if !defined(TCMALLOC_DEPRECATED_PERTHREAD)) until this
  // occurs.  See fast_alloc's use of TryRecordAllocationFast.
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
