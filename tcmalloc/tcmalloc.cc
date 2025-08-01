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
//
// tcmalloc is a fast malloc implementation.  See
// https://github.com/google/tcmalloc/tree/master/docs/design.md for a high-level description of
// how this malloc works.
//
// SYNCHRONIZATION
//  1. The thread-/cpu-specific lists are accessed without acquiring any locks.
//     This is safe because each such list is only accessed by one thread/cpu at
//     a time.
//  2. We have a lock per central free-list, and hold it while manipulating
//     the central free list for a particular size.
//  3. The central page allocator is protected by "pageheap_lock".
//  4. The pagemap (which maps from page-number to descriptor),
//     can be read without holding any locks, and written while holding
//     the "pageheap_lock".
//
//     This multi-threaded access to the pagemap is safe for fairly
//     subtle reasons.  We basically assume that when an object X is
//     allocated by thread A and deallocated by thread B, there must
//     have been appropriate synchronization in the handoff of object
//     X from thread A to thread B.
//
// PAGEMAP
// -------
// Page map contains a mapping from page id to Span.
//
// If Span s occupies pages [p..q],
//      pagemap[p] == s
//      pagemap[q] == s
//      pagemap[p+1..q-1] are undefined
//      pagemap[p-1] and pagemap[q+1] are defined:
//         NULL if the corresponding page is not yet in the address space.
//         Otherwise it points to a Span.  This span may be free
//         or allocated.  If free, it is in one of pageheap's freelist.

#include "tcmalloc/tcmalloc.h"

#include <errno.h>
#include <sched.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <algorithm>
#include <cstddef>
#include <limits>
#include <map>
#include <memory>
#include <new>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "absl/base/attributes.h"
#include "absl/base/casts.h"
#include "absl/base/const_init.h"
#include "absl/base/internal/spinlock.h"
#include "absl/base/optimization.h"
#include "absl/base/thread_annotations.h"
#include "absl/debugging/stacktrace.h"
#include "absl/numeric/bits.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_format.h"
#include "absl/strings/string_view.h"
#include "absl/time/clock.h"
#include "absl/types/span.h"
#include "tcmalloc/alloc_at_least.h"
#include "tcmalloc/allocation_sample.h"
#include "tcmalloc/allocation_sampling.h"
#include "tcmalloc/common.h"
#include "tcmalloc/cpu_cache.h"
#include "tcmalloc/deallocation_profiler.h"
#include "tcmalloc/error_reporting.h"
#include "tcmalloc/experiment.h"
#include "tcmalloc/global_stats.h"
#include "tcmalloc/guarded_allocations.h"
#include "tcmalloc/guarded_page_allocator.h"
#include "tcmalloc/internal/allocation_guard.h"
#include "tcmalloc/internal/config.h"
#include "tcmalloc/internal/logging.h"
#include "tcmalloc/internal/optimization.h"
#include "tcmalloc/internal/overflow.h"
#include "tcmalloc/internal/page_size.h"
#include "tcmalloc/internal/percpu.h"
#include "tcmalloc/internal/sampled_allocation.h"
#include "tcmalloc/internal_malloc_extension.h"
#include "tcmalloc/malloc_extension.h"
#include "tcmalloc/malloc_hook.h"
#include "tcmalloc/malloc_tracing_extension.h"
#include "tcmalloc/metadata_object_allocator.h"
#include "tcmalloc/page_allocator.h"
#include "tcmalloc/page_allocator_interface.h"
#include "tcmalloc/pagemap.h"
#include "tcmalloc/pages.h"
#include "tcmalloc/parameters.h"
#include "tcmalloc/sampler.h"
#include "tcmalloc/segv_handler.h"
#include "tcmalloc/selsan/selsan.h"
#include "tcmalloc/span.h"
#include "tcmalloc/static_vars.h"
#include "tcmalloc/stats.h"
#include "tcmalloc/system-alloc.h"
#include "tcmalloc/tcmalloc_policy.h"
#include "tcmalloc/thread_cache.h"
#include "tcmalloc/transfer_cache.h"

#if defined(TCMALLOC_HAVE_STRUCT_MALLINFO) || \
    defined(TCMALLOC_HAVE_STRUCT_MALLINFO2)
#include <malloc.h>
#endif

#if !defined(__x86_64__) && !defined(__aarch64__) && !defined(__riscv)
#error "Unsupported architecture."
#endif

#if defined(__android__) || defined(__APPLE__)
#error "Unsupported platform."
#endif

#ifndef __linux__
#error "Unsupported platform."
#endif

#ifndef ABSL_IS_LITTLE_ENDIAN
#error "TCMalloc only supports little endian architectures"
#endif

// We use this before out-of-line calls to slow paths so that compiler
// does not emit long conditional jump on the fast path.
//
// Without this compiler used to emit such jump for new sampling slow path:
//
// 0000000000516300 <TCMallocInternalNew>:
//   ...
//   51631b: 64 48 29 04 25 d0 fc ff ff    subq    %rax, %fs:-0x330
//   516324: 0f 82 b6 44 00 00             jb      0x51a7e0 <sampling slow path>
//   ...
//
// With this compiler emits a short jump on for the new new sampling slow path:
//
// 00000000005164c0 <TCMallocInternalNew>:
//   ...
//   5164db: 64 48 29 04 25 d0 fc ff ff    subq    %rax, %fs:-0x330
//   5164e4: 72 6c                         jb      0x516552 <_Znwm+0x92>
//   ...
//   ...
//   ...
//   516552: e9 e9 44 00 00                jmp     0x51aa40 <sampling slow path>
//
// The corresponding llvm issue:
// https://github.com/llvm/llvm-project/issues/80107
#define SLOW_PATH_BARRIER() asm("")

GOOGLE_MALLOC_SECTION_BEGIN
namespace tcmalloc {
namespace tcmalloc_internal {

// Gets a human readable description of the current state of the malloc data
// structures. Returns the actual written size.
// [buffer, buffer+result] will contain NUL-terminated output string.
//
// REQUIRES: buffer_length > 0.
extern "C" ABSL_ATTRIBUTE_UNUSED int MallocExtension_Internal_GetStatsInPbtxt(
    char* buffer, int buffer_length) {
  TC_ASSERT_GT(buffer_length, 0);
  Printer printer(buffer, buffer_length);

  // Print level one stats unless lots of space is available
  if (buffer_length < 10000) {
    DumpStatsInPbtxt(printer, 1);
  } else {
    DumpStatsInPbtxt(printer, 2);
  }

  size_t required = printer.SpaceRequired();

  if (buffer_length > required) {
    PageHeapSpinLockHolder l;
    required +=
        tc_globals.system_allocator().GetRegionFactory()->GetStatsInPbtxt(
            absl::Span<char>(buffer + required, buffer_length - required));
  }

  return required;
}

static void PrintStats(int level) {
  const int kBufferSize = 64 << 10;
  char* buffer = new char[kBufferSize];
  Printer printer(buffer, kBufferSize);
  DumpStats(printer, level);
  (void)write(STDERR_FILENO, buffer, strlen(buffer));
  delete[] buffer;
}

extern "C" void MallocExtension_Internal_GetStats(std::string* ret) {
  size_t shift = std::max<size_t>(18, absl::bit_width(ret->capacity()) - 1);
  for (; shift < 22; shift++) {
    const size_t size = 1 << shift;
    // Double ret's size until we succeed in writing the buffer without
    // truncation.
    //
    // TODO(b/142931922):  printer only writes data and does not read it.
    // Leverage https://wg21.link/P1072 when it is standardized.
    ret->resize(size - 1);

    size_t written_size = TCMalloc_Internal_GetStats(&*ret->begin(), size - 1);
    if (written_size < size - 1) {
      // We did not truncate.
      ret->resize(written_size);
      break;
    }
  }
}

extern "C" size_t TCMalloc_Internal_GetStats(char* buffer,
                                             size_t buffer_length) {
  Printer printer(buffer, buffer_length);
  if (buffer_length < 10000) {
    DumpStats(printer, 1);
  } else {
    DumpStats(printer, 2);
  }

  printer.printf("\nLow-level allocator stats:\n");
  auto& system_allocator = tc_globals.system_allocator();
  printer.printf("Memory Release Failures: %d\n",
                 system_allocator.release_errors());

  size_t n = printer.SpaceRequired();

  size_t bytes_remaining = buffer_length > n ? buffer_length - n : 0;
  if (bytes_remaining > 0) {
    n += system_allocator.GetRegionFactory()->GetStats(
        absl::Span<char>(buffer + n, bytes_remaining));
  }

  return n;
}

extern "C" const ProfileBase* MallocExtension_Internal_SnapshotCurrent(
    ProfileType type) {
  switch (type) {
    case ProfileType::kHeap:
      return DumpHeapProfile(tc_globals).release();
    case ProfileType::kFragmentation:
      return DumpFragmentationProfile(tc_globals).release();
    case ProfileType::kPeakHeap:
      return tc_globals.peak_heap_tracker().DumpSample().release();
    default:
      return nullptr;
  }
}

extern "C" AllocationProfilingTokenBase*
MallocExtension_Internal_StartAllocationProfiling() {
  return new AllocationSample(&tc_globals.allocation_samples, absl::Now());
}

extern "C" tcmalloc_internal::AllocationProfilingTokenBase*
MallocExtension_Internal_StartLifetimeProfiling() {
  return new deallocationz::DeallocationSample(
      &tc_globals.deallocation_samples);
}

MallocExtension::Ownership GetOwnership(const void* ptr) {
  const PageId p = PageIdContainingTagged(ptr);
  return tc_globals.pagemap().GetDescriptor(p)
             ? MallocExtension::Ownership::kOwned
             : MallocExtension::Ownership::kNotOwned;
}

extern "C" bool MallocExtension_Internal_GetNumericProperty(
    const char* name_data, size_t name_size, size_t* value) {
  return GetNumericProperty(name_data, name_size, value);
}

// Make sure the two definitions are in sync.
static_assert(static_cast<int>(tcmalloc::MallocExtension::LimitKind::kSoft) ==
              PageAllocator::kSoft);
static_assert(static_cast<int>(tcmalloc::MallocExtension::LimitKind::kHard) ==
              PageAllocator::kHard);

extern "C" size_t MallocExtension_Internal_GetMemoryLimit(
    tcmalloc::MallocExtension::LimitKind limit_kind) {
  return tc_globals.page_allocator().limit(
      static_cast<PageAllocator::LimitKind>(limit_kind));
}

extern "C" void MallocExtension_Internal_SetMemoryLimit(
    size_t limit, tcmalloc::MallocExtension::LimitKind limit_kind) {
  if (limit_kind == tcmalloc::MallocExtension::LimitKind::kHard) {
    Parameters::set_heap_size_hard_limit(limit);
  }
  tc_globals.page_allocator().set_limit(
      limit, static_cast<PageAllocator::LimitKind>(limit_kind));
}

extern "C" void MallocExtension_Internal_MarkThreadIdle() {
  ThreadCache::BecomeIdle();
}

extern "C" AddressRegionFactory* MallocExtension_Internal_GetRegionFactory() {
  PageHeapSpinLockHolder l;
  return tc_globals.system_allocator().GetRegionFactory();
}

extern "C" void MallocExtension_Internal_SetRegionFactory(
    AddressRegionFactory* factory) {
  PageHeapSpinLockHolder l;
  tc_globals.system_allocator().SetRegionFactory(factory);
}

// ReleaseMemoryToSystem drops the page heap lock while actually calling to
// kernel to release pages. To avoid confusing ourselves with
// releaser handling, lets do separate lock just for release.
ABSL_CONST_INIT static absl::base_internal::SpinLock release_lock(
    absl::kConstInit, absl::base_internal::SCHEDULE_KERNEL_ONLY);

extern "C" size_t MallocExtension_Internal_ReleaseMemoryToSystem(
    size_t num_bytes) {
  ABSL_CONST_INIT static ConstantRatePageAllocatorReleaser releaser
      ABSL_GUARDED_BY(release_lock);

  const AllocationGuardSpinLockHolder rh(&release_lock);

  return releaser.Release(num_bytes,
                          /*reason=*/PageReleaseReason::kReleaseMemoryToSystem);
}

// nallocx slow path.
// Moved to a separate function because size_class_with_alignment is not inlined
// which would cause nallocx to become non-leaf function with stack frame and
// stack spills. ABSL_ATTRIBUTE_ALWAYS_INLINE does not work on
// size_class_with_alignment, compiler barks that it can't inline the function
// somewhere.
static ABSL_ATTRIBUTE_NOINLINE size_t nallocx_slow(size_t size, int flags,
                                                   hot_cold_t hot_cold) {
  tc_globals.InitIfNecessary();
  size_t align = static_cast<size_t>(1ull << (flags & 0x3f));
  size_t size_class;
  if (ABSL_PREDICT_TRUE(tc_globals.sizemap().GetSizeClass(
          CppPolicy().AlignAs(align).AccessAs(hot_cold), size, &size_class))) {
    TC_ASSERT_NE(size_class, 0);
    return tc_globals.sizemap().class_to_size(size_class);
  } else {
    return BytesToLengthCeil(size).in_bytes();
  }
}

// The nallocx function allocates no memory, but it performs the same size
// computation as the malloc function, and returns the real size of the
// allocation that would result from the equivalent malloc function call.
// nallocx is a malloc extension originally implemented by jemalloc:
// http://www.unix.com/man-page/freebsd/3/nallocx/
extern "C" size_t nallocx(size_t size, int flags) noexcept {
  if (ABSL_PREDICT_FALSE(!tc_globals.IsInited() || flags != 0)) {
    return nallocx_slow(size, flags, hot_cold_t{255});
  }
  size_t size_class;
  if (ABSL_PREDICT_TRUE(
          tc_globals.sizemap().GetSizeClass(CppPolicy(), size, &size_class))) {
    TC_ASSERT_NE(size_class, 0);
    return tc_globals.sizemap().class_to_size(size_class);
  } else {
    return BytesToLengthCeil(size).in_bytes();
  }
}

extern "C" size_t MallocExtension_Internal_GetEstimatedAllocatedSize(
    size_t bytes, tcmalloc::hot_cold_t hot_cold) {
  return nallocx_slow(bytes, 0, hot_cold);
}

extern "C" MallocExtension::Ownership MallocExtension_Internal_GetOwnership(
    const void* ptr) {
  return GetOwnership(ptr);
}

extern "C" void MallocExtension_Internal_GetProperties(
    std::map<std::string, MallocExtension::Property>* result) {
  TCMallocStats stats;
  // Include residency stats to avoid overestimating reported memory usage from
  // returned slabs, see b/372229857#comment10.
  ExtractTCMallocStats(&stats, /*report_residence*/ true);

  const uint64_t virtual_memory_used = VirtualMemoryUsed(stats);
  const uint64_t physical_memory_used = PhysicalMemoryUsed(stats);
  const uint64_t bytes_in_use_by_app = InUseByApp(stats);

  result->clear();
  // Virtual Memory Used
  (*result)["generic.virtual_memory_used"].value = virtual_memory_used;
  // Physical Memory used
  (*result)["generic.physical_memory_used"].value = physical_memory_used;
  // Bytes in use By App
  (*result)["generic.current_allocated_bytes"].value = bytes_in_use_by_app;
  (*result)["generic.bytes_in_use_by_app"].value = bytes_in_use_by_app;
  (*result)["generic.heap_size"].value = HeapSizeBytes(stats.pageheap);
  (*result)["generic.peak_memory_usage"].value =
      static_cast<uint64_t>(stats.peak_stats.sampled_application_bytes);
  (*result)["generic.realized_fragmentation"].value = static_cast<uint64_t>(
      100. * safe_div(stats.peak_stats.backed_bytes -
                          stats.peak_stats.sampled_application_bytes,
                      stats.peak_stats.sampled_application_bytes));
  // Page Heap Free
  (*result)["tcmalloc.page_heap_free"].value = stats.pageheap.free_bytes;
  (*result)["tcmalloc.pageheap_free_bytes"].value = stats.pageheap.free_bytes;
  // Metadata Bytes
  (*result)["tcmalloc.metadata_bytes"].value = stats.metadata_bytes;
  // Heaps in Use
  (*result)["tcmalloc.thread_cache_count"].value = stats.tc_stats.in_use;
  // Central Cache Free List
  (*result)["tcmalloc.central_cache_free"].value = stats.central_bytes;
  // Transfer Cache Free List
  (*result)["tcmalloc.transfer_cache_free"].value = stats.transfer_bytes;
  // Per CPU Cache Free List
  (*result)["tcmalloc.cpu_free"].value = stats.per_cpu_bytes;
  (*result)["tcmalloc.sharded_transfer_cache_free"].value =
      stats.sharded_transfer_bytes;
  (*result)["tcmalloc.per_cpu_caches_active"].value =
      tc_globals.CpuCacheActive();
  // Thread Cache Free List
  (*result)["tcmalloc.current_total_thread_cache_bytes"].value =
      stats.thread_bytes;
  (*result)["tcmalloc.thread_cache_free"].value = stats.thread_bytes;
  (*result)["tcmalloc.local_bytes"].value = LocalBytes(stats);

  (*result)["tcmalloc.max_total_thread_cache_bytes"].value =
      ThreadCache::overall_thread_cache_size();

  // Page Unmapped
  (*result)["tcmalloc.pageheap_unmapped_bytes"].value =
      stats.pageheap.unmapped_bytes;
  // Arena non-resident bytes aren't on the page heap, but they are unmapped.
  (*result)["tcmalloc.page_heap_unmapped"].value =
      stats.pageheap.unmapped_bytes + stats.arena.bytes_nonresident;
  (*result)["tcmalloc.sampled_internal_fragmentation"].value =
      tc_globals.sampled_internal_fragmentation_.value();

  (*result)["tcmalloc.external_fragmentation_bytes"].value =
      ExternalBytes(stats);
  (*result)["tcmalloc.required_bytes"].value = RequiredBytes(stats);
  (*result)["tcmalloc.slack_bytes"].value = SlackBytes(stats.pageheap);

  const uint64_t hard_limit =
      tc_globals.page_allocator().limit(PageAllocator::kHard);
  const uint64_t soft_limit =
      tc_globals.page_allocator().limit(PageAllocator::kSoft);
  (*result)["tcmalloc.hard_usage_limit_bytes"].value = hard_limit;
  (*result)["tcmalloc.desired_usage_limit_bytes"].value = soft_limit;
  (*result)["tcmalloc.soft_limit_hits"].value =
      tc_globals.page_allocator().limit_hits(PageAllocator::kSoft);
  (*result)["tcmalloc.hard_limit_hits"].value =
      tc_globals.page_allocator().limit_hits(PageAllocator::kHard);

  (*result)["tcmalloc.successful_shrinks_after_soft_limit_hit"].value =
      tc_globals.page_allocator().successful_shrinks_after_limit_hit(
          PageAllocator::kSoft);
  (*result)["tcmalloc.successful_shrinks_after_hard_limit_hit"].value =
      tc_globals.page_allocator().successful_shrinks_after_limit_hit(
          PageAllocator::kHard);

  (*result)["tcmalloc.num_released_total_bytes"].value =
      stats.num_released_total.in_bytes();
  (*result)["tcmalloc.num_released_release_memory_to_system_bytes"].value =
      stats.num_released_release_memory_to_system.in_bytes();
  (*result)["tcmalloc.num_released_process_background_actions_bytes"].value =
      stats.num_released_process_background_actions.in_bytes();
  (*result)["tcmalloc.num_released_soft_limit_exceeded_bytes"].value =
      stats.num_released_soft_limit_exceeded.in_bytes();
  (*result)["tcmalloc.num_released_hard_limit_exceeded_bytes"].value =
      stats.num_released_hard_limit_exceeded.in_bytes();
}

extern "C" size_t MallocExtension_Internal_ReleaseCpuMemory(int cpu) {
  if (ABSL_PREDICT_FALSE(!subtle::percpu::IsFast())) return 0;

  size_t bytes = 0;
  if (tc_globals.CpuCacheActive()) {
    bytes = tc_globals.cpu_cache().Reclaim(cpu);
  }
  return bytes;
}

//-------------------------------------------------------------------
// Helpers for the exported routines below
//-------------------------------------------------------------------

struct SizeAndSampled {
  size_t size;
  bool sampled;
};

inline SizeAndSampled GetLargeSizeAndSampled(const void* ptr,
                                             const Span& span) {
  if (span.sampled()) {
    if (tc_globals.guardedpage_allocator().PointerIsMine(ptr)) {
      return SizeAndSampled{
          tc_globals.guardedpage_allocator().GetRequestedSize(ptr), true};
    }
    return SizeAndSampled{
        span.sampled_allocation().sampled_stack.allocated_size, true};
  } else {
    return SizeAndSampled{span.bytes_in_span(), false};
  }
}

inline size_t GetLargeSize(const void* ptr, const Span& span) {
  return GetLargeSizeAndSampled(ptr, span).size;
}

inline SizeAndSampled GetLargeSizeAndSampled(const void* ptr, const PageId p) {
  return GetLargeSizeAndSampled(ptr,
                                *tc_globals.pagemap().GetExistingDescriptor(p));
}

inline SizeAndSampled GetSizeAndSampled(const void* ptr) {
  if (ptr == nullptr) return SizeAndSampled{0, false};
  const PageId p = PageIdContainingTagged(ptr);
  const auto [span, size_class] =
      tc_globals.pagemap().GetExistingDescriptorAndSizeClass(p);
  if (size_class != 0) {
    return SizeAndSampled{tc_globals.sizemap().class_to_size(size_class),
                          false};
  } else {
    return GetLargeSizeAndSampled(ptr, *span);
  }
}

inline size_t GetSize(const void* ptr) { return GetSizeAndSampled(ptr).size; }

// This slow path also handles delete hooks and non-per-cpu mode.
ABSL_ATTRIBUTE_NOINLINE static void FreeWithHooksOrPerThread(
    void* ptr, std::optional<size_t> size, size_t size_class) {
  MallocHook::InvokeDeleteHook({ptr, size,
                                tc_globals.sizemap().class_to_size(size_class),
                                HookMemoryMutable::kMutable});
  if (ABSL_PREDICT_TRUE(UsePerCpuCache(tc_globals))) {
    tc_globals.cpu_cache().DeallocateSlow(ptr, size_class);
  } else if (ThreadCache* cache = ThreadCache::GetCacheIfPresent();
             ABSL_PREDICT_TRUE(cache)) {
    cache->Deallocate(ptr, size_class);
  } else {
    // This thread doesn't have thread-cache yet or already. Delete directly
    // into central cache.
    tc_globals.transfer_cache().InsertRange(size_class,
                                            absl::Span<void*>(&ptr, 1));
  }
}

// In free fast-path we handle a number of conditions (delete hooks,
// full cpu cache, uncached per-cpu slab pointer, etc) by delegating work to
// slower function that handles all of these cases. This is done so that free
// fast-path only does tail calls, which allow compiler to avoid generating
// costly prologue/epilogue for fast-path.
#if defined(__clang__)
__attribute__((flatten))
#endif
ABSL_ATTRIBUTE_NOINLINE static void FreeSmallSlow(void* ptr,
                                                  std::optional<size_t> size,
                                                  size_t size_class) {
  if (ABSL_PREDICT_FALSE(Static::HaveHooks()) ||
      ABSL_PREDICT_FALSE(!UsePerCpuCache(tc_globals))) {
    return FreeWithHooksOrPerThread(ptr, size, size_class);
  }
  tc_globals.cpu_cache().DeallocateSlowNoHooks(ptr, size_class);
}

static inline ABSL_ATTRIBUTE_ALWAYS_INLINE void FreeSmall(
    void* ptr, std::optional<size_t> size, size_t size_class) {
  if (!IsExpandedSizeClass(size_class)) {
    TC_ASSERT(IsNormalMemory(ptr) || IsSelSanMemory(ptr), "ptr=%p", ptr);
  } else {
    TC_ASSERT_EQ(GetMemoryTag(ptr), MemoryTag::kCold, "ptr=%p", ptr);
  }

  ptr = absl::bit_cast<void*>(absl::bit_cast<uintptr_t>(ptr) &
                              ~(static_cast<uintptr_t>(kAlignment) - 1u));

  // DeallocateFast may fail if:
  //  - the cpu cache is full
  //  - the cpu cache is not initialized
  //  - hooks are installed
  //  - per-thread mode is enabled
  if (ABSL_PREDICT_FALSE(
          !tc_globals.cpu_cache().DeallocateFast(ptr, size_class))) {
    FreeSmallSlow(ptr, size, size_class);
  }
}

namespace selsan {
// Returns best-effort info (start/size) about a heap object that contains ptr.
// If a non-heap pointer is passed, returns {} (but must not crash).
std::pair<void*, size_t> HeapObjectInfo(void* ptr) {
  PageId page = PageIdContaining(ptr);
  Span* span = tc_globals.pagemap().GetDescriptor(page);
  if (span == nullptr) {
    return {};
  }
  size_t size_class = tc_globals.pagemap().sizeclass(page);
  if (size_class == 0) {
    return {};
  }
  uintptr_t span_start = reinterpret_cast<uintptr_t>(span->start_address());
  size_t obj_size = tc_globals.sizemap().class_to_size(size_class);
  uintptr_t obj_start =
      span_start +
      (reinterpret_cast<uintptr_t>(ptr) - span_start) / obj_size * obj_size;
  if (obj_start + obj_size > span_start + span->bytes_in_span()) {
    obj_start -= obj_size;
  }
  return {reinterpret_cast<void*>(obj_start), obj_size};
}
}  // namespace selsan

namespace {

template <typename Policy>
inline sized_ptr_t do_malloc_pages(size_t size, size_t weight, Policy policy) {
  // Page allocator does not deal well with num_pages = 0.
  Length num_pages = std::max<Length>(BytesToLengthCeil(size), Length(1));

  MemoryTag tag = MemoryTag::kNormal;
  if (policy.is_cold()) {
    tag = MemoryTag::kCold;
  } else if (tc_globals.numa_topology().numa_aware()) {
    tag = NumaNormalTag(policy.numa_partition());
  }
  Span* span = tc_globals.page_allocator().NewAligned(
      num_pages, BytesToLengthCeil(policy.align()),
      {1, AccessDensityPrediction::kSparse}, tag);
  if (span == nullptr) return {nullptr, 0};

  // Set capacity to the exact size for a page allocation.  This needs to be
  // revisited if we introduce gwp-asan sampling / guarded allocations to
  // do_malloc_pages().
  sized_ptr_t res{span->start_address(), num_pages.in_bytes()};
  TC_ASSERT(!ColdFeatureActive() || tag == GetMemoryTag(span->start_address()));

  if (weight != 0) {
    auto ptr = SampleLargeAllocation(tc_globals, policy, size, weight, span);
    TC_CHECK_EQ(res.p, ptr.p);
  }

  return res;
}

// Handles freeing object that doesn't have size class, i.e. which
// is either large or sampled. We explicitly prevent inlining it to
// keep it out of fast-path. This helps avoid expensive
// prologue/epilogue for fast-path freeing functions.
template <typename Policy>
ABSL_ATTRIBUTE_NOINLINE static void InvokeHooksAndFreePages(
    void* ptr, std::optional<size_t> size, Policy policy) {
  const PageId p = PageIdContaining(ptr);

  Span* span = tc_globals.pagemap().GetExistingDescriptor(p);
  // This check failing most likely means we double-freed the span.  In the
  // page heap, we clear the descriptor on Delete(span).
  //
  // We may also encounter this if we free a pointer that was never allocated
  // (it's corrupted, it's an interior pointer to another allocation separated
  // by more than kPageSize from the true pointer, etc.).
  if (ABSL_PREDICT_FALSE(span == nullptr)) {
    ReportDoubleFree(tc_globals, ptr);
  }

  auto& gwp_asan = tc_globals.guardedpage_allocator();
  const bool is_gwp_asan_ptr = gwp_asan.PointerIsMine(ptr);
  // Check for alignment before invoking hooks.
  //
  // We need to do special checks for GWP-ASan guarded allocations, since we may
  // right-align them to make it easier to find small buffer overruns.  This
  // causes our internal hook invocations to overrun and fail with SIGSEGV
  // rather than a cleaner error.
  bool valid_ptr = true;
  if (ABSL_PREDICT_FALSE(is_gwp_asan_ptr)) {
    valid_ptr = gwp_asan.PointerIsCorrectlyAligned(ptr);
  } else if (ABSL_PREDICT_FALSE(ptr != span->start_address())) {
    valid_ptr = false;
  }

  if (ABSL_PREDICT_TRUE(valid_ptr)) {
    MallocHook::InvokeDeleteHook(
        {ptr, size, GetLargeSize(ptr, *span), HookMemoryMutable::kMutable});
  }

  MaybeUnsampleAllocation(tc_globals, policy, ptr, size, *span);

  if (ABSL_PREDICT_FALSE(is_gwp_asan_ptr)) {
    gwp_asan.Deallocate(ptr);
#ifdef TCMALLOC_INTERNAL_LEGACY_LOCKING
    PageHeapSpinLockHolder l;
#endif  // TCMALLOC_INTERNAL_LEGACY_LOCKING
    Span::Delete(span);
  } else {
    if (ABSL_PREDICT_FALSE(ptr != span->start_address())) {
      ReportCorruptedFree(tc_globals, static_cast<std::align_val_t>(kPageSize),
                          ptr);
    }
#ifdef TCMALLOC_INTERNAL_LEGACY_LOCKING
    PageHeapSpinLockHolder l;
    tc_globals.page_allocator().Delete(
        span, GetMemoryTag(ptr),
        {.objects_per_span = 1, .density = AccessDensityPrediction::kSparse});
#else
    PageAllocatorInterface::AllocationState a{
        Range(p, span->num_pages()),
        span->donated(),
    };
    Span::Delete(span);
    PageHeapSpinLockHolder l;
    tc_globals.page_allocator().Delete(
        a, GetMemoryTag(ptr),
        {.objects_per_span = 1, .density = AccessDensityPrediction::kSparse});
#endif  // TCMALLOC_INTERNAL_LEGACY_LOCKING
  }
  // We expect to crash in GuardedPageAllocator::Delete or in
  // ReportCorruptedFree if the pointer was invalid.  We shouldn't make it here.
  TC_ASSERT(valid_ptr);
}

template <typename AlignPolicy>
bool CorrectSize(void* ptr, size_t size, AlignPolicy align);

bool CorrectAlignment(void* ptr, std::align_val_t alignment);

// Helper for the object deletion (free, delete, etc.).  Inputs:
//   ptr is object to be freed
//   size_class is the size class of that object, or 0 if it's unknown
//   have_size_class is true iff size_class is known and is non-0.
//
// Note that since have_size_class is compile-time constant, genius compiler
// would not need it. Since it would be able to somehow infer that
// GetSizeClass never produces 0 size_class, and so it
// would know that places that call this function with explicit 0 is
// "have_size_class-case" and others are "!have_size_class-case". But we
// certainly don't have such compiler. See also do_free_with_size below.
template <typename Policy>
inline ABSL_ATTRIBUTE_ALWAYS_INLINE void do_free(void* ptr, Policy policy) {
  // TODO(b/404341539):  Improve the bound.
  TC_ASSERT(CorrectAlignment(ptr, static_cast<std::align_val_t>(1)));

  if (!kSelSanPresent || ABSL_PREDICT_FALSE(!IsNormalMemory(ptr))) {
    if (ABSL_PREDICT_FALSE(ptr == nullptr)) {
      return;
    }
    if (ABSL_PREDICT_TRUE(IsSelSanMemory(ptr))) {
      size_t size_class =
          tc_globals.pagemap().sizeclass(PageIdContainingTagged(ptr));
      size_t size = tc_globals.sizemap().class_to_size(size_class);
      ptr = selsan::UpdateTag(ptr, size);
      FreeSmall(ptr, std::nullopt, size_class);
      return;
    }
  }
  TC_ASSERT_NE(ptr, nullptr);

  // ptr must be a result of a previous malloc/memalign/... call, and
  // therefore static initialization must have already occurred.
  TC_ASSERT(tc_globals.IsInited());

  size_t size_class = tc_globals.pagemap().sizeclass(PageIdContaining(ptr));
  if (ABSL_PREDICT_TRUE(size_class != 0)) {
    FreeSmall(ptr, std::nullopt, size_class);
  } else {
    SLOW_PATH_BARRIER();
    InvokeHooksAndFreePages(ptr, std::nullopt, policy);
  }
}

template <typename Policy>
ABSL_ATTRIBUTE_NOINLINE static void free_non_normal(void* ptr, size_t size,
                                                    Policy policy) {
  TC_ASSERT_NE(ptr, nullptr);
  if (GetMemoryTag(ptr) == MemoryTag::kSampled) {
    // we don't know true class size of the ptr
    return InvokeHooksAndFreePages(ptr, size, policy);
  }
  TC_ASSERT_EQ(GetMemoryTag(ptr), MemoryTag::kCold);
  size_t size_class;
  if (ABSL_PREDICT_FALSE(!tc_globals.sizemap().GetSizeClass(
          policy.AccessAsCold(), size, &size_class))) {
    // We couldn't calculate the size class, which means size > kMaxSize.
    TC_ASSERT(size > kMaxSize || policy.align() > alignof(std::max_align_t));
    static_assert(kMaxSize >= kPageSize, "kMaxSize must be at least kPageSize");
    return InvokeHooksAndFreePages(ptr, size, policy);
  }
  FreeSmall(ptr, size, size_class);
}

template <typename Policy>
inline ABSL_ATTRIBUTE_ALWAYS_INLINE void do_free_with_size(void* ptr,
                                                           size_t size,
                                                           Policy policy) {
  TC_ASSERT(
      CorrectAlignment(ptr, static_cast<std::align_val_t>(policy.align())));

  // This is an optimized path that may be taken if the binary is compiled
  // with -fsized-delete. We attempt to discover the size class cheaply
  // without any cache misses by doing a plain computation that
  // maps from size to size-class.
  //
  // The optimized path doesn't work with non-normal objects (sampled, cold),
  // whose deletions trigger more operations and require to visit metadata.
  if (ABSL_PREDICT_FALSE(!IsNormalMemory(ptr))) {
    if (ABSL_PREDICT_TRUE(ptr == nullptr)) {
      return;
    }
    if (ABSL_PREDICT_TRUE(IsSelSanMemory(ptr))) {
      TC_ASSERT(CorrectSize(ptr, size, policy));
      size_t size_class = tc_globals.sizemap().SizeClass(
          policy.InSameNumaPartitionAs(ptr), size);
      size = tc_globals.sizemap().class_to_size(size_class);
      ptr = selsan::UpdateTag(ptr, size);
      FreeSmall(ptr, size, size_class);
      return;
    }
    // Outline cold path to avoid putting cold size lookup on the fast path.
    SLOW_PATH_BARRIER();
    return free_non_normal(ptr, size, policy);
  }

  // Mismatched-size-delete error detection for sampled memory is performed in
  // the slow path above in all builds.
  TC_ASSERT(CorrectSize(ptr, size, policy));

  // At this point, since ptr's tag bit is 1, it means that it
  // cannot be nullptr either. Thus all code below may rely on ptr != nullptr.
  TC_ASSERT_NE(ptr, nullptr);

  size_t size_class;
  if (ABSL_PREDICT_FALSE(!tc_globals.sizemap().GetSizeClass(
          policy.InSameNumaPartitionAs(ptr), size, &size_class))) {
    // We couldn't calculate the size class, which means size > kMaxSize.
    TC_ASSERT(size > kMaxSize || policy.align() > alignof(std::max_align_t));
    static_assert(kMaxSize >= kPageSize, "kMaxSize must be at least kPageSize");
    SLOW_PATH_BARRIER();
    return InvokeHooksAndFreePages(ptr, size, policy);
  }

  FreeSmall(ptr, size, size_class);
}

// Checks that an asserted object size for <ptr> is valid.
template <typename Policy>
bool CorrectSize(void* ptr, const size_t provided_size, Policy policy) {
  if (ptr == nullptr) return true;
  size_t size = provided_size;
  size_t minimum_size, maximum_size;
  size_t size_class = 0;
  const size_t actual = GetSize(ptr);
  // Round-up passed in size to how much tcmalloc allocates for that size.
  if (tc_globals.guardedpage_allocator().PointerIsMine(ptr)) {
    // For guarded allocations we recorded the actual requested size.
    minimum_size = maximum_size = actual;
  } else if (tc_globals.sizemap().GetSizeClass(policy, size, &size_class)) {
    size = maximum_size = tc_globals.sizemap().class_to_size(size_class);
  } else {
    size = maximum_size = BytesToLengthCeil(size).in_bytes();
    minimum_size = maximum_size - (kPageSize - 1u);
  }

  if (ABSL_PREDICT_TRUE(actual == size)) return true;
  // We might have had a cold size class, so actual > size.  If we did not use
  // size returning new, the caller may not know this occurred.
  //
  // Nonetheless, it is permitted to pass a size anywhere in [requested, actual]
  // to sized delete.
  if (actual > size && !IsNormalMemory(ptr)) {
    if (tc_globals.sizemap().GetSizeClass(policy.AccessAsCold(), size,
                                          &size_class)) {
      size = maximum_size = tc_globals.sizemap().class_to_size(size_class);
      if (ABSL_PREDICT_TRUE(actual == size)) {
        return true;
      }
    }
  }

  if (size_class > 0) {
    if (policy.align() > static_cast<size_t>(kAlignment)) {
      // Nontrivial alignment.  We might have used a larger size to satisify it.
      minimum_size = 0;
    } else {
      minimum_size = tc_globals.sizemap().class_to_size(size_class - 1);
    }
  }

  ReportMismatchedDelete(tc_globals, ptr, provided_size, minimum_size,
                         maximum_size);

  return false;
}

// Checks that an asserted object <ptr> has <align> alignment.
bool CorrectAlignment(void* ptr, std::align_val_t alignment) {
  size_t align = static_cast<size_t>(alignment);
  TC_ASSERT(absl::has_single_bit(align));
  if (GetMemoryTag(ptr) != MemoryTag::kSampled) {
    // TODO(b/404341539): Use stricter alignment than kAlignment when the object
    // size is larger.
    align = std::max(align, static_cast<size_t>(kAlignment));
  }
  if (ABSL_PREDICT_FALSE((reinterpret_cast<uintptr_t>(ptr) & (align - 1)) !=
                         0)) {
    ReportCorruptedFree(tc_globals, static_cast<std::align_val_t>(align), ptr);
    return false;
  }
  return true;
}

// Helpers for use by exported routines below or inside debugallocation.cc:

inline void do_malloc_stats() { PrintStats(1); }

inline int do_malloc_trim(size_t pad) {
  // We ignore pad for now and just do a best effort release of pages.
  static_cast<void>(pad);
  return MallocExtension_Internal_ReleaseMemoryToSystem(0) != 0 ? 1 : 0;
}

inline int do_mallopt(int cmd, int value) {
  return 1;  // Indicates error
}

#ifdef TCMALLOC_HAVE_STRUCT_MALLINFO
inline struct mallinfo do_mallinfo() {
  TCMallocStats stats;
  ExtractTCMallocStats(&stats, false);

  // Just some of the fields are filled in.
  struct mallinfo info;
  memset(&info, 0, sizeof(info));

  // Unfortunately, the struct contains "int" field, so some of the
  // size values will be truncated.
  info.arena = static_cast<int>(stats.pageheap.system_bytes);
  info.fsmblks = static_cast<int>(stats.thread_bytes + stats.central_bytes +
                                  stats.transfer_bytes);
  info.fordblks = static_cast<int>(stats.pageheap.free_bytes +
                                   stats.pageheap.unmapped_bytes);
  info.uordblks = static_cast<int>(InUseByApp(stats));

  return info;
}
#endif  // TCMALLOC_HAVE_STRUCT_MALLINFO

#ifdef TCMALLOC_HAVE_STRUCT_MALLINFO2
inline struct mallinfo2 do_mallinfo2() {
  TCMallocStats stats;
  ExtractTCMallocStats(&stats, false);

  // Just some of the fields are filled in.
  struct mallinfo2 info;
  memset(&info, 0, sizeof(info));

  info.arena = static_cast<size_t>(stats.pageheap.system_bytes);
  info.fsmblks = static_cast<size_t>(stats.thread_bytes + stats.central_bytes +
                                     stats.transfer_bytes);
  info.fordblks = static_cast<size_t>(stats.pageheap.free_bytes +
                                      stats.pageheap.unmapped_bytes);
  info.uordblks = static_cast<size_t>(InUseByApp(stats));

  return info;
}
#endif

}  // namespace
}  // namespace tcmalloc_internal
}  // namespace tcmalloc

using tcmalloc::tcmalloc_internal::CppPolicy;
#ifdef TCMALLOC_HAVE_STRUCT_MALLINFO
using tcmalloc::tcmalloc_internal::do_mallinfo;
#endif
#ifdef TCMALLOC_HAVE_STRUCT_MALLINFO2
using tcmalloc::tcmalloc_internal::do_mallinfo2;
#endif
using tcmalloc::tcmalloc_internal::do_malloc_stats;
using tcmalloc::tcmalloc_internal::do_malloc_trim;
using tcmalloc::tcmalloc_internal::do_mallopt;
using tcmalloc::tcmalloc_internal::GetThreadSampler;
using tcmalloc::tcmalloc_internal::MallocPolicy;
using tcmalloc::tcmalloc_internal::tc_globals;
using tcmalloc::tcmalloc_internal::UsePerCpuCache;

namespace tcmalloc {
namespace tcmalloc_internal {

template <typename Policy>
ABSL_ATTRIBUTE_NOINLINE static typename Policy::pointer_type
alloc_small_sampled_hooks_or_perthread(size_t size, size_t size_class,
                                       Policy policy, size_t weight) {
  if (ABSL_PREDICT_FALSE(size_class == 0)) {
    // This happens on the first call then the size class table is not inited.
    TC_ASSERT(tc_globals.IsInited());
    tc_globals.sizemap().GetSizeClass(policy, size, &size_class);
  }
  void* res;
  // If we are here because of sampling, try AllocateFast first.
  if (ABSL_PREDICT_TRUE(weight == 0) ||
      (res = tc_globals.cpu_cache().AllocateFast(size_class)) == nullptr) {
    if (UsePerCpuCache(tc_globals)) {
      res = tc_globals.cpu_cache().AllocateSlow(size_class);
    } else {
      res = ThreadCache::GetCache()->Allocate(size_class);
    }
    if (ABSL_PREDICT_FALSE(res == nullptr)) return policy.handle_oom(size);
  }
  __sized_ptr_t ptr = {res, tc_globals.sizemap().class_to_size(size_class)};
  if (ABSL_PREDICT_FALSE(weight != 0)) {
    ptr = SampleSmallAllocation(tc_globals, policy, size, weight, size_class,
                                ptr);
  }
  if (Policy::invoke_hooks()) {
    // TODO(b/273983652): Size returning tcmallocs call NewHooks with capacity
    // as requested_size
    MallocHook::InvokeNewHook({ptr.p, Policy::size_returning() ? ptr.n : size,
                               ptr.n, HookMemoryMutable::kMutable});
  }
  return Policy::as_pointer(ptr.p, ptr.n);
}

// Slow path implementation.
// This function is used by `fast_alloc` if the allocation requires page sized
// allocations or some complex logic is required such as initialization,
// invoking new/delete hooks, sampling, etc.
//
// TODO(b/130771275):  This function is marked as static, rather than appearing
// in the anonymous namespace, to workaround incomplete heapz filtering.
template <typename Policy>
#if defined(__clang__)
__attribute__((flatten))
#endif
ABSL_ATTRIBUTE_NOINLINE static
    typename Policy::pointer_type slow_alloc_small(size_t size,
                                                   uint32_t size_class,
                                                   Policy policy) {
  size_t weight = GetThreadSampler()->RecordedAllocationFast(size);
  if (ABSL_PREDICT_FALSE(weight != 0) ||
      ABSL_PREDICT_FALSE(tcmalloc::tcmalloc_internal::Static::HaveHooks()) ||
      ABSL_PREDICT_FALSE(!UsePerCpuCache(tc_globals))) {
    return alloc_small_sampled_hooks_or_perthread(size, size_class, policy,
                                                  weight);
  }

  void* res = tc_globals.cpu_cache().AllocateSlowNoHooks(size_class);
  if (ABSL_PREDICT_FALSE(res == nullptr)) return policy.handle_oom(size);
  return Policy::to_pointer(res, size_class);
}

template <typename Policy>
ABSL_ATTRIBUTE_NOINLINE static typename Policy::pointer_type slow_alloc_large(
    size_t size, Policy policy) {
  size_t weight = GetThreadSampler()->RecordAllocation(size);
  __sized_ptr_t res = do_malloc_pages(size, weight, policy);
  if (ABSL_PREDICT_FALSE(res.p == nullptr)) return policy.handle_oom(size);

  if (Policy::invoke_hooks()) {
    // TODO(b/273983652): Size returning tcmallocs call NewHooks with capacity
    // as requested_size
    MallocHook::InvokeNewHook({res.p, Policy::size_returning() ? res.n : size,
                               res.n, HookMemoryMutable::kMutable});
  }
  return Policy::as_pointer(res.p, res.n);
}

template <typename Policy, typename Pointer = typename Policy::pointer_type>
static inline Pointer ABSL_ATTRIBUTE_ALWAYS_INLINE fast_alloc(size_t size,
                                                              Policy policy) {
  // If size is larger than kMaxSize, it's not fast-path anymore. In
  // such case, GetSizeClass will return false, and we'll delegate to the slow
  // path. If malloc is not yet initialized, we may end up with size_class == 0
  // (regardless of size), but in this case should also delegate to the slow
  // path by the fast path check further down.
  size_t size_class;
  bool is_small = tc_globals.sizemap().GetSizeClass(policy, size, &size_class);
  if (ABSL_PREDICT_FALSE(!is_small)) {
    SLOW_PATH_BARRIER();
    TCMALLOC_MUSTTAIL return slow_alloc_large(size, policy);
  }

  // TryRecordAllocationFast() returns true if no extra logic is required, e.g.:
  // - this allocation does not need to be sampled
  // - no new/delete hooks need to be invoked
  // - no need to initialize thread globals, data or caches.
  // The method updates 'bytes until next sample' thread sampler counters.
  if (ABSL_PREDICT_FALSE(!GetThreadSampler()->TryRecordAllocationFast(size))) {
    SLOW_PATH_BARRIER();
    return slow_alloc_small(size, size_class, policy);
  }

  // Fast path implementation for allocating small size memory.
  // This code should only be reached if all of the below conditions are met:
  // - the size does not exceed the maximum size (size class > 0)
  // - cpu / thread cache data has been initialized.
  // - the allocation is not subject to sampling / gwp-asan.
  // - no new/delete hook is installed and required to be called.
  void* ret = tc_globals.cpu_cache().AllocateFast(size_class);
  if (ABSL_PREDICT_FALSE(ret == nullptr)) {
    SLOW_PATH_BARRIER();
    return slow_alloc_small(size, size_class, policy);
  }

  TC_ASSERT_NE(ret, nullptr);
  return Policy::to_pointer(ret, size_class);
}

}  // namespace tcmalloc_internal
}  // namespace tcmalloc

extern "C" void MallocHook_HooksChanged() {
  // A hook has been added, so we need to move off of the fast path.
  tc_globals.cpu_cache().MaybeForceSlowPath();
}

using tcmalloc::tcmalloc_internal::GetOwnership;
using tcmalloc::tcmalloc_internal::GetSize;

extern "C" size_t MallocExtension_Internal_GetAllocatedSize(const void* ptr) {
  TC_ASSERT(!ptr || GetOwnership(ptr) !=
                        tcmalloc::MallocExtension::Ownership::kNotOwned);
  return GetSize(ptr);
}

extern "C" void MallocExtension_Internal_MarkThreadBusy() {
  tc_globals.InitIfNecessary();

  if (UsePerCpuCache(tc_globals)) {
    return;
  }

  // Force creation of the cache.
  tcmalloc::tcmalloc_internal::ThreadCache::GetCache();
}

absl::StatusOr<tcmalloc::malloc_tracing_extension::AllocatedAddressRanges>
MallocTracingExtension_Internal_GetAllocatedAddressRanges() {
  tcmalloc::malloc_tracing_extension::AllocatedAddressRanges
      allocated_address_ranges;
  constexpr float kAllocatedSpansSizeReserveFactor = 1.2;
  constexpr int kMaxAttempts = 10;
  for (int i = 0; i < kMaxAttempts; i++) {
    int estimated_span_count = tc_globals.span_allocator().stats().total;

    // We need to avoid allocation events during GetAllocatedSpans, as that may
    // cause a deadlock on pageheap_lock. To this end, we ensure that the result
    // vector already has a capacity greater than the current total span count.
    allocated_address_ranges.spans.reserve(estimated_span_count *
                                           kAllocatedSpansSizeReserveFactor);
    int actual_span_count =
        tc_globals.pagemap().GetAllocatedSpans(allocated_address_ranges.spans);
    if (allocated_address_ranges.spans.size() == actual_span_count) {
      return allocated_address_ranges;
    }
    allocated_address_ranges.spans.clear();
  }
  return absl::InternalError(
      "Could not fetch all Spans due to insufficient reserved capacity in the "
      "output vector.");
}

tcmalloc::tcmalloc_internal::MadvisePreference TCMalloc_Internal_GetMadvise() {
  return tc_globals.system_allocator().madvise_preference();
}

void TCMalloc_Internal_SetMadvise(
    tcmalloc::tcmalloc_internal::MadvisePreference v) {
  tc_globals.system_allocator().set_madvise_preference(v);
}

//-------------------------------------------------------------------
// Exported routines
//-------------------------------------------------------------------

using tcmalloc::tcmalloc_internal::BytesToLengthCeil;
using tcmalloc::tcmalloc_internal::CorrectAlignment;
using tcmalloc::tcmalloc_internal::CorrectSize;
using tcmalloc::tcmalloc_internal::do_free;
using tcmalloc::tcmalloc_internal::do_free_with_size;
using tcmalloc::tcmalloc_internal::GetPageSize;
using tcmalloc::tcmalloc_internal::GetSizeAndSampled;
using tcmalloc::tcmalloc_internal::kMaxSize;
using tcmalloc::tcmalloc_internal::MultiplyOverflow;

// depends on TCMALLOC_HAVE_STRUCT_MALLINFO, so needs to come after that.
#ifndef TCMALLOC_INTERNAL_METHODS_ONLY
#include "tcmalloc/libc_override.h"
#else
#define TCMALLOC_ALIAS(tc_fn) \
  __attribute__((alias(#tc_fn), visibility("default")))
#endif  //  !TCMALLOC_INTERNAL_METHODS_ONLY

extern "C" ABSL_CACHELINE_ALIGNED void* TCMallocInternalMalloc(
    size_t size) noexcept {
  return fast_alloc(size, MallocPolicy());
}

extern "C" ABSL_CACHELINE_ALIGNED void* TCMallocInternalNew(size_t size) {
  return fast_alloc(size, CppPolicy());
}

extern "C" ABSL_CACHELINE_ALIGNED void* TCMallocInternalNewNothrow(
    size_t size, const std::nothrow_t&) noexcept {
  return fast_alloc(size, CppPolicy().Nothrow());
}

extern "C" ABSL_CACHELINE_ALIGNED __sized_ptr_t
TCMallocInternalSizeReturningNew(size_t size) {
  return fast_alloc(size, CppPolicy().SizeReturning());
}

extern "C" ABSL_CACHELINE_ALIGNED __sized_ptr_t
TCMallocInternalSizeReturningNewAligned(size_t size,
                                        std::align_val_t alignment) {
  TC_ASSERT(absl::has_single_bit(static_cast<size_t>(alignment)));
  return fast_alloc(size, CppPolicy().AlignAs(alignment).SizeReturning());
}

#ifndef TCMALLOC_INTERNAL_METHODS_ONLY
// Below we provide 'strong' implementations for size returning operator new
// operations. This is an early implementation of P0901. In the future:
// - libc++ implements a customized `__allocate_at_least()` which calls
//   `__size_returning_new(...)`
// - libc++ provides a weak default `__size_returning_new(...)` which is
//   implemented in terms of `{::operator new(...), n}`
// - tcmalloc provides strong implementations of `__size_returning_new`

extern "C" ABSL_CACHELINE_ALIGNED ABSL_ATTRIBUTE_SECTION(google_malloc)
__sized_ptr_t __size_returning_new(size_t size)
    TCMALLOC_ALIAS(TCMallocInternalSizeReturningNew);

extern "C" ABSL_CACHELINE_ALIGNED ABSL_ATTRIBUTE_SECTION(google_malloc)
__sized_ptr_t __size_returning_new_aligned(size_t size,
                                           std::align_val_t alignment)
    TCMALLOC_ALIAS(TCMallocInternalSizeReturningNewAligned);

extern "C" ABSL_CACHELINE_ALIGNED ABSL_ATTRIBUTE_SECTION(google_malloc)
__sized_ptr_t __size_returning_new_hot_cold(size_t size,
                                            __hot_cold_t hot_cold) {
  return fast_alloc(size, CppPolicy().AccessAs(hot_cold).SizeReturning());
}

extern "C" ABSL_CACHELINE_ALIGNED ABSL_ATTRIBUTE_SECTION(google_malloc)
__sized_ptr_t __size_returning_new_aligned_hot_cold(size_t size,
                                                    std::align_val_t alignment,
                                                    __hot_cold_t hot_cold) {
  TC_ASSERT(absl::has_single_bit(static_cast<size_t>(alignment)));
  return fast_alloc(
      size, CppPolicy().AlignAs(alignment).AccessAs(hot_cold).SizeReturning());
}
#endif  // !TCMALLOC_INTERNAL_METHODS_ONLY

extern "C" ABSL_CACHELINE_ALIGNED void* TCMallocInternalMemalign(
    size_t align, size_t size) noexcept {
  TC_ASSERT(absl::has_single_bit(align));
  return fast_alloc(size, MallocPolicy().AlignAs(align));
}

extern "C" ABSL_CACHELINE_ALIGNED void* TCMallocInternalNewAligned(
    size_t size, std::align_val_t alignment) {
  TC_ASSERT(absl::has_single_bit(static_cast<size_t>(alignment)));
  return fast_alloc(size, CppPolicy().AlignAs(alignment));
}

extern "C" ABSL_CACHELINE_ALIGNED void* TCMallocInternalNewAlignedNothrow(
    size_t size, std::align_val_t alignment, const std::nothrow_t&) noexcept {
  TC_ASSERT(absl::has_single_bit(static_cast<size_t>(alignment)));
  return fast_alloc(size, CppPolicy().Nothrow().AlignAs(alignment));
}

extern "C" ABSL_CACHELINE_ALIGNED void* TCMallocInternalCalloc(
    size_t n, size_t elem_size) noexcept {
  size_t size;
  if (ABSL_PREDICT_FALSE(MultiplyOverflow(n, elem_size, &size))) {
    return MallocPolicy::handle_oom(std::numeric_limits<size_t>::max());
  }
  void* result = fast_alloc(size, MallocPolicy());
  if (ABSL_PREDICT_TRUE(result != nullptr)) {
    memset(result, 0, size);
  }
  return result;
}

static inline ABSL_ATTRIBUTE_ALWAYS_INLINE void* do_realloc(void* old_ptr,
                                                            size_t new_size) {
  tc_globals.InitIfNecessary();
  // Get the size of the old entry
  const auto [old_size, was_sampled] = GetSizeAndSampled(old_ptr);

  // Sampled allocations are reallocated and copied even if not strictly
  // necessary. This is problematic for very large allocations, since some old
  // programs rely on realloc to be very efficient (e.g. call realloc to the
  // same size repeatedly assuming it will do nothing). Very large allocations
  // are both all sampled and expensive to allocate and copy, so don't
  // reallocate them if not necessary. The use of kMaxSize here as a notion of
  // "very large" is somewhat arbitrary.
  const bool will_sample = new_size <= kMaxSize &&
                           GetThreadSampler()->WillRecordAllocation(new_size);

  // We could avoid doing this calculation in some scenarios by using if
  // statements to check old_size with new_size, but we chose to unconditionally
  // calculate the actual size for readability purposes.
  bool changes_correct_size;
  {
    size_t new_size_class;
    size_t actual_new_size;
    if (tc_globals.sizemap().GetSizeClass(MallocPolicy(), new_size,
                                          &new_size_class)) {
      actual_new_size = tc_globals.sizemap().class_to_size(new_size_class);
    } else {
      actual_new_size = BytesToLengthCeil(new_size).in_bytes();
    }
    changes_correct_size = actual_new_size != old_size;
  }

  if (changes_correct_size || was_sampled || will_sample ||
      tc_globals.guardedpage_allocator().PointerIsMine(old_ptr)) {
    // Need to reallocate.
    void* new_ptr = fast_alloc(new_size, MallocPolicy());
    if (new_ptr == nullptr) {
      return nullptr;
    }
    memcpy(new_ptr, old_ptr, ((old_size < new_size) ? old_size : new_size));
    // We could use a variant of do_free() that leverages the fact
    // that we already know the sizeclass of old_ptr.  The benefit
    // would be small, so don't bother.
    do_free(old_ptr, MallocPolicy());
    return new_ptr;
  } else {
    // We still need to call hooks to report the updated size:
    tcmalloc::MallocHook::InvokeDeleteHook(
        {const_cast<void*>(old_ptr), std::nullopt, old_size,
         tcmalloc::HookMemoryMutable::kImmutable});
    tcmalloc::MallocHook::InvokeNewHook(
        {const_cast<void*>(old_ptr), new_size, old_size,
         tcmalloc::HookMemoryMutable::kImmutable});
    // Assert that free_sized will work correctly.
    TC_ASSERT(CorrectSize(old_ptr, new_size, MallocPolicy()));
    return old_ptr;
  }
}

extern "C" ABSL_CACHELINE_ALIGNED void* TCMallocInternalRealloc(
    void* ptr, size_t size) noexcept {
  if (ptr == nullptr) {
    return fast_alloc(size, MallocPolicy());
  }
  if (size == 0) {
    do_free(ptr, MallocPolicy());
    return nullptr;
  }
  return do_realloc(ptr, size);
}

extern "C" ABSL_CACHELINE_ALIGNED void* TCMallocInternalReallocArray(
    void* ptr, size_t n, size_t elem_size) noexcept {
  size_t size;
  if (ABSL_PREDICT_FALSE(MultiplyOverflow(n, elem_size, &size))) {
    return MallocPolicy::handle_oom(std::numeric_limits<size_t>::max());
  }
  if (ptr == nullptr) {
    return fast_alloc(size, MallocPolicy());
  }
  if (size == 0) {
    do_free(ptr, MallocPolicy());
    return nullptr;
  }
  return do_realloc(ptr, size);
}

#ifndef TCMALLOC_INTERNAL_METHODS_ONLY
extern "C" ABSL_CACHELINE_ALIGNED ABSL_ATTRIBUTE_SECTION(google_malloc)
__sized_ptr_t tcmalloc_size_returning_operator_new_nothrow(
    size_t size) noexcept {
  return fast_alloc(size, CppPolicy().Nothrow().SizeReturning());
}

extern "C" ABSL_CACHELINE_ALIGNED ABSL_ATTRIBUTE_SECTION(google_malloc)
__sized_ptr_t tcmalloc_size_returning_operator_new_aligned_nothrow(
    size_t size, std::align_val_t alignment) noexcept {
  TC_ASSERT(absl::has_single_bit(static_cast<size_t>(alignment)));
  return fast_alloc(size,
                    CppPolicy().AlignAs(alignment).Nothrow().SizeReturning());
}

extern "C" ABSL_CACHELINE_ALIGNED ABSL_ATTRIBUTE_SECTION(google_malloc)
__sized_ptr_t tcmalloc_size_returning_operator_new_hot_cold_nothrow(
    size_t size, __hot_cold_t hot_cold) noexcept {
  return fast_alloc(size,
                    CppPolicy().AccessAs(hot_cold).Nothrow().SizeReturning());
}

extern "C" ABSL_CACHELINE_ALIGNED ABSL_ATTRIBUTE_SECTION(google_malloc)
__sized_ptr_t tcmalloc_size_returning_operator_new_aligned_hot_cold_nothrow(
    size_t size, std::align_val_t alignment, __hot_cold_t hot_cold) noexcept {
  TC_ASSERT(absl::has_single_bit(static_cast<size_t>(alignment)));
  return fast_alloc(size, CppPolicy()
                              .AlignAs(alignment)
                              .AccessAs(hot_cold)
                              .Nothrow()
                              .SizeReturning());
}
#endif  // !TCMALLOC_INTERNAL_METHODS_ONLY

extern "C" ABSL_CACHELINE_ALIGNED void TCMallocInternalFree(
    void* ptr) noexcept {
  do_free(ptr, MallocPolicy());
}

extern "C" ABSL_CACHELINE_ALIGNED void TCMallocInternalFreeSized(
    void* ptr, size_t size) noexcept {
  do_free_with_size(ptr, size, MallocPolicy());
}

extern "C" ABSL_CACHELINE_ALIGNED void TCMallocInternalFreeAlignedSized(
    void* ptr, size_t align, size_t size) noexcept {
  TC_ASSERT(absl::has_single_bit(align));
  do_free_with_size(ptr, size, MallocPolicy().AlignAs(align));
}

extern "C" void TCMallocInternalCfree(void* ptr) noexcept
    TCMALLOC_ALIAS(TCMallocInternalFree);

extern "C" ABSL_CACHELINE_ALIGNED void TCMallocInternalSdallocx(
    void* ptr, size_t size, int flags) noexcept {
  size_t alignment = alignof(std::max_align_t);

  if (ABSL_PREDICT_FALSE(flags != 0)) {
    TC_ASSERT_EQ(flags & ~0x3f, 0);
    alignment = static_cast<size_t>(1ull << (flags & 0x3f));
  }

  return do_free_with_size(ptr, size, MallocPolicy().AlignAs(alignment));
}

extern "C" void TCMallocInternalDelete(void* p) noexcept {
  return do_free(p, CppPolicy());
}

extern "C" void TCMallocInternalDeleteAligned(
    void* p, std::align_val_t alignment) noexcept
#if defined(NDEBUG)
    TCMALLOC_ALIAS(TCMallocInternalDelete);
#else
{
  // Note: The aligned delete/delete[] implementations differ slightly from
  // their respective aliased implementations to take advantage of checking the
  // passed-in alignment.
  TC_ASSERT(CorrectAlignment(p, alignment));
  return do_free(p, CppPolicy().AlignAs(alignment));
}
#endif

extern "C" ABSL_CACHELINE_ALIGNED void TCMallocInternalDeleteSized(
    void* p, size_t size) noexcept {
  do_free_with_size(p, size, CppPolicy());
}

extern "C" ABSL_CACHELINE_ALIGNED void TCMallocInternalDeleteSizedAligned(
    void* p, size_t t, std::align_val_t alignment) noexcept {
  TC_ASSERT(absl::has_single_bit(static_cast<size_t>(alignment)));
  return do_free_with_size(p, t, CppPolicy().AlignAs(alignment));
}

extern "C" void TCMallocInternalDeleteArraySized(void* p, size_t size) noexcept
    TCMALLOC_ALIAS(TCMallocInternalDeleteSized);

extern "C" void TCMallocInternalDeleteArraySizedAligned(
    void* p, size_t t, std::align_val_t alignment) noexcept
    TCMALLOC_ALIAS(TCMallocInternalDeleteSizedAligned);

// Standard C++ library implementations define and use this
// (via ::operator delete(ptr, nothrow)).
// But it's really the same as normal delete, so we just do the same thing.
extern "C" void TCMallocInternalDeleteNothrow(void* p,
                                              const std::nothrow_t&) noexcept
    TCMALLOC_ALIAS(TCMallocInternalDelete);

extern "C" void TCMallocInternalDeleteAlignedNothrow(
    void* p, std::align_val_t alignment, const std::nothrow_t&) noexcept
    TCMALLOC_ALIAS(TCMallocInternalDeleteAligned);

extern "C" void* TCMallocInternalNewArray(size_t size)
    TCMALLOC_ALIAS(TCMallocInternalNew);

extern "C" void* TCMallocInternalNewArrayAligned(size_t size,
                                                 std::align_val_t alignment)
    TCMALLOC_ALIAS(TCMallocInternalNewAligned);

extern "C" void* TCMallocInternalNewArrayNothrow(
    size_t size, const std::nothrow_t& nt) noexcept
    TCMALLOC_ALIAS(TCMallocInternalNewNothrow);

extern "C" void* TCMallocInternalNewArrayAlignedNothrow(
    size_t size, std::align_val_t alignment, const std::nothrow_t& nt) noexcept
    TCMALLOC_ALIAS(TCMallocInternalNewAlignedNothrow);

extern "C" void TCMallocInternalDeleteArray(void* p) noexcept
    TCMALLOC_ALIAS(TCMallocInternalDelete);

extern "C" void TCMallocInternalDeleteArrayAligned(
    void* p, std::align_val_t alignment) noexcept
    TCMALLOC_ALIAS(TCMallocInternalDeleteAligned);

extern "C" void TCMallocInternalDeleteArrayNothrow(
    void* p, const std::nothrow_t& nt) noexcept
    TCMALLOC_ALIAS(TCMallocInternalDelete);

extern "C" void TCMallocInternalDeleteArrayAlignedNothrow(
    void* p, std::align_val_t alignment, const std::nothrow_t& nt) noexcept
    TCMALLOC_ALIAS(TCMallocInternalDeleteAligned);

extern "C" ABSL_CACHELINE_ALIGNED void* TCMallocInternalAlignedAlloc(
    size_t align, size_t size) noexcept {
  // See https://www.open-std.org/jtc1/sc22/wg14/www/docs/summary.htm#dr_460.
  // The standard was updated to say that if align is not supported by the
  // implementation, a null pointer should be returned. We require alignment to
  // be greater than 0 and a power of 2.
  if (ABSL_PREDICT_FALSE(!absl::has_single_bit(align))) {
    // glibc, FreeBSD, and NetBSD manuals all document aligned_alloc() as
    // returning EINVAL if align is not a power of 2. We do the same.
    errno = EINVAL;
    return nullptr;
  }
  return fast_alloc(size, MallocPolicy().AlignAs(align));
}

extern "C" ABSL_CACHELINE_ALIGNED int TCMallocInternalPosixMemalign(
    void** result_ptr, size_t align, size_t size) noexcept {
  TC_ASSERT_NE(result_ptr, nullptr);
  if (ABSL_PREDICT_FALSE(((align % sizeof(void*)) != 0) ||
                         !absl::has_single_bit(align))) {
    return EINVAL;
  }
  void* result = fast_alloc(size, MallocPolicy().AlignAs(align));
  if (ABSL_PREDICT_FALSE(result == nullptr)) {
    return ENOMEM;
  }
  *result_ptr = result;
  return 0;
}

extern "C" ABSL_CACHELINE_ALIGNED void* TCMallocInternalValloc(
    size_t size) noexcept {
  // Allocate page-aligned object of length >= size bytes
  return fast_alloc(size, MallocPolicy().AlignAs(GetPageSize()));
}

extern "C" ABSL_CACHELINE_ALIGNED void* TCMallocInternalPvalloc(
    size_t size) noexcept {
  // Round up size to a multiple of pagesize
  size_t page_size = GetPageSize();
  if (size == 0) {     // pvalloc(0) should allocate one page, according to
    size = page_size;  // http://man.free4web.biz/man3/libmpatrol.3.html
  }
  size = (size + page_size - 1) & ~(page_size - 1);
  return fast_alloc(size, MallocPolicy().AlignAs(page_size));
}

extern "C" void TCMallocInternalMallocStats(void) noexcept {
  do_malloc_stats();
}

extern "C" int TCMallocInternalMallocTrim(size_t pad) noexcept {
  return do_malloc_trim(pad);
}

extern "C" int TCMallocInternalMallOpt(int cmd, int value) noexcept {
  return do_mallopt(cmd, value);
}

#ifdef TCMALLOC_HAVE_STRUCT_MALLINFO
extern "C" struct mallinfo TCMallocInternalMallInfo(void) noexcept {
  return do_mallinfo();
}
#endif

#ifdef TCMALLOC_HAVE_STRUCT_MALLINFO2
extern "C" struct mallinfo2 TCMallocInternalMallInfo2(void) noexcept {
  return do_mallinfo2();
}
#endif

extern "C" int TCMallocInternalMallocInfo(int opts ABSL_ATTRIBUTE_UNUSED,
                                          FILE* fp) noexcept {
  fputs("<malloc></malloc>\n", fp);
  return 0;
}

extern "C" size_t TCMallocInternalMallocSize(void* ptr) noexcept {
  if (ptr == nullptr) {
    return 0;
  }

  TC_ASSERT(GetOwnership(ptr) !=
            tcmalloc::MallocExtension::Ownership::kNotOwned);
  return GetSize(ptr);
}

extern "C" ABSL_CACHELINE_ALIGNED alloc_result_t
TCMallocInternalAllocAtLeast(size_t min_size) noexcept {
  auto sized_ptr = fast_alloc(min_size, MallocPolicy().SizeReturning());
  return alloc_result_t{sized_ptr.p, sized_ptr.n};
}

extern "C" ABSL_CACHELINE_ALIGNED alloc_result_t
TCMallocInternalAlignedAllocAtLeast(size_t alignment,
                                    size_t min_size) noexcept {
  // See https://www.open-std.org/jtc1/sc22/wg14/www/docs/summary.htm#dr_460.
  // The standard was updated to say that if align is not supported by the
  // implementation, a null pointer should be returned. We require alignment to
  // be greater than 0 and a power of 2.
  if (ABSL_PREDICT_FALSE(!absl::has_single_bit(alignment))) {
    // glibc, FreeBSD, and NetBSD manuals all document aligned_alloc() as
    // returning EINVAL if align is not a power of 2. We do the same.
    errno = EINVAL;
    return alloc_result_t{nullptr, 0};
  }
  auto sized_ptr =
      fast_alloc(min_size, MallocPolicy().AlignAs(alignment).SizeReturning());
  return alloc_result_t{sized_ptr.p, sized_ptr.n};
}

GOOGLE_MALLOC_SECTION_BEGIN
namespace tcmalloc {
namespace tcmalloc_internal {
namespace {

// The constructor allocates an object to ensure that initialization
// runs before main(), and therefore we do not have a chance to become
// multi-threaded before initialization.  We also create the TSD key
// here.  Presumably by the time this constructor runs, glibc is in
// good enough shape to handle pthread_key_create().
//
// The destructor prints stats when the program exits.
class TCMallocGuard {
 public:
  TCMallocGuard() {
    TCMallocInternalFree(TCMallocInternalMalloc(1));
    ThreadCache::InitTSD();
    TCMallocInternalFree(TCMallocInternalMalloc(1));
    // Ensure our MallocHook_HooksChanged implementation is linked in.
    MallocHook_HooksChanged();
  }
};

static TCMallocGuard module_enter_exit_hook;

}  // namespace
}  // namespace tcmalloc_internal
}  // namespace tcmalloc

#ifndef TCMALLOC_INTERNAL_METHODS_ONLY
ABSL_CACHELINE_ALIGNED void* operator new(
    size_t size, __hot_cold_t hot_cold) noexcept(false) {
  return fast_alloc(size, CppPolicy().AccessAs(hot_cold));
}

ABSL_CACHELINE_ALIGNED void* operator new(size_t size, const std::nothrow_t&,
                                          __hot_cold_t hot_cold) noexcept {
  return fast_alloc(size, CppPolicy().Nothrow().AccessAs(hot_cold));
}

ABSL_CACHELINE_ALIGNED void* operator new(
    size_t size, std::align_val_t align,
    __hot_cold_t hot_cold) noexcept(false) {
  TC_ASSERT(absl::has_single_bit(static_cast<size_t>(align)));
  return fast_alloc(size, CppPolicy().AlignAs(align).AccessAs(hot_cold));
}

ABSL_CACHELINE_ALIGNED void* operator new(size_t size, std::align_val_t align,
                                          const std::nothrow_t&,
                                          __hot_cold_t hot_cold) noexcept {
  TC_ASSERT(absl::has_single_bit(static_cast<size_t>(align)));
  return fast_alloc(size,
                    CppPolicy().Nothrow().AlignAs(align).AccessAs(hot_cold));
}

ABSL_CACHELINE_ALIGNED void* operator new[](
    size_t size, __hot_cold_t hot_cold) noexcept(false) {
  return fast_alloc(size, CppPolicy().AccessAs(hot_cold));
}

ABSL_CACHELINE_ALIGNED void* operator new[](size_t size, const std::nothrow_t&,
                                            __hot_cold_t hot_cold) noexcept {
  return fast_alloc(size, CppPolicy().Nothrow().AccessAs(hot_cold));
}

ABSL_CACHELINE_ALIGNED void* operator new[](
    size_t size, std::align_val_t align,
    __hot_cold_t hot_cold) noexcept(false) {
  TC_ASSERT(absl::has_single_bit(static_cast<size_t>(align)));
  return fast_alloc(size, CppPolicy().AlignAs(align).AccessAs(hot_cold));
}

ABSL_CACHELINE_ALIGNED void* operator new[](size_t size, std::align_val_t align,
                                            const std::nothrow_t&,
                                            __hot_cold_t hot_cold) noexcept {
  TC_ASSERT(absl::has_single_bit(static_cast<size_t>(align)));
  return fast_alloc(size,
                    CppPolicy().Nothrow().AlignAs(align).AccessAs(hot_cold));
}
#endif  // !TCMALLOC_INTERNAL_METHODS_ONLY

GOOGLE_MALLOC_SECTION_END
