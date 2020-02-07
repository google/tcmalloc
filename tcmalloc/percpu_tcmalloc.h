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

#ifndef TCMALLOC_PERCPU_TCMALLOC_H_
#define TCMALLOC_PERCPU_TCMALLOC_H_

#include <bits/wordsize.h>

#include <atomic>
#include <cstring>

#include "absl/base/dynamic_annotations.h"
#include "absl/base/internal/sysinfo.h"
#include "tcmalloc/internal/mincore.h"
#include "tcmalloc/internal/percpu.h"

namespace tcmalloc {

struct PerCPUMetadataState {
  size_t virtual_size;
  size_t resident_size;
};

namespace subtle {
namespace percpu {

// Tcmalloc slab for per-cpu caching mode.
// Conceptually it is equivalent to an array of NumClasses PerCpuSlab's,
// and in fallback implementation it is implemented that way. But optimized
// implementation uses more compact layout and provides faster operations.
//
// Methods of this type must only be used in threads where it is known that the
// percpu primitives are available and percpu::IsFast() has previously returned
// 'true'.
//
// The template parameter Shift indicates the number of bits to shift the
// the CPU id in order to get the location of the per-cpu slab. If this
// parameter matches PERCPU_TCMALLOC_FIXED_SLAB_SHIFT as set in
// percpu_intenal.h then the assembly language versions of push/pop batch
// can be used; otherwise batch operations are emulated.
template <size_t Shift, size_t NumClasses>
class TcmallocSlab {
 public:
  TcmallocSlab() {}

  // Init must be called before any other methods.
  // <alloc> is memory allocation callback (e.g. malloc).
  // <capacity> callback returns max capacity for size class <cl>.
  // <lazy> indicates that per-CPU slabs should be populated on demand
  //
  // Initial capacity is 0 for all slabs.
  void Init(void*(alloc)(size_t size), size_t (*capacity)(size_t cl),
            bool lazy);

  // Only may be called if Init(..., lazy = true) was used.
  void InitCPU(int cpu, size_t (*capacity)(size_t cl));

  // For tests.
  void Destroy(void(free)(void*));

  // Number of elements in cpu/cl slab.
  size_t Length(int cpu, size_t cl) const;

  // Number of elements (currently) allowed in cpu/cl slab.
  size_t Capacity(int cpu, size_t cl) const;

  // If running on cpu, increment the cpu/cl slab's capacity to no greater than
  // min(capacity+len, max_cap) and return the increment applied. Otherwise
  // return 0. Note: max_cap must be the same as returned by capacity callback
  // passed to Init.
  size_t Grow(int cpu, size_t cl, size_t len, size_t max_cap);

  // If running on cpu, decrement the cpu/cl slab's capacity to no less than
  // max(capacity-len, 0) and return the actual decrement applied. Otherwise
  // return 0.
  size_t Shrink(int cpu, size_t cl, size_t len);

  // Add an item (which must be non-zero) to the current CPU's slab. Returns
  // true if add succeeds. Otherwise invokes <f> and returns false (assuming
  // that <f> returns negative value).
  bool Push(size_t cl, void* item, OverflowHandler f);

  // Remove an item (LIFO) from the current CPU's slab. If the slab is empty,
  // invokes <f> and returns its result.
  void* Pop(size_t cl, UnderflowHandler f);

  // Add up to <len> items to the current cpu slab from the array located at
  // <batch>. Returns the number of items that were added (possibly 0). All
  // items not added will be returned at the start of <batch>. Items are only
  // not added if there is no space on the current cpu.
  // REQUIRES: len > 0.
  size_t PushBatch(size_t cl, void** batch, size_t len);

  // Pop up to <len> items from the current cpu slab and return them in <batch>.
  // Returns the number of items actually removed.
  // REQUIRES: len > 0.
  size_t PopBatch(size_t cl, void** batch, size_t len);

  // Remove all items (of all classes) from <cpu>'s slab; reset capacity for all
  // classes to zero.  Then, for each sizeclass, invoke
  // DrainHandler(drain_ctx, cl, <items from slab>, <previous slab capacity>);
  //
  // It is invalid to concurrently execute Drain() for the same CPU; calling
  // Push/Pop/Grow/Shrink concurrently (even on the same CPU) is safe.
  typedef void (*DrainHandler)(void* drain_ctx, size_t cl, void** batch,
                               size_t n, size_t cap);
  void Drain(int cpu, void* drain_ctx, DrainHandler f);

  PerCPUMetadataState MetadataMemoryUsage() const;

 private:
  // Slab header (packed, atomically updated 64-bit).
  struct Header {
    // All values are word offsets from per-CPU region start.
    // The array is [begin, end).
    uint16_t current;
    // Copy of end. Updated by Shrink/Grow, but is not overwritten by Drain.
    uint16_t end_copy;
    // Lock updates only begin and end with a 32-bit write.
    uint16_t begin;
    uint16_t end;

    // Lock is used by Drain to stop concurrent mutations of the Header.
    // Lock sets begin to 0xffff and end to 0, which makes Push and Pop fail
    // regardless of current value.
    bool IsLocked() const;
    void Lock();
  };

  // We cast Header to std::atomic<int64_t>.
  static_assert(sizeof(Header) == sizeof(std::atomic<int64_t>),
                "bad Header size");

  // We use a single continuous region of memory for all slabs on all CPUs.
  // This region is split into NumCPUs regions of size kPerCpuMem (256k).
  // First NumClasses words of each CPU region are occupied by slab
  // headers (Header struct). The remaining memory contain slab arrays.
  struct Slabs {
    std::atomic<int64_t> header[NumClasses];
    void* mem[((1ul << Shift) - sizeof(header)) / sizeof(void*)];
  };
  static_assert(sizeof(Slabs) == (1ul << Shift), "Slabs has unexpected size");

  Slabs* slabs_;

  Slabs* CpuMemoryStart(int cpu) const;
  std::atomic<int64_t>* GetHeader(int cpu, size_t cl) const;
  static Header LoadHeader(std::atomic<int64_t>* hdrp);
  static void StoreHeader(std::atomic<int64_t>* hdrp, Header hdr);
  static int CompareAndSwapHeader(int cpu, std::atomic<int64_t>* hdrp,
                                  Header old, Header hdr);
};

template <size_t Shift, size_t NumClasses>
inline size_t TcmallocSlab<Shift, NumClasses>::Length(int cpu,
                                                      size_t cl) const {
  Header hdr = LoadHeader(GetHeader(cpu, cl));
  return hdr.IsLocked() ? 0 : hdr.current - hdr.begin;
}

template <size_t Shift, size_t NumClasses>
inline size_t TcmallocSlab<Shift, NumClasses>::Capacity(int cpu,
                                                        size_t cl) const {
  Header hdr = LoadHeader(GetHeader(cpu, cl));
  return hdr.IsLocked() ? 0 : hdr.end - hdr.begin;
}

template <size_t Shift, size_t NumClasses>
inline size_t TcmallocSlab<Shift, NumClasses>::Grow(int cpu, size_t cl,
                                                    size_t len,
                                                    size_t max_cap) {
  std::atomic<int64_t>* hdrp = GetHeader(cpu, cl);
  for (;;) {
    Header old = LoadHeader(hdrp);
    if (old.IsLocked() || old.end - old.begin == max_cap) {
      return 0;
    }
    uint16_t n = std::min<uint16_t>(len, max_cap - (old.end - old.begin));
    Header hdr = old;
    hdr.end += n;
    hdr.end_copy += n;
    const int ret = CompareAndSwapHeader(cpu, hdrp, old, hdr);
    if (ret == cpu) {
      return n;
    } else if (ret >= 0) {
      return 0;
    }
  }
}

template <size_t Shift, size_t NumClasses>
inline size_t TcmallocSlab<Shift, NumClasses>::Shrink(int cpu, size_t cl,
                                                      size_t len) {
  std::atomic<int64_t>* hdrp = GetHeader(cpu, cl);
  for (;;) {
    Header old = LoadHeader(hdrp);
    if (old.IsLocked() || old.current == old.end) {
      return 0;
    }
    uint16_t n = std::min<uint16_t>(len, old.end - old.current);
    Header hdr = old;
    hdr.end -= n;
    hdr.end_copy -= n;
    const int ret = CompareAndSwapHeader(cpu, hdrp, old, hdr);
    if (ret == cpu) {
      return n;
    } else if (ret >= 0) {
      return 0;
    }
  }
}

template <size_t Shift, size_t NumClasses>
inline ABSL_ATTRIBUTE_ALWAYS_INLINE bool TcmallocSlab<Shift, NumClasses>::Push(
    size_t cl, void* item, OverflowHandler f) {
  ASSERT(item != nullptr);
  if (Shift == PERCPU_TCMALLOC_FIXED_SLAB_SHIFT) {
    return TcmallocSlab_Push_FixedShift(slabs_, cl, item, f) >= 0;
  } else {
    return TcmallocSlab_Push(slabs_, cl, item, Shift, f) >= 0;
  }
}

template <size_t Shift, size_t NumClasses>
inline ABSL_ATTRIBUTE_ALWAYS_INLINE void* TcmallocSlab<Shift, NumClasses>::Pop(
    size_t cl, UnderflowHandler f) {
  if (Shift == PERCPU_TCMALLOC_FIXED_SLAB_SHIFT) {
    return TcmallocSlab_Pop_FixedShift(slabs_, cl, f);
  } else {
    return TcmallocSlab_Pop(slabs_, cl, f, Shift);
  }
}

static inline void* NoopUnderflow(int cpu, size_t cl) { return nullptr; }

static inline int NoopOverflow(int cpu, size_t cl, void* item) { return -1; }

template <size_t Shift, size_t NumClasses>
inline size_t TcmallocSlab<Shift, NumClasses>::PushBatch(size_t cl,
                                                         void** batch,
                                                         size_t len) {
  ASSERT(len != 0);
  if (Shift == PERCPU_TCMALLOC_FIXED_SLAB_SHIFT) {
    return TcmallocSlab_PushBatch_FixedShift(slabs_, cl, batch, len);
  } else {
    size_t n = 0;
    // Push items until either all done or a push fails
    while (n < len && Push(cl, batch[len - 1 - n], NoopOverflow)) {
      n++;
    }
    return n;
  }
}

template <size_t Shift, size_t NumClasses>
inline size_t TcmallocSlab<Shift, NumClasses>::PopBatch(size_t cl, void** batch,
                                                        size_t len) {
  ASSERT(len != 0);
  size_t n = 0;
  if (Shift == PERCPU_TCMALLOC_FIXED_SLAB_SHIFT) {
    n = TcmallocSlab_PopBatch_FixedShift(slabs_, cl, batch, len);
    // PopBatch is implemented in assembly, msan does not know that the returned
    // batch is initialized.
    ANNOTATE_MEMORY_IS_INITIALIZED(batch, n * sizeof(batch[0]));
  } else {
    // Pop items until either all done or a pop fails
    while (n < len && (batch[n] = Pop(cl, NoopUnderflow))) {
      n++;
    }
  }
  return n;
}

template <size_t Shift, size_t NumClasses>
inline typename TcmallocSlab<Shift, NumClasses>::Slabs*
TcmallocSlab<Shift, NumClasses>::CpuMemoryStart(int cpu) const {
  return &slabs_[cpu];
}

template <size_t Shift, size_t NumClasses>
inline std::atomic<int64_t>* TcmallocSlab<Shift, NumClasses>::GetHeader(
    int cpu, size_t cl) const {
  return &CpuMemoryStart(cpu)->header[cl];
}

template <size_t Shift, size_t NumClasses>
inline typename TcmallocSlab<Shift, NumClasses>::Header
TcmallocSlab<Shift, NumClasses>::LoadHeader(std::atomic<int64_t>* hdrp) {
  uint64_t raw = hdrp->load(std::memory_order_relaxed);
  Header hdr;
  memcpy(&hdr, &raw, sizeof(hdr));
  return hdr;
}

template <size_t Shift, size_t NumClasses>
inline void TcmallocSlab<Shift, NumClasses>::StoreHeader(
    std::atomic<int64_t>* hdrp, Header hdr) {
  uint64_t raw;
  memcpy(&raw, &hdr, sizeof(raw));
  hdrp->store(raw, std::memory_order_relaxed);
}

template <size_t Shift, size_t NumClasses>
inline int TcmallocSlab<Shift, NumClasses>::CompareAndSwapHeader(
    int cpu, std::atomic<int64_t>* hdrp, Header old, Header hdr) {
#if __WORDSIZE == 64
  uint64_t old_raw, new_raw;
  memcpy(&old_raw, &old, sizeof(old_raw));
  memcpy(&new_raw, &hdr, sizeof(new_raw));
  return CompareAndSwapUnsafe(cpu, hdrp, static_cast<intptr_t>(old_raw),
                              static_cast<intptr_t>(new_raw));
#else
  Log(kCrash, __FILE__, __LINE__, "This architecture is not supported.");
#endif
}

template <size_t Shift, size_t NumClasses>
inline bool TcmallocSlab<Shift, NumClasses>::Header::IsLocked() const {
  return begin == 0xffffu;
}

template <size_t Shift, size_t NumClasses>
inline void TcmallocSlab<Shift, NumClasses>::Header::Lock() {
  // Write 0xffff to begin and 0 to end. This blocks new Push'es and Pop's.
  // Note: we write only 4 bytes. The first 4 bytes are left intact.
  // See Drain method for details. tl;dr: C++ does not allow us to legally
  // express this without undefined behavior.
  std::atomic<int32_t>* p = reinterpret_cast<std::atomic<int32_t>*>(&begin);
  Header hdr;
  hdr.begin = 0xffffu;
  hdr.end = 0;
  int32_t raw;
  memcpy(&raw, &hdr.begin, sizeof(raw));
  p->store(raw, std::memory_order_relaxed);
}

template <size_t Shift, size_t NumClasses>
void TcmallocSlab<Shift, NumClasses>::Init(void*(alloc)(size_t size),
                                           size_t (*capacity)(size_t cl),
                                           bool lazy) {
  size_t mem_size = absl::base_internal::NumCPUs() * (1ul << Shift);
  void* backing = alloc(mem_size);
  // MSan does not see writes in assembly.
  ANNOTATE_MEMORY_IS_INITIALIZED(backing, mem_size);
  if (!lazy) {
    memset(backing, 0, mem_size);
  }
  slabs_ = static_cast<Slabs*>(backing);
  size_t bytes_used = 0;
  for (int cpu = 0; cpu < absl::base_internal::NumCPUs(); ++cpu) {
    bytes_used += sizeof(std::atomic<int64_t>) * NumClasses;

    for (size_t cl = 0; cl < NumClasses; ++cl) {
      size_t cap = capacity(cl);
      if (!cap) {
        continue;
      }

      // One extra element for prefetch
      bytes_used += (cap + 1) * sizeof(void*);
    }

    if (!lazy) {
      InitCPU(cpu, capacity);
    }
  }
  // Check for less than 90% usage of the reserved memory
  if (bytes_used * 10 < 9 * mem_size) {
    Log(kLog, __FILE__, __LINE__, "Bytes used per cpu of available", bytes_used,
        mem_size);
  }
}

template <size_t Shift, size_t NumClasses>
void TcmallocSlab<Shift, NumClasses>::InitCPU(int cpu,
                                              size_t (*capacity)(size_t cl)) {
  void** elems = slabs_[cpu].mem;
  for (size_t cl = 0; cl < NumClasses; ++cl) {
    size_t cap = capacity(cl);
    if (!cap) {
      continue;
    }
    CHECK_CONDITION(static_cast<uint16_t>(cap) == cap);

    Header hdr = {};
    // In Pop() we prefetch the item a subsequent Pop() would return;
    // this is slow if it's not a valid pointer. To avoid this problem
    // when popping the last item, keep one fake item before the actual
    // ones (that points, safely, to itself.)
    *elems = elems;
    elems++;
    size_t begin = elems - reinterpret_cast<void**>(CpuMemoryStart(cpu));
    hdr.current = begin;
    hdr.begin = begin;
    hdr.end = begin;
    hdr.end_copy = begin;
    elems += cap;
    CHECK_CONDITION(reinterpret_cast<char*>(elems) -
                        reinterpret_cast<char*>(CpuMemoryStart(cpu)) <=
                    (1 << Shift));

    StoreHeader(GetHeader(cpu, cl), hdr);
  }
}

template <size_t Shift, size_t NumClasses>
void TcmallocSlab<Shift, NumClasses>::Destroy(void(free)(void*)) {
  free(slabs_);
  slabs_ = nullptr;
}

template <size_t Shift, size_t NumClasses>
void TcmallocSlab<Shift, NumClasses>::Drain(int cpu, void* ctx,
                                            DrainHandler f) {
  CHECK_CONDITION(cpu >= 0);
  CHECK_CONDITION(cpu < absl::base_internal::NumCPUs());

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
  uint16_t begin[NumClasses];
  for (size_t cl = 0; cl < NumClasses; ++cl) {
    Header hdr = LoadHeader(GetHeader(cpu, cl));
    CHECK_CONDITION(!hdr.IsLocked());
    begin[cl] = hdr.begin;
  }

  // Phase 2: stop concurrent mutations.
  for (bool done = false; !done;) {
    for (size_t cl = 0; cl < NumClasses; ++cl) {
      // Note: this reinterpret_cast and write in Lock lead to undefined
      // behavior, because the actual object type is std::atomic<int64_t>. But
      // C++ does not allow to legally express what we need here: atomic writes
      // of different sizes.
      reinterpret_cast<Header*>(GetHeader(cpu, cl))->Lock();
    }
    FenceCpu(cpu);
    done = true;
    for (size_t cl = 0; cl < NumClasses; ++cl) {
      Header hdr = LoadHeader(GetHeader(cpu, cl));
      if (!hdr.IsLocked()) {
        // Header was overwritten by Grow/Shrink. Retry.
        done = false;
        break;
      }
    }
  }

  // Phase 3: execute callbacks.
  for (size_t cl = 0; cl < NumClasses; ++cl) {
    Header hdr = LoadHeader(GetHeader(cpu, cl));
    // We overwrote begin and end, instead we use our local copy of begin
    // and end_copy.
    size_t n = hdr.current - begin[cl];
    size_t cap = hdr.end_copy - begin[cl];
    void** batch = reinterpret_cast<void**>(GetHeader(cpu, 0) + begin[cl]);
    f(ctx, cl, batch, n, cap);
  }

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
  for (size_t cl = 0; cl < NumClasses; ++cl) {
    std::atomic<int64_t>* hdrp = GetHeader(cpu, cl);
    Header hdr = LoadHeader(hdrp);
    hdr.current = begin[cl];
    StoreHeader(hdrp, hdr);
  }

  // Phase 5: fence and reset the remaining fields to beginning of the region.
  // This allows concurrent mutations again.
  FenceCpu(cpu);
  for (size_t cl = 0; cl < NumClasses; ++cl) {
    std::atomic<int64_t>* hdrp = GetHeader(cpu, cl);
    Header hdr;
    hdr.current = begin[cl];
    hdr.begin = begin[cl];
    hdr.end = begin[cl];
    hdr.end_copy = begin[cl];
    StoreHeader(hdrp, hdr);
  }
}

template <size_t Shift, size_t NumClasses>
PerCPUMetadataState TcmallocSlab<Shift, NumClasses>::MetadataMemoryUsage()
    const {
  PerCPUMetadataState result;
  result.virtual_size = absl::base_internal::NumCPUs() * sizeof(*slabs_);
  result.resident_size = MInCore::residence(slabs_, result.virtual_size);
  return result;
}

}  // namespace percpu
}  // namespace subtle
}  // namespace tcmalloc

#endif  // TCMALLOC_PERCPU_TCMALLOC_H_
