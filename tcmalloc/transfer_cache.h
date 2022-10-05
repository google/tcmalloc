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

#ifndef TCMALLOC_TRANSFER_CACHE_H_
#define TCMALLOC_TRANSFER_CACHE_H_

#include <stddef.h>
#include <stdint.h>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <limits>
#include <optional>
#include <utility>

#include "absl/base/attributes.h"
#include "absl/base/call_once.h"
#include "absl/base/internal/spinlock.h"
#include "absl/base/macros.h"
#include "absl/base/optimization.h"
#include "absl/base/thread_annotations.h"
#include "absl/types/span.h"
#include "tcmalloc/central_freelist.h"
#include "tcmalloc/common.h"
#include "tcmalloc/internal/cache_topology.h"
#include "tcmalloc/internal/logging.h"
#include "tcmalloc/internal/percpu.h"
#include "tcmalloc/transfer_cache_stats.h"

#ifndef TCMALLOC_SMALL_BUT_SLOW
#include "tcmalloc/transfer_cache_internals.h"
#endif

GOOGLE_MALLOC_SECTION_BEGIN
namespace tcmalloc {
namespace tcmalloc_internal {

enum class TransferCacheImplementation {
  Legacy,
  None,
  Ring,
};

absl::string_view TransferCacheImplementationToLabel(
    TransferCacheImplementation type);

#ifndef TCMALLOC_SMALL_BUT_SLOW

class StaticForwarder {
 public:
  static constexpr size_t kNumClasses =
      tcmalloc::tcmalloc_internal::kNumClasses;

  static size_t class_to_size(int size_class);
  static size_t num_objects_to_move(int size_class);
  static void *Alloc(size_t size, int alignment = kAlignment)
      ABSL_EXCLUSIVE_LOCKS_REQUIRED(pageheap_lock);
  static bool PartialLegacyTransferCache() {
    return Parameters::partial_transfer_cache();
  }
};

class ShardedStaticForwarder : public StaticForwarder {
 public:
  static void Init() {
    // When generic sharded cache experiment is enabled and the traditional
    // cache experiment is disabled, we use sharded cache for all size classes.
    // To make sure that we do not change the behavior of the traditional
    // sharded cache configuration, we use generic version of the cache only
    // when the traditional version is not enabled.
    //
    // TODO(b/250929998): Delete this experiment after evaluation.
    use_generic_cache_ =
        IsExperimentActive(
            Experiment::TEST_ONLY_TCMALLOC_GENERIC_SHARDED_TRANSFER_CACHE) &&
        !IsExperimentActive(
            Experiment::TEST_ONLY_TCMALLOC_SHARDED_TRANSFER_CACHE);

    // Traditionally, we enable sharded transfer cache for large size classes
    // alone.
    enable_cache_for_large_classes_only_ = IsExperimentActive(
        Experiment::TEST_ONLY_TCMALLOC_SHARDED_TRANSFER_CACHE);
  }

  static bool UseGenericCache() { return use_generic_cache_; }

  static bool EnableCacheForLargeClassesOnly() {
    return enable_cache_for_large_classes_only_;
  }

 private:
  static bool use_generic_cache_;
  static bool enable_cache_for_large_classes_only_;
};

class ProdCpuLayout {
 public:
  static int CurrentCpu() { return subtle::percpu::RseqCpuId(); }
  static int BuildCacheMap(uint8_t l3_cache_index[CPU_SETSIZE]) {
    return BuildCpuToL3CacheMap(l3_cache_index);
  }
};

// Forwards calls to the unsharded TransferCache.
class BackingTransferCache {
 public:
  void Init(int size_class) { size_class_ = size_class; }
  void InsertRange(absl::Span<void *> batch) const;
  ABSL_MUST_USE_RESULT int RemoveRange(void **batch, int n) const;
  int size_class() const { return size_class_; }

 private:
  int size_class_ = -1;
};

// This transfer-cache is set up to be sharded per L3 cache. It is backed by
// the non-sharded "normal" TransferCacheManager.
template <typename Manager, typename CpuLayout, typename FreeList>
class ShardedTransferCacheManagerBase {
 public:
  constexpr ShardedTransferCacheManagerBase(Manager *owner,
                                            CpuLayout *cpu_layout)
      : owner_(owner), cpu_layout_(cpu_layout) {}

  void Init() ABSL_EXCLUSIVE_LOCKS_REQUIRED(pageheap_lock) {
    owner_->Init();
    num_shards_ = CpuLayout::BuildCacheMap(l3_cache_index_);
    shards_ = reinterpret_cast<Shard *>(
        owner_->Alloc(sizeof(Shard) * num_shards_, ABSL_CACHELINE_SIZE));
    ASSERT(shards_ != nullptr);
    for (int shard = 0; shard < num_shards_; ++shard) {
      new (&shards_[shard]) Shard;
    }
    for (int size_class = 0; size_class < kNumClasses; ++size_class) {
      const int size_per_object = Manager::class_to_size(size_class);
      // We enable sharded transfer cache for all the size classes when a
      // generic sharded transfer cache is enabled. Otherwise, we enable it for
      // size classes of >= 4096 with a traditional sharded cache
      // implementation.
      static const int min_size = UseGenericCache() ? 0 : 4096;
      active_for_class_[size_class] = size_per_object >= min_size;
    }
  }

  bool should_use(int size_class) const {
    return active_for_class_[size_class];
  }

  size_t TotalBytes() {
    if (shards_ == nullptr) return 0;
    size_t out = 0;
    for (int shard = 0; shard < num_shards_; ++shard) {
      if (!shard_initialized(shard)) continue;
      for (int size_class = 0; size_class < kNumClasses; ++size_class) {
        const int bytes_per_entry = Manager::class_to_size(size_class);
        if (bytes_per_entry <= 0) continue;
        out += shards_[shard].transfer_caches[size_class].tc_length() *
               bytes_per_entry;
      }
    }
    return out;
  }

  void *Pop(int size_class) {
    void *batch[1];
    const int got = get_cache(size_class).RemoveRange(size_class, batch, 1);
    return got == 1 ? batch[0] : nullptr;
  }

  void Push(int size_class, void *ptr) {
    get_cache(size_class).InsertRange(size_class, {&ptr, 1});
  }

  // Returns cumulative stats over all the shards of the sharded transfer cache.
  TransferCacheStats GetStats(int size_class) const {
    TransferCacheStats stats = {};
    for (int index = 0; index < num_shards_; ++index) {
      if (!shard_initialized(index)) continue;
      Shard &shard = shards_[index];
      TransferCacheStats shard_stats =
          shard.transfer_caches[size_class].GetStats();
      stats.insert_hits += shard_stats.insert_hits;
      stats.insert_misses += shard_stats.insert_misses;
      stats.insert_non_batch_misses += shard_stats.insert_non_batch_misses;
      stats.remove_hits += shard_stats.remove_hits;
      stats.remove_misses += shard_stats.remove_misses;
      stats.remove_non_batch_misses += shard_stats.remove_non_batch_misses;
      stats.used += shard_stats.used;
      stats.capacity += shard_stats.capacity;
      stats.max_capacity += shard_stats.max_capacity;
    }
    return stats;
  }

  int RemoveRange(int size_class, void **batch, size_t count) {
    return get_cache(size_class).RemoveRange(size_class, batch, count);
  }

  void InsertRange(int size_class, absl::Span<void *> batch) {
    get_cache(size_class).InsertRange(size_class, batch);
  }

  // All caches not touched since last attempt will return all objects
  // to the non-sharded TransferCache.
  void Plunder() {
    if (shards_ == nullptr || num_shards_ == 0) return;
    for (int shard = 0; shard < num_shards_; ++shard) {
      if (!shard_initialized(shard)) continue;
      for (int size_class = 0; size_class < kNumClasses; ++size_class) {
        TransferCache &cache = shards_[shard].transfer_caches[size_class];
        cache.TryPlunder(cache.freelist().size_class());
      }
    }
  }

  int tc_length(int cpu, int size_class) {
    if (shards_ == nullptr) return 0;
    const uint8_t shard = l3_cache_index_[cpu];
    if (!shard_initialized(shard)) return 0;
    return shards_[shard].transfer_caches[size_class].tc_length();
  }

  bool shard_initialized(int shard) const {
    if (shards_ == nullptr) return false;
    return shards_[shard].initialized.load(std::memory_order_acquire);
  }

  bool UseCacheForLargeClassesOnly() const {
    return Manager::EnableCacheForLargeClassesOnly();
  }

  bool UseGenericCache() const { return Manager::UseGenericCache(); }

  int NumActiveShards() const {
    return active_shards_.load(std::memory_order_relaxed);
  }

 private:
  using TransferCache =
      internal_transfer_cache::TransferCache<FreeList, Manager>;

  // Store the transfer cache pointers and information about whether they are
  // initialized next to each other.
  struct Shard {
    Shard() {
      // The constructor of atomic values is not atomic. Set the value
      // explicitly and atomically here.
      initialized.store(false, std::memory_order_release);
    }
    TransferCache *transfer_caches = nullptr;
    absl::once_flag once_flag;
    // We need to be able to tell whether a given shard is initialized, which
    // the `once_flag` API doesn't offer.
    std::atomic<bool> initialized;
  };

  struct Capacity {
    int capacity;
    int max_capacity;
  };

  Capacity LargeCacheCapacity(size_t size_class) const {
    const int size_per_object = Manager::class_to_size(size_class);
    static constexpr int k12MB = 12 << 20;
    const int capacity = should_use(size_class) ? k12MB / size_per_object : 0;
    return {capacity, capacity};
  }

  Capacity ScaledCacheCapacity(size_t size_class) const {
    auto [capacity, max_capacity] = TransferCache::CapacityNeeded(size_class);
    return {capacity, max_capacity};
  }

  // Initializes all transfer caches in the given shard.
  void InitShard(Shard &shard) ABSL_LOCKS_EXCLUDED(pageheap_lock) {
    absl::base_internal::SpinLockHolder h(&pageheap_lock);
    TransferCache *new_caches = reinterpret_cast<TransferCache *>(owner_->Alloc(
        sizeof(TransferCache) * kNumClasses, ABSL_CACHELINE_SIZE));
    ASSERT(new_caches != nullptr);
    for (int size_class = 0; size_class < kNumClasses; ++size_class) {
      Capacity capacity = UseGenericCache() ? ScaledCacheCapacity(size_class)
                                            : LargeCacheCapacity(size_class);
      new (&new_caches[size_class])
          TransferCache(owner_, capacity.capacity > 0 ? size_class : 0,
                        {capacity.capacity, capacity.max_capacity});
      new_caches[size_class].freelist().Init(size_class);
    }
    shard.transfer_caches = new_caches;
    active_shards_.fetch_add(1, std::memory_order_relaxed);
    shard.initialized.store(true, std::memory_order_release);
  }

  // Returns the cache shard corresponding to the given size class and the
  // current cpu's L3 node. The cache will be initialized if required.
  TransferCache &get_cache(int size_class) {
    const int cpu = cpu_layout_->CurrentCpu();
    ASSERT(cpu < ABSL_ARRAYSIZE(l3_cache_index_));
    ASSERT(cpu >= 0);
    const uint8_t shard_index = l3_cache_index_[cpu];
    ASSERT(shard_index < num_shards_);
    Shard &shard = shards_[shard_index];
    absl::call_once(shard.once_flag, [this, &shard]() { InitShard(shard); });
    return shard.transfer_caches[size_class];
  }

  // Mapping from cpu to the L3 cache used.
  uint8_t l3_cache_index_[CPU_SETSIZE] = {0};

  Shard *shards_ = nullptr;
  int num_shards_ = 0;
  std::atomic<int> active_shards_ = 0;
  bool active_for_class_[kNumClasses] = {false};
  Manager *const owner_;
  CpuLayout *const cpu_layout_;
};

using ShardedTransferCacheManager =
    ShardedTransferCacheManagerBase<ShardedStaticForwarder, ProdCpuLayout,
                                    BackingTransferCache>;

class TransferCacheManager : public StaticForwarder {
  template <typename CentralFreeList, typename Manager>
  friend class internal_transfer_cache::TransferCache;
  using TransferCache =
      internal_transfer_cache::TransferCache<tcmalloc_internal::CentralFreeList,
                                             TransferCacheManager>;

  template <typename CentralFreeList, typename Manager>
  friend class internal_transfer_cache::RingBufferTransferCache;
  using RingBufferTransferCache =
      internal_transfer_cache::RingBufferTransferCache<
          tcmalloc_internal::CentralFreeList, TransferCacheManager>;

  friend class FakeMultiClassRingBufferManager;
  friend class FakeMultiClassTransferCacheManager;

 public:
  constexpr TransferCacheManager() {}

  TransferCacheManager(const TransferCacheManager &) = delete;
  TransferCacheManager &operator=(const TransferCacheManager &) = delete;

  void Init() ABSL_EXCLUSIVE_LOCKS_REQUIRED(pageheap_lock) {
    implementation_ = ChooseImplementation();
    InitCaches();
  }

  void InsertRange(int size_class, absl::Span<void *> batch) {
    if (implementation_ == TransferCacheImplementation::Ring) {
      cache_[size_class].rbtc.InsertRange(size_class, batch);
    } else {
      cache_[size_class].tc.InsertRange(size_class, batch);
    }
  }

  ABSL_MUST_USE_RESULT int RemoveRange(int size_class, void **batch, int n) {
    if (implementation_ == TransferCacheImplementation::Ring) {
      return cache_[size_class].rbtc.RemoveRange(size_class, batch, n);
    } else {
      return cache_[size_class].tc.RemoveRange(size_class, batch, n);
    }
  }

  // This is not const because the underlying ring-buffer transfer cache
  // function requires acquiring a lock.
  size_t tc_length(int size_class) {
    if (implementation_ == TransferCacheImplementation::Ring) {
      return cache_[size_class].rbtc.tc_length();
    } else {
      return cache_[size_class].tc.tc_length();
    }
  }

  bool HasSpareCapacity(int size_class) {
    if (implementation_ == TransferCacheImplementation::Ring) {
      return cache_[size_class].rbtc.HasSpareCapacity(size_class);
    } else {
      return cache_[size_class].tc.HasSpareCapacity(size_class);
    }
  }

  TransferCacheStats GetStats(int size_class) {
    if (implementation_ == TransferCacheImplementation::Ring) {
      return cache_[size_class].rbtc.GetStats();
    } else {
      return cache_[size_class].tc.GetStats();
    }
  }

  CentralFreeList &central_freelist(int size_class) {
    if (implementation_ == TransferCacheImplementation::Ring) {
      return cache_[size_class].rbtc.freelist();
    } else {
      return cache_[size_class].tc.freelist();
    }
  }

  TransferCacheImplementation implementation() const { return implementation_; }

  bool CanIncreaseCapacity(int size_class) {
    if (implementation_ == TransferCacheImplementation::Ring) {
      return cache_[size_class].rbtc.CanIncreaseCapacity(size_class);
    } else {
      return cache_[size_class].tc.CanIncreaseCapacity(size_class);
    }
  }

  // We try to grow up to 10% of the total number of size classes during one
  // resize interval.
  static constexpr double kFractionClassesToResize = 0.1;
  static constexpr int kMaxSizeClassesToResize = std::max<int>(
      static_cast<int>(kNumClasses * kFractionClassesToResize), 1);

  // Tries to resize transfer caches based on the number of misses that they
  // incurred during the previous resize interval.
  void TryResizingCaches() {
    internal_transfer_cache::TryResizingCaches(*this);
  }

  static TransferCacheImplementation ChooseImplementation();

  void InitCaches() ABSL_EXCLUSIVE_LOCKS_REQUIRED(pageheap_lock) {
    for (int i = 0; i < kNumClasses; ++i) {
      if (implementation_ == TransferCacheImplementation::Ring) {
        new (&cache_[i].rbtc) RingBufferTransferCache(this, i);
      } else {
        new (&cache_[i].tc) TransferCache(this, i);
      }
    }
  }

  bool ShrinkCache(int size_class) {
    if (implementation_ == TransferCacheImplementation::Ring) {
      return cache_[size_class].rbtc.ShrinkCache(size_class);
    } else {
      return cache_[size_class].tc.ShrinkCache(size_class);
    }
  }

  bool IncreaseCacheCapacity(int size_class) {
    if (implementation_ == TransferCacheImplementation::Ring) {
      return cache_[size_class].rbtc.IncreaseCacheCapacity(size_class);
    } else {
      return cache_[size_class].tc.IncreaseCacheCapacity(size_class);
    }
  }

  size_t FetchCommitIntervalMisses(int size_class) {
    if (implementation_ == TransferCacheImplementation::Ring) {
      return cache_[size_class].rbtc.FetchCommitIntervalMisses();
    } else {
      return cache_[size_class].tc.FetchCommitIntervalMisses();
    }
  }

 private:
  TransferCacheImplementation implementation_ =
      TransferCacheImplementation::Legacy;
  union Cache {
    constexpr Cache() : dummy(false) {}
    ~Cache() {}

    TransferCache tc;
    RingBufferTransferCache rbtc;
    bool dummy;
  };
  Cache cache_[kNumClasses];
} ABSL_CACHELINE_ALIGNED;

#else

// For the small memory model, the transfer cache is not used.
class TransferCacheManager {
 public:
  constexpr TransferCacheManager() : freelist_() {}
  TransferCacheManager(const TransferCacheManager&) = delete;
  TransferCacheManager& operator=(const TransferCacheManager&) = delete;

  void Init() ABSL_EXCLUSIVE_LOCKS_REQUIRED(pageheap_lock) {
    for (int i = 0; i < kNumClasses; ++i) {
      freelist_[i].Init(i);
    }
  }

  void InsertRange(int size_class, absl::Span<void*> batch) {
    freelist_[size_class].InsertRange(batch);
  }

  ABSL_MUST_USE_RESULT int RemoveRange(int size_class, void** batch, int n) {
    return freelist_[size_class].RemoveRange(batch, n);
  }

  static constexpr size_t tc_length(int size_class) { return 0; }

  static constexpr TransferCacheStats GetStats(int size_class) { return {}; }

  const CentralFreeList& central_freelist(int size_class) const {
    return freelist_[size_class];
  }

  CentralFreeList& central_freelist(int size_class) {
    return freelist_[size_class];
  }

  TransferCacheImplementation implementation() const {
    return TransferCacheImplementation::None;
  }

 private:
  CentralFreeList freelist_[kNumClasses];
} ABSL_CACHELINE_ALIGNED;

// A trivial no-op implementation.
struct ShardedTransferCacheManager {
  constexpr ShardedTransferCacheManager(std::nullptr_t, std::nullptr_t) {}
  static constexpr void Init() {}
  static constexpr bool should_use(int size_class) { return false; }
  static constexpr void* Pop(int size_class) { return nullptr; }
  static constexpr void Push(int size_class, void* ptr) {}
  static constexpr int RemoveRange(int size_class, void** batch, int n) {
    return 0;
  }
  static constexpr void InsertRange(int size_class, absl::Span<void*> batch) {}
  static constexpr size_t TotalBytes() { return 0; }
  static constexpr void Plunder() {}
  static int tc_length(int cpu, int size_class) { return 0; }
  static constexpr TransferCacheStats GetStats(int size_class) { return {}; }
  bool UseGenericCache() const { return false; }
  bool UseCacheForLargeClassesOnly() const { return false; }
  int NumActiveShards() const { return 0; }
};

#endif

}  // namespace tcmalloc_internal
}  // namespace tcmalloc
GOOGLE_MALLOC_SECTION_END

#endif  // TCMALLOC_TRANSFER_CACHE_H_
