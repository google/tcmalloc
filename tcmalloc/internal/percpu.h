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

#ifndef TCMALLOC_INTERNAL_PERCPU_H_
#define TCMALLOC_INTERNAL_PERCPU_H_

// sizeof(Sampler)
#define TCMALLOC_SAMPLER_SIZE 32
// alignof(Sampler)
#define TCMALLOC_SAMPLER_ALIGN 8
// Sampler::HotDataOffset()
#define TCMALLOC_SAMPLER_HOT_OFFSET 24

// Offset from __rseq_abi to the cached slabs address.
#define TCMALLOC_RSEQ_SLABS_OFFSET -8

// Offset from the cached slabs address to the sampler.
#define TCMALLOC_SAMPLER_SLABS_OFFSET 40

// Offset from the sampler to __rseq_abi.
#define TCMALLOC_SAMPLER_RSEQ_OFFSET \
  (TCMALLOC_SAMPLER_SLABS_OFFSET - TCMALLOC_RSEQ_SLABS_OFFSET)

// The first 4 bytes of a per-CPU slab region hold a marker that says whether
// the region may be used by the allocation fast paths:
//
//   0                      the region is unpopulated.  This is the value left
//                          behind by mmap() and MADV_DONTNEED.
//   TCMALLOC_SLAB_STOPPED  the region is populated, but a remote operation
//                          (resize/drain/grow/shrink) owns it right now.
//   cpu + 1                the region is the current, running region for cpu.
//
// The bias avoids conflating a zeroed region with CPU 0's region.  The marker
// occupies the whole (otherwise unused) size class 0 header, so that
// TCMALLOC_SLAB_STOPPED cannot collide with a CPU id: the fast paths load the
// CPU id as a 16 bit value, so cpu + TCMALLOC_SLAB_CPU_BIAS fits in 17 bits.
#define TCMALLOC_SLAB_CPU_BIAS 1
#define TCMALLOC_SLAB_STOPPED 0xffffffff

// TCMALLOC_PERCPU_RSEQ_SUPPORTED_PLATFORM defines whether or not we have an
// implementation for the target OS and architecture.
// TODO(b/478927694): re-enable for HWASan
#if defined(__linux__) && (defined(__x86_64__) || defined(__aarch64__)) && \
    !defined(ABSL_HAVE_HWADDRESS_SANITIZER)
#define TCMALLOC_PERCPU_RSEQ_SUPPORTED_PLATFORM 1
#else
#define TCMALLOC_PERCPU_RSEQ_SUPPORTED_PLATFORM 0
#endif

#define TCMALLOC_PERCPU_RSEQ_VERSION 0x0
#define TCMALLOC_PERCPU_RSEQ_FLAGS 0x0
#if defined(__x86_64__)
#define TCMALLOC_PERCPU_RSEQ_SIGNATURE 0x53053053
#elif defined(__aarch64__)
#define TCMALLOC_PERCPU_RSEQ_SIGNATURE 0xd428bc00
#else
// Rather than error, allow us to build, but with an invalid signature.
#define TCMALLOC_PERCPU_RSEQ_SIGNATURE 0x0
#endif

// The constants above this line must be macros since they are shared with the
// RSEQ assembly sources.
#ifndef __ASSEMBLER__

#ifdef __linux__
#include <sched.h>
#endif

#include <atomic>
#include <cstddef>
#include <cstdint>

#include "absl/base/attributes.h"
#include "absl/base/optimization.h"
#include "tcmalloc/internal/config.h"
#include "tcmalloc/internal/linux_syscall_support.h"
#include "tcmalloc/internal/logging.h"

// TCMALLOC_INTERNAL_PERCPU_USE_RSEQ defines whether TCMalloc support for RSEQ
// on the target architecture exists. We currently only provide RSEQ for 64-bit
// x86, Arm binaries.
#if !defined(TCMALLOC_INTERNAL_PERCPU_USE_RSEQ)
#if TCMALLOC_PERCPU_RSEQ_SUPPORTED_PLATFORM == 1
#define TCMALLOC_INTERNAL_PERCPU_USE_RSEQ 1
#else
#define TCMALLOC_INTERNAL_PERCPU_USE_RSEQ 0
#endif
#endif  // !defined(TCMALLOC_INTERNAL_PERCPU_USE_RSEQ)

GOOGLE_MALLOC_SECTION_BEGIN
namespace tcmalloc {
namespace tcmalloc_internal {
namespace subtle {
namespace percpu {

inline constexpr int kRseqUnregister = 1;

// Internal state used for tracking initialization of GetRealCpuUnsafe()
inline constexpr int kCpuIdUnsupported = -2;
inline constexpr int kCpuIdUninitialized = -1;
inline constexpr int kCpuIdInitialized = 0;

// The fast paths read the CPU id as a 16 bit value and add
// TCMALLOC_SLAB_CPU_BIAS to it before comparing the result against a region's
// marker.  A thread that has not synchronized yet reads kCpuIdUninitialized or
// kCpuIdUnsupported from that field, which truncate to 0xffff and 0xfffe and
// so produce these two values.  Neither may name a running region.
inline constexpr uint32_t kUninitializedMarker =
    uint32_t{static_cast<uint16_t>(kCpuIdUninitialized)} +
    TCMALLOC_SLAB_CPU_BIAS;
inline constexpr uint32_t kUnsupportedMarker =
    uint32_t{static_cast<uint16_t>(kCpuIdUnsupported)} + TCMALLOC_SLAB_CPU_BIAS;

// A 16 bit CPU id biases to at most 0xffff, which puts kUninitializedMarker
// (0x10000) and TCMALLOC_SLAB_STOPPED out of reach on its own.
// kUnsupportedMarker (0xffff) is not, so it is the binding constraint: CPU ids
// stop one short of the one that would produce it.
inline constexpr uint32_t kMaxRunningMarker = kUnsupportedMarker - 1;
static_assert(kMaxRunningMarker < kUnsupportedMarker);
static_assert(kMaxRunningMarker < kUninitializedMarker);
static_assert(kMaxRunningMarker < TCMALLOC_SLAB_STOPPED);

// The most CPUs the marker protocol can tell apart.  CPU <c> runs with marker
// <c> + TCMALLOC_SLAB_CPU_BIAS, so the largest usable id is
// kMaxRunningMarker - TCMALLOC_SLAB_CPU_BIAS and there are kMaxRunningMarker
// of them.
inline constexpr size_t kMaxSlabCpus = kMaxRunningMarker;

#if TCMALLOC_INTERNAL_PERCPU_USE_RSEQ
// We provide a per-thread value (defined in percpu_rseq_asm.S) which both
// tracks thread-local initialization state and (with RSEQ) provides an atomic
// in-memory reference for this thread's execution CPU. This value is only
// valid when the thread is currently executing.
// Possible values:
//   Unavailable/uninitialized:
//     { kCpuIdUnsupported, kCpuIdUninitialized }
//   Initialized, available:
//     [0, NumCpus())    (Always updated at context-switch)
//
// CPU slabs region address caching.
// Calculation of the address of the current CPU slabs region is needed for
// allocation/deallocation fast paths, but is quite expensive. Due to variable
// shift and experimental support for "virtual CPUs", the calculation involves
// several additional loads and dependent calculations. Pseudo-code for the
// address calculation is as follows:
//
//   cpu_offset = __rseq_virtual_flat_cpu_id_offset;
//   cpu = *(&__rseq_abi + cpu_offset);
//   slabs_and_shift = TcmallocSlab.slabs_and_shift_;
//   shift = slabs_and_shift & kShiftMask;
//   shifted_cpu = cpu << shift;
//   slabs = slabs_and_shift & kSlabsMask;
//   slabs += shifted_cpu;
//
// To remove this calculation from fast paths, we cache the slabs address for
// the current CPU in thread local storage (tcmalloc_slabs). A cached address
// goes stale when the thread migrates to another CPU, when a remote operation
// claims the region, and when the slabs are resized. Rather than invalidating
// the cached address, we validate it against the region it points to: the
// first 4 bytes of a per-CPU region hold TCMALLOC_SLAB_CPU_BIAS + cpu while
// (and only while) the region is the current, running region of cpu.
//
// The fast path is thus a load and a compare:
//
//   slabs = tcmalloc_slabs;
//   if (*(uint32_t*)slabs != *(uint16_t*)(&__rseq_abi + cpu_offset) + 1)
//     goto slowpath;
//
// which is executed within an rseq critical section -- that is what makes the
// comparison atomic with respect to migration and to remote operations. The
// marker shares a cache line with the size class headers that the fast path
// is about to touch anyway.
//
// The validation is off the critical path. Its loads -- tcmalloc_vcpu_id_offset
// and then the CPU id out of __rseq_abi -- form a dependency chain that is
// disjoint from the one that produces the object (tcmalloc_slabs, then the size
// class header, then the object pointer). The two chains meet only at a
// perfectly predicted branch, so out-of-order execution resolves the validation
// alongside the allocation rather than ahead of it.
//
// Before the first successful CacheCpuSlab(), and after UncacheCpuSlab(),
// tcmalloc_slabs names tcmalloc_dummy_slab rather than nothing at all.  Its
// marker is 0, which is never a running marker, so the fast paths reach the
// slow path through the comparison they already make instead of through an
// additional test for a null address.
//
// Note: we read the CPU id as a 16 bit value, which assumes little-endian
// byte order (the case for our supported architectures) and NumCPUs() at most
// kMaxSlabCpus (checked in InitPerCpu() and TcmallocSlab::Init()).
//
// The slow path does the full slabs address calculation and caches it.
//
// Since we need to export the __rseq_abi variable (as part of rseq ABI),
// we arrange the placement of __rseq_abi and the preceding cached slabs
// address in percpu_rseq_asm.S (C++ is not capable of expressing that).
// __rseq_abi must be aligned to 32 bytes as per ABI. We want the cached slabs
// address to be contained within a single cache line (64 bytes), rather than
// split 2 cache lines. To achieve that we locate __rseq_abi in the second
// part of a cache line.
// For performance reasons we also collocate tcmalloc_sampler with __rseq_abi
// in the same cache line.
// InitPerCpu contains checks that the resulting data layout is as expected.

// A stand-in for a per-CPU slab region, used while a thread has no region of
// its own cached.  Only the marker (the first 4 bytes) is ever read from it,
// and it is 0, so every fast path comparison against it fails.  It lives in
// read-only memory, both because nothing may write to it and so that it is not
// mistaken for a real region.
extern "C" const uint32_t tcmalloc_dummy_slab[16];

// Address of the current CPU's slab region, or of tcmalloc_dummy_slab if none
// is cached.  percpu_rseq_asm.S supplies the initial value.
extern "C" ABSL_CONST_INIT thread_local volatile uintptr_t tcmalloc_slabs
    ABSL_ATTRIBUTE_INITIAL_EXEC;
extern "C" ABSL_CONST_INIT thread_local volatile kernel_rseq __rseq_abi
    ABSL_ATTRIBUTE_INITIAL_EXEC;
extern "C" ABSL_CONST_INIT thread_local volatile int tcmalloc_cached_vcpu
    ABSL_ATTRIBUTE_INITIAL_EXEC;
// The offset within __rseq_abi of the CPU id the fast paths compare against
// slab markers: kernel_rseq::cpu_id, kernel_rseq::vcpu_id or
// kernel_rseq::mm_cid, depending on the mode rseq was registered with.
//
// This duplicates __rseq_virtual_flat_cpu_id_offset, which the fast paths
// cannot read directly: it is defined in another module, so the compiler must
// reach it through the GOT.  Hidden visibility lets the fast paths address this
// copy relative to the program counter instead, which is also cheaper than a
// thread local would be -- a thread local's offset does not fit an addressing
// mode's immediate, so it costs an extra instruction to materialize.
//
// The value describes the process rather than the thread: rseq registration
// settles it before any allocation can complete, and it does not change
// afterwards.  SyncCpuIdOffset() records it.
extern "C" ABSL_CONST_INIT uint32_t tcmalloc_vcpu_id_offset
    __attribute__((visibility("hidden")));
// Note that for builds with RSEQ enabled, we declare the sampler here so that
// we can reference its address in percpu_tcmalloc.h without creating a
// circular dependency with the Sampler definition.
extern "C" ABSL_CONST_INIT thread_local char tcmalloc_sampler
    ABSL_ATTRIBUTE_INITIAL_EXEC;

// Provide weak definitions here to enable more efficient codegen.
// If compiler sees only extern declaration when generating accesses,
// then even with initial-exec model and -fno-PIE compiler has to assume
// that the definition may come from a dynamic library and has to use
// GOT access. When compiler sees even a weak definition, it knows the
// declaration will be in the current module and can generate direct accesses.
// The initializer below is not the one the program runs with: the strong
// definition in percpu_rseq_asm.S names tcmalloc_dummy_slab, and it is the one
// that ends up in the .tdata image.  It cannot be spelled here because casting
// an address to uintptr_t is not a constant expression.  TcmallocSlab.
// InitialSlabsAreTheDummyRegion checks that the value threads actually start
// with is the intended one.
ABSL_CONST_INIT thread_local volatile uintptr_t tcmalloc_slabs
    ABSL_ATTRIBUTE_WEAK = {};
ABSL_CONST_INIT thread_local volatile kernel_rseq __rseq_abi
    ABSL_ATTRIBUTE_WEAK = {
        0, static_cast<unsigned>(kCpuIdUninitialized),   0, 0, 0,
        0, {{kCpuIdUninitialized, kCpuIdUninitialized}},
};
ABSL_CONST_INIT thread_local volatile int tcmalloc_cached_vcpu
    ABSL_ATTRIBUTE_WEAK = kCpuIdUninitialized;
ABSL_CONST_INIT thread_local char tcmalloc_sampler ABSL_ATTRIBUTE_WEAK = 0;

// Because of the weak definitions we also add this non-weak symbol.
// It's not used other than ensuring that our asm file with the real TLS layout
// is actually linked. Without it linker manages to not link the asm file
// in some configurations since all symbols it provides are already defined
// (as weak), even though the asm file is in the same static library as other
// linked in files.
extern "C" ABSL_CONST_INIT thread_local char tcmalloc_rseq_layout;

// The offset, from &__rseq_abi, of the 16 bit CPU id that the fast paths
// compare against the slab marker. This is the low half of kernel_rseq::cpu_id
// when running on real CPUs, and kernel_rseq::vcpu_id or kernel_rseq::mm_cid
// when virtual CPUs are in use.
extern "C" ABSL_CONST_INIT size_t __rseq_virtual_flat_cpu_id_offset;

// Address of the 16 bit CPU id that the fast paths compare against slab
// markers: kernel_rseq::cpu_id on real CPUs, kernel_rseq::vcpu_id or
// kernel_rseq::mm_cid when virtual CPUs are in use.
//
// This reads tcmalloc_vcpu_id_offset, so that it agrees with the assembly fast
// paths and costs no indirection through the GOT.
inline volatile uint16_t* VirtualCpuIdAddress() {
  return reinterpret_cast<volatile uint16_t*>(
      reinterpret_cast<uintptr_t>(&__rseq_abi) + tcmalloc_vcpu_id_offset);
}

inline int GetRealCpuUnsafe() { return __rseq_abi.cpu_id; }
#else  // !TCMALLOC_INTERNAL_PERCPU_USE_RSEQ
inline int GetRealCpuUnsafe() { return kCpuIdUnsupported; }
#endif

// Functions below are implemented in the architecture-specific percpu_rseq_*.S
// files.
extern "C" {
size_t TcmallocSlab_Internal_PushBatch(size_t size_class, void** batch,
                                       size_t len);
size_t TcmallocSlab_Internal_PopBatch(size_t size_class, void** batch,
                                      size_t len,
                                      std::atomic<uint16_t>* begin_ptr);
}  // extern "C"

// NOTE:  We skirt the usual naming convention slightly above using "_" to
// increase the visibility of functions embedded into the root-namespace (by
// virtue of C linkage) in the supported case.

enum class RseqVcpuMode { kNone, kMM };

extern RseqVcpuMode vcpu_mode;

inline RseqVcpuMode GetRseqVcpuMode() {
  return vcpu_mode;
}

// Return whether we are using any kind of virtual CPUs.
inline bool UsingVirtualCpus() {
  return GetRseqVcpuMode() != RseqVcpuMode::kNone;
}

// Return whether we are using flat virtual CPUs (provided by kernel RSEQ).
bool UsingRseqVirtualCpus();

inline int GetRealCpu() {
  // The "unsafe" variant strongly depends on RSEQ.
  int cpu = GetRealCpuUnsafe();

  // We open-code the check for fast-cpu availability since we do not want to
  // force initialization in the first-call case.  This is done so that we can
  // use this in places where it may not always be safe to initialize.
  // Initialization is also simply unneeded on some platforms.
  if (ABSL_PREDICT_TRUE(cpu >= kCpuIdInitialized)) {
    return cpu;
  }

#ifdef TCMALLOC_HAVE_SCHED_GETCPU
  cpu = sched_getcpu();
  TC_ASSERT_GE(cpu, 0);
#endif  // TCMALLOC_HAVE_SCHED_GETCPU

  return cpu;
}

// We want to be able to get the address of the sampler in percpu so that we
// can calculate the address of other thread local variables relative to it.
// We just return a void* here since percpu doesn't know about the Sampler
// type and we need to avoid a circular dependency between percpu and
// the Sampler.
inline void* GetThreadSamplerAddress() {
#if TCMALLOC_INTERNAL_PERCPU_USE_RSEQ
  return &tcmalloc_sampler;
#else
  return nullptr;
#endif
}

// Static accessors functions for any kind of vCPU IDs, which transparently
// choose the right vCPU source based on the initialized mode. Wrapping it into
// a class helps to restrict access and avoid accidental misuse.
class VirtualCpu {
 public:
  // Returns the last vCPU ID since the last synchronization point. This may be
  // used where the vCPU ID is not used to derive RSEQ-validated state. Returns
  // kCpuIdUninitialized if this thread has never synchronized with a vCPU ID,
  // or kCpuIdUnsupported if RSEQ is not used.
  //
  // This is safe, because without a RSEQ critical section to detect thread
  // preemption, a thread may be preempted at any point and the virtual (or
  // real) CPU may change.
  static int get() {
#if TCMALLOC_INTERNAL_PERCPU_USE_RSEQ
    return tcmalloc_cached_vcpu;
#else   // TCMALLOC_INTERNAL_PERCPU_USE_RSEQ
    return kCpuIdUnsupported;
#endif  // TCMALLOC_INTERNAL_PERCPU_USE_RSEQ
  }

  // Returns the last vCPU ID since the last synchronization point.
  // REQUIRES: Synchronize() has been called by this thread
  static int GetAfterSynchronize() {
    const int ret = get();
    TC_ASSERT_GE(ret, kCpuIdInitialized);
    return ret;
  }

  // Returns the current vCPU ID. Use to synchronize RSEQ-validated state that
  // depends on the (per-CPU mutually exclusive) vCPU ID with the current vCPU
  // ID after a thread preemption was detected. This function may be expensive,
  // so it should only be called on slow paths.
  static int Synchronize();

 private:
  // The return value of Synchronize() may be overridden by tests if they define
  // VirtualCpu::TestSynchronize().
  ABSL_ATTRIBUTE_WEAK static int TestSynchronize();
};

bool InitFastPerCpu();

// Records where the kernel reports the CPU id, which the fast paths compare
// against slab markers, in tcmalloc_vcpu_id_offset.  Registering rseq settles
// the location; this must be called before the first fast path runs.
void SyncCpuIdOffset();

inline bool IsFast() {
  if (!TCMALLOC_INTERNAL_PERCPU_USE_RSEQ) {
    return false;
  }

  int cpu = GetRealCpuUnsafe();

  if (ABSL_PREDICT_TRUE(cpu >= kCpuIdInitialized)) {
    return true;
  } else if (ABSL_PREDICT_FALSE(cpu == kCpuIdUnsupported)) {
    return false;
  } else {
    // Sets 'cpu' for next time, and calls EnsureSlowModeInitialized if
    // necessary.
    return InitFastPerCpu();
  }
}

// As IsFast(), but if this thread isn't already initialized, will not
// attempt to do so.
inline bool IsFastNoInit() {
  if (!TCMALLOC_INTERNAL_PERCPU_USE_RSEQ) {
    return false;
  }
  int cpu = GetRealCpuUnsafe();
  return ABSL_PREDICT_TRUE(cpu >= kCpuIdInitialized);
}

// A barrier that prevents compiler reordering.
inline void CompilerBarrier() {
#if defined(__GNUC__)
  __asm__ __volatile__("" : : : "memory");
#else
  std::atomic_thread_fence(std::memory_order_seq_cst);
#endif
}

// Internal tsan annotations, do not use externally.
// Required as tsan does not natively understand RSEQ.
#ifdef ABSL_HAVE_THREAD_SANITIZER
extern "C" {
void __tsan_acquire(void* addr);
void __tsan_release(void* addr);
}
#endif

// TSAN relies on seeing (and rewriting) memory accesses.  It can't
// get at the memory accesses we make from RSEQ assembler sequences,
// which means it doesn't know about the semantics our sequences
// enforce.  So if we're under TSAN, add barrier annotations.
inline void TSANAcquire(void* p) {
#ifdef ABSL_HAVE_THREAD_SANITIZER
  __tsan_acquire(p);
#endif
}

inline void TSANAcquireBatch(void** batch, int n) {
#ifdef ABSL_HAVE_THREAD_SANITIZER
  for (int i = 0; i < n; i++) {
    __tsan_acquire(batch[i]);
  }
#endif
}

inline void TSANRelease(void* p) {
#ifdef ABSL_HAVE_THREAD_SANITIZER
  __tsan_release(p);
#endif
}

inline void TSANReleaseBatch(void** batch, int n) {
#ifdef ABSL_HAVE_THREAD_SANITIZER
  for (int i = 0; i < n; i++) {
    __tsan_release(batch[i]);
  }
#endif
}

void FenceCpu(int vcpu);
void FenceAllCpus();

}  // namespace percpu
}  // namespace subtle
}  // namespace tcmalloc_internal
}  // namespace tcmalloc
GOOGLE_MALLOC_SECTION_END

#endif  // !__ASSEMBLER__
#endif  // TCMALLOC_INTERNAL_PERCPU_H_
