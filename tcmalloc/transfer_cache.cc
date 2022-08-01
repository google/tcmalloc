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

#include "tcmalloc/transfer_cache.h"

#include <fcntl.h>
#include <string.h>

#include <atomic>
#include <cstdint>
#include <optional>

#include "absl/base/attributes.h"
#include "absl/base/optimization.h"
#include "tcmalloc/common.h"
#include "tcmalloc/experiment.h"
#include "tcmalloc/guarded_page_allocator.h"
#include "tcmalloc/internal/cache_topology.h"
#include "tcmalloc/internal/environment.h"
#include "tcmalloc/internal/linked_list.h"
#include "tcmalloc/internal/logging.h"
#include "tcmalloc/internal/optimization.h"
#include "tcmalloc/internal/util.h"
#include "tcmalloc/static_vars.h"

GOOGLE_MALLOC_SECTION_BEGIN
namespace tcmalloc {
namespace tcmalloc_internal {

absl::string_view TransferCacheImplementationToLabel(
    TransferCacheImplementation type) {
  switch (type) {
    case TransferCacheImplementation::Legacy:
      return "LEGACY";
    case TransferCacheImplementation::None:
      return "NO_TRANSFERCACHE";
    case TransferCacheImplementation::Ring:
      return "RING";
    default:
      ASSUME(false);
  }
}

#ifndef TCMALLOC_SMALL_BUT_SLOW

size_t StaticForwarder::class_to_size(int size_class) {
  return Static::sizemap().class_to_size(size_class);
}
size_t StaticForwarder::num_objects_to_move(int size_class) {
  return Static::sizemap().num_objects_to_move(size_class);
}
void *StaticForwarder::Alloc(size_t size, int alignment) {
  return Static::arena().Alloc(size, alignment);
}

void BackingTransferCache::InsertRange(absl::Span<void *> batch) const {
  Static::transfer_cache().InsertRange(size_class_, batch);
}

ABSL_MUST_USE_RESULT int BackingTransferCache::RemoveRange(void **batch,
                                                           int n) const {
  return Static::transfer_cache().RemoveRange(size_class_, batch, n);
}

TransferCacheImplementation TransferCacheManager::ChooseImplementation() {
  // Prefer ring, if we're forcing it on.
  if (IsExperimentActive(
          Experiment::TEST_ONLY_TCMALLOC_RING_BUFFER_TRANSFER_CACHE)) {
    return TransferCacheImplementation::Ring;
  }

  // Consider opt-outs
  const char *e = thread_safe_getenv("TCMALLOC_INTERNAL_TRANSFERCACHE_CONTROL");
  if (e) {
    if (e[0] == '0') {
      return TransferCacheImplementation::Legacy;
    }
    if (e[0] == '1') {
      return TransferCacheImplementation::Ring;
    }
    Crash(kCrash, __FILE__, __LINE__, "bad env var", e);
  }

  // Otherwise, default to legacy.
  return TransferCacheImplementation::Legacy;
}

int TransferCacheManager::DetermineSizeClassToEvict(int current_size_class) {
  int t = next_to_evict_.load(std::memory_order_relaxed);
  if (t >= kNumClasses) t = 1;
  next_to_evict_.store(t + 1, std::memory_order_relaxed);

  // Ask nicely first.
  if (implementation_ == TransferCacheImplementation::Ring) {
    // HasSpareCapacity may take lock_, but HasSpareCapacity(t) will fail if
    // we're already evicting from t so we can avoid consulting the lock in
    // that cases.
    if (ABSL_PREDICT_FALSE(t == current_size_class) ||
        cache_[t].rbtc.HasSpareCapacity(t))
      return t;
  } else {
    if (cache_[t].tc.HasSpareCapacity(t)) return t;
  }

  // But insist on the second try.
  t = next_to_evict_.load(std::memory_order_relaxed);
  if (t >= kNumClasses) t = 1;
  next_to_evict_.store(t + 1, std::memory_order_relaxed);
  return t;
}

// Tracks misses per size class.
struct MissInfo {
  int size_class;
  uint64_t misses;
};

void TransferCacheManager::TryResizingCaches() {
  absl::FixedArray<MissInfo> misses(kNumClasses);

  // Collect misses for all the size classes that were incurred during the
  // previous resize interval.
  for (int size_class = 0; size_class < kNumClasses; ++size_class) {
    size_t miss = GetIntervalMisses(size_class, MissType::kResize);
    misses[size_class] = {.size_class = size_class, .misses = miss};
  }

  std::sort(misses.begin(), misses.end(),
            [](const MissInfo &a, const MissInfo &b) {
              if (a.misses == b.misses) {
                return a.size_class < b.size_class;
              }
              return a.misses > b.misses;
            });

  // Prioritize shrinking cache that had least number of misses.
  int to_grow = 0;
  int to_shrink = kNumClasses - 1;
  int total_grown = 0;

  // Grow up to kMaxSizeClassesToResize caches and make sure that we never
  // shrink a cache that we are supposed to grow during this interval.
  while (total_grown < kMaxSizeClassesToResize && to_grow < to_shrink) {
    // As the list is sorted, we won't encounter any size class with a non-zero
    // miss. So, it is ok to break.
    if (misses[to_grow].misses == 0) break;

    int class_to_grow = misses[to_grow].size_class;
    // If this cache is already at its maximum capacity, continue to the next
    // cache.
    if (!CanIncreaseCapacity(class_to_grow)) {
      ++to_grow;
      continue;
    }

    bool made_space = false;
    while (to_grow < to_shrink) {
      const int to_evict = misses[to_shrink].size_class;
      made_space = ShrinkCache(to_evict);
      --to_shrink;
      if (made_space) {
        break;
      }
    }

    if (made_space) {
      IncreaseCacheCapacity(class_to_grow);
      ++to_grow;
      ++total_grown;
    }
  }

  // Finally, take a snapshot of misses at the end of this interval. We would
  // use this during the next resize operation to calculate the number of misses
  // incurred over the interval.
  for (int size_class = 0; size_class < kNumClasses; ++size_class) {
    UpdateResizeIntervalMisses(size_class, MissType::kResize);
  }
}

#endif

}  // namespace tcmalloc_internal
}  // namespace tcmalloc
GOOGLE_MALLOC_SECTION_END
