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
#include <utility>

#include "absl/base/attributes.h"
#include "absl/base/const_init.h"
#include "absl/base/internal/spinlock.h"
#include "absl/base/macros.h"
#include "absl/base/thread_annotations.h"
#include "absl/types/span.h"
#include "tcmalloc/central_freelist.h"
#include "tcmalloc/common.h"
#include "tcmalloc/transfer_cache_stats.h"

#ifndef TCMALLOC_SMALL_BUT_SLOW
#include "tcmalloc/transfer_cache_internals.h"
#endif

GOOGLE_MALLOC_SECTION_BEGIN
namespace tcmalloc {
namespace tcmalloc_internal {

#ifndef TCMALLOC_SMALL_BUT_SLOW

class TransferCacheManager {
  template <typename CentralFreeList, typename Manager>
  friend class internal_transfer_cache::TransferCache;
  using TransferCache =
      internal_transfer_cache::TransferCache<tcmalloc_internal::CentralFreeList,
                                             TransferCacheManager>;

 public:
  constexpr TransferCacheManager() : next_to_evict_(1) {}

  TransferCacheManager(const TransferCacheManager &) = delete;
  TransferCacheManager &operator=(const TransferCacheManager &) = delete;

  void Init() ABSL_EXCLUSIVE_LOCKS_REQUIRED(pageheap_lock) {
    for (int i = 0; i < kNumClasses; ++i) {
      auto *c = &cache_[i].tc;
      new (c) TransferCache(this, i);
      c->Init(i);
    }
  }

  void InsertRange(int size_class, absl::Span<void *> batch) {
    cache_[size_class].tc.InsertRange(batch);
  }

  ABSL_MUST_USE_RESULT int RemoveRange(int size_class, void **batch, int n) {
    return cache_[size_class].tc.RemoveRange(batch, n);
  }

  size_t central_length(int size_class) const {
    return cache_[size_class].tc.central_length();
  }

  size_t tc_length(int size_class) const {
    return cache_[size_class].tc.tc_length();
  }

  size_t OverheadBytes(int size_class) const {
    return cache_[size_class].tc.OverheadBytes();
  }

  SpanStats GetSpanStats(int size_class) const {
    return cache_[size_class].tc.GetSpanStats();
  }

  TransferCacheStats GetHitRateStats(int size_class) {
    return cache_[size_class].tc.GetHitRateStats();
  }

 private:
  static size_t class_to_size(int size_class);
  static size_t num_objects_to_move(int size_class);
  void *Alloc(size_t size) ABSL_EXCLUSIVE_LOCKS_REQUIRED(pageheap_lock);
  int DetermineSizeClassToEvict();
  bool ShrinkCache(int size_class) {
    return cache_[size_class].tc.ShrinkCache();
  }
  bool GrowCache(int size_class) { return cache_[size_class].tc.GrowCache(); }

  std::atomic<int32_t> next_to_evict_;
  union Cache {
    constexpr Cache() : dummy(false) {}
    ~Cache() {}

    TransferCache tc;
    bool dummy;
  };
  Cache cache_[kNumClasses];
} ABSL_CACHELINE_ALIGNED;

#else

// For the small memory model, the transfer cache is not used.
class TransferCacheManager {
 public:
  constexpr TransferCacheManager() : freelist_() {}
  TransferCacheManager(const TransferCacheManager &) = delete;
  TransferCacheManager &operator=(const TransferCacheManager &) = delete;

  void Init() ABSL_EXCLUSIVE_LOCKS_REQUIRED(pageheap_lock) {
    for (int i = 0; i < kNumClasses; ++i) {
      freelist_[i].Init(i);
    }
  }

  void InsertRange(int size_class, absl::Span<void *> batch) {
    freelist_[size_class].InsertRange(batch);
  }

  ABSL_MUST_USE_RESULT int RemoveRange(int size_class, void **batch, int n) {
    return freelist_[size_class].RemoveRange(batch, n);
  }

  size_t central_length(int size_class) const {
    return freelist_[size_class].length();
  }

  static constexpr size_t tc_length(int size_class) { return 0; }

  size_t OverheadBytes(int size_class) {
    return freelist_[size_class].OverheadBytes();
  }

  SpanStats GetSpanStats(int size_class) const {
    return freelist_[size_class].GetSpanStats();
  }

  TransferCacheStats GetHitRateStats(int size_class) const {
    TransferCacheStats stats;
    stats.insert_hits = 0;
    stats.insert_misses = 0;
    stats.remove_hits = 0;
    stats.remove_misses = 0;
    return stats;
  }

 private:
  CentralFreeList freelist_[kNumClasses];
} ABSL_CACHELINE_ALIGNED;

#endif

}  // namespace tcmalloc_internal
}  // namespace tcmalloc
GOOGLE_MALLOC_SECTION_END

#endif  // TCMALLOC_TRANSFER_CACHE_H_
