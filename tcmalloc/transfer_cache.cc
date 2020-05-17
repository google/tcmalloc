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
  max_capacity_ = 0;
  SizeInfo info = {0, 0};

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
    max_capacity_ = 64 * objs_to_move;
    // A transfer cache freelist can have anywhere from 0 to
    // max_capacity_ slots to put link list chains into.
    info.capacity = 16 * objs_to_move;

    // Limit each size class cache to at most 1MB of objects or one entry,
    // whichever is greater. Total transfer cache memory used across all
    // size classes then can't be greater than approximately
    // 1MB * kMaxNumTransferEntries.
    max_capacity_ = std::clamp<size_t>(
        objs_to_move, max_capacity_,
        (1024 * 1024) / (bytes * objs_to_move) * objs_to_move);
    info.capacity = std::min(info.capacity, max_capacity_);
    slots_ = reinterpret_cast<void **>(
        Static::arena()->Alloc(max_capacity_ * sizeof(void *)));
  }
  SetSlotInfo(info);
}

bool TransferCache::EvictRandomSizeClass(int locked_size_class, bool force) {
  // t == 0 is never valid, hence starting at/reseting to 1
  static std::atomic<int32_t> race_counter{1};
  int t = race_counter.load(std::memory_order_relaxed);
  if (t >= kNumClasses) t = 1;
  race_counter.store(t + 1, std::memory_order_relaxed);
  ASSERT(0 < t && t < kNumClasses);
  if (t == locked_size_class) return false;
  return Static::transfer_cache()[t].ShrinkCache(locked_size_class, force);
}

bool TransferCache::MakeCacheSpace(int N) {
  auto info = slot_info_.load(std::memory_order_relaxed);
  // Is there room in the cache?
  if (info.used + N <= info.capacity) return true;
  // Check if we can expand this cache?
  if (info.capacity + N > max_capacity_) return false;
  // Ok, we'll try to grab an entry from some other size class.
  if (EvictRandomSizeClass(freelist_.size_class(), false) ||
      EvictRandomSizeClass(freelist_.size_class(), true)) {
    // Succeeded in evicting, we're going to make our cache larger.  However, we
    // may have dropped and re-acquired the lock in EvictRandomSizeClass (via
    // ShrinkCache), so the cache_size may have changed.  Therefore, check and
    // verify that it is still OK to increase the cache_size.
    info = slot_info_.load(std::memory_order_relaxed);
    if (info.capacity + N <= max_capacity_) {
      info.capacity += N;
      SetSlotInfo(info);
      return true;
    }
  }
  return false;
}

bool TransferCache::ShrinkCache(int locked_size_class, bool force) {
  auto info = slot_info_.load(std::memory_order_relaxed);

  // Start with a quick check without taking a lock.
  if (info.capacity == 0) return false;

  int N = Static::sizemap()->num_objects_to_move(freelist_.size_class());

  // We don't evict from a full cache unless we are 'forcing'.
  if (!force && info.used + N >= info.capacity) return false;

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
    info = slot_info_.load(std::memory_order_relaxed);

    if (info.capacity == 0) return false;
    if (!arbitrary_transfer_ && info.capacity < N) return false;

    N = std::min(N, info.capacity);
    int unused = info.capacity - info.used;
    if (N <= unused) {
      info.capacity -= N;
      SetSlotInfo(info);
      return true;
    }
    if (!force) return false;

    num_to_free = N - unused;
    info.capacity -= N;
    info.used -= num_to_free;
    SetSlotInfo(info);
    // Our internal slot array may get overwritten as soon as we drop the lock,
    // so copy the items to free to an on stack buffer.
    memcpy(to_free, GetSlot(info.used), sizeof(void *) * num_to_free);
  }
  // Access the freelist while holding *neither* lock.
  freelist_.InsertRange(to_free, num_to_free);
  return true;
}

void TransferCache::InsertRange(absl::Span<void *> batch, int N) {
  const int B = Static::sizemap()->num_objects_to_move(freelist_.size_class());
  ASSERT(0 < N && N <= B);
  auto info = slot_info_.load(std::memory_order_relaxed);
  if (N == B && info.used + N <= max_capacity_) {
    absl::base_internal::SpinLockHolder h(&lock_);
    if (MakeCacheSpace(N)) {
      // MakeCacheSpace can drop the lock, so refetch
      info = slot_info_.load(std::memory_order_relaxed);
      info.used += N;
      SetSlotInfo(info);

      void **entry = GetSlot(info.used - N);
      memcpy(entry, batch.data(), sizeof(void *) * N);
      tracking::Report(kTCInsertHit, freelist_.size_class(), 1);
      return;
    }
  } else if (arbitrary_transfer_) {
    absl::base_internal::SpinLockHolder h(&lock_);
    MakeCacheSpace(N);
    // MakeCacheSpace can drop the lock, so refetch
    info = slot_info_.load(std::memory_order_relaxed);
    int unused = info.capacity - info.used;
    if (N < unused) {
      info.used += N;
      SetSlotInfo(info);
      void **entry = GetSlot(info.used - N);
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
    if (N > 1 && extra > 0 && info.used > 0 && batch.size() >= B) {
      // Take at most all the objects present
      extra = std::min(extra, info.used);
      ASSERT(extra + N <= kMaxObjectsToMove);
      info.used -= extra;
      SetSlotInfo(info);

      void **entry = GetSlot(info.used);
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
  auto info = slot_info_.load(std::memory_order_relaxed);
  if (N == B && info.used >= N) {
    absl::base_internal::SpinLockHolder h(&lock_);
    // Refetch with the lock
    info = slot_info_.load(std::memory_order_relaxed);
    if (info.used >= N) {
      info.used -= N;
      SetSlotInfo(info);
      void **entry = GetSlot(info.used);
      memcpy(batch, entry, sizeof(void *) * N);
      tracking::Report(kTCRemoveHit, freelist_.size_class(), 1);
      return N;
    }
  } else if (arbitrary_transfer_ && info.used >= 0) {
    absl::base_internal::SpinLockHolder h(&lock_);
    // Refetch with the lock
    info = slot_info_.load(std::memory_order_relaxed);

    fetch = std::min(N, info.used);
    info.used -= fetch;
    SetSlotInfo(info);
    void **entry = GetSlot(info.used);
    memcpy(batch, entry, sizeof(void *) * fetch);
    tracking::Report(kTCRemoveHit, freelist_.size_class(), 1);
    if (fetch == N) return N;
  }
  tracking::Report(kTCRemoveMiss, freelist_.size_class(), 1);
  return freelist_.RemoveRange(batch + fetch, N - fetch) + fetch;
}

size_t TransferCache::tc_length() {
  return static_cast<size_t>(slot_info_.load(std::memory_order_relaxed).used);
}

#endif
}  // namespace tcmalloc
