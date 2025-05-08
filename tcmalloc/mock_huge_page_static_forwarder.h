// Copyright 2023 The TCMalloc Authors
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

#ifndef TCMALLOC_MOCK_HUGE_PAGE_STATIC_FORWARDER_H_
#define TCMALLOC_MOCK_HUGE_PAGE_STATIC_FORWARDER_H_

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <utility>

#include "absl/base/attributes.h"
#include "absl/base/call_once.h"
#include "absl/base/internal/low_level_alloc.h"
#include "absl/base/thread_annotations.h"
#include "absl/container/flat_hash_map.h"
#include "absl/hash/hash.h"
#include "absl/numeric/bits.h"
#include "absl/time/time.h"
#include "tcmalloc/arena.h"
#include "tcmalloc/common.h"
#include "tcmalloc/huge_pages.h"
#include "tcmalloc/internal/config.h"
#include "tcmalloc/internal/logging.h"
#include "tcmalloc/pages.h"
#include "tcmalloc/span.h"
#include "tcmalloc/system-alloc.h"

GOOGLE_MALLOC_SECTION_BEGIN
namespace tcmalloc {
namespace tcmalloc_internal {
namespace huge_page_allocator_internal {

class FakeStaticForwarder {
 public:
  // Runtime parameters.  This can change between calls.
  absl::Duration filler_skip_subrelease_short_interval() {
    return short_interval_;
  }
  absl::Duration filler_skip_subrelease_long_interval() {
    return long_interval_;
  }
  absl::Duration cache_demand_release_short_interval() {
    return cache_demand_release_short_interval_;
  }
  absl::Duration cache_demand_release_long_interval() {
    return cache_demand_release_long_interval_;
  }
  bool release_partial_alloc_pages() { return release_partial_alloc_pages_; }
  bool hpaa_subrelease() const { return hpaa_subrelease_; }

  void set_filler_skip_subrelease_short_interval(absl::Duration value) {
    short_interval_ = value;
  }
  void set_filler_skip_subrelease_long_interval(absl::Duration value) {
    long_interval_ = value;
  }
  void set_cache_demand_release_short_interval(absl::Duration value) {
    cache_demand_release_short_interval_ = value;
  }
  void set_cache_demand_release_long_interval(absl::Duration value) {
    cache_demand_release_long_interval_ = value;
  }
  void set_release_partial_alloc_pages(bool value) {
    release_partial_alloc_pages_ = value;
  }
  void set_hpaa_subrelease(bool value) { hpaa_subrelease_ = value; }
  bool release_succeeds() const { return release_succeeds_; }
  void set_release_succeeds(bool value) { release_succeeds_ = value; }
  void set_collapse_succeeds(bool value) { collapse_succeeds_ = value; }

  bool huge_region_demand_based_release() const {
    return huge_region_demand_based_release_;
  }
  void set_huge_region_demand_based_release(bool value) {
    huge_region_demand_based_release_ = value;
  }

  bool huge_cache_demand_based_release() const {
    return huge_cache_demand_based_release_;
  }
  void set_huge_cache_demand_based_release(bool value) {
    huge_cache_demand_based_release_ = value;
  }

  // Arena state.
  Arena& arena() { return arena_; }

  // PageAllocator state.

  // Check page heap memory limit.  `n` indicates the size of the allocation
  // currently being made, which will not be included in the sampled memory heap
  // for realized fragmentation estimation.
  void ShrinkToUsageLimit(Length n)
      ABSL_EXCLUSIVE_LOCKS_REQUIRED(pageheap_lock) {}

  // PageMap state.
  // TODO(b/242550501): We should just swap out a PageMap instead of doing
  // these things. See the description of cl/169552275 for the original TODO.
  // While we're here, also look into consolidating forwarders.
  [[nodiscard]] void* GetHugepage(HugePage p) {
    auto it = trackers_.find(p);
    if (it == trackers_.end()) {
      return nullptr;
    }
    return it->second;
  }
  [[nodiscard]] bool Ensure(Range r)
      ABSL_EXCLUSIVE_LOCKS_REQUIRED(pageheap_lock) {
    return true;
  }
  void Set(PageId page, Span* span) {}
  void SetHugepage(HugePage p, void* pt) { trackers_[p] = pt; }

  // SpanAllocator state.
  [[nodiscard]] Span* NewSpan(Range r)
#ifdef TCMALLOC_INTERNAL_LEGACY_LOCKING
      ABSL_EXCLUSIVE_LOCKS_REQUIRED(pageheap_lock)
#endif  // TCMALLOC_INTERNAL_LEGACY_LOCKING
          ABSL_ATTRIBUTE_RETURNS_NONNULL {
    Span* span;
    void* result = absl::base_internal::LowLevelAlloc::AllocWithArena(
        sizeof(*span) + alignof(Span) + sizeof(void*), ll_arena());
    span = new (reinterpret_cast<void*>(
        (reinterpret_cast<uintptr_t>(result) + alignof(Span) - 1u) &
        ~(alignof(Span) - 1u))) Span(r);
    *(reinterpret_cast<uintptr_t*>(span + 1)) =
        reinterpret_cast<uintptr_t>(result);
    return span;
  }
  void DeleteSpan(Span* span)
#ifdef TCMALLOC_INTERNAL_LEGACY_LOCKING
      ABSL_EXCLUSIVE_LOCKS_REQUIRED(pageheap_lock)
#endif  // TCMALLOC_INTERNAL_LEGACY_LOCKING
          ABSL_ATTRIBUTE_NONNULL() {
    absl::base_internal::LowLevelAlloc::Free(
        reinterpret_cast<void*>(*(reinterpret_cast<uintptr_t*>(span + 1))));
  }

  // SystemAlloc state.
  [[nodiscard]] AddressRange AllocatePages(size_t bytes, size_t align,
                                           MemoryTag tag) {
    TC_CHECK(absl::has_single_bit(align), "align=%v", align);
    uintptr_t allocation, aligned_allocation, new_allocation;
    do {
      allocation = fake_allocation_.load(std::memory_order_relaxed);
      aligned_allocation = (allocation + align - 1u) & ~(align - 1u);
      new_allocation = aligned_allocation + bytes;
    } while (!fake_allocation_.compare_exchange_weak(
        allocation, new_allocation, std::memory_order_relaxed));

    AddressRange ret{
        reinterpret_cast<void*>(aligned_allocation |
                                (static_cast<uintptr_t>(tag) << kTagShift)),
        bytes};
    return ret;
  }
  void Back(Range r) {}
  [[nodiscard]] bool ReleasePages(Range r) {
    const uintptr_t start =
        reinterpret_cast<uintptr_t>(r.p.start_addr()) & ~kTagMask;
    const uintptr_t end = start + r.n.in_bytes();
    TC_CHECK_LE(end, fake_allocation_);

    return release_succeeds_;
  }

  [[noreturn]] void ReportDoubleFree(void* ptr) {
    TC_BUG("Double free of %p", ptr);
  }
  [[nodiscard]] bool CollapsePages(Range r) { return collapse_succeeds_; }

 private:
  static absl::base_internal::LowLevelAlloc::Arena* ll_arena() {
    ABSL_CONST_INIT static absl::base_internal::LowLevelAlloc::Arena* a;
    ABSL_CONST_INIT static absl::once_flag flag;
    absl::base_internal::LowLevelCallOnce(&flag, [&]() {
      a = absl::base_internal::LowLevelAlloc::NewArena(
          absl::base_internal::LowLevelAlloc::kAsyncSignalSafe);
    });
    return a;
  }
  absl::Duration short_interval_ = absl::Seconds(60);
  absl::Duration long_interval_ = absl::Seconds(300);
  absl::Duration cache_demand_release_short_interval_ = absl::Seconds(10);
  absl::Duration cache_demand_release_long_interval_ = absl::Seconds(30);
  bool release_partial_alloc_pages_ = false;
  bool hpaa_subrelease_ = true;
  bool release_succeeds_ = true;
  bool collapse_succeeds_ = true;
  bool huge_region_demand_based_release_ = false;
  bool huge_cache_demand_based_release_ = false;
  Arena arena_;

  std::atomic<uintptr_t> fake_allocation_ = 0x1000;

  template <typename T>
  class AllocAdaptor final {
   public:
    using value_type = T;

    AllocAdaptor() = default;
    AllocAdaptor(const AllocAdaptor&) = default;

    template <class T1>
    using rebind = AllocAdaptor<T1>;

    template <class T1>
    explicit AllocAdaptor(const AllocAdaptor<T1>&) {}

    T* allocate(size_t n) {
      // Check if n is too big to allocate.
      TC_ASSERT_EQ((n * sizeof(T)) / sizeof(T), n);
      return static_cast<T*>(absl::base_internal::LowLevelAlloc::AllocWithArena(
          n * sizeof(T), ll_arena()));
    }
    void deallocate(T* p, size_t n) {
      absl::base_internal::LowLevelAlloc::Free(p);
    }
  };

  absl::flat_hash_map<HugePage, void*, absl::Hash<HugePage>,
                      std::equal_to<HugePage>,
                      AllocAdaptor<std::pair<HugePage, void*>>>
      trackers_;
};

}  // namespace huge_page_allocator_internal
}  // namespace tcmalloc_internal
}  // namespace tcmalloc
GOOGLE_MALLOC_SECTION_END

#endif  // TCMALLOC_MOCK_HUGE_PAGE_STATIC_FORWARDER_H_
