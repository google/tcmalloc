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

// As we read the fuzzer input, we update these variables to control global
// state.
int64_t fake_clock = 0;
bool unback_success = true;
bool collapse_success = true;
int collapse_latency = 0;
int error_number = 0;
std::optional<bool> is_hugepage_backed = true;
Bitmap<kMaxResidencyBits> unbacked_bitmap;
Bitmap<kMaxResidencyBits> swapped_bitmap;

int64_t mock_clock() { return fake_clock; }

double freq() { return 1 << 10; }

absl::flat_hash_set<PageId>& ReleasedPages() {
  static auto* set = new absl::flat_hash_set<PageId>();
  return *set;
}

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
  [[nodiscard]] MemoryModifyStatus operator()(Range r) override {
    if (release_callback_) {
      release_callback_();
    }
    if (!unback_success) {
      return {.success = false, .error_number = 0};
    }

    absl::flat_hash_set<PageId>& released_set = ReleasedPages();

    PageId end = r.p + r.n;
    for (; r.p != end; ++r.p) {
      released_set.insert(r.p);
    }

    return {.success = true, .error_number = error_number};
  }

  std::function<void()> release_callback_;
};

class MockSetAnonVmaName final : public MemoryTagFunction {
 public:
  void operator()(Range r, std::optional<absl::string_view> name) override {}
};

class FakePageFlags : public PageFlagsBase {
 public:
  FakePageFlags() = default;
  std::optional<PageStats> Get(const void* addr, size_t size) override {
    return PageStats{};
  }

  PageFlagsBitmaps GetSinglePageBitmaps(const void* addr) override {
    return {.status = absl::StatusCode::kUnimplemented};
  }

  std::optional<bool> IsHugepageBacked(const void* addr) override {
    return is_hugepage_backed;
  }
};

class FakeResidency : public Residency {
 public:
  FakeResidency() = default;
  std::optional<Info> Get(const void* addr, size_t size) override {
    return std::nullopt;
  };

  SinglePageBitmaps GetUnbackedAndSwappedBitmaps(const void* addr) override {
    return {unbacked_bitmap, swapped_bitmap, absl::StatusCode::kOk};
  };

  const size_t kHardwarePagesInHugePage = kHugePageSize / kPageSize;
  size_t GetHardwarePagesInHugePage() const override {
    return kHardwarePagesInHugePage;
  };

 private:
  absl::flat_hash_map<const void*, SinglePageBitmaps> residency_bitmaps_;
};

class MockCollapse final : public MemoryModifyFunction {
 public:
  [[nodiscard]] MemoryModifyStatus operator()(Range r) override {
    if (release_callback_) {
      release_callback_();
    }
    fake_clock += collapse_latency;
    return {collapse_success, error_number};
  }

  std::function<void()> release_callback_;
};

struct Allocate {
  uint16_t length;
  uint32_t num_objects;
  bool density_dense;

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

  template <typename Sink>
  friend void AbslStringify(Sink& sink, const AdvanceClock& a) {
    absl::Format(&sink, "AdvanceClock{.amount=absl::Nanoseconds(%v)}",
                 absl::ToInt64Nanoseconds(a.amount));
  }
};

struct ToggleUnback {
  template <typename Sink>
  friend void AbslStringify(Sink& sink, const ToggleUnback&) {
    sink.Append("ToggleUnback{}");
  }
};

struct GatherStats {
  template <typename Sink>
  friend void AbslStringify(Sink& sink, const GatherStats&) {
    sink.Append("GatherStats{}");
  }
};

struct ModelTail {
  uint16_t length;

  template <typename Sink>
  friend void AbslStringify(Sink& sink, const ModelTail& m) {
    absl::Format(&sink, "ModelTail{.length=%d}", m.length);
  }
};

struct MemoryLimitHitRelease {
  uint16_t desired;

  template <typename Sink>
  friend void AbslStringify(Sink& sink, const MemoryLimitHitRelease& m) {
    absl::Format(&sink, "MemoryLimitHitRelease{.desired=%d}", m.desired);
  }
};

struct GatherStatsPbtxt {
  template <typename Sink>
  friend void AbslStringify(Sink& sink, const GatherStatsPbtxt&) {
    sink.Append("GatherStatsPbtxt{}");
  }
};

struct GatherSpanStats {
  template <typename Sink>
  friend void AbslStringify(Sink& sink, const GatherSpanStats&) {
    sink.Append("GatherSpanStats{}");
  }
};

struct TreatTrackers {
  bool enable_collapse;
  bool use_userspace_collapse_heuristics;
  bool enable_unfiltered_collapse;

  template <typename Sink>
  friend void AbslStringify(Sink& sink, const TreatTrackers& t) {
    absl::Format(&sink,
                 "TreatTrackers{.enable_collapse=%v, "
                 ".use_userspace_collapse_heuristics=%v, "
                 ".enable_unfiltered_collapse=%v}",
                 t.enable_collapse, t.use_userspace_collapse_heuristics,
                 t.enable_unfiltered_collapse);
  }
};

struct UpdateBitmaps {
  bool hugepage_backed_set;
  bool hugepage_backed_val;
  uint16_t unbacked_bitmap_val;
  uint16_t swapped_bitmap_val;

  template <typename Sink>
  friend void AbslStringify(Sink& sink, const UpdateBitmaps& u) {
    absl::Format(&sink,
                 "UpdateBitmaps{.hugepage_backed_set=%v, "
                 ".hugepage_backed_val=%v, .unbacked_bitmap_val=%d, "
                 ".swapped_bitmap_val=%d}",
                 u.hugepage_backed_set, u.hugepage_backed_val,
                 u.unbacked_bitmap_val, u.swapped_bitmap_val);
  }
};

struct ToggleCollapseSuccess {
  template <typename Sink>
  friend void AbslStringify(Sink& sink, const ToggleCollapseSuccess&) {
    sink.Append("ToggleCollapseSuccess{}");
  }
};

struct SetErrorNumber {
  uint8_t error_type;
  uint32_t raw_value;

  template <typename Sink>
  friend void AbslStringify(Sink& sink, const SetErrorNumber& s) {
    absl::Format(&sink, "SetErrorNumber{.error_type=%d}", s.error_type);
  }
};

struct SetCollapseLatency {
  absl::Duration latency;

  template <typename Sink>
  friend void AbslStringify(Sink& sink, const SetCollapseLatency& s) {
    absl::Format(&sink, "SetCollapseLatency{.latency=absl::Nanoseconds(%v)}",
                 absl::ToInt64Nanoseconds(s.latency));
  }
};

struct Instruction;

struct ReentrantSubprogram {
  std::vector<Instruction> subprogram;
};

using InstructionVariant =
    std::variant<Allocate, Deallocate, Release, AdvanceClock, ToggleUnback,
                 GatherStats, ModelTail, MemoryLimitHitRelease,
                 GatherStatsPbtxt, GatherSpanStats, TreatTrackers,
                 UpdateBitmaps, ToggleCollapseSuccess, SetErrorNumber,
                 SetCollapseLatency, ReentrantSubprogram>;

struct Instruction {
  InstructionVariant instr;

  template <typename T, typename = std::enable_if_t<
                            !std::is_same_v<std::decay_t<T>, Instruction> &&
                            std::is_constructible_v<InstructionVariant, T>>>
  // NOLINTNEXTLINE(google-explicit-constructor)
  Instruction(T&& t) : instr(std::forward<T>(t)) {}

  Instruction() = default;
};

template <typename Sink>
void AbslStringify(Sink& sink, const InstructionVariant& v) {
  std::visit([&](auto&& arg) { absl::Format(&sink, "%v", arg); }, v);
}

template <typename Sink>
void AbslStringify(Sink& sink, const Instruction& i) {
  AbslStringify(sink, i.instr);
}

template <typename Sink>
void AbslStringify(Sink& sink, const ReentrantSubprogram& r) {
  absl::Format(&sink, "ReentrantSubprogram{.subprogram={%s}}",
               absl::StrJoin(r.subprogram, ", ",
                             [](std::string* out, const Instruction& i) {
                               absl::StrAppend(out, i);
                             }));
}

void FuzzFiller(const std::vector<Instruction>& instructions,
                SubreleaseUnbackedMode subrelease_unbacked_mode) {
  // Reset global state.
  MockUnback unback;
  MockCollapse collapse;
  MockSetAnonVmaName set_anon_vma_name;
  fake_clock = 0;
  unback_success = true;
  absl::flat_hash_set<PageId>& released_set = ReleasedPages();
  released_set.clear();
  // To avoid reentrancy during unback, reserve space in released_set.  We have
  // at most instructions.size() allocations, for at most kPagesPerHugePage
  // pages each, that we can track the released status of.
  //
  // TODO(b/73749855): Releasing the pageheap_lock during ReleaseFree will
  // eliminate the need for this.
  released_set.reserve(kPagesPerHugePage.raw_num() * instructions.size());

  HugePageFiller<PageTracker> filler(
      Clock{.now = mock_clock, .freq = freq}, MemoryTag::kNormal, unback,
      unback, collapse, set_anon_vma_name, subrelease_unbacked_mode);

  std::vector<PageTracker*> trackers;
  absl::flat_hash_map<PageTracker*,
                      std::vector<std::pair<Range, SpanAllocInfo>>>
      allocs;

  size_t next_hugepage = 1;

  std::vector<std::vector<Instruction>> reentrant_stack;
  int depth = 0;
  bool treating_trackers = false;

  auto run_instructions = [&](const std::vector<Instruction>& instrs) {
    for (const auto& instruction_wrapper : instrs) {
      std::visit(
          [&](auto&& arg) {
            using T = std::decay_t<decltype(arg)>;

            if constexpr (std::is_same_v<T, ReentrantSubprogram>) {
              if (depth == 0) {
                reentrant_stack.push_back(arg.subprogram);
              }
            } else if constexpr (std::is_same_v<T, Allocate>) {
              // Allocate.
              // length: We choose a Length to allocate.
              // num_objects: We select num_to_objects.
              Length n(std::clamp<size_t>(arg.length, 1,
                                          kPagesPerHugePage.raw_num() - 1));
              size_t num_objects = std::max<size_t>(arg.num_objects, 1);
              AccessDensityPrediction density =
                  arg.density_dense ? AccessDensityPrediction::kDense
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
              absl::flat_hash_set<PageId>& released_set = ReleasedPages();

              if (depth == 0) {
                TC_CHECK_EQ(filler.size().raw_num(), trackers.size());
                TC_CHECK_EQ(filler.unmapped_pages().raw_num(),
                            released_set.size());
              }

              HugePageFiller<PageTracker>::TryGetResult result;
              {
                PageHeapSpinLockHolder l;
                result = filler.TryGet(n, alloc_info);
              }

              if (result.pt == nullptr) {
                // Since small objects are likely to be found, we model those
                // tail donations separately.
                const bool donated = n > kPagesPerHugePage / 2;
                result.pt = new PageTracker(HugePage{.pn = next_hugepage},
                                            donated, fake_clock);
                next_hugepage++;
                {
                  PageHeapSpinLockHolder l;
                  result.page = result.pt->Get(n, alloc_info).page;
                  filler.Contribute(result.pt, donated, alloc_info);
                }
                trackers.push_back(result.pt);
              }

              for (PageId p = result.page, end = p + n; p != end; ++p) {
                released_set.erase(p);
              }

              allocs[result.pt].push_back({{result.page, n}, alloc_info});

              if (depth == 0) {
                TC_CHECK_EQ(filler.size().raw_num(), trackers.size());
                TC_CHECK_EQ(filler.unmapped_pages().raw_num(),
                            released_set.size());
              }
            } else if constexpr (std::is_same_v<T, Deallocate>) {
              // Deallocate.
              // tracker_index: Index of the huge page (from trackers) to select
              // alloc_index: Index of the allocation (on pt) to select
              if (trackers.empty()) return;
              const size_t lo = arg.tracker_index % trackers.size();
              PageTracker* pt = trackers[lo];
              TC_CHECK(!allocs[pt].empty());
              const size_t hi = arg.alloc_index % allocs[pt].size();
              auto [alloc, alloc_info] = allocs[pt][hi];

              std::swap(allocs[pt][hi], allocs[pt].back());
              allocs[pt].resize(allocs[pt].size() - 1);
              bool last_alloc = allocs[pt].empty();
              if (last_alloc) {
                allocs.erase(pt);
                std::swap(trackers[lo], trackers.back());
                trackers.resize(trackers.size() - 1);
              }

              PageTracker* ret;
              {
                PageHeapSpinLockHolder l;
                ret = filler.Put(pt, alloc, alloc_info);
              }
              if (depth == 0) {
                TC_CHECK_EQ(ret != nullptr, last_alloc);
              }
              absl::flat_hash_set<PageId>& released_set = ReleasedPages();
              if (ret) {
                HugePage hp = ret->location();
                for (PageId p = hp.first_page(),
                            end = hp.first_page() + kPagesPerHugePage;
                     p != end; ++p) {
                  released_set.erase(p);
                }
                delete ret;
              }

              if (depth == 0) {
                TC_CHECK_EQ(filler.size().raw_num(), trackers.size());
                TC_CHECK_EQ(filler.unmapped_pages().raw_num(),
                            released_set.size());
              }
            } else if constexpr (std::is_same_v<T, Release>) {
              // Release
              // hit_limit: Whether are trying to apply TCMalloc's memory limits
              // use_peak_interval: Whether using peak interval for skip
              // subrelease peak_interval: Peak interval for skip subrelease (if
              // using peak interval) short_interval: Short interval for skip
              // subrelease (if not using peak interval) long_interval: Long
              // interval for skip subrelease (if not using peak interval)
              // desired_pages: Number of pages to try to release
              // release_partial_allocs: Whether we release all free pages from
              // partial allocs.
              bool hit_limit = arg.hit_limit;
              bool use_peak_interval = arg.use_peak_interval;
              SkipSubreleaseIntervals skip_subrelease_intervals;
              if (use_peak_interval) {
                skip_subrelease_intervals.peak_interval = arg.peak_interval;
              } else {
                skip_subrelease_intervals.short_interval = arg.short_interval;
                skip_subrelease_intervals.long_interval = arg.long_interval;
                if (skip_subrelease_intervals.short_interval >
                    skip_subrelease_intervals.long_interval) {
                  std::swap(skip_subrelease_intervals.short_interval,
                            skip_subrelease_intervals.long_interval);
                }
              }
              Length desired(arg.desired_pages);
              const bool release_partial_allocs = arg.release_partial_allocs;
              size_t to_release_from_partial_allocs;

              Length released;
              {
                PageHeapSpinLockHolder l;
                to_release_from_partial_allocs =
                    HugePageFiller<PageTracker>::kPartialAllocPagesRelease *
                    filler.FreePagesInPartialAllocs().raw_num();
                released =
                    filler.ReleasePages(desired, skip_subrelease_intervals,
                                        release_partial_allocs, hit_limit);
              }

              if (release_partial_allocs && !hit_limit &&
                  !skip_subrelease_intervals.SkipSubreleaseEnabled() &&
                  unback_success) {
                if (depth == 0) {
                  TC_CHECK_GE(released.raw_num(),
                              to_release_from_partial_allocs);
                }
              }
            } else if constexpr (std::is_same_v<T, AdvanceClock>) {
              // Advance clock
              // amount: Advances clock by this amount in arbitrary units.
              fake_clock += absl::ToInt64Nanoseconds(
                  std::clamp(arg.amount, absl::ZeroDuration(), absl::Hours(1)));
            } else if constexpr (std::is_same_v<T, ToggleUnback>) {
              // Toggle unback, simulating madvise potentially failing or
              // succeeding.
              unback_success = !unback_success;

            } else if constexpr (std::is_same_v<T, GatherStats>) {
              // Gather stats
              std::string output;
              output.resize(1 << 20);
              Printer p(&output[0], output.size());
              FakePageFlags pageflags;
              PageHeapSpinLockHolder l;
              filler.Print(p, true, pageflags);
            } else if constexpr (std::is_same_v<T, ModelTail>) {
              // Model a tail from a larger allocation.  The tail can have any
              // size [1,kPagesPerHugePage).
              //
              // length: We choose a Length to allocate.
              const Length n(std::clamp<size_t>(
                  arg.length, 1, kPagesPerHugePage.raw_num() - 1));
              absl::flat_hash_set<PageId>& released_set = ReleasedPages();

              auto* pt = new PageTracker(HugePage{.pn = next_hugepage},
                                         /*was_donated=*/true, fake_clock);
              next_hugepage++;
              PageId start;
              {
                PageHeapSpinLockHolder l;
                start = pt->Get(n, {1, AccessDensityPrediction::kSparse}).page;
                filler.Contribute(pt, /*donated=*/true,
                                  {1, AccessDensityPrediction::kSparse});
              }

              trackers.push_back(pt);

              for (PageId p = start, end = p + n; p != end; ++p) {
                released_set.erase(p);
              }

              allocs[pt].push_back(
                  {{start, n}, {1, AccessDensityPrediction::kSparse}});

              if (depth == 0) {
                TC_CHECK_EQ(filler.size().raw_num(), trackers.size());
                TC_CHECK_EQ(filler.unmapped_pages().raw_num(),
                            released_set.size());
              }
            } else if constexpr (std::is_same_v<T, MemoryLimitHitRelease>) {
              // Memory limit hit. Release.
              // desired: Number of pages to try to release
              Length desired(arg.desired);
              Length released;
              const Length free = filler.free_pages();
              {
                PageHeapSpinLockHolder l;
                released = filler.ReleasePages(
                    desired, SkipSubreleaseIntervals{},
                    /*release_partial_alloc_pages=*/false, /*hit_limit=*/true);
              }
              const Length expected =
                  unback_success ? std::min(free, desired) : Length(0);
              if (depth == 0) {
                TC_CHECK_GE(released.raw_num(), expected.raw_num());
              }
            } else if constexpr (std::is_same_v<T, GatherStatsPbtxt>) {
              // Gather stats in pbtxt format.
              std::string output;
              output.resize(1 << 20);
              Printer p(&output[0], output.size());
              FakePageFlags pageflags;
              {
                PbtxtRegion region(p, kTop);
                PageHeapSpinLockHolder l;
                filler.PrintInPbtxt(region, pageflags);
              }
              TC_CHECK_LE(p.SpaceRequired(), output.size());
            } else if constexpr (std::is_same_v<T, GatherSpanStats>) {
              // Gather span stats.
              SmallSpanStats small;
              LargeSpanStats large;
              filler.AddSpanStats(&small, &large);
            } else if constexpr (std::is_same_v<T, TreatTrackers>) {
              if (treating_trackers) return;
              treating_trackers = true;
              FakePageFlags pageflags;
              FakeResidency residency;
              PageHeapSpinLockHolder l;
              filler.TreatHugepageTrackers(
                  arg.enable_collapse ? EnableCollapse::kEnabled
                                      : EnableCollapse::kDisabled,
                  arg.enable_unfiltered_collapse
                      ? EnableUnfilteredCollapse::kEnabled
                      : EnableUnfilteredCollapse::kDisabled,
                  &pageflags, &residency);
              treating_trackers = false;
              absl::flat_hash_set<PageId>& released_set = ReleasedPages();
              while (PageTracker* pt = filler.FetchFullyFreedTracker()) {
                HugePage hp = pt->location();
                for (PageId p = hp.first_page(),
                            end = hp.first_page() + kPagesPerHugePage;
                     p != end; ++p) {
                  released_set.erase(p);
                }
                delete pt;
              }
              for (PageTracker* pt : trackers) {
                HugePage hp = pt->location();
                Bitmap<kPagesPerHugePage.raw_num()> rel =
                    pt->released_by_page();
                for (size_t i = 0; i < kPagesPerHugePage.raw_num(); ++i) {
                  PageId p = hp.first_page() + Length(i);
                  if (rel.GetBit(i)) {
                    released_set.insert(p);
                  } else {
                    released_set.erase(p);
                  }
                }
              }
            } else if constexpr (std::is_same_v<T, UpdateBitmaps>) {
              if (arg.hugepage_backed_set) {
                is_hugepage_backed = arg.hugepage_backed_val;
              } else {
                is_hugepage_backed = std::nullopt;
              }
              if (is_hugepage_backed.value_or(false)) {
                unbacked_bitmap.Clear();
                swapped_bitmap.Clear();
              } else {
                unbacked_bitmap = GetBitmap(arg.unbacked_bitmap_val);
                swapped_bitmap = GetBitmap(arg.swapped_bitmap_val);
              }
            } else if constexpr (std::is_same_v<T, ToggleCollapseSuccess>) {
              collapse_success = !collapse_success;
            } else if constexpr (std::is_same_v<T, SetErrorNumber>) {
              switch (arg.error_type % 4) {
                case 0:
                  error_number = ENOMEM;
                  break;
                case 1:
                  error_number = EAGAIN;
                  break;
                case 2:
                  error_number = EBUSY;
                  break;
                case 3:
                  error_number = EINVAL;
                  break;
              }
            } else if constexpr (std::is_same_v<T, SetCollapseLatency>) {
              collapse_latency = absl::ToInt64Nanoseconds(std::clamp(
                  arg.latency, absl::ZeroDuration(), absl::Seconds(1)));
            }
          },
          instruction_wrapper.instr);
    }
  };

  auto release_callback = [&]() {
    if (tcmalloc::tcmalloc_internal::pageheap_lock.IsHeld()) {
      return;
    }
    if (reentrant_stack.empty()) {
      return;
    }
    if (depth >= 5) {
      return;
    }

    auto ops = std::move(reentrant_stack.back());
    reentrant_stack.pop_back();

    depth++;
    ScopedAllocationAllow allow;
    run_instructions(ops);
    depth--;
  };

  unback.release_callback_ = release_callback;
  collapse.release_callback_ = release_callback;

  run_instructions(instructions);

  // Shut down, confirm filler is empty.
  CHECK_EQ(ReleasedPages().size(), filler.unmapped_pages().raw_num());
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

auto NonNegativeDurationDomain() {
  return fuzztest::Map([](int64_t ns) { return absl::Nanoseconds(ns); },
                       fuzztest::NonNegative<int64_t>());
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
          NonNegativeDurationDomain()),
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
                     .use_userspace_collapse_heuristics = false,
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
                        .use_userspace_collapse_heuristics = false,
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
                        .use_userspace_collapse_heuristics = false,
                        .enable_unfiltered_collapse = true},
          TreatTrackers{.enable_collapse = false,
                        .use_userspace_collapse_heuristics = false,
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
                        .use_userspace_collapse_heuristics = false,
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
                        .use_userspace_collapse_heuristics = true,
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
                     .use_userspace_collapse_heuristics = true,
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
                     .use_userspace_collapse_heuristics = false,
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
      .use_userspace_collapse_heuristics = false,
      .enable_unfiltered_collapse = true,
  });

  FuzzFiller(instructions, SubreleaseUnbackedMode::kDisabled);
}

TEST(HugePageFillerTest, SubreleaseUnbackedRegression) {
  FuzzFiller(
      {ModelTail{.length = 0}, GatherStatsPbtxt{},
       TreatTrackers{.enable_collapse = true,
                     .use_userspace_collapse_heuristics = false,
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
                                     .use_userspace_collapse_heuristics = true,
                                     .enable_unfiltered_collapse = false};
    std::string s = absl::StrFormat("%v", inst);
    EXPECT_EQ(s,
              "TreatTrackers{.enable_collapse=true, "
              ".use_userspace_collapse_heuristics=true, "
              ".enable_unfiltered_collapse=false}");
  }
  {
    Instruction inst = UpdateBitmaps{.hugepage_backed_set = true,
                                     .hugepage_backed_val = false,
                                     .unbacked_bitmap_val = 1,
                                     .swapped_bitmap_val = 2};
    std::string s = absl::StrFormat("%v", inst);
    EXPECT_EQ(
        s,
        "UpdateBitmaps{.hugepage_backed_set=true, .hugepage_backed_val=false, "
        ".unbacked_bitmap_val=1, .swapped_bitmap_val=2}");
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
                        .use_userspace_collapse_heuristics = false,
                        .enable_unfiltered_collapse = false},
      },
      SubreleaseUnbackedMode::kDisabled);
}

}  // namespace
}  // namespace tcmalloc::tcmalloc_internal
