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
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "fuzztest/fuzztest.h"
#include "absl/container/flat_hash_map.h"
#include "absl/container/flat_hash_set.h"
#include "absl/log/check.h"
#include "absl/status/status.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_format.h"
#include "absl/strings/string_view.h"
#include "absl/time/time.h"
#include "tcmalloc/common.h"
#include "tcmalloc/huge_cache.h"
#include "tcmalloc/huge_page_filler.h"
#include "tcmalloc/huge_page_subrelease.h"
#include "tcmalloc/huge_pages.h"
#include "tcmalloc/internal/allocation_guard.h"
#include "tcmalloc/internal/clock.h"
#include "tcmalloc/internal/config.h"
#include "tcmalloc/internal/logging.h"
#include "tcmalloc/internal/scoped_allow_allocation.h"
#include "tcmalloc/internal/memory_tag.h"
#include "tcmalloc/internal/pageflags.h"
#include "tcmalloc/internal/range_tracker.h"
#include "tcmalloc/internal/residency.h"
#include "tcmalloc/internal/system_allocator.h"
#include "tcmalloc/pages.h"
#include "tcmalloc/span.h"
#include "tcmalloc/stats.h"

namespace tcmalloc::tcmalloc_internal {
namespace {

using testing::HasSubstr;

struct State;

ABSL_CONST_INIT static int64_t fake_clock = 0;

int64_t mock_clock() { return fake_clock; }

double freq() { return 1 << 10; }

Bitmap<kMaxResidencyBits> GetBitmap(int value) {
  int v = value % kMaxResidencyBits;
  Bitmap<kMaxResidencyBits> bitmap;
  if (v > 0) {
    bitmap.SetRange(/*index=*/0, v);
  }
  return bitmap;
}

class MockUnback final : public MemoryModifyFunction {
 public:
  explicit MockUnback(State& state) : state_(state) {}
  [[nodiscard]] MemoryModifyStatus operator()(Range r) override;
  std::function<void()> release_callback_;

 private:
  State& state_;
};

class MockSetAnonVmaName final : public MemoryTagFunction {
 public:
  void operator()(Range r, std::optional<absl::string_view> name) override {}
};

class FakePageFlags : public PageFlagsBase {
 public:
  explicit FakePageFlags(const State& state) : state_(state) {}
  std::optional<PageStats> Get(const void* addr, size_t size) override {
    return PageStats{};
  }

  PageFlagsBitmaps GetSinglePageBitmaps(const void* addr) override;
  std::optional<bool> IsHugepageBacked(const void* addr) override;

 private:
  const State& state_;
};

class FakeResidency : public Residency {
 public:
  explicit FakeResidency(const State& state) : state_(state) {}
  std::optional<Info> Get(const void* addr, size_t size) override {
    return std::nullopt;
  }

  SinglePageBitmaps GetUnbackedAndSwappedBitmaps(const void* addr) override;

  const size_t kHardwarePagesInHugePage = kHugePageSize / kPageSize;
  size_t GetHardwarePagesInHugePage() const override {
    return kHardwarePagesInHugePage;
  }

 private:
  const State& state_;
};

class MockCollapse final : public MemoryModifyFunction {
 public:
  explicit MockCollapse(State& state) : state_(state) {}
  [[nodiscard]] MemoryModifyStatus operator()(Range r) override;
  std::function<void()> release_callback_;

 private:
  State& state_;
};

struct Allocate {
  uint16_t length;
  uint32_t num_objects;
  bool density_dense;

  void Perform(State& state) const;

  template <typename Sink>
  friend void AbslStringify(Sink& sink, const Allocate& a) {
    absl::Format(&sink,
                 "Allocate{.length=%d, .num_objects=%d, .density_dense=%v}",
                 a.length, a.num_objects, a.density_dense);
  }
};

struct Deallocate {
  uint32_t tracker_index;
  uint32_t alloc_index;

  void Perform(State& state) const;

  template <typename Sink>
  friend void AbslStringify(Sink& sink, const Deallocate& d) {
    absl::Format(&sink, "Deallocate{.tracker_index=%d, .alloc_index=%d}",
                 d.tracker_index, d.alloc_index);
  }
};

struct Release {
  bool hit_limit;
  bool use_peak_interval;
  absl::Duration peak_interval;
  absl::Duration short_interval;
  absl::Duration long_interval;
  uint16_t desired_pages;
  bool release_partial_allocs;

  void Perform(State& state) const;

  template <typename Sink>
  friend void AbslStringify(Sink& sink, const Release& r) {
    absl::Format(&sink,
                 "Release{.hit_limit=%v, .use_peak_interval=%v, "
                 ".peak_interval=absl::Nanoseconds(%v), "
                 ".short_interval=absl::Nanoseconds(%v), "
                 ".long_interval=absl::Nanoseconds(%v), .desired_pages=%d, "
                 ".release_partial_allocs=%v}",
                 r.hit_limit, r.use_peak_interval,
                 absl::ToInt64Nanoseconds(r.peak_interval),
                 absl::ToInt64Nanoseconds(r.short_interval),
                 absl::ToInt64Nanoseconds(r.long_interval), r.desired_pages,
                 r.release_partial_allocs);
  }
};

struct AdvanceClock {
  absl::Duration amount;

  void Perform(State& state) const;

  template <typename Sink>
  friend void AbslStringify(Sink& sink, const AdvanceClock& a) {
    absl::Format(&sink, "AdvanceClock{.amount=absl::Nanoseconds(%v)}",
                 absl::ToInt64Nanoseconds(a.amount));
  }
};

struct ToggleUnback {
  void Perform(State& state) const;

  template <typename Sink>
  friend void AbslStringify(Sink& sink, const ToggleUnback&) {
    sink.Append("ToggleUnback{}");
  }
};

struct GatherStats {
  void Perform(State& state) const;

  template <typename Sink>
  friend void AbslStringify(Sink& sink, const GatherStats&) {
    sink.Append("GatherStats{}");
  }
};

struct ModelTail {
  uint16_t length;

  void Perform(State& state) const;

  template <typename Sink>
  friend void AbslStringify(Sink& sink, const ModelTail& m) {
    absl::Format(&sink, "ModelTail{.length=%d}", m.length);
  }
};

struct MemoryLimitHitRelease {
  uint16_t desired;

  void Perform(State& state) const;

  template <typename Sink>
  friend void AbslStringify(Sink& sink, const MemoryLimitHitRelease& m) {
    absl::Format(&sink, "MemoryLimitHitRelease{.desired=%d}", m.desired);
  }
};

struct GatherStatsPbtxt {
  void Perform(State& state) const;

  template <typename Sink>
  friend void AbslStringify(Sink& sink, const GatherStatsPbtxt&) {
    sink.Append("GatherStatsPbtxt{}");
  }
};

struct GatherSpanStats {
  void Perform(State& state) const;

  template <typename Sink>
  friend void AbslStringify(Sink& sink, const GatherSpanStats&) {
    sink.Append("GatherSpanStats{}");
  }
};

struct TreatTrackers {
  bool enable_collapse;
  bool enable_unfiltered_collapse;
  bool enable_release_stale_pages;

  void Perform(State& state) const;

  template <typename Sink>
  friend void AbslStringify(Sink& sink, const TreatTrackers& t) {
    absl::Format(&sink,
                 "TreatTrackers{.enable_collapse=%v, "
                 ".enable_unfiltered_collapse=%v, "
                 ".enable_release_stale_pages=%v}",
                 t.enable_collapse, t.enable_unfiltered_collapse,
                 t.enable_release_stale_pages);
  }
};

struct UpdateBitmaps {
  bool hugepage_backed_set;
  bool hugepage_backed_val;
  uint16_t unbacked_bitmap_val;
  uint16_t swapped_bitmap_val;
  uint16_t stale_bitmap_val;

  void Perform(State& state) const;

  template <typename Sink>
  friend void AbslStringify(Sink& sink, const UpdateBitmaps& u) {
    absl::Format(&sink,
                 "UpdateBitmaps{.hugepage_backed_set=%v, "
                 ".hugepage_backed_val=%v, .unbacked_bitmap_val=%d, "
                 ".swapped_bitmap_val=%d, .stale_bitmap_val=%d}",
                 u.hugepage_backed_set, u.hugepage_backed_val,
                 u.unbacked_bitmap_val, u.swapped_bitmap_val,
                 u.stale_bitmap_val);
  }
};

struct ToggleCollapseSuccess {
  void Perform(State& state) const;

  template <typename Sink>
  friend void AbslStringify(Sink& sink, const ToggleCollapseSuccess&) {
    sink.Append("ToggleCollapseSuccess{}");
  }
};

struct SetErrorNumber {
  uint8_t error_type;
  uint32_t raw_value;

  void Perform(State& state) const;

  template <typename Sink>
  friend void AbslStringify(Sink& sink, const SetErrorNumber& s) {
    absl::Format(&sink, "SetErrorNumber{.error_type=%d}", s.error_type);
  }
};

struct SetCollapseLatency {
  absl::Duration latency;

  void Perform(State& state) const;

  template <typename Sink>
  friend void AbslStringify(Sink& sink, const SetCollapseLatency& s) {
    absl::Format(&sink, "SetCollapseLatency{.latency=absl::Nanoseconds(%v)}",
                 absl::ToInt64Nanoseconds(s.latency));
  }
};

struct ReentrantSubprogram;

using Instruction =
    std::variant<Allocate, Deallocate, Release, AdvanceClock, ToggleUnback,
                 GatherStats, ModelTail, MemoryLimitHitRelease,
                 GatherStatsPbtxt, GatherSpanStats, TreatTrackers,
                 UpdateBitmaps, ToggleCollapseSuccess, SetErrorNumber,
                 SetCollapseLatency, ReentrantSubprogram>;

struct ReentrantSubprogram {
  std::vector<Instruction> subprogram;

  void Perform(State& state) const;
};

template <typename Sink>
void AbslStringify(Sink& sink, const Instruction& i);

template <typename Sink>
void AbslStringify(Sink& sink, const ReentrantSubprogram& r) {
  absl::Format(&sink, "ReentrantSubprogram{.subprogram={%s}}",
               absl::StrJoin(r.subprogram, ", ",
                             [](std::string* out, const Instruction& i) {
                               absl::StrAppend(out, absl::StrCat(i));
                             }));
}

template <typename Sink>
void AbslStringify(Sink& sink, const Instruction& i) {
  std::visit([&](const auto& arg) { absl::Format(&sink, "%v", arg); }, i);
}

struct State {
  explicit State(SubreleaseUnbackedMode subrelease_unbacked_mode,
                 size_t num_instructions)
      : subrelease_unbacked_mode(subrelease_unbacked_mode),
        unback(*this),
        collapse(*this),
        filler(Clock{.now = mock_clock, .freq = freq}, MemoryTag::kNormal,
               unback, unback, collapse, set_anon_vma_name,
               subrelease_unbacked_mode) {
    fake_clock = 0;
    output.resize(1 << 20);
    // To avoid reentrancy during unback, reserve space in released_set.  We
    // have at most num_instructions allocations, for at most kPagesPerHugePage
    // pages each, that we can track the released status of.
    //
    // TODO(b/73749855): Releasing the pageheap_lock during ReleaseFree will
    // eliminate the need for this.
    released_set.reserve(kPagesPerHugePage.raw_num() * num_instructions);

    auto release_callback = [this]() {
      if (tcmalloc::tcmalloc_internal::pageheap_lock.IsHeld()) {
        return;
      }
      if (reentrant_stack.empty()) {
        return;
      }
      if (depth >= 5) {
        return;
      }

      auto ops = reentrant_stack.back();
      reentrant_stack.pop_back();

      depth++;
      ScopedAllocationAllow allow;
      RunInstructions(ops);
      depth--;
    };

    unback.release_callback_ = release_callback;
    collapse.release_callback_ = release_callback;
  }

  ~State() {
    // Shut down, confirm filler is empty.
    CHECK_EQ(released_set.size(), filler.unmapped_pages().raw_num());
    for (auto& [pt, v] : allocs) {
      for (size_t i = 0, n = v.size(); i < n; ++i) {
        auto [alloc, alloc_info] = v[i];
        PageTracker* ret;
        {
          PageHeapSpinLockHolder l;
          ret = filler.Put(pt, alloc, alloc_info);
        }
        CHECK_EQ(ret != nullptr, i + 1 == n);
      }
      delete pt;
    }
    CHECK(filler.size() == NHugePages(0));
  }

  void RunInstructions(absl::Span<const Instruction> instrs) {
    for (const auto& instruction : instrs) {
      std::visit([&](const auto& instr) { instr.Perform(*this); }, instruction);
    }
  }

  SubreleaseUnbackedMode subrelease_unbacked_mode;
  bool unback_success = true;
  bool collapse_success = true;
  int64_t collapse_latency = 0;
  int error_number = 0;
  std::optional<bool> is_hugepage_backed = true;
  Bitmap<kMaxResidencyBits> unbacked_bitmap;
  Bitmap<kMaxResidencyBits> swapped_bitmap;
  Bitmap<kMaxResidencyBits> stale_bitmap;
  absl::flat_hash_set<PageId> released_set;

  MockUnback unback;
  MockCollapse collapse;
  MockSetAnonVmaName set_anon_vma_name;
  HugePageFiller<PageTracker> filler;

  std::vector<PageTracker*> trackers;
  absl::flat_hash_map<PageTracker*,
                      std::vector<std::pair<Range, SpanAllocInfo>>>
      allocs;
  size_t next_hugepage = 1;
  std::vector<absl::Span<const Instruction>> reentrant_stack;
  int depth = 0;
  bool treating_trackers = false;
  std::string output;
};

MemoryModifyStatus MockUnback::operator()(Range r) {
  if (release_callback_) {
    release_callback_();
  }
  if (!state_.unback_success) {
    return {.success = false, .error_number = 0};
  }

  PageId end = r.p + r.n;
  for (; r.p != end; ++r.p) {
    state_.released_set.insert(r.p);
  }

  return {.success = true, .error_number = state_.error_number};
}

PageFlagsBase::PageFlagsBitmaps FakePageFlags::GetSinglePageBitmaps(
    const void* addr) {
  return {state_.stale_bitmap, absl::StatusCode::kOk};
}

std::optional<bool> FakePageFlags::IsHugepageBacked(const void* addr) {
  return state_.is_hugepage_backed;
}

Residency::SinglePageBitmaps FakeResidency::GetUnbackedAndSwappedBitmaps(
    const void* addr) {
  return {state_.unbacked_bitmap, state_.swapped_bitmap, absl::StatusCode::kOk};
}

MemoryModifyStatus MockCollapse::operator()(Range r) {
  if (release_callback_) {
    release_callback_();
  }
  fake_clock += state_.collapse_latency;
  return {.success = state_.collapse_success,
          .error_number = state_.error_number};
}

void Allocate::Perform(State& state) const {
  Length n(std::clamp<size_t>(length, 1, kPagesPerHugePage.raw_num() - 1));
  size_t num_objects = std::max<size_t>(this->num_objects, 1);
  AccessDensityPrediction density = density_dense
                                        ? AccessDensityPrediction::kDense
                                        : AccessDensityPrediction::kSparse;
  // Truncate to single object for larger allocations. This ensures
  // that we always allocate few-object spans from donations.
  if (n > kPagesPerHugePage / 2) {
    num_objects = 1;
    density = AccessDensityPrediction::kSparse;
  }
  if (density == AccessDensityPrediction::kDense) {
    n = Length(1);
  }
  SpanAllocInfo alloc_info = {.objects_per_span = num_objects,
                              .density = density};

  if (state.depth == 0) {
    TC_CHECK_EQ(state.filler.size().raw_num(), state.trackers.size());
    TC_CHECK_EQ(state.filler.unmapped_pages().raw_num(),
                state.released_set.size());
  }

  HugePageFiller<PageTracker>::TryGetResult result;
  {
    PageHeapSpinLockHolder l;
    result = state.filler.TryGet(n, alloc_info);
  }

  if (result.pt == nullptr) {
    // Since small objects are likely to be found, we model those
    // tail donations separately.
    const bool donated = n > kPagesPerHugePage / 2;
    result.pt = new PageTracker(HugePage{.pn = state.next_hugepage}, donated,
                                fake_clock);
    state.next_hugepage++;
    {
      PageHeapSpinLockHolder l;
      result.page = result.pt->Get(n, alloc_info).page;
      state.filler.Contribute(result.pt, donated, alloc_info);
    }
    state.trackers.push_back(result.pt);
  }

  for (PageId p = result.page, end = p + n; p != end; ++p) {
    state.released_set.erase(p);
  }

  state.allocs[result.pt].push_back({{result.page, n}, alloc_info});

  if (state.depth == 0) {
    TC_CHECK_EQ(state.filler.size().raw_num(), state.trackers.size());
    TC_CHECK_EQ(state.filler.unmapped_pages().raw_num(),
                state.released_set.size());
  }
}

void Deallocate::Perform(State& state) const {
  if (state.trackers.empty()) {
    return;
  }
  const size_t lo = tracker_index % state.trackers.size();
  PageTracker* pt = state.trackers[lo];
  TC_CHECK(!state.allocs[pt].empty());
  const size_t hi = alloc_index % state.allocs[pt].size();
  auto [alloc, alloc_info] = state.allocs[pt][hi];

  std::swap(state.allocs[pt][hi], state.allocs[pt].back());
  state.allocs[pt].resize(state.allocs[pt].size() - 1);
  bool last_alloc = state.allocs[pt].empty();
  if (last_alloc) {
    state.allocs.erase(pt);
    std::swap(state.trackers[lo], state.trackers.back());
    state.trackers.resize(state.trackers.size() - 1);
  }

  PageTracker* ret;
  {
    PageHeapSpinLockHolder l;
    ret = state.filler.Put(pt, alloc, alloc_info);
  }
  if (state.depth == 0) {
    TC_CHECK_EQ(ret != nullptr, last_alloc);
  }
  if (ret) {
    HugePage hp = ret->location();
    for (PageId p = hp.first_page(), end = hp.first_page() + kPagesPerHugePage;
         p != end; ++p) {
      state.released_set.erase(p);
    }
    delete ret;
  }

  if (state.depth == 0) {
    TC_CHECK_EQ(state.filler.size().raw_num(), state.trackers.size());
    TC_CHECK_EQ(state.filler.unmapped_pages().raw_num(),
                state.released_set.size());
  }
}

void Release::Perform(State& state) const {
  SkipSubreleaseIntervals skip_subrelease_intervals;
  if (use_peak_interval) {
    skip_subrelease_intervals.peak_interval = peak_interval;
  } else {
    skip_subrelease_intervals.short_interval = short_interval;
    skip_subrelease_intervals.long_interval = long_interval;
    if (skip_subrelease_intervals.short_interval >
        skip_subrelease_intervals.long_interval) {
      std::swap(skip_subrelease_intervals.short_interval,
                skip_subrelease_intervals.long_interval);
    }
  }
  Length desired(desired_pages);
  size_t to_release_from_partial_allocs;

  Length released;
  {
    PageHeapSpinLockHolder l;
    to_release_from_partial_allocs =
        HugePageFiller<PageTracker>::kPartialAllocPagesRelease *
        state.filler.FreePagesInPartialAllocs().raw_num();
    released = state.filler.ReleasePages(desired, skip_subrelease_intervals,
                                         release_partial_allocs, hit_limit);
  }

  if (!release_partial_allocs || hit_limit ||
      skip_subrelease_intervals.SkipSubreleaseEnabled() ||
      !state.unback_success || state.depth != 0) {
    return;
  }
  TC_CHECK_GE(released.raw_num(), to_release_from_partial_allocs);
}

void AdvanceClock::Perform(State& state) const {
  fake_clock += absl::ToInt64Nanoseconds(
      std::clamp(amount, -absl::Hours(1), absl::Hours(1)));
}

void ToggleUnback::Perform(State& state) const {
  state.unback_success = !state.unback_success;
}

void GatherStats::Perform(State& state) const {
  Printer p(&state.output[0], state.output.size());
  FakePageFlags pageflags(state);
  PageHeapSpinLockHolder l;
  state.filler.Print(p, true, pageflags);
}

void ModelTail::Perform(State& state) const {
  // Model a tail from a larger allocation.  The tail can have any
  // size [1,kPagesPerHugePage).
  //
  // length: We choose a Length to allocate.
  const Length n(
      std::clamp<size_t>(length, 1, kPagesPerHugePage.raw_num() - 1));

  auto* pt = new PageTracker(HugePage{.pn = state.next_hugepage},
                             /*was_donated=*/true, fake_clock);
  state.next_hugepage++;
  PageId start;
  {
    PageHeapSpinLockHolder l;
    start = pt->Get(n, {1, AccessDensityPrediction::kSparse}).page;
    state.filler.Contribute(pt, /*donated=*/true,
                            {1, AccessDensityPrediction::kSparse});
  }

  state.trackers.push_back(pt);

  for (PageId p = start, end = p + n; p != end; ++p) {
    state.released_set.erase(p);
  }

  state.allocs[pt].push_back(
      {{start, n}, {1, AccessDensityPrediction::kSparse}});

  if (state.depth == 0) {
    TC_CHECK_EQ(state.filler.size().raw_num(), state.trackers.size());
    TC_CHECK_EQ(state.filler.unmapped_pages().raw_num(),
                state.released_set.size());
  }
}

void MemoryLimitHitRelease::Perform(State& state) const {
  Length desired_len(desired);
  Length released;
  const Length free = state.filler.free_pages();
  {
    PageHeapSpinLockHolder l;
    released = state.filler.ReleasePages(desired_len, SkipSubreleaseIntervals{},
                                         /*release_partial_alloc_pages=*/false,
                                         /*hit_limit=*/true);
  }
  if (state.depth != 0) {
    return;
  }
  const Length expected =
      state.unback_success ? std::min(free, desired_len) : Length(0);
  TC_CHECK_GE(released.raw_num(), expected.raw_num());
}

void GatherStatsPbtxt::Perform(State& state) const {
  // Gather stats in pbtxt format.
  Printer p(&state.output[0], state.output.size());
  FakePageFlags pageflags(state);
  {
    PbtxtRegion region(p, kTop);
    PageHeapSpinLockHolder l;
    state.filler.PrintInPbtxt(region, pageflags);
  }
  TC_CHECK_LE(p.SpaceRequired(), state.output.size());
}

void GatherSpanStats::Perform(State& state) const {
  // Gather span stats.
  SmallSpanStats small;
  LargeSpanStats large;
  state.filler.AddSpanStats(&small, &large);
}

void TreatTrackers::Perform(State& state) const {
  if (state.treating_trackers) {
    return;
  }
  state.treating_trackers = true;
  FakePageFlags pageflags(state);
  FakeResidency residency(state);
  PageHeapSpinLockHolder l;
  state.filler.TreatHugepageTrackers(
      enable_collapse ? EnableCollapse::kEnabled : EnableCollapse::kDisabled,
      enable_unfiltered_collapse ? EnableUnfilteredCollapse::kEnabled
                                 : EnableUnfilteredCollapse::kDisabled,
      enable_release_stale_pages ? ReleaseStalePages::kEnabled
                                 : ReleaseStalePages::kDisabled,
      &pageflags, &residency);
  state.treating_trackers = false;
  while (PageTracker* pt = state.filler.FetchFullyFreedTracker()) {
    HugePage hp = pt->location();
    for (PageId p = hp.first_page(), end = hp.first_page() + kPagesPerHugePage;
         p != end; ++p) {
      state.released_set.erase(p);
    }
    delete pt;
  }
  for (PageTracker* pt : state.trackers) {
    HugePage hp = pt->location();
    Bitmap<kPagesPerHugePage.raw_num()> rel = pt->released_by_page();
    for (size_t i = 0; i < kPagesPerHugePage.raw_num(); ++i) {
      PageId p = hp.first_page() + Length(i);
      if (rel.GetBit(i)) {
        state.released_set.insert(p);
      } else {
        state.released_set.erase(p);
      }
    }
  }
}

void UpdateBitmaps::Perform(State& state) const {
  if (hugepage_backed_set) {
    state.is_hugepage_backed = hugepage_backed_val;
  } else {
    state.is_hugepage_backed = std::nullopt;
  }
  if (state.is_hugepage_backed.value_or(false)) {
    state.unbacked_bitmap.Clear();
    state.swapped_bitmap.Clear();
    state.stale_bitmap.Clear();
    return;
  }
  state.unbacked_bitmap = GetBitmap(unbacked_bitmap_val);
  state.swapped_bitmap = GetBitmap(swapped_bitmap_val);
  state.stale_bitmap = GetBitmap(stale_bitmap_val);
}

void ToggleCollapseSuccess::Perform(State& state) const {
  state.collapse_success = !state.collapse_success;
}

void SetErrorNumber::Perform(State& state) const {
  switch (error_type % 4) {
    case 0:
      state.error_number = ENOMEM;
      break;
    case 1:
      state.error_number = EAGAIN;
      break;
    case 2:
      state.error_number = EBUSY;
      break;
    case 3:
      state.error_number = EINVAL;
      break;
  }
}

void SetCollapseLatency::Perform(State& state) const {
  state.collapse_latency = absl::ToInt64Nanoseconds(
      std::clamp(latency, absl::ZeroDuration(), absl::Seconds(1)));
}

void ReentrantSubprogram::Perform(State& state) const {
  if (state.depth != 0 || subprogram.empty()) {
    return;
  }
  state.reentrant_stack.push_back(subprogram);
}

void FuzzFiller(const std::vector<Instruction>& instructions,
                SubreleaseUnbackedMode subrelease_unbacked_mode) {
  State state(subrelease_unbacked_mode, instructions.size());
  state.RunInstructions(instructions);
}

auto NonNegativeDurationDomain() {
  return fuzztest::Map([](int64_t ns) { return absl::Nanoseconds(ns); },
                       fuzztest::NonNegative<int64_t>());
}

auto AnyDurationDomain() {
  return fuzztest::Map([](int64_t ns) { return absl::Nanoseconds(ns); },
                       fuzztest::Arbitrary<int64_t>());
}

fuzztest::Domain<Instruction> GetInstructionDomain(int depth) {
  auto base_domain = fuzztest::OneOf(
      fuzztest::Map([](Allocate a) { return Instruction{a}; },
                    fuzztest::Arbitrary<Allocate>()),
      fuzztest::Map([](Deallocate d) { return Instruction{d}; },
                    fuzztest::Arbitrary<Deallocate>()),
      fuzztest::Map(
          [](bool hl, bool upi, absl::Duration pi, absl::Duration si,
             absl::Duration li, uint16_t dp, bool rpa) {
            return Instruction{Release{hl, upi, pi, si, li, dp, rpa}};
          },
          fuzztest::Arbitrary<bool>(), fuzztest::Arbitrary<bool>(),
          NonNegativeDurationDomain(), NonNegativeDurationDomain(),
          NonNegativeDurationDomain(), fuzztest::Arbitrary<uint16_t>(),
          fuzztest::Arbitrary<bool>()),
      fuzztest::Map(
          [](absl::Duration d) { return Instruction{AdvanceClock{d}}; },
          AnyDurationDomain()),
      fuzztest::Map([](ToggleUnback t) { return Instruction{t}; },
                    fuzztest::Arbitrary<ToggleUnback>()),
      fuzztest::Map([](GatherStats g) { return Instruction{g}; },
                    fuzztest::Arbitrary<GatherStats>()),
      fuzztest::Map([](ModelTail m) { return Instruction{m}; },
                    fuzztest::Arbitrary<ModelTail>()),
      fuzztest::Map([](MemoryLimitHitRelease m) { return Instruction{m}; },
                    fuzztest::Arbitrary<MemoryLimitHitRelease>()),
      fuzztest::Map([](GatherStatsPbtxt g) { return Instruction{g}; },
                    fuzztest::Arbitrary<GatherStatsPbtxt>()),
      fuzztest::Map([](GatherSpanStats g) { return Instruction{g}; },
                    fuzztest::Arbitrary<GatherSpanStats>()),
      fuzztest::Map([](TreatTrackers t) { return Instruction{t}; },
                    fuzztest::Arbitrary<TreatTrackers>()),
      fuzztest::Map([](UpdateBitmaps u) { return Instruction{u}; },
                    fuzztest::Arbitrary<UpdateBitmaps>()),
      fuzztest::Map([](ToggleCollapseSuccess t) { return Instruction{t}; },
                    fuzztest::Arbitrary<ToggleCollapseSuccess>()),
      fuzztest::Map([](SetErrorNumber s) { return Instruction{s}; },
                    fuzztest::Arbitrary<SetErrorNumber>()),
      fuzztest::Map(
          [](absl::Duration d) { return Instruction{SetCollapseLatency{d}}; },
          NonNegativeDurationDomain()));

  if (depth <= 0) {
    return base_domain;
  } else {
    return fuzztest::OneOf(
        base_domain, fuzztest::Map(
                         [](std::vector<Instruction> v) {
                           return Instruction{ReentrantSubprogram{v}};
                         },
                         fuzztest::VectorOf(GetInstructionDomain(depth - 1))));
  }
}

FUZZ_TEST(HugePageFillerTest, FuzzFiller)
    .WithDomains(fuzztest::VectorOf(GetInstructionDomain(5)).WithMaxSize(20000),
                 fuzztest::ElementOf({SubreleaseUnbackedMode::kDisabled,
                                      SubreleaseUnbackedMode::kEnabled}));

TEST(HugePageFillerTest, b510326948) {
  FuzzFiller(
      {SetCollapseLatency{.latency = absl::Nanoseconds(9223372036854775807)},
       SetErrorNumber{.error_type = 115},
       UpdateBitmaps{.hugepage_backed_set = false,
                     .hugepage_backed_val = false,
                     .unbacked_bitmap_val = 65535,
                     .swapped_bitmap_val = 1},
       UpdateBitmaps{.hugepage_backed_set = true,
                     .hugepage_backed_val = false,
                     .unbacked_bitmap_val = 1,
                     .swapped_bitmap_val = 1},
       ToggleCollapseSuccess{},
       Allocate{
           .length = 32767, .num_objects = 2147483647, .density_dense = false},
       TreatTrackers{.enable_collapse = true,
                     .enable_unfiltered_collapse = false},
       Deallocate{.tracker_index = 2147483647, .alloc_index = 2147483647},
       ModelTail{.length = 4096},
       Allocate{
           .length = 40147, .num_objects = 2790469646, .density_dense = true},
       Allocate{.length = 65535, .num_objects = 1, .density_dense = false},
       Allocate{
           .length = 65535, .num_objects = 4294967295, .density_dense = false},
       Allocate{.length = 41298, .num_objects = 1, .density_dense = false},
       Allocate{
           .length = 24021, .num_objects = 2147483647, .density_dense = true},
       SetCollapseLatency{.latency = absl::ZeroDuration()},
       ModelTail{.length = 0},
       ToggleUnback{},
       AdvanceClock{.amount = absl::Nanoseconds(1237243357567017495)},
       ModelTail{.length = 31734},
       SetCollapseLatency{.latency = absl::Nanoseconds(1)},
       GatherStats{}},
      SubreleaseUnbackedMode::kEnabled);
}

TEST(
    HugePageFillerTest,
    Regression_clusterfuzz_testcase_minimized_huge_page_filler_fuzz_5161409228701696_test) {
  FuzzFiller(
      {
#include "tcmalloc/testdata/huge_page_filler_fuzz/clusterfuzz_testcase_minimized_huge_page_filler_fuzz_5161409228701696_test.inc"
      },
      SubreleaseUnbackedMode::kDisabled);
}

TEST(
    HugePageFillerTest,
    Regression_clusterfuzz_testcase_minimized_huge_page_filler_fuzz_5516474505363456_test) {
  FuzzFiller(
      {
          Allocate{.length = 1, .num_objects = 1, .density_dense = false},
          Allocate{.length = 1, .num_objects = 4431, .density_dense = false},
          TreatTrackers{.enable_collapse = true,
                        .enable_unfiltered_collapse = true},
          SetCollapseLatency{.latency = absl::ZeroDuration()},
          Allocate{.length = 255, .num_objects = 19968, .density_dense = true},
          Deallocate{.tracker_index = 217, .alloc_index = 286},
          Allocate{.length = 1, .num_objects = 1, .density_dense = false},
      },
      SubreleaseUnbackedMode::kEnabled);
}

TEST(
    HugePageFillerTest,
    Regression_clusterfuzz_testcase_minimized_huge_page_filler_fuzz_6053674183688192_test) {
  FuzzFiller(
      {
#include "tcmalloc/testdata/huge_page_filler_fuzz/clusterfuzz_testcase_minimized_huge_page_filler_fuzz_6053674183688192_test.inc"
      },
      SubreleaseUnbackedMode::kDisabled);
}

TEST(
    HugePageFillerTest,
    Regression_clusterfuzz_testcase_minimized_huge_page_filler_fuzz_6159120802381824) {
  FuzzFiller(
      {
          SetErrorNumber{.error_type = 0, .raw_value = 1644167168},
          ToggleUnback{},
          Allocate{.length = 1, .num_objects = 7680, .density_dense = false},
          TreatTrackers{.enable_collapse = true,
                        .enable_unfiltered_collapse = true},
          TreatTrackers{.enable_collapse = false,
                        .enable_unfiltered_collapse = false},
      },
      SubreleaseUnbackedMode::kEnabled);
}

TEST(
    HugePageFillerTest,
    Regression_clusterfuzz_testcase_minimized_huge_page_filler_fuzz_6512022070886400_test) {
  FuzzFiller(
      {
          TreatTrackers{.enable_collapse = false,
                        .enable_unfiltered_collapse = false},
          SetErrorNumber{.error_type = 0, .raw_value = 2483028032},
          Deallocate{.tracker_index = 0, .alloc_index = 255},
          Allocate{.length = 255, .num_objects = 128, .density_dense = false},
          Allocate{.length = 255, .num_objects = 529, .density_dense = true},
      },
      SubreleaseUnbackedMode::kDisabled);
}

TEST(
    HugePageFillerTest,
    Regression_clusterfuzz_testcase_minimized_huge_page_filler_fuzz_6622985612820480) {
  FuzzFiller(
      {
#include "tcmalloc/testdata/huge_page_filler_fuzz/clusterfuzz_testcase_minimized_huge_page_filler_fuzz_6622985612820480.inc"
      },
      SubreleaseUnbackedMode::kDisabled);
}

TEST(HugePageFillerTest,
     Regression_crash_869dbc1cdf6a1f79b386adf046c7df32257ef684) {
  FuzzFiller(
      {
          SetErrorNumber{.error_type = 0, .raw_value = 1644167168},
          ToggleUnback{},
          ToggleUnback{},
          Allocate{.length = 1, .num_objects = 1, .density_dense = false},
      },
      SubreleaseUnbackedMode::kDisabled);
}

TEST(HugePageFillerTest,
     Regression_crash_e9f3aa3ad83e808a5588ec529c6cdf00d5d397fc) {
  FuzzFiller(
      {
          GatherSpanStats{},
          GatherSpanStats{},
          Allocate{.length = 255, .num_objects = 5841, .density_dense = false},
          Allocate{.length = 1, .num_objects = 202, .density_dense = false},
          Allocate{.length = 203, .num_objects = 1, .density_dense = false},
          UpdateBitmaps{.hugepage_backed_set = true,
                        .hugepage_backed_val = true,
                        .unbacked_bitmap_val = 0,
                        .swapped_bitmap_val = 0},
          UpdateBitmaps{.hugepage_backed_set = true,
                        .hugepage_backed_val = true,
                        .unbacked_bitmap_val = 0,
                        .swapped_bitmap_val = 0},
          UpdateBitmaps{.hugepage_backed_set = true,
                        .hugepage_backed_val = true,
                        .unbacked_bitmap_val = 0,
                        .swapped_bitmap_val = 0},
          UpdateBitmaps{.hugepage_backed_set = true,
                        .hugepage_backed_val = true,
                        .unbacked_bitmap_val = 0,
                        .swapped_bitmap_val = 0},
          UpdateBitmaps{.hugepage_backed_set = true,
                        .hugepage_backed_val = true,
                        .unbacked_bitmap_val = 0,
                        .swapped_bitmap_val = 0},
          UpdateBitmaps{.hugepage_backed_set = true,
                        .hugepage_backed_val = true,
                        .unbacked_bitmap_val = 0,
                        .swapped_bitmap_val = 0},
          UpdateBitmaps{.hugepage_backed_set = false,
                        .hugepage_backed_val = false,
                        .unbacked_bitmap_val = 0,
                        .swapped_bitmap_val = 448},
          SetErrorNumber{.error_type = 1, .raw_value = 23901},
      },
      SubreleaseUnbackedMode::kDisabled);
}

TEST(HugePageFillerTest, Regression_testcase_6686265543557120) {
  FuzzFiller(
      {
          TreatTrackers{.enable_collapse = false,
                        .enable_unfiltered_collapse = false},
          ModelTail{.length = 255},
          Release{.hit_limit = false,
                  .use_peak_interval = false,
                  .peak_interval = absl::ZeroDuration(),
                  .short_interval = absl::Seconds(158),
                  .long_interval = absl::Seconds(200),
                  .desired_pages = 2050,
                  .release_partial_allocs = false},
          Allocate{.length = 255, .num_objects = 4145, .density_dense = false},
          UpdateBitmaps{.hugepage_backed_set = false,
                        .hugepage_backed_val = false,
                        .unbacked_bitmap_val = 72,
                        .swapped_bitmap_val = 333},
          Deallocate{.tracker_index = 8241, .alloc_index = 2685},
      },
      SubreleaseUnbackedMode::kDisabled);
}

TEST(HugePageFillerTest, b510325622) {
  FuzzFiller(
      {SetCollapseLatency{.latency = absl::Nanoseconds(1229275970250789748)},
       Release{.hit_limit = true,
               .use_peak_interval = false,
               .peak_interval = absl::Nanoseconds(1),
               .short_interval = absl::ZeroDuration(),
               .long_interval = absl::Nanoseconds(1),
               .desired_pages = 1,
               .release_partial_allocs = true},
       MemoryLimitHitRelease{.desired = 15389},
       UpdateBitmaps{.hugepage_backed_set = true,
                     .hugepage_backed_val = false,
                     .unbacked_bitmap_val = 65533,
                     .swapped_bitmap_val = 32767},
       SetCollapseLatency{.latency = absl::Nanoseconds(1876442616651942554)},
       Allocate{.length = 65535, .num_objects = 0, .density_dense = true},
       Allocate{
           .length = 22787, .num_objects = 2147483647, .density_dense = true},
       SetCollapseLatency{.latency = absl::ZeroDuration()},
       SetCollapseLatency{.latency = absl::Nanoseconds(1997603242660686471)},
       Release{.hit_limit = false,
               .use_peak_interval = true,
               .peak_interval = absl::ZeroDuration(),
               .short_interval = absl::ZeroDuration(),
               .long_interval = absl::ZeroDuration(),
               .desired_pages = 1,
               .release_partial_allocs = true},
       Allocate{.length = 0, .num_objects = 1, .density_dense = false},
       TreatTrackers{.enable_collapse = true,
                     .enable_unfiltered_collapse = true},
       Deallocate{.tracker_index = 4294967295, .alloc_index = 0},
       GatherStatsPbtxt{}},
      SubreleaseUnbackedMode::kDisabled);
}

TEST(HugePageFillerTest, DepthDependentDeallocate) {
  FuzzFiller(
      {Allocate{.length = 65535, .num_objects = 1, .density_dense = true},
       ReentrantSubprogram{.subprogram = {Deallocate{
                               .tracker_index = 4294967295, .alloc_index = 1}}},
       GatherSpanStats{},
       TreatTrackers{.enable_collapse = true,
                     .enable_unfiltered_collapse = true}},
      SubreleaseUnbackedMode::kDisabled);
}

TEST(HugePageFillerTest, ConcurrentTreatmentInterferenceStress) {
  std::vector<Instruction> instructions;
  instructions.push_back(UpdateBitmaps{
      .hugepage_backed_set = true,
      .hugepage_backed_val = false,
      .unbacked_bitmap_val = 0,
      .swapped_bitmap_val = 0,
  });

  const size_t half_hp = kPagesPerHugePage.raw_num() / 2;

  // 1. Allocate 47 trackers, full, 4 objects each
  for (int i = 0; i < 47; ++i) {
    instructions.push_back(Allocate{
        .length = static_cast<uint16_t>(half_hp),
        .num_objects = 2,
        .density_dense = false,
    });
    instructions.push_back(Allocate{
        .length = static_cast<uint16_t>(half_hp),
        .num_objects = 2,
        .density_dense = false,
    });
  }

  // 2. Allocate Tracker 48 (X), 1 object, partial (size = half_hp)
  // X is at index 47 in trackers vector.
  // X is the 48th contributed tracker, so it will be sampled by RNG.
  instructions.push_back(Allocate{
      .length = static_cast<uint16_t>(half_hp),
      .num_objects = 1,
      .density_dense = false,
  });

  // 3. Fill X. Allocating half_hp will reuse X (since it has half_hp free).
  // X becomes full with 3 objects.
  instructions.push_back(Allocate{
      .length = static_cast<uint16_t>(half_hp),
      .num_objects = 2,
      .density_dense = false,
  });

  // 4. Allocate Trackers 49..64 (16 trackers), full, 4 objects each
  for (int i = 0; i < 16; ++i) {
    instructions.push_back(Allocate{
        .length = static_cast<uint16_t>(half_hp),
        .num_objects = 2,
        .density_dense = false,
    });
    instructions.push_back(Allocate{
        .length = static_cast<uint16_t>(half_hp),
        .num_objects = 2,
        .density_dense = false,
    });
  }

  // 5. Allocate Tracker 65, partial, 4 objects
  instructions.push_back(Allocate{
      .length = static_cast<uint16_t>(half_hp),
      .num_objects = 4,
      .density_dense = false,
  });

  // Advance clock to make X eligible for scan (elapsed > 5 minutes)
  instructions.push_back(AdvanceClock{
      .amount = absl::Minutes(10),
  });

  // Queue reentrant deallocation of X (index 47) during collapse.
  // X has 2 allocations, so we must deallocate both to free it.
  instructions.push_back(
      ReentrantSubprogram{.subprogram = {Deallocate{
                                             .tracker_index = 47,
                                             .alloc_index = 0,
                                         },
                                         Deallocate{
                                             .tracker_index = 47,
                                             .alloc_index = 0,
                                         }}});

  instructions.push_back(TreatTrackers{
      .enable_collapse = true,
      .enable_unfiltered_collapse = true,
  });

  FuzzFiller(instructions, SubreleaseUnbackedMode::kDisabled);
}

TEST(HugePageFillerTest, SubreleaseUnbackedRegression) {
  FuzzFiller(
      {ModelTail{.length = 0}, GatherStatsPbtxt{},
       TreatTrackers{.enable_collapse = true,
                     .enable_unfiltered_collapse = false},
       SetCollapseLatency{.latency = absl::Nanoseconds(9223372036854775807)},
       ToggleUnback{},
       AdvanceClock{.amount = absl::Nanoseconds(386593854685132995)},
       AdvanceClock{.amount = absl::Nanoseconds(6294226378870810818)},
       ToggleCollapseSuccess{}},
      SubreleaseUnbackedMode::kEnabled);
}

TEST(HugePageFillerTest, InstructionStringify) {
  {
    Instruction inst =
        Allocate{.length = 1, .num_objects = 2, .density_dense = true};
    std::string s = absl::StrFormat("%v", inst);
    EXPECT_EQ(s, "Allocate{.length=1, .num_objects=2, .density_dense=true}");
    EXPECT_THAT(s, Not(HasSubstr("<MAPPING_FUNCTION>")));
  }
  {
    Instruction inst = Deallocate{.tracker_index = 3, .alloc_index = 4};
    std::string s = absl::StrFormat("%v", inst);
    EXPECT_EQ(s, "Deallocate{.tracker_index=3, .alloc_index=4}");
    EXPECT_THAT(s, Not(HasSubstr("<MAPPING_FUNCTION>")));
  }
  {
    Instruction inst = AdvanceClock{.amount = absl::Seconds(1)};
    std::string s = absl::StrFormat("%v", inst);
    EXPECT_EQ(s, "AdvanceClock{.amount=absl::Nanoseconds(1000000000)}");
  }
  {
    Instruction inst = ToggleUnback{};
    std::string s = absl::StrFormat("%v", inst);
    EXPECT_EQ(s, "ToggleUnback{}");
  }
  {
    Instruction inst = Release{.hit_limit = true,
                               .use_peak_interval = false,
                               .peak_interval = absl::Seconds(1),
                               .short_interval = absl::Seconds(2),
                               .long_interval = absl::Seconds(3),
                               .desired_pages = 4,
                               .release_partial_allocs = true};
    std::string s = absl::StrFormat("%v", inst);
    EXPECT_EQ(s,
              "Release{.hit_limit=true, .use_peak_interval=false, "
              ".peak_interval=absl::Nanoseconds(1000000000), "
              ".short_interval=absl::Nanoseconds(2000000000), "
              ".long_interval=absl::Nanoseconds(3000000000), .desired_pages=4, "
              ".release_partial_allocs=true}");
  }
  {
    Instruction inst = GatherStats{};
    std::string s = absl::StrFormat("%v", inst);
    EXPECT_EQ(s, "GatherStats{}");
  }
  {
    Instruction inst = ModelTail{.length = 5};
    std::string s = absl::StrFormat("%v", inst);
    EXPECT_EQ(s, "ModelTail{.length=5}");
  }
  {
    Instruction inst = MemoryLimitHitRelease{.desired = 10};
    std::string s = absl::StrFormat("%v", inst);
    EXPECT_EQ(s, "MemoryLimitHitRelease{.desired=10}");
  }
  {
    Instruction inst = GatherStatsPbtxt{};
    std::string s = absl::StrFormat("%v", inst);
    EXPECT_EQ(s, "GatherStatsPbtxt{}");
  }
  {
    Instruction inst = GatherSpanStats{};
    std::string s = absl::StrFormat("%v", inst);
    EXPECT_EQ(s, "GatherSpanStats{}");
  }
  {
    Instruction inst = TreatTrackers{.enable_collapse = true,
                                     .enable_unfiltered_collapse = false,
                                     .enable_release_stale_pages = true};
    std::string s = absl::StrFormat("%v", inst);
    EXPECT_EQ(
        s,
        "TreatTrackers{.enable_collapse=true, "
        ".enable_unfiltered_collapse=false, .enable_release_stale_pages=true}");
  }
  {
    Instruction inst = UpdateBitmaps{.hugepage_backed_set = true,
                                     .hugepage_backed_val = false,
                                     .unbacked_bitmap_val = 1,
                                     .swapped_bitmap_val = 2,
                                     .stale_bitmap_val = 3};
    std::string s = absl::StrFormat("%v", inst);
    EXPECT_EQ(
        s,
        "UpdateBitmaps{.hugepage_backed_set=true, .hugepage_backed_val=false, "
        ".unbacked_bitmap_val=1, .swapped_bitmap_val=2, .stale_bitmap_val=3}");
  }
  {
    Instruction inst = ToggleCollapseSuccess{};
    std::string s = absl::StrFormat("%v", inst);
    EXPECT_EQ(s, "ToggleCollapseSuccess{}");
  }
  {
    Instruction inst = SetErrorNumber{.error_type = 1};
    std::string s = absl::StrFormat("%v", inst);
    EXPECT_EQ(s, "SetErrorNumber{.error_type=1}");
  }
  {
    Instruction inst = SetCollapseLatency{.latency = absl::Seconds(5)};
    std::string s = absl::StrFormat("%v", inst);
    EXPECT_EQ(s, "SetCollapseLatency{.latency=absl::Nanoseconds(5000000000)}");
  }
}

TEST(HugePageFillerTest, Regression_b525818096) {
  FuzzFiller(
      {
          Allocate{.length = 32767,
                   .num_objects = 3840777803,
                   .density_dense = true},
          UpdateBitmaps{.hugepage_backed_set = false,
                        .hugepage_backed_val = false,
                        .unbacked_bitmap_val = 65535,
                        .swapped_bitmap_val = 49577},
          ReentrantSubprogram{
              .subprogram = {MemoryLimitHitRelease{.desired = 1},
                             Deallocate{.tracker_index = 1322071847,
                                        .alloc_index = 1}}},
          Allocate{.length = 32767,
                   .num_objects = 460278703,
                   .density_dense = false},
          Allocate{
              .length = 5, .num_objects = 3242772467, .density_dense = true},
          UpdateBitmaps{.hugepage_backed_set = true,
                        .hugepage_backed_val = false,
                        .unbacked_bitmap_val = 1,
                        .swapped_bitmap_val = 0},
          TreatTrackers{.enable_collapse = true,
                        .enable_unfiltered_collapse = false},
      },
      SubreleaseUnbackedMode::kDisabled);
}

}  // namespace
}  // namespace tcmalloc::tcmalloc_internal
