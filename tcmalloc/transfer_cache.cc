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

#include <algorithm>
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
  // Return if resizing caches in background is disabled.
  if (!ResizeCachesInBackground()) return;

  // We try to grow up to 10% of the total number of size classes during one
  // resize interval.
  constexpr double kFractionClassesToResize = 0.1;
  constexpr int kMaxSizeClassesToResize = std::max<int>(
      static_cast<int>(kNumClasses * kFractionClassesToResize), 1);
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
  int to_shrink = kNumClasses - 1;
  for (int i = 0; i < kMaxSizeClassesToResize; ++i) {
    int class_to_grow = misses[i].size_class;
    if (misses[i].misses == 0) break;
    // No point in shrinking the other cache if there is no space available in
    // the cache that we would like to grow.
    if (!CanIncreaseCapacity(class_to_grow)) continue;

    // Make sure we do not shrink the caches that we would eventually want to
    // grow during this interval.
    bool made_space = false;
    while (to_shrink >= kMaxSizeClassesToResize) {
      const int to_evict = misses[to_shrink].size_class;
      made_space = ShrinkCache(to_evict);
      --to_shrink;
      if (made_space) {
        break;
      }
    }

    if (made_space) {
      IncreaseCacheCapacity(class_to_grow);
    }
    if (to_shrink == kMaxSizeClassesToResize - 1) {
      break;
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
