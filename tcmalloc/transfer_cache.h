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

#include <atomic>
#include <cstddef>
#include <limits>
#include <optional>
#include <utility>

#include "absl/base/attributes.h"
#include "absl/base/call_once.h"
#include "absl/base/const_init.h"
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
  static size_t class_to_size(int size_class);
  static size_t num_objects_to_move(int size_class);
  static void *Alloc(size_t size, int alignment = kAlignment)
      ABSL_EXCLUSIVE_LOCKS_REQUIRED(pageheap_lock);
};

// The NoStealingManager is set up so that stealing is disabled for this
// TransferCache.
class NoStealingManager : public StaticForwarder {
 public:
  static constexpr int DetermineSizeClassToEvict(int size_class) { return -1; }
  static constexpr bool ShrinkCache(int) { return false; }
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
    num_shards_ = CpuLayout::BuildCacheMap(l3_cache_index_);
    shards_ = reinterpret_cast<Shard *>(
        owner_->Alloc(sizeof(Shard) * num_shards_, ABSL_CACHELINE_SIZE));
    ASSERT(shards_ != nullptr);
    for (int shard = 0; shard < num_shards_; ++shard) {
      new (&shards_[shard]) Shard;
    }
    for (int size_class = 0; size_class < kNumClasses; ++size_class) {
      const int size_per_object = Manager::class_to_size(size_class);
      static constexpr int min_size = 4096;
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

  bool shard_initialized(int shard) {
    if (shards_ == nullptr) return false;
    return shards_[shard].initialized.load(std::memory_order_acquire);
  }

 private:
  using TransferCache =
      internal_transfer_cache::RingBufferTransferCache<FreeList, Manager>;

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

  // Initializes all transfer caches in the given shard.
  void InitShard(Shard &shard) ABSL_LOCKS_EXCLUDED(pageheap_lock) {
    absl::base_internal::SpinLockHolder h(&pageheap_lock);
    TransferCache *new_caches = reinterpret_cast<TransferCache *>(owner_->Alloc(
        sizeof(TransferCache) * kNumClasses, ABSL_CACHELINE_SIZE));
    ASSERT(new_caches != nullptr);
    for (int size_class = 0; size_class < kNumClasses; ++size_class) {
      const int size_per_object = Manager::class_to_size(size_class);
      static constexpr int k12MB = 12 << 20;
      const int capacity = should_use(size_class) ? k12MB / size_per_object : 0;
      new (&new_caches[size_class]) TransferCache(
          owner_, capacity > 0 ? size_class : 0, {capacity, capacity});
      new_caches[size_class].freelist().Init(size_class);
    }
    shard.transfer_caches = new_caches;
    shard.initialized.store(true, std::memory_order_release);
  }

  // Returns the cache shard corresponding to the given size class and the
  // current cpu's L3 node. The cache will be initialized if required.
  TransferCache &get_cache(int size_class) {
    const int cpu = cpu_layout_->CurrentCpu();
    ASSERT(cpu < 256);
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
  bool active_for_class_[kNumClasses] = {false};
  Manager *const owner_;
  CpuLayout *const cpu_layout_;
};

using ShardedTransferCacheManager =
    ShardedTransferCacheManagerBase<NoStealingManager, ProdCpuLayout,
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

 public:
  constexpr TransferCacheManager() : next_to_evict_(1) {}

  TransferCacheManager(const TransferCacheManager &) = delete;
  TransferCacheManager &operator=(const TransferCacheManager &) = delete;

  void Init() ABSL_EXCLUSIVE_LOCKS_REQUIRED(pageheap_lock) {
    implementation_ = ChooseImplementation();
    for (int i = 0; i < kNumClasses; ++i) {
      if (implementation_ == TransferCacheImplementation::Ring) {
        new (&cache_[i].rbtc) RingBufferTransferCache(this, i);
      } else {
        new (&cache_[i].tc) TransferCache(this, i);
      }
    }
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

  TransferCacheStats GetHitRateStats(int size_class) const {
    if (implementation_ == TransferCacheImplementation::Ring) {
      return cache_[size_class].rbtc.GetHitRateStats();
    } else {
      return cache_[size_class].tc.GetHitRateStats();
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

 private:
  static TransferCacheImplementation ChooseImplementation();

  int DetermineSizeClassToEvict(int size_class);
  bool ShrinkCache(int size_class) {
    if (implementation_ == TransferCacheImplementation::Ring) {
      return cache_[size_class].rbtc.ShrinkCache(size_class);
    } else {
      return cache_[size_class].tc.ShrinkCache(size_class);
    }
  }

  TransferCacheImplementation implementation_ =
      TransferCacheImplementation::Legacy;
  std::atomic<int32_t> next_to_evict_;
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

  static constexpr TransferCacheStats GetHitRateStats(int size_class) {
    return {0, 0, 0, 0};
  }

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
  static constexpr size_t TotalBytes() { return 0; }
  static constexpr void Plunder() {}
  static int tc_length(int cpu, int size_class) { return 0; }
};

#endif

}  // namespace tcmalloc_internal
}  // namespace tcmalloc
GOOGLE_MALLOC_SECTION_END

#endif  // TCMALLOC_TRANSFER_CACHE_H_
