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
#include <iterator>
#include <map>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "gtest/gtest.h"
#include "fuzztest/fuzztest.h"
#include "absl/base/attributes.h"
#include "absl/log/check.h"
#include "absl/numeric/bits.h"
#include "absl/status/status.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_format.h"
#include "absl/strings/str_join.h"
#include "absl/strings/string_view.h"
#include "absl/time/time.h"
#include "tcmalloc/common.h"
#include "tcmalloc/huge_page_aware_allocator.h"
#include "tcmalloc/huge_page_filler.h"
#include "tcmalloc/huge_page_options.h"
#include "tcmalloc/huge_pages.h"
#include "tcmalloc/huge_region.h"
#include "tcmalloc/internal/logging.h"
#include "tcmalloc/internal/memory_tag.h"
#include "tcmalloc/internal/pageflags.h"
#include "tcmalloc/internal/range_tracker.h"
#include "tcmalloc/internal/residency.h"
#include "tcmalloc/internal/scoped_allow_allocation.h"
#include "tcmalloc/internal/system_allocator.h"
#include "tcmalloc/mock_huge_page_static_forwarder.h"
#include "tcmalloc/page_allocator_interface.h"
#include "tcmalloc/pages.h"
#include "tcmalloc/sizemap.h"
#include "tcmalloc/span.h"
#include "tcmalloc/stats.h"

namespace tcmalloc::tcmalloc_internal {

namespace {

using huge_page_allocator_internal::FakeStaticForwarder;
using huge_page_allocator_internal::HugePageAwareAllocator;
using huge_page_allocator_internal::HugePageAwareAllocatorOptions;

struct FuzzHugePageAwareAllocatorOptions {
  MemoryTag tag;
  HugeRegionUsageOption use_huge_region_more_often;

  explicit operator HugePageAwareAllocatorOptions() const {
    HugePageAwareAllocatorOptions options;
    // Roundtrip the tag through kTagMask.  Under some sanitizers, we restrict
    // the width of the tag.
    options.tag = static_cast<MemoryTag>(
        ((static_cast<uintptr_t>(tag) << kTagShift) & kTagMask) >> kTagShift);
    options.use_huge_region_more_often = use_huge_region_more_often;
    return options;
  }

  template <typename Sink>
  friend void AbslStringify(Sink& sink,
                            const FuzzHugePageAwareAllocatorOptions& options) {
    absl::Format(
        &sink,
        "FuzzHugePageAwareAllocatorOptions{"
        ".tag = static_cast<tcmalloc::tcmalloc_internal::MemoryTag>(%v), "
        ".use_huge_region_more_often = "
        "static_cast<tcmalloc::tcmalloc_internal::"
        "HugeRegionUsageOption>(%v)}",
        static_cast<int>(options.tag),
        static_cast<int>(options.use_huge_region_more_often));
  }
};

class FakeStaticForwarderWithUnback : public FakeStaticForwarder {
 public:
  AddressRange AllocatePages(size_t bytes, size_t align, MemoryTag tag) {
    if (!allocate_succeeds_) {
      return AddressRange{nullptr, 0};
    }
    return FakeStaticForwarder::AllocatePages(bytes, align, tag);
  }

  // The allocator drops pageheap_lock around the system calls below.  Each
  // invokes lock_dropped_callback_ so the fuzzer can interleave other
  // operations, as another thread would while the lock is free.
  MemoryModifyStatus ReleasePages(Range r) {
    pending_release_ += r.n;
    lock_dropped_callback_();
    pending_release_ -= r.n;

    return FakeStaticForwarder::ReleasePages(r);
  }

  MemoryModifyStatus CollapsePages(Range r) {
    lock_dropped_callback_();
    return FakeStaticForwarder::CollapsePages(r);
  }

  void SetAnonVmaName(Range r, std::optional<absl::string_view> name) {
    lock_dropped_callback_();
    FakeStaticForwarder::SetAnonVmaName(r, name);
  }

  // New and NewAligned back the span after LockAndAlloc has released
  // pageheap_lock, so the span is already accounted as used by the allocator
  // but not yet recorded by the fuzzer.
  void Back(Range r) {
    ASSERT_TRUE(BackAllocations());
    TC_CHECK_LE(r.in_bytes(), BackSizeThresholdBytes());
    pending_back_ += r.n;
    lock_dropped_callback_();
    pending_back_ -= r.n;
    return FakeStaticForwarder::Back(r);
  }

  bool allocate_succeeds_ = true;
  Length pending_release_;
  Length pending_back_;
  std::function<void()> lock_dropped_callback_;
};

struct State;

// Treatment queries hugepage backing and residency with pageheap_lock dropped.
// The forwarder hands out fake addresses, so the real PageFlags and
// ResidencyPageMap either cannot read them or report them as hugepage backed,
// and collapse never runs.  These fakes answer from State, so the fuzzer picks
// the outcome and can interleave other operations at each query.
class FakePageFlags final : public PageFlagsBase {
 public:
  explicit FakePageFlags(State& state) : state_(state) {}
  std::optional<PageStats> Get(const void* addr, size_t size) override {
    return PageStats{};
  }

  PageFlagsBitmaps GetSinglePageBitmaps(const void* addr) override;
  std::optional<bool> IsHugepageBacked(const void* addr) override;

 private:
  State& state_;
};

class FakeResidency final : public Residency {
 public:
  explicit FakeResidency(State& state) : state_(state) {}
  std::optional<Info> Get(const void* addr, size_t size) override {
    return std::nullopt;
  }

  SinglePageBitmaps GetUnbackedAndSwappedBitmaps(const void* addr) override;

  size_t GetHardwarePagesInHugePage() const override {
    return kHugePageSize / kPageSize;
  }

 private:
  State& state_;
};

Bitmap<kMaxResidencyBits> GetBitmap(int value) {
  int v = value % kMaxResidencyBits;
  Bitmap<kMaxResidencyBits> bitmap;
  if (v > 0) {
    bitmap.SetRange(/*index=*/0, v);
  }
  return bitmap;
}

struct Alloc {
  size_t length;
  size_t num_objects;
  size_t alignment;
  bool use_aligned;
  bool dense;

  void Perform(State& state) const;

  template <typename Sink>
  friend void AbslStringify(Sink& sink, const Alloc& a) {
    absl::Format(&sink,
                 "Alloc{.length=%v, .num_objects=%v, .alignment=%v, "
                 ".use_aligned=%v, .dense=%v}",
                 a.length, a.num_objects, a.alignment, a.use_aligned, a.dense);
  }
};

struct Dealloc {
  size_t index;

  void Perform(State& state) const;

  template <typename Sink>
  friend void AbslStringify(Sink& sink, const Dealloc& d) {
    absl::Format(&sink, "Dealloc{.index=%v}", d.index);
  }
};

struct ReleasePages {
  size_t desired;
  bool release_memory_to_system;

  void Perform(State& state) const;

  template <typename Sink>
  friend void AbslStringify(Sink& sink, const ReleasePages& r) {
    absl::Format(&sink,
                 "ReleasePages{.desired=%v, .release_memory_to_system=%v}",
                 r.desired, r.release_memory_to_system);
  }
};

struct ReleasePagesBreakingHugepages {
  size_t desired;
  bool soft_limit_exceeded;

  void Perform(State& state) const;

  template <typename Sink>
  friend void AbslStringify(Sink& sink,
                            const ReleasePagesBreakingHugepages& r) {
    absl::Format(&sink,
                 "ReleasePagesBreakingHugepages{.desired=%v, "
                 ".soft_limit_exceeded=%v}",
                 r.desired, r.soft_limit_exceeded);
  }
};

struct GatherStatsPbtxt {
  void Perform(State& state) const;

  template <typename Sink>
  friend void AbslStringify(Sink& sink, const GatherStatsPbtxt&) {
    sink.Append("GatherStatsPbtxt{}");
  }
};

struct PrintStats {
  bool everything;

  void Perform(State& state) const;

  template <typename Sink>
  friend void AbslStringify(Sink& sink, const PrintStats& p) {
    absl::Format(&sink, "PrintStats{.everything=%v}", p.everything);
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
  EnableCollapse enable_collapse;

  void Perform(State& state) const;

  template <typename Sink>
  friend void AbslStringify(Sink& sink, const TreatTrackers& t) {
    absl::Format(&sink, "TreatTrackers{.enable_collapse=%v}",
                 t.enable_collapse);
  }
};

struct SetFillerSkipSubreleaseShortInterval {
  int64_t duration_ns;

  void Perform(State& state) const;

  template <typename Sink>
  friend void AbslStringify(Sink& sink,
                            const SetFillerSkipSubreleaseShortInterval& s) {
    absl::Format(&sink,
                 "SetFillerSkipSubreleaseShortInterval{.duration_ns = %v}",
                 s.duration_ns);
  }
};

struct SetFillerSkipSubreleaseLongInterval {
  int64_t duration_ns;

  void Perform(State& state) const;

  template <typename Sink>
  friend void AbslStringify(Sink& sink,
                            const SetFillerSkipSubreleaseLongInterval& s) {
    absl::Format(&sink,
                 "SetFillerSkipSubreleaseLongInterval{.duration_ns = %v}",
                 s.duration_ns);
  }
};

struct SetReleasePartialAllocPages {
  bool value;

  void Perform(State& state) const;

  template <typename Sink>
  friend void AbslStringify(Sink& sink, const SetReleasePartialAllocPages& s) {
    absl::Format(&sink, "SetReleasePartialAllocPages{.value=%v}", s.value);
  }
};

struct SetHpaaSubrelease {
  bool value;

  void Perform(State& state) const;

  template <typename Sink>
  friend void AbslStringify(Sink& sink, const SetHpaaSubrelease& s) {
    absl::Format(&sink, "SetHpaaSubrelease{.value=%v}", s.value);
  }
};

struct SetSubreleaseUnbackedHugepages {
  bool value;

  void Perform(State& state) const;

  template <typename Sink>
  friend void AbslStringify(Sink& sink,
                            const SetSubreleaseUnbackedHugepages& s) {
    absl::Format(&sink, "SetSubreleaseUnbackedHugepages{.value=%v}", s.value);
  }
};

struct SetReleaseSucceeds {
  bool value;

  void Perform(State& state) const;

  template <typename Sink>
  friend void AbslStringify(Sink& sink, const SetReleaseSucceeds& s) {
    absl::Format(&sink, "SetReleaseSucceeds{.value=%v}", s.value);
  }
};

struct SetCollapseSucceeds {
  bool value;

  void Perform(State& state) const;

  template <typename Sink>
  friend void AbslStringify(Sink& sink, const SetCollapseSucceeds& s) {
    absl::Format(&sink, "SetCollapseSucceeds{.value=%v}", s.value);
  }
};

struct SetHugeRegionAdaptiveRelease {
  bool value;

  void Perform(State& state) const;

  template <typename Sink>
  friend void AbslStringify(Sink& sink, const SetHugeRegionAdaptiveRelease& s) {
    absl::Format(&sink, "SetHugeRegionAdaptiveRelease{.value=%v}", s.value);
  }
};

struct SetAllocateSucceeds {
  bool value;

  void Perform(State& state) const;

  template <typename Sink>
  friend void AbslStringify(Sink& sink, const SetAllocateSucceeds& s) {
    absl::Format(&sink, "SetAllocateSucceeds{.value=%v}", s.value);
  }
};

struct SetBackAllocations {
  bool value;

  void Perform(State& state) const;

  template <typename Sink>
  friend void AbslStringify(Sink& sink, const SetBackAllocations& s) {
    absl::Format(&sink, "SetBackAllocations{.value=%v}", s.value);
  }
};

struct SetBackSizeThresholdBytes {
  int32_t value;

  void Perform(State& state) const;

  template <typename Sink>
  friend void AbslStringify(Sink& sink, const SetBackSizeThresholdBytes& s) {
    absl::Format(&sink, "SetBackSizeThresholdBytes{.value=%v}", s.value);
  }
};

struct ResetSubreleaseIntervals {
  void Perform(State& state) const;

  template <typename Sink>
  friend void AbslStringify(Sink& sink, const ResetSubreleaseIntervals&) {
    sink.Append("ResetSubreleaseIntervals{}");
  }
};

struct SetEnableUnfilteredCollapse {
  bool value;

  void Perform(State& state) const;

  template <typename Sink>
  friend void AbslStringify(Sink& sink, const SetEnableUnfilteredCollapse& s) {
    absl::Format(&sink, "SetEnableUnfilteredCollapse{.value=%v}", s.value);
  }
};

struct SetReleaseMaxColdPages {
  bool value;

  void Perform(State& state) const;

  template <typename Sink>
  friend void AbslStringify(Sink& sink, const SetReleaseMaxColdPages& s) {
    absl::Format(&sink, "SetReleaseMaxColdPages{.value=%v}", s.value);
  }
};

struct SetReleaseMaxFillerPages {
  bool value;

  void Perform(State& state) const;

  template <typename Sink>
  friend void AbslStringify(Sink& sink, const SetReleaseMaxFillerPages& s) {
    absl::Format(&sink, "SetReleaseMaxFillerPages{.value=%v}", s.value);
  }
};

struct SetEnableReleaseStalePages {
  bool value;

  void Perform(State& state) const;

  template <typename Sink>
  friend void AbslStringify(Sink& sink, const SetEnableReleaseStalePages& s) {
    absl::Format(&sink, "SetEnableReleaseStalePages{.value=%v}", s.value);
  }
};

struct SetMadvNoHugepageHugeRegions {
  bool value;

  void Perform(State& state) const;

  template <typename Sink>
  friend void AbslStringify(Sink& sink, const SetMadvNoHugepageHugeRegions& s) {
    absl::Format(&sink, "SetMadvNoHugepageHugeRegions{.value=%v}", s.value);
  }
};

// Sets what FakePageFlags and FakeResidency report for every tracker that the
// next treatments scan.
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

struct Instruction;

template <typename Sink>
void AbslStringify(Sink& sink, const Instruction& i);

struct ReentrantSubprogram {
  std::vector<Instruction> subprogram;

  void Perform(State& state) const;
};

using ParamOp = std::variant<
    ResetSubreleaseIntervals, SetFillerSkipSubreleaseShortInterval,
    SetFillerSkipSubreleaseLongInterval, SetReleasePartialAllocPages,
    SetHpaaSubrelease, SetSubreleaseUnbackedHugepages, SetReleaseSucceeds,
    SetCollapseSucceeds, SetHugeRegionAdaptiveRelease, SetAllocateSucceeds,
    SetBackAllocations, SetBackSizeThresholdBytes, ReentrantSubprogram,
    SetEnableUnfilteredCollapse, SetReleaseMaxColdPages,
    SetReleaseMaxFillerPages, SetEnableReleaseStalePages,
    SetMadvNoHugepageHugeRegions, UpdateBitmaps>;

template <typename Sink>
void AbslStringify(Sink& sink, const ParamOp& p) {
  std::visit([&](auto&& arg) { AbslStringify(sink, arg); }, p);
}

struct ChangeParam {
  ParamOp op;

  void Perform(State& state) const;

  template <typename Sink>
  friend void AbslStringify(Sink& sink, const ChangeParam& c) {
    absl::Format(&sink, "ChangeParam{.op=%v}", c.op);
  }
};

using InstructionVariant =
    std::variant<Alloc, Dealloc, ReleasePages, ReleasePagesBreakingHugepages,
                 GatherStatsPbtxt, PrintStats, GatherSpanStats, TreatTrackers,
                 ChangeParam>;

template <typename Sink>
void AbslStringify(Sink& sink, const InstructionVariant& v) {
  std::visit([&](auto&& arg) { AbslStringify(sink, arg); }, v);
}

struct Instruction {
  InstructionVariant instr;

  void Perform(State& state) const;

  template <typename Sink>
  friend void AbslStringify(Sink& sink, const Instruction& i) {
    absl::Format(&sink, "Instruction{.instr=%v}", i.instr);
  }
};

template <typename Sink>
void AbslStringify(Sink& sink, const ReentrantSubprogram& r) {
  absl::Format(&sink, "ReentrantSubprogram{.subprogram={%s}}",
               absl::StrJoin(r.subprogram, ", ",
                             [](std::string* out, const Instruction& i) {
                               absl::StrAppend(out, i);
                             }));
}

struct SpanInfo {
  Span* span;
  size_t objects_per_span;
  AccessDensityPrediction density;
};

struct State {
  explicit State(HugePageAwareAllocatorOptions options)
      : allocator(options), tag(options.tag) {
    allocs.reserve(100000);
    reentrant_stack.reserve(1000);
    output.resize(1 << 20);

    allocator.forwarder().lock_dropped_callback_ = [this]() {
      OnLockDropped();
    };
  }

  // Runs the next queued reentrant subprogram, if any.  Invoked by the
  // forwarder and the residency fakes wherever the allocator has dropped
  // pageheap_lock around a system call.
  void OnLockDropped() {
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

    if (reentrant_stack.empty()) {
      return;
    }

    if (depth >= 5) {
      return;
    }

    absl::Span<const Instruction> ops = reentrant_stack.back();
    reentrant_stack.pop_back();

    depth++;
    reentrant_runs++;
    // The instruction that dropped the lock may still be inside a
    // PageHeapSpinLockHolder, whose AllocationGuard would otherwise abort
    // the fuzzer's own bookkeeping (live_ranges) in the subprogram.
    ScopedAllocationAllow allow;
    RunInstructions(ops);
    depth--;
  }

  void RunInstructions(absl::Span<const Instruction> instrs) {
    for (const auto& instruction_wrapper : instrs) {
      instruction_wrapper.Perform(*this);
      CheckInvariants();
    }
  }

  void CheckInvariants() {
    BackingStats stats;
    PageReleaseStats release_stats;
    {
      PageHeapSpinLockHolder l;
      stats = allocator.stats();
      release_stats = allocator.GetReleaseStats();
    }
    // Everything not free or unmapped is held by a live span, except for
    // pages whose release is in flight.
    TC_CHECK_GE(stats.system_bytes, stats.free_bytes + stats.unmapped_bytes);
    const size_t used =
        stats.system_bytes - stats.free_bytes - stats.unmapped_bytes;
    const size_t expected_used =
        allocated.in_bytes() +
        allocator.forwarder().pending_release_.in_bytes() +
        allocator.forwarder().pending_back_.in_bytes();
    if (treating_trackers) {
      // A tracker emptied by a reentrant free while a treatment still pins it
      // is parked off every list until the treatment finishes, so its
      // hugepage is neither free nor unmapped in stats until then.
      TC_CHECK_GE(used, expected_used);
      TC_CHECK_EQ((used - expected_used) % kHugePageSize, 0);
    } else {
      TC_CHECK_EQ(used, expected_used);
    }
    TC_CHECK_EQ(release_stats, expected_stats);
    TC_CHECK_EQ(live_ranges.size(), allocs.size());
  }

  HugePageAwareAllocator<FakeStaticForwarderWithUnback> allocator;
  const MemoryTag tag;
  FakePageFlags pageflags{*this};
  FakeResidency residency{*this};
  std::optional<bool> is_hugepage_backed = true;
  Bitmap<kMaxResidencyBits> unbacked_bitmap;
  Bitmap<kMaxResidencyBits> swapped_bitmap;
  Bitmap<kMaxResidencyBits> stale_bitmap;
  std::vector<SpanInfo> allocs;
  // Live spans keyed by first page index, for overlap checks.
  std::map<PageId, Length> live_ranges;
  Length allocated;
  PageReleaseStats expected_stats;
  std::vector<absl::Span<const Instruction>> reentrant_stack;
  int depth = 0;
  // Bumped whenever a reentrant subprogram runs, so an operation can tell
  // whether other instructions interleaved with it.
  size_t reentrant_runs = 0;
  bool treating_trackers = false;
  std::string output;
};

PageFlagsBase::PageFlagsBitmaps FakePageFlags::GetSinglePageBitmaps(
    const void* addr) {
  state_.OnLockDropped();
  return {state_.stale_bitmap, absl::StatusCode::kOk};
}

std::optional<bool> FakePageFlags::IsHugepageBacked(const void* addr) {
  state_.OnLockDropped();
  return state_.is_hugepage_backed;
}

Residency::SinglePageBitmaps FakeResidency::GetUnbackedAndSwappedBitmaps(
    const void* addr) {
  state_.OnLockDropped();
  return {state_.unbacked_bitmap, state_.swapped_bitmap, absl::StatusCode::kOk};
}

void ChangeParam::Perform(State& state) const {
  std::visit([&](const auto& o) { o.Perform(state); }, op);
}

void Instruction::Perform(State& state) const {
  std::visit([&](const auto& i) { i.Perform(state); }, instr);
}

void Alloc::Perform(State& state) const {
  Length len(std::clamp<size_t>(length, 1, 4 * kPagesPerHugePage.raw_num()));
  size_t num_obj = std::max<size_t>(num_objects, 1);
  size_t object_size = len.in_bytes() / num_obj;
  const Length align(
      use_aligned
          ? std::clamp<size_t>(alignment, 1, kPagesPerHugePage.raw_num() - 1)
          : 1);
  AccessDensityPrediction density = dense ? AccessDensityPrediction::kDense
                                          : AccessDensityPrediction::kSparse;

  if (object_size > kMaxSize || align > Length(1) ||
      len > kPagesPerHugePage / 2) {
    // Truncate to a single object.
    num_obj = 1;
    // TODO(b/283843066): Revisit this once we have fluid partitioning.
    density = AccessDensityPrediction::kSparse;
  } else if (!SizeMap::IsValidSizeClass(object_size, len, kMinObjectsToMove)) {
    // This is an invalid size class, so skip it.
    return;
  } else if (density == AccessDensityPrediction::kDense) {
    len = Length(1);
  }

  // Allocation is too big for filler if we try to allocate >
  // kPagesPerHugePage / 2 run of pages. The allocations may go to HugeRegion
  // and that might lead to donations with kSparse density.
  if (len > kPagesPerHugePage / 2) {
    density = AccessDensityPrediction::kSparse;
  }

  SpanAllocInfo alloc_info = {.objects_per_span = num_obj, .density = density};
  TC_CHECK(density == AccessDensityPrediction::kSparse || len == Length(1));
  BackingStats before_stats;
  {
    PageHeapSpinLockHolder l;
    before_stats = state.allocator.stats();
  }
  const size_t before_backed =
      before_stats.system_bytes - before_stats.unmapped_bytes;
  const size_t runs_before = state.reentrant_runs;

  Span* s = use_aligned ? state.allocator.NewAligned(len, align, alloc_info)
                        : state.allocator.New(len, alloc_info);
  if (s == nullptr) {
    return;
  }
  TC_CHECK_EQ(s->num_pages(), len);
  TC_CHECK(GetMemoryTag(s->start_address()) == state.tag);
  if (align > Length(1)) {
    // NewAligned requires a power-of-two alignment; honor the largest one the
    // fuzzed value implies.
    size_t pow2 = absl::bit_ceil(align.raw_num());
    TC_CHECK_EQ(s->first_page().index() % pow2, 0);
  }

  // The span is disjoint from every live span.
  const PageId first = s->first_page();
  const PageId end = first + s->num_pages();
  auto next = state.live_ranges.lower_bound(first);
  TC_CHECK(next == state.live_ranges.end() || next->first >= end);
  if (next != state.live_ranges.begin()) {
    const auto prev = std::prev(next);
    TC_CHECK_LE(prev->first + prev->second, first);
  }
  state.live_ranges.emplace(first, s->num_pages());

  // A subprogram run while backing the span may have grown the heap itself.
  if (runs_before == state.reentrant_runs &&
      !state.allocator.forwarder().last_may_have_grown()) {
    BackingStats after_stats;
    {
      PageHeapSpinLockHolder l;
      after_stats = state.allocator.stats();
    }
    const size_t after_backed =
        after_stats.system_bytes - after_stats.unmapped_bytes;
    TC_CHECK_LE(after_backed, before_backed);
  }

  state.allocs.push_back(SpanInfo{s, num_obj, density});
  state.allocated += s->num_pages();
}

void Dealloc::Perform(State& state) const {
  if (state.allocs.empty()) {
    return;
  }

  const size_t pos = index % state.allocs.size();
  std::swap(state.allocs[pos], state.allocs.back());

  SpanInfo span_info = state.allocs.back();
  state.allocs.pop_back();
  state.allocated -= span_info.span->num_pages();
  TC_CHECK_EQ(state.live_ranges.erase(span_info.span->first_page()), 1);

#ifdef TCMALLOC_INTERNAL_LEGACY_LOCKING
  PageHeapSpinLockHolder l;
  state.allocator.Delete(span_info.span,
                         {.objects_per_span = span_info.objects_per_span,
                          .density = span_info.density});
#else
  PageAllocatorInterface::AllocationState a{
      Range(span_info.span->first_page(), span_info.span->num_pages()),
      span_info.span->donated(),
  };
  state.allocator.forwarder().DeleteSpan(span_info.span);
  PageHeapSpinLockHolder l;
  state.allocator.Delete(a, {.objects_per_span = span_info.objects_per_span,
                             .density = span_info.density});
#endif  // TCMALLOC_INTERNAL_LEGACY_LOCKING
}

void ReleasePages::Perform(State& state) const {
  const Length desired_len(desired);
  const PageReleaseReason reason =
      release_memory_to_system ? PageReleaseReason::kReleaseMemoryToSystem
                               : PageReleaseReason::kProcessBackgroundActions;
  Length released;
  PageReleaseStats actual_stats;
  {
    PageHeapSpinLockHolder l;
    released = state.allocator.ReleaseAtLeastNPages(desired_len, reason);
    actual_stats = state.allocator.GetReleaseStats();
  }

  state.expected_stats.total += released;
  if (reason == PageReleaseReason::kReleaseMemoryToSystem) {
    state.expected_stats.release_memory_to_system += released;
  } else {
    state.expected_stats.process_background_actions += released;
  }

  TC_CHECK_EQ(actual_stats, state.expected_stats);
}

void ReleasePagesBreakingHugepages::Perform(State& state) const {
  const Length desired_len(desired);
  const PageReleaseReason reason = soft_limit_exceeded
                                       ? PageReleaseReason::kSoftLimitExceeded
                                       : PageReleaseReason::kHardLimitExceeded;
  Length released;
  size_t releasable_bytes;
  PageReleaseStats actual_stats;
  // If we might run other operations when we simulate the lock being
  // released, we might not get the results we expected.
  const bool reentrant_was_pending = !state.reentrant_stack.empty();
  {
    PageHeapSpinLockHolder l;
    releasable_bytes = state.allocator.FillerStats().free_bytes +
                       state.allocator.RegionsFreeBacked().in_bytes() +
                       state.allocator.CacheStats().free_bytes;
    released = state.allocator.ReleaseAtLeastNPagesBreakingHugepages(
        desired_len, reason);
    actual_stats = state.allocator.GetReleaseStats();
  }

  if (state.allocator.forwarder().release_succeeds() &&
      !reentrant_was_pending) {
    const size_t min_released =
        std::min(desired_len.in_bytes(), releasable_bytes);
    EXPECT_GE(released.in_bytes(), min_released);
  } else {
    // TODO(b/271282540):  This is not strict equality due to
    // HugePageFiller's unmapping_unaccounted_ state.  Narrow this bound.
    TC_CHECK_GE(released.in_bytes(), 0);
  }

  state.expected_stats.total += released;
  if (reason == PageReleaseReason::kSoftLimitExceeded) {
    state.expected_stats.soft_limit_exceeded += released;
  } else {
    state.expected_stats.hard_limit_exceeded += released;
  }

  TC_CHECK_EQ(actual_stats, state.expected_stats);
}

void GatherStatsPbtxt::Perform(State& state) const {
  Printer p(&state.output[0], state.output.size());
  {
    PbtxtRegion region(p, kTop);
    state.allocator.PrintInPbtxt(region, state.pageflags);
  }
  CHECK_LE(p.SpaceRequired(), state.output.size());
}

void PrintStats::Perform(State& state) const {
  Printer p(&state.output[0], state.output.size());
  state.allocator.Print(p, everything, state.pageflags);
}

void GatherSpanStats::Perform(State& state) const {
  SmallSpanStats small;
  LargeSpanStats large;
  PageHeapSpinLockHolder l;
  state.allocator.GetSmallSpanStats(&small);
  state.allocator.GetLargeSpanStats(&large);
}

void TreatTrackers::Perform(State& state) const {
  // Treatment drops pageheap_lock around collapse and VMA naming, so a
  // reentrant subprogram could start a second treatment.  Production runs
  // treatment from a single background thread, and nested treatments would
  // clear each other's DontFreeTracker bits, so never nest them.
  // TODO(b/565392619): Fuzz concurrent treatments once tracker state survives
  // multiple treatment threads.
  if (state.treating_trackers) {
    return;
  }
  state.treating_trackers = true;
  state.allocator.TreatHugepageTrackers(enable_collapse, &state.pageflags,
                                        &state.residency);
  state.treating_trackers = false;
}

void ResetSubreleaseIntervals::Perform(State& state) const {
  auto& forwarder = state.allocator.forwarder();
  forwarder.set_filler_skip_subrelease_short_interval(absl::ZeroDuration());
  forwarder.set_filler_skip_subrelease_long_interval(absl::ZeroDuration());
}

void SetFillerSkipSubreleaseShortInterval::Perform(State& state) const {
  state.allocator.forwarder().set_filler_skip_subrelease_short_interval(
      absl::Nanoseconds(duration_ns));
}

void SetFillerSkipSubreleaseLongInterval::Perform(State& state) const {
  state.allocator.forwarder().set_filler_skip_subrelease_long_interval(
      absl::Nanoseconds(duration_ns));
}

void SetReleasePartialAllocPages::Perform(State& state) const {
  state.allocator.forwarder().set_release_partial_alloc_pages(value);
}

void SetHpaaSubrelease::Perform(State& state) const {
  state.allocator.forwarder().set_hpaa_subrelease(value);
}

void SetSubreleaseUnbackedHugepages::Perform(State& state) const {
  state.allocator.forwarder().set_subrelease_unbacked_hugepages(
      value ? SubreleaseUnbackedMode::kEnabled
            : SubreleaseUnbackedMode::kDisabled);
}

void SetReleaseSucceeds::Perform(State& state) const {
  state.allocator.forwarder().set_release_succeeds(value);
}

void SetCollapseSucceeds::Perform(State& state) const {
  state.allocator.forwarder().set_collapse_succeeds(value);
}

void SetHugeRegionAdaptiveRelease::Perform(State& state) const {
  state.allocator.forwarder().set_huge_region_adaptive_release(value);
}

void SetAllocateSucceeds::Perform(State& state) const {
  state.allocator.forwarder().allocate_succeeds_ = value;
}

void SetBackAllocations::Perform(State& state) const {
  state.allocator.forwarder().SetBackAllocations(value);
}

void SetBackSizeThresholdBytes::Perform(State& state) const {
  state.allocator.forwarder().SetBackSizeThresholdBytes(value);
}

void ReentrantSubprogram::Perform(State& state) const {
  state.reentrant_stack.push_back(subprogram);
}

void SetEnableUnfilteredCollapse::Perform(State& state) const {
  state.allocator.forwarder().set_enable_unfiltered_collapse(value);
}

void SetReleaseMaxColdPages::Perform(State& state) const {
  state.allocator.forwarder().set_release_max_cold_pages(value);
}

void SetReleaseMaxFillerPages::Perform(State& state) const {
  state.allocator.forwarder().set_release_max_filler_pages(value);
}

void SetEnableReleaseStalePages::Perform(State& state) const {
  state.allocator.forwarder().set_release_stale_pages(
      value ? ReleaseStalePages::kEnabled : ReleaseStalePages::kDisabled);
}

void SetMadvNoHugepageHugeRegions::Perform(State& state) const {
  state.allocator.forwarder().set_madvise_cold_regions_nohugepage(
      value ? MadviseRegionsNoHugepage::kEnabled
            : MadviseRegionsNoHugepage::kDisabled);
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

void FuzzHPAA(FuzzHugePageAwareAllocatorOptions fuzz_options,
              const std::vector<Instruction>& instructions) {
  HugePageAwareAllocatorOptions options =
      static_cast<HugePageAwareAllocatorOptions>(fuzz_options);
  // Use kNormalP1 memory tag only if we have more than one partitions.
  if (kNormalPartitions == 1 && options.tag == MemoryTag::kNormalP1) {
    options.tag = MemoryTag::kNormalP0;
  }

  State state(options);
  state.CheckInvariants();
  state.RunInstructions(instructions);

  // Stop recursing, since allocator.Delete below might cause us to "release"
  // more pages to the system.
  state.reentrant_stack.clear();

  // Clean up.
  const PageReleaseStats final_stats = [&] {
    for (auto span_info : state.allocs) {
      Span* span = span_info.span;
      state.allocated -= span->num_pages();
#ifdef TCMALLOC_INTERNAL_LEGACY_LOCKING
      PageHeapSpinLockHolder l;
      state.allocator.Delete(span_info.span,
                             {.objects_per_span = span_info.objects_per_span,
                              .density = span_info.density});
#else
      PageAllocatorInterface::AllocationState a{
          Range(span_info.span->first_page(), span_info.span->num_pages()),
          span_info.span->donated(),
      };
      state.allocator.forwarder().DeleteSpan(span_info.span);
      PageHeapSpinLockHolder l;
      state.allocator.Delete(a, {.objects_per_span = span_info.objects_per_span,
                                 .density = span_info.density});
#endif  // TCMALLOC_INTERNAL_LEGACY_LOCKING
    }

    PageHeapSpinLockHolder l;
    return state.allocator.GetReleaseStats();
  }();

  TC_CHECK_EQ(state.allocated.in_bytes(), 0);
  TC_CHECK_EQ(final_stats, state.expected_stats);
}

auto AnyDuration() { return fuzztest::NonNegative<int64_t>(); }

auto AnyPositiveDuration() { return fuzztest::Positive<int64_t>(); }

auto GetHPAADomain() {
  return fuzztest::Map(
      [](MemoryTag tag, HugeRegionUsageOption usage) {
        return FuzzHugePageAwareAllocatorOptions{tag, usage};
      },
      fuzztest::ElementOf({MemoryTag::kSampled, MemoryTag::kSampledP1,
                           MemoryTag::kNormalP0, MemoryTag::kNormalP1,
                           MemoryTag::kNormal, MemoryTag::kCold}),
      fuzztest::ElementOf({HugeRegionUsageOption::kDefault,
                           HugeRegionUsageOption::kUseForAllLargeAllocs}));
}

fuzztest::Domain<Instruction> GetInstructionDomain(int depth);

fuzztest::Domain<ChangeParam> GetChangeParamDomain(int depth) {
  auto base_domain = fuzztest::OneOf(
      fuzztest::Map([](ResetSubreleaseIntervals r) { return ChangeParam{r}; },
                    fuzztest::Arbitrary<ResetSubreleaseIntervals>()),
      fuzztest::Map(
          [](int64_t d) {
            return ChangeParam{SetFillerSkipSubreleaseShortInterval{d}};
          },
          AnyDuration()),
      fuzztest::Map(
          [](int64_t d) {
            return ChangeParam{SetFillerSkipSubreleaseLongInterval{d}};
          },
          AnyDuration()),
      fuzztest::Map(
          [](SetReleasePartialAllocPages s) { return ChangeParam{s}; },
          fuzztest::Arbitrary<SetReleasePartialAllocPages>()),
      fuzztest::Map([](SetHpaaSubrelease s) { return ChangeParam{s}; },
                    fuzztest::Arbitrary<SetHpaaSubrelease>()),
      fuzztest::Map(
          [](SetSubreleaseUnbackedHugepages s) { return ChangeParam{s}; },
          fuzztest::Arbitrary<SetSubreleaseUnbackedHugepages>()),
      fuzztest::Map([](SetReleaseSucceeds s) { return ChangeParam{s}; },
                    fuzztest::Arbitrary<SetReleaseSucceeds>()),
      fuzztest::Map([](SetCollapseSucceeds s) { return ChangeParam{s}; },
                    fuzztest::Arbitrary<SetCollapseSucceeds>()),
      fuzztest::Map(
          [](SetHugeRegionAdaptiveRelease s) { return ChangeParam{s}; },
          fuzztest::Arbitrary<SetHugeRegionAdaptiveRelease>()),
      fuzztest::Map([](SetAllocateSucceeds s) { return ChangeParam{s}; },
                    fuzztest::Arbitrary<SetAllocateSucceeds>()),
      fuzztest::Map([](SetBackAllocations s) { return ChangeParam{s}; },
                    fuzztest::Arbitrary<SetBackAllocations>()),
      fuzztest::Map([](SetBackSizeThresholdBytes s) { return ChangeParam{s}; },
                    fuzztest::Arbitrary<SetBackSizeThresholdBytes>()),
      fuzztest::Map(
          [](SetEnableUnfilteredCollapse s) { return ChangeParam{s}; },
          fuzztest::Arbitrary<SetEnableUnfilteredCollapse>()),
      fuzztest::Map([](SetReleaseMaxColdPages s) { return ChangeParam{s}; },
                    fuzztest::Arbitrary<SetReleaseMaxColdPages>()),
      fuzztest::Map([](SetReleaseMaxFillerPages s) { return ChangeParam{s}; },
                    fuzztest::Arbitrary<SetReleaseMaxFillerPages>()),
      fuzztest::Map([](SetEnableReleaseStalePages s) { return ChangeParam{s}; },
                    fuzztest::Arbitrary<SetEnableReleaseStalePages>()),
      fuzztest::Map(
          [](SetMadvNoHugepageHugeRegions s) { return ChangeParam{s}; },
          fuzztest::Arbitrary<SetMadvNoHugepageHugeRegions>()),
      fuzztest::Map([](UpdateBitmaps u) { return ChangeParam{u}; },
                    fuzztest::Arbitrary<UpdateBitmaps>()));

  if (depth <= 0) {
    return fuzztest::OneOf(
        base_domain, fuzztest::Map(
                         [](std::vector<Instruction> v) {
                           return ChangeParam{ReentrantSubprogram{v}};
                         },
                         fuzztest::VectorOf(fuzztest::Just(Instruction{
                                                Alloc{1, 1, 1, false, false}}))
                             .WithSize(0)));
  }

  return fuzztest::OneOf(
      base_domain, fuzztest::Map(
                       [](std::vector<Instruction> v) {
                         return ChangeParam{ReentrantSubprogram{v}};
                       },
                       fuzztest::VectorOf(GetInstructionDomain(depth - 1))));
}

fuzztest::Domain<Instruction> GetInstructionDomain(int depth) {
  return fuzztest::OneOf(
      fuzztest::Map([](Alloc a) { return Instruction{a}; },
                    fuzztest::Arbitrary<Alloc>()),
      fuzztest::Map([](Dealloc d) { return Instruction{d}; },
                    fuzztest::Arbitrary<Dealloc>()),
      fuzztest::Map([](ReleasePages r) { return Instruction{r}; },
                    fuzztest::Arbitrary<ReleasePages>()),
      fuzztest::Map(
          [](ReleasePagesBreakingHugepages r) { return Instruction{r}; },
          fuzztest::Arbitrary<ReleasePagesBreakingHugepages>()),
      fuzztest::Map([](GatherStatsPbtxt g) { return Instruction{g}; },
                    fuzztest::Arbitrary<GatherStatsPbtxt>()),
      fuzztest::Map([](PrintStats p) { return Instruction{p}; },
                    fuzztest::Arbitrary<PrintStats>()),
      fuzztest::Map([](GatherSpanStats g) { return Instruction{g}; },
                    fuzztest::Arbitrary<GatherSpanStats>()),
      fuzztest::Map([](TreatTrackers t) { return Instruction{t}; },
                    fuzztest::Arbitrary<TreatTrackers>()),
      fuzztest::Map([](ChangeParam c) { return Instruction{c}; },
                    GetChangeParamDomain(depth)));
}

FUZZ_TEST(HugePageAwareAllocatorTest, FuzzHPAA)
    .WithDomains(GetHPAADomain(),
                 fuzztest::VectorOf(GetInstructionDomain(/*depth=*/5)));

TEST(HugePageAwareAllocatorTest, FuzzHPAARegression) {
  FuzzHugePageAwareAllocatorOptions options;
  options.tag = MemoryTag::kNormal;
  options.use_huge_region_more_often =
      HugeRegionUsageOption::kUseForAllLargeAllocs;

  std::vector<Instruction> instructions;
  instructions.push_back(Instruction{Alloc{
      .length = 255,
      .num_objects = 8025,
      .alignment = 255,
      .use_aligned = true,
      .dense = true,
  }});

  FuzzHPAA(options, instructions);
}

TEST(HugePageAwareAllocatorTest, FuzzHPAARegression2) {
  FuzzHugePageAwareAllocatorOptions options;
  options.tag = MemoryTag::kCold;
  options.use_huge_region_more_often =
      HugeRegionUsageOption::kUseForAllLargeAllocs;

  std::vector<Instruction> instructions;
  instructions.push_back(Instruction{Alloc{
      .length = 255,
      .num_objects = 31615,
      .alignment = 255,
      .use_aligned = true,
      .dense = false,
  }});

  FuzzHPAA(options, instructions);
}

// ReleaseAtLeastNPages runs under PageHeapSpinLockHolder, whose
// AllocationGuard outlives the lock drop in UnbackWithoutLock.  A subprogram
// interleaved there must still be able to allocate, both in the allocator under
// test and in the fuzzer's own bookkeeping.
TEST(HugePageAwareAllocatorTest, ReentrantAllocDuringRelease) {
  FuzzHPAA(
      FuzzHugePageAwareAllocatorOptions{
          .tag = MemoryTag::kNormal,
          .use_huge_region_more_often = HugeRegionUsageOption::kDefault},
      {Instruction{.instr = Alloc{.length = 1,
                                  .num_objects = 1,
                                  .alignment = 1,
                                  .use_aligned = false,
                                  .dense = false}},
       Instruction{.instr = Dealloc{.index = 0}},
       Instruction{
           .instr = ChangeParam{.op =
                                    ReentrantSubprogram{
                                        .subprogram = {Instruction{
                                            .instr = Alloc{.length = 1,
                                                           .num_objects = 1,
                                                           .alignment = 1,
                                                           .use_aligned = false,
                                                           .dense = false}}}}}},
       Instruction{.instr = ReleasePages{.desired = 65535,
                                         .release_memory_to_system = true}}});
}

// Frees the tracker under treatment while collapse has dropped pageheap_lock.
// The tracker must be parked and drained after treatment rather than freed
// under the treatment's feet.
TEST(HugePageAwareAllocatorTest, ReentrantDeallocDuringCollapse) {
  FuzzHPAA(
      FuzzHugePageAwareAllocatorOptions{
          .tag = MemoryTag::kNormal,
          .use_huge_region_more_often = HugeRegionUsageOption::kDefault},
      {Instruction{
           .instr =
               ChangeParam{.op = SetEnableUnfilteredCollapse{.value = true}}},
       Instruction{
           .instr =
               ChangeParam{.op = UpdateBitmaps{.hugepage_backed_set = true,
                                               .hugepage_backed_val = false,
                                               .unbacked_bitmap_val = 0,
                                               .swapped_bitmap_val = 0,
                                               .stale_bitmap_val = 0}}},
       Instruction{.instr = Alloc{.length = 1,
                                  .num_objects = 1,
                                  .alignment = 1,
                                  .use_aligned = false,
                                  .dense = false}},
       Instruction{
           .instr = ChangeParam{.op =
                                    ReentrantSubprogram{
                                        .subprogram = {Instruction{
                                            .instr = Dealloc{.index = 0}}}}}},
       Instruction{.instr = TreatTrackers{.enable_collapse =
                                              EnableCollapse::kEnabled}}});
}

// Allocating from released pages backs the span outside pageheap_lock, after
// the allocator counts it as used but before the fuzzer records it.  Stats
// checks and further allocations interleaved there must still balance.
TEST(HugePageAwareAllocatorTest, ReentrantAllocDuringBack) {
  FuzzHPAA(
      FuzzHugePageAwareAllocatorOptions{
          .tag = MemoryTag::kNormal,
          .use_huge_region_more_often = HugeRegionUsageOption::kDefault},
      {Instruction{.instr =
                       ChangeParam{.op = SetBackAllocations{.value = true}}},
       Instruction{
           .instr =
               ChangeParam{.op = SetBackSizeThresholdBytes{.value = 1 << 20}}},
       Instruction{.instr = Alloc{.length = 1,
                                  .num_objects = 1,
                                  .alignment = 1,
                                  .use_aligned = false,
                                  .dense = false}},
       Instruction{.instr =
                       ReleasePagesBreakingHugepages{
                           .desired = 255, .soft_limit_exceeded = true}},
       Instruction{
           .instr = ChangeParam{.op =
                                    ReentrantSubprogram{
                                        .subprogram = {Instruction{
                                            .instr = Alloc{.length = 1,
                                                           .num_objects = 1,
                                                           .alignment = 1,
                                                           .use_aligned = false,
                                                           .dense = false}}}}}},
       Instruction{.instr = Alloc{.length = 1,
                                  .num_objects = 1,
                                  .alignment = 1,
                                  .use_aligned = false,
                                  .dense = false}}});
}

TEST(HugePageAwareAllocatorTest, b471822138) {
  FuzzHPAA(
      FuzzHugePageAwareAllocatorOptions{
          .tag = MemoryTag::kNormalP0,
          .use_huge_region_more_often = HugeRegionUsageOption::kDefault},
      {Instruction{.instr = Alloc{.length = 15576967129319913528ULL,
                                  .num_objects = 1,
                                  .alignment = 18446744073709551615ULL,
                                  .use_aligned = false,
                                  .dense = false}},
       Instruction{.instr = Alloc{.length = 9223372036854775807ULL,
                                  .num_objects = 0,
                                  .alignment = 1,
                                  .use_aligned = false,
                                  .dense = false}},
       Instruction{.instr = GatherStatsPbtxt{}},
       Instruction{.instr = PrintStats{.everything = true}},
       Instruction{.instr = Dealloc{.index = 18446744073709551615ULL}},
       Instruction{.instr = PrintStats{.everything = true}},
       Instruction{.instr = ReleasePagesBreakingHugepages{
                       .desired = 18446744073709551615ULL,
                       .soft_limit_exceeded = true}}});
}

TEST(HugePageAwareAllocatorTest, b470332457) {
  // Regression found in b/470332457.
  FuzzHPAA(
      FuzzHugePageAwareAllocatorOptions{
          .tag = MemoryTag::kNormalP1,
          .use_huge_region_more_often =
              HugeRegionUsageOption::kUseForAllLargeAllocs},
      {Instruction{.instr = GatherStatsPbtxt{}},
       Instruction{.instr = PrintStats{.everything = false}},
       Instruction{
           .instr = ChangeParam{.op =
                                    SetFillerSkipSubreleaseLongInterval{
                                        .duration_ns = 7795569869804108969}}},
       Instruction{.instr = ReleasePages{.desired = 9223372036854775807,
                                         .release_memory_to_system = false}}});
}

TEST(HugePageAwareAllocatorTest, b509249056) {
  FuzzHPAA(
      FuzzHugePageAwareAllocatorOptions{
          .tag = static_cast<tcmalloc::tcmalloc_internal::MemoryTag>(4),
          .use_huge_region_more_often =
              static_cast<tcmalloc::tcmalloc_internal::HugeRegionUsageOption>(
                  0)},
      {Instruction{ChangeParam{ReentrantSubprogram{
           {Instruction{ReleasePagesBreakingHugepages{1, false}}}}}},
       Instruction{Alloc{15576967129319913528ULL, 1, 18446744073709551615ULL,
                         false, false}},
       Instruction{Alloc{9223372036854775807ULL, 0, 9223372036854775809ULL,
                         false, false}},
       Instruction{GatherStatsPbtxt{}}, Instruction{PrintStats{true}},
       Instruction{Dealloc{18446744073709551615ULL}},
       Instruction{
           ReleasePagesBreakingHugepages{18446744073709551615ULL, true}}});
}

TEST(HugePageAwareAllocatorTest, b552964934) {
  FuzzHPAA(
      FuzzHugePageAwareAllocatorOptions{
          .tag = static_cast<MemoryTag>(255),
          .use_huge_region_more_often = HugeRegionUsageOption::kDefault},
      {Instruction{Alloc{.length = 18446744073709551615ULL,
                         .num_objects = 0,
                         .alignment = 0,
                         .use_aligned = false,
                         .dense = true}},
       Instruction{
           ChangeParam{ReentrantSubprogram{{Instruction{GatherStatsPbtxt{}}}}}},
       Instruction{Alloc{.length = 0,
                         .num_objects = 18446744073709551615ULL,
                         .alignment = 4986272791356442309ULL,
                         .use_aligned = true,
                         .dense = true}},
       Instruction{GatherStatsPbtxt{}},
       Instruction{Alloc{.length = 13048929216474975844ULL,
                         .num_objects = 9223372036854775807ULL,
                         .alignment = 9223372036854775807ULL,
                         .use_aligned = true,
                         .dense = true}},
       Instruction{GatherStatsPbtxt{}},
       Instruction{Dealloc{.index = 16919753808000827841ULL}},
       Instruction{Alloc{.length = 18446744073709551615ULL,
                         .num_objects = 9223372036854775807ULL,
                         .alignment = 18446744073709551615ULL,
                         .use_aligned = true,
                         .dense = true}},
       Instruction{ReleasePagesBreakingHugepages{
           .desired = 679955740158667079ULL, .soft_limit_exceeded = true}},
       Instruction{Alloc{.length = 0,
                         .num_objects = 7098601873124347438ULL,
                         .alignment = 18446744073709551615ULL,
                         .use_aligned = false,
                         .dense = true}},
       Instruction{Alloc{.length = 8989307443488097081ULL,
                         .num_objects = 9223372036854775807ULL,
                         .alignment = 1,
                         .use_aligned = false,
                         .dense = false}},
       Instruction{Alloc{.length = 12986827811513833492ULL,
                         .num_objects = 0,
                         .alignment = 11998874783831327886ULL,
                         .use_aligned = false,
                         .dense = true}},
       Instruction{GatherStatsPbtxt{}},
       Instruction{Alloc{.length = 9223372036854775807ULL,
                         .num_objects = 0,
                         .alignment = 3877567228145324769ULL,
                         .use_aligned = true,
                         .dense = true}},
       Instruction{Alloc{.length = 0,
                         .num_objects = 0,
                         .alignment = 18165241808580756788ULL,
                         .use_aligned = true,
                         .dense = true}},
       Instruction{Alloc{.length = 18446744073709551615ULL,
                         .num_objects = 0,
                         .alignment = 1,
                         .use_aligned = true,
                         .dense = true}},
       Instruction{ReleasePages{.desired = 18446744073709551615ULL,
                                .release_memory_to_system = false}},
       Instruction{Alloc{.length = 1263613339170499819ULL,
                         .num_objects = 1,
                         .alignment = 0,
                         .use_aligned = true,
                         .dense = false}},
       Instruction{Alloc{.length = 562949953421312ULL,
                         .num_objects = 0,
                         .alignment = 9085375705730478286ULL,
                         .use_aligned = true,
                         .dense = false}},
       Instruction{GatherStatsPbtxt{}},
       Instruction{Alloc{.length = 9223372036854775807ULL,
                         .num_objects = 1040401377717634479ULL,
                         .alignment = 18446744073709551615ULL,
                         .use_aligned = true,
                         .dense = false}},
       Instruction{Alloc{.length = 1012,
                         .num_objects = 1,
                         .alignment = 1,
                         .use_aligned = true,
                         .dense = true}},
       Instruction{Alloc{.length = 1818594535876303149ULL,
                         .num_objects = 1316016424333542563ULL,
                         .alignment = 2326280589541938511ULL,
                         .use_aligned = true,
                         .dense = false}},
       Instruction{ReleasePagesBreakingHugepages{
           .desired = 18446744073709551610ULL, .soft_limit_exceeded = true}},
       Instruction{Alloc{.length = 9223372036854775807ULL,
                         .num_objects = 12111074860701291363ULL,
                         .alignment = 18446744073709551615ULL,
                         .use_aligned = true,
                         .dense = false}},
       Instruction{Dealloc{.index = 18446603336221196287ULL}},
       Instruction{Alloc{.length = 0,
                         .num_objects = 18446744073709551615ULL,
                         .alignment = 18446744073709551611ULL,
                         .use_aligned = true,
                         .dense = false}},
       Instruction{ReleasePages{.desired = 9223372036854775810ULL,
                                .release_memory_to_system = true}},
       Instruction{ReleasePagesBreakingHugepages{.desired = 1,
                                                 .soft_limit_exceeded = true}},
       Instruction{GatherStatsPbtxt{}},
       Instruction{PrintStats{.everything = true}}});
}

TEST(HugePageAwareAllocatorTest, b552964557) {
  FuzzHPAA(
      FuzzHugePageAwareAllocatorOptions{
          .tag = static_cast<tcmalloc::tcmalloc_internal::MemoryTag>(6),
          .use_huge_region_more_often =
              static_cast<tcmalloc::tcmalloc_internal::HugeRegionUsageOption>(
                  0)},
      {Instruction{Alloc{.length = 18446744073709551615ULL,
                         .num_objects = 0,
                         .alignment = 0,
                         .use_aligned = false,
                         .dense = true}},
       Instruction{
           ChangeParam{ReentrantSubprogram{{Instruction{GatherStatsPbtxt{}}}}}},
       Instruction{Alloc{.length = 0,
                         .num_objects = 18446744073709551615ULL,
                         .alignment = 4986272791356442309ULL,
                         .use_aligned = true,
                         .dense = true}},
       Instruction{GatherStatsPbtxt{}},
       Instruction{Alloc{.length = 13048929216474975844ULL,
                         .num_objects = 9223372036854775807ULL,
                         .alignment = 9223372036854775807ULL,
                         .use_aligned = true,
                         .dense = true}},
       Instruction{GatherStatsPbtxt{}},
       Instruction{Dealloc{.index = 16919753808000827841ULL}},
       Instruction{Alloc{.length = 18446744073709551615ULL,
                         .num_objects = 9223372036854775807ULL,
                         .alignment = 18446744073709551615ULL,
                         .use_aligned = true,
                         .dense = true}},
       Instruction{ReleasePagesBreakingHugepages{
           .desired = 679955740158667079ULL, .soft_limit_exceeded = true}},
       Instruction{Alloc{.length = 0,
                         .num_objects = 7098601873124347438ULL,
                         .alignment = 18446744073709551615ULL,
                         .use_aligned = false,
                         .dense = true}},
       Instruction{Alloc{.length = 8989307443488097081ULL,
                         .num_objects = 9223372036854775807ULL,
                         .alignment = 1,
                         .use_aligned = false,
                         .dense = false}},
       Instruction{Alloc{.length = 12986827811513833492ULL,
                         .num_objects = 0,
                         .alignment = 11998874783831327886ULL,
                         .use_aligned = false,
                         .dense = true}},
       Instruction{GatherStatsPbtxt{}},
       Instruction{Alloc{.length = 9223372036854775807ULL,
                         .num_objects = 0,
                         .alignment = 3877567228145324769ULL,
                         .use_aligned = true,
                         .dense = true}},
       Instruction{Alloc{.length = 0,
                         .num_objects = 0,
                         .alignment = 18165241808580756788ULL,
                         .use_aligned = true,
                         .dense = true}},
       Instruction{Alloc{.length = 18446744073709551615ULL,
                         .num_objects = 0,
                         .alignment = 1,
                         .use_aligned = true,
                         .dense = true}},
       Instruction{ReleasePages{.desired = 18446744073709551615ULL,
                                .release_memory_to_system = false}},
       Instruction{Alloc{.length = 1263613339170499819ULL,
                         .num_objects = 1,
                         .alignment = 0,
                         .use_aligned = true,
                         .dense = false}},
       Instruction{Alloc{.length = 562949953421312ULL,
                         .num_objects = 0,
                         .alignment = 9085375705730478286ULL,
                         .use_aligned = true,
                         .dense = false}},
       Instruction{GatherStatsPbtxt{}},
       Instruction{Alloc{.length = 9223372036854775807ULL,
                         .num_objects = 1040401377717634479ULL,
                         .alignment = 18446744073709551615ULL,
                         .use_aligned = true,
                         .dense = false}},
       Instruction{Alloc{.length = 1012,
                         .num_objects = 1,
                         .alignment = 1,
                         .use_aligned = true,
                         .dense = true}},
       Instruction{Alloc{.length = 1818594535876303149ULL,
                         .num_objects = 1316016424333542563ULL,
                         .alignment = 2326280589541938511ULL,
                         .use_aligned = true,
                         .dense = false}},
       Instruction{ReleasePagesBreakingHugepages{
           .desired = 18446744073709551610ULL, .soft_limit_exceeded = true}},
       Instruction{Alloc{.length = 9223372036854775807ULL,
                         .num_objects = 12111074860701291363ULL,
                         .alignment = 18446744073709551615ULL,
                         .use_aligned = true,
                         .dense = false}},
       Instruction{Dealloc{.index = 18446603336221196287ULL}},
       Instruction{Alloc{.length = 0,
                         .num_objects = 18446744073709551615ULL,
                         .alignment = 18446744073709551611ULL,
                         .use_aligned = true,
                         .dense = false}},
       Instruction{ReleasePages{.desired = 9223372036854775810ULL,
                                .release_memory_to_system = true}},
       Instruction{ReleasePagesBreakingHugepages{.desired = 1,
                                                 .soft_limit_exceeded = true}},
       Instruction{GatherStatsPbtxt{}},
       Instruction{PrintStats{.everything = true}}});
}

TEST(HugePageAwareAllocatorTest, PrinterTest) {
  Alloc a{.length = 15576967129319913528ULL,
          .num_objects = 1,
          .alignment = 18446744073709551615ULL,
          .use_aligned = false,
          .dense = true};
  EXPECT_EQ(
      absl::StrCat(a),
      "Alloc{.length=15576967129319913528, .num_objects=1, "
      ".alignment=18446744073709551615, .use_aligned=false, .dense=true}");

  Dealloc d{.index = 18446744073709551615ULL};
  EXPECT_EQ(absl::StrCat(d), "Dealloc{.index=18446744073709551615}");

  Instruction i{Alloc{1, 2, 3, true, false}};
  EXPECT_EQ(absl::StrCat(i),
            "Instruction{.instr=Alloc{.length=1, .num_objects=2, .alignment=3, "
            ".use_aligned=true, .dense=false}}");

  ReentrantSubprogram r{{Instruction{Dealloc{5}}}};
  EXPECT_EQ(absl::StrCat(r),
            "ReentrantSubprogram{.subprogram={Instruction{.instr=Dealloc{."
            "index=5}}}}");

  EXPECT_EQ(absl::StrCat(GatherStatsPbtxt{}), "GatherStatsPbtxt{}");
  EXPECT_EQ(absl::StrCat(GatherStatsPbtxt{}), "GatherStatsPbtxt{}");
  EXPECT_EQ(absl::StrCat(GatherSpanStats{}), "GatherSpanStats{}");
  EXPECT_EQ(
      absl::StrCat(TreatTrackers{.enable_collapse = EnableCollapse::kEnabled}),
      "TreatTrackers{.enable_collapse=EnableCollapse::kEnabled}");
  EXPECT_EQ(absl::StrCat(ResetSubreleaseIntervals{}),
            "ResetSubreleaseIntervals{}");
  EXPECT_EQ(absl::StrCat(SetAllocateSucceeds{.value = false}),
            "SetAllocateSucceeds{.value=false}");
  EXPECT_EQ(absl::StrCat(SetCollapseSucceeds{.value = true}),
            "SetCollapseSucceeds{.value=true}");
  EXPECT_EQ(absl::StrCat(SetSubreleaseUnbackedHugepages{.value = false}),
            "SetSubreleaseUnbackedHugepages{.value=false}");
}

}  // namespace
}  // namespace tcmalloc::tcmalloc_internal
