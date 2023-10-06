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

#ifndef TCMALLOC_INTERNAL_PERCPU_TCMALLOC_H_
#define TCMALLOC_INTERNAL_PERCPU_TCMALLOC_H_

#if defined(__linux__)
#include <linux/param.h>
#else
#include <sys/param.h>
#endif
#include <sys/mman.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <memory>
#include <new>
#include <utility>

#include "absl/base/attributes.h"
#include "absl/base/casts.h"
#include "absl/base/dynamic_annotations.h"
#include "absl/base/internal/sysinfo.h"
#include "absl/base/optimization.h"
#include "absl/functional/function_ref.h"
#include "tcmalloc/internal/logging.h"
#include "tcmalloc/internal/mincore.h"
#include "tcmalloc/internal/percpu.h"
#include "tcmalloc/internal/sysinfo.h"

#if defined(TCMALLOC_INTERNAL_PERCPU_USE_RSEQ)
#if !defined(__clang__)
#define TCMALLOC_INTERNAL_PERCPU_USE_RSEQ_ASM_GOTO 1
#elif __clang_major__ >= 9 && !__has_feature(speculative_load_hardening)
// asm goto requires the use of Clang 9 or newer:
// https://releases.llvm.org/9.0.0/tools/clang/docs/ReleaseNotes.html#c-language-changes-in-clang
//
// SLH (Speculative Load Hardening) builds do not support asm goto.  We can
// detect these compilation modes since
// https://github.com/llvm/llvm-project/commit/379e68a763097bed55556c6dc7453e4b732e3d68.
#define TCMALLOC_INTERNAL_PERCPU_USE_RSEQ_ASM_GOTO 1
#if __clang_major__ >= 11
#define TCMALLOC_INTERNAL_PERCPU_USE_RSEQ_ASM_GOTO_OUTPUT 1
#endif

#else
#define TCMALLOC_INTERNAL_PERCPU_USE_RSEQ_ASM_GOTO 0
#endif
#else
#define TCMALLOC_INTERNAL_PERCPU_USE_RSEQ_ASM_GOTO 0
#endif

GOOGLE_MALLOC_SECTION_BEGIN
namespace tcmalloc {
namespace tcmalloc_internal {

struct PerCPUMetadataState {
  size_t virtual_size;
  size_t resident_size;
};

// The bit denotes that tcmalloc_slabs contains valid slabs offset.
constexpr inline uintptr_t kCachedSlabsBit = 63;
constexpr inline uintptr_t kCachedSlabsMask = 1ul << kCachedSlabsBit;

struct ResizeSlabsInfo {
  void* old_slabs;
  size_t old_slabs_size;
};

namespace subtle {
namespace percpu {

enum class Shift : uint8_t;
constexpr uint8_t ToUint8(Shift shift) { return static_cast<uint8_t>(shift); }
constexpr Shift ToShiftType(size_t shift) {
  ASSERT(ToUint8(static_cast<Shift>(shift)) == shift);
  return static_cast<Shift>(shift);
}

// The allocation size for the slabs array.
inline size_t GetSlabsAllocSize(Shift shift, int num_cpus) {
  return static_cast<size_t>(num_cpus) << ToUint8(shift);
}

// Since we lazily initialize our slab, we expect it to be mmap'd and not
// resident.  We align it to a page size so neighboring allocations (from
// TCMalloc's internal arena) do not necessarily cause the metadata to be
// faulted in.
//
// We prefer a small page size (EXEC_PAGESIZE) over the anticipated huge page
// size to allow small-but-slow to allocate the slab in the tail of its
// existing Arena block.
static constexpr std::align_val_t kPhysicalPageAlign{EXEC_PAGESIZE};

// Tcmalloc slab for per-cpu caching mode.
// Conceptually it is equivalent to an array of NumClasses PerCpuSlab's,
// and in fallback implementation it is implemented that way. But optimized
// implementation uses more compact layout and provides faster operations.
//
// Methods of this type must only be used in threads where it is known that the
// percpu primitives are available and percpu::IsFast() has previously returned
// 'true'.
template <size_t NumClasses>
class TcmallocSlab {
 public:
  using DrainHandler = absl::FunctionRef<void(
      int cpu, size_t size_class, void** batch, size_t size, size_t cap)>;
  using ShrinkHandler =
      absl::FunctionRef<void(size_t size_class, void** batch, size_t size)>;

  // We use a single continuous region of memory for all slabs on all CPUs.
  // This region is split into NumCPUs regions of size kPerCpuMem (256k).
  // First NumClasses words of each CPU region are occupied by slab
  // headers (Header struct). The remaining memory contain slab arrays.
  struct Slabs {
    std::atomic<int64_t> header[NumClasses];
    void* mem[];
  };

  constexpr TcmallocSlab() = default;

  // Init must be called before any other methods.
  // <slabs> is memory for the slabs with size corresponding to <shift>.
  // <capacity> callback returns max capacity for size class <size_class>.
  // <shift> indicates the number of bits to shift the CPU ID in order to
  //     obtain the location of the per-CPU slab.
  //
  // Initial capacity is 0 for all slabs.
  void Init(Slabs* slabs, absl::FunctionRef<size_t(size_t)> capacity,
            Shift shift);

  // Lazily initializes the slab for a specific cpu.
  // <capacity> callback returns max capacity for size class <size_class>.
  //
  // Prior to InitCpu being called on a particular `cpu`, non-const operations
  // other than Push/Pop/PushBatch/PopBatch are invalid.
  void InitCpu(int cpu, absl::FunctionRef<size_t(size_t)> capacity);

  // Grows or shrinks the size of the slabs to use the <new_shift> value. First
  // we initialize <new_slabs>, then lock all headers on the old slabs,
  // atomically update to use the new slabs, and teardown the old slabs. Returns
  // a pointer to old slabs to be madvised away along with the size of the old
  // slabs and the number of bytes that were reused.
  //
  // <alloc> is memory allocation callback (e.g. malloc).
  // <capacity> callback returns max capacity for size class <cl>.
  // <populated> returns whether the corresponding cpu has been populated.
  //
  // Caller must ensure that there are no concurrent calls to InitCpu,
  // ShrinkOtherCache, or Drain.
  ABSL_MUST_USE_RESULT ResizeSlabsInfo ResizeSlabs(
      Shift new_shift, Slabs* new_slabs,
      absl::FunctionRef<void*(size_t, std::align_val_t)> alloc,
      absl::FunctionRef<size_t(size_t)> capacity,
      absl::FunctionRef<bool(size_t)> populated, DrainHandler drain_handler);

  // For tests. Returns the freed slabs pointer.
  void* Destroy(absl::FunctionRef<void(void*, size_t, std::align_val_t)> free);

  // Number of elements in cpu/size_class slab.
  size_t Length(int cpu, size_t size_class) const;

  // Number of elements (currently) allowed in cpu/size_class slab.
  size_t Capacity(int cpu, size_t size_class) const;

  // If running on cpu, increment the cpu/size_class slab's capacity to no
  // greater than min(capacity+len, max_capacity(<shift>)) and return the
  // increment applied. Otherwise return 0.
  // <max_capacity> is a callback that takes the current slab shift as input and
  // returns the max capacity of <size_class> for that shift value - this is in
  // order to ensure that the shift value used is consistent with the one used
  // in the rest of this function call. Note: max_capacity must be the same as
  // returned by capacity callback passed to Init.
  size_t Grow(int cpu, size_t size_class, size_t len,
              absl::FunctionRef<size_t(uint8_t)> max_capacity);

  // If running on cpu, decrement the cpu/size_class slab's capacity to no less
  // than max(capacity-len, 0) and return the actual decrement applied.
  // Otherwise return 0.
  size_t Shrink(int cpu, size_t size_class, size_t len);

  // Add an item (which must be non-zero) to the current CPU's slab. Returns
  // true if add succeeds. Otherwise invokes <overflow_handler> and returns
  // false (assuming that <overflow_handler> returns negative value).
  bool Push(size_t size_class, void* item, OverflowHandler overflow_handler,
            void* arg);

  // Minimum policy required for Pop().
  struct NoopPolicy {
    using pointer_type = void*;
    static void* to_pointer(void* p, size_t size_class) { return p; }
    static constexpr bool size_returning() { return false; }
  };

  // Remove an item (LIFO) from the current CPU's slab. If the slab is empty,
  // invokes <underflow_handler> and returns its result.
  template <typename Policy = NoopPolicy>
  ABSL_MUST_USE_RESULT auto Pop(
      size_t class_size,
      UnderflowHandler<typename Policy::pointer_type> underflow_handler,
      void* arg);

  // Add up to <len> items to the current cpu slab from the array located at
  // <batch>. Returns the number of items that were added (possibly 0). All
  // items not added will be returned at the start of <batch>. Items are not
  // added if there is no space on the current cpu, or if the thread was
  // re-scheduled since last Push/Pop.
  // REQUIRES: len > 0.
  size_t PushBatch(size_t size_class, void** batch, size_t len);

  // Pop up to <len> items from the current cpu slab and return them in <batch>.
  // Returns the number of items actually removed. If the thread was
  // re-scheduled since last Push/Pop, the function returns 0.
  // REQUIRES: len > 0.
  size_t PopBatch(size_t size_class, void** batch, size_t len);

  // Grows the cpu/size_class slab's capacity to no greater than
  // min(capacity+len, max_capacity(<shift>)) and returns the increment
  // applied.
  // <max_capacity> is a callback that takes the current slab shift as input and
  // returns the max capacity of <size_class> for that shift value - this is in
  // order to ensure that the shift value used is consistent with the one used
  // in the rest of this function call. Note: max_capacity must be the same as
  // returned by capacity callback passed to Init.
  // This may be called from another processor, not just the <cpu>.
  size_t GrowOtherCache(int cpu, size_t size_class, size_t len,
                        absl::FunctionRef<size_t(uint8_t)> max_capacity);

  // Decrements the cpu/size_class slab's capacity to no less than
  // max(capacity-len, 0) and returns the actual decrement applied. It attempts
  // to shrink any unused capacity (i.e end-current) in cpu/size_class's slab;
  // if it does not have enough unused items, it pops up to <len> items from
  // cpu/size_class slab and then shrinks the freed capacity.
  //
  // May be called from another processor, not just the <cpu>.
  // REQUIRES: len > 0.
  size_t ShrinkOtherCache(int cpu, size_t size_class, size_t len,
                          ShrinkHandler shrink_handler);

  // Remove all items (of all classes) from <cpu>'s slab; reset capacity for all
  // classes to zero.  Then, for each sizeclass, invoke
  // DrainHandler(size_class, <items from slab>, <previous slab capacity>);
  //
  // It is invalid to concurrently execute Drain() for the same CPU; calling
  // Push/Pop/Grow/Shrink concurrently (even on the same CPU) is safe.
  void Drain(int cpu, DrainHandler drain_handler);

  PerCPUMetadataState MetadataMemoryUsage() const;

  inline int GetCurrentVirtualCpuUnsafe() {
    return VirtualRseqCpuId(virtual_cpu_id_offset_);
  }

  // Gets the current shift of the slabs. Intended for use by the thread that
  // calls ResizeSlabs().
  uint8_t GetShift() const {
    return ToUint8(GetSlabsAndShift(std::memory_order_relaxed).second);
  }

 private:
  // In order to support dynamic slab metadata sizes, we need to be able to
  // atomically update both the slabs pointer and the shift value so we store
  // both together in an atomic SlabsAndShift, which manages the bit operations.
  class SlabsAndShift {
   public:
    // These masks allow for distinguishing the shift bits from the slabs
    // pointer bits. The maximum shift value is less than kShiftMask and
    // kShiftMask is less than kPhysicalPageAlign.
    static constexpr size_t kShiftMask = 0xFF;
    static constexpr size_t kSlabsMask = ~kShiftMask;

    constexpr explicit SlabsAndShift() noexcept : raw_(0) {}
    SlabsAndShift(const Slabs* slabs, Shift shift)
        : raw_(reinterpret_cast<uintptr_t>(slabs) | ToUint8(shift)) {
      ASSERT((raw_ & kShiftMask) == ToUint8(shift));
      ASSERT(reinterpret_cast<Slabs*>(raw_ & kSlabsMask) == slabs);
    }

    std::pair<Slabs*, Shift> Get() const {
      static_assert(kShiftMask >= 0 && kShiftMask <= UCHAR_MAX,
                    "kShiftMask must fit in a uint8_t");
      // Avoid expanding the width of Shift else the compiler will insert an
      // additional instruction to zero out the upper bits on the critical path
      // of alloc / free.  Not zeroing out the bits is safe because both ARM and
      // x86 only use the lowest byte for shift count in variable shifts.
      return {reinterpret_cast<TcmallocSlab::Slabs*>(raw_ & kSlabsMask),
              static_cast<Shift>(raw_ & kShiftMask)};
    }

    uintptr_t Raw() const {
      // We depend on this in PushBatch/PopBatch.
      static_assert(kShiftMask == 0xFF);
      static_assert(kSlabsMask ==
                    static_cast<size_t>(TCMALLOC_PERCPU_SLABS_MASK));
      return raw_;
    }

   private:
    uintptr_t raw_;
  };

  // Slab header (packed, atomically updated 64-bit).
  // All {begin, current, end} values are pointer offsets from per-CPU region
  // start. The slot array is in [begin, end), and the occupied slots are in
  // [begin, current).
  struct Header {
    // The end offset of the currently occupied slots.
    uint16_t current;
    // Copy of end. Updated by Shrink/Grow, but is not overwritten by Drain.
    uint16_t end_copy;
    // Lock updates only begin and end with a 32-bit write.
    union {
      struct {
        // The begin offset of the slot array for this size class.
        uint16_t begin;
        // The end offset of the slot array for this size class.
        uint16_t end;
      };
      uint32_t lock_update;
    };

    // Lock is used by Drain to stop concurrent mutations of the Header.
    // Lock sets begin to 0xffff and end to 0, which makes Push and Pop fail
    // regardless of current value.
    bool IsLocked() const;
    void Lock();

    bool IsInitialized() const {
      // Once we initialize a header, begin/end are never simultaneously 0
      // to avoid pointing at the Header array.
      return lock_update != 0;
    }
  };

  // We cast Header to std::atomic<int64_t>.
  static_assert(sizeof(Header) == sizeof(std::atomic<int64_t>),
                "bad Header size");

  // It's important that we use consistent values for slabs/shift rather than
  // loading from the atomic repeatedly whenever we use one of the values.
  ABSL_MUST_USE_RESULT std::pair<Slabs*, Shift> GetSlabsAndShift(
      std::memory_order order) const {
    return slabs_and_shift_.load(order).Get();
  }

  static Slabs* CpuMemoryStart(Slabs* slabs, Shift shift, int cpu);
  static std::atomic<int64_t>* GetHeader(Slabs* slabs, Shift shift, int cpu,
                                         size_t size_class);
  static Header LoadHeader(std::atomic<int64_t>* hdrp);
  static void StoreHeader(std::atomic<int64_t>* hdrp, Header hdr);
  static void LockHeader(Slabs* slabs, Shift shift, int cpu, size_t size_class);
  static int CompareAndSwapHeader(int cpu, std::atomic<int64_t>* hdrp,
                                  Header old, Header hdr,
                                  size_t virtual_cpu_id_offset);
  // <begins> is an array of the <begin> values for each size class.
  static void DrainCpu(Slabs* slabs, Shift shift, int cpu, uint16_t* begins,
                       DrainHandler drain_handler);
  // Stops concurrent mutations from occurring for <cpu> by locking the
  // corresponding headers. All allocations/deallocations will miss this cache
  // for <cpu> until the headers are unlocked.
  static void StopConcurrentMutations(Slabs* slabs, Shift shift, int cpu,
                                      size_t virtual_cpu_id_offset);

  // Implementation of InitCpu() allowing for reuse in ResizeSlabs().
  static void InitCpuImpl(Slabs* slabs, Shift shift, int cpu,
                          size_t virtual_cpu_id_offset,
                          absl::FunctionRef<size_t(size_t)> capacity);

  // Pop slow path when we don't have any elements, don't have slab offset
  // cached, or resizing.
  template <typename Policy = NoopPolicy>
  ABSL_MUST_USE_RESULT auto PopSlow(
      size_t class_size,
      UnderflowHandler<typename Policy::pointer_type> underflow_handler,
      void* arg);

  // Push slow path when we don't have any elements, don't have slab offset
  // cached, or resizing.
  bool PushSlow(size_t size_class, void* item, OverflowHandler overflow_handler,
                void* arg);

  // Caches the current cpu slab offset in tcmalloc_slabs if it wasn't
  // cached and the slab is not resizing. Returns -1 if the offset was cached
  // and Push/Pop needs to be retried. Returns the current CPU ID (>=0) when
  // the slabs offset was already cached and we need to call underflow/overflow
  // callback.
  int CacheCpuSlab();
  int CacheCpuSlabSlow();

  // We store both a pointer to the array of slabs and the shift value together
  // so that we can atomically update both with a single store.
  std::atomic<SlabsAndShift> slabs_and_shift_{};
  // This is in units of bytes.
  size_t virtual_cpu_id_offset_ = offsetof(kernel_rseq, cpu_id);
  // In ResizeSlabs, we need to allocate space to store begin offsets on the
  // arena. We reuse this space here.
  uint16_t (*resize_begins_)[NumClasses] = nullptr;
  // ResizeSlabs is running so any Push/Pop should go to fallback
  // overflow/underflow handler.
  std::atomic<bool> resizing_{false};
};

template <size_t NumClasses>
inline size_t TcmallocSlab<NumClasses>::Length(int cpu,
                                               size_t size_class) const {
  const auto [slabs, shift] = GetSlabsAndShift(std::memory_order_relaxed);
  Header hdr = LoadHeader(GetHeader(slabs, shift, cpu, size_class));
  return hdr.IsLocked() ? 0 : hdr.current - hdr.begin;
}

template <size_t NumClasses>
inline size_t TcmallocSlab<NumClasses>::Capacity(int cpu,
                                                 size_t size_class) const {
  const auto [slabs, shift] = GetSlabsAndShift(std::memory_order_relaxed);
  Header hdr = LoadHeader(GetHeader(slabs, shift, cpu, size_class));
  return hdr.IsLocked() ? 0 : hdr.end - hdr.begin;
}

template <size_t NumClasses>
inline size_t TcmallocSlab<NumClasses>::Grow(
    int cpu, size_t size_class, size_t len,
    absl::FunctionRef<size_t(uint8_t)> max_capacity) {
  const auto [slabs, shift] = GetSlabsAndShift(std::memory_order_relaxed);
  const size_t max_cap = max_capacity(ToUint8(shift));
  const size_t virtual_cpu_id_offset = virtual_cpu_id_offset_;
  std::atomic<int64_t>* hdrp = GetHeader(slabs, shift, cpu, size_class);
  for (;;) {
    Header old = LoadHeader(hdrp);
    if (old.IsLocked() || old.end - old.begin == max_cap) {
      return 0;
    }
    uint16_t n = std::min<uint16_t>(len, max_cap - (old.end - old.begin));
    Header hdr = old;
    hdr.end += n;
    hdr.end_copy += n;
    const int ret =
        CompareAndSwapHeader(cpu, hdrp, old, hdr, virtual_cpu_id_offset);
    if (ret == cpu) {
      return n;
    } else if (ret >= 0) {
      return 0;
    }
  }
}

template <size_t NumClasses>
inline size_t TcmallocSlab<NumClasses>::Shrink(int cpu, size_t size_class,
                                               size_t len) {
  const auto [slabs, shift] = GetSlabsAndShift(std::memory_order_relaxed);
  const size_t virtual_cpu_id_offset = virtual_cpu_id_offset_;
  std::atomic<int64_t>* hdrp = GetHeader(slabs, shift, cpu, size_class);
  for (;;) {
    Header old = LoadHeader(hdrp);
    if (old.IsLocked() || old.current == old.end) {
      return 0;
    }
    uint16_t n = std::min<uint16_t>(len, old.end - old.current);
    Header hdr = old;
    hdr.end -= n;
    hdr.end_copy -= n;
    const int ret =
        CompareAndSwapHeader(cpu, hdrp, old, hdr, virtual_cpu_id_offset);
    if (ret == cpu) {
      return n;
    } else if (ret >= 0) {
      return 0;
    }
  }
}

#if defined(__x86_64__)
#define TCMALLOC_RSEQ_RELOC_TYPE "R_X86_64_NONE"
#define TCMALLOC_RSEQ_JUMP "jmp"
#define TCMALLOC_RSEQ_SET_CS(name) \
  "lea __rseq_cs_" #name           \
  "_%=(%%rip), %[scratch]\n"       \
  "mov %[scratch], " TCMALLOC_RSEQ_TLS_ADDR(rseq_cs_offset) "\n"
#if defined(__PIC__) && !defined(__PIE__)
// With -fPIC we assume the asm block has [rseq_abi] input.
// This input is passed by TCMALLOC_RSEQ_INPUTS macro.
#define TCMALLOC_RSEQ_TLS_ADDR(off) "%c[" #off "](%[rseq_abi])"
#define TCMALLOC_RSEQ_ABI_INPUT [rseq_abi] "r"(&__rseq_abi),
#else
// Without -fPIC access __rseq_abi directly since it's faster.
#define TCMALLOC_RSEQ_TLS_ADDR(off) "%%fs:__rseq_abi@TPOFF + %c[" #off "]"
#define TCMALLOC_RSEQ_ABI_INPUT
#endif

#elif defined(__aarch64__)
#define TCMALLOC_RSEQ_RELOC_TYPE "R_AARCH64_NONE"
#define TCMALLOC_RSEQ_JUMP "b"
#define TCMALLOC_RSEQ_SET_CS(name)                     \
  "adrp %[scratch], __rseq_cs_" #name                  \
  "_%=\n"                                              \
  "add %[scratch], %[scratch], :lo12:__rseq_cs_" #name \
  "_%=\n"                                              \
  "str %[scratch], [%[rseq_abi], %c[rseq_cs_offset]]\n"
#define TCMALLOC_RSEQ_TLS_ADDR(off) "[%[rseq_abi], %c[" #off "]]"
#define TCMALLOC_RSEQ_ABI_INPUT [rseq_abi] "r"(&__rseq_abi),
#endif

#if !defined(__clang_major__) || __clang_major__ >= 9
#define TCMALLOC_RSEQ_RELOC ".reloc 0, " TCMALLOC_RSEQ_RELOC_TYPE ", 1f\n"
#else
#define TCMALLOC_RSEQ_RELOC
#endif

// Common rseq asm prologue.
// It uses labels 1-4 and assumes the critical section ends with label 5.
// The prologue assumes there is [scratch] input with a scratch register.
#define TCMALLOC_RSEQ_PROLOGUE(name)                                          \
  /* __rseq_cs only needs to be writeable to allow for relocations.*/         \
  ".pushsection __rseq_cs, \"aw?\"\n"                                         \
  ".balign 32\n"                                                              \
  ".local __rseq_cs_" #name                                                   \
  "_%=\n"                                                                     \
  ".type __rseq_cs_" #name                                                    \
  "_%=,@object\n"                                                             \
  ".size __rseq_cs_" #name                                                    \
  "_%=,32\n"                                                                  \
  "__rseq_cs_" #name                                                          \
  "_%=:\n"                                                                    \
  ".long 0x0\n"                                                               \
  ".long 0x0\n"                                                               \
  ".quad 4f\n"                                                                \
  ".quad 5f - 4f\n"                                                           \
  ".quad 2f\n"                                                                \
  ".popsection\n" TCMALLOC_RSEQ_RELOC                                         \
  ".pushsection __rseq_cs_ptr_array, \"aw?\"\n"                               \
  "1:\n"                                                                      \
  ".balign 8\n"                                                               \
  ".quad __rseq_cs_" #name                                                    \
  "_%=\n" /* Force this section to be retained.                               \
             It is for debugging, but is otherwise not referenced. */         \
  ".popsection\n"                                                             \
  ".pushsection .text.unlikely, \"ax?\"\n" /* This is part of the upstream    \
                                              rseq ABI.  The 4 bytes prior to \
                                              the abort IP must match         \
                                              TCMALLOC_PERCPU_RSEQ_SIGNATURE  \
                                              (as configured by our rseq      \
                                              syscall's signature parameter). \
                                              This signature is used to       \
                                              annotate valid abort IPs (since \
                                              rseq_cs could live in a         \
                                              user-writable segment). */      \
  ".long %c[rseq_sig]\n"                                                      \
  ".local " #name                                                             \
  "_trampoline_%=\n"                                                          \
  ".type " #name                                                              \
  "_trampoline_%=,@function\n"                                                \
  "" #name                                                                    \
  "_trampoline_%=:\n"                                                         \
  "2:\n" TCMALLOC_RSEQ_JUMP                                                   \
  " 3f\n"                                                                     \
  ".size " #name "_trampoline_%=, . - " #name                                 \
  "_trampoline_%=\n"                                                          \
  ".popsection\n"                   /* Prepare */                             \
  "3:\n" TCMALLOC_RSEQ_SET_CS(name) /* Start */                               \
      "4:\n"

#define TCMALLOC_RSEQ_INPUTS                                                 \
  TCMALLOC_RSEQ_ABI_INPUT                                                    \
  [rseq_cs_offset] "n"(offsetof(kernel_rseq, rseq_cs)),                      \
      [rseq_sig] "n"(                                                        \
          TCMALLOC_PERCPU_RSEQ_SIGNATURE), /* Also pass common consts, there \
                                              is no cost to passing unused   \
                                              consts. */                     \
      [rseq_slabs_offset] "n"(TCMALLOC_RSEQ_SLABS_OFFSET),                   \
      [cached_slabs_bit] "n"(TCMALLOC_CACHED_SLABS_BIT),                     \
      [cached_slabs_mask_neg] "n"(~TCMALLOC_CACHED_SLABS_MASK)

#if TCMALLOC_INTERNAL_PERCPU_USE_RSEQ && defined(__x86_64__)
static inline ABSL_ATTRIBUTE_ALWAYS_INLINE bool TcmallocSlab_Internal_Push(
    size_t size_class, void* item) {
  uintptr_t scratch, current;
#if TCMALLOC_INTERNAL_PERCPU_USE_RSEQ_ASM_GOTO_OUTPUT
  asm goto(
#else
  bool overflow;
  asm volatile(
#endif
      TCMALLOC_RSEQ_PROLOGUE(TcmallocSlab_Internal_Push)
  // scratch = tcmalloc_slabs;
      "movq " TCMALLOC_RSEQ_TLS_ADDR(rseq_slabs_offset) ", %[scratch]\n"
      // if (scratch & TCMALLOC_CACHED_SLABS_MASK>) goto overflow_label;
      // scratch &= ~TCMALLOC_CACHED_SLABS_MASK;
      "btrq $%c[cached_slabs_bit], %[scratch]\n"
#if TCMALLOC_INTERNAL_PERCPU_USE_RSEQ_ASM_GOTO_OUTPUT
      "jnc %l[overflow_label]\n"
#else
      "jae 5f\n"  // ae==c
#endif
      // current = slabs->current;
      "movzwq (%[scratch], %[size_class], 8), %[current]\n"
      // if (ABSL_PREDICT_FALSE(current >= slabs->end)) { goto overflow_label; }
      "cmp 6(%[scratch], %[size_class], 8), %w[current]\n"
#if TCMALLOC_INTERNAL_PERCPU_USE_RSEQ_ASM_GOTO_OUTPUT
      "jae %l[overflow_label]\n"
#else
      "jae 5f\n"
  // Important! code below this must not affect any flags (i.e.: ccae)
  // If so, the above code needs to explicitly set a ccae return value.
#endif
      "mov %[item], (%[scratch], %[current], 8)\n"
      "lea 1(%[current]), %[current]\n"
      "mov %w[current], (%[scratch], %[size_class], 8)\n"
      // Commit
      "5:\n"
      :
#if !TCMALLOC_INTERNAL_PERCPU_USE_RSEQ_ASM_GOTO_OUTPUT
      [overflow] "=@ccae"(overflow),
#endif
      [scratch] "=&r"(scratch), [current] "=&r"(current)
      : TCMALLOC_RSEQ_INPUTS,
        [size_class] "r"(size_class), [item] "r"(item)
      : "cc", "memory"
#if TCMALLOC_INTERNAL_PERCPU_USE_RSEQ_ASM_GOTO_OUTPUT
      : overflow_label
#endif
  );
#if !TCMALLOC_INTERNAL_PERCPU_USE_RSEQ_ASM_GOTO_OUTPUT
  if (ABSL_PREDICT_FALSE(overflow)) {
    return false;
  }
  return true;
#else
  return true;
overflow_label:
  return false;
#endif
}
#endif  // defined(__x86_64__)

#if TCMALLOC_INTERNAL_PERCPU_USE_RSEQ && defined(__aarch64__)
static inline ABSL_ATTRIBUTE_ALWAYS_INLINE bool TcmallocSlab_Internal_Push(
    size_t size_class, void* item) {
  uintptr_t region_start, scratch, end_ptr, end;
  // Multiply size_class by the bytesize of each header
  size_t size_class_lsl3 = size_class * 8;
#if TCMALLOC_INTERNAL_PERCPU_USE_RSEQ_ASM_GOTO_OUTPUT
  asm goto(
#else
  bool overflow;
  asm volatile(
#endif
       TCMALLOC_RSEQ_PROLOGUE(TcmallocSlab_Internal_Push)
      // region_start = tcmalloc_slabs;
      "ldr %[region_start], " TCMALLOC_RSEQ_TLS_ADDR(rseq_slabs_offset) "\n"
  // if (region_start & TCMALLOC_CACHED_SLABS_MASK) goto overflow_label;
  // region_start &= ~TCMALLOC_CACHED_SLABS_MASK;
#if TCMALLOC_INTERNAL_PERCPU_USE_RSEQ_ASM_GOTO_OUTPUT
      "tbz %[region_start], #%c[cached_slabs_bit], %l[overflow_label]\n"
      "and %[region_start], %[region_start], #%c[cached_slabs_mask_neg]\n"
#else
      "subs %[region_start], %[region_start], %[cached_slabs_mask]\n"
      "b.ls 5f\n"
#endif
      // end_ptr = &(slab_headers[0]->end)
      "add %[end_ptr], %[region_start], #6\n"
      // scratch = slab_headers[size_class]->current (current index)
      "ldrh %w[scratch], [%[region_start], %[size_class_lsl3]]\n"
      // end = slab_headers[size_class]->end (end index)
      "ldrh %w[end], [%[end_ptr], %[size_class_lsl3]]\n"
      // if (ABSL_PREDICT_FALSE(end <= scratch)) { goto overflow_label; }
      "cmp %[end], %[scratch]\n"
#if TCMALLOC_INTERNAL_PERCPU_USE_RSEQ_ASM_GOTO_OUTPUT
      "b.ls %l[overflow_label]\n"
#else
      "b.ls 5f\n"
  // Important! code below this must not affect any flags (i.e.: ccls)
  // If so, the above code needs to explicitly set a ccls return value.
#endif
      "str %[item], [%[region_start], %[scratch], LSL #3]\n"
      "add %w[scratch], %w[scratch], #1\n"
      "strh %w[scratch], [%[region_start], %[size_class_lsl3]]\n"
      // Commit
      "5:\n"
      : [end_ptr] "=&r"(end_ptr), [scratch] "=&r"(scratch), [end] "=&r"(end),
#if !TCMALLOC_INTERNAL_PERCPU_USE_RSEQ_ASM_GOTO_OUTPUT
        [overflow] "=@ccls"(overflow),
#endif
        [region_start] "=&r"(region_start)
      : TCMALLOC_RSEQ_INPUTS,
#if !TCMALLOC_INTERNAL_PERCPU_USE_RSEQ_ASM_GOTO_OUTPUT
        [cached_slabs_mask] "r"(TCMALLOC_CACHED_SLABS_MASK),
#endif
        [size_class_lsl3] "r"(size_class_lsl3), [item] "r"(item)
      // Add x16 and x17 as an explicit clobber registers:
      // The RSEQ code above uses non-local branches in the restart sequence
      // which is located inside .text.unlikely. The maximum distance of B
      // and BL branches in ARM is limited to 128MB. If the linker detects
      // the distance being too large, it injects a thunk which may clobber
      // the x16 or x17 register according to the ARMv8 ABI standard.
      : "x16", "x17", "cc", "memory"
#if TCMALLOC_INTERNAL_PERCPU_USE_RSEQ_ASM_GOTO_OUTPUT
      : overflow_label
#endif
  );
#if !TCMALLOC_INTERNAL_PERCPU_USE_RSEQ_ASM_GOTO_OUTPUT
  if (ABSL_PREDICT_FALSE(overflow)) {
    goto overflow_label;
  }
#endif
  return true;
overflow_label:
  return false;
}
#endif  // defined (__aarch64__)

template <size_t NumClasses>
inline ABSL_ATTRIBUTE_ALWAYS_INLINE bool TcmallocSlab<NumClasses>::Push(
    size_t size_class, void* item, OverflowHandler overflow_handler,
    void* arg) {
  ASSERT(IsFastNoInit());
  ASSERT(item != nullptr);
  // Speculatively annotate item as released to TSan.  We may not succeed in
  // pushing the item, but if we wait for the restartable sequence to succeed,
  // it may become visible to another thread before we can trigger the
  // annotation.
  TSANRelease(item);
#if TCMALLOC_INTERNAL_PERCPU_USE_RSEQ
  if (ABSL_PREDICT_TRUE(TcmallocSlab_Internal_Push(size_class, item))) {
    return true;
  }
  return PushSlow(size_class, item, overflow_handler, arg);
#else
  Crash(kCrash, __FILE__, __LINE__,
        "RSEQ Push called on unsupported platform.");
#endif
}

template <size_t NumClasses>
ABSL_ATTRIBUTE_NOINLINE bool TcmallocSlab<NumClasses>::PushSlow(
    size_t size_class, void* item, OverflowHandler overflow_handler,
    void* arg) {
  int cpu = CacheCpuSlab();
  if (cpu < 0) {
    return Push(size_class, item, overflow_handler, arg);
  }
  return overflow_handler(cpu, size_class, item, arg) >= 0;
}

// PrefetchNextObject provides a common code path across architectures for
// generating a prefetch of the next object.
//
// It is in a distinct, always-lined method to make its cost more transparent
// when profiling with debug information.
inline ABSL_ATTRIBUTE_ALWAYS_INLINE void PrefetchNextObject(
    void* prefetch_target) {
  // A note about prefetcht0 in Pop:  While this prefetch may appear costly,
  // trace analysis shows the target is frequently used (b/70294962). Stalling
  // on a TLB miss at the prefetch site (which has no deps) and prefetching the
  // line async is better than stalling at the use (which may have deps) to fill
  // the TLB and the cache miss.
  //
  // See "Beyond malloc efficiency to fleet efficiency"
  // (https://research.google/pubs/pub50370/), section 6.4 for additional
  // details.
  //
  // TODO(b/214608320): Evaluate prefetch for write.
  __builtin_prefetch(prefetch_target, 0, 3);
}

#if TCMALLOC_INTERNAL_PERCPU_USE_RSEQ && defined(__x86_64__)
template <size_t NumClasses>
template <typename Policy>
inline ABSL_ATTRIBUTE_ALWAYS_INLINE auto TcmallocSlab<NumClasses>::Pop(
    size_t size_class,
    UnderflowHandler<typename Policy::pointer_type> underflow_handler,
    void* arg) {
  ASSERT(IsFastNoInit());

  void* next;
  void* result;
  void* scratch;
  uintptr_t current;

#if TCMALLOC_INTERNAL_PERCPU_USE_RSEQ_ASM_GOTO_OUTPUT
  asm goto(
#else
  bool underflow;
  asm(
#endif
         TCMALLOC_RSEQ_PROLOGUE(TcmallocSlab_Internal_Pop)
        // scratch = tcmalloc_slabs;
        "movq " TCMALLOC_RSEQ_TLS_ADDR(rseq_slabs_offset) ", %[scratch]\n"
  // if (scratch & TCMALLOC_CACHED_SLABS_MASK) goto overflow_label;
  // scratch &= ~TCMALLOC_CACHED_SLABS_MASK;
#if TCMALLOC_INTERNAL_PERCPU_USE_RSEQ_ASM_GOTO_OUTPUT
          "btrq $%c[cached_slabs_bit], %[scratch]\n"
          "jnc %l[underflow_path]\n"
#else
          "cmpq %[cached_slabs_mask], %[scratch]\n"
          "jbe 5f\n"
          "subq %[cached_slabs_mask], %[scratch]\n"
#endif
          // current = scratch->header[size_class].current;
          "movzwq (%[scratch], %[size_class], 8), %[current]\n"
          // if (ABSL_PREDICT_FALSE(current <=
          //                        scratch->header[size_class].begin))
          //   goto underflow_path;
          "cmp 4(%[scratch], %[size_class], 8), %w[current]\n"
#if TCMALLOC_INTERNAL_PERCPU_USE_RSEQ_ASM_GOTO_OUTPUT
          "jbe %l[underflow_path]\n"
#else
          "jbe 5f\n"
  // Important! code below this must not affect any flags (i.e.: ccbe)
  // If so, the above code needs to explicitly set a ccbe return value.
#endif
          "lea -1(%[current]), %[current]\n"
          "movq -8(%[scratch], %[current], 8), %[next]\n"
          "movq (%[scratch], %[current], 8), %[result]\n"
          "mov %w[current], (%[scratch], %[size_class], 8)\n"
          // Commit
          "5:\n"
          : [result] "=&r"(result),
#if !TCMALLOC_INTERNAL_PERCPU_USE_RSEQ_ASM_GOTO_OUTPUT
            [underflow] "=@ccbe"(underflow),
#endif
            [scratch] "=&r"(scratch), [current] "=&r"(current),
            [next] "=&r"(next)
          : TCMALLOC_RSEQ_INPUTS,
#if !TCMALLOC_INTERNAL_PERCPU_USE_RSEQ_ASM_GOTO_OUTPUT
            [cached_slabs_mask] "r"(TCMALLOC_CACHED_SLABS_MASK),
#endif
            [size_class] "r"(size_class)
          : "cc", "memory"
#if TCMALLOC_INTERNAL_PERCPU_USE_RSEQ_ASM_GOTO_OUTPUT
          : underflow_path
#endif
      );
#if !TCMALLOC_INTERNAL_PERCPU_USE_RSEQ_ASM_GOTO_OUTPUT
  if (ABSL_PREDICT_FALSE(underflow)) {
    goto underflow_path;
  }
#endif
  ASSERT(next);
  ASSERT(result);
  TSANAcquire(result);

  PrefetchNextObject(next);
  return Policy::to_pointer(result, size_class);
underflow_path:
  return PopSlow<Policy>(size_class, underflow_handler, arg);
}
#endif  // defined(__x86_64__)

#if TCMALLOC_INTERNAL_PERCPU_USE_RSEQ && defined(__aarch64__)
template <size_t NumClasses>
template <typename Policy>
inline ABSL_ATTRIBUTE_ALWAYS_INLINE auto TcmallocSlab<NumClasses>::Pop(
    size_t size_class,
    UnderflowHandler<typename Policy::pointer_type> underflow_handler,
    void* arg) {
  ASSERT(IsFastNoInit());

  void* result;
  void* region_start;
  void* prefetch;
  uintptr_t scratch;
  uintptr_t previous;
  uintptr_t begin;
#if TCMALLOC_INTERNAL_PERCPU_USE_RSEQ_ASM_GOTO_OUTPUT
  asm goto(
#else
  bool underflow;
  asm(
#endif
         TCMALLOC_RSEQ_PROLOGUE(TcmallocSlab_Internal_Pop)
          // region_start = tcmalloc_slabs;
          "ldr %[region_start], " TCMALLOC_RSEQ_TLS_ADDR(rseq_slabs_offset) "\n"
  // if (region_start & TCMALLOC_CACHED_SLABS_MASK) goto overflow_label;
  // region_start &= ~TCMALLOC_CACHED_SLABS_MASK;
#if TCMALLOC_INTERNAL_PERCPU_USE_RSEQ_ASM_GOTO_OUTPUT
          "tbz %[region_start], #%c[cached_slabs_bit], %l[underflow_path]\n"
          "and %[region_start], %[region_start], #%c[cached_slabs_mask_neg]\n"
#else
          "subs %[region_start], %[region_start], %[cached_slabs_mask]\n"
          "b.ls 5f\n"
#endif
          // scratch = slab_headers[size_class]->current (current index)
          "ldrh %w[scratch], [%[region_start], %[size_class_lsl3]]\n"
          // begin = slab_headers[size_class]->begin (begin index)
          // Temporarily use begin as scratch.
          "add %[begin], %[size_class_lsl3], #4\n"
          "ldrh %w[begin], [%[region_start], %[begin]]\n"
          // if (ABSL_PREDICT_FALSE(begin >= scratch)) { goto underflow_path; }
          "cmp %w[scratch], %w[begin]\n"
#if TCMALLOC_INTERNAL_PERCPU_USE_RSEQ_ASM_GOTO_OUTPUT
          "b.ls %l[underflow_path]\n"
#else
          "b.ls 5f\n"
  // Important! code below this must not affect any flags (i.e.: ccls)
  // If so, the above code needs to explicitly set a ccls return value.
#endif
          // scratch--
          "sub %w[scratch], %w[scratch], #1\n"
          "ldr %[result], [%[region_start], %[scratch], LSL #3]\n"
          "sub %w[previous], %w[scratch], #1\n"
          "ldr %[prefetch], [%[region_start], %[previous], LSL #3]\n"
          "strh %w[scratch], [%[region_start], %[size_class_lsl3]]\n"
          // Commit
          "5:\n"
          :
#if !TCMALLOC_INTERNAL_PERCPU_USE_RSEQ_ASM_GOTO_OUTPUT
          [underflow] "=@ccls"(underflow),
#endif
          [result] "=&r"(result), [prefetch] "=&r"(prefetch),
          // Temps
          [region_start] "=&r"(region_start), [previous] "=&r"(previous),
          [begin] "=&r"(begin), [scratch] "=&r"(scratch)
          // Real inputs
          : TCMALLOC_RSEQ_INPUTS,
#if !TCMALLOC_INTERNAL_PERCPU_USE_RSEQ_ASM_GOTO_OUTPUT
            [cached_slabs_mask] "r"(TCMALLOC_CACHED_SLABS_MASK),
#endif
            [size_class] "r"(size_class),
            [size_class_lsl3] "r"(size_class << 3)
          // Add x16 and x17 as an explicit clobber registers:
          // The RSEQ code above uses non-local branches in the restart sequence
          // which is located inside .text.unlikely. The maximum distance of B
          // and BL branches in ARM is limited to 128MB. If the linker detects
          // the distance being too large, it injects a thunk which may clobber
          // the x16 or x17 register according to the ARMv8 ABI standard.
          : "x16", "x17", "cc", "memory"
#if TCMALLOC_INTERNAL_PERCPU_USE_RSEQ_ASM_GOTO_OUTPUT
          : underflow_path
#endif
      );
#if !TCMALLOC_INTERNAL_PERCPU_USE_RSEQ_ASM_GOTO_OUTPUT
  if (ABSL_PREDICT_FALSE(underflow)) {
    goto underflow_path;
  }
#endif
  TSANAcquire(result);
  PrefetchNextObject(prefetch);
  return Policy::to_pointer(result, size_class);
underflow_path:
  return PopSlow<Policy>(size_class, underflow_handler, arg);
}
#endif  // defined(__aarch64__)

#if !TCMALLOC_INTERNAL_PERCPU_USE_RSEQ
template <size_t NumClasses>
template <typename Policy>
inline ABSL_ATTRIBUTE_ALWAYS_INLINE auto TcmallocSlab<NumClasses>::Pop(
    size_t size_class,
    UnderflowHandler<typename Policy::pointer_type> underflow_handler,
    void* arg) {
  Crash(kCrash, __FILE__, __LINE__, "RSEQ Pop called on unsupported platform.");
  return Policy::to_pointer(nullptr, 0);
}
#endif

template <size_t NumClasses>
template <typename Policy>
ABSL_ATTRIBUTE_NOINLINE auto TcmallocSlab<NumClasses>::PopSlow(
    size_t size_class,
    UnderflowHandler<typename Policy::pointer_type> underflow_handler,
    void* arg) {
  int cpu = CacheCpuSlab();
  if (cpu < 0) {
    return Pop<Policy>(size_class, underflow_handler, arg);
  }
  return underflow_handler(cpu, size_class, arg);
}

template <size_t NumClasses>
int TcmallocSlab<NumClasses>::CacheCpuSlab() {
  int cpu = VirtualRseqCpuId(virtual_cpu_id_offset_);
  ASSERT(cpu >= 0);
#if TCMALLOC_INTERNAL_PERCPU_USE_RSEQ
  if (ABSL_PREDICT_FALSE((tcmalloc_slabs & TCMALLOC_CACHED_SLABS_MASK) == 0)) {
    return CacheCpuSlabSlow();
  }
  // We already have slab offset cached, so the slab is indeed full/empty
  // and we need to call overflow/underflow handler.
#endif
  return cpu;
}

#if TCMALLOC_INTERNAL_PERCPU_USE_RSEQ
template <size_t NumClasses>
ABSL_ATTRIBUTE_NOINLINE int TcmallocSlab<NumClasses>::CacheCpuSlabSlow() {
  int cpu = VirtualRseqCpuId(virtual_cpu_id_offset_);
  for (;;) {
    intptr_t val = tcmalloc_slabs;
    ASSERT(!(val & TCMALLOC_CACHED_SLABS_MASK));
    const auto [slabs, shift] = GetSlabsAndShift(std::memory_order_relaxed);
    Slabs* start = CpuMemoryStart(slabs, shift, cpu);
    intptr_t new_val =
        reinterpret_cast<uintptr_t>(start) | TCMALLOC_CACHED_SLABS_MASK;
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Waddress-of-packed-member"
    auto* ptr = reinterpret_cast<std::atomic<intptr_t>*>(
        const_cast<uintptr_t*>(&tcmalloc_slabs));
#pragma GCC diagnostic pop
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
    return cpu;
  }
  return -1;
}
#endif

template <size_t NumClasses>
inline size_t TcmallocSlab<NumClasses>::PushBatch(size_t size_class,
                                                  void** batch, size_t len) {
  ASSERT(len != 0);
  // We need to annotate batch[...] as released before running the restartable
  // sequence, since those objects become visible to other threads the moment
  // the restartable sequence is complete and before the annotation potentially
  // runs.
  //
  // This oversynchronizes slightly, since PushBatch may succeed only partially.
  TSANReleaseBatch(batch, len);
  return TcmallocSlab_Internal_PushBatch(size_class, batch, len);
}

template <size_t NumClasses>
inline size_t TcmallocSlab<NumClasses>::PopBatch(size_t size_class,
                                                 void** batch, size_t len) {
  ASSERT(len != 0);
  const size_t n = TcmallocSlab_Internal_PopBatch(size_class, batch, len);
  ASSERT(n <= len);

  // PopBatch is implemented in assembly, msan does not know that the returned
  // batch is initialized.
  ANNOTATE_MEMORY_IS_INITIALIZED(batch, n * sizeof(batch[0]));
  TSANAcquireBatch(batch, n);
  return n;
}

template <size_t NumClasses>
inline auto TcmallocSlab<NumClasses>::CpuMemoryStart(Slabs* slabs, Shift shift,
                                                     int cpu) -> Slabs* {
  char* const bytes = reinterpret_cast<char*>(slabs);
  return reinterpret_cast<Slabs*>(&bytes[cpu << ToUint8(shift)]);
}

template <size_t NumClasses>
inline std::atomic<int64_t>* TcmallocSlab<NumClasses>::GetHeader(
    Slabs* slabs, Shift shift, int cpu, size_t size_class) {
  return &CpuMemoryStart(slabs, shift, cpu)->header[size_class];
}

template <size_t NumClasses>
inline auto TcmallocSlab<NumClasses>::LoadHeader(std::atomic<int64_t>* hdrp)
    -> Header {
  return absl::bit_cast<Header>(hdrp->load(std::memory_order_relaxed));
}

template <size_t NumClasses>
inline void TcmallocSlab<NumClasses>::StoreHeader(std::atomic<int64_t>* hdrp,
                                                  Header hdr) {
  hdrp->store(absl::bit_cast<int64_t>(hdr), std::memory_order_relaxed);
}

template <size_t NumClasses>
inline void TcmallocSlab<NumClasses>::LockHeader(Slabs* slabs, Shift shift,
                                                 int cpu, size_t size_class) {
  // Note: this reinterpret_cast and write in Lock lead to undefined
  // behavior, because the actual object type is std::atomic<int64_t>. But
  // C++ does not allow to legally express what we need here: atomic writes
  // of different sizes.
  reinterpret_cast<Header*>(GetHeader(slabs, shift, cpu, size_class))->Lock();
}

template <size_t NumClasses>
inline int TcmallocSlab<NumClasses>::CompareAndSwapHeader(
    int cpu, std::atomic<int64_t>* hdrp, Header old, Header hdr,
    const size_t virtual_cpu_id_offset) {
  const int64_t old_raw = absl::bit_cast<int64_t>(old);
  const int64_t new_raw = absl::bit_cast<int64_t>(hdr);
  return CompareAndSwapUnsafe(cpu, hdrp, static_cast<intptr_t>(old_raw),
                              static_cast<intptr_t>(new_raw),
                              virtual_cpu_id_offset);
}

template <size_t NumClasses>
inline void TcmallocSlab<NumClasses>::DrainCpu(Slabs* slabs, Shift shift,
                                               int cpu, uint16_t* begins,
                                               DrainHandler drain_handler) {
  for (size_t size_class = 0; size_class < NumClasses; ++size_class) {
    Header header = LoadHeader(GetHeader(slabs, shift, cpu, size_class));
    const size_t size = header.current - begins[size_class];
    const size_t cap = header.end_copy - begins[size_class];
    void** batch = reinterpret_cast<void**>(GetHeader(slabs, shift, cpu, 0) +
                                            begins[size_class]);
    TSANAcquireBatch(batch, size);
    drain_handler(cpu, size_class, batch, size, cap);
  }
}

template <size_t NumClasses>
inline void TcmallocSlab<NumClasses>::StopConcurrentMutations(
    Slabs* slabs, Shift shift, int cpu, size_t virtual_cpu_id_offset) {
  for (bool done = false; !done;) {
    for (size_t size_class = 0; size_class < NumClasses; ++size_class) {
      LockHeader(slabs, shift, cpu, size_class);
    }
    FenceCpu(cpu, virtual_cpu_id_offset);
    done = true;
    for (size_t size_class = 0; size_class < NumClasses; ++size_class) {
      Header hdr = LoadHeader(GetHeader(slabs, shift, cpu, size_class));
      if (!hdr.IsLocked()) {
        // Header was overwritten by Grow/Shrink. Retry.
        done = false;
        break;
      }
    }
  }
}

template <size_t NumClasses>
inline bool TcmallocSlab<NumClasses>::Header::IsLocked() const {
  ASSERT(end != 0 || begin == 0 || begin == 0xffffu);
  // Checking end == 0 also covers the case of MADV_DONTNEEDed slabs after
  // a call to ResizeSlabs(). Such slabs are locked for any practical purposes.
  return end == 0;
}

template <size_t NumClasses>
inline void TcmallocSlab<NumClasses>::Header::Lock() {
  // Write 0xffff to begin and 0 to end. This blocks new Push'es and Pop's.
  // Note: we write only 4 bytes. The first 4 bytes are left intact.
  // See Drain method for details. tl;dr: C++ does not allow us to legally
  // express this without undefined behavior.
  std::atomic<int32_t>* p =
      reinterpret_cast<std::atomic<int32_t>*>(&lock_update);
  Header hdr;
  hdr.begin = 0xffffu;
  hdr.end = 0;
  p->store(absl::bit_cast<int32_t>(hdr.lock_update), std::memory_order_relaxed);
}

template <size_t NumClasses>
void TcmallocSlab<NumClasses>::Init(Slabs* slabs,
                                    absl::FunctionRef<size_t(size_t)> capacity,
                                    Shift shift) {
  if (UsingFlatVirtualCpus()) {
    virtual_cpu_id_offset_ = offsetof(kernel_rseq, vcpu_id);
  }

#if TCMALLOC_INTERNAL_PERCPU_USE_RSEQ
  // This is needed only for tests that create/destroy slabs,
  // w/o this cpu_id_start may contain wrong offset for a new slab.
  __rseq_abi.cpu_id_start = 0;
#endif
  slabs_and_shift_.store({slabs, shift}, std::memory_order_relaxed);
  const int num_cpus = NumCPUs();
  for (int cpu = 0; cpu < num_cpus; ++cpu) {
    Slabs* curr_slab = CpuMemoryStart(slabs, shift, cpu);
    void** elems = curr_slab->mem;

    for (size_t size_class = 0; size_class < NumClasses; ++size_class) {
      size_t cap = capacity(size_class);
      CHECK_CONDITION(static_cast<uint16_t>(cap) == cap);

      if (cap == 0) {
        continue;
      }

      // One extra element for prefetch
      const size_t num_pointers = cap + 1;
      elems += num_pointers;
      const size_t bytes_used_on_curr_slab =
          reinterpret_cast<char*>(elems) - reinterpret_cast<char*>(curr_slab);
      if (bytes_used_on_curr_slab > (1 << ToUint8(shift))) {
        Crash(kCrash, __FILE__, __LINE__, "per-CPU memory exceeded, have ",
              1 << ToUint8(shift), " need ", bytes_used_on_curr_slab);
      }
    }
  }
}

template <size_t NumClasses>
void TcmallocSlab<NumClasses>::InitCpu(
    int cpu, absl::FunctionRef<size_t(size_t)> capacity) {
  const auto [slabs, shift] = GetSlabsAndShift(std::memory_order_relaxed);
  InitCpuImpl(slabs, shift, cpu, virtual_cpu_id_offset_, capacity);
}

template <size_t NumClasses>
void TcmallocSlab<NumClasses>::InitCpuImpl(
    Slabs* slabs, Shift shift, int cpu, size_t virtual_cpu_id_offset,
    absl::FunctionRef<size_t(size_t)> capacity) {
  // Phase 1: stop concurrent mutations for <cpu>. Locking ensures that there
  // exists no value of current such that begin < current.
  StopConcurrentMutations(slabs, shift, cpu, virtual_cpu_id_offset);

  // Phase 2: Initialize prefetch target and compute the offsets for the
  // boundaries of each size class' cache.
  Slabs* curr_slab = CpuMemoryStart(slabs, shift, cpu);
  void** elems = curr_slab->mem;

  uint16_t begin[NumClasses];

  // Number of free pointers is limited by uint16_t sized offsets in slab
  // header, with an additional offset value 0xffff reserved for locking.
  constexpr size_t kMaxAllowedOffset = std::numeric_limits<uint16_t>::max() - 1;
  for (size_t size_class = 0; size_class < NumClasses; ++size_class) {
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
  for (size_t size_class = 0; size_class < NumClasses; ++size_class) {
    std::atomic<int64_t>* hdrp = GetHeader(slabs, shift, cpu, size_class);
    Header hdr = LoadHeader(hdrp);
    hdr.current = begin[size_class];
    StoreHeader(hdrp, hdr);
  }
  FenceCpu(cpu, virtual_cpu_id_offset);

  // Phase 4: Allow access to this cache.
  for (size_t size_class = 0; size_class < NumClasses; ++size_class) {
    Header hdr;
    hdr.current = begin[size_class];
    hdr.begin = begin[size_class];
    hdr.end = begin[size_class];
    hdr.end_copy = begin[size_class];
    StoreHeader(GetHeader(slabs, shift, cpu, size_class), hdr);
  }
}

template <size_t NumClasses>
auto TcmallocSlab<NumClasses>::ResizeSlabs(
    Shift new_shift, Slabs* new_slabs,
    absl::FunctionRef<void*(size_t, std::align_val_t)> alloc,
    absl::FunctionRef<size_t(size_t)> capacity,
    absl::FunctionRef<bool(size_t)> populated, DrainHandler drain_handler)
    -> ResizeSlabsInfo {
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
  // Note: we can't do regular malloc here for resize_begins_ because we may be
  // holding the CpuCache spinlocks. We allocate memory on the arena and keep
  // the pointer for reuse.
  const size_t begins_size = sizeof(uint16_t) * NumClasses * num_cpus;
  if (resize_begins_ == nullptr) {
    resize_begins_ = reinterpret_cast<uint16_t(*)[NumClasses]>(
        alloc(begins_size, std::align_val_t{alignof(uint16_t)}));
  }
  // Setting resizing_ in combination with a fence on every CPU before setting
  // new slabs_and_shift_ prevents Push/Pop fast path from using the old
  // slab offset/shift with the new slabs pointer. After the fence all CPUs
  // will uncache the offset and observe resizing_ on the next attempt
  // to cache the offset.
  CHECK_CONDITION(!resizing_.load(std::memory_order_relaxed));
  resizing_.store(true, std::memory_order_relaxed);
  for (size_t cpu = 0; cpu < num_cpus; ++cpu) {
    if (!populated(cpu)) continue;
    for (size_t size_class = 0; size_class < NumClasses; ++size_class) {
      Header header =
          LoadHeader(GetHeader(old_slabs, old_shift, cpu, size_class));
      CHECK_CONDITION(!header.IsLocked());
      resize_begins_[cpu][size_class] = header.begin;
    }
    StopConcurrentMutations(old_slabs, old_shift, cpu, virtual_cpu_id_offset);
  }

  // Phase 3: Atomically update slabs and shift.
  slabs_and_shift_.store({new_slabs, new_shift}, std::memory_order_relaxed);

  // Phase 4: Return pointers from the old slab to the TransferCache.
  for (size_t cpu = 0; cpu < num_cpus; ++cpu) {
    if (!populated(cpu)) continue;
    DrainCpu(old_slabs, old_shift, cpu, &resize_begins_[cpu][0], drain_handler);
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
    for (size_t size_class = 0; size_class < NumClasses; ++size_class) {
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

template <size_t NumClasses>
void* TcmallocSlab<NumClasses>::Destroy(
    absl::FunctionRef<void(void*, size_t, std::align_val_t)> free) {
  const auto [slabs, shift] = GetSlabsAndShift(std::memory_order_relaxed);
  free(slabs, GetSlabsAllocSize(shift, NumCPUs()), kPhysicalPageAlign);
  slabs_and_shift_.store({nullptr, shift}, std::memory_order_relaxed);
  return slabs;
}

template <size_t NumClasses>
size_t TcmallocSlab<NumClasses>::GrowOtherCache(
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

template <size_t NumClasses>
size_t TcmallocSlab<NumClasses>::ShrinkOtherCache(
    int cpu, size_t size_class, size_t len, ShrinkHandler shrink_handler) {
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

template <size_t NumClasses>
void TcmallocSlab<NumClasses>::Drain(int cpu, DrainHandler drain_handler) {
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
  uint16_t begin[NumClasses];
  for (size_t size_class = 0; size_class < NumClasses; ++size_class) {
    Header hdr = LoadHeader(GetHeader(slabs, shift, cpu, size_class));
    CHECK_CONDITION(!hdr.IsLocked());
    begin[size_class] = hdr.begin;
  }

  // Phase 2: stop concurrent mutations for <cpu>.
  StopConcurrentMutations(slabs, shift, cpu, virtual_cpu_id_offset);

  // Phase 3: execute callbacks.
  DrainCpu(slabs, shift, cpu, begin, drain_handler);

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
  for (size_t size_class = 0; size_class < NumClasses; ++size_class) {
    std::atomic<int64_t>* hdrp = GetHeader(slabs, shift, cpu, size_class);
    Header hdr = LoadHeader(hdrp);
    hdr.current = begin[size_class];
    StoreHeader(hdrp, hdr);
  }

  // Phase 5: fence and reset the remaining fields to beginning of the region.
  // This allows concurrent mutations again.
  FenceCpu(cpu, virtual_cpu_id_offset);
  for (size_t size_class = 0; size_class < NumClasses; ++size_class) {
    std::atomic<int64_t>* hdrp = GetHeader(slabs, shift, cpu, size_class);
    Header hdr;
    hdr.current = begin[size_class];
    hdr.begin = begin[size_class];
    hdr.end = begin[size_class];
    hdr.end_copy = begin[size_class];
    StoreHeader(hdrp, hdr);
  }
}

template <size_t NumClasses>
PerCPUMetadataState TcmallocSlab<NumClasses>::MetadataMemoryUsage() const {
  PerCPUMetadataState result;
  const auto [slabs, shift] = GetSlabsAndShift(std::memory_order_relaxed);
  result.virtual_size = GetSlabsAllocSize(shift, NumCPUs());
  result.resident_size = MInCore::residence(slabs, result.virtual_size);
  return result;
}

}  // namespace percpu
}  // namespace subtle
}  // namespace tcmalloc_internal
}  // namespace tcmalloc
GOOGLE_MALLOC_SECTION_END

#endif  // TCMALLOC_INTERNAL_PERCPU_TCMALLOC_H_
