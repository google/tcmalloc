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

#include "absl/base/attributes.h"
#include "tcmalloc/common.h"
#include "tcmalloc/experiment.h"
#include "tcmalloc/guarded_page_allocator.h"
#include "tcmalloc/internal/linked_list.h"
#include "tcmalloc/internal/logging.h"
#include "tcmalloc/static_vars.h"
#include "tcmalloc/tracking.h"

namespace tcmalloc {
#ifndef TCMALLOC_SMALL_BUT_SLOW
class EvictionManager {
 public:
  constexpr EvictionManager() : next_(1) {}

  int DetermineSizeClassToEvict() {
    int t = next_.load(std::memory_order_relaxed);
    if (t >= kNumClasses) t = 1;
    next_.store(t + 1, std::memory_order_relaxed);

    // Ask nicely first.
    int N = Static::sizemap()->num_objects_to_move(t);
    auto info = Static::transfer_cache()[t].GetSlotInfo();
    if (info.capacity - info.used >= N) {
      return t;
    }

    // But insist on the second try.
    t = next_.load(std::memory_order_relaxed);
    if (t >= kNumClasses) t = 1;
    next_.store(t + 1, std::memory_order_relaxed);
    return t;
  }

 private:
  std::atomic<int32_t> next_;
};

ABSL_CONST_INIT EvictionManager gEvictionManager;

void TransferCache::Init(size_t cl) {
  // Init is guarded by the pageheap_lock, so no other threads can Init freelist
  // simultaneously.
  freelist().Init(cl);
  absl::base_internal::SpinLockHolder h(&lock_);

  // We need at least 2 slots to store list head and tail.
  ASSERT(kMinObjectsToMove >= 2);

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
    max_capacity_ = std::min<size_t>(
        max_capacity_,
        std::max<size_t>(objs_to_move, (1024 * 1024) / (bytes * objs_to_move) *
                                           objs_to_move));
    info.capacity = std::min(info.capacity, max_capacity_);
    slots_ = reinterpret_cast<void **>(
        Static::arena()->Alloc(max_capacity_ * sizeof(void *)));
  }
  SetSlotInfo(info);
}

bool TransferCache::MakeCacheSpace(int N) {
  auto info = slot_info_.load(std::memory_order_relaxed);
  // Is there room in the cache?
  if (info.used + N <= info.capacity) return true;
  // Check if we can expand this cache?
  if (info.capacity + N > max_capacity_) return false;

  int to_evict = gEvictionManager.DetermineSizeClassToEvict();
  if (to_evict == size_class()) return false;

  // Release the held lock before the other instance tries to grab its lock.
  lock_.Unlock();
  bool made_space = Static::transfer_cache()[to_evict].ShrinkCache();
  lock_.Lock();

  if (!made_space) return false;

  // Succeeded in evicting, we're going to make our cache larger.  However, we
  // may have dropped and re-acquired the lock, so the cache_size may have
  // changed.  Therefore, check and verify that it is still OK to increase the
  // cache_size.
  info = slot_info_.load(std::memory_order_relaxed);
  if (info.capacity + N > max_capacity_) return false;
  info.capacity += N;
  SetSlotInfo(info);
  return true;
}

bool TransferCache::ShrinkCache() {
  int N = Static::sizemap()->num_objects_to_move(size_class());

  void *to_free[kMaxObjectsToMove];
  int num_to_free;
  {
    absl::base_internal::SpinLockHolder h(&lock_);
    auto info = slot_info_.load(std::memory_order_relaxed);
    if (info.capacity == 0) return false;

    N = std::min(N, info.capacity);
    int unused = info.capacity - info.used;
    if (N <= unused) {
      info.capacity -= N;
      SetSlotInfo(info);
      return true;
    }

    num_to_free = N - unused;
    info.capacity -= N;
    info.used -= num_to_free;
    SetSlotInfo(info);

    // Our internal slot array may get overwritten as soon as we drop the lock,
    // so copy the items to free to an on stack buffer.
    memcpy(to_free, GetSlot(info.used), sizeof(void *) * num_to_free);
  }

  // Access the freelist without holding the lock.
  freelist().InsertRange(to_free, num_to_free);
  return true;
}

void TransferCache::InsertRange(absl::Span<void *> batch, int N) {
  const int B = Static::sizemap()->num_objects_to_move(size_class());
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
      tracking::Report(kTCInsertHit, size_class(), 1);
      return;
    }
  } else {
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
      tracking::Report(kTCInsertHit, size_class(), 1);
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
    // We don't need to hold the lock here, so release it earlier.
  }
  tracking::Report(kTCInsertMiss, size_class(), 1);
  freelist().InsertRange(batch.data(), N);
}

int TransferCache::RemoveRange(void **batch, int N) {
  ASSERT(N > 0);
  const int B = Static::sizemap()->num_objects_to_move(size_class());
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
      tracking::Report(kTCRemoveHit, size_class(), 1);
      return N;
    }
  } else if (info.used >= 0) {
    absl::base_internal::SpinLockHolder h(&lock_);
    // Refetch with the lock
    info = slot_info_.load(std::memory_order_relaxed);

    fetch = std::min(N, info.used);
    info.used -= fetch;
    SetSlotInfo(info);
    void **entry = GetSlot(info.used);
    memcpy(batch, entry, sizeof(void *) * fetch);
    tracking::Report(kTCRemoveHit, size_class(), 1);
    if (fetch == N) return N;
    // We don't need to hold the lock here, so release it earlier.
  }
  tracking::Report(kTCRemoveMiss, size_class(), 1);
  return freelist().RemoveRange(batch + fetch, N - fetch) + fetch;
}

size_t TransferCache::tc_length() {
  return static_cast<size_t>(slot_info_.load(std::memory_order_relaxed).used);
}

#endif
}  // namespace tcmalloc
