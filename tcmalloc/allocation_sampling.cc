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

#include "tcmalloc/allocation_sampling.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>

#include "absl/base/attributes.h"
#include "absl/base/optimization.h"
#include "absl/debugging/stacktrace.h"
#include "absl/time/clock.h"
#include "absl/time/time.h"
#include "absl/types/span.h"
#include "tcmalloc/common.h"
#include "tcmalloc/cpu_cache.h"
#include "tcmalloc/guarded_allocations.h"
#include "tcmalloc/internal/config.h"
#include "tcmalloc/internal/exponential_biased.h"
#include "tcmalloc/internal/logging.h"
#include "tcmalloc/internal/sampled_allocation.h"
#include "tcmalloc/malloc_extension.h"
#include "tcmalloc/pagemap.h"
#include "tcmalloc/pages.h"
#include "tcmalloc/parameters.h"
#include "tcmalloc/span.h"
#include "tcmalloc/stack_trace_table.h"
#include "tcmalloc/tcmalloc_policy.h"
#include "tcmalloc/thread_cache.h"

GOOGLE_MALLOC_SECTION_BEGIN
namespace tcmalloc::tcmalloc_internal {

std::unique_ptr<const ProfileBase> DumpFragmentationProfile(Static& state) {
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

std::unique_ptr<const ProfileBase> DumpHeapProfile(Static& state) {
  auto profile = std::make_unique<StackTraceTable>(ProfileType::kHeap);
  state.sampled_allocation_recorder().Iterate(
      [&](const SampledAllocation& sampled_allocation) {
        profile->AddTrace(1.0, sampled_allocation.sampled_stack);
      });
  return profile;
}

// If this allocation can be guarded, and if it's time to do a guarded sample,
// returns an instance of GuardedAllocWithStatus, that includes guarded
// allocation Span and guarded status. Otherwise, returns nullptr and the status
// indicating why the allocation may not be guarded.
template <typename State>
static GuardedAllocWithStatus TrySampleGuardedAllocation(
    State& state, size_t size, size_t alignment, Length num_pages,
    const StackTrace& stack_trace) {
  if (num_pages != Length(1)) {
    return {nullptr, Profile::Sample::GuardedStatus::LargerThanOnePage};
  }
  const int64_t guarded_sampling_rate =
      tcmalloc::tcmalloc_internal::Parameters::guarded_sampling_rate();
  if (guarded_sampling_rate < 0) {
    return {nullptr, Profile::Sample::GuardedStatus::Disabled};
  }
  // TODO(b/273954799): Possible optimization: only calculate this when
  // guarded_sampling_rate or profile_sampling_rate change.  Likewise for
  // margin_multiplier below.
  const int64_t profile_sampling_rate =
      tcmalloc::tcmalloc_internal::Parameters::profile_sampling_rate();
  // If guarded_sampling_rate == 0, then attempt to guard, as usual.
  if (guarded_sampling_rate > 0) {
    const double configured_sampled_to_guarded_ratio =
        profile_sampling_rate > 0
            ? std::ceil(guarded_sampling_rate / profile_sampling_rate)
            : 1.0;
    // Select a multiplier of the configured_sampled_to_guarded_ratio that
    // yields a product at least 2.0 greater than
    // configured_sampled_to_guarded_ratio.
    //
    //    ((configured_sampled_to_guarded_ratio * margin_multiplier) -
    //        configured_sampled_to_guarded_ratio) >= 2.0
    //
    // This multiplier is applied to configured_sampled_to_guarded_ratio to
    // create the maximum, and configured_sampled_to_guarded_ratio is the
    // minimum. This max and min are the boundaries between which
    // current_sampled_to_guarded_ratio travels between.  While traveling
    // towards the maximum, no guarding is attempted.  While traveling towards
    // the minimum, guarding of every sample is attempted.
    const double margin_multiplier =
        std::clamp((configured_sampled_to_guarded_ratio + 2.0) /
                       configured_sampled_to_guarded_ratio,
                   1.2, 2.0);
    const double maximum_configured_sampled_to_guarded_ratio =
        margin_multiplier * configured_sampled_to_guarded_ratio;
    const double minimum_configured_sampled_to_guarded_ratio =
        configured_sampled_to_guarded_ratio;

    // Ensure that successful_allocations is at least 1 (not zero).
    const size_t successful_allocations =
        std::max(state.guardedpage_allocator().SuccessfulAllocations(), 1UL);
    const size_t current_sampled_to_guarded_ratio =
        state.total_sampled_count_.value() / successful_allocations;
    static std::atomic<bool> striving_to_guard_{true};
    if (striving_to_guard_.load(std::memory_order_relaxed)) {
      if (current_sampled_to_guarded_ratio <=
          minimum_configured_sampled_to_guarded_ratio) {
        striving_to_guard_.store(false, std::memory_order_relaxed);
        return {nullptr, Profile::Sample::GuardedStatus::RateLimited};
      }
      // Fall through to possible allocation below.
    } else {
      if (current_sampled_to_guarded_ratio >=
          maximum_configured_sampled_to_guarded_ratio) {
        striving_to_guard_.store(true, std::memory_order_relaxed);
        // Fall through to possible allocation below.
      } else {
        return {nullptr, Profile::Sample::GuardedStatus::RateLimited};
      }
    }

    size_t guard_count = state.stacktrace_filter().Count(stack_trace);
    if (guard_count >= kMaxGuardsPerStackTraceSignature) {
      return {nullptr, Profile::Sample::GuardedStatus::Filtered};
    }
    ABSL_CONST_INIT static std::atomic<uint64_t> rnd_(0);
    uint64_t new_rnd = 0;
    if (guard_count > 0) {
      new_rnd =
          ExponentialBiased::NextRandom(rnd_.load(std::memory_order_relaxed));
      rnd_.store(new_rnd, std::memory_order_relaxed);
    }
    switch (guard_count) {
      case 0:
        // Fall through to allocation below.
        break;
      case 1:
        /* 25% */
        if (new_rnd % 4 != 0) {
          return {nullptr, Profile::Sample::GuardedStatus::Filtered};
        }
        break;
      case 2:
        /* 12.5% */
        if (new_rnd % 8 != 0) {
          return {nullptr, Profile::Sample::GuardedStatus::Filtered};
        }
        break;
      case 3:
        /* ~1% */
        if (new_rnd % 128 != 0) {
          return {nullptr, Profile::Sample::GuardedStatus::Filtered};
        }
        break;
      default:
        return {nullptr, Profile::Sample::GuardedStatus::Filtered};
    }
  }
  // The num_pages == 1 constraint ensures that size <= kPageSize.  And
  // since alignments above kPageSize cause size_class == 0, we're also
  // guaranteed alignment <= kPageSize
  //
  // In all cases kPageSize <= GPA::page_size_, so Allocate's preconditions
  // are met.
  GuardedAllocWithStatus alloc_with_status =
      state.guardedpage_allocator().Allocate(size, alignment);
  if (alloc_with_status.status == Profile::Sample::GuardedStatus::Guarded) {
    state.stacktrace_filter().Add(stack_trace);
  }
  return alloc_with_status;
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
sized_ptr_t SampleifyAllocation(Static& state, size_t requested_size,
                                size_t align, size_t weight, size_t size_class,
                                hot_cold_t access_hint, bool size_returning,
                                void* obj, Span* span) {
  CHECK_CONDITION((size_class != 0 && obj != nullptr && span == nullptr) ||
                  (size_class == 0 && obj == nullptr && span != nullptr));

  StackTrace stack_trace;
  stack_trace.proxy = nullptr;
  stack_trace.requested_size = requested_size;
  // Grab the stack trace outside the heap lock.
  stack_trace.depth = absl::GetStackTrace(stack_trace.stack, kMaxStackDepth, 0);

  // requested_alignment = 1 means 'small size table alignment was used'
  // Historically this is reported as requested_alignment = 0
  stack_trace.requested_alignment = align;
  if (stack_trace.requested_alignment == 1) {
    stack_trace.requested_alignment = 0;
  }

  stack_trace.requested_size_returning = size_returning;
  stack_trace.access_hint = static_cast<uint8_t>(access_hint);
  stack_trace.weight = weight;

  GuardedAllocWithStatus alloc_with_status{
      nullptr, Profile::Sample::GuardedStatus::NotAttempted};

  size_t capacity = 0;
  if (size_class != 0) {
    ASSERT(size_class == state.pagemap().sizeclass(PageIdContaining(obj)));

    stack_trace.allocated_size = state.sizemap().class_to_size(size_class);
    stack_trace.cold_allocated = IsExpandedSizeClass(size_class);

    Length num_pages = BytesToLengthCeil(stack_trace.allocated_size);
    alloc_with_status = TrySampleGuardedAllocation(
        state, requested_size, stack_trace.requested_alignment, num_pages,
        stack_trace);
    if (alloc_with_status.status == Profile::Sample::GuardedStatus::Guarded) {
      ASSERT(IsSampledMemory(alloc_with_status.alloc));
      const PageId p = PageIdContaining(alloc_with_status.alloc);
      PageHeapSpinLockHolder l;
      span = Span::New(p, num_pages);
      state.pagemap().Set(p, span);
      // If we report capacity back from a size returning allocation, we can not
      // report the stack_trace.allocated_size, as we guard the size to
      // 'requested_size', and we maintain the invariant that GetAllocatedSize()
      // must match the returned size from size returning allocations. So in
      // that case, we report the requested size for both capacity and
      // GetAllocatedSize().
      if (size_returning) {
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
    size_t objects_per_span = span_size / stack_trace.allocated_size;

    if (objects_per_span != 1) {
      ASSERT(objects_per_span > 1);
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
    stack_trace.cold_allocated = IsColdMemory(span->start_address());
    capacity = stack_trace.allocated_size;
  }

  // A span must be provided or created by this point.
  ASSERT(span != nullptr);

  stack_trace.sampled_alloc_handle =
      state.sampled_alloc_handle_generator.fetch_add(
          1, std::memory_order_relaxed) +
      1;
  stack_trace.span_start_address = span->start_address();
  stack_trace.allocation_time = absl::Now();
  stack_trace.guarded_status = static_cast<int>(alloc_with_status.status);

  // How many allocations does this sample represent, given the sampling
  // frequency (weight) and its size.
  const double allocation_estimate =
      static_cast<double>(weight) / (requested_size + 1);

  // Adjust our estimate of internal fragmentation.
  ASSERT(requested_size <= stack_trace.allocated_size);
  if (requested_size < stack_trace.allocated_size) {
    state.sampled_internal_fragmentation_.Add(
        allocation_estimate * (stack_trace.allocated_size - requested_size));
  }

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
    ASSERT(size_class != 0);
    FreeProxyObject(state, obj, size_class);
  }
  ASSERT(state.pagemap().sizeclass(span->first_page()) == 0);
  return {(alloc_with_status.alloc != nullptr) ? alloc_with_status.alloc
                                               : span->start_address(),
          capacity};
}

void MaybeUnsampleAllocation(Static& state, void* ptr, Span* span) {
  // No pageheap_lock required. The sampled span should be unmarked and have its
  // state cleared only once. External synchronization when freeing is required;
  // otherwise, concurrent writes here would likely report a double-free.
  if (SampledAllocation* sampled_allocation = span->Unsample()) {
    ASSERT(state.pagemap().sizeclass(PageIdContaining(ptr)) == 0);

    void* const proxy = sampled_allocation->sampled_stack.proxy;
    const size_t weight = sampled_allocation->sampled_stack.weight;
    const size_t requested_size =
        sampled_allocation->sampled_stack.requested_size;
    const size_t allocated_size =
        sampled_allocation->sampled_stack.allocated_size;
    // SampleifyAllocation turns alignment 1 into 0, turn it back for
    // SizeMap::SizeClass.
    const size_t alignment =
        sampled_allocation->sampled_stack.requested_alignment != 0
            ? sampled_allocation->sampled_stack.requested_alignment
            : 1;
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

}  // namespace tcmalloc::tcmalloc_internal
GOOGLE_MALLOC_SECTION_END
