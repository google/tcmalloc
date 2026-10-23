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
#include <map>
#include <utility>

#include "absl/base/attributes.h"
#include "absl/base/call_once.h"
#include "absl/base/internal/low_level_alloc.h"
#include "absl/base/internal/spinlock.h"
#include "absl/base/thread_annotations.h"
#include "absl/container/flat_hash_map.h"
#include "absl/container/flat_hash_set.h"
#include "absl/hash/hash.h"
#include "absl/numeric/bits.h"
#include "absl/strings/string_view.h"
#include "absl/time/time.h"
#include "tcmalloc/arena.h"
#include "tcmalloc/common.h"
#include "tcmalloc/huge_page_filler.h"
#include "tcmalloc/huge_page_options.h"
#include "tcmalloc/huge_pages.h"
#include "tcmalloc/internal/clock.h"
#include "tcmalloc/internal/config.h"
#include "tcmalloc/internal/logging.h"
#include "tcmalloc/internal/memory_tag.h"
#include "tcmalloc/internal/packed_ptr.h"
#include "tcmalloc/internal/system_allocator.h"
#include "tcmalloc/pages.h"
#include "tcmalloc/parameters.h"
#include "tcmalloc/span.h"
#include "tcmalloc/static_vars.h"

GOOGLE_MALLOC_SECTION_BEGIN
namespace tcmalloc {
namespace tcmalloc_internal {
namespace huge_page_allocator_internal {

// Stands in for a Parameters getter in a test forwarder: callable like the
// static it replaces, so templated callers (forwarder_.x()) are unchanged.
template <typename T>
class FakeParameter {
 public:
  explicit FakeParameter(T value) : value_(value) {}
  [[nodiscard]] T operator()() const { return value_; }
  void set(T value) { value_ = value; }

 private:
  T value_;
};

class FakeHugePageStaticForwarder : private Parameters {
 public:
  FakeHugePageStaticForwarder() {
    // Arena allocates its blocks from tc_globals.system_allocator(), which is
    // only initialized by Static::SlowInitIfNecessary().  If this test is not
    // linked against TCMalloc as its malloc, nothing else triggers that.
    tc_globals.InitIfNecessary();
  }

  // Runtime parameters.  This can change between calls.
  // NOLINTBEGIN(readability-identifier-naming)
  FakeParameter<absl::Duration> filler_skip_subrelease_short_interval{
      Parameters::filler_skip_subrelease_short_interval()};
  FakeParameter<absl::Duration> filler_skip_subrelease_long_interval{
      Parameters::filler_skip_subrelease_long_interval()};
  FakeParameter<absl::Duration> filler_skip_subrelease_cold_short_interval{
      Parameters::filler_skip_subrelease_cold_short_interval()};
  FakeParameter<absl::Duration> filler_skip_subrelease_cold_long_interval{
      Parameters::filler_skip_subrelease_cold_long_interval()};
  FakeParameter<ReleasePartialAllocPages> release_partial_alloc_pages{
      Parameters::release_partial_alloc_pages()};
  FakeParameter<bool> hpaa_subrelease{Parameters::hpaa_subrelease()};
  FakeParameter<SubreleaseUnbackedMode> subrelease_unbacked_hugepages{
      Parameters::subrelease_unbacked_hugepages()};
  FakeParameter<TrackBrokenHugepages> track_broken_hugepages{
      Parameters::track_broken_hugepages()};
  FakeParameter<HugeRegionAdaptiveRelease> huge_region_adaptive_release{
      Parameters::huge_region_adaptive_release()};
  FakeParameter<MallocExtension::BytesPerSecond> background_release_rate{
      Parameters::background_release_rate()};
  FakeParameter<bool> release_max_filler_pages{
      Parameters::release_max_filler_pages()};
  FakeParameter<ReleaseStalePages> release_stale_pages{
      Parameters::release_stale_pages()};
  FakeParameter<MadviseRegionsNoHugepage> madvise_cold_regions_nohugepage{
      Parameters::madvise_cold_regions_nohugepage()};
  FakeParameter<bool> back_small_allocations{
      Parameters::back_small_allocations()};
  FakeParameter<int32_t> back_size_threshold_bytes{
      Parameters::back_size_threshold_bytes()};
  FakeParameter<EnableUnfilteredCollapse> enable_unfiltered_collapse{
      Parameters::enable_unfiltered_collapse()};
  FakeParameter<PreserveDonatedOnCollapse> preserve_donated_on_collapse{
      Parameters::preserve_donated_on_collapse()};
  FakeParameter<CollapseReleasedHugePages> collapse_released_hugepages{
      Parameters::collapse_released_hugepages()};
  // NOLINTEND(readability-identifier-naming)

  [[nodiscard]] bool release_succeeds() const { return release_succeeds_; }
  void set_release_succeeds(bool value) { release_succeeds_ = value; }
  void set_collapse_succeeds(bool value) { collapse_succeeds_ = value; }
  void set_error_number(int value) { error_number_ = value; }

  [[nodiscard]] Clock clock() const {
    return Clock{.now = FakeClock, .freq = FakeClockFrequency};
  }
  static void AdvanceClock(absl::Duration d) {
    clock_.fetch_add(absl::ToInt64Nanoseconds(d), std::memory_order_relaxed);
  }
  static void ResetClock() { clock_.store(0, std::memory_order_relaxed); }

  // Arena state.  One arena for every forwarder in the process: Arena never
  // returns its blocks, and each block is a hugepage from the system allocator,
  // so an arena per forwarder leaks that much per allocator a test or fuzzer
  // builds.  Metadata the arena hands out stays allocated once the allocator
  // that owns it is gone, which is a few KiB per allocator.
  [[nodiscard]] static Arena& arena() {
    ABSL_CONST_INIT static Arena arena;
    return arena;
  }

  // PageAllocator state.

  // Check page heap memory limit.  `n` indicates the size of the allocation
  // currently being made, which will not be included in the sampled memory heap
  // for realized fragmentation estimation.
  void ShrinkToUsageLimit(Length n, bool may_have_grown)
      ABSL_EXCLUSIVE_LOCKS_REQUIRED(pageheap_lock) {
    last_may_have_grown_ = may_have_grown;
  }
  [[nodiscard]] bool last_may_have_grown() const {
    return last_may_have_grown_;
  }

  // PageMap state.
  // TODO(b/242550501): We should just swap out a PageMap instead of doing
  // these things. See the description of cl/169552275 for the original TODO.
  // While we're here, also look into consolidating forwarders.
  [[nodiscard]] PackedPtr<PageTracker> GetHugepage(HugePage p) const {
    auto it = trackers_.find(p);
    if (it == trackers_.end()) {
      return PackedPtr<PageTracker>();
    }
    return it->second;
  }
  [[nodiscard]] bool Ensure(Range r)
      ABSL_EXCLUSIVE_LOCKS_REQUIRED(pageheap_lock) {
    return true;
  }
  void SetHugepage(HugePage p, PackedPtr<PageTracker> pt) { trackers_[p] = pt; }

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

    void* ptr = reinterpret_cast<void*>(
        aligned_allocation | (static_cast<uintptr_t>(tag) << kTagShift));
    TC_CHECK_EQ(GetMemoryTag(ptr), tag);
    return AddressRange{ptr, bytes};
  }

  void MarkPagesResident(Range r) {
    absl::base_internal::SpinLockHolder h(live_spans_lock_);
    for (PageId p = r.p, end = r.p + r.n; p != end; ++p) {
      non_resident_pages_.erase(p);
    }
  }

  void MarkPagesNonResident(Range r) {
    absl::base_internal::SpinLockHolder h(live_spans_lock_);
    for (PageId p = r.p, end = r.p + r.n; p != end; ++p) {
      non_resident_pages_.insert(p);
    }
  }

  [[nodiscard]] bool IsPageResident(PageId p) const {
    absl::base_internal::SpinLockHolder h(live_spans_lock_);
    return !non_resident_pages_.contains(p);
  }

  template <typename Fn>
  void ForEachTracker(Fn&& fn) const
      ABSL_EXCLUSIVE_LOCKS_REQUIRED(pageheap_lock) {
    for (const auto& [hp, pt] : trackers_) {
      if (pt.Get() != nullptr) {
        fn(hp, pt.Get());
      }
    }
  }

  void Back(Range r) {
    const uintptr_t start =
        reinterpret_cast<uintptr_t>(r.p.start_addr()) & ~kTagMask;
    const uintptr_t end = start + r.n.in_bytes();
    TC_CHECK_LE(end, fake_allocation_);
    MarkPagesResident(r);
  }

  [[nodiscard]] MemoryModifyStatus ReleasePages(Range r) {
    const uintptr_t start =
        reinterpret_cast<uintptr_t>(r.p.start_addr()) & ~kTagMask;
    const uintptr_t end = start + r.n.in_bytes();
    TC_CHECK_LE(end, fake_allocation_);
    CheckNotLive(r);
    if (release_succeeds_) {
      MarkPagesNonResident(r);
    }
    return {.success = release_succeeds_, .error_number = 0};
  }

  [[noreturn]] void ReportDoubleFree(void* ptr) {
    TC_BUG("Double free of %p", ptr);
  }
  [[nodiscard]] MemoryModifyStatus CollapsePages(Range r) {
    if (collapse_succeeds_) {
      MarkPagesResident(r);
    }
    return {.success = collapse_succeeds_, .error_number = error_number_};
  }
  void SetAnonVmaName(
      Range, std::optional<absl::string_view> name) { /* unimplemented */ }

 private:
  // A released range overlaps no live span.  This is the property at risk once
  // unback runs with pageheap_lock dropped: a page handed to another thread in
  // the meantime would have its contents discarded.  Derived forwarders that
  // run interleaved work before deferring to this ReleasePages are checked
  // after that work, as the kernel would act on the range then.
  void CheckNotLive(Range r) {
    absl::base_internal::SpinLockHolder h(live_spans_lock_);
    auto it = live_spans_.lower_bound(r.p);
    if (it != live_spans_.end()) {
      TC_CHECK_LE(r.p + r.n, it->first,
                  "releasing [%v, %v) overlaps live span [%v, %v)", r.p,
                  r.p + r.n, it->first, it->first + it->second);
    }
    if (it != live_spans_.begin()) {
      --it;
      TC_CHECK_LE(it->first + it->second, r.p,
                  "releasing [%v, %v) overlaps live span [%v, %v)", r.p,
                  r.p + r.n, it->first, it->first + it->second);
    }
  }

  [[nodiscard]] static absl::base_internal::LowLevelAlloc::Arena* ll_arena() {
    ABSL_CONST_INIT static absl::base_internal::LowLevelAlloc::Arena* a;
    ABSL_CONST_INIT static absl::once_flag flag;
    absl::base_internal::LowLevelCallOnce(&flag, [&]() {
      a = absl::base_internal::LowLevelAlloc::NewArena(
          absl::base_internal::LowLevelAlloc::kAsyncSignalSafe);
    });
    return a;
  }
  bool release_succeeds_ = true;
  bool collapse_succeeds_ = true;
  int error_number_ = 0;

  static int64_t FakeClock() { return clock_.load(std::memory_order_relaxed); }
  static double FakeClockFrequency() { return 1e9; }

  std::atomic<uintptr_t> fake_allocation_ = 0x1000;
  static inline std::atomic<int64_t> clock_{0};

  // Not final: libstdc++'s std::map derives from its allocator.
  template <typename T>
  class AllocAdaptor {
   public:
    using value_type = T;

    AllocAdaptor() = default;
    AllocAdaptor(const AllocAdaptor&) = default;

    template <class T1>
    using rebind = AllocAdaptor<T1>;

    template <class T1>
    explicit AllocAdaptor(const AllocAdaptor<T1>&) {}

    [[nodiscard]] T* allocate(size_t n) {
      // Check if n is too big to allocate.
      TC_ASSERT_EQ((n * sizeof(T)) / sizeof(T), n);
      return static_cast<T*>(absl::base_internal::LowLevelAlloc::AllocWithArena(
          n * sizeof(T), ll_arena()));
    }
    void deallocate(T* p, size_t n) {
      absl::base_internal::LowLevelAlloc::Free(p);
    }
  };

  absl::flat_hash_map<HugePage, PackedPtr<PageTracker>, absl::Hash<HugePage>,
                      std::equal_to<HugePage>,
                      AllocAdaptor<std::pair<HugePage, PackedPtr<PageTracker>>>>
      trackers_;
  // Live spans by first page, with their length.  SetSpan runs outside
  // pageheap_lock and ReleasePages runs with or without it, so the map has its
  // own lock.
  //
  // TODO(b/73749855): This lock serializes allocation, deallocation and
  // release across threads and may hide the interleavings that dropping
  // pageheap_lock during unback introduces.  If injected bugs show that it
  // does, replace the map with a structure that needs no lock.
  mutable absl::base_internal::SpinLock live_spans_lock_{
      absl::base_internal::SCHEDULE_KERNEL_ONLY};
  std::map<PageId, Length, std::less<PageId>,
           AllocAdaptor<std::pair<const PageId, Length>>>
      live_spans_ ABSL_GUARDED_BY(live_spans_lock_);
  absl::flat_hash_set<PageId, absl::Hash<PageId>, std::equal_to<PageId>,
                      AllocAdaptor<PageId>>
      non_resident_pages_ ABSL_GUARDED_BY(live_spans_lock_);
  bool last_may_have_grown_{false};
};

}  // namespace huge_page_allocator_internal
}  // namespace tcmalloc_internal
}  // namespace tcmalloc
GOOGLE_MALLOC_SECTION_END

#endif  // TCMALLOC_MOCK_HUGE_PAGE_STATIC_FORWARDER_H_
