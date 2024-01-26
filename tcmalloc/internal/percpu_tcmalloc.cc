// Copyright 2024 The TCMalloc Authors
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

#include "tcmalloc/internal/percpu_tcmalloc.h"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <new>
#include <utility>

#include "absl/functional/function_ref.h"
#include "tcmalloc/internal/config.h"
#include "tcmalloc/internal/linux_syscall_support.h"
#include "tcmalloc/internal/logging.h"
#include "tcmalloc/internal/mincore.h"
#include "tcmalloc/internal/percpu.h"
#include "tcmalloc/internal/sysinfo.h"

GOOGLE_MALLOC_SECTION_BEGIN
namespace tcmalloc {
namespace tcmalloc_internal {
namespace subtle {
namespace percpu {

void TcmallocSlab::Init(
    size_t num_classes,
    absl::FunctionRef<void*(size_t, std::align_val_t)> alloc, void* slabs,
    absl::FunctionRef<size_t(size_t)> capacity, Shift shift) {
  ASSERT(num_classes_ == 0 && num_classes != 0);
  num_classes_ = num_classes;
  if (UsingFlatVirtualCpus()) {
    virtual_cpu_id_offset_ = offsetof(kernel_rseq, vcpu_id);
  }
  resize_begins_ =
      static_cast<uint16_t*>(alloc(sizeof(uint16_t) * num_classes_ * NumCPUs(),
                                   std::align_val_t{alignof(uint16_t)}));

#if TCMALLOC_INTERNAL_PERCPU_USE_RSEQ
  // This is needed only for tests that create/destroy slabs,
  // w/o this cpu_id_start may contain wrong offset for a new slab.
  __rseq_abi.cpu_id_start = 0;
#endif
  slabs_and_shift_.store({slabs, shift}, std::memory_order_relaxed);
  size_t consumed_bytes = num_classes_ * sizeof(Header);
  for (size_t size_class = 0; size_class < num_classes_; ++size_class) {
    size_t cap = capacity(size_class);
    CHECK_CONDITION(static_cast<uint16_t>(cap) == cap);

    if (cap == 0) {
      continue;
    }

    // One extra element for prefetch
    const size_t num_pointers = cap + 1;
    consumed_bytes += num_pointers * sizeof(void*);
    if (consumed_bytes > (1 << ToUint8(shift))) {
      Crash(kCrash, __FILE__, __LINE__, "per-CPU memory exceeded, have ",
            1 << ToUint8(shift), " need ", consumed_bytes, " size_class ",
            size_class);
    }
  }
}

void TcmallocSlab::InitCpu(int cpu,
                           absl::FunctionRef<size_t(size_t)> capacity) {
  const auto [slabs, shift] = GetSlabsAndShift(std::memory_order_relaxed);
  InitCpuImpl(slabs, shift, cpu, virtual_cpu_id_offset_, capacity);
}

void TcmallocSlab::InitCpuImpl(void* slabs, Shift shift, int cpu,
                               size_t virtual_cpu_id_offset,
                               absl::FunctionRef<size_t(size_t)> capacity) {
  // Phase 1: stop concurrent mutations for <cpu>. Locking ensures that there
  // exists no value of current such that begin < current.
  StopConcurrentMutations(slabs, shift, cpu, virtual_cpu_id_offset);

  // Phase 2: Initialize prefetch target and compute the offsets for the
  // boundaries of each size class' cache.
  void* curr_slab = CpuMemoryStart(slabs, shift, cpu);
  void** elems =
      reinterpret_cast<void**>(GetHeader(slabs, shift, cpu, num_classes_));
  uint16_t* begin = &resize_begins_[cpu * num_classes_];

  // Number of free pointers is limited by uint16_t sized offsets in slab
  // header, with an additional offset value 0xffff reserved for locking.
  constexpr size_t kMaxAllowedOffset = std::numeric_limits<uint16_t>::max() - 1;
  for (size_t size_class = 0; size_class < num_classes_; ++size_class) {
    size_t cap = capacity(size_class);
    CHECK_CONDITION(static_cast<uint16_t>(cap) == cap);

    if (cap) {
      // In Pop() we prefetch the item a subsequent Pop() would return; this is
      // slow if it's not a valid pointer. To avoid this problem when popping
      // the last item, keep one fake item before the actual ones (that points,
      // safely, to itself).
      *elems = elems;
      ++elems;
    }

    size_t offset = elems - reinterpret_cast<void**>(curr_slab);
    CHECK_CONDITION(static_cast<uint16_t>(offset) == offset);
    begin[size_class] = offset;

    elems += cap;
    const size_t bytes_used_on_curr_slab =
        reinterpret_cast<char*>(elems) - reinterpret_cast<char*>(curr_slab);
    if (bytes_used_on_curr_slab > (1 << ToUint8(shift))) {
      Crash(kCrash, __FILE__, __LINE__, "per-CPU memory exceeded, have ",
            1 << ToUint8(shift), " need ", bytes_used_on_curr_slab);
    }

    size_t max_end_offset = offset + cap;
    CHECK_CONDITION(static_cast<uint16_t>(max_end_offset) == max_end_offset);
    if (max_end_offset >= kMaxAllowedOffset) {
      Crash(kCrash, __FILE__, __LINE__, "per-CPU slab pointers exceeded, have ",
            kMaxAllowedOffset, " need at least", max_end_offset);
    }
  }

  // Phase 3: Store current.  No restartable sequence will proceed
  // (successfully) as !(begin < current) for all size classes.
  //
  // We must write current and complete a fence before storing begin and end
  // (b/147974701).
  for (size_t size_class = 0; size_class < num_classes_; ++size_class) {
    std::atomic<int64_t>* hdrp = GetHeader(slabs, shift, cpu, size_class);
    Header hdr = LoadHeader(hdrp);
    hdr.current = begin[size_class];
    StoreHeader(hdrp, hdr);
  }
  FenceCpu(cpu, virtual_cpu_id_offset);

  // Phase 4: Allow access to this cache.
  for (size_t size_class = 0; size_class < num_classes_; ++size_class) {
    Header hdr;
    hdr.current = begin[size_class];
    hdr.begin = begin[size_class];
    hdr.end = begin[size_class];
    hdr.end_copy = begin[size_class];
    StoreHeader(GetHeader(slabs, shift, cpu, size_class), hdr);
  }
}

#if TCMALLOC_INTERNAL_PERCPU_USE_RSEQ
std::pair<int, bool> TcmallocSlab::CacheCpuSlabSlow(int cpu) {
  for (;;) {
    intptr_t val = tcmalloc_slabs;
    ASSERT(!(val & TCMALLOC_CACHED_SLABS_MASK));
    const auto [slabs, shift] = GetSlabsAndShift(std::memory_order_relaxed);
    void* start = CpuMemoryStart(slabs, shift, cpu);
    intptr_t new_val =
        reinterpret_cast<uintptr_t>(start) | TCMALLOC_CACHED_SLABS_MASK;
    auto* ptr = reinterpret_cast<std::atomic<intptr_t>*>(
        const_cast<uintptr_t*>(&tcmalloc_slabs));
    int new_cpu =
        CompareAndSwapUnsafe(cpu, ptr, val, new_val, virtual_cpu_id_offset_);
    if (cpu == new_cpu) {
      break;
    }
    if (new_cpu >= 0) {
      cpu = new_cpu;
    }
  }
  // If ResizeSlabs is concurrently modifying slabs_and_shift_, we may
  // cache the offset with the shift that won't match slabs pointer used
  // by Push/Pop operations later. To avoid this, we check resizing_ after
  // the calculation. Coupled with setting of resizing_ and a Fence
  // in ResizeSlabs, this prevents possibility of mismatching shift/slabs.
  CompilerBarrier();
  if (resizing_.load(std::memory_order_relaxed)) {
    tcmalloc_slabs = 0;
    return {cpu, false};
  }
  return {cpu, true};
}
#endif

void TcmallocSlab::DrainCpu(void* slabs, Shift shift, int cpu,
                            DrainHandler drain_handler) {
  for (size_t size_class = 0; size_class < num_classes_; ++size_class) {
    Header header = LoadHeader(GetHeader(slabs, shift, cpu, size_class));
    uint16_t begin = resize_begins_[cpu * num_classes_ + size_class];
    const size_t size = header.current - begin;
    const size_t cap = header.end_copy - begin;
    void** batch =
        reinterpret_cast<void**>(GetHeader(slabs, shift, cpu, 0) + begin);
    TSANAcquireBatch(batch, size);
    drain_handler(cpu, size_class, batch, size, cap);
  }
}

void TcmallocSlab::StopConcurrentMutations(void* slabs, Shift shift, int cpu,
                                           size_t virtual_cpu_id_offset) {
  for (bool done = false; !done;) {
    for (size_t size_class = 0; size_class < num_classes_; ++size_class) {
      LockHeader(slabs, shift, cpu, size_class);
    }
    FenceCpu(cpu, virtual_cpu_id_offset);
    done = true;
    for (size_t size_class = 0; size_class < num_classes_; ++size_class) {
      Header hdr = LoadHeader(GetHeader(slabs, shift, cpu, size_class));
      if (!hdr.IsLocked()) {
        // Header was overwritten by Grow/Shrink. Retry.
        done = false;
        break;
      }
    }
  }
}

auto TcmallocSlab::ResizeSlabs(Shift new_shift, void* new_slabs,
                               absl::FunctionRef<size_t(size_t)> capacity,
                               absl::FunctionRef<bool(size_t)> populated,
                               DrainHandler drain_handler) -> ResizeSlabsInfo {
  // Phase 1: Initialize any cores in the new slab that have already been
  // populated in the old slab.
  const auto [old_slabs, old_shift] =
      GetSlabsAndShift(std::memory_order_relaxed);
  ASSERT(new_shift != old_shift);
  const size_t virtual_cpu_id_offset = virtual_cpu_id_offset_;
  const int num_cpus = NumCPUs();
  for (size_t cpu = 0; cpu < num_cpus; ++cpu) {
    if (populated(cpu)) {
      InitCpuImpl(new_slabs, new_shift, cpu, virtual_cpu_id_offset, capacity);
    }
  }

  // Phase 2: Collect all `begin`s (these are not mutated by anybody else thanks
  // to the cpu locks) and stop concurrent mutations for all populated CPUs and
  // size classes by locking all the headers.
  // Setting resizing_ in combination with a fence on every CPU before setting
  // new slabs_and_shift_ prevents Push/Pop fast path from using the old
  // slab offset/shift with the new slabs pointer. After the fence all CPUs
  // will uncache the offset and observe resizing_ on the next attempt
  // to cache the offset.
  CHECK_CONDITION(!resizing_.load(std::memory_order_relaxed));
  resizing_.store(true, std::memory_order_relaxed);
  for (size_t cpu = 0; cpu < num_cpus; ++cpu) {
    if (!populated(cpu)) continue;
    for (size_t size_class = 0; size_class < num_classes_; ++size_class) {
      Header header =
          LoadHeader(GetHeader(old_slabs, old_shift, cpu, size_class));
      CHECK_CONDITION(!header.IsLocked());
      resize_begins_[cpu * num_classes_ + size_class] = header.begin;
    }
    StopConcurrentMutations(old_slabs, old_shift, cpu, virtual_cpu_id_offset);
  }

  // Phase 3: Atomically update slabs and shift.
  slabs_and_shift_.store({new_slabs, new_shift}, std::memory_order_relaxed);

  // Phase 4: Return pointers from the old slab to the TransferCache.
  for (size_t cpu = 0; cpu < num_cpus; ++cpu) {
    if (!populated(cpu)) continue;
    DrainCpu(old_slabs, old_shift, cpu, drain_handler);
  }

  // Phase 5: Update all the `current` values to 0 and fence all CPUs. In RSEQ
  // for Pop/PopBatch, we load current before loading begin so it's possible to
  // get an interleaving of: (Thread 1) load current (>0); (Thread 2)
  // MADVISE_DONTNEED away slabs; (Thread 1) load begin (now ==0), see
  // begin<current so we can Pop.
  // NOTE: we do this after DrainCpu because DrainCpu relies on headers having
  // accurate `current` values.
  for (size_t cpu = 0; cpu < num_cpus; ++cpu) {
    if (!populated(cpu)) continue;
    for (size_t size_class = 0; size_class < num_classes_; ++size_class) {
      std::atomic<int64_t>* header_ptr =
          GetHeader(old_slabs, old_shift, cpu, size_class);
      Header header = LoadHeader(header_ptr);
      header.current = 0;
      StoreHeader(header_ptr, header);
    }
  }
  FenceAllCpus();
  resizing_.store(false, std::memory_order_relaxed);

  return {old_slabs, GetSlabsAllocSize(old_shift, num_cpus)};
}

void* TcmallocSlab::Destroy(
    absl::FunctionRef<void(void*, size_t, std::align_val_t)> free) {
  const auto [slabs, shift] = GetSlabsAndShift(std::memory_order_relaxed);
  free(slabs, GetSlabsAllocSize(shift, NumCPUs()), kPhysicalPageAlign);
  slabs_and_shift_.store({nullptr, shift}, std::memory_order_relaxed);
  return slabs;
}

size_t TcmallocSlab::GrowOtherCache(
    int cpu, size_t size_class, size_t len,
    absl::FunctionRef<size_t(uint8_t)> max_capacity) {
  ASSERT(cpu >= 0);
  ASSERT(cpu < NumCPUs());
  const auto [slabs, shift] = GetSlabsAndShift(std::memory_order_relaxed);
  const size_t virtual_cpu_id_offset = virtual_cpu_id_offset_;
  const size_t max_cap = max_capacity(ToUint8(shift));

  // Phase 1: Collect begin as it will be overwritten by the lock.
  std::atomic<int64_t>* hdrp = GetHeader(slabs, shift, cpu, size_class);
  Header hdr = LoadHeader(hdrp);
  CHECK_CONDITION(!hdr.IsLocked());
  ASSERT(hdr.IsInitialized());
  const uint16_t begin = hdr.begin;

  // Phase 2: stop concurrent mutations for <cpu> for size class <size_class>.
  do {
    LockHeader(slabs, shift, cpu, size_class);
    FenceCpu(cpu, virtual_cpu_id_offset);
    hdr = LoadHeader(hdrp);
    // If the header was overwritten in Grow/Shrink, then we need to try again.
  } while (!hdr.IsLocked());

  // Phase 3: Grow the capacity. Use a copy of begin and end_copy to
  // restore the header, shrink it, and return the length by which the
  // region was shrunk.
  uint16_t to_grow = std::min<uint16_t>(len, max_cap - (hdr.end_copy - begin));

  hdr.begin = begin;
  hdr.end_copy += to_grow;
  hdr.end = hdr.end_copy;
  StoreHeader(hdrp, hdr);
  return to_grow;
}

size_t TcmallocSlab::ShrinkOtherCache(int cpu, size_t size_class, size_t len,
                                      ShrinkHandler shrink_handler) {
  ASSERT(cpu >= 0);
  ASSERT(cpu < NumCPUs());
  const auto [slabs, shift] = GetSlabsAndShift(std::memory_order_relaxed);
  const size_t virtual_cpu_id_offset = virtual_cpu_id_offset_;

  // Phase 1: Collect begin as it will be overwritten by the lock.
  std::atomic<int64_t>* hdrp = GetHeader(slabs, shift, cpu, size_class);
  Header hdr = LoadHeader(hdrp);
  CHECK_CONDITION(!hdr.IsLocked());
  const uint16_t begin = hdr.begin;

  // Phase 2: stop concurrent mutations for <cpu> for size class <size_class>.
  do {
    LockHeader(slabs, shift, cpu, size_class);
    FenceCpu(cpu, virtual_cpu_id_offset);
    hdr = LoadHeader(hdrp);
    // If the header was overwritten in Grow/Shrink, then we need to try again.
  } while (!hdr.IsLocked());

  // Phase 3: If we do not have len number of items to shrink, we try
  // to pop items from the list first to create enough capacity that can be
  // shrunk. If we pop items, we also execute callbacks.
  //
  // We can't write all 4 fields at once with a single write, because Pop does
  // several non-atomic loads of the fields. Consider that a concurrent Pop
  // loads old current (still pointing somewhere in the middle of the region);
  // then we update all fields with a single write; then Pop loads the updated
  // begin which allows it to proceed; then it decrements current below begin.
  //
  // So we instead first just update current--our locked begin/end guarantee
  // no Push/Pop will make progress.  Once we Fence below, we know no Push/Pop
  // is using the old current, and can safely update begin/end to be an empty
  // slab.

  const uint16_t unused = hdr.end_copy - hdr.current;
  uint16_t actual_pop = 0;
  if (unused < len) {
    const uint16_t expected_pop = len - unused;
    actual_pop = std::min<uint16_t>(expected_pop, hdr.current - begin);
  }

  if (actual_pop > 0) {
    void** batch = reinterpret_cast<void**>(CpuMemoryStart(slabs, shift, cpu)) +
                   hdr.current - actual_pop;
    TSANAcquireBatch(batch, actual_pop);
    shrink_handler(size_class, batch, actual_pop);
    hdr.current -= actual_pop;
    StoreHeader(hdrp, hdr);
    FenceCpu(cpu, virtual_cpu_id_offset);
  }

  // Phase 4: Shrink the capacity. Use a copy of begin and end_copy to
  // restore the header, shrink it, and return the length by which the
  // region was shrunk.
  hdr.begin = begin;
  const uint16_t to_shrink =
      std::min<uint16_t>(len, hdr.end_copy - hdr.current);
  hdr.end_copy -= to_shrink;
  hdr.end = hdr.end_copy;
  StoreHeader(hdrp, hdr);
  return to_shrink;
}

void TcmallocSlab::Drain(int cpu, DrainHandler drain_handler) {
  CHECK_CONDITION(cpu >= 0);
  CHECK_CONDITION(cpu < NumCPUs());
  const auto [slabs, shift] = GetSlabsAndShift(std::memory_order_relaxed);
  const size_t virtual_cpu_id_offset = virtual_cpu_id_offset_;

  // Push/Pop/Grow/Shrink can be executed concurrently with Drain.
  // That's not an expected case, but it must be handled for correctness.
  // Push/Pop/Grow/Shrink can only be executed on <cpu> and use rseq primitives.
  // Push only updates current. Pop only updates current and end_copy
  // (it mutates only current but uses 4 byte write for performance).
  // Grow/Shrink mutate end and end_copy using 64-bit stores.

  // We attempt to stop all concurrent operations by writing 0xffff to begin
  // and 0 to end. However, Grow/Shrink can overwrite our write, so we do this
  // in a loop until we know that the header is in quiescent state.

  // Phase 1: collect all begin's (these are not mutated by anybody else).
  uint16_t* begin = &resize_begins_[cpu * num_classes_];
  for (size_t size_class = 0; size_class < num_classes_; ++size_class) {
    Header hdr = LoadHeader(GetHeader(slabs, shift, cpu, size_class));
    CHECK_CONDITION(!hdr.IsLocked());
    begin[size_class] = hdr.begin;
  }

  // Phase 2: stop concurrent mutations for <cpu>.
  StopConcurrentMutations(slabs, shift, cpu, virtual_cpu_id_offset);

  // Phase 3: execute callbacks.
  DrainCpu(slabs, shift, cpu, drain_handler);

  // Phase 4: reset current to beginning of the region.
  // We can't write all 4 fields at once with a single write, because Pop does
  // several non-atomic loads of the fields. Consider that a concurrent Pop
  // loads old current (still pointing somewhere in the middle of the region);
  // then we update all fields with a single write; then Pop loads the updated
  // begin which allows it to proceed; then it decrements current below begin.
  //
  // So we instead first just update current--our locked begin/end guarantee
  // no Push/Pop will make progress.  Once we Fence below, we know no Push/Pop
  // is using the old current, and can safely update begin/end to be an empty
  // slab.
  for (size_t size_class = 0; size_class < num_classes_; ++size_class) {
    std::atomic<int64_t>* hdrp = GetHeader(slabs, shift, cpu, size_class);
    Header hdr = LoadHeader(hdrp);
    hdr.current = begin[size_class];
    StoreHeader(hdrp, hdr);
  }

  // Phase 5: fence and reset the remaining fields to beginning of the region.
  // This allows concurrent mutations again.
  FenceCpu(cpu, virtual_cpu_id_offset);
  for (size_t size_class = 0; size_class < num_classes_; ++size_class) {
    std::atomic<int64_t>* hdrp = GetHeader(slabs, shift, cpu, size_class);
    Header hdr;
    hdr.current = begin[size_class];
    hdr.begin = begin[size_class];
    hdr.end = begin[size_class];
    hdr.end_copy = begin[size_class];
    StoreHeader(hdrp, hdr);
  }
}

PerCPUMetadataState TcmallocSlab::MetadataMemoryUsage() const {
  PerCPUMetadataState result;
  const auto [slabs, shift] = GetSlabsAndShift(std::memory_order_relaxed);
  size_t slabs_size = GetSlabsAllocSize(shift, NumCPUs());
  size_t resize_size = num_classes_ * NumCPUs() * sizeof(resize_begins_[0]);
  result.virtual_size = resize_size + slabs_size;
  result.resident_size = MInCore::residence(slabs, slabs_size);
  return result;
}

}  // namespace percpu
}  // namespace subtle
}  // namespace tcmalloc_internal
}  // namespace tcmalloc
GOOGLE_MALLOC_SECTION_END
