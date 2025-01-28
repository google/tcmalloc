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

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "gtest/gtest.h"
#include "fuzztest/fuzztest.h"
#include "absl/base/attributes.h"
#include "absl/log/check.h"
#include "absl/time/time.h"
#include "tcmalloc/common.h"
#include "tcmalloc/huge_page_aware_allocator.h"
#include "tcmalloc/huge_page_filler.h"
#include "tcmalloc/huge_pages.h"
#include "tcmalloc/huge_region.h"
#include "tcmalloc/internal/logging.h"
#include "tcmalloc/mock_huge_page_static_forwarder.h"
#include "tcmalloc/pages.h"
#include "tcmalloc/sizemap.h"
#include "tcmalloc/span.h"
#include "tcmalloc/stats.h"

namespace tcmalloc::tcmalloc_internal {
namespace {
using huge_page_allocator_internal::FakeStaticForwarder;
using huge_page_allocator_internal::HugePageAwareAllocator;
using huge_page_allocator_internal::HugePageAwareAllocatorOptions;

class FakeStaticForwarderWithUnback : public FakeStaticForwarder {
 public:
  bool ReleasePages(Range r) {
    pending_release_ += r.n;
    release_callback_();
    pending_release_ -= r.n;

    return FakeStaticForwarder::ReleasePages(r);
  }

  Length pending_release_;
  std::function<void()> release_callback_;
};

void FuzzHPAA(const std::string& s) {
  const char* data = s.data();
  size_t size = s.size();

  if (size < 13 || size > 100000) {
    // size < 13 for needing some entropy to initialize huge page aware
    // allocator.
    //
    // size > 100000 for avoiding overly large inputs given we do extra
    // checking.
    return;
  }

#if ABSL_HAVE_ADDRESS_SANITIZER
  // Since asan introduces runtime overhead, limit size of fuzz targets further.
  if (size > 10000) {
    return;
  }
#endif

  // We interpret data as a small DSL for exploring the state space of
  // HugePageAwareAllocator.
  //
  // [0] - Memory tag.
  // [1] - HugeRegionsMode.
  // [2] - HugeCache release time
  // [3:4] - Reserved.
  // [5] - Dense tracker type
  // [6:12] - Reserved.
  //
  // TODO(b/271282540): Convert these to strongly typed fuzztest parameters.
  //
  // Afterwards, we read 9 bytes at a time until the buffer is exhausted.
  // [i + 0]        - Specifies an operation to perform on the allocator
  // [i + 1, i + 8] - Specifies an integer. We use this as a source of
  //                  deterministic entropy to allow inputs to be replayed.
  //                  For example, this input can provide a Length to
  //                  allocate, or the index of the previous allocation to
  //                  deallocate.

  constexpr MemoryTag kTagOptions[] = {
      MemoryTag::kSampled, MemoryTag::kNormalP0, MemoryTag::kNormalP1,
      MemoryTag::kNormal, MemoryTag::kCold};
  constexpr int kTagSize = sizeof(kTagOptions) / sizeof(MemoryTag);
  static_assert(kTagSize > 0);
  MemoryTag tag = kTagOptions[static_cast<uint8_t>(data[0]) % kTagSize];
  // Use kNormalP1 memory tag only if we have more than one NUMA partitions.
  tag = (kNumaPartitions == 1 && tag == MemoryTag::kNormalP1)
            ? MemoryTag::kNormalP0
            : tag;

  const HugeRegionUsageOption huge_region_option =
      static_cast<uint8_t>(data[1]) >= 128
          ? HugeRegionUsageOption::kDefault
          : HugeRegionUsageOption::kUseForAllLargeAllocs;

  const HugePageFillerDenseTrackerType dense_tracker_type =
      static_cast<uint8_t>(data[5]) >= 128
          ? HugePageFillerDenseTrackerType::kLongestFreeRangeAndChunks
          : HugePageFillerDenseTrackerType::kSpansAllocated;

  const int32_t huge_cache_release_s = std::max<int32_t>(data[2], 1);

  // data[6:12] - Reserve additional bytes for any features we might want to add
  // in the future.
  data += 13;
  size -= 13;

  // HugePageAwareAllocator can't be destroyed cleanly, so we store a pointer
  // to one and construct in place.
  void* p =
      malloc(sizeof(HugePageAwareAllocator<FakeStaticForwarderWithUnback>));
  HugePageAwareAllocatorOptions options;
  options.tag = tag;
  options.use_huge_region_more_often = huge_region_option;
  options.huge_cache_time = absl::Seconds(huge_cache_release_s);
  options.dense_tracker_type = dense_tracker_type;
  HugePageAwareAllocator<FakeStaticForwarderWithUnback>* allocator;
  allocator =
      new (p) HugePageAwareAllocator<FakeStaticForwarderWithUnback>(options);
  auto& forwarder = allocator->forwarder();

  struct SpanInfo {
    Span* span;
    size_t objects_per_span;
  };
  std::vector<SpanInfo> allocs;
  Length allocated;
  PageReleaseStats expected_stats;

  std::vector<std::pair<const char*, size_t>> reentrant;
  std::string output;
  output.resize(1 << 20);

  auto run_dsl = [&](const char* data, size_t size) {
    for (size_t i = 0; i + 9 <= size; i += 9) {
      const uint16_t op = data[i];
      uint64_t value;
      memcpy(&value, &data[i + 1], sizeof(value));

      switch (op & 0x7) {
        case 0: {
          // Aligned allocate.  We divide up our random value by:
          //
          // value[0:15]  - We choose a Length to allocate.
          // value[16:31] - We select num_to_objects, i.e. the number of
          //                objects to allocate.
          // value[32:47] - Alignment.
          // value[48]    - Should we use aligned allocate?
          // value[49]    - Is the span sparsely- or densely-accessed?
          // value[63:50] - Reserved.
          Length length(std::clamp<size_t>(value & 0xFFFF, 1,
                                           kPagesPerHugePage.raw_num() - 1));
          size_t num_objects = std::max<size_t>((value >> 16) & 0xFFFF, 1);
          size_t object_size = length.in_bytes() / num_objects;
          const bool use_aligned = ((value >> 48) & 0x1) == 0;
          const Length align(
              use_aligned ? std::clamp<size_t>((value >> 32) & 0xFFFF, 1,
                                               kPagesPerHugePage.raw_num() - 1)
                          : 1);

          AccessDensityPrediction density =
              ((value >> 49) & 0x1) == 0 ? AccessDensityPrediction::kSparse
                                         : AccessDensityPrediction::kDense;
          if (object_size > kMaxSize || align > Length(1)) {
            // Truncate to a single object.
            num_objects = 1;
            // TODO(b/283843066): Revisit this once we have fluid
            // partitioning.
            density = AccessDensityPrediction::kSparse;
          } else if (!SizeMap::IsValidSizeClass(object_size, length.raw_num(),
                                                kMinObjectsToMove)) {
            // This is an invalid size class, so skip it.
            break;
          }
          if (dense_tracker_type ==
                  HugePageFillerDenseTrackerType::kSpansAllocated &&
              density == AccessDensityPrediction::kDense) {
            length = Length(1);
          }

          // Allocation is too big for filler if we try to allocate >
          // kPagesPerHugePage / 2 run of pages. The allocations may go to
          // HugeRegion and that might lead to donations with kSparse
          // density.
          if (length > kPagesPerHugePage / 2) {
            density = AccessDensityPrediction::kSparse;
          }

          Span* s;
          SpanAllocInfo alloc_info = {.objects_per_span = num_objects,
                                      .density = density};
          TC_CHECK(
              dense_tracker_type ==
                  HugePageFillerDenseTrackerType::kLongestFreeRangeAndChunks ||
              density == AccessDensityPrediction::kSparse ||
              length == Length(1));
          if (use_aligned) {
            s = allocator->NewAligned(length, align, alloc_info);
          } else {
            s = allocator->New(length, alloc_info);
          }
          TC_CHECK_NE(s, nullptr);
          TC_CHECK_GE(s->num_pages().raw_num(), length.raw_num());

          allocs.push_back(SpanInfo{s, num_objects});

          allocated += s->num_pages();
          break;
        }
        case 1: {
          // Deallocate.  We divide up our random value by:
          //
          // value - We choose index in allocs to deallocate a span.

          if (allocs.empty()) break;

          const size_t pos = value % allocs.size();
          std::swap(allocs[pos], allocs[allocs.size() - 1]);

          SpanInfo span_info = allocs[allocs.size() - 1];
          allocs.resize(allocs.size() - 1);
          allocated -= span_info.span->num_pages();

          {
#ifdef TCMALLOC_INTERNAL_LEGACY_LOCKING
            PageHeapSpinLockHolder l;
            allocator->Delete(span_info.span);
#else
            PageAllocatorInterface::AllocationState a{
                Range(span_info.span->first_page(),
                      span_info.span->num_pages()),
                span_info.span->donated(),
            };
            allocator->forwarder().DeleteSpan(span_info.span);
            PageHeapSpinLockHolder l;
            allocator->Delete(a);
#endif  // TCMALLOC_INTERNAL_LEGACY_LOCKING
          }
          break;
        }
        case 2: {
          // Release pages.  We divide up our random value by:
          //
          // value[7:0]  - Choose number of pages to release.
          // value[8:9]  - Choose page release reason.
          // value[63:9] - Reserved.
          Length desired(value & 0x00FF);
          PageReleaseReason reason;
          switch ((value >> 8) & 0x3) {
            case 0:
              reason = PageReleaseReason::kReleaseMemoryToSystem;
              break;
            case 1:
              reason = PageReleaseReason::kProcessBackgroundActions;
              break;
            case 2:
              reason = PageReleaseReason::kSoftLimitExceeded;
              break;
            case 3:
              reason = PageReleaseReason::kHardLimitExceeded;
              break;
          }
          Length released;
          PageReleaseStats actual_stats;
          {
            PageHeapSpinLockHolder l;
            released = allocator->ReleaseAtLeastNPages(desired, reason);
            actual_stats = allocator->GetReleaseStats();
          }
          expected_stats.total += released;
          switch (reason) {
            case PageReleaseReason::kReleaseMemoryToSystem:
              expected_stats.release_memory_to_system += released;
              break;
            case PageReleaseReason::kProcessBackgroundActions:
              expected_stats.process_background_actions += released;
              break;
            case PageReleaseReason::kSoftLimitExceeded:
              expected_stats.soft_limit_exceeded += released;
              break;
            case PageReleaseReason::kHardLimitExceeded:
              expected_stats.hard_limit_exceeded += released;
              break;
          }
          TC_CHECK_EQ(actual_stats, expected_stats);

          break;
        }
        case 3: {
          // Release pages by breaking hugepages.  We divide up our random
          // value by:
          //
          // value[15:0]  - Choose number of pages to release.
          // value[16]    - Choose page release reason. SoftLimitExceeded if
          //                zero, HardLimitExceeded otherwise.
          // value[63:17] - Reserved.
          Length desired(value & 0xFFFF);
          const PageReleaseReason reason =
              ((value & (uint64_t{1} << 16)) == 0)
                  ? PageReleaseReason::kSoftLimitExceeded
                  : PageReleaseReason::kHardLimitExceeded;
          Length released;
          size_t releasable_bytes;
          PageReleaseStats actual_stats;
          {
            PageHeapSpinLockHolder l;
            releasable_bytes = allocator->FillerStats().free_bytes +
                               allocator->RegionsFreeBacked().in_bytes() +
                               allocator->CacheStats().free_bytes;
            released = allocator->ReleaseAtLeastNPagesBreakingHugepages(desired,
                                                                        reason);
            actual_stats = allocator->GetReleaseStats();
          }

          if (forwarder.release_succeeds()) {
            const size_t min_released =
                std::min(desired.in_bytes(), releasable_bytes);
            TC_CHECK_GE(released.in_bytes(), min_released);
          } else {
            // TODO(b/271282540):  This is not strict equality due to
            // HugePageFiller's unmapping_unaccounted_ state.  Narrow this
            // bound.
            TC_CHECK_GE(released.in_bytes(), 0);
          }

          expected_stats.total += released;
          if (reason == PageReleaseReason::kSoftLimitExceeded) {
            expected_stats.soft_limit_exceeded += released;
          } else {
            expected_stats.hard_limit_exceeded += released;
          }

          TC_CHECK_EQ(actual_stats, expected_stats);

          break;
        }
        case 4: {
          // Gather stats in pbtxt format.
          //
          // value is unused.
          Printer p(&output[0], output.size());
          {
            PbtxtRegion region(p, kTop);
            allocator->PrintInPbtxt(region);
          }
          CHECK_LE(p.SpaceRequired(), output.size());
          break;
        }
        case 5: {
          // Print stats.
          //
          // value[0]: Choose if we print everything.
          // value[63:1]: Reserved.
          Printer p(&output[0], output.size());
          bool everything = (value % 2 == 0);
          allocator->Print(p, everything);
          break;
        }
        case 6: {
          // Gather and check stats.
          //
          // value is unused.
          BackingStats stats;
          {
            PageHeapSpinLockHolder l;
            stats = allocator->stats();
          }
          uint64_t used_bytes =
              stats.system_bytes - stats.free_bytes - stats.unmapped_bytes;
          TC_CHECK_EQ(used_bytes, allocated.in_bytes() +
                                      forwarder.pending_release_.in_bytes());
          break;
        }
        case 7: {
          // Change a runtime parameter.
          //
          // value[0:3] - Select parameter
          // value[4:7] - Reserved
          // value[8:63] - The value
          const uint64_t actual_value = value >> 8;
          switch (value & 0xF) {
            case 0:
              forwarder.set_filler_skip_subrelease_interval(
                  absl::Nanoseconds(actual_value));
              forwarder.set_filler_skip_subrelease_short_interval(
                  absl::ZeroDuration());
              forwarder.set_filler_skip_subrelease_long_interval(
                  absl::ZeroDuration());
              break;
            case 1:
              forwarder.set_filler_skip_subrelease_interval(
                  absl::ZeroDuration());
              forwarder.set_filler_skip_subrelease_short_interval(
                  absl::Nanoseconds(actual_value));
              break;
            case 2:
              forwarder.set_filler_skip_subrelease_interval(
                  absl::ZeroDuration());
              forwarder.set_filler_skip_subrelease_long_interval(
                  absl::Nanoseconds(actual_value));
              break;
            case 3:
              forwarder.set_release_partial_alloc_pages(actual_value & 0x1);
              break;
            case 4:
              forwarder.set_hpaa_subrelease(actual_value & 0x1);
              break;
            case 5:
              forwarder.set_release_succeeds(actual_value & 0x1);
              break;
            case 6:
              forwarder.set_huge_region_demand_based_release(actual_value &
                                                             0x1);
              break;
            case 7: {
              // Not quite a runtime parameter:  Interpret actual_value as a
              // subprogram in our dsl.
              size_t subprogram = std::min(size - i - 9, actual_value);
              reentrant.emplace_back(data + i + 9, subprogram);
              i += size;
              break;
            }
            case 8: {
              // Flips the settings used by demand-based release in HugeCache:
              // actual_value[0] - release enabled
              // actual_value[1:16] - interval_1
              // actual_value[17:32] - interval_2
              forwarder.set_huge_cache_demand_based_release(actual_value & 0x1);
              if (forwarder.huge_cache_demand_based_release()) {
                const uint64_t interval_1 = (actual_value >> 1) & 0xffff;
                const uint64_t interval_2 = (actual_value >> 17) & 0xffff;
                forwarder.set_cache_demand_release_long_interval(
                    interval_1 >= interval_2 ? absl::Nanoseconds(interval_1)
                                             : absl::Nanoseconds(interval_2));
                forwarder.set_cache_demand_release_short_interval(
                    interval_1 >= interval_2 ? absl::Nanoseconds(interval_2)
                                             : absl::Nanoseconds(interval_1));
              }
              break;
            }
          }
          break;
        }
      }
    }
  };

  forwarder.release_callback_ = [&]() {
    if (tcmalloc::tcmalloc_internal::pageheap_lock.IsHeld()) {
      // This permits a slight degree of nondeterminism when linked against
      // TCMalloc for the real memory allocator, as a background thread could
      // also be holding the lock.  Nevertheless, HPAA doesn't make it clear
      // when we are releasing with/without the pageheap_lock.
      //
      // TODO(b/73749855): When all release paths unconditionally release the
      // lock, remove this check and take the lock for an instant to ensure it
      // can be taken.
      return;
    }

    if (reentrant.empty()) {
      return;
    }

    ABSL_CONST_INIT static int depth = 0;
    if (depth >= 5) {
      return;
    }

    auto [data, size] = reentrant.back();
    reentrant.pop_back();

    depth++;
    run_dsl(data, size);
    depth--;
  };

  run_dsl(data, size);

  // Stop recursing, since allocator->Delete below might cause us to "release"
  // more pages to the system.
  reentrant.clear();

  // Clean up.
  const PageReleaseStats final_stats = [&] {
    for (auto span_info : allocs) {
      Span* span = span_info.span;
      allocated -= span->num_pages();
#ifdef TCMALLOC_INTERNAL_LEGACY_LOCKING
      PageHeapSpinLockHolder l;
      allocator->Delete(span_info.span);
#else
      PageAllocatorInterface::AllocationState a{
          Range(span_info.span->first_page(), span_info.span->num_pages()),
          span_info.span->donated(),
      };
      allocator->forwarder().DeleteSpan(span_info.span);
      PageHeapSpinLockHolder l;
      allocator->Delete(a);
#endif  // TCMALLOC_INTERNAL_LEGACY_LOCKING
    }

    PageHeapSpinLockHolder l;
    return allocator->GetReleaseStats();
  }();

  TC_CHECK_EQ(allocated.in_bytes(), 0);
  TC_CHECK_EQ(final_stats, expected_stats);

  free(allocator);
}

FUZZ_TEST(HugePageAwareAllocatorTest, FuzzHPAA)
    ;

TEST(HugePageAwareAllocatorTest, FuzzHPAARegression) {
  FuzzHPAA(
      "\370n,,,\3708\304\320\327\311["
      "PXG\"Y\037\216\366\366b\216\340\375\332\362");
}

TEST(HugePageAwareAllocatorTest, FuzzHPAARegression2) {
  FuzzHPAA(
      "\376\006\366>\354{{{{{\347\242\2048:\204\177{{"
      "9\376d\027\224\312\257\276\252\026?\013\010\010");
}

}  // namespace
}  // namespace tcmalloc::tcmalloc_internal
