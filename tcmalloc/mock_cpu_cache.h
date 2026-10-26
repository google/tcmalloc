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

#ifndef TCMALLOC_MOCK_CPU_CACHE_H_
#define TCMALLOC_MOCK_CPU_CACHE_H_

#if defined(__linux__)
#include <linux/param.h>
#else
#include <sys/param.h>
#endif
#include <sys/mman.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <new>
#include <optional>
#include <vector>

#include "gtest/gtest.h"
#include "absl/base/internal/spinlock.h"
#include "absl/base/thread_annotations.h"
#include "absl/numeric/bits.h"
#include "absl/strings/string_view.h"
#include "tcmalloc/common.h"
#include "tcmalloc/cpu_cache.h"
#include "tcmalloc/internal/allocation_guard.h"
#include "tcmalloc/internal/config.h"
#include "tcmalloc/internal/logging.h"
#include "tcmalloc/internal/numa.h"
#include "tcmalloc/internal/percpu_tcmalloc.h"
#include "tcmalloc/internal/sysinfo.h"
#include "tcmalloc/mock_central_freelist.h"
#include "tcmalloc/mock_transfer_cache.h"
#include "tcmalloc/parameters.h"
#include "tcmalloc/sizemap.h"
#include "tcmalloc/transfer_cache.h"

namespace tcmalloc {
namespace tcmalloc_internal {

class CpuCachePeer {
 public:
  template <typename CpuCache>
  static uint8_t GetSlabShift(const CpuCache& cpu_cache) {
    return cpu_cache.freelist_.GetShift();
  }

  template <typename CpuCache>
  static void IncrementCacheMisses(CpuCache& cpu_cache) {
    cpu_cache.RecordCacheMissStat(/*cpu=*/0, /*is_alloc=*/true);
    cpu_cache.RecordCacheMissStat(/*cpu=*/0, /*is_alloc=*/false);
  }

  // Validate that we're using >99% of the available slab bytes.
  template <typename CpuCache>
  static void ValidateSlabBytes(CpuCache& cpu_cache) {
    SlabShiftBounds bounds = cpu_cache.GetPerCpuSlabShiftBounds();
    for (uint8_t shift = bounds.initial_shift;
         shift <= bounds.max_shift && shift > kInitialBasePerCpuShift;
         ++shift) {
      std::atomic<uint16_t> max_capacity[kNumClasses] = {0};

      cpu_cache.CalculateMaxCapacityForAllClasses(shift, max_capacity);
      const size_t bytes_available = 1 << shift;
      const size_t bytes_required = CpuCache::Freelist::EstimateSlabBytes(
          MaxCapacityFunctor{max_capacity});
      EXPECT_GT(bytes_required * 100, bytes_available * 99)
          << bytes_required << " " << bytes_available << " " << kNumaPartitions
          << " " << kNumBaseClasses << " " << kNumClasses;
      EXPECT_GT(bytes_required * 20, bytes_available * 17);
      EXPECT_LE(bytes_required, bytes_available);
    }
  }

  template <typename CpuCache>
  static size_t ResizeInfoSize() {
    return sizeof(typename CpuCache::ResizeInfo);
  }

  template <typename CpuCache>
  static size_t CpuStateSize(const CpuCache& cpu_cache) {
    return cpu_cache.freelist_.GetCpuStateSize();
  }

  template <typename CpuCache>
  static void MadviseAwaySlabs(CpuCache& cpu_cache, void* slab_addr,
                               size_t slab_size) {
    cpu_cache.MadviseAwaySlabs(slab_addr, slab_size);
  }

  template <typename CpuCache>
  static int64_t Available(const CpuCache& cpu_cache, int cpu) {
    return cpu_cache.resize_[cpu].available.load(std::memory_order_relaxed);
  }

  template <typename CpuCache>
  static void CheckResizeLockInvariants(CpuCache& cpu_cache, int cpu) {
    AllocationGuardSpinLockHolder h(cpu_cache.resize_[cpu].lock);
    EXPECT_GE(Available(cpu_cache, cpu), 0);
    EXPECT_EQ(cpu_cache.Allocated(cpu) + cpu_cache.Unallocated(cpu),
              cpu_cache.Capacity(cpu));
  }

  template <typename CpuCache>
  static size_t SubtractFromAvailable(CpuCache& cpu_cache, int cpu,
                                      size_t bytes) {
    return subtract_at_least(&cpu_cache.resize_[cpu].available, bytes, bytes);
  }

  template <typename CpuCache>
  static void AddToAvailable(CpuCache& cpu_cache, int cpu, size_t bytes) {
    cpu_cache.resize_[cpu].available.fetch_add(bytes,
                                               std::memory_order_relaxed);
  }

  template <typename CpuCache>
  static int64_t TakeAvailable(CpuCache& cpu_cache, int cpu) {
    return cpu_cache.resize_[cpu].TakeAvailable();
  }

  template <typename CpuCache>
  static void HoldResizeLockWithZeroAvailable(CpuCache& cpu_cache, int cpu,
                                              std::atomic<bool>& ready,
                                              std::atomic<bool>& release) {
    AllocationGuardSpinLockHolder h(cpu_cache.resize_[cpu].lock);
    auto available =
        cpu_cache.resize_[cpu].available.exchange(0, std::memory_order_relaxed);
    ready.store(true, std::memory_order_release);
    while (!release.load(std::memory_order_acquire)) {
    }
    cpu_cache.resize_[cpu].available.fetch_add(available,
                                               std::memory_order_relaxed);
  }

  template <typename CpuCache>
  static uint8_t PartitionShift(const CpuCache& cpu_cache) {
    return cpu_cache.PartitionShift();
  }

  template <typename CpuCache>
  static void RecordCapacityMiss(CpuCache& cpu_cache, int cpu,
                                 size_t size_class) {
    cpu_cache.resize_[cpu].per_class[size_class].RecordMiss(
        CpuCache::PerClassMissType::kCapacityTotal);
  }

  template <typename CpuCache>
  static void ResizeCpuSizeClasses(CpuCache& cpu_cache, int cpu) {
    cpu_cache.ResizeCpuSizeClasses(cpu);
  }

  // Drains <cpu> and marks it unpopulated, as TryDrainingCaches does through
  // ReleaseSlabMetadataForDrainedCpus, so that the next allocation on it takes
  // the Populate path again.
  template <typename CpuCache>
  static uint64_t Reclaim(CpuCache& cpu_cache, int cpu) {
    const uint64_t bytes = cpu_cache.Drain(cpu);
    AllocationGuardSpinLockHolder h(cpu_cache.resize_[cpu].lock);
    if (cpu_cache.resize_[cpu].populated.load(std::memory_order_relaxed)) {
      cpu_cache.resize_[cpu].populated.store(false, std::memory_order_release);
      cpu_cache.resize_[cpu].num_unpopulates.fetch_add(
          1, std::memory_order_relaxed);
    }
    return bytes;
  }

  // Releases the slab metadata of every drained CPU, as TryDrainingCaches does
  // when release_drained_slab_metadata() is set, without requiring two idle
  // drain intervals first.
  template <typename CpuCache>
  static void ReleaseDrainedSlabMetadata(CpuCache& cpu_cache)
      ABSL_NO_THREAD_SAFETY_ANALYSIS {
    const int num_cpus = NumCPUs();
    // As in TryDrainingCaches, resize_lock_ keeps capacity stable for the
    // available <= capacity check.
    AllocationGuardSpinLockHolder l(cpu_cache.resize_lock_);
    for (int cpu = 0; cpu < num_cpus; ++cpu) {
      cpu_cache.resize_[cpu].lock.lock();
    }
    cpu_cache.freelist_.ReleaseSlabMetadataForDrainedCpus(
        [&cpu_cache](int cpu) { return cpu_cache.HasPopulated(cpu); },
        [&cpu_cache](int cpu) {
          TC_CHECK_LE(
              cpu_cache.resize_[cpu].available.load(std::memory_order_relaxed),
              static_cast<int64_t>(cpu_cache.resize_[cpu].capacity()));
          cpu_cache.resize_[cpu].populated.store(false,
                                                 std::memory_order_release);
          cpu_cache.resize_[cpu].num_unpopulates.fetch_add(
              1, std::memory_order_relaxed);
        },
        [&cpu_cache](void* slab_addr, size_t slab_size) {
          return cpu_cache.MadviseAwaySlabs(slab_addr, slab_size,
                                            /*skip_nohugepage=*/true);
        });
    for (int cpu = 0; cpu < num_cpus; ++cpu) {
      cpu_cache.resize_[cpu].lock.unlock();
    }
  }
};

enum class DynamicSlab { kGrow, kShrink, kNoop };

class TestStaticForwarder : private Parameters {
 public:
  using Parameters::per_cpu_caches_dynamic_slab_grow_threshold;
  using Parameters::per_cpu_caches_dynamic_slab_shrink_threshold;
  TestStaticForwarder() : sharded_manager_(&cpu_layout_) {
    numa_topology_.Init();
  }

  void InitializeShardedManager(int num_shards) {
    cpu_layout_.Init(num_shards);
    sharded_manager_.Init();
  }

  static void* AllocCpuCache(size_t size, std::align_val_t alignment) {
    const size_t align = static_cast<size_t>(alignment);
    TC_ASSERT(absl::has_single_bit(align));

    size_t alloc_size = size;
    if (align > EXEC_PAGESIZE) {
      // Allocate enough memory that we know for sure
      // we will be able to satisfy the request.
      // We will free the part we don't use, below.
      alloc_size += align - EXEC_PAGESIZE;
    }
    void* ptr = mmap(nullptr, alloc_size, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (ptr == MAP_FAILED) {
      return ptr;
    }
    if (alloc_size > size) {
      uintptr_t addr = reinterpret_cast<uintptr_t>(ptr);

      // Release the area before the aligned block, if any.
      uintptr_t offset = addr & (align - 1);
      if (offset != 0) {
        size_t before = align - offset;
        munmap(ptr, before);
        addr += before;
        alloc_size -= before;
        ptr = reinterpret_cast<void*>(addr);
      }

      // Release the area after the aligned block, if any.
      if (alloc_size > size) {
        munmap(reinterpret_cast<void*>(addr + size), alloc_size - size);
      }
    }
    return ptr;
  }

  void* AllocReportedImpending(size_t size, std::align_val_t alignment) {
    arena_reported_impending_bytes_ -= static_cast<int64_t>(size);
    // Resized slabs must be aligned to their size, as the arena guarantees, for
    // ReleaseSlabMetadataForDrainedCpus to accept them.
    return AllocCpuCache(size, alignment);
  }

  struct SetAnonVmaNameCall {
    void* ptr;
    size_t size;
    char name[64] = {0};
  };
  // CpuCache names slab regions while holding every resize_ lock and an
  // AllocationGuard, so SetAnonVmaName must not allocate.  Calls are recorded
  // in fixed storage; those beyond kMaxVmaNameCalls are counted but dropped.
  static constexpr size_t kMaxVmaNameCalls = 64;
  absl::base_internal::SpinLock vma_name_mu_;
  std::array<SetAnonVmaNameCall, kMaxVmaNameCalls> vma_name_calls_
      ABSL_GUARDED_BY(vma_name_mu_);
  size_t num_vma_name_calls_ ABSL_GUARDED_BY(vma_name_mu_) = 0;

  std::vector<SetAnonVmaNameCall> vma_name_calls() {
    absl::base_internal::SpinLockHolder l(vma_name_mu_);
    return std::vector<SetAnonVmaNameCall>(
        vma_name_calls_.begin(),
        vma_name_calls_.begin() +
            std::min(num_vma_name_calls_, kMaxVmaNameCalls));
  }

  void clear_vma_name_calls() {
    absl::base_internal::SpinLockHolder l(vma_name_mu_);
    num_vma_name_calls_ = 0;
  }

  void SetAnonVmaName(void* ptr, size_t size,
                      std::optional<absl::string_view> name) {
    TC_CHECK_EQ(reinterpret_cast<uintptr_t>(ptr) % kHugePageSize, 0);
    TC_CHECK_EQ(size % kHugePageSize, 0);
    TC_CHECK(name.has_value());

    absl::base_internal::SpinLockHolder l(vma_name_mu_);
    const size_t index = num_vma_name_calls_++;
    if (index >= kMaxVmaNameCalls) return;
    SetAnonVmaNameCall& elem = vma_name_calls_[index];
    elem.ptr = ptr;
    elem.size = size;
    const size_t len = std::min(name->size(), sizeof(elem.name) - 1);
    memcpy(elem.name, name->data(), len);
    elem.name[len] = '\0';
  }

  static void Dealloc(void* ptr, size_t size, std::align_val_t /*alignment*/) {
    munmap(ptr, size);
  }

  void ArenaUpdateAllocatedAndNonresident(int64_t allocated,
                                          int64_t nonresident) {
    if (allocated > 0) {
      EXPECT_EQ(arena_reported_impending_bytes_, 0);
      ++shrink_to_usage_limit_calls_;
    }

    if (nonresident == 0) {
      arena_reported_impending_bytes_ += allocated;
    } else {
      arena_reported_impending_bytes_ = 0;
    }
    arena_reported_nonresident_bytes_ += nonresident;
  }

  bool per_cpu_caches_dynamic_slab_enabled() { return dynamic_slab_enabled_; }

  double per_cpu_caches_dynamic_slab_grow_threshold() {
    if (dynamic_slab_grow_threshold_ >= 0) return dynamic_slab_grow_threshold_;
    return dynamic_slab_ == DynamicSlab::kGrow
               ? -1.0
               : std::numeric_limits<double>::max();
  }

  double per_cpu_caches_dynamic_slab_shrink_threshold() {
    if (dynamic_slab_shrink_threshold_ >= 0)
      return dynamic_slab_shrink_threshold_;
    return dynamic_slab_ == DynamicSlab::kShrink
               ? std::numeric_limits<double>::max()
               : -1.0;
  }

  bool reuse_size_classes() const { return true; }

  size_t class_to_size(int size_class) const {
    if (size_map_.has_value()) {
      // TODO(b/567919967): SizeMap::Init uses tc_globals's configuration rather
      // than this forwarder's, so size_map_ may have more partitions populated
      // than active_partitions() and we need to fix it up here (and in
      // num_objects_to_move). The proper fix would be to allow mocking
      // config/system info in SizeMap, which we don't do yet.
      if (!IsColdSizeClass(size_class) &&
          size_class >= active_partitions() * kNumBaseClasses) {
        return 0;
      }
      return size_map_->class_to_size(size_class);
    } else {
      return transfer_cache_.class_to_size(size_class);
    }
  }

  size_t num_objects_to_move(int size_class) const {
    if (size_map_.has_value()) {
      if (!IsColdSizeClass(size_class) &&
          size_class >= active_partitions() * kNumBaseClasses) {
        return 0;
      }
      return size_map_->num_objects_to_move(size_class);
    } else {
      return transfer_cache_.num_objects_to_move(size_class);
    }
  }

  const NumaTopology<kNumaPartitions, kNumBaseClasses>& numa_topology() const {
    return numa_topology_;
  }

  bool UseWiderSlabs() const { return true; }

  using ShardedManager =
      ShardedTransferCacheManagerBase<FakeShardedTransferCacheForwarder,
                                      FakeCpuLayout,
                                      MinimalFakeCentralFreeList>;

  ShardedManager& sharded_transfer_cache() { return sharded_manager_; }

  const ShardedManager& sharded_transfer_cache() const {
    return sharded_manager_;
  }

  ThreeSizeClassManager<FakeCentralFreeList,
                        internal_transfer_cache::TransferCache>&
  transfer_cache() {
    return transfer_cache_;
  }

  void SetGenericShardedCache(bool value) {
    sharded_manager_.forwarder().SetGenericShardedCache(value);
  }
  void SetShardedCacheForLargeClassesOnly(bool value) {
    sharded_manager_.forwarder().SetShardedCacheForLargeClassesOnly(value);
  }

  bool HaveHooks() const {
    // TODO(b/242550501): Test other states.
    return false;
  }

  size_t active_partitions() const { return active_partitions_; }

  bool multiple_non_numa_partitions() const {
    // TODO(b/446814339): Test other states.
    return false;
  }

  bool release_drained_slab_metadata() const {
    return release_drained_slab_metadata_;
  }

  size_t arena_reported_nonresident_bytes_ = 0;
  int64_t arena_reported_impending_bytes_ = 0;
  size_t shrink_to_usage_limit_calls_ = 0;
  bool dynamic_slab_enabled_ = false;
  double dynamic_slab_grow_threshold_ = -1;
  double dynamic_slab_shrink_threshold_ = -1;
  DynamicSlab dynamic_slab_ = DynamicSlab::kNoop;
  std::optional<SizeMap> size_map_;
  bool release_drained_slab_metadata_ = false;
  size_t active_partitions_ = 1u;

 private:
  NumaTopology<kNumaPartitions, kNumBaseClasses> numa_topology_;
  FakeCpuLayout cpu_layout_;
  ShardedManager sharded_manager_;
  ThreeSizeClassManager<FakeCentralFreeList,
                        internal_transfer_cache::TransferCache>
      transfer_cache_;
};

}  // namespace tcmalloc_internal
}  // namespace tcmalloc

#endif  // TCMALLOC_MOCK_CPU_CACHE_H_
