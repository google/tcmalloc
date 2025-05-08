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

#include <cmath>
#include <cstddef>
#include <memory>
#include <optional>
#include <utility>

#include "absl/base/attributes.h"
#include "absl/base/optimization.h"
#include "absl/debugging/stacktrace.h"
#include "absl/time/clock.h"
#include "absl/time/time.h"
#include "absl/types/span.h"
#include "tcmalloc/common.h"
#include "tcmalloc/cpu_cache.h"
#include "tcmalloc/error_reporting.h"
#include "tcmalloc/guarded_allocations.h"
#include "tcmalloc/internal/config.h"
#include "tcmalloc/internal/exponential_biased.h"
#include "tcmalloc/internal/logging.h"
#include "tcmalloc/internal/sampled_allocation.h"
#include "tcmalloc/malloc_extension.h"
#include "tcmalloc/malloc_hook.h"
#include "tcmalloc/pagemap.h"
#include "tcmalloc/pages.h"
#include "tcmalloc/parameters.h"
#include "tcmalloc/span.h"
#include "tcmalloc/stack_trace_table.h"
#include "tcmalloc/static_vars.h"
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
          TC_ASSERT_NE(span, nullptr);
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
  profile->SetStartTime(absl::Now());
  state.sampled_allocation_recorder().Iterate(
      [&](const SampledAllocation& sampled_allocation) {
        profile->AddTrace(1.0, sampled_allocation.sampled_stack);
      });
  return profile;
}

ABSL_ATTRIBUTE_NOINLINE void FreeProxyObject(Static& state, void* ptr,
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

void MaybeUnsampleAllocation(Static& state, void* ptr,
                             std::optional<size_t> size, Span& span) {
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
