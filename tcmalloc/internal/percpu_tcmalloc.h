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

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <new>
#include <utility>

#include "absl/base/attributes.h"
#include "absl/base/casts.h"
#include "absl/base/dynamic_annotations.h"
#include "absl/base/optimization.h"
#include "absl/functional/function_ref.h"
#include "absl/numeric/bits.h"
#include "tcmalloc/internal/atomic_danger.h"
#include "tcmalloc/internal/delay_injection.h"
#include "tcmalloc/internal/is_aligned_to.h"
#include "tcmalloc/internal/logging.h"
#include "tcmalloc/internal/mincore.h"
#include "tcmalloc/internal/optimization.h"
#include "tcmalloc/internal/percpu.h"
#include "tcmalloc/internal/prefetch.h"
#include "tcmalloc/internal/sysinfo.h"

#if defined(__GNUC__) && !defined(__clang__) && defined(__x86_64__)
// Work around https://gcc.gnu.org/bugzilla/show_bug.cgi?id=125526
// by force-loading the address of the thread-local rseq_cs_addr into
// a register instead of giving it as a "m" constraint.
//
// TODO: Remove this when GCC releases a fixed version.
#define TCMALLOC_INTERNAL_PERCPU_USE_TLS_WORKAROUND 1
#else
#define TCMALLOC_INTERNAL_PERCPU_USE_TLS_WORKAROUND 0
#endif

GOOGLE_MALLOC_SECTION_BEGIN
namespace tcmalloc {
namespace tcmalloc_internal {

struct PerSizeClassMaxCapacity {
  size_t size_class;
  size_t max_capacity;
};

struct PerCPUMetadataState {
  size_t virtual_size;
  size_t resident_size;
};

struct ResizeSlabsInfo {
  void* old_slabs;
  size_t old_slabs_size;
};

namespace subtle {
namespace percpu {

enum class Shift : uint8_t;
constexpr uint8_t ToUint8(Shift shift) { return static_cast<uint8_t>(shift); }
constexpr Shift ToShiftType(size_t shift) {
  TC_ASSERT_EQ(ToUint8(static_cast<Shift>(shift)), shift);
  return static_cast<Shift>(shift);
}

// The allocation size for the slabs array.
inline size_t GetSlabsAllocSize(Shift shift, int num_cpus) {
  return static_cast<size_t>(num_cpus) << ToUint8(shift);
}

// Since we lazily initialize our slab, we expect it to be mmap'd and not
// resident.  We align it to the slab size so that it is easier to make
// entire hugepages nonresident when we get to clearing out metadata
// from drained CPUs.
//
// For small-but-slow, we instead prefer a small page size (EXEC_PAGESIZE)
// to allocate the slab in the tail of its existing Arena block.
// We still align it a page size so neighboring allocations (from
// TCMalloc's internal arena) do not necessarily cause the metadata to be
// faulted in.
constexpr std::align_val_t SlabAlignment(Shift shift) {
  constexpr std::align_val_t kPhysicalPageAlign{EXEC_PAGESIZE};
#ifdef TCMALLOC_INTERNAL_SMALL_BUT_SLOW
  return kPhysicalPageAlign;
#else
  return std::max(std::align_val_t{size_t{1} << ToUint8(shift)},
                  kPhysicalPageAlign);
#endif
}

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
  // This region is split into NumCPUs regions of a power-of-2 size
  // (32/64/128/256/512k).
  // First NumClasses words of each CPU region are occupied by slab
  // headers (Header struct). The remaining memory contain slab arrays.
  // struct Slabs {
  //  std::atomic<int32_t> header[NumClasses];
  //  void* mem[];
  // };

  constexpr TcmallocSlab() = default;

  // Init must be called before any other methods.
  // <slabs> is memory for the slabs with size corresponding to <shift>.  It
  //     must be zero-filled: offset 0 of every region holds that region's
  //     marker, and 0 is what marks a region as unpopulated.  The same holds
  //     for the buffers handed to ResizeSlabs() and UpdateMaxCapacities().
  //     Memory freshly obtained from the kernel already satisfies this; a
  //     test allocator built on operator new must zero it explicitly.
  // <capacity> callback returns max capacity for size class <size_class>.
  // <shift> indicates the number of bits to shift the CPU ID in order to
  //     obtain the location of the per-CPU slab.
  //
  // Initial capacity is 0 for all slabs.
  void Init(absl::FunctionRef<void*(size_t, std::align_val_t)> alloc,
            void* slabs, absl::FunctionRef<size_t(size_t)> capacity,
            Shift shift);

  void InitSlabs(void* slabs, Shift shift,
                 absl::FunctionRef<size_t(size_t)> capacity);

  // Lazily initializes the slab for a specific cpu.
  // <capacity> callback returns max capacity for size class <size_class>.
  //
  // Prior to InitCpu being called on a particular `cpu`, non-const operations
  // other than Push/Pop/PushBatch/PopBatch are invalid.
  void InitCpu(int cpu, absl::FunctionRef<size_t(size_t)> capacity);

  // Update maximum capacities allocated to each size class.
  // Build and initialize <new_slabs> so as to use new maximum capacities
  // provided by <capacity> callback for the <size_class>.
  // <capacity> should return the new maximum capacity for the given size
  // class, regardless of whether <update_capacity> has been called or not.
  // <update_capacity> updates capacities for the <size_class> with the new
  // <cap> once the slabs are initialized.
  // <populated> returns whether the given cpu's slab is populated. The
  // return value should remain constant for the duration of the call.
  // <new_max_capacity> provides an array of new maximum capacities to be
  // updated for size classes.
  // <classes_to_resize> provides the number of size classes for which the
  // capacity needs to be updated.
  // <drain_handler> callback drains the old slab.
  [[nodiscard]] ResizeSlabsInfo UpdateMaxCapacities(
      void* new_slabs, absl::FunctionRef<size_t(size_t)> capacity,
      absl::FunctionRef<void(int, uint16_t)> update_capacity,
      absl::FunctionRef<bool(size_t)> populated, DrainHandler drain_handler,
      PerSizeClassMaxCapacity* new_max_capacity, int classes_to_resize);

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
  [[nodiscard]] ResizeSlabsInfo ResizeSlabs(
      Shift new_shift, void* new_slabs,
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

  // Add an item (which must be non-zero) to the current CPU's slab. Returns
  // true if add succeeds. Otherwise invokes <overflow_handler> and returns
  // false (assuming that <overflow_handler> returns negative value).
  bool Push(size_t size_class, void* item);

  // Remove an item (LIFO) from the current CPU's slab. If the slab is empty,
  // invokes <underflow_handler> and returns its result.
  [[nodiscard]] void* Pop(size_t size_class);

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

  // Caches the address of the current cpu's slab region in tcmalloc_slabs if
  // the region is not stopped. Returns the current cpu and whether the address
  // was previously uncached and is now cached. If the cpu is stopped, returns
  // {-1, true}.
  std::pair<int, bool> CacheCpuSlab();

  // Uncaches the slab address for the current thread, so that the next
  // Push/Pop operation will return false.
  void UncacheCpuSlab();

  // Synchronization protocol between local and remote operations.
  // This class supports a set of cpu local operations (Push/Pop/
  // PushBatch/PopBatch/Grow), and a set of remote operations that
  // operate on non-current cpu's slab (GrowOtherCache/ShrinkOtherCache/
  // Drain/Resize). Local operations always use a restartable sequence that
  // aborts unless the cached slab region (tcmalloc_slabs) is marked as running
  // for the cpu the thread is executing on.
  //
  // Remote operations mark the region as stopped and then execute a Fence.
  // This ensures that any local operation on the cpu will abort without
  // changing any state, and that any local operation that starts afterwards
  // observes the stopped marker. Threads may keep an address of a stopped (or
  // even of a swapped out) region cached in tcmalloc_slabs indefinitely, which
  // is why old slabs must not be unmapped.
  //
  // This part uses relaxed atomic operations on the marker because the Fence
  // provides all necessary synchronization between remote and local threads.
  // When a remote operation finishes, it marks the region as running using
  // release memory ordering.
  //
  //  * On x86, loads are implicitly acquire, so a local operation that observes
  //    the running marker also observes all side-effects of the remote
  //    operation.
  //  * On aarch64, the fast paths load the marker with a relaxed ldr, so
  //    StartCpu/StartAllCpus issue a Fence before publishing the running marker
  //    instead.
  //
  // StopCpu/StartCpu implement the corresponding parts of the remote
  // synchronization protocol.
  void StopCpu(int cpu);
  void StartCpu(int cpu);

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

  enum HugePageStatus : uint8_t { kNotTouched = 0, kCannotFree, kShouldFree };

  // Find whether enough consecutive CPUs are drained so that their metadata
  // spans an entire hugepage, and if so, release their metadata.
  //
  // All CPUs' ResizeInfo must be locked before calling this function.
  // The function stops them itself.
  void ReleaseSlabMetadataForDrainedCpus(
      absl::FunctionRef<bool(size_t)> populated,
      absl::FunctionRef<void(size_t)> unpopulate,
      absl::FunctionRef<void(void*, size_t)> madvise_away_slabs);

  PerCPUMetadataState MetadataMemoryUsage() const;

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
    SlabsAndShift(const void* slabs, Shift shift)
        : raw_(reinterpret_cast<uintptr_t>(slabs) | ToUint8(shift)) {
      TC_ASSERT_EQ(raw_ & kShiftMask, ToUint8(shift));
      TC_ASSERT_EQ(reinterpret_cast<void*>(raw_ & kSlabsMask), slabs);
    }

    std::pair<void*, Shift> Get() const {
      static_assert(kShiftMask >= 0 && kShiftMask <= UCHAR_MAX,
                    "kShiftMask must fit in a uint8_t");
      // Avoid expanding the width of Shift else the compiler will insert an
      // additional instruction to zero out the upper bits on the critical path
      // of alloc / free.  Not zeroing out the bits is safe because both ARM and
      // x86 only use the lowest byte for shift count in variable shifts.
      return {reinterpret_cast<void*>(raw_ & kSlabsMask),
              static_cast<Shift>(raw_ & kShiftMask)};
    }

    bool operator!=(const SlabsAndShift& other) const {
      return raw_ != other.raw_;
    }

   private:
    uintptr_t raw_;
  };

  // Slab header (packed, atomically updated 32-bit).
  // Current is a pointer offset from the per-CPU region start
  // (in units of sizeof(void*)). The slot array is prefixed
  // with an item that has low bit set and ends at
  // (current+remaining_capacity), and the occupied slots are
  // up to current.
  //
  // If you modify this definition, remember that it is also processed
  // by hand-written assembly code that also needs updating (look for
  // asm statements in this file, as well as in internal/percpu_rseq_*.S).
  struct Header {
    // The end offset of the currently occupied slots.
    // 0xffff is an invalid value, because it would disturb our overflow logic.
    uint16_t current;
    // The amount of elements left we can allocate before hitting our
    // class capacity. May be changed by Grow() or Shrink(), up until
    // storage limits in the slab.
    uint16_t remaining_capacity;

    void set_current(uint16_t val) {
      TC_ASSERT_NE(val, 0xffff);
      current = val;
    }
    uint16_t capacity(uint16_t begin) const {
      if (current < begin) {
        // Uninitialized; special case.
        return 0;
      }
      return (current - begin) + remaining_capacity;
    }
  };

  using AtomicHeader = std::atomic<int32_t>;

  // We cast Header to AtomicHeader.
  static_assert(sizeof(Header) == sizeof(AtomicHeader));

  // We mark the pointer that's stored right before size class object range
  // in the slabs array with this mask. When we reach pointer marked with this
  // mask when popping, we understand that we reached the beginning of the
  // range (the slab is empty). The pointer is also a valid pointer for
  // prefetching, so it allows us to always prefetch the previous element
  // when popping.
  static constexpr uintptr_t kBeginMark = 1;

  // Size class 0 is unused, so its header holds the state of the whole per-CPU
  // region instead. See the TCMALLOC_SLAB_STOPPED comment in percpu.h.
  //
  // TODO(b/550113085): consider moving the marker to include header[1] and
  // shifting the real size class headers by one, so that offset 0 of a region
  // can never be read as (part of) an object pointer by Pop's begin marker
  // check.
  using AtomicMarker = std::atomic<uint32_t>;
  static_assert(sizeof(AtomicMarker) <= sizeof(Header));

  // It's important that we use consistent values for slabs/shift rather than
  // loading from the atomic repeatedly whenever we use one of the values.
  [[nodiscard]] std::pair<void*, Shift> GetSlabsAndShift(
      std::memory_order order) const {
    return slabs_and_shift_.load(order).Get();
  }

  static void* CpuMemoryStart(void* slabs, Shift shift, int cpu);
  static const void* CpuMemoryStart(const void* slabs, Shift shift, int cpu);
  static AtomicHeader* GetHeader(void* slabs, Shift shift, int cpu,
                                 size_t size_class);
  static AtomicMarker& GetCpuMarker(void* cpu_slab);
  static const AtomicMarker& GetCpuMarker(const void* cpu_slab);
  static AtomicMarker& GetCpuMarker(void* slabs, Shift shift, int cpu);
  static const AtomicMarker& GetCpuMarker(const void* slabs, Shift shift,
                                          int cpu);
  // The marker value that cpu's region holds while it is the current, running
  // region of cpu.
  static uint32_t RunningMarker(int cpu);
  static void AssertCpuStopped(const void* slabs, Shift shift, int cpu);
  static void SetCpuStopped(void* slabs, Shift shift, int cpu);
  static void SetCpuRunning(void* slabs, Shift shift, int cpu);
  // <populated> returns whether the given cpu's region is populated. Stopping
  // and starting must skip unpopulated regions: even reading one makes mincore
  // report the page as mapped and breaks resident memory accounting.
  void StopAllCpus(absl::FunctionRef<bool(size_t)> populated);
  void StartAllCpus(absl::FunctionRef<bool(size_t)> populated);
  static Header LoadHeader(AtomicHeader* hdrp);
  static void StoreHeader(AtomicHeader* hdrp, Header hdr);
  void DrainCpu(void* slabs, Shift shift, int cpu, DrainHandler drain_handler);
  bool CpuIsDrained(void* slabs, Shift shift, int cpu);
  void DrainOldSlabs(void* slabs, Shift shift, int cpu,
                     const std::array<uint16_t, NumClasses>& old_begins,
                     DrainHandler drain_handler);

  // Implementation of InitCpu() allowing for reuse in ResizeSlabs().
  void InitCpuImpl(void* slabs, Shift shift, int cpu,
                   absl::FunctionRef<size_t(size_t)> capacity);

  std::pair<int, bool> CacheCpuSlabSlow();

  // We store both a pointer to the array of slabs and the shift value together
  // so that we can atomically update both with a single store.
  std::atomic<SlabsAndShift> slabs_and_shift_{};

  // Incremented every time slabs_and_shift_ is published (see InitSlabs()).
  //
  // Grow() reads the slabs, the shift and a size class header outside of a
  // restartable sequence and commits inside one.  The marker it compares there
  // says that the region is the running region of this cpu, but not that it is
  // the same generation of it: cpu_cache.h keeps one slabs buffer per shift and
  // recycles it, so a buffer can be retired and later become current again.
  // The header it read could compare equal by coincidence while begins_ and the
  // maximum capacities it was computed against have since changed.  Comparing
  // the generation inside the critical section rules that out.
  std::atomic<uint32_t> slabs_generation_{0};

  size_t num_cpus() const { return num_cpus_; }

  size_t num_cpus_ = 0;

  // begins_[size_class] is offset of the size_class region in the slabs area.
  std::atomic<uint16_t>* begins_ = nullptr;
};

// RAII for StopCpu/StartCpu.
template <size_t NumClasses>
class ScopedSlabCpuStop {
 public:
  ScopedSlabCpuStop(TcmallocSlab<NumClasses>& slab, int cpu)
      : slab_(slab), cpu_(cpu) {
    slab_.StopCpu(cpu_);
  }

  ~ScopedSlabCpuStop() { slab_.StartCpu(cpu_); }

 private:
  TcmallocSlab<NumClasses>& slab_;
  const int cpu_;

  ScopedSlabCpuStop(const ScopedSlabCpuStop&) = delete;
  ScopedSlabCpuStop& operator=(const ScopedSlabCpuStop&) = delete;
};

template <size_t NumClasses>
inline size_t TcmallocSlab<NumClasses>::Length(int cpu,
                                               size_t size_class) const {
  const auto [slabs, shift] = GetSlabsAndShift(std::memory_order_relaxed);
  Header hdr = LoadHeader(GetHeader(slabs, shift, cpu, size_class));
  uint16_t begin = begins_[size_class].load(std::memory_order_relaxed);
  // We can read inconsistent hdr/begin during Resize, to avoid surprising
  // callers return 0 instead of overflows values.
  return std::max<ssize_t>(0, hdr.current - begin);
}

template <size_t NumClasses>
inline size_t TcmallocSlab<NumClasses>::Capacity(int cpu,
                                                 size_t size_class) const {
  const auto [slabs, shift] = GetSlabsAndShift(std::memory_order_relaxed);
  Header hdr = LoadHeader(GetHeader(slabs, shift, cpu, size_class));
  uint16_t begin = begins_[size_class].load(std::memory_order_relaxed);
  return std::max<ssize_t>(0, hdr.capacity(begin));
}

#if defined(__x86_64__)
#define TCMALLOC_RSEQ_RELOC_TYPE "R_X86_64_NONE"
#define TCMALLOC_RSEQ_JUMP "jmp"

#if TCMALLOC_INTERNAL_PERCPU_USE_TLS_WORKAROUND
#if !defined(__PIC__) && !defined(__PIE__)
#define TCMALLOC_RSEQ_SET_CS(name) \
  "movq $__rseq_cs_" #name "_%=, (%[rseq_cs_addr_addr])\n"
#else
#define TCMALLOC_RSEQ_SET_CS(name)    \
  "mov __rseq_cs_" #name              \
  "_%=@GOTPCREL(%%rip), %[scratch]\n" \
  "movq %[scratch], (%[rseq_cs_addr_addr])\n"
#endif
#define TCMALLOC_RSEQ_CS_INPUT [rseq_cs_addr_addr] "r"(&__rseq_abi.rseq_cs)
#else
#if !defined(__PIC__) && !defined(__PIE__)
#define TCMALLOC_RSEQ_SET_CS(name) \
  "movq $__rseq_cs_" #name "_%=, %[rseq_cs_addr]\n"
#else
#define TCMALLOC_RSEQ_SET_CS(name)    \
  "mov __rseq_cs_" #name              \
  "_%=@GOTPCREL(%%rip), %[scratch]\n" \
  "movq %[scratch], %[rseq_cs_addr]\n"
#endif
#define TCMALLOC_RSEQ_CS_INPUT [rseq_cs_addr] "m"(__rseq_abi.rseq_cs)
#endif

#elif defined(__aarch64__)
// The trampoline uses a non-local branch to restart critical sections.
// The trampoline is located in the .text.unlikely section, and the maximum
// distance of B and BL branches in ARM64 is limited to 128MB. If the linker
// detects the distance being too large, it injects a thunk which may clobber
// the x16 or x17 register according to the ARMv8 ABI standard.
// The actual clobbering is hard to trigger in a test, so instead of waiting
// for clobbering to happen in production binaries, we proactively always
// clobber x16 and x17 to shake out bugs earlier.
// RSEQ critical section asm blocks should use TCMALLOC_RSEQ_CLOBBER
// in the clobber list to account for this.
#ifndef NDEBUG
#define TCMALLOC_RSEQ_TRAMPLINE_SMASH \
  "mov x16, #-2097\n"                 \
  "mov x17, #-2099\n"
#else
#define TCMALLOC_RSEQ_TRAMPLINE_SMASH
#endif
#define TCMALLOC_RSEQ_CLOBBER "x16", "x17"
#define TCMALLOC_RSEQ_RELOC_TYPE "R_AARCH64_NONE"
#define TCMALLOC_RSEQ_JUMP "b"
#define TCMALLOC_RSEQ_SET_CS(name)                     \
  TCMALLOC_RSEQ_TRAMPLINE_SMASH                        \
  "adrp %[scratch], __rseq_cs_" #name                  \
  "_%=\n"                                              \
  "add %[scratch], %[scratch], :lo12:__rseq_cs_" #name \
  "_%=\n"                                              \
  "str %[scratch], %[rseq_cs_addr]\n"
#endif  // defined(__aarch64__)

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
  "_trampoline_PREEMPTION_OR_SIGNAL_BASED_PROFILER_ACTIVE_%=\n"               \
  ".type " #name                                                              \
  "_trampoline_PREEMPTION_OR_SIGNAL_BASED_PROFILER_ACTIVE_%=,@function\n"     \
  "" #name                                                                    \
  "_trampoline_PREEMPTION_OR_SIGNAL_BASED_PROFILER_ACTIVE_%=:\n"              \
  "2:\n" TCMALLOC_RSEQ_JUMP                                                   \
  " 3f\n"                                                                     \
  ".size " #name                                                              \
  "_trampoline_PREEMPTION_OR_SIGNAL_BASED_PROFILER_ACTIVE_%=, . - " #name     \
  "_trampoline_PREEMPTION_OR_SIGNAL_BASED_PROFILER_ACTIVE_%=\n"               \
  ".popsection\n"                   /* Prepare */                             \
  "3:\n" TCMALLOC_RSEQ_SET_CS(name) /* Start */                               \
      "4:\n"
#ifdef __aarch64__
// Note that we calculate thread local variable offsets relative to the address
// of the sampler. The sampler is already accessed first on the path to Pop(),
// and calculating the address of other thread local variables relative to it is
// faster than doing independent TLS references for Arm.
#define TCMALLOC_RSEQ_INPUTS                                                 \
  [sampler_addr] "r"(subtle::percpu::GetThreadSamplerAddress()),             \
      [rseq_cs_addr] "m"(__rseq_abi.rseq_cs),                                \
      [rseq_sig] "n"(                                                        \
          TCMALLOC_PERCPU_RSEQ_SIGNATURE), /* Also pass common consts, there \
                                              is no cost to passing unused   \
                                              consts. */                     \
      [cpu_addr] "r"(subtle::percpu::VirtualCpuIdAddress()),                 \
      [slab_cpu_bias] "n"(TCMALLOC_SLAB_CPU_BIAS),                           \
      [sampler_slabs_offset] "n"(TCMALLOC_SAMPLER_SLABS_OFFSET)
#else
#define TCMALLOC_RSEQ_INPUTS                                                 \
  TCMALLOC_RSEQ_CS_INPUT,                                                    \
      [rseq_slabs_addr] "m"(*reinterpret_cast<volatile char*>(               \
          reinterpret_cast<uintptr_t>(&__rseq_abi) +                         \
          TCMALLOC_RSEQ_SLABS_OFFSET)),                                      \
      [rseq_cpu_addr] "m"(*subtle::percpu::VirtualCpuIdAddress()),           \
      [rseq_sig] "n"(                                                        \
          TCMALLOC_PERCPU_RSEQ_SIGNATURE), /* Also pass common consts, there \
                                              is no cost to passing unused   \
                                              consts. */                     \
      [slab_cpu_bias] "n"(TCMALLOC_SLAB_CPU_BIAS)
#endif
// Store v to p (*p = v) if *marker names the cpu the calling thread is
// executing on, *generation still holds <old_generation> and *p still holds
// <old>. Otherwise returns false.
//
// All three comparisons are needed:
//
//  * *marker establishes that we are running on the cpu that owns the region
//    and that no remote operation owns it.
//  * The marker does not change when the thread is merely preempted and
//    rescheduled onto the same cpu, so a thread that ran in between could have
//    updated *p through a fast path. <old> detects that and makes the update
//    atomic with respect to Push/Pop.
//  * A retired slabs buffer can become current again -- cpu_cache.h keeps one
//    buffer per shift and recycles it -- so *marker and *p can both match
//    across a resize that changed begins_ and the maximum capacities.
//    <old_generation> rules that out.
inline ABSL_ATTRIBUTE_ALWAYS_INLINE bool StoreCurrentCpu(
    std::atomic<uint32_t>* marker, std::atomic<uint32_t>* generation,
    uint32_t old_generation, std::atomic<int32_t>* p, int32_t old, int32_t v) {
  // The asm reads and writes all three locations with plain 32 bit accesses,
  // which is what a relaxed load or store of a lock-free 32 bit atomic compiles
  // to. They stay std::atomic<> right up to the operand lists below, which are
  // the only place the compiler has to be told about the memory the asm
  // touches.
  uintptr_t scratch = 0;
#if TCMALLOC_INTERNAL_PERCPU_USE_RSEQ && defined(__x86_64__)
  uint32_t tmp;
  asm(TCMALLOC_RSEQ_PROLOGUE(TcmallocSlab_Internal_StoreCurrentCpu)
          R"(
      xorq %[scratch], %[scratch]
      movzwl %[rseq_cpu_addr], %[tmp]
      addl $%c[slab_cpu_bias], %[tmp]
      cmpl %[tmp], (%[marker])
      jne 5f
      cmpl %[old_generation], %[generation]
      jne 5f
      cmpl %[old], %[p]
      jne 5f
      movl $1, %k[scratch]
      movl %[v], %[p]
      5 :)"
      : [scratch] "=&r"(scratch), [tmp] "=&r"(tmp),
        [p] "+m"(*atomic_danger::CastToIntegral(p))
      : TCMALLOC_RSEQ_INPUTS,
        [marker] "r"(atomic_danger::CastToIntegral(marker)),
        [generation] "m"(*atomic_danger::CastToIntegral(generation)),
        [old_generation] "r"(old_generation), [old] "r"(old), [v] "r"(v)
      : "cc", "memory");
#elif TCMALLOC_INTERNAL_PERCPU_USE_RSEQ && defined(__aarch64__)
  uintptr_t tmp, scratch2;
  asm(TCMALLOC_RSEQ_PROLOGUE(TcmallocSlab_Internal_StoreCurrentCpu)
          R"(
      mov %[scratch], #0
      ldrh %w[tmp], [%[cpu_addr]]
      ldr %w[scratch2], [%[marker]]
      sub %w[scratch2], %w[scratch2], #%c[slab_cpu_bias]
      cmp %w[scratch2], %w[tmp]
      b.ne 5f
      ldr %w[scratch2], %[generation]
      cmp %w[scratch2], %w[old_generation]
      b.ne 5f
      ldr %w[scratch2], %[p]
      cmp %w[scratch2], %w[old]
      b.ne 5f
      mov %[scratch], #1
      str %w[v], %[p]
      5 :)"
      : [scratch] "=&r"(scratch), [tmp] "=&r"(tmp), [scratch2] "=&r"(scratch2),
        [p] "+m"(*atomic_danger::CastToIntegral(p))
      : TCMALLOC_RSEQ_INPUTS,
        [marker] "r"(atomic_danger::CastToIntegral(marker)),
        [generation] "m"(*atomic_danger::CastToIntegral(generation)),
        [old_generation] "r"(old_generation), [old] "r"(old), [v] "r"(v)
      : TCMALLOC_RSEQ_CLOBBER, "cc", "memory");
#endif
  return scratch;
}

// Prefetch slabs memory for the case of repeated pushes/pops.
// Note: this prefetch slows down micro-benchmarks, but provides ~0.1-0.5%
// speedup for larger real applications.
inline ABSL_ATTRIBUTE_ALWAYS_INLINE void PrefetchSlabMemory(uintptr_t ptr) {
  PrefetchWT0(reinterpret_cast<void*>(ptr));
}

#if TCMALLOC_INTERNAL_PERCPU_USE_RSEQ && defined(__x86_64__)
// Note: These helpers must be "static inline" to avoid ODR violations due to
// different labels emitted in TCMALLOC_RSEQ_PROLOGUE.
static inline ABSL_ATTRIBUTE_ALWAYS_INLINE bool TcmallocSlab_Internal_Push(
    size_t size_class, void* item) {
  uintptr_t scratch, current;
  uint32_t tmp;
  asm goto(
      TCMALLOC_RSEQ_PROLOGUE(TcmallocSlab_Internal_Push)
      // scratch = tcmalloc_slabs;
      "movq %[rseq_slabs_addr], %[scratch]\n"
      // tmp = cpu + TCMALLOC_SLAB_CPU_BIAS;
      "movzwl %[rseq_cpu_addr], %[tmp]\n"
      "addl $%c[slab_cpu_bias], %[tmp]\n"
      // if (scratch->marker != tmp) goto overflow_label;
      "cmpl %[tmp], (%[scratch])\n"
      "jne %l[overflow_label]\n"
      // tmp = [slabs->current, slabs->remaining_capacity];
      "movl (%[scratch], %[size_class], 4), %[tmp]\n"
      "movzwq %w[tmp], %[current]\n"
      // tmp.remaining_capacity--;
      // tmp.current++;
      "addl $0xffff0001, %[tmp]\n"
      // if (tmp.remaining_capacity < 0) { goto overflow_label; }
      // (The only way we can _not_ get the carry flag set
      // is if remaining_capacity was 0; every other value will
      // wrap past 0 and become a decrease. This test will have
      // a false negative if remaining_capacity == 0 and
      // current == 0xffff, which is why we forbid that value
      // for current.)
      "jnc %l[overflow_label]\n"
      "movq %[item], (%[scratch], %[current], 8)\n"
      // [slabs->current, slabs->remaining_capacity] = tmp;
      "movl %[tmp], (%[scratch], %[size_class], 4)\n"
      // Commit
      "5:\n"
      : [scratch] "=&r"(scratch), [current] "=&r"(current), [tmp] "=&r"(tmp)
      : TCMALLOC_RSEQ_INPUTS, [size_class] "r"(size_class), [item] "r"(item)
      : "cc", "memory"
      : overflow_label);
  // Current now points to the slot we just pushed to.
  PrefetchSlabMemory(scratch + (current + 1) * sizeof(void*));
  return true;
overflow_label:
  return false;
}
#endif  // defined(__x86_64__)

#if TCMALLOC_INTERNAL_PERCPU_USE_RSEQ && defined(__aarch64__)
static inline ABSL_ATTRIBUTE_ALWAYS_INLINE bool TcmallocSlab_Internal_Push(
    size_t size_class, void* item) {
  uintptr_t region_start, scratch, current;
  asm goto(
      TCMALLOC_RSEQ_PROLOGUE(TcmallocSlab_Internal_Push)
      // region_start = tcmalloc_slabs;
      "ldr %[region_start], [%[sampler_addr], #%c[sampler_slabs_offset]]\n"
      // if (region_start->marker - TCMALLOC_SLAB_CPU_BIAS != cpu)
      //   goto overflow_label;
      "ldrh %w[scratch], [%[cpu_addr]]\n"
      "ldr %w[current], [%[region_start]]\n"
      "sub %w[current], %w[current], #%c[slab_cpu_bias]\n"
      "cmp %w[current], %w[scratch]\n"
      "b.ne %l[overflow_label]\n"
      // scratch = slab_headers[size_class] (current index, remaining capacity)
      "ldr %w[scratch], [%[region_start], %[size_class], LSL #2]\n"
      "and %w[current], %w[scratch], #0xffff\n"
      // scratch.remaining_capacity--;
      // scratch.current++;
      "subs %w[scratch], %w[scratch], %w[kAdjustment]\n"
      // if (scratch.remaining_capacity < 0)) { goto overflow_label; }
      // (See x86-64 code for reasoning)
      "b.cc %l[overflow_label]\n"
      "str %[item], [%[region_start], %[current], LSL #3]\n"
      "str %w[scratch], [%[region_start], %[size_class], LSL #2]\n"
      // Commit
      "5:\n"
      // If we do (current + 1) in the C++ code below, the compiler
      // will start CSE-ing the tail of the this function with the slow path,
      // it does not properly understand that the fast path should not
      // have a branch (i.e., it picks the wrong one for fallthrough).
      // So we simply do that one instruction in assembler.
      "add %[current], %[current], #1\n"
      : [scratch] "=&r"(scratch), [current] "=&r"(current),
        [region_start] "=&r"(region_start)
      : TCMALLOC_RSEQ_INPUTS, [size_class] "r"(size_class), [item] "r"(item),
        [kAdjustment] "r"(0xffff)
      : TCMALLOC_RSEQ_CLOBBER, "memory", "cc"
      : overflow_label);

  // Current now points to the slot we are going to push to next.
  PrefetchSlabMemory(reinterpret_cast<uintptr_t>(region_start) +
                     current * sizeof(void*));
  return true;
overflow_label:
  return false;
}
#endif  // defined (__aarch64__)

template <size_t NumClasses>
inline ABSL_ATTRIBUTE_ALWAYS_INLINE bool TcmallocSlab<NumClasses>::Push(
    size_t size_class, void* item) {
  TC_ASSERT_NE(size_class, 0);
  TC_ASSERT_NE(item, nullptr);
  TC_ASSERT_EQ(reinterpret_cast<uintptr_t>(item) & kBeginMark, 0);
  // Speculatively annotate item as released to TSan.  We may not succeed in
  // pushing the item, but if we wait for the restartable sequence to succeed,
  // it may become visible to another thread before we can trigger the
  // annotation.
  TSANRelease(item);
#if TCMALLOC_INTERNAL_PERCPU_USE_RSEQ
  return TcmallocSlab_Internal_Push(size_class, item);
#else
  return false;
#endif
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
inline ABSL_ATTRIBUTE_ALWAYS_INLINE void* TcmallocSlab<NumClasses>::Pop(
    size_t size_class) {
  TC_ASSERT_NE(size_class, 0);
  void* next;
  void* result;
  uintptr_t tcmalloc_slabs_addr, current;

  asm goto(TCMALLOC_RSEQ_PROLOGUE(TcmallocSlab_Internal_Pop)
           // scratch = tcmalloc_slabs;
           "movq %[rseq_slabs_addr], %[scratch]\n"
           // current = cpu + TCMALLOC_SLAB_CPU_BIAS;
           "movzwl %[rseq_cpu_addr], %k[current]\n"
           "addl $%c[slab_cpu_bias], %k[current]\n"
           // if (scratch->marker != current) goto underflow_path;
           "cmpl %k[current], (%[scratch])\n"
           "jne %l[underflow_path]\n"
           // current = scratch->header[size_class].current;
           "movzwq (%[scratch], %[size_class], 4), %[current]\n"
           "movq -8(%[scratch], %[current], 8), %[result]\n"
           "testb $%c[begin_mark_mask], %b[result]\n"
           "jnz %l[underflow_path]\n"
           "movq -16(%[scratch], %[current], 8), %[next]\n"
           // scratch->header[size_class].remaining_capacity++;
           // scratch->header[size_class].current--;
           "subl $0xffff0001, (%[scratch], %[size_class], 4)\n"
           // Commit
           "5:\n"
           : [result] "=&r"(result), [scratch] "=&r"(tcmalloc_slabs_addr),
             [current] "=&r"(current), [next] "=&r"(next)
           : TCMALLOC_RSEQ_INPUTS, [begin_mark_mask] "n"(kBeginMark),
             [size_class] "r"(size_class)
           : "cc", "memory"
           : underflow_path);
  TC_ASSERT(next);
  TC_ASSERT(result);
  TSANAcquire(result);

  // The next pop will be from current-2, but because we prefetch the previous
  // element we've already just read that, so prefetch current-3.
  PrefetchSlabMemory(tcmalloc_slabs_addr + (current - 3) * sizeof(void*));
  PrefetchNextObject(next);
  return AssumeNotNull(result);
underflow_path:
  return nullptr;
}
#endif  // defined(__x86_64__)

#if TCMALLOC_INTERNAL_PERCPU_USE_RSEQ && defined(__aarch64__)
template <size_t NumClasses>
inline ABSL_ATTRIBUTE_ALWAYS_INLINE void* TcmallocSlab<NumClasses>::Pop(
    size_t size_class) {
  TC_ASSERT_NE(size_class, 0);
  void* result;
  void* region_start;
  void* prefetch;
  uintptr_t scratch;
  uintptr_t current_plus_slabs_addr;
  asm goto(
      TCMALLOC_RSEQ_PROLOGUE(TcmallocSlab_Internal_Pop)
      // region_start = tcmalloc_slabs;

      "ldr %[region_start], [%[sampler_addr], #%c[sampler_slabs_offset]]\n"
      // if (region_start->marker - TCMALLOC_SLAB_CPU_BIAS != cpu)
      //   goto underflow_path;
      "ldrh %w[scratch], [%[cpu_addr]]\n"
      "ldr %w[current], [%[region_start]]\n"
      "sub %w[current], %w[current], #%c[slab_cpu_bias]\n"
      "cmp %w[current], %w[scratch]\n"
      "b.ne %l[underflow_path]\n"
      // scratch = slab_headers[size_class] (current index, remaining capacity)
      "ldr %w[scratch], [%[region_start], %[size_class], LSL #2]\n"
      // current = scratch.current
      "and %w[current], %w[scratch], #0xffff\n"
      // scratch.remaining_capacity++;
      // scratch.current--;
      "add %w[scratch], %w[scratch], %w[kAdjustment]\n"
      "add %[current], %[region_start], %[current], LSL #3\n"
      "ldp %[prefetch], %[result], [%[current], #-16]\n"
      "tbnz %[result], #%c[begin_mark_bit], %l[underflow_path]\n"
      "str %w[scratch], [%[region_start], %[size_class], LSL #2]\n"
      // Commit
      "5:\n"
      : [result] "=&r"(result), [prefetch] "=&r"(prefetch),
        [current] "=&r"(current_plus_slabs_addr),
        // Temps
        [region_start] "=&r"(region_start), [scratch] "=&r"(scratch)
      // Real inputs
      : TCMALLOC_RSEQ_INPUTS,
        [begin_mark_bit] "n"(absl::countr_zero(kBeginMark)),
        [size_class] "r"(size_class), [kAdjustment] "r"(0xffff)
      : TCMALLOC_RSEQ_CLOBBER, "memory", "cc"
      : underflow_path);
  TSANAcquire(result);

  // The next pop will be from current-2, but because we prefetch the previous
  // element we've already just read that, so prefetch current-3.
  PrefetchSlabMemory(current_plus_slabs_addr - 3 * sizeof(void*));
  PrefetchNextObject(prefetch);
  return AssumeNotNull(result);
underflow_path:
  return nullptr;
}
#endif  // defined(__aarch64__)

#if !TCMALLOC_INTERNAL_PERCPU_USE_RSEQ
template <size_t NumClasses>
inline ABSL_ATTRIBUTE_ALWAYS_INLINE void* TcmallocSlab<NumClasses>::Pop(
    size_t size_class) {
  return nullptr;
}
#endif

template <size_t NumClasses>
inline size_t TcmallocSlab<NumClasses>::Grow(
    int cpu, size_t size_class, size_t len,
    absl::FunctionRef<size_t(uint8_t)> max_capacity) {
  TC_ASSERT_NE(size_class, 0);
  // Acquire, and ordered before the load of slabs_and_shift_: everything read
  // below belongs to this generation or to a later one, never to an earlier
  // one.
  const uint32_t generation = slabs_generation_.load(std::memory_order_acquire);
  const auto [slabs, shift] = GetSlabsAndShift(std::memory_order_relaxed);
  const size_t max_cap = max_capacity(ToUint8(shift));
  void* cpu_slab = CpuMemoryStart(slabs, shift, cpu);
  auto* hdrp = GetHeader(slabs, shift, cpu, size_class);
  const Header old_hdr = LoadHeader(hdrp);
  uint16_t begin = begins_[size_class].load(std::memory_order_relaxed);
  ssize_t have = static_cast<ssize_t>(max_cap - old_hdr.capacity(begin));
  if (have <= 0) {
    return 0;
  }
  uint16_t n = std::min<uint16_t>(len, have);
  Header hdr = old_hdr;
  hdr.remaining_capacity += n;
  return StoreCurrentCpu(&GetCpuMarker(cpu_slab), &slabs_generation_,
                         generation, hdrp, absl::bit_cast<int32_t>(old_hdr),
                         absl::bit_cast<int32_t>(hdr))
             ? n
             : 0;
}

template <size_t NumClasses>
inline std::pair<int, bool> TcmallocSlab<NumClasses>::CacheCpuSlab() {
#if TCMALLOC_INTERNAL_PERCPU_USE_RSEQ
  const uintptr_t slabs = tcmalloc_slabs;
  const int vcpu = *VirtualCpuIdAddress();
  // slabs may name tcmalloc_dummy_slab, whose marker is 0 and therefore never
  // matches; no separate test for an uncached region is needed.
  if (ABSL_PREDICT_FALSE(GetCpuMarker(reinterpret_cast<void*>(slabs))
                             .load(std::memory_order_relaxed) !=
                         vcpu + TCMALLOC_SLAB_CPU_BIAS)) {
    return CacheCpuSlabSlow();
  }
  // The current cpu's region is cached and running, so the slab is indeed
  // full/empty.  Report the id we just validated against: tcmalloc_cached_vcpu
  // is only refreshed by Synchronize(), which this path deliberately skips, so
  // it may name a cpu we were on when we cached the region.
  return {vcpu, false};
#else
  return {VirtualCpu::GetAfterSynchronize(), false};
#endif
}

template <size_t NumClasses>
inline void TcmallocSlab<NumClasses>::UncacheCpuSlab() {
#if TCMALLOC_INTERNAL_PERCPU_USE_RSEQ
  tcmalloc_slabs = reinterpret_cast<uintptr_t>(tcmalloc_dummy_slab);
#endif
}

template <size_t NumClasses>
inline size_t TcmallocSlab<NumClasses>::PushBatch(size_t size_class,
                                                  void** batch, size_t len) {
  TC_ASSERT_NE(size_class, 0);
  TC_ASSERT_NE(len, 0);
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
  TC_ASSERT_NE(size_class, 0);
  TC_ASSERT_NE(len, 0);
  const size_t n = TcmallocSlab_Internal_PopBatch(size_class, batch, len,
                                                  &begins_[size_class]);
  TC_ASSERT_LE(n, len);

  // PopBatch is implemented in assembly, msan does not know that the returned
  // batch is initialized.
  ANNOTATE_MEMORY_IS_INITIALIZED(batch, n * sizeof(batch[0]));
  TSANAcquireBatch(batch, n);
  return n;
}

template <size_t NumClasses>
inline void* TcmallocSlab<NumClasses>::CpuMemoryStart(void* slabs, Shift shift,
                                                      int cpu) {
  return &static_cast<char*>(slabs)[cpu << ToUint8(shift)];
}

template <size_t NumClasses>
inline const void* TcmallocSlab<NumClasses>::CpuMemoryStart(const void* slabs,
                                                            Shift shift,
                                                            int cpu) {
  return &static_cast<const char*>(slabs)[cpu << ToUint8(shift)];
}

template <size_t NumClasses>
inline auto TcmallocSlab<NumClasses>::GetHeader(void* slabs, Shift shift,
                                                int cpu, size_t size_class)
    -> AtomicHeader* {
  TC_ASSERT_NE(size_class, 0);
  return &static_cast<AtomicHeader*>(
      CpuMemoryStart(slabs, shift, cpu))[size_class];
}

template <size_t NumClasses>
inline auto TcmallocSlab<NumClasses>::GetCpuMarker(void* cpu_slab)
    -> AtomicMarker& {
  return *static_cast<AtomicMarker*>(cpu_slab);
}

template <size_t NumClasses>
inline auto TcmallocSlab<NumClasses>::GetCpuMarker(const void* cpu_slab)
    -> const AtomicMarker& {
  return *static_cast<const AtomicMarker*>(cpu_slab);
}

template <size_t NumClasses>
inline auto TcmallocSlab<NumClasses>::GetCpuMarker(void* slabs, Shift shift,
                                                   int cpu) -> AtomicMarker& {
  return GetCpuMarker(CpuMemoryStart(slabs, shift, cpu));
}

template <size_t NumClasses>
inline auto TcmallocSlab<NumClasses>::GetCpuMarker(const void* slabs,
                                                   Shift shift, int cpu)
    -> const AtomicMarker& {
  return GetCpuMarker(CpuMemoryStart(slabs, shift, cpu));
}

template <size_t NumClasses>
inline uint32_t TcmallocSlab<NumClasses>::RunningMarker(int cpu) {
  TC_ASSERT_GE(cpu, 0);
  TC_ASSERT_LE(static_cast<uint32_t>(cpu) + TCMALLOC_SLAB_CPU_BIAS,
               kMaxRunningMarker);
  return cpu + TCMALLOC_SLAB_CPU_BIAS;
}

template <size_t NumClasses>
void TcmallocSlab<NumClasses>::AssertCpuStopped(const void* slabs, Shift shift,
                                                int cpu) {
  TC_ASSERT_EQ(GetCpuMarker(slabs, shift, cpu).load(std::memory_order_relaxed),
               TCMALLOC_SLAB_STOPPED);
}

template <size_t NumClasses>
void TcmallocSlab<NumClasses>::SetCpuStopped(void* slabs, Shift shift,
                                             int cpu) {
  auto& marker = GetCpuMarker(slabs, shift, cpu);
  TC_CHECK_EQ(marker.load(std::memory_order_relaxed), RunningMarker(cpu));
  marker.store(TCMALLOC_SLAB_STOPPED, std::memory_order_relaxed);
}

template <size_t NumClasses>
void TcmallocSlab<NumClasses>::SetCpuRunning(void* slabs, Shift shift,
                                             int cpu) {
  // A region only ever starts from stopped: InitCpuImpl() leaves it that way
  // and StopCpu()/StopAllCpus() put it back.
  AssertCpuStopped(slabs, shift, cpu);
  GetCpuMarker(slabs, shift, cpu)
      .store(RunningMarker(cpu), std::memory_order_release);
}

template <size_t NumClasses>
inline auto TcmallocSlab<NumClasses>::LoadHeader(AtomicHeader* hdrp) -> Header {
  return absl::bit_cast<Header>(hdrp->load(std::memory_order_relaxed));
}

template <size_t NumClasses>
inline void TcmallocSlab<NumClasses>::StoreHeader(AtomicHeader* hdrp,
                                                  Header hdr) {
  hdrp->store(absl::bit_cast<int32_t>(hdr), std::memory_order_relaxed);
}

template <size_t NumClasses>
void TcmallocSlab<NumClasses>::Init(
    absl::FunctionRef<void*(size_t, std::align_val_t)> alloc, void* slabs,
    absl::FunctionRef<size_t(size_t)> capacity, Shift shift) {
  num_cpus_ = NumCPUs();
  // Every CPU needs a running marker of its own that no unsynchronized thread
  // can compute; see kMaxSlabCpus.
  TC_CHECK_LE(num_cpus_, kMaxSlabCpus);
  begins_ = static_cast<std::atomic<uint16_t>*>(alloc(
      sizeof(begins_[0]) * NumClasses, std::align_val_t{ABSL_CACHELINE_SIZE}));
  InitSlabs(slabs, shift, capacity);

#if TCMALLOC_INTERNAL_PERCPU_USE_RSEQ
  // The fast paths compare the kernel's CPU id against region markers, so they
  // need to know where the kernel reports it before the first one runs.
  subtle::percpu::SyncCpuIdOffset();
  // This is needed only for tests that create/destroy slabs: this thread may
  // still have an address in the destroyed slabs cached.
  UncacheCpuSlab();
  FenceAllCpus();
#endif
}

template <size_t NumClasses>
void TcmallocSlab<NumClasses>::InitSlabs(
    void* slabs, Shift shift, absl::FunctionRef<size_t(size_t)> capacity) {
  slabs_and_shift_.store({slabs, shift}, std::memory_order_relaxed);
  // Release, and ordered after the store above: Grow() reads the generation
  // first, so bumping it before publishing the slabs would let Grow() pair the
  // new generation with the slabs that are about to be retired.
  slabs_generation_.fetch_add(1, std::memory_order_release);
  size_t consumed_bytes =
      (NumClasses * sizeof(Header) + sizeof(void*) - 1) & ~(sizeof(void*) - 1);
  bool prev_empty = false;
  for (size_t size_class = 1; size_class < NumClasses; ++size_class) {
    size_t cap = capacity(size_class);
    TC_CHECK_EQ(static_cast<uint16_t>(cap), cap);
    // One extra element for prefetch/begin marker.
    if (!prev_empty) {
      consumed_bytes += sizeof(void*);
    }
    prev_empty = cap == 0;
    begins_[size_class].store(consumed_bytes / sizeof(void*),
                              std::memory_order_relaxed);
    consumed_bytes += cap * sizeof(void*);
    if (consumed_bytes > (1 << ToUint8(shift))) {
      TC_BUG("per-CPU memory exceeded, have %v, need %v, size_class %v",
             1 << ToUint8(shift), consumed_bytes, size_class);
    }
  }
}

template <size_t NumClasses>
void TcmallocSlab<NumClasses>::InitCpu(
    int cpu, absl::FunctionRef<size_t(size_t)> capacity) {
  const auto [slabs, shift] = GetSlabsAndShift(std::memory_order_relaxed);
  // The region is not running (it is either unpopulated or stopped), so no
  // local operation can observe it while we initialize it. Callers serialize
  // with remote operations themselves.
  InitCpuImpl(slabs, shift, cpu, capacity);
  StartCpu(cpu);
}

template <size_t NumClasses>
void TcmallocSlab<NumClasses>::InitCpuImpl(
    void* slabs, Shift shift, int cpu,
    absl::FunctionRef<size_t(size_t)> capacity) {
  // The region we are about to overwrite must not be usable by <cpu>: it is
  // either unpopulated (0) or owned by a remote operation (STOPPED).  A
  // running marker would mean that the local fast paths are free to run in it.
  const uint32_t entry_marker =
      GetCpuMarker(slabs, shift, cpu).load(std::memory_order_relaxed);
  TC_CHECK(entry_marker == 0 || entry_marker == TCMALLOC_SLAB_STOPPED,
           "entry_marker=%v cpu=%v", entry_marker, cpu);
  TC_CHECK_LE((1 << ToUint8(shift)), (1 << 16) * sizeof(void*));

  // Initialize prefetch target and compute the offsets for the
  // boundaries of each size class' cache.
  void* curr_slab = CpuMemoryStart(slabs, shift, cpu);
  void** elems = reinterpret_cast<void**>(
      (reinterpret_cast<uintptr_t>(GetHeader(slabs, shift, cpu, NumClasses)) +
       sizeof(void*) - 1) &
      ~(sizeof(void*) - 1));
  bool prev_empty = false;
  for (size_t size_class = 1; size_class < NumClasses; ++size_class) {
    size_t cap = capacity(size_class);
    TC_CHECK_EQ(static_cast<uint16_t>(cap), cap);

    // This item serves both as the marker of slab begin (Pop checks for low bit
    // set to understand that it reached begin), and as prefetching stub
    // (Pop prefetches the previous element and prefetching an invalid pointer
    // is slow, this is a valid pointer for prefetching).
    if (!prev_empty) {
      *elems = reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(elems) |
                                       kBeginMark);
      ++elems;
    }
    prev_empty = cap == 0;

    Header hdr = {};
    hdr.set_current(elems - reinterpret_cast<void**>(curr_slab));
    hdr.remaining_capacity = 0;
    StoreHeader(GetHeader(slabs, shift, cpu, size_class), hdr);

    elems += cap;
    const size_t bytes_used_on_curr_slab =
        reinterpret_cast<char*>(elems) - reinterpret_cast<char*>(curr_slab);
    if (bytes_used_on_curr_slab > (1 << ToUint8(shift))) {
      TC_BUG("per-CPU memory exceeded, have %v, need %v", 1 << ToUint8(shift),
             bytes_used_on_curr_slab);
    }
  }

  // The region is now initialized but not yet usable: the caller starts it
  // when it publishes the slabs.  Leaving the marker at 0 would advertise it
  // as unpopulated, and ResizeSlabs()/UpdateMaxCapacities() publish the new
  // slabs before they start the cpus, so CacheCpuSlabSlow() would tell a
  // caller to populate a cpu that is already populated and mid-resize.
  GetCpuMarker(slabs, shift, cpu)
      .store(TCMALLOC_SLAB_STOPPED, std::memory_order_relaxed);
}

#if TCMALLOC_INTERNAL_PERCPU_USE_RSEQ
template <size_t NumClasses>
std::pair<int, bool> TcmallocSlab<NumClasses>::CacheCpuSlabSlow() {
  const int vcpu = VirtualCpu::Synchronize();
  TC_ASSERT_GE(vcpu, 0);
  TC_ASSERT_LT(vcpu, num_cpus());
  const auto [slabs, shift] = GetSlabsAndShift(std::memory_order_relaxed);
  void* start = CpuMemoryStart(slabs, shift, vcpu);

  // From September 2023 (cl/564370519) to September 2026, the cached slab
  // address overlapped __rseq_abi's cpu_id_start field, so the kernel clobbered
  // it on every migration and this function had to publish it from inside a
  // critical section and re-read slabs_and_shift_ afterwards. Validating
  // against the region's marker needs neither. Every use of the cached
  // address re-validates it against the region's marker, so caching an address
  // that goes stale -- because we were migrated between Synchronize() and the
  // store, or because the slabs are being resized right now -- costs at most
  // one more trip through this function.
  const uint32_t marker = GetCpuMarker(start).load(std::memory_order_relaxed);
  // A region for <vcpu> only ever holds one of these three values, and the
  // cached-slab fast path already rejected the running one.
  TC_CHECK(marker == 0 || marker == TCMALLOC_SLAB_STOPPED ||
               marker == RunningMarker(vcpu),
           "marker=%v vcpu=%v", marker, vcpu);
  if (marker == TCMALLOC_SLAB_STOPPED) {
    // A remote operation owns this cpu's region. Report that so that the
    // caller falls back to the backing cache instead of spinning on a region
    // it cannot use.
    UncacheCpuSlab();
    return {-1, true};
  }
  // An unpopulated region is cached as is: the caller populates it, which
  // marks it as running.
  tcmalloc_slabs = reinterpret_cast<uintptr_t>(start);
  return {vcpu, true};
}
#endif

template <size_t NumClasses>
void TcmallocSlab<NumClasses>::DrainCpu(void* slabs, Shift shift, int cpu,
                                        DrainHandler drain_handler) {
  AssertCpuStopped(slabs, shift, cpu);
  for (size_t size_class = 1; size_class < NumClasses; ++size_class) {
    uint16_t begin = begins_[size_class].load(std::memory_order_relaxed);
    auto* hdrp = GetHeader(slabs, shift, cpu, size_class);
    Header hdr = LoadHeader(hdrp);
    if (hdr.current == 0) {
      continue;
    }
    const size_t size = hdr.current - begin;
    const size_t cap = hdr.capacity(begin);

    void** batch =
        reinterpret_cast<void**>(CpuMemoryStart(slabs, shift, cpu)) + begin;
    TSANAcquireBatch(batch, size);
    drain_handler(cpu, size_class, batch, size, cap);
    hdr.set_current(begin);
    hdr.remaining_capacity = 0;
    StoreHeader(hdrp, hdr);
  }
}

template <size_t NumClasses>
bool TcmallocSlab<NumClasses>::CpuIsDrained(void* slabs, Shift shift, int cpu) {
  for (size_t size_class = 1; size_class < NumClasses; ++size_class) {
    uint16_t begin = begins_[size_class].load(std::memory_order_relaxed);
    auto* hdrp = GetHeader(slabs, shift, cpu, size_class);
    Header hdr = LoadHeader(hdrp);
    if (hdr.capacity(begin) != 0) {
      return false;
    }
  }
  return true;
}

template <size_t NumClasses>
void TcmallocSlab<NumClasses>::DrainOldSlabs(
    void* slabs, Shift shift, int cpu,
    const std::array<uint16_t, NumClasses>& old_begins,
    DrainHandler drain_handler) {
  for (size_t size_class = 1; size_class < NumClasses; ++size_class) {
    uint16_t begin = old_begins[size_class];
    auto* hdrp = GetHeader(slabs, shift, cpu, size_class);
    Header hdr = LoadHeader(hdrp);
    if (hdr.current == 0) {
      continue;
    }
    const size_t size = hdr.current - begin;
    const size_t cap = hdr.capacity(begin);

    void** batch =
        reinterpret_cast<void**>(CpuMemoryStart(slabs, shift, cpu)) + begin;
    TSANAcquireBatch(batch, size);
    drain_handler(cpu, size_class, batch, size, cap);
    hdr.set_current(begin);
    hdr.remaining_capacity = 0;
    StoreHeader(hdrp, hdr);
  }

  // These slabs are retired, but the caller may hand the buffer back to us
  // later (cpu_cache.h keeps one buffer per shift and reuses it after an
  // MADV_DONTNEED).  Mark the region unpopulated rather than leaving the
  // stopped marker that StopAllCpus wrote: MADV_DONTNEED would do the same,
  // but it is allowed to fail, and a region that came back marked as stopped
  // could never be populated again.
  GetCpuMarker(slabs, shift, cpu).store(0, std::memory_order_relaxed);
}

template <size_t NumClasses>
ResizeSlabsInfo TcmallocSlab<NumClasses>::UpdateMaxCapacities(
    void* new_slabs, absl::FunctionRef<size_t(size_t)> capacity,
    absl::FunctionRef<void(int, uint16_t)> update_capacity,
    absl::FunctionRef<bool(size_t)> populated, DrainHandler drain_handler,
    PerSizeClassMaxCapacity* new_max_capacity, int classes_to_resize) {
  const int n_cpus = num_cpus();
  const auto [old_slabs, shift] = GetSlabsAndShift(std::memory_order_relaxed);

  // Phase 0: Initialize slabs for populated CPUs BEFORE stopping CPUs.
  // This prefaults pages while all CPUs continue running undisturbed.
  for (size_t cpu = 0; cpu < n_cpus; ++cpu) {
    // populated should not change while this function runs because the caller
    // should be holding all the CPU resize locks.
    if (!populated(cpu)) continue;
    InitCpuImpl(new_slabs, shift, cpu, capacity);
  }

  // Phase 1: Stop all CPUs and initialize any CPUs in the new slab that have
  // already been populated in the old slab.
  std::array<uint16_t, NumClasses> old_begins;
  for (int size_class = 1; size_class < NumClasses; ++size_class) {
    old_begins[size_class] =
        begins_[size_class].load(std::memory_order_relaxed);
  }

  StopAllCpus(populated);

#ifdef TCMALLOC_INTERNAL_LATENCY_INJECTION
  // TODO(b/29448043): Remove latency injection.
  ScopedDelay delay(ScopedDelay::update_max_capacities_delay);
#endif

  // Phase 2: Update max capacity of the size classes.
  for (int i = 0; i < classes_to_resize; ++i) {
    size_t size_class = new_max_capacity[i].size_class;
    size_t cap = new_max_capacity[i].max_capacity;
    // Size class 0's header is the region's marker, so it has no capacity to
    // update.  This is a cold path, so check rather than assert.
    TC_CHECK_NE(size_class, 0);
    TC_CHECK_LT(size_class, NumClasses);
    update_capacity(size_class, cap);
  }

  // Phase 3: Initialize slabs.
  InitSlabs(new_slabs, shift, capacity);

  // Phase 4: Re-start all CPUs.
  StartAllCpus(populated);

  // Phase 5: Return pointers from the old slab to the TransferCache.
  for (size_t cpu = 0; cpu < n_cpus; ++cpu) {
    if (!populated(cpu)) continue;
    DrainOldSlabs(old_slabs, shift, cpu, old_begins, drain_handler);
  }
  return {old_slabs, GetSlabsAllocSize(shift, n_cpus)};
}

template <size_t NumClasses>
auto TcmallocSlab<NumClasses>::ResizeSlabs(
    Shift new_shift, void* new_slabs,
    absl::FunctionRef<size_t(size_t)> capacity,
    absl::FunctionRef<bool(size_t)> populated, DrainHandler drain_handler)
    -> ResizeSlabsInfo {
  // Phase 1: Collect begins, initialize any CPUs in the new
  // slab that have already been populated in the old slab,
  // then stop all CPUs.
  const auto [old_slabs, old_shift] =
      GetSlabsAndShift(std::memory_order_relaxed);
  std::array<uint16_t, NumClasses> old_begins;
  for (int size_class = 1; size_class < NumClasses; ++size_class) {
    old_begins[size_class] =
        begins_[size_class].load(std::memory_order_relaxed);
  }

  TC_ASSERT_NE(new_shift, old_shift);
  const int n_cpus = num_cpus();
  for (size_t cpu = 0; cpu < n_cpus; ++cpu) {
    if (populated(cpu)) {
      InitCpuImpl(new_slabs, new_shift, cpu, capacity);
    }
  }

  StopAllCpus(populated);

#ifdef TCMALLOC_INTERNAL_LATENCY_INJECTION
  // TODO(b/29448043): Remove latency injection.
  ScopedDelay delay(ScopedDelay::resize_slabs_delay);
#endif

  // Phase 2: Atomically update slabs and shift.
  InitSlabs(new_slabs, new_shift, capacity);

  // Phase 3: Re-start all CPUs.
  StartAllCpus(populated);

  // Phase 4: Return pointers from the old slab to the TransferCache.
  for (size_t cpu = 0; cpu < n_cpus; ++cpu) {
    if (!populated(cpu)) continue;
    DrainOldSlabs(old_slabs, old_shift, cpu, old_begins, drain_handler);
  }

  return {old_slabs, GetSlabsAllocSize(old_shift, n_cpus)};
}

template <size_t NumClasses>
void* TcmallocSlab<NumClasses>::Destroy(
    absl::FunctionRef<void(void*, size_t, std::align_val_t)> free) {
  // REQUIRES: no thread reaches a fast path on this slab again.  Threads name
  // their region through a cached address that nothing republishes, so freeing
  // the slabs does not stop them from reading it, and no fence can make them
  // let go of it.
  const size_t n_cpus = num_cpus();

  free(begins_, sizeof(begins_[0]) * NumClasses,
       std::align_val_t{ABSL_CACHELINE_SIZE});
  begins_ = nullptr;
  const auto [slabs, shift] = GetSlabsAndShift(std::memory_order_relaxed);
  size_t slabs_size = GetSlabsAllocSize(shift, n_cpus);
  free(slabs, slabs_size, SlabAlignment(shift));
  slabs_and_shift_.store({nullptr, shift}, std::memory_order_relaxed);
  return slabs;
}

template <size_t NumClasses>
size_t TcmallocSlab<NumClasses>::GrowOtherCache(
    int cpu, size_t size_class, size_t len,
    absl::FunctionRef<size_t(uint8_t)> max_capacity) {
  const auto [slabs, shift] = GetSlabsAndShift(std::memory_order_relaxed);
  AssertCpuStopped(slabs, shift, cpu);
  const size_t max_cap = max_capacity(ToUint8(shift));
  auto* hdrp = GetHeader(slabs, shift, cpu, size_class);
  Header hdr = LoadHeader(hdrp);
  uint16_t begin = begins_[size_class].load(std::memory_order_relaxed);
  uint16_t to_grow = std::min<uint16_t>(len, max_cap - hdr.capacity(begin));
  hdr.remaining_capacity += to_grow;
  StoreHeader(hdrp, hdr);
  return to_grow;
}

template <size_t NumClasses>
size_t TcmallocSlab<NumClasses>::ShrinkOtherCache(
    int cpu, size_t size_class, size_t len, ShrinkHandler shrink_handler) {
  const auto [slabs, shift] = GetSlabsAndShift(std::memory_order_relaxed);
  AssertCpuStopped(slabs, shift, cpu);

  auto* hdrp = GetHeader(slabs, shift, cpu, size_class);
  Header hdr = LoadHeader(hdrp);

  // If we do not have len number of items to shrink, we try to pop items from
  // the list first to create enough capacity that can be shrunk.
  // If we pop items, we also execute callbacks.
  const uint16_t unused = hdr.remaining_capacity;
  uint16_t begin = begins_[size_class].load(std::memory_order_relaxed);
  if (unused < len && hdr.current != begin) {
    uint16_t pop = std::min<uint16_t>(len - unused, hdr.current - begin);
    void** batch = reinterpret_cast<void**>(CpuMemoryStart(slabs, shift, cpu)) +
                   hdr.current - pop;
    TSANAcquireBatch(batch, pop);
    shrink_handler(size_class, batch, pop);
    hdr.current -= pop;
    hdr.remaining_capacity += pop;
  }

  // Shrink the capacity.
  const uint16_t to_shrink = std::min<uint16_t>(len, hdr.remaining_capacity);
  hdr.remaining_capacity -= to_shrink;
  StoreHeader(hdrp, hdr);
  return to_shrink;
}

template <size_t NumClasses>
void TcmallocSlab<NumClasses>::Drain(int cpu, DrainHandler drain_handler) {
  ScopedSlabCpuStop<NumClasses> cpu_stop(*this, cpu);
  const auto [slabs, shift] = GetSlabsAndShift(std::memory_order_relaxed);
  DrainCpu(slabs, shift, cpu, drain_handler);
}

template <size_t NumClasses>
void TcmallocSlab<NumClasses>::ReleaseSlabMetadataForDrainedCpus(
    absl::FunctionRef<bool(size_t)> populated,
    absl::FunctionRef<void(size_t)> unpopulate,
    absl::FunctionRef<void(void*, size_t)> madvise_away_slabs) {
  const int n_cpus = num_cpus();

  // For each hugepage touched by our slabs, track whether there is something
  // there that needs to be freed (because all CPUs belonging to that hugepage
  // are drained).
  //
  // // The max per-CPU metadata size is smaller than a hugepage (asserted
  // below) and we are aligned to it (also checked below), so it's fine to
  // allocate tracking for as many hugepages as we can have CPUs. Still,
  // we add + 1 as a buffer.
  constexpr int kMaxHugePagesTouched = kMaxCpus + 1;
  std::array<HugePageStatus, kMaxHugePagesTouched> hugepage_status;
  std::fill(hugepage_status.begin(), hugepage_status.end(), kNotTouched);

  // We can't allocate while holding the per-cpu spinlocks.
  AllocationGuard enforce_no_alloc;

  // Stop all CPUs. They must also be locked, since we are touching the
  // populated bit later.
  StopAllCpus(populated);

  // See which ones are actually drained, and which hugepages we can free.
  const auto [slabs, shift] = GetSlabsAndShift(std::memory_order_relaxed);
  const size_t slab_size_bytes = 1ULL << static_cast<uint8_t>(shift);
  TC_CHECK_LT(slab_size_bytes, kHugePageSize);

  // If the slab is not aligned to its own size, freeing any hugepage
  // would tear through a CPU's data, and we can do nothing.
  if (!IsAlignedTo(slabs, slab_size_bytes)) {
    TC_BUG("Slabs are not properly aligned");
    return;
  }

  auto address_to_hugepage_number = [](const void* addr) {
    return reinterpret_cast<uintptr_t>(addr) >> kHugePageShift;
  };
  const size_t base_hugepage_nr = address_to_hugepage_number(slabs);

  void* slabs_start = CpuMemoryStart(slabs, shift, 0);
  if (!IsAlignedTo(slabs_start, kHugePageSize)) {
    // If our slab doesn't doesn't start hugepage-aligned,
    // we cannot free the first hugepage.
    hugepage_status[0] = kCannotFree;
  }

  // We cannot free the last page page either, if the slabs doesn't
  // end perfectly on a hugepage boundary. (At the very least,
  // we'd risk tearing a hugepage.)
  void* slabs_end = CpuMemoryStart(slabs, shift, n_cpus);
  hugepage_status[address_to_hugepage_number(slabs_end) - base_hugepage_nr] =
      kCannotFree;

  // Go through all the CPUs and figure out which hugepage its slab
  // lives in. (Because we've already tested that slabs are slab-aligned
  // and not larger than a hugepage, and they are also powers of two,
  // it can never cross hugepages.)
  for (size_t cpu = 0; cpu < n_cpus; ++cpu) {
    if (!populated(cpu)) {
      continue;
    }

    size_t slab_hugepage =
        address_to_hugepage_number(CpuMemoryStart(slabs, shift, cpu));
    TC_CHECK_GE(slab_hugepage, base_hugepage_nr);
    HugePageStatus& status = hugepage_status[slab_hugepage - base_hugepage_nr];

    if (status == kCannotFree) {
      // No need to check, don't do anything.
    } else if (CpuIsDrained(slabs, shift, cpu)) {
      status = kShouldFree;
    } else {
      status = kCannotFree;
    }
  }

  for (size_t hugepage_idx = 0; hugepage_idx < hugepage_status.size();
       ++hugepage_idx) {
    if (hugepage_status[hugepage_idx] != kShouldFree) {
      continue;
    }

    void* hugepage_start = reinterpret_cast<void*>(
        (base_hugepage_nr + hugepage_idx) * kHugePageSize);

    // Coalesce neighboring madvises.
    size_t bytes_to_free = kHugePageSize;
    while (hugepage_idx + 1 < hugepage_status.size() &&
           hugepage_status[hugepage_idx + 1] == kShouldFree) {
      bytes_to_free += kHugePageSize;
      ++hugepage_idx;
    }

    const size_t first_cpu = (reinterpret_cast<uintptr_t>(hugepage_start) -
                              reinterpret_cast<uintptr_t>(slabs)) /
                             slab_size_bytes;
    const size_t cpus_to_free = bytes_to_free / slab_size_bytes;

    // Mark the regions unpopulated before releasing them.  MADV_DONTNEED does
    // the same by zeroing them, but it is allowed to fail, and a region left
    // marked as stopped could never be populated again.  These regions are
    // resident (we stopped them above), so this adds no residency.
    for (size_t i = 0; i < cpus_to_free; ++i) {
      if (!populated(first_cpu + i)) continue;
      GetCpuMarker(slabs, shift, first_cpu + i)
          .store(0, std::memory_order_relaxed);
    }

    madvise_away_slabs(hugepage_start, bytes_to_free);
    for (size_t i = 0; i < cpus_to_free; ++i) {
      // Only the populated cpus in this range are being unpopulated.  Calling
      // unpopulate() for the others would count an unpopulation that never
      // happened.
      if (!populated(first_cpu + i)) continue;
      unpopulate(first_cpu + i);
    }
  }

  // Restart the CPUs again. The ones we just released the metadata of are no
  // longer populated, and madvising their metadata away has reset their
  // markers, so they are skipped.
  StartAllCpus(populated);
}

template <size_t NumClasses>
void TcmallocSlab<NumClasses>::StopCpu(int cpu) {
  TC_ASSERT(cpu >= 0 && cpu < num_cpus(), "cpu=%d", cpu);
  const auto [slabs, shift] = GetSlabsAndShift(std::memory_order_relaxed);
  SetCpuStopped(slabs, shift, cpu);
  FenceCpu(cpu);
}

template <size_t NumClasses>
void TcmallocSlab<NumClasses>::StartCpu(int cpu) {
  TC_ASSERT(cpu >= 0 && cpu < num_cpus(), "cpu=%d", cpu);
  // TODO(b/543348098): Leverage vCPU-centric fence.
#if defined(__aarch64__)
  // The fast paths load the marker with a relaxed load, so a release store of
  // the running marker is not enough to order our writes to the region against
  // them. Fence instead: fencing runs a context synchronizing event on every
  // thread that is executing, discarding anything it had speculated, and a
  // thread that is not executing has nothing in flight and reaches userspace
  // through one. Neither can then have read the region ahead of the marker.
  FenceCpu(cpu);
#endif
  const auto [slabs, shift] = GetSlabsAndShift(std::memory_order_relaxed);
  SetCpuRunning(slabs, shift, cpu);
}

template <size_t NumClasses>
void TcmallocSlab<NumClasses>::StopAllCpus(
    absl::FunctionRef<bool(size_t)> populated) {
  const auto [slabs, shift] = GetSlabsAndShift(std::memory_order_relaxed);
  for (size_t cpu = 0; cpu < num_cpus(); ++cpu) {
    if (populated(cpu)) {
      SetCpuStopped(slabs, shift, cpu);
    }
  }
  FenceAllCpus();
}

template <size_t NumClasses>
void TcmallocSlab<NumClasses>::StartAllCpus(
    absl::FunctionRef<bool(size_t)> populated) {
#if defined(__aarch64__)
  // See the comment in StartCpu.
  FenceAllCpus();
#endif
  const auto [slabs, shift] = GetSlabsAndShift(std::memory_order_relaxed);
  for (size_t cpu = 0; cpu < num_cpus(); ++cpu) {
    if (populated(cpu)) {
      SetCpuRunning(slabs, shift, cpu);
    }
  }
}

template <size_t NumClasses>
PerCPUMetadataState TcmallocSlab<NumClasses>::MetadataMemoryUsage() const {
  PerCPUMetadataState result;
  const auto [slabs, shift] = GetSlabsAndShift(std::memory_order_relaxed);
  const size_t n_cpus = num_cpus();
  size_t slabs_size = GetSlabsAllocSize(shift, n_cpus);
  size_t begins_size = NumClasses * sizeof(begins_[0]);
  result.virtual_size = slabs_size + begins_size;
  result.resident_size = MInCore::residence(slabs, slabs_size);
  return result;
}

}  // namespace percpu
}  // namespace subtle
}  // namespace tcmalloc_internal
}  // namespace tcmalloc
GOOGLE_MALLOC_SECTION_END

#endif  // TCMALLOC_INTERNAL_PERCPU_TCMALLOC_H_
