// Copyright 2026 The TCMalloc Authors
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

#ifndef TCMALLOC_STATIC_FORWARDER_H_
#define TCMALLOC_STATIC_FORWARDER_H_

#include <cstddef>
#include <cstdint>
#include <new>
#include <optional>
#include <type_traits>
#include <utility>

#include "absl/base/attributes.h"
#include "absl/base/internal/cycleclock.h"
#include "absl/base/nullability.h"
#include "absl/base/optimization.h"
#include "absl/base/thread_annotations.h"
#include "absl/strings/string_view.h"
#include "absl/types/span.h"
#include "tcmalloc/common.h"
#include "tcmalloc/experiment.h"
#include "tcmalloc/internal/central_freelist_hooks.h"
#include "tcmalloc/internal/config.h"
#include "tcmalloc/internal/logging.h"
#include "tcmalloc/internal/memory_tag.h"
#include "tcmalloc/internal/numa.h"
#include "tcmalloc/internal/system_allocator.h"
#include "tcmalloc/pages.h"
#include "tcmalloc/parameters.h"
#include "tcmalloc/sizemap.h"
#include "tcmalloc/span.h"

GOOGLE_MALLOC_SECTION_BEGIN
namespace tcmalloc {
namespace tcmalloc_internal {

class Arena;

using InsertRangeHook = void (*)(size_t size_class, absl::Span<void*> batch);
using RemoveRangeHook = void (*)(size_t size_class, absl::Span<void*> batch);

class Forwarder : public Parameters {
 public:
  static constexpr size_t kNumBaseClasses =
      tcmalloc::tcmalloc_internal::kNumBaseClasses;
  static constexpr size_t kNumClasses =
      tcmalloc::tcmalloc_internal::kNumClasses;
  static constexpr size_t kNormalPartitions =
      tcmalloc::tcmalloc_internal::kNormalPartitions;
  static constexpr size_t kSecurityPartitions =
      tcmalloc::tcmalloc_internal::kSecurityPartitions;
  static constexpr size_t kHasColdClasses =
      tcmalloc::tcmalloc_internal::kHasColdClasses;
  static constexpr size_t kColdClassesStart =
      tcmalloc::tcmalloc_internal::kColdClassesStart;

  constexpr Forwarder() = default;

  [[nodiscard]] static void* absl_nonnull Alloc(
      size_t size, std::align_val_t alignment = kAlignment);

  [[nodiscard]] void* absl_nonnull AllocReportedImpending(
      size_t size, std::align_val_t alignment)
      ABSL_LOCKS_EXCLUDED(pageheap_lock);

  void Dealloc(void* ptr, size_t size, std::align_val_t alignment) {
    TC_ASSERT(false);
  }

  void SetAnonVmaName(void* ptr, size_t size,
                      std::optional<absl::string_view> name) const;

  void ArenaUpdateAllocatedAndNonresident(int64_t allocated,
                                          int64_t nonresident)
      ABSL_LOCKS_EXCLUDED(pageheap_lock);

  bool reuse_size_classes() const;

  static size_t class_to_size(int size_class);
  static size_t num_objects_to_move(int size_class);
  static Length class_to_pages(int size_class);

  const NumaTopology<kNumaPartitions, kNumBaseClasses>& numa_topology() const;

  bool HaveHooks() const;

  size_t active_partitions() const;

  bool multiple_non_numa_partitions() const;

  // Central freelist hooks and clock
  static void InvokeInsertRangeHook(size_t size_class,
                                    absl::Span<void*> batch) {
    if (ABSL_PREDICT_TRUE(central_freelist_insert_range_hooks.empty())) {
      return;
    }
    InvokeInsertRangeHookSlow(size_class, batch);
  }

  static void InvokeRemoveRangeHook(size_t size_class,
                                    absl::Span<void*> batch) {
    if (ABSL_PREDICT_TRUE(central_freelist_remove_range_hooks.empty())) {
      return;
    }
    InvokeRemoveRangeHookSlow(size_class, batch);
  }

  static uint64_t clock_now() { return absl::base_internal::CycleClock::Now(); }
  static double clock_frequency() {
    return absl::base_internal::CycleClock::Frequency();
  }

  static void MapObjectsToSpans(absl::Span<void*> batch,
                                Span** absl_nonnull spans,
                                int expected_size_class);
  [[nodiscard]] static Span* absl_nullable AllocateSpan(int size_class,
                                                        size_t objects_per_span,
                                                        Length pages_per_span)
      ABSL_LOCKS_EXCLUDED(pageheap_lock);
  static void DeallocateSpans(size_t objects_per_span,
                              absl::Span<Span*> free_spans)
      ABSL_LOCKS_EXCLUDED(pageheap_lock);

  // Arena state.
  static Arena& arena();

  // PageAllocator state.
  static void ShrinkToUsageLimit(Length n, bool may_have_grown)
      ABSL_EXCLUSIVE_LOCKS_REQUIRED(pageheap_lock);

  // PageMap state.
  static void* GetHugepage(HugePage p);
  [[nodiscard]] static bool Ensure(Range r)
      ABSL_EXCLUSIVE_LOCKS_REQUIRED(pageheap_lock);
  static void ClearSpan(PageId page);
  static void SetSpan(PageId page, Span* absl_nonnull span);
  static void SetHugepage(HugePage p, void* pt);

  // SpanAllocator state.
  static Span* NewSpan(Range r)
#ifdef TCMALLOC_INTERNAL_LEGACY_LOCKING
      ABSL_EXCLUSIVE_LOCKS_REQUIRED(pageheap_lock)
#else
      ABSL_LOCKS_EXCLUDED(pageheap_lock)
#endif  // TCMALLOC_INTERNAL_LEGACY_LOCKING
          ABSL_ATTRIBUTE_RETURNS_NONNULL;
  static void DeleteSpan(Span* span)
#ifdef TCMALLOC_INTERNAL_LEGACY_LOCKING
      ABSL_EXCLUSIVE_LOCKS_REQUIRED(pageheap_lock)
#else
      ABSL_LOCKS_EXCLUDED(pageheap_lock)
#endif  // TCMALLOC_INTERNAL_LEGACY_LOCKING
          ABSL_ATTRIBUTE_NONNULL();

  // Error reporting
  [[noreturn]] static void ReportDoubleFree(void* ptr);

  // SystemAlloc state.
  [[nodiscard]] static AddressRange AllocatePages(size_t bytes, size_t align,
                                                  MemoryTag tag);
  static bool BackAllocations() { return back_small_allocations(); }
  static int32_t BackSizeThresholdBytes() {
    return back_size_threshold_bytes();
  }
  static void Back(Range r);
  [[nodiscard]] static MemoryModifyStatus ReleasePages(Range r);
  [[nodiscard]] static MemoryModifyStatus CollapsePages(Range r);
  static void SetAnonVmaName(Range r, std::optional<absl::string_view> name);

  static void InvokeInsertRangeHookSlow(size_t size_class,
                                        absl::Span<void*> batch);
  static void InvokeRemoveRangeHookSlow(size_t size_class,
                                        absl::Span<void*> batch);
};

using StaticForwarder = Forwarder;

class ShardedForwarder : public Forwarder {
 public:
  static void Init() {
    use_generic_cache_ =
        IsExperimentActive(Experiment::TCMALLOC_SHARDED_TC_ABLATION) &&
        !IsExperimentActive(
            Experiment::TEST_ONLY_TCMALLOC_SHARDED_TRANSFER_CACHE);
    // Traditionally, we enable sharded transfer cache for large size
    // classes alone.
    enable_cache_for_large_classes_only_ = IsExperimentActive(
        Experiment::TEST_ONLY_TCMALLOC_SHARDED_TRANSFER_CACHE);
  }

  static bool UseGenericCache() { return use_generic_cache_; }

  static bool EnableCacheForLargeClassesOnly() {
    return enable_cache_for_large_classes_only_;
  }

 private:
  inline static bool use_generic_cache_ = false;
  inline static bool enable_cache_for_large_classes_only_ = false;
};

}  // namespace tcmalloc_internal
}  // namespace tcmalloc
GOOGLE_MALLOC_SECTION_END

#endif  // TCMALLOC_STATIC_FORWARDER_H_
