// Copyright 2022 The TCMalloc Authors
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

#ifndef TCMALLOC_ALLOCATION_SAMPLING_H_
#define TCMALLOC_ALLOCATION_SAMPLING_H_

#include <cstddef>
#include <memory>
#include <optional>

#include "absl/base/attributes.h"
#include "absl/debugging/stacktrace.h"
#include "tcmalloc/error_reporting.h"
#include "tcmalloc/internal/config.h"
#include "tcmalloc/internal/logging.h"
#include "tcmalloc/internal/percpu.h"
#include "tcmalloc/malloc_extension.h"
#include "tcmalloc/malloc_hook.h"
#include "tcmalloc/malloc_hook_invoke.h"
#include "tcmalloc/pagemap.h"
#include "tcmalloc/sampler.h"
#include "tcmalloc/span.h"
#include "tcmalloc/tcmalloc_policy.h"

GOOGLE_MALLOC_SECTION_BEGIN
namespace tcmalloc::tcmalloc_internal {

class Static;

// This function computes a profile that maps a live stack trace to
// the number of bytes of central-cache memory pinned by an allocation
// at that stack trace.
// In the case when span is hosting >= 1 number of small objects (t.proxy !=
// nullptr), we call span::Fragmentation() and read `span->allocated_`. It is
// safe to do so since we hold the per-sample lock while iterating over sampled
// allocations. It prevents the sampled allocation that has the proxy object to
// complete deallocation, thus `proxy` can not be returned to the span yet. It
// thus prevents the central free list to return the span to the page heap.
std::unique_ptr<const ProfileBase> DumpFragmentationProfile(Static& state);

std::unique_ptr<const ProfileBase> DumpHeapProfile(Static& state);

extern "C" ABSL_CONST_INIT thread_local Sampler tcmalloc_sampler
    ABSL_ATTRIBUTE_INITIAL_EXEC;

// Compiler needs to see definition of this variable to generate more
// efficient code for -fPIE/PIC. If the compiler does not see the definition
// it considers it may come from another dynamic library. So even for
// initial-exec model, it need to emit an access via GOT (GOTTPOFF).
// When it sees the definition, it can emit direct %fs:TPOFF access.
// So we provide a weak definition here, but the actual definition is in
// percpu_rseq_asm.S.
ABSL_CONST_INIT ABSL_ATTRIBUTE_WEAK thread_local Sampler tcmalloc_sampler
    ABSL_ATTRIBUTE_INITIAL_EXEC;

inline Sampler& GetThreadSampler() {
  static_assert(sizeof(Sampler) == TCMALLOC_SAMPLER_SIZE,
                "update TCMALLOC_SAMPLER_SIZE");
  static_assert(alignof(Sampler) == TCMALLOC_SAMPLER_ALIGN,
                "update TCMALLOC_SAMPLER_ALIGN");
  static_assert(Sampler::HotDataOffset() == TCMALLOC_SAMPLER_HOT_OFFSET,
                "update TCMALLOC_SAMPLER_HOT_OFFSET");
  return tcmalloc_sampler;
}

void FreeProxyObject(Static& state, void* absl_nonnull ptr, size_t size_class);

// Performs sampling for already occurred allocation of object.
//
// For very small object sizes, object is used as 'proxy' and full
// page with sampled marked is allocated instead.
//
// For medium-sized objects that have single instance per span,
// they're simply freed and fresh page span is allocated to represent
// sampling.
//
// For large objects (i.e. allocated with do_malloc_pages) they are
// also fully reused and their span is marked as sampled.
//
// Note that do_free_with_size assumes sampled objects have
// page-aligned addresses. Please change both functions if need to
// invalidate the assumption.
//
// Note that size_class might not match requested_size in case of
// memalign. I.e. when larger than requested allocation is done to
// satisfy alignment constraint.
//
// In case of out-of-memory condition when allocating span or
// stacktrace struct, this function simply cheats and returns original
// object. As if no sampling was requested.
template <typename Policy>
ABSL_ATTRIBUTE_NOINLINE sized_ptr_t SampleifyAllocation(
    Static& state, Policy policy, size_t requested_size, size_t weight,
    size_t size_class, void* absl_nullable obj, Span* absl_nullable span) {
  TC_CHECK((size_class != 0 && obj != nullptr && span == nullptr) ||
           (size_class == 0 && obj == nullptr && span != nullptr));

  StackTrace stack_trace;
  stack_trace.proxy = nullptr;
  stack_trace.requested_size = requested_size;
  // Grab the stack trace outside the heap lock.
  stack_trace.depth = absl::GetStackTrace(stack_trace.stack, kMaxStackDepth, 0);

  // requested_alignment = 1 means 'small size table alignment was used'
  // Historically this is reported as requested_alignment = 0
  stack_trace.requested_alignment = policy.align();
  if (stack_trace.requested_alignment == 1) {
    stack_trace.requested_alignment = 0;
  }

  stack_trace.requested_size_returning = policy.size_returning();
  stack_trace.access_hint = static_cast<uint8_t>(policy.access());
  stack_trace.weight = weight;

  // How many allocations does this sample represent, given the sampling
  // frequency (weight) and its size.
  const double allocation_estimate =
      static_cast<double>(weight) / (requested_size + 1);

  GuardedAllocWithStatus alloc_with_status{
      nullptr, Profile::Sample::GuardedStatus::NotAttempted};

  size_t capacity = 0;
  if (size_class != 0) {
    TC_ASSERT_EQ(size_class,
                 state.pagemap().sizeclass(PageIdContainingTagged(obj)));
    state.per_size_class_counts()[size_class].Add(allocation_estimate);

    stack_trace.allocated_size = state.sizemap().class_to_size(size_class);
    stack_trace.cold_allocated = IsExpandedSizeClass(size_class);

    Length num_pages = BytesToLengthCeil(stack_trace.allocated_size);
    alloc_with_status = state.guardedpage_allocator().TrySample(
        requested_size, stack_trace.requested_alignment, num_pages,
        stack_trace);
    if (alloc_with_status.status == Profile::Sample::GuardedStatus::Guarded) {
      TC_ASSERT(!IsNormalMemory(alloc_with_status.alloc));
      const PageId p = PageIdContaining(alloc_with_status.alloc);
#ifdef TCMALLOC_INTERNAL_LEGACY_LOCKING
      PageHeapSpinLockHolder l;
#endif  // TCMALLOC_INTERNAL_LEGACY_LOCKING
      span = Span::New(Range(p, num_pages));
      state.pagemap().Set(p, span);
      // If we report capacity back from a size returning allocation, we can not
      // report the stack_trace.allocated_size, as we guard the size to
      // 'requested_size', and we maintain the invariant that GetAllocatedSize()
      // must match the returned size from size returning allocations. So in
      // that case, we report the requested size for both capacity and
      // GetAllocatedSize().
      if (policy.size_returning()) {
        stack_trace.allocated_size = requested_size;
      }
      capacity = requested_size;
    } else if ((span = state.page_allocator().New(
                    num_pages, {1, AccessDensityPrediction::kSparse},
                    MemoryTag::kSampled)) == nullptr) {
      capacity = stack_trace.allocated_size;
      return {obj, capacity};
    } else {
      capacity = stack_trace.allocated_size;
    }

    size_t span_size =
        Length(state.sizemap().class_to_pages(size_class)).in_bytes();
    size_t objects_per_span = stack_trace.allocated_size != 0
                                  ? span_size / stack_trace.allocated_size
                                  : -1ul;

    if (objects_per_span != 1) {
      TC_ASSERT_GT(objects_per_span, 1);
      stack_trace.proxy = obj;
      obj = nullptr;
    }
  } else {
    // Set stack_trace.allocated_size to the exact size for a page allocation.
    // NOTE: if we introduce gwp-asan sampling / guarded allocations
    // for page allocations, then we need to revisit do_malloc_pages as
    // the current assumption is that only class sized allocs are sampled
    // for gwp-asan.
    stack_trace.allocated_size = span->bytes_in_span();
    stack_trace.cold_allocated =
        GetMemoryTag(span->start_address()) == MemoryTag::kCold;
    capacity = stack_trace.allocated_size;
  }

  // A span must be provided or created by this point.
  TC_ASSERT_NE(span, nullptr);

  // TODO(b/414876446): Add entropy to the handles generated.
  stack_trace.sampled_alloc_handle =
      AllocHandle(state.sampled_alloc_handle_generator.fetch_add(
                      1, std::memory_order_relaxed) +
                  1);
  stack_trace.span_start_address = span->start_address();
  stack_trace.allocation_time = absl::Now();
  stack_trace.guarded_status = alloc_with_status.status;
  stack_trace.allocation_type = policy.allocation_type();

  // Adjust our estimate of internal fragmentation.
  TC_ASSERT_LE(requested_size, stack_trace.allocated_size);
  if (requested_size < stack_trace.allocated_size) {
    state.sampled_internal_fragmentation_.Add(
        allocation_estimate * (stack_trace.allocated_size - requested_size));
  }

  MallocHook::SampledAlloc sampled_alloc = {
      .handle = stack_trace.sampled_alloc_handle,
      .requested_size = stack_trace.requested_size,
      .requested_alignment = stack_trace.requested_alignment,
      .allocated_size = stack_trace.allocated_size,
      .weight = allocation_estimate,
      .stack = absl::MakeSpan(stack_trace.stack, stack_trace.depth),
      .allocation_time = stack_trace.allocation_time,
      .ptr = (alloc_with_status.alloc != nullptr)
                 ? alloc_with_status.alloc
                 : stack_trace.span_start_address,
  };
  MallocHook::InvokeSampledNewHook(sampled_alloc);

  state.allocation_samples.ReportMalloc(stack_trace);

  state.deallocation_samples.ReportMalloc(stack_trace);

  // The SampledAllocation object is visible to readers after this. Readers only
  // care about its various metadata (e.g. stack trace, weight) to generate the
  // heap profile, and won't need any information from Span::Sample() next.
  SampledAllocation* sampled_allocation =
      state.sampled_allocation_recorder().Register(std::move(stack_trace));
  // No pageheap_lock required. The span is freshly allocated and no one else
  // can access it. It is visible after we return from this allocation path.
  span->Sample(sampled_allocation);

  state.peak_heap_tracker().MaybeSaveSample();

  if (obj != nullptr) {
    // We are not maintaining precise statistics on malloc hit/miss rates at our
    // cache tiers.  We can deallocate into our ordinary cache.
    TC_ASSERT_NE(size_class, 0);
    FreeProxyObject(state, obj, size_class);
  }
  TC_ASSERT_EQ(state.pagemap().sizeclass(span->first_page()), 0);
  return {(alloc_with_status.alloc != nullptr) ? alloc_with_status.alloc
                                               : span->start_address(),
          capacity};
}

void MaybeUnsampleAllocation(Static& state, void* absl_nonnull ptr,
                             std::optional<size_t> size, Span& span,
                             Profile::Sample::AllocationType type);

template <typename Policy>
static sized_ptr_t SampleLargeAllocation(Static& state, Policy policy,
                                         size_t requested_size, size_t weight,
                                         Span* span) {
  return SampleifyAllocation(state, policy, requested_size, weight, 0, nullptr,
                             span);
}

template <typename Policy>
static sized_ptr_t SampleSmallAllocation(Static& state, Policy policy,
                                         size_t requested_size, size_t weight,
                                         size_t size_class, sized_ptr_t res) {
  return SampleifyAllocation(state, policy, requested_size, weight, size_class,
                             res.p, nullptr);
}

// Rewrite type so that the allocation type falls into one of the categories we
// use for deallocations (new or malloc, not aligned new).
static inline Profile::Sample::AllocationType SimplifyType(
    Profile::Sample::AllocationType type) {
  using AllocationType = Profile::Sample::AllocationType;

  switch (type) {
    case AllocationType::New:
    case AllocationType::Malloc:
      return type;
    case AllocationType::AlignedMalloc:
      return AllocationType::Malloc;
  }

  ABSL_UNREACHABLE();
}

template <typename Policy>
void MaybeUnsampleAllocation(Static& state, Policy policy,
                             void* absl_nonnull ptr, std::optional<size_t> size,
                             Span& span) {
  // No pageheap_lock required. The sampled span should be unmarked and have its
  // state cleared only once. External synchronization when freeing is required;
  // otherwise, concurrent writes here would likely report a double-free.
  SampledAllocation* sampled_allocation = span.Unsample();
  if (sampled_allocation == nullptr) {
    if (ABSL_PREDICT_TRUE(size.has_value())) {
      const size_t maximum_size = span.bytes_in_span();
      const size_t minimum_size = maximum_size - (kPageSize - 1u);

      if (ABSL_PREDICT_FALSE(*size < minimum_size || *size > maximum_size)) {
        // While we don't have precise allocation-time information because this
        // span was not sampled, the deallocated object's purported size exceeds
        // the span it is on.  This is impossible and indicates corruption.
        ReportMismatchedDelete(state, ptr, *size, minimum_size, maximum_size);
      }
    }

    return;
  }

  TC_ASSERT_EQ(state.pagemap().sizeclass(PageIdContainingTagged(ptr)), 0);

  void* const proxy = sampled_allocation->sampled_stack.proxy;
  const size_t weight = sampled_allocation->sampled_stack.weight;
  const size_t requested_size =
      sampled_allocation->sampled_stack.requested_size;
  const size_t allocated_size =
      sampled_allocation->sampled_stack.allocated_size;
  if (size.has_value()) {
    if (sampled_allocation->sampled_stack.requested_size_returning) {
      if (ABSL_PREDICT_FALSE(
              !(requested_size <= *size && *size <= allocated_size))) {
        ReportMismatchedDelete(state, *sampled_allocation, *size,
                               requested_size, allocated_size);
      }
    } else if (ABSL_PREDICT_FALSE(size != requested_size)) {
      ReportMismatchedDelete(state, *sampled_allocation, *size, requested_size,
                             std::nullopt);
    }
  }

  if (auto dealloc_type = SimplifyType(policy.allocation_type()),
      alloc_type =
          SimplifyType(sampled_allocation->sampled_stack.allocation_type);
      ABSL_PREDICT_FALSE(dealloc_type != alloc_type)) {
    ReportMismatchedFree(
        state, ptr, sampled_allocation->sampled_stack.allocation_type,
        policy.allocation_type(),
        absl::MakeSpan(sampled_allocation->sampled_stack.stack,
                       sampled_allocation->sampled_stack.depth));
  }

  // Check pointer for misalignment.
  //
  // TODO(ckennelly): Eliminate redundant guarded check with
  // InvokeHooksAndFreePages.
  if (ABSL_PREDICT_FALSE(ptr != span.start_address()) &&
      sampled_allocation->sampled_stack.guarded_status !=
          Profile::Sample::GuardedStatus::Guarded) {
    ReportCorruptedFree(
        tc_globals, static_cast<std::align_val_t>(kPageSize), ptr,
        absl::MakeSpan(sampled_allocation->sampled_stack.stack,
                       sampled_allocation->sampled_stack.depth));
  }

  // SampleifyAllocation turns alignment 1 into 0, turn it back for
  // SizeMap::SizeClass.
  //
  // TODO(b/404341539): Make requested_alignment an optional and
  // std::align_val_t-typed.
  const size_t alignment =
      sampled_allocation->sampled_stack.requested_alignment != 0
          ? sampled_allocation->sampled_stack.requested_alignment
          : 1;

  // TODO(b/404341539):
  // * Handle situation where malloc is deallocated with free_aligned_sized, or
  //   vice-versa.
  if ((size.has_value() ||
       policy.allocation_type() == Profile::Sample::AllocationType::New) &&
      ABSL_PREDICT_FALSE(policy.align() != alignment)) {
    ReportMismatchedFree(
        state, ptr,
        sampled_allocation->sampled_stack.requested_alignment != 0
            ? std::make_optional(static_cast<std::align_val_t>(
                  sampled_allocation->sampled_stack.requested_alignment))
            : std::nullopt,
        policy.has_explicit_alignment()
            ? std::make_optional<std::align_val_t>(
                  static_cast<std::align_val_t>(policy.align()))
            : std::nullopt,
        absl::MakeSpan(sampled_allocation->sampled_stack.stack,
                       sampled_allocation->sampled_stack.depth));
  }

  // How many allocations does this sample represent, given the sampling
  // frequency (weight) and its size.
  const double allocation_estimate =
      static_cast<double>(weight) / (requested_size + 1);
  AllocHandle sampled_alloc_handle =
      sampled_allocation->sampled_stack.sampled_alloc_handle;
  MallocHook::SampledAlloc sampled_alloc = {
      .handle = sampled_alloc_handle,
      .requested_size = requested_size,
      .requested_alignment =
          sampled_allocation->sampled_stack.requested_alignment,
      .allocated_size = allocated_size,
      .weight = allocation_estimate,
      .stack = absl::MakeSpan(sampled_allocation->sampled_stack.stack,
                              sampled_allocation->sampled_stack.depth),
      .allocation_time = sampled_allocation->sampled_stack.allocation_time,
      .ptr = ptr,
  };
  state.sampled_allocation_recorder().Unregister(sampled_allocation);

  // Adjust our estimate of internal fragmentation.
  TC_ASSERT_LE(requested_size, allocated_size);
  if (requested_size < allocated_size) {
    const size_t sampled_fragmentation =
        allocation_estimate * (allocated_size - requested_size);

    // Check against wraparound
    TC_ASSERT_GE(state.sampled_internal_fragmentation_.value(),
                 sampled_fragmentation);
    state.sampled_internal_fragmentation_.Add(-sampled_fragmentation);
  }
  MallocHook::InvokeSampledDeleteHook(sampled_alloc);

  state.deallocation_samples.ReportFree(sampled_alloc_handle);

  if (proxy) {
    const auto policy = CppPolicy().InSameNumaPartitionAs(proxy);
    size_t size_class;
    if (AccessFromPointer(proxy) == AllocationAccess::kCold) {
      size_class = state.sizemap().SizeClass(
          policy.AccessAsCold().AlignAs(alignment), allocated_size);
    } else {
      size_class = state.sizemap().SizeClass(
          policy.AccessAsHot().AlignAs(alignment), allocated_size);
    }
    TC_ASSERT_EQ(size_class,
                 state.pagemap().sizeclass(PageIdContainingTagged(proxy)));
    FreeProxyObject(state, proxy, size_class);
  }
}

}  // namespace tcmalloc::tcmalloc_internal
GOOGLE_MALLOC_SECTION_END

#endif  // TCMALLOC_ALLOCATION_SAMPLING_H_
