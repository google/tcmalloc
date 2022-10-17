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

#include <memory>

#include "tcmalloc/cpu_cache.h"
#include "tcmalloc/internal/logging.h"
#include "tcmalloc/malloc_extension.h"
#include "tcmalloc/pagemap.h"
#include "tcmalloc/sampler.h"
#include "tcmalloc/span.h"
#include "tcmalloc/stack_trace_table.h"
#include "tcmalloc/tcmalloc_policy.h"
#include "tcmalloc/thread_cache.h"

GOOGLE_MALLOC_SECTION_BEGIN
namespace tcmalloc::tcmalloc_internal {

// This function computes a profile that maps a live stack trace to
// the number of bytes of central-cache memory pinned by an allocation
// at that stack trace.
// In the case when span is hosting >= 1 number of small objects (t.proxy !=
// nullptr), we call span::Fragmentation() and read `span->allocated_`. It is
// safe to do so since we hold the per-sample lock while iterating over sampled
// allocations. It prevents the sampled allocation that has the proxy object to
// complete deallocation, thus `proxy` can not be returned to the span yet. It
// thus prevents the central free list to return the span to the page heap.
template <typename State>
static std::unique_ptr<const ProfileBase> DumpFragmentationProfile(
    State& state) {
  auto profile = std::make_unique<StackTraceTable>(ProfileType::kFragmentation);
  state.sampled_allocation_recorder().Iterate(
      [&state, &profile](const SampledAllocation& sampled_allocation) {
        // Compute fragmentation to charge to this sample:
        const StackTrace& t = sampled_allocation.sampled_stack;
        if (t.proxy == nullptr) {
          // There is just one object per-span, and neighboring spans
          // can be released back to the system, so we charge no
          // fragmentation to this sampled object.
          return;
        }

        // Fetch the span on which the proxy lives so we can examine its
        // co-residents.
        const PageId p = PageIdContaining(t.proxy);
        Span* span = state.pagemap().GetDescriptor(p);
        if (span == nullptr) {
          // Avoid crashes in production mode code, but report in tests.
          ASSERT(span != nullptr);
          return;
        }

        const double frag = span->Fragmentation(t.allocated_size);
        if (frag > 0) {
          // Associate the memory warmth with the actual object, not the proxy.
          // The residency information (t.span_start_address) is likely not very
          // useful, but we might as well pass it along.
          profile->AddTrace(frag, t);
        }
      });
  return profile;
}

template <typename State>
static std::unique_ptr<const ProfileBase> DumpHeapProfile(State& state) {
  auto profile = std::make_unique<StackTraceTable>(ProfileType::kHeap);
  state.sampled_allocation_recorder().Iterate(
      [&](const SampledAllocation& sampled_allocation) {
        profile->AddTrace(1.0, sampled_allocation.sampled_stack);
      });
  return profile;
}

ABSL_CONST_INIT static thread_local Sampler thread_sampler_
    ABSL_ATTRIBUTE_INITIAL_EXEC;

inline Sampler* GetThreadSampler() { return &thread_sampler_; }

// If this allocation can be guarded, and if it's time to do a guarded sample,
// returns a guarded allocation Span.  Otherwise returns nullptr.
template <typename State>
static void* TrySampleGuardedAllocation(State& state, size_t size,
                                        size_t alignment, Length num_pages) {
  if (num_pages == Length(1) &&
      GetThreadSampler()->ShouldSampleGuardedAllocation()) {
    // The num_pages == 1 constraint ensures that size <= kPageSize.  And since
    // alignments above kPageSize cause size_class == 0, we're also guaranteed
    // alignment <= kPageSize
    //
    // In all cases kPageSize <= GPA::page_size_, so Allocate's preconditions
    // are met.
    return state.guardedpage_allocator().Allocate(size, alignment);
  }
  return nullptr;
}

// ShouldSampleAllocation() is called when an allocation of the given requested
// size is in progress. It returns the sampling weight of the allocation if it
// should be "sampled," and 0 otherwise. See SampleifyAllocation().
//
// Sampling is done based on requested sizes and later unskewed during profile
// generation.
inline size_t ShouldSampleAllocation(size_t size) {
  return GetThreadSampler()->RecordAllocation(size);
}

template <typename State>
ABSL_ATTRIBUTE_NOINLINE static inline void FreeProxyObject(State& state,
                                                           void* ptr,
                                                           size_t size_class) {
  if (ABSL_PREDICT_TRUE(UsePerCpuCache(state))) {
    state.cpu_cache().Deallocate(ptr, size_class);
  } else if (ThreadCache* cache = ThreadCache::GetCacheIfPresent();
             ABSL_PREDICT_TRUE(cache)) {
    cache->Deallocate(ptr, size_class);
  } else {
    // This thread doesn't have thread-cache yet or already. Delete directly
    // into transfer cache.
    state.transfer_cache().InsertRange(size_class, absl::Span<void*>(&ptr, 1));
  }
}

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
template <typename State, typename Policy>
static void* SampleifyAllocation(State& state, Policy policy,
                                 size_t requested_size, size_t weight,
                                 size_t size_class, void* obj, Span* span,
                                 size_t* capacity) {
  CHECK_CONDITION((size_class != 0 && obj != nullptr && span == nullptr) ||
                  (size_class == 0 && obj == nullptr && span != nullptr));

  void* proxy = nullptr;
  void* guarded_alloc = nullptr;
  size_t allocated_size;
  bool allocated_cold;

  // requested_alignment = 1 means 'small size table alignment was used'
  // Historically this is reported as requested_alignment = 0
  size_t requested_alignment = policy.align();
  if (requested_alignment == 1) {
    requested_alignment = 0;
  }

  if (size_class != 0) {
    ASSERT(size_class == state.pagemap().sizeclass(PageIdContaining(obj)));

    allocated_size = state.sizemap().class_to_size(size_class);
    allocated_cold = IsExpandedSizeClass(size_class);

    // If the caller didn't provide a span, allocate one:
    Length num_pages = BytesToLengthCeil(allocated_size);
    if ((guarded_alloc = TrySampleGuardedAllocation(
             state, requested_size, requested_alignment, num_pages))) {
      ASSERT(IsSampledMemory(guarded_alloc));
      const PageId p = PageIdContaining(guarded_alloc);
      absl::base_internal::SpinLockHolder h(&pageheap_lock);
      span = Span::New(p, num_pages);
      state.pagemap().Set(p, span);
      // If we report capacity back from a size returning allocation, we can not
      // report the allocated_size, as we guard the size to 'requested_size',
      // and we maintain the invariant that GetAllocatedSize() must match the
      // returned size from size returning allocations. So in that case, we
      // report the requested size for both capacity and GetAllocatedSize().
      if (capacity) allocated_size = requested_size;
    } else if ((span = state.page_allocator().New(
                    num_pages, 1, MemoryTag::kSampled)) == nullptr) {
      if (capacity) *capacity = allocated_size;
      return obj;
    }

    size_t span_size =
        Length(state.sizemap().class_to_pages(size_class)).in_bytes();
    size_t objects_per_span = span_size / allocated_size;

    if (objects_per_span != 1) {
      ASSERT(objects_per_span > 1);
      proxy = obj;
      obj = nullptr;
    }
  } else {
    // Set allocated_size to the exact size for a page allocation.
    // NOTE: if we introduce gwp-asan sampling / guarded allocations
    // for page allocations, then we need to revisit do_malloc_pages as
    // the current assumption is that only class sized allocs are sampled
    // for gwp-asan.
    allocated_size = span->bytes_in_span();
    allocated_cold = IsColdMemory(span->start_address());
  }
  if (capacity) *capacity = allocated_size;

  ASSERT(span != nullptr);

  // Grab the stack trace outside the heap lock.
  StackTrace tmp;
  tmp.proxy = proxy;
  tmp.depth = absl::GetStackTrace(tmp.stack, kMaxStackDepth, 0);
  tmp.requested_size = requested_size;
  tmp.requested_alignment = requested_alignment;
  tmp.requested_size_returning = capacity != nullptr;
  tmp.allocated_size = allocated_size;
  tmp.sampled_alloc_handle = state.sampled_alloc_handle_generator.fetch_add(
                                 1, std::memory_order_relaxed) +
                             1;
  tmp.access_hint = static_cast<uint8_t>(policy.access());
  tmp.cold_allocated = allocated_cold;
  tmp.weight = weight;
  tmp.span_start_address = span->start_address();
  tmp.allocation_time = absl::Now();

  // How many allocations does this sample represent, given the sampling
  // frequency (weight) and its size.
  const double allocation_estimate =
      static_cast<double>(weight) / (requested_size + 1);

  // Adjust our estimate of internal fragmentation.
  ASSERT(requested_size <= allocated_size);
  if (requested_size < allocated_size) {
    state.sampled_internal_fragmentation_.Add(
        allocation_estimate * (allocated_size - requested_size));
  }

  state.allocation_samples.ReportMalloc(tmp);

  state.deallocation_samples.ReportMalloc(tmp);

  // The SampledAllocation object is visible to readers after this. Readers only
  // care about its various metadata (e.g. stack trace, weight) to generate the
  // heap profile, and won't need any information from Span::Sample() next.
  SampledAllocation* sampled_allocation =
      state.sampled_allocation_recorder().Register(std::move(tmp));
  // No pageheap_lock required. The span is freshly allocated and no one else
  // can access it. It is visible after we return from this allocation path.
  span->Sample(sampled_allocation);

  state.peak_heap_tracker().MaybeSaveSample();

  if (obj != nullptr) {
    // We are not maintaining precise statistics on malloc hit/miss rates at our
    // cache tiers.  We can deallocate into our ordinary cache.
    ASSERT(size_class != 0);
    FreeProxyObject(state, obj, size_class);
  }
  return guarded_alloc ? guarded_alloc : span->start_address();
}

template <typename State>
inline void MaybeUnsampleAllocation(State& state, void* ptr, Span* span) {
  // No pageheap_lock required. The sampled span should be unmarked and have its
  // state cleared only once. External synchronization when freeing is required;
  // otherwise, concurrent writes here would likely report a double-free.
  if (SampledAllocation* sampled_allocation = span->Unsample()) {
    void* const proxy = sampled_allocation->sampled_stack.proxy;
    const size_t weight = sampled_allocation->sampled_stack.weight;
    const size_t requested_size =
        sampled_allocation->sampled_stack.requested_size;
    const size_t allocated_size =
        sampled_allocation->sampled_stack.allocated_size;
    const size_t alignment =
        sampled_allocation->sampled_stack.requested_alignment;
    // How many allocations does this sample represent, given the sampling
    // frequency (weight) and its size.
    const double allocation_estimate =
        static_cast<double>(weight) / (requested_size + 1);
    AllocHandle sampled_alloc_handle =
        sampled_allocation->sampled_stack.sampled_alloc_handle;
    state.sampled_allocation_recorder().Unregister(sampled_allocation);

    // Adjust our estimate of internal fragmentation.
    ASSERT(requested_size <= allocated_size);
    if (requested_size < allocated_size) {
      const size_t sampled_fragmentation =
          allocation_estimate * (allocated_size - requested_size);

      // Check against wraparound
      ASSERT(state.sampled_internal_fragmentation_.value() >=
             sampled_fragmentation);
      state.sampled_internal_fragmentation_.Add(-sampled_fragmentation);
    }

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
      ASSERT(size_class == state.pagemap().sizeclass(PageIdContaining(proxy)));
      FreeProxyObject(state, proxy, size_class);
    }
  }
}

template <typename State, typename Policy, typename CapacityPtr>
static void* SampleLargeAllocation(State& state, Policy policy,
                                   size_t requested_size, size_t weight,
                                   Span* span, CapacityPtr capacity) {
  return SampleifyAllocation(state, policy, requested_size, weight, 0, nullptr,
                             span, capacity);
}

template <typename State, typename Policy, typename CapacityPtr>
static void* SampleSmallAllocation(State& state, Policy policy,
                                   size_t requested_size, size_t weight,
                                   size_t size_class, void* obj,
                                   CapacityPtr capacity) {
  return SampleifyAllocation(state, policy, requested_size, weight, size_class,
                             obj, nullptr, capacity);
}

}  // namespace tcmalloc::tcmalloc_internal
GOOGLE_MALLOC_SECTION_END

#endif  // TCMALLOC_ALLOCATION_SAMPLING_H_
