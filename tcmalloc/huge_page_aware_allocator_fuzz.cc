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
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "gtest/gtest.h"
#include "fuzztest/fuzztest.h"
#include "absl/base/attributes.h"
#include "absl/log/check.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_format.h"
#include "absl/strings/str_join.h"
#include "absl/time/time.h"
#include "tcmalloc/common.h"
#include "tcmalloc/huge_page_aware_allocator.h"
#include "tcmalloc/huge_page_filler.h"
#include "tcmalloc/huge_pages.h"
#include "tcmalloc/huge_region.h"
#include "tcmalloc/internal/logging.h"
#include "tcmalloc/internal/memory_tag.h"
#include "tcmalloc/internal/pageflags.h"
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
  int64_t huge_cache_time_ns;

  explicit operator HugePageAwareAllocatorOptions() const {
    HugePageAwareAllocatorOptions options;
    options.tag = tag;
    options.use_huge_region_more_often = use_huge_region_more_often;
    options.huge_cache_time = absl::Nanoseconds(huge_cache_time_ns);
    return options;
  }

  template <typename Sink>
  friend void AbslStringify(Sink& sink,
                            const FuzzHugePageAwareAllocatorOptions& options) {
    absl::Format(
        &sink,
        "FuzzHugePageAwareAllocatorOptions{"
        ".tag = static_cast<tcmalloc::tcmalloc_internal::MemoryTag>(%d), "
        ".use_huge_region_more_often = "
        "static_cast<tcmalloc::tcmalloc_internal::"
        "HugeRegionUsageOption>(%d), "
        ".huge_cache_time_ns = %d}",
        static_cast<int>(options.tag),
        static_cast<int>(options.use_huge_region_more_often),
        options.huge_cache_time_ns);
  }
};

class FakeStaticForwarderWithUnback : public FakeStaticForwarder {
 public:
  MemoryModifyStatus ReleasePages(Range r) {
    pending_release_ += r.n;
    release_callback_();
    pending_release_ -= r.n;

    return FakeStaticForwarder::ReleasePages(r);
  }

  void Back(Range r) {
    ASSERT_TRUE(BackAllocations());
    TC_CHECK_LE(r.in_bytes(), BackSizeThresholdBytes());
    return FakeStaticForwarder::Back(r);
  }

  Length pending_release_;
  std::function<void()> release_callback_;
};

struct Alloc {
  size_t length;
  size_t num_objects;
  size_t alignment;
  bool use_aligned;
  bool dense;

  template <typename Sink>
  friend void AbslStringify(Sink& sink, const Alloc& a) {
    absl::Format(&sink,
                 "Alloc{.length=%d, .num_objects=%d, .alignment=%d, "
                 ".use_aligned=%d, .dense=%d}",
                 a.length, a.num_objects, a.alignment, a.use_aligned, a.dense);
  }
};

struct Dealloc {
  size_t index;

  template <typename Sink>
  friend void AbslStringify(Sink& sink, const Dealloc& d) {
    absl::Format(&sink, "Dealloc{.index=%d}", d.index);
  }
};

struct ReleasePages {
  size_t desired;
  bool release_memory_to_system;

  template <typename Sink>
  friend void AbslStringify(Sink& sink, const ReleasePages& r) {
    absl::Format(&sink,
                 "ReleasePages{.desired=%d, .release_memory_to_system=%d}",
                 r.desired, r.release_memory_to_system);
  }
};

struct ReleasePagesBreakingHugepages {
  size_t desired;
  bool soft_limit_exceeded;

  template <typename Sink>
  friend void AbslStringify(Sink& sink,
                            const ReleasePagesBreakingHugepages& r) {
    absl::Format(&sink,
                 "ReleasePagesBreakingHugepages{.desired=%d, "
                 ".soft_limit_exceeded=%d}",
                 r.desired, r.soft_limit_exceeded);
  }
};

struct GatherStatsPbtxt {
  template <typename Sink>
  friend void AbslStringify(Sink& sink, const GatherStatsPbtxt& g) {
    absl::Format(&sink, "GatherStatsPbtxt{}");
  }
};

struct PrintStats {
  bool everything;

  template <typename Sink>
  friend void AbslStringify(Sink& sink, const PrintStats& p) {
    absl::Format(&sink, "PrintStats{.everything=%d}", p.everything);
  }
};

struct GatherAndCheckStats {
  template <typename Sink>
  friend void AbslStringify(Sink& sink, const GatherAndCheckStats& g) {
    absl::Format(&sink, "GatherAndCheckStats{}");
  }
};

struct SetFillerSkipSubreleaseShortInterval {
  int64_t duration_ns;

  template <typename Sink>
  friend void AbslStringify(Sink& sink,
                            const SetFillerSkipSubreleaseShortInterval& s) {
    absl::Format(&sink,
                 "SetFillerSkipSubreleaseShortInterval{.duration_ns = %d}",
                 s.duration_ns);
  }
};

struct SetFillerSkipSubreleaseLongInterval {
  int64_t duration_ns;

  template <typename Sink>
  friend void AbslStringify(Sink& sink,
                            const SetFillerSkipSubreleaseLongInterval& s) {
    absl::Format(&sink,
                 "SetFillerSkipSubreleaseLongInterval{.duration_ns = %d}",
                 s.duration_ns);
  }
};

struct SetReleasePartialAllocPages {
  bool value;

  template <typename Sink>
  friend void AbslStringify(Sink& sink, const SetReleasePartialAllocPages& s) {
    absl::Format(&sink, "SetReleasePartialAllocPages{.value=%d}", s.value);
  }
};

struct SetHpaaSubrelease {
  bool value;

  template <typename Sink>
  friend void AbslStringify(Sink& sink, const SetHpaaSubrelease& s) {
    absl::Format(&sink, "SetHpaaSubrelease{.value=%d}", s.value);
  }
};

struct SetReleaseSucceeds {
  bool value;

  template <typename Sink>
  friend void AbslStringify(Sink& sink, const SetReleaseSucceeds& s) {
    absl::Format(&sink, "SetReleaseSucceeds{.value=%d}", s.value);
  }
};

struct SetHugeRegionDemandBasedRelease {
  bool value;

  template <typename Sink>
  friend void AbslStringify(Sink& sink,
                            const SetHugeRegionDemandBasedRelease& s) {
    absl::Format(&sink, "SetHugeRegionDemandBasedRelease{.value=%d}", s.value);
  }
};

struct SetUseUserspaceCollapseHeuristics {
  bool value;

  template <typename Sink>
  friend void AbslStringify(Sink& sink,
                            const SetUseUserspaceCollapseHeuristics& s) {
    absl::Format(&sink, "SetUseUserspaceCollapseHeuristics{.value=%d}",
                 s.value);
  }
};

struct SetBackAllocations {
  bool value;

  template <typename Sink>
  friend void AbslStringify(Sink& sink, const SetBackAllocations& s) {
    absl::Format(&sink, "SetBackAllocations{.value=%d}", s.value);
  }
};

struct SetBackSizeThresholdBytes {
  int32_t value;

  template <typename Sink>
  friend void AbslStringify(Sink& sink, const SetBackSizeThresholdBytes& s) {
    absl::Format(&sink, "SetBackSizeThresholdBytes{.value=%d}", s.value);
  }
};

struct ResetSubreleaseIntervals {
  template <typename Sink>
  friend void AbslStringify(Sink& sink, const ResetSubreleaseIntervals& r) {
    absl::Format(&sink, "ResetSubreleaseIntervals{}");
  }
};

struct Instruction;

template <typename Sink>
void AbslStringify(Sink& sink, const Instruction& i);

struct ReentrantSubprogram {
  std::vector<Instruction> subprogram;
};

using ParamOp =
    std::variant<ResetSubreleaseIntervals, SetFillerSkipSubreleaseShortInterval,
                 SetFillerSkipSubreleaseLongInterval,
                 SetReleasePartialAllocPages, SetHpaaSubrelease,
                 SetReleaseSucceeds, SetHugeRegionDemandBasedRelease,
                 SetUseUserspaceCollapseHeuristics, SetBackAllocations,
                 SetBackSizeThresholdBytes, ReentrantSubprogram>;

template <typename Sink>
void AbslStringify(Sink& sink, const ParamOp& p) {
  std::visit([&](auto&& arg) { AbslStringify(sink, arg); }, p);
}

struct ChangeParam {
  ParamOp op;

  template <typename Sink>
  friend void AbslStringify(Sink& sink, const ChangeParam& c) {
    absl::Format(&sink, "ChangeParam{.op=%v}", c.op);
  }
};

using InstructionVariant =
    std::variant<Alloc, Dealloc, ReleasePages, ReleasePagesBreakingHugepages,
                 GatherStatsPbtxt, PrintStats, GatherAndCheckStats,
                 ChangeParam>;

template <typename Sink>
void AbslStringify(Sink& sink, const InstructionVariant& v) {
  std::visit([&](auto&& arg) { AbslStringify(sink, arg); }, v);
}

struct Instruction {
  InstructionVariant instr;

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

void FuzzHPAA(FuzzHugePageAwareAllocatorOptions fuzz_options,
              const std::vector<Instruction>& instructions) {
  GTEST_SKIP() << "Skipping";
  HugePageAwareAllocatorOptions options =
      static_cast<HugePageAwareAllocatorOptions>(fuzz_options);
  // Use kNormalP1 memory tag only if we have more than one partitions.
  if (kNormalPartitions == 1 && options.tag == MemoryTag::kNormalP1) {
    options.tag = MemoryTag::kNormalP0;
  }

  HugePageAwareAllocator<FakeStaticForwarderWithUnback> allocator(options);
  auto& forwarder = allocator.forwarder();

  struct SpanInfo {
    Span* span;
    size_t objects_per_span;
  };
  std::vector<SpanInfo> allocs;
  Length allocated;
  PageReleaseStats expected_stats;

  std::vector<std::vector<Instruction>> reentrant_stack;
  int depth = 0;

  std::string output;
  output.resize(1 << 20);

  auto run_instructions = [&](const std::vector<Instruction>& instrs) {
    for (const auto& instruction_wrapper : instrs) {
      std::visit(
          [&](auto&& arg) {
            using T = std::decay_t<decltype(arg)>;
            if constexpr (std::is_same_v<T, Alloc>) {
              Length length(std::clamp<size_t>(
                  arg.length, 1, kPagesPerHugePage.raw_num() - 1));
              size_t num_objects = std::max<size_t>(arg.num_objects, 1);
              size_t object_size = length.in_bytes() / num_objects;
              const bool use_aligned = arg.use_aligned;
              const Length align(
                  use_aligned
                      ? std::clamp<size_t>(arg.alignment, 1,
                                           kPagesPerHugePage.raw_num() - 1)
                      : 1);
              AccessDensityPrediction density =
                  arg.dense ? AccessDensityPrediction::kDense
                            : AccessDensityPrediction::kSparse;

              if (object_size > kMaxSize || align > Length(1)) {
                // Truncate to a single object.
                num_objects = 1;
                // TODO(b/283843066): Revisit this once we have fluid
                // partitioning.
                density = AccessDensityPrediction::kSparse;
              } else if (!SizeMap::IsValidSizeClass(object_size,
                                                    length.raw_num(),
                                                    kMinObjectsToMove)) {
                // This is an invalid size class, so skip it.
                return;
              } else if (density == AccessDensityPrediction::kDense) {
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
              TC_CHECK(density == AccessDensityPrediction::kSparse ||
                       length == Length(1));
              if (use_aligned) {
                s = allocator.NewAligned(length, align, alloc_info);
              } else {
                s = allocator.New(length, alloc_info);
              }
              TC_CHECK_NE(s, nullptr);
              TC_CHECK_GE(s->num_pages().raw_num(), length.raw_num());

              allocs.push_back(SpanInfo{s, num_objects});
              allocated += s->num_pages();
            } else if constexpr (std::is_same_v<T, Dealloc>) {
              if (allocs.empty()) return;

              const size_t pos = arg.index % allocs.size();
              std::swap(allocs[pos], allocs[allocs.size() - 1]);

              SpanInfo span_info = allocs[allocs.size() - 1];
              allocs.resize(allocs.size() - 1);
              allocated -= span_info.span->num_pages();

#ifdef TCMALLOC_INTERNAL_LEGACY_LOCKING
              PageHeapSpinLockHolder l;
              allocator.Delete(span_info.span,
                               {.objects_per_span = span_info.objects_per_span,
                                .density = AccessDensityPrediction::kSparse});
#else
              PageAllocatorInterface::AllocationState a{
                  Range(span_info.span->first_page(),
                        span_info.span->num_pages()),
                  span_info.span->donated(),
              };
              allocator.forwarder().DeleteSpan(span_info.span);
              PageHeapSpinLockHolder l;
              allocator.Delete(a,
                               {.objects_per_span = span_info.objects_per_span,
                                .density = AccessDensityPrediction::kSparse});
#endif  // TCMALLOC_INTERNAL_LEGACY_LOCKING
            } else if constexpr (std::is_same_v<T, ReleasePages>) {
              Length desired(arg.desired);
              const PageReleaseReason reason =
                  arg.release_memory_to_system
                      ? PageReleaseReason::kReleaseMemoryToSystem
                      : PageReleaseReason::kProcessBackgroundActions;
              Length released;
              PageReleaseStats actual_stats;
              {
                PageHeapSpinLockHolder l;
                released = allocator.ReleaseAtLeastNPages(desired, reason);
                actual_stats = allocator.GetReleaseStats();
              }

              expected_stats.total += released;
              if (reason == PageReleaseReason::kReleaseMemoryToSystem) {
                expected_stats.release_memory_to_system += released;
              } else {
                expected_stats.process_background_actions += released;
              }

              TC_CHECK_EQ(actual_stats, expected_stats);
            } else if constexpr (std::is_same_v<
                                     T, ReleasePagesBreakingHugepages>) {
              Length desired(arg.desired);
              const PageReleaseReason reason =
                  arg.soft_limit_exceeded
                      ? PageReleaseReason::kSoftLimitExceeded
                      : PageReleaseReason::kHardLimitExceeded;
              Length released;
              size_t releasable_bytes;
              PageReleaseStats actual_stats;
              {
                PageHeapSpinLockHolder l;
                releasable_bytes = allocator.FillerStats().free_bytes +
                                   allocator.RegionsFreeBacked().in_bytes() +
                                   allocator.CacheStats().free_bytes;
                released = allocator.ReleaseAtLeastNPagesBreakingHugepages(
                    desired, reason);
                actual_stats = allocator.GetReleaseStats();
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
            } else if constexpr (std::is_same_v<T, GatherStatsPbtxt>) {
              Printer p(&output[0], output.size());
              PageFlags pageflags;
              {
                PbtxtRegion region(p, kTop);
                allocator.PrintInPbtxt(region, pageflags);
              }
              CHECK_LE(p.SpaceRequired(), output.size());
            } else if constexpr (std::is_same_v<T, PrintStats>) {
              PageFlags pageflags;
              Printer p(&output[0], output.size());
              allocator.Print(p, arg.everything, pageflags);
            } else if constexpr (std::is_same_v<T, GatherAndCheckStats>) {
              BackingStats stats;
              {
                PageHeapSpinLockHolder l;
                stats = allocator.stats();
              }
              uint64_t used_bytes =
                  stats.system_bytes - stats.free_bytes - stats.unmapped_bytes;
              TC_CHECK_EQ(
                  used_bytes,
                  allocated.in_bytes() + forwarder.pending_release_.in_bytes());
            } else if constexpr (std::is_same_v<T, ChangeParam>) {
              std::visit(
                  [&](auto&& param_arg) {
                    using P = std::decay_t<decltype(param_arg)>;
                    if constexpr (std::is_same_v<P, ResetSubreleaseIntervals>) {
                      forwarder.set_filler_skip_subrelease_short_interval(
                          absl::ZeroDuration());
                      forwarder.set_filler_skip_subrelease_long_interval(
                          absl::ZeroDuration());
                    } else if constexpr (
                        std::is_same_v<P,
                                       SetFillerSkipSubreleaseShortInterval>) {
                      forwarder.set_filler_skip_subrelease_short_interval(
                          absl::Nanoseconds(param_arg.duration_ns));
                    } else if constexpr (
                        std::is_same_v<P,
                                       SetFillerSkipSubreleaseLongInterval>) {
                      forwarder.set_filler_skip_subrelease_long_interval(
                          absl::Nanoseconds(param_arg.duration_ns));
                    } else if constexpr (std::is_same_v<
                                             P, SetReleasePartialAllocPages>) {
                      forwarder.set_release_partial_alloc_pages(
                          param_arg.value);
                    } else if constexpr (std::is_same_v<P, SetHpaaSubrelease>) {
                      forwarder.set_hpaa_subrelease(param_arg.value);
                    } else if constexpr (std::is_same_v<P,
                                                        SetReleaseSucceeds>) {
                      forwarder.set_release_succeeds(param_arg.value);
                    } else if constexpr (std::is_same_v<
                                             P,
                                             SetHugeRegionDemandBasedRelease>) {
                      forwarder.set_huge_region_demand_based_release(
                          param_arg.value);
                    } else if constexpr (
                        std::is_same_v<P, SetUseUserspaceCollapseHeuristics>) {
                      forwarder.set_use_userspace_collapse_heuristics(
                          param_arg.value);
                    } else if constexpr (std::is_same_v<P,
                                                        SetBackAllocations>) {
                      forwarder.SetBackAllocations(param_arg.value);
                    } else if constexpr (std::is_same_v<
                                             P, SetBackSizeThresholdBytes>) {
                      forwarder.SetBackSizeThresholdBytes(param_arg.value);
                    } else if constexpr (std::is_same_v<P,
                                                        ReentrantSubprogram>) {
                      reentrant_stack.push_back(param_arg.subprogram);
                    }
                  },
                  arg.op);
            }
          },
          instruction_wrapper.instr);
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

    if (reentrant_stack.empty()) {
      return;
    }

    if (depth >= 5) {
      return;
    }

    auto ops = reentrant_stack.back();
    reentrant_stack.pop_back();

    depth++;
    run_instructions(ops);
    depth--;
  };

  run_instructions(instructions);

  // Stop recursing, since allocator.Delete below might cause us to "release"
  // more pages to the system.
  reentrant_stack.clear();

  // Clean up.
  const PageReleaseStats final_stats = [&] {
    for (auto span_info : allocs) {
      Span* span = span_info.span;
      allocated -= span->num_pages();
#ifdef TCMALLOC_INTERNAL_LEGACY_LOCKING
      PageHeapSpinLockHolder l;
      allocator.Delete(span_info.span,
                       {.objects_per_span = span_info.objects_per_span,
                        .density = AccessDensityPrediction::kSparse});
#else
      PageAllocatorInterface::AllocationState a{
          Range(span_info.span->first_page(), span_info.span->num_pages()),
          span_info.span->donated(),
      };
      allocator.forwarder().DeleteSpan(span_info.span);
      PageHeapSpinLockHolder l;
      allocator.Delete(a, {.objects_per_span = span_info.objects_per_span,
                           .density = AccessDensityPrediction::kSparse});
#endif  // TCMALLOC_INTERNAL_LEGACY_LOCKING
    }

    PageHeapSpinLockHolder l;
    return allocator.GetReleaseStats();
  }();

  TC_CHECK_EQ(allocated.in_bytes(), 0);
  TC_CHECK_EQ(final_stats, expected_stats);
}

auto AnyDuration() { return fuzztest::NonNegative<int64_t>(); }

auto AnyPositiveDuration() { return fuzztest::Positive<int64_t>(); }

auto GetHPAADomain() {
  return fuzztest::Map(
      [](MemoryTag tag, HugeRegionUsageOption usage, int64_t ns) {
        // TODO(b/271282540): Reduce clamp.
        return FuzzHugePageAwareAllocatorOptions{
            tag, usage,
            absl::ToInt64Nanoseconds(

                absl::Floor(std::clamp(absl::Nanoseconds(ns), absl::Seconds(1),
                                       absl::Seconds(3600)),
                            absl::Seconds(1)))};
      },
      fuzztest::ElementOf({MemoryTag::kSampled, MemoryTag::kNormalP0,
                           MemoryTag::kNormalP1, MemoryTag::kNormal,
                           MemoryTag::kCold}),
      fuzztest::ElementOf({HugeRegionUsageOption::kDefault,
                           HugeRegionUsageOption::kUseForAllLargeAllocs}),
      AnyPositiveDuration());
}

fuzztest::Domain<Instruction> GetInstructionDomain(int depth);

fuzztest::Domain<ChangeParam> GetChangeParamDomain(int depth) {
  if (depth <= 0) {
    return fuzztest::OneOf(
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
        fuzztest::Map([](SetReleaseSucceeds s) { return ChangeParam{s}; },
                      fuzztest::Arbitrary<SetReleaseSucceeds>()),
        fuzztest::Map(
            [](SetHugeRegionDemandBasedRelease s) { return ChangeParam{s}; },
            fuzztest::Arbitrary<SetHugeRegionDemandBasedRelease>()),
        fuzztest::Map(
            [](SetUseUserspaceCollapseHeuristics s) { return ChangeParam{s}; },
            fuzztest::Arbitrary<SetUseUserspaceCollapseHeuristics>()),
        fuzztest::Map([](SetBackAllocations s) { return ChangeParam{s}; },
                      fuzztest::Arbitrary<SetBackAllocations>()),
        fuzztest::Map(
            [](SetBackSizeThresholdBytes s) { return ChangeParam{s}; },
            fuzztest::Arbitrary<SetBackSizeThresholdBytes>()),
        fuzztest::Map(
            [](std::vector<Instruction> v) {
              return ChangeParam{ReentrantSubprogram{v}};
            },
            fuzztest::VectorOf(
                fuzztest::Just(Instruction{Alloc{1, 1, 1, false, false}}))
                .WithSize(0)));
  } else {
    return fuzztest::OneOf(
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
        fuzztest::Map([](SetReleaseSucceeds s) { return ChangeParam{s}; },
                      fuzztest::Arbitrary<SetReleaseSucceeds>()),
        fuzztest::Map(
            [](SetHugeRegionDemandBasedRelease s) { return ChangeParam{s}; },
            fuzztest::Arbitrary<SetHugeRegionDemandBasedRelease>()),
        fuzztest::Map(
            [](SetUseUserspaceCollapseHeuristics s) { return ChangeParam{s}; },
            fuzztest::Arbitrary<SetUseUserspaceCollapseHeuristics>()),
        fuzztest::Map([](SetBackAllocations s) { return ChangeParam{s}; },
                      fuzztest::Arbitrary<SetBackAllocations>()),
        fuzztest::Map(
            [](SetBackSizeThresholdBytes s) { return ChangeParam{s}; },
            fuzztest::Arbitrary<SetBackSizeThresholdBytes>()),
        fuzztest::Map(
            [](std::vector<Instruction> v) {
              return ChangeParam{ReentrantSubprogram{v}};
            },
            fuzztest::VectorOf(GetInstructionDomain(depth - 1))));
  }
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
      fuzztest::Map([](GatherAndCheckStats g) { return Instruction{g}; },
                    fuzztest::Arbitrary<GatherAndCheckStats>()),
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
  options.huge_cache_time_ns = absl::ToInt64Nanoseconds(absl::Seconds(44));

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
  options.huge_cache_time_ns = absl::ToInt64Nanoseconds(absl::Seconds(246));

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

TEST(HugePageAwareAllocatorTest, b471822138) {
  FuzzHPAA(
      FuzzHugePageAwareAllocatorOptions{
          .tag = MemoryTag::kNormalP0,
          .use_huge_region_more_often = HugeRegionUsageOption::kDefault,
          .huge_cache_time_ns = 3600000000000},
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
              HugeRegionUsageOption::kUseForAllLargeAllocs,
          .huge_cache_time_ns = 3600000000000},
      {Instruction{.instr = GatherStatsPbtxt{}},
       Instruction{.instr = PrintStats{.everything = false}},
       Instruction{
           .instr = ChangeParam{.op =
                                    SetFillerSkipSubreleaseLongInterval{
                                        .duration_ns = 7795569869804108969}}},
       Instruction{.instr = ReleasePages{.desired = 9223372036854775807,
                                         .release_memory_to_system = false}}});
}

}  // namespace
}  // namespace tcmalloc::tcmalloc_internal
