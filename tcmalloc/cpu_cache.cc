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

#include "tcmalloc/cpu_cache.h"

#include <stdlib.h>
#include <string.h>

#include <algorithm>
#include <atomic>

#include "absl/base/dynamic_annotations.h"
#include "absl/base/internal/spinlock.h"
#include "absl/base/internal/sysinfo.h"
#include "absl/base/macros.h"
#include "absl/base/thread_annotations.h"
#include "tcmalloc/arena.h"
#include "tcmalloc/common.h"
#include "tcmalloc/internal/logging.h"
#include "tcmalloc/internal_malloc_extension.h"
#include "tcmalloc/parameters.h"
#include "tcmalloc/static_vars.h"
#include "tcmalloc/transfer_cache.h"

namespace tcmalloc {

using subtle::percpu::GetCurrentVirtualCpuUnsafe;

static cpu_set_t FillActiveCpuMask() {
  cpu_set_t allowed_cpus;
  if (sched_getaffinity(0, sizeof(allowed_cpus), &allowed_cpus) != 0) {
    CPU_ZERO(&allowed_cpus);
  }

#ifdef PERCPU_USE_RSEQ
  const bool real_cpus = !tcmalloc::subtle::percpu::UsingFlatVirtualCpus();
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
static size_t MaxCapacity(size_t cl) {
  // The number of size classes that are commonly used and thus should be
  // allocated more slots in the per-cpu cache.
  static constexpr size_t kNumSmall = 10;
  // The remaining size classes, excluding size class 0.
  static constexpr size_t kNumLarge = kNumClasses - 1 - kNumSmall;
  // The memory used for each per-CPU slab is the sum of:
  //   sizeof(std::atomic<size_t>) * kNumClasses
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
  static const size_t kSmallObjectDepth = 16;
  static const size_t kLargeObjectDepth = 6;
#else
  // We allocate 256KiB per-cpu for pointers to cached per-cpu memory.
  // Each 256KiB is a subtle::percpu::TcmallocSlab::Slabs
  // Max(kNumClasses) is 89, so the maximum footprint per CPU is:
  //   89 * 8 + 8 * ((2048 + 1) * 10 + (152 + 1) * 78 + 88) = 254 KiB
  static const size_t kSmallObjectDepth = 2048;
  static const size_t kLargeObjectDepth = 152;
#endif
  static_assert(sizeof(std::atomic<size_t>) * kNumClasses +
                        sizeof(void *) * (kSmallObjectDepth + 1) * kNumSmall +
                        sizeof(void *) * (kLargeObjectDepth + 1) * kNumLarge <=
                    (1 << CPUCache::kPerCpuShift),
                "per-CPU memory exceeded");
  if (cl == 0 || cl >= kNumClasses) return 0;
  if (cl <= kNumSmall) {
    // Small object sizes are very heavily used and need very deep caches for
    // good performance (well over 90% of malloc calls are for cl <= 10.)
    return kSmallObjectDepth;
  }

  return kLargeObjectDepth;
}

static void *SlabAlloc(size_t size)
    ABSL_EXCLUSIVE_LOCKS_REQUIRED(pageheap_lock) {
  return Static::arena().Alloc(size);
}

void CPUCache::Activate(ActivationMode mode) {
  ASSERT(Static::IsInited());
  int num_cpus = absl::base_internal::NumCPUs();

  absl::base_internal::SpinLockHolder h(&pageheap_lock);

  resize_ = reinterpret_cast<ResizeInfo *>(
      Static::arena().Alloc(sizeof(ResizeInfo) * num_cpus));
  lazy_slabs_ = Parameters::lazy_per_cpu_caches();

  auto max_cache_size = Parameters::max_per_cpu_cache_size();

  for (int cpu = 0; cpu < num_cpus; ++cpu) {
    for (int cl = 1; cl < kNumClasses; ++cl) {
      resize_[cpu].per_class[cl].Init();
    }
    resize_[cpu].available.store(max_cache_size, std::memory_order_relaxed);
    resize_[cpu].last_steal.store(1, std::memory_order_relaxed);
  }

  freelist_.Init(SlabAlloc, MaxCapacity, lazy_slabs_);
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
void *CPUCache::Refill(int cpu, size_t cl) {
  const size_t batch_length = Static::sizemap().num_objects_to_move(cl);

  // UpdateCapacity can evict objects from other size classes as it tries to
  // increase capacity of this size class. The objects are returned in
  // to_return, we insert them into transfer cache at the end of function
  // (to increase possibility that we stay on the current CPU as we are
  // refilling the list).
  size_t returned = 0;
  ObjectClass to_return[kNumClasses];
  const size_t target =
      UpdateCapacity(cpu, cl, batch_length, false, to_return, &returned);

  // Refill target objects in batch_length batches.
  size_t total = 0;
  size_t got;
  size_t i;
  void *result = nullptr;
  void *batch[kMaxObjectsToMove];
  do {
    const size_t want = std::min(batch_length, target - total);
    got = Static::transfer_cache().RemoveRange(cl, batch, want);
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
        Static::transfer_cache().InsertRange(cl, absl::Span<void *>(batch), i);
      }
    }
  } while (got == batch_length && i == 0 && total < target &&
           cpu == GetCurrentVirtualCpuUnsafe());

  for (size_t i = 0; i < returned; ++i) {
    ObjectClass *ret = &to_return[i];
    Static::transfer_cache().InsertRange(ret->cl,
                                         absl::Span<void *>(&ret->obj, 1), 1);
  }

  return result;
}

size_t CPUCache::UpdateCapacity(int cpu, size_t cl, size_t batch_length,
                                bool overflow, ObjectClass *to_return,
                                size_t *returned) {
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

  const size_t max_capacity = MaxCapacity(cl);
  size_t capacity = freelist_.Capacity(cpu, cl);
  // We assert that the return value, target, is non-zero, so starting from an
  // initial capacity of zero means we may be populating this core for the
  // first time.
  absl::base_internal::LowLevelCallOnce(
      &resize_[cpu].initialized,
      [](CPUCache *cache, int cpu) {
        if (cache->lazy_slabs_) {
          absl::base_internal::SpinLockHolder h(&cache->resize_[cpu].lock);
          cache->freelist_.InitCPU(cpu, MaxCapacity);
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
    Grow(cpu, cl, increase, to_return, returned);
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

void CPUCache::Grow(int cpu, size_t cl, size_t desired_increase,
                    ObjectClass *to_return, size_t *returned) {
  const size_t size = Static::sizemap().class_to_size(cl);
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
    acquired_bytes +=
        Steal(cpu, cl, desired_bytes - acquired_bytes, to_return, returned);
  }

  // We have all the memory we could reserve.  Time to actually do the growth.

  // We might have gotten more than we wanted (stealing from larger sizeclasses)
  // so don't grow _too_ much.
  size_t actual_increase = acquired_bytes / size;
  actual_increase = std::min(actual_increase, desired_increase);
  // Remember, Grow may not give us all we ask for.
  size_t increase = freelist_.Grow(cpu, cl, actual_increase, MaxCapacity(cl));
  size_t increased_bytes = increase * size;
  if (increased_bytes < acquired_bytes) {
    // return whatever we didn't use to the slack.
    size_t unused = acquired_bytes - increased_bytes;
    resize_[cpu].available.fetch_add(unused, std::memory_order_relaxed);
  }
}

// There are rather a lot of policy knobs we could tweak here.
size_t CPUCache::Steal(int cpu, size_t dest_cl, size_t bytes,
                       ObjectClass *to_return, size_t *returned) {
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
    const size_t batch_length =
        Static::sizemap().num_objects_to_move(source_cl);
    size_t size = Static::sizemap().class_to_size(source_cl);

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
      void *obj = freelist_.Pop(source_cl, NoopUnderflow);
      if (obj) {
        ObjectClass *ret = &to_return[*returned];
        ++(*returned);
        ret->cl = source_cl;
        ret->obj = obj;
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

    if (cpu != GetCurrentVirtualCpuUnsafe() || acquired >= bytes) {
      // can't steal any more or don't need to
      break;
    }
  }
  // update the hint
  resize_[cpu].last_steal.store(source_cl, std::memory_order_relaxed);
  return acquired;
}

int CPUCache::Overflow(void *ptr, size_t cl, int cpu) {
  const size_t batch_length = Static::sizemap().num_objects_to_move(cl);
  const size_t target =
      UpdateCapacity(cpu, cl, batch_length, true, nullptr, nullptr);
  // Return target objects in batch_length batches.
  size_t total = 0;
  size_t count = 1;
  void *batch[kMaxObjectsToMove];
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
    Static::transfer_cache().InsertRange(cl, absl::Span<void *>(batch), count);
    if (count != batch_length) break;
    count = 0;
  } while (total < target && cpu == GetCurrentVirtualCpuUnsafe());
  tracking::Report(kFreeTruncations, cl, 1);
  return 1;
}

uint64_t CPUCache::UsedBytes(int target_cpu) const {
  ASSERT(target_cpu >= 0);
  if (!HasPopulated(target_cpu)) {
    return 0;
  }

  uint64_t total = 0;
  for (int cl = 1; cl < kNumClasses; cl++) {
    int size = Static::sizemap().class_to_size(cl);
    total += size * freelist_.Length(target_cpu, cl);
  }
  return total;
}

bool CPUCache::HasPopulated(int target_cpu) const {
  ASSERT(target_cpu >= 0);
  return resize_[target_cpu].populated.load(std::memory_order_relaxed);
}

PerCPUMetadataState CPUCache::MetadataMemoryUsage() const {
  return freelist_.MetadataMemoryUsage();
}

uint64_t CPUCache::TotalUsedBytes() const {
  uint64_t total = 0;
  for (int cpu = 0, num_cpus = absl::base_internal::NumCPUs(); cpu < num_cpus;
       ++cpu) {
    total += UsedBytes(cpu);
  }
  return total;
}

uint64_t CPUCache::TotalObjectsOfClass(size_t cl) const {
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

uint64_t CPUCache::Unallocated(int cpu) const {
  return resize_[cpu].available.load(std::memory_order_relaxed);
}

uint64_t CPUCache::CacheLimit() const {
  return Parameters::max_per_cpu_cache_size();
}

struct DrainContext {
  std::atomic<size_t> *available;
  uint64_t bytes;
};

static void DrainHandler(void *arg, size_t cl, void **batch, size_t count,
                         size_t cap) {
  DrainContext *ctx = static_cast<DrainContext *>(arg);
  const size_t size = Static::sizemap().class_to_size(cl);
  const size_t batch_length = Static::sizemap().num_objects_to_move(cl);
  ctx->bytes += count * size;
  // Drain resets capacity to 0, so return the allocated capacity to that
  // CPU's slack.
  ctx->available->fetch_add(cap * size, std::memory_order_relaxed);
  for (size_t i = 0; i < count; i += batch_length) {
    size_t n = std::min(batch_length, count - i);
    Static::transfer_cache().InsertRange(cl, absl::Span<void *>(batch + i, n),
                                         n);
  }
}

uint64_t CPUCache::Reclaim(int cpu) {
  absl::base_internal::SpinLockHolder h(&resize_[cpu].lock);

  // If we haven't populated this core, freelist_.Drain() will touch the memory
  // (for writing) as part of its locking process.  Avoid faulting new pages as
  // part of a release process.
  if (!resize_[cpu].populated.load(std::memory_order_relaxed)) {
    return 0;
  }

  DrainContext ctx{&resize_[cpu].available, 0};
  freelist_.Drain(cpu, &ctx, DrainHandler);
  return ctx.bytes;
}

void CPUCache::Print(TCMalloc_Printer *out) const {
  out->printf("------------------------------------------------\n");
  out->printf("Bytes in per-CPU caches (per cpu limit: %" PRIu64 " bytes)\n",
              Static::cpu_cache().CacheLimit());
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
}

void CPUCache::PrintInPbtxt(PbtxtRegion *region) const {
  const cpu_set_t allowed_cpus = FillActiveCpuMask();

  for (int cpu = 0, num_cpus = absl::base_internal::NumCPUs(); cpu < num_cpus;
       ++cpu) {
    PbtxtRegion entry = region->CreateSubRegion("cpu_cache");
    uint64_t rbytes = UsedBytes(cpu);
    bool populated = HasPopulated(cpu);
    uint64_t unallocated = Unallocated(cpu);
    entry.PrintI64("cpu", uint64_t(cpu));
    entry.PrintI64("used", rbytes);
    entry.PrintI64("unused", unallocated);
    entry.PrintBool("active", CPU_ISSET(cpu, &allowed_cpus));
    entry.PrintBool("populated", populated);
  }
}

void CPUCache::PerClassResizeInfo::Init() {
  state_.store(0, std::memory_order_relaxed);
}

bool CPUCache::PerClassResizeInfo::Update(bool overflow, bool grow,
                                          uint32_t *successive) {
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

uint32_t CPUCache::PerClassResizeInfo::Tick() {
  int32_t raw = state_.load(std::memory_order_relaxed);
  State state;
  memcpy(&state, &raw, sizeof(state));
  state.quiescent_ticks++;
  memcpy(&raw, &state, sizeof(raw));
  state_.store(raw, std::memory_order_relaxed);
  return state.quiescent_ticks - 1;
}

static void ActivatePerCPUCaches() {
  // RunningOnValgrind is a proxy for "is something intercepting malloc."
  //
  // If Valgrind, et. al., are in use, TCMalloc isn't in use and we shouldn't
  // activate our per-CPU caches.
  if (RunningOnValgrind()) {
    return;
  }
  if (Parameters::per_cpu_caches() && subtle::percpu::IsFast()) {
    Static::InitIfNecessary();
    Static::cpu_cache().Activate(CPUCache::ActivationMode::FastPathOn);
    // no need for this thread cache anymore, I guess.
    ThreadCache::BecomeIdle();
    // If there's a problem with this code, let's notice it right away:
    ::operator delete(::operator new(1));
  }
}

class PerCPUInitializer {
 public:
  PerCPUInitializer() { ActivatePerCPUCaches(); }
};
static PerCPUInitializer module_enter_exit;

}  // namespace tcmalloc

extern "C" bool MallocExtension_Internal_GetPerCpuCachesActive() {
  return tcmalloc::Static::CPUCacheActive();
}

extern "C" int32_t MallocExtension_Internal_GetMaxPerCpuCacheSize() {
  return tcmalloc::Parameters::max_per_cpu_cache_size();
}

extern "C" void MallocExtension_Internal_SetMaxPerCpuCacheSize(int32_t value) {
  tcmalloc::Parameters::set_max_per_cpu_cache_size(value);
}
