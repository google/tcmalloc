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

#include "tcmalloc/huge_page_treatment.h"

#include <errno.h>
#include <sys/mman.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>

#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "absl/status/status.h"
#include "absl/strings/string_view.h"
#include "absl/time/time.h"
#include "tcmalloc/common.h"
#include "tcmalloc/huge_cache.h"
#include "tcmalloc/huge_page_filler.h"
#include "tcmalloc/huge_page_options.h"
#include "tcmalloc/huge_page_subrelease.h"
#include "tcmalloc/huge_page_tracker.h"
#include "tcmalloc/huge_pages.h"
#include "tcmalloc/internal/clock.h"
#include "tcmalloc/internal/config.h"
#include "tcmalloc/internal/memory_tag.h"
#include "tcmalloc/internal/numa.h"
#include "tcmalloc/internal/page_size.h"
#include "tcmalloc/internal/pageflags.h"
#include "tcmalloc/internal/residency.h"
#include "tcmalloc/internal/system_allocator.h"
#include "tcmalloc/pages.h"
#include "tcmalloc/span.h"
#include "tcmalloc/testing/smaps.h"

namespace tcmalloc::tcmalloc_internal {
namespace {

class FakeClock {
 public:
  [[nodiscard]] static int64_t now() {
    return clock_.load(std::memory_order_relaxed);
  }
  [[nodiscard]] static double freq() {
    return absl::ToDoubleNanoseconds(absl::Seconds(1));
  }
  static void Advance(absl::Duration d) {
    clock_.fetch_add(static_cast<int64_t>(absl::ToDoubleSeconds(d) * freq()),
                     std::memory_order_relaxed);
  }
  static void Reset() { clock_.store(1000, std::memory_order_relaxed); }

 private:
  static std::atomic<int64_t> clock_;
};

std::atomic<int64_t> FakeClock::clock_{1000};

using TestSystemAllocator = SystemAllocator<NumaTopology<2, 50>, 1>;

class SystemAllocatorRelease final : public MemoryModifyFunction {
 public:
  explicit SystemAllocatorRelease(TestSystemAllocator& allocator)
      : allocator_(allocator) {}

  [[nodiscard]] MemoryModifyStatus operator()(Range r) override {
    return allocator_.Release(r.start_addr(), r.in_bytes());
  }

 private:
  TestSystemAllocator& allocator_;
};

class SystemAllocatorCollapse final : public MemoryModifyFunction {
 public:
  explicit SystemAllocatorCollapse(TestSystemAllocator& allocator)
      : allocator_(allocator) {}

  [[nodiscard]] MemoryModifyStatus operator()(Range r) override {
    ++calls_;
    last_status_ = allocator_.Collapse(r.start_addr(), r.in_bytes());
    return last_status_;
  }

  [[nodiscard]] size_t calls() const { return calls_; }
  [[nodiscard]] MemoryModifyStatus last_status() const { return last_status_; }

 private:
  TestSystemAllocator& allocator_;
  size_t calls_ = 0;
  MemoryModifyStatus last_status_ = {.success = false, .error_number = 0};
};

class SystemAllocatorSetAnonVmaName final : public MemoryTagFunction {
 public:
  explicit SystemAllocatorSetAnonVmaName(TestSystemAllocator& allocator)
      : allocator_(allocator) {}

  void operator()(Range r, std::optional<absl::string_view> name) override {
    allocator_.SetAnonVmaName(r.start_addr(), r.in_bytes(), name);
  }

 private:
  TestSystemAllocator& allocator_;
};

// Queries /proc/self/pageflags when available, falling back to reading
// AnonHugePages via ForEachSmapEntry (/proc/self/smaps) on standard Linux
// kernels so real 2 MiB MADV_DONTNEED + MADV_COLLAPSE integration tests execute
// on both.
class SmapsOrProcPageFlags final : public PageFlagsBase {
 public:
  [[nodiscard]] std::optional<bool> IsHugepageBacked(
      const void* addr) override {
    if (std::optional<bool> proc_res = proc_.IsHugepageBacked(addr);
        proc_res.has_value()) {
      return proc_res;
    }
    std::optional<bool> result;
    if (!ForEachSmapEntry([&](const SmapEntry& entry) {
          if (entry.Overlaps(addr, 1)) {
            result = entry.anon_huge_pages_bytes >= kHugePageSize;
          }
        })) {
      return std::nullopt;
    }
    return result;
  }

  [[nodiscard]] std::optional<PageStats> Get(const void* addr,
                                             size_t size) override {
    if (std::optional<PageStats> proc_res = proc_.Get(addr, size);
        proc_res.has_value()) {
      return proc_res;
    }
    return PageStats{};
  }

  [[nodiscard]] PageFlagsBitmaps GetSinglePageBitmaps(
      const void* addr) override {
    PageFlagsBitmaps res = proc_.GetSinglePageBitmaps(addr);
    if (res.status == absl::StatusCode::kOk) {
      return res;
    }
    return {.stale = {}, .status = absl::StatusCode::kOk};
  }

 private:
  PageFlags proc_;
};

struct TrackerDeleter {
  HugePageFiller<PageTracker>* filler = nullptr;
  Range kept_range;
  SpanAllocInfo info = {.objects_per_span = 1,
                        .density = AccessDensityPrediction::kSparse};

  void operator()(PageTracker* pt) const {
    if (pt == nullptr) {
      return;
    }
    std::unique_ptr<PageTracker> emptied;
    {
      PageHeapSpinLockHolder l;
      emptied.reset(filler->Put(pt, kept_range, info));
    }
    EXPECT_EQ(emptied.get(), pt);
  }
};

using OwnedTracker = std::unique_ptr<PageTracker, TrackerDeleter>;

class HugePageTreatmentRealMemoryTest : public ::testing::Test {
 protected:
  static NumaTopology<2, 50>& InitTestState(NumaTopology<2, 50>& topology) {
    FakeClock::Reset();
    topology.Init();
    return topology;
  }

  void SetUp() override {
    host_page_size_ = GetPageSize();
    if (host_page_size_ > kPageSize || kPagesPerHugePage < Length(8)) {
      GTEST_SKIP() << "Requires host page size <= TCMalloc page size and at "
                      "least 8 TCMalloc pages per hugepage.";
    }

    backing_ =
        allocator_.Allocate(kHugePageSize, kHugePageSize, MemoryTag::kNormal);
    ASSERT_NE(backing_.ptr, nullptr);
    ASSERT_GE(backing_.bytes, kHugePageSize);

    // Populate base 4 KiB pages across the 2 MiB region under MADV_NOHUGEPAGE
    // (and keep MADV_NOHUGEPAGE set until right before the positive collapse
    // step) so kernel khugepaged cannot asynchronously collapse the VMA behind
    // the test's back during setup, subrelease, or negative-case checks.
    ASSERT_EQ(madvise(backing_.ptr, kHugePageSize, MADV_NOHUGEPAGE), 0)
        << errno;
    if (madvise(backing_.ptr, kHugePageSize, MADV_POPULATE_WRITE) != 0) {
      GTEST_SKIP() << "madvise(MADV_POPULATE_WRITE) unavailable: errno="
                   << errno;
    }

    std::optional<bool> initial_thp = pageflags_.IsHugepageBacked(backing_.ptr);
    if (!initial_thp.has_value()) {
      GTEST_SKIP() << "PageFlags/smaps unavailable in this environment.";
    }
    EXPECT_FALSE(*initial_thp);

    auto initial_residency =
        residency_.GetUnbackedAndSwappedBitmaps(backing_.ptr);
    if (initial_residency.status != absl::StatusCode::kOk) {
      GTEST_SKIP() << "/proc/self/pagemap is unavailable in this environment.";
    }
    EXPECT_EQ(initial_residency.unbacked.CountBits(), 0);
  }

  void TearDown() override {
    if (backing_.ptr != nullptr) {
      EXPECT_EQ(munmap(backing_.ptr, backing_.bytes), 0);
    }
  }

  [[nodiscard]] OwnedTracker CreateTrackerWithKeptPages(Length kept_len,
                                                        SpanAllocInfo info,
                                                        PageId* kept_page) {
    const HugePage hp = HugePageContaining(PageIdContaining(backing_.ptr));
    EXPECT_EQ(hp.start_addr(), backing_.ptr);

    auto tracker = std::make_unique<PageTracker>(hp, /*was_donated=*/false,
                                                 FakeClock::now());
    {
      PageHeapSpinLockHolder l;
      *kept_page = tracker->Get(kept_len, info).page;
      filler_.Contribute(tracker.get(), /*donated=*/false, info);
    }
    return OwnedTracker(
        tracker.release(),
        TrackerDeleter{.filler = &filler_,
                       .kept_range = Range(*kept_page, kept_len),
                       .info = info});
  }

  size_t host_page_size_ = 0;
  NumaTopology<2, 50> topology_;
  TestSystemAllocator allocator_{InitTestState(topology_), kHugePageSize};
  SystemAllocatorRelease unback_{allocator_};
  SystemAllocatorCollapse collapse_{allocator_};
  SystemAllocatorSetAnonVmaName set_anon_vma_name_{allocator_};
  SmapsOrProcPageFlags pageflags_;
  ResidencyPageMap residency_;
  HugePageFiller<PageTracker> filler_{
      Clock{.now = FakeClock::now, .freq = FakeClock::freq},
      MemoryTag::kNormal,
      unback_,
      collapse_,
      set_anon_vma_name_,
      SubreleaseUnbackedMode::kDisabled};
  AddressRange backing_ = {nullptr, 0};
};

TEST_F(HugePageTreatmentRealMemoryTest,
       AllocateTouchAllReleaseSomeAndCollapseReal2MBRegion) {
  const SpanAllocInfo info = {.objects_per_span = 1,
                              .density = AccessDensityPrediction::kSparse};
  const Length released_len(4);
  const Length kept_len = kPagesPerHugePage - released_len;

  PageId kept_page;
  OwnedTracker pt = CreateTrackerWithKeptPages(kept_len, info, &kept_page);
  *reinterpret_cast<uint8_t*>(kept_page.start_addr()) = 0xAB;

  PageId rel_page;
  {
    PageHeapSpinLockHolder l;
    auto [got_pt, got_page, from_released] = filler_.TryGet(released_len, info);
    ASSERT_EQ(got_pt, pt.get());
    EXPECT_FALSE(from_released);
    rel_page = got_page;
  }

  // Free the 4-page `rel` span and subrelease it to the OS via
  // SystemAllocator::Release.
  {
    PageHeapSpinLockHolder l;
    EXPECT_EQ(filler_.Put(pt.get(), Range(rel_page, released_len), info),
              nullptr);
    const Length released = filler_.ReleasePages(
        released_len, SkipSubreleaseIntervals{},
        /*release_partial_alloc_pages=*/true, /*hit_limit=*/false);
    ASSERT_EQ(released, released_len);
  }

  // Confirm that SystemAllocator::Release unbacked those native pages in OS.
  const size_t expected_unbacked_native_pages =
      released_len.in_bytes() / host_page_size_;
  auto post_release_residency =
      residency_.GetUnbackedAndSwappedBitmaps(backing_.ptr);
  ASSERT_EQ(post_release_residency.status, absl::StatusCode::kOk);
  EXPECT_EQ(post_release_residency.unbacked.CountBits(),
            expected_unbacked_native_pages);
  EXPECT_THAT(pageflags_.IsHugepageBacked(backing_.ptr),
              testing::Optional(false));

  // Add one free unreleased page so `pt` sits on
  // regular_alloc_partial_released_ (exercising the exact list scanned first by
  // TreatHugepageTrackers).
  PageId extra_free_page;
  {
    PageHeapSpinLockHolder l;
    auto [got_pt, got_page, from_released] = filler_.TryGet(Length(1), info);
    ASSERT_EQ(got_pt, pt.get());
    EXPECT_TRUE(from_released);
    extra_free_page = got_page;
    // Populate the refilled page so 3 TCMalloc pages remain unbacked in OS
    // pagemap.
    ASSERT_EQ(
        madvise(extra_free_page.start_addr(), kPageSize, MADV_POPULATE_WRITE),
        0)
        << errno;
    *reinterpret_cast<uint8_t*>(extra_free_page.start_addr()) = 0xCD;
    EXPECT_EQ(filler_.Put(pt.get(), Range(extra_free_page, Length(1)), info),
              nullptr);
    EXPECT_TRUE(pt->released());
    EXPECT_EQ(pt->released_pages(), Length(3));
    EXPECT_EQ(filler_.unmapped_pages(), Length(3));
  }

  // Userspace rejects collapse of a subreleased tracker before issuing
  // MADV_COLLAPSE; missing pages stay unbacked in pagemap.
  {
    PageHeapSpinLockHolder l;
    filler_.TreatHugepageTrackers(
        EnableCollapse::kEnabled, EnableUnfilteredCollapse::kDisabled,
        ReleaseStalePages::kDisabled, PreserveDonatedOnCollapse::kDisabled,
        CollapseReleasedHugePages::kDisabled, &pageflags_, &residency_);
    EXPECT_EQ(filler_.GetHugePageTreatmentStats().collapse_succeeded,
              NHugePages(0));
    EXPECT_FALSE(pt->GetHugePageResidencyState().unbacked.IsZero());
    EXPECT_TRUE(pt->released());
    EXPECT_EQ(pt->released_pages(), Length(3));
    EXPECT_EQ(filler_.unmapped_pages(), Length(3));
  }
  EXPECT_EQ(collapse_.calls(), 0);
  EXPECT_EQ(residency_.GetUnbackedAndSwappedBitmaps(backing_.ptr)
                .unbacked.CountBits(),
            Length(3).in_bytes() / host_page_size_);

  // Advance past kRecordInterval, re-enable MADV_HUGEPAGE, and run with
  // CollapseReleasedHugePages::kEnabled. Real MADV_COLLAPSE collapses the
  // 2 MiB range despite the 3 missing TCMalloc pages, re-backing them with
  // zeroed physical pages inside the compound THP and clearing released state.
  FakeClock::Advance(kRecordInterval + absl::Minutes(1));
  {
    PageHeapSpinLockHolder l;
    ASSERT_EQ(madvise(backing_.ptr, kHugePageSize, MADV_HUGEPAGE), 0) << errno;
    filler_.TreatHugepageTrackers(
        EnableCollapse::kEnabled, EnableUnfilteredCollapse::kDisabled,
        ReleaseStalePages::kDisabled, PreserveDonatedOnCollapse::kDisabled,
        CollapseReleasedHugePages::kEnabled, &pageflags_, &residency_);
  }
  if (collapse_.calls() == 0 &&
      pageflags_.IsHugepageBacked(backing_.ptr).value_or(false)) {
    GTEST_SKIP() << "Kernel khugepaged collapsed VMA concurrently with test.";
  }
  ASSERT_EQ(collapse_.calls(), 1);
  if (!collapse_.last_status().success) {
    GTEST_SKIP() << "madvise(MADV_COLLAPSE) failed on host kernel with errno="
                 << collapse_.last_status().error_number;
  }

  EXPECT_THAT(pageflags_.IsHugepageBacked(backing_.ptr),
              testing::Optional(true));
  auto post_collapse_residency =
      residency_.GetUnbackedAndSwappedBitmaps(backing_.ptr);
  ASSERT_EQ(post_collapse_residency.status, absl::StatusCode::kOk);
  EXPECT_EQ(post_collapse_residency.unbacked.CountBits(), 0);
  EXPECT_EQ(*reinterpret_cast<uint8_t*>(kept_page.start_addr()), 0xAB);
  EXPECT_EQ(*reinterpret_cast<uint8_t*>(extra_free_page.start_addr()), 0xCD);
  EXPECT_EQ(*reinterpret_cast<uint8_t*>((rel_page + Length(1)).start_addr()),
            0x00);

  {
    PageHeapSpinLockHolder l;
    EXPECT_EQ(filler_.GetHugePageTreatmentStats().collapse_succeeded,
              NHugePages(1));
    EXPECT_TRUE(pt->GetHugePageResidencyState().unbacked.IsZero());
    EXPECT_TRUE(pt->GetHugePageResidencyState().swapped.IsZero());
    EXPECT_FALSE(pt->released());
    EXPECT_EQ(pt->released_pages(), Length(0));
    EXPECT_TRUE(pt->unbroken());
    EXPECT_EQ(filler_.unmapped_pages(), Length(0));
    EXPECT_EQ(filler_.used_pages_in_partial_released(), Length(0));
  }
}

// Verifies behavior when missing (unbacked) pages in a real 2 MiB region exceed
// kMaxUnbackedPagesForCollapse (64 native pages):
// - Filtered collapse (EnableUnfilteredCollapse::kDisabled) skips MADV_COLLAPSE
//   so the 1 MiB hole stays unbacked in /proc/self/pagemap.
// - Unfiltered collapse (EnableUnfilteredCollapse::kEnabled) issues
//   MADV_COLLAPSE and Linux synchronously collapses the half-unbacked 2 MiB
//   region into a single compound THP, re-backing all missing pages to zero.
TEST_F(HugePageTreatmentRealMemoryTest,
       Real2MBRegionWithHalfMissingPagesRespectsUnbackedFilterThenCollapses) {
  const SpanAllocInfo info = {.objects_per_span = 1,
                              .density = AccessDensityPrediction::kSparse};
  const Length half_hp = kPagesPerHugePage / 2;

  PageId kept_page;
  OwnedTracker pt = CreateTrackerWithKeptPages(half_hp, info, &kept_page);
  *reinterpret_cast<uint8_t*>(kept_page.start_addr()) = 0xAB;

  // Unback the remaining half of the 2 MiB region in the OS without marking
  // the tracker as subreleased.
  const PageId unbacked_start = kept_page + half_hp;
  ASSERT_TRUE(
      allocator_.Release(unbacked_start.start_addr(), half_hp.in_bytes())
          .success);

  const size_t expected_unbacked = half_hp.in_bytes() / host_page_size_;
  ASSERT_GE(expected_unbacked, 64);
  EXPECT_EQ(residency_.GetUnbackedAndSwappedBitmaps(backing_.ptr)
                .unbacked.CountBits(),
            expected_unbacked);

  // Filtered collapse skips because unbacked native pages >= 64.
  {
    PageHeapSpinLockHolder l;
    filler_.TreatHugepageTrackers(
        EnableCollapse::kEnabled, EnableUnfilteredCollapse::kDisabled,
        ReleaseStalePages::kDisabled, PreserveDonatedOnCollapse::kDisabled,
        CollapseReleasedHugePages::kDisabled, &pageflags_, &residency_);
    EXPECT_EQ(filler_.GetHugePageTreatmentStats().collapse_attempted,
              NHugePages(0));
    EXPECT_FALSE(pt->GetHugePageResidencyState().unbacked.IsZero());
  }
  EXPECT_EQ(collapse_.calls(), 0);
  EXPECT_EQ(residency_.GetUnbackedAndSwappedBitmaps(backing_.ptr)
                .unbacked.CountBits(),
            expected_unbacked);

  // Unfiltered collapse issues MADV_COLLAPSE even with ~1 MiB missing PTEs;
  // Linux collapses the range and re-backs all missing PTEs into a 2 MiB THP.
  FakeClock::Advance(kRecordInterval + absl::Minutes(1));
  {
    PageHeapSpinLockHolder l;
    ASSERT_EQ(madvise(backing_.ptr, kHugePageSize, MADV_HUGEPAGE), 0) << errno;
    filler_.TreatHugepageTrackers(
        EnableCollapse::kEnabled, EnableUnfilteredCollapse::kEnabled,
        ReleaseStalePages::kDisabled, PreserveDonatedOnCollapse::kDisabled,
        CollapseReleasedHugePages::kDisabled, &pageflags_, &residency_);
  }
  if (collapse_.calls() == 0 &&
      pageflags_.IsHugepageBacked(backing_.ptr).value_or(false)) {
    GTEST_SKIP() << "Kernel khugepaged collapsed VMA concurrently with test.";
  }
  ASSERT_EQ(collapse_.calls(), 1);
  if (!collapse_.last_status().success) {
    GTEST_SKIP() << "madvise(MADV_COLLAPSE) failed on host kernel with errno="
                 << collapse_.last_status().error_number;
  }

  EXPECT_THAT(pageflags_.IsHugepageBacked(backing_.ptr),
              testing::Optional(true));
  EXPECT_EQ(residency_.GetUnbackedAndSwappedBitmaps(backing_.ptr)
                .unbacked.CountBits(),
            0);
  EXPECT_EQ(*reinterpret_cast<uint8_t*>(kept_page.start_addr()), 0xAB);
  EXPECT_EQ(*reinterpret_cast<uint8_t*>(unbacked_start.start_addr()), 0x00);
  {
    PageHeapSpinLockHolder l;
    EXPECT_TRUE(pt->GetHugePageResidencyState().unbacked.IsZero());
    EXPECT_TRUE(pt->GetHugePageResidencyState().swapped.IsZero());
    EXPECT_TRUE(pt->unbroken());
  }
}

}  // namespace
}  // namespace tcmalloc::tcmalloc_internal
