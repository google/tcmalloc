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

#include <string.h>

#include <algorithm>
#include <atomic>

#include "tcmalloc/common.h"
#include "tcmalloc/experiment.h"
#include "tcmalloc/internal/linked_list.h"
#include "tcmalloc/internal/logging.h"
#include "tcmalloc/static_vars.h"
#include "tcmalloc/tracking.h"

namespace tcmalloc {
#ifndef TCMALLOC_SMALL_BUT_SLOW
void TransferCache::Init(size_t cl) {
  absl::base_internal::SpinLockHolder h(&lock_);
  freelist_.Init(cl);

  // We need at least 2 slots to store list head and tail.
  ASSERT(kMinObjectsToMove >= 2);

  // Cache this value, for performance.
  arbitrary_transfer_ =
      IsExperimentActive(Experiment::TCMALLOC_ARBITRARY_TRANSFER_CACHE);

  slots_ = nullptr;
  max_cache_slots_ = 0;
  int32_t cache_slots = 0;

  if (cl > 0) {
    // Limit the maximum size of the cache based on the size class.  If this
    // is not done, large size class objects will consume a lot of memory if
    // they just sit in the transfer cache.
    size_t bytes = Static::sizemap()->class_to_size(cl);
    size_t objs_to_move = Static::sizemap()->num_objects_to_move(cl);
    ASSERT(objs_to_move > 0 && bytes > 0);

    // Starting point for the maximum number of entries in the transfer cache.
    // This actual maximum for a given size class may be lower than this
    // maximum value.
    max_cache_slots_ = 64 * objs_to_move;
    // A transfer cache freelist can have anywhere from 0 to
    // max_cache_slots_ slots to put link list chains into.
    cache_slots = 16 * objs_to_move;

    // Limit each size class cache to at most 1MB of objects or one entry,
    // whichever is greater. Total transfer cache memory used across all
    // size classes then can't be greater than approximately
    // 1MB * kMaxNumTransferEntries.
    max_cache_slots_ = std::min<size_t>(
        max_cache_slots_,
        std::max<size_t>(objs_to_move, (1024 * 1024) / (bytes * objs_to_move) *
                                           objs_to_move));
    cache_slots = std::min(cache_slots, max_cache_slots_);
    slots_ = reinterpret_cast<void **>(
        Static::arena()->Alloc(max_cache_slots_ * sizeof(void *)));
  }
  used_slots_.store(0, std::memory_order_relaxed);
  cache_slots_.store(cache_slots, std::memory_order_relaxed);
  ASSERT(cache_slots <= max_cache_slots_);
}

bool TransferCache::EvictRandomSizeClass(int locked_size_class, bool force) {
  static std::atomic<int32_t> race_counter{1};
  int t = race_counter.load(std::memory_order_relaxed);
  race_counter.store(t + 1, std::memory_order_relaxed);
  if (t >= kNumClasses) {
    while (t >= kNumClasses) {
      t -= kNumClasses - 1;  // Don't want t == 0
    }
    race_counter.store(t, std::memory_order_relaxed);
  }
  ASSERT(t > 0);
  ASSERT(t < kNumClasses);
  if (t == locked_size_class) return false;
  return Static::transfer_cache()[t].ShrinkCache(locked_size_class, force);
}

bool TransferCache::MakeCacheSpace(int N) {
  int32_t used_slots = used_slots_.load(std::memory_order_relaxed);
  int32_t cache_slots = cache_slots_.load(std::memory_order_relaxed);
  // Is there room in the cache?
  if (used_slots + N <= cache_slots) return true;
  // Check if we can expand this cache?
  if (cache_slots + N > max_cache_slots_) return false;
  // Ok, we'll try to grab an entry from some other size class.
  if (EvictRandomSizeClass(freelist_.size_class(), false) ||
      EvictRandomSizeClass(freelist_.size_class(), true)) {
    // Succeeded in evicting, we're going to make our cache larger.  However, we
    // may have dropped and re-acquired the lock in EvictRandomSizeClass (via
    // ShrinkCache), so the cache_size may have changed.  Therefore, check and
    // verify that it is still OK to increase the cache_size.
    cache_slots = cache_slots_.load(std::memory_order_relaxed);
    if (cache_slots + N <= max_cache_slots_) {
      cache_slots += N;
      cache_slots_.store(cache_slots, std::memory_order_relaxed);
      return true;
    }
  }
  return false;
}

bool TransferCache::ShrinkCache(int locked_size_class, bool force) {
  int32_t used_slots = used_slots_.load(std::memory_order_relaxed);
  int32_t cache_slots = cache_slots_.load(std::memory_order_relaxed);

  // Start with a quick check without taking a lock.
  if (cache_slots == 0) return false;

  int N = Static::sizemap()->num_objects_to_move(freelist_.size_class());

  // We don't evict from a full cache unless we are 'forcing'.
  if (!force && used_slots + N >= cache_slots) return false;

  // Release the other held lock before acquiring the current lock to avoid a
  // dead lock.
  struct SpinLockReleaser {
    absl::base_internal::SpinLock *lock_;

    SpinLockReleaser(absl::base_internal::SpinLock *lock) : lock_(lock) {
      lock_->Unlock();
    }
    ~SpinLockReleaser() { lock_->Lock(); }
  };
  SpinLockReleaser unlocker(&Static::transfer_cache()[locked_size_class].lock_);
  void *to_free[kMaxObjectsToMove];
  int num_to_free;
  {
    absl::base_internal::SpinLockHolder h(&lock_);

    // Fetch while holding the lock in case they changed.
    cache_slots = cache_slots_.load(std::memory_order_relaxed);
    used_slots = used_slots_.load(std::memory_order_relaxed);
    ASSERT(0 <= used_slots && used_slots <= cache_slots);

    if (cache_slots == 0) return false;
    if (!arbitrary_transfer_ && cache_slots < N) return false;

    N = std::min(N, cache_slots);
    int unused = cache_slots - used_slots;
    if (N <= unused) {
      cache_slots -= N;
      cache_slots_.store(cache_slots, std::memory_order_relaxed);
      return true;
    }
    if (!force) return false;

    num_to_free = N - unused;
    cache_slots -= N;
    used_slots -= num_to_free;
    cache_slots_.store(cache_slots, std::memory_order_relaxed);
    used_slots_.store(used_slots, std::memory_order_relaxed);
    // Our internal slot array may get overwritten as soon as we drop the lock,
    // so copy the items to free to an on stack buffer.
    memcpy(to_free, GetSlot(used_slots), sizeof(void *) * num_to_free);
  }
  // Access the freelist while holding *neither* lock.
  freelist_.InsertRange(to_free, num_to_free);
  return true;
}

void TransferCache::InsertRange(absl::Span<void *> batch, int N) {
  const int B = Static::sizemap()->num_objects_to_move(freelist_.size_class());
  ASSERT(0 < N && N <= B);
  int32_t used_slots = used_slots_.load(std::memory_order_relaxed);
  if (N == B && used_slots + N <= max_cache_slots_) {
    absl::base_internal::SpinLockHolder h(&lock_);
    if (MakeCacheSpace(N)) {
      // MakeCacheSpace can drop the lock, so refetch
      used_slots = used_slots_.load(std::memory_order_relaxed);
      ASSERT(0 <= used_slots && used_slots + N <= max_cache_slots_);
      used_slots_.store(used_slots + N, std::memory_order_relaxed);

      void **entry = GetSlot(used_slots);
      memcpy(entry, batch.data(), sizeof(void *) * N);
      tracking::Report(kTCInsertHit, freelist_.size_class(), 1);
      return;
    }
  } else if (arbitrary_transfer_) {
    absl::base_internal::SpinLockHolder h(&lock_);
    MakeCacheSpace(N);
    // MakeCacheSpace can drop the lock, so refetch
    int32_t used_slots = used_slots_.load(std::memory_order_relaxed);
    int32_t cache_slots = cache_slots_.load(std::memory_order_relaxed);
    int unused = cache_slots - used_slots;
    if (N < unused) {
      used_slots_.store(used_slots + N, std::memory_order_relaxed);
      ASSERT(0 <= used_slots && used_slots + N <= max_cache_slots_);
      void **entry = GetSlot(used_slots);
      memcpy(entry, batch.data(), sizeof(void *) * N);
      tracking::Report(kTCInsertHit, freelist_.size_class(), 1);
      return;
    }
    // We could not fit the entire batch into the transfer cache
    // so send the batch to the freelist and also take some elements from
    // the transfer cache so that we amortise the cost of accessing spans
    // in the freelist. Only do this if caller has sufficient space in
    // batch.
    // First of all fill up the rest of the batch with elements from the
    // transfer cache.
    int extra = B - N;
    if (N > 1 && extra > 0 && used_slots > 0 && batch.size() >= B) {
      // Take at most all the objects present
      extra = std::min(extra, used_slots);
      ASSERT(extra + N <= kMaxObjectsToMove);
      used_slots -= extra;
      used_slots_.store(used_slots, std::memory_order_relaxed);

      void **entry = GetSlot(used_slots);
      memcpy(batch.data() + N, entry, sizeof(void *) * extra);
      N += extra;
#ifndef NDEBUG
      int rest = batch.size() - N - 1;
      if (rest > 0) {
        memset(batch.data() + N, 0x3f, rest * sizeof(void *));
      }
#endif
    }
  }
  tracking::Report(kTCInsertMiss, freelist_.size_class(), 1);
  freelist_.InsertRange(batch.data(), N);
}

int TransferCache::RemoveRange(void **batch, int N) {
  ASSERT(N > 0);
  const int B = Static::sizemap()->num_objects_to_move(freelist_.size_class());
  int fetch = 0;
  int32_t used_slots = used_slots_.load(std::memory_order_relaxed);
  if (N == B && used_slots >= N) {
    absl::base_internal::SpinLockHolder h(&lock_);
    // Refetch with the lock
    used_slots = used_slots_.load(std::memory_order_relaxed);
    if (used_slots >= N) {
      used_slots -= N;
      used_slots_.store(used_slots, std::memory_order_relaxed);
      ASSERT(0 <= used_slots);
      void **entry = GetSlot(used_slots);
      memcpy(batch, entry, sizeof(void *) * N);
      tracking::Report(kTCRemoveHit, freelist_.size_class(), 1);
      return N;
    }
  } else if (arbitrary_transfer_ && used_slots >= 0) {
    absl::base_internal::SpinLockHolder h(&lock_);
    // Refetch with the lock
    used_slots = used_slots_.load(std::memory_order_relaxed);

    fetch = std::min(N, used_slots);
    used_slots -= fetch;
    ASSERT(0 <= used_slots);
    used_slots_.store(used_slots, std::memory_order_relaxed);
    void **entry = GetSlot(used_slots);
    memcpy(batch, entry, sizeof(void *) * fetch);
    tracking::Report(kTCRemoveHit, freelist_.size_class(), 1);
    if (fetch == N) return N;
  }
  tracking::Report(kTCRemoveMiss, freelist_.size_class(), 1);
  return freelist_.RemoveRange(batch + fetch, N - fetch) + fetch;
}

size_t TransferCache::tc_length() {
  return static_cast<size_t>(used_slots_.load(std::memory_order_relaxed));
}

#endif
}  // namespace tcmalloc
