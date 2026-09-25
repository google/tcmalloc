// Copyright 2019 The TCMalloc Authors
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

#include <stdint.h>

#include <algorithm>
#include <cstddef>
#include <limits>
#include <map>
#include <new>
#include <optional>
#include <string>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

#include "gtest/gtest.h"
#include "fuzztest/fuzztest.h"
#include "absl/strings/match.h"
#include "absl/strings/str_format.h"
#include "absl/strings/string_view.h"
#include "absl/time/time.h"
#include "tcmalloc/internal/config.h"
#include "tcmalloc/internal/logging.h"
#include "tcmalloc/internal/sysinfo.h"
#include "tcmalloc/malloc_extension.h"
#include "tcmalloc/testing/testutil.h"

namespace tcmalloc {
namespace {

using tcmalloc_internal::kSanitizerPresent;

void FuzzGetProperty(absl::string_view property) {
  std::optional<size_t> val = MallocExtension::GetNumericProperty(property);
  if (!val.has_value()) {
    // Rather than inspect the result of MallocExtension::GetProperties, we
    // defer to the test in
    // //tcmalloc/testing/malloc_extension_test.cc to ensure that
    // every key in GetProperties has a value returned by GetNumericProperty.
    return;
  }

  MallocExtension::PropertyMap properties = MallocExtension::GetProperties();
  if (properties.find(std::string(property)) == properties.end()) {
    __builtin_trap();
  }
}

FUZZ_TEST(MallocExtensionTest, FuzzGetProperty)
    .WithDomains(fuzztest::String())
    ;

TEST(MallocExtensionTest, FuzzGetPropertyRegression) { FuzzGetProperty(""); }

// Control-plane fuzzer: drives the MallocExtension setters, getters, release
// entry points, and stats dumps against the linked allocator while a bounded
// live set keeps the heap non-trivial.
//
// Under sanitizers the weak MallocExtension_Internal_* hooks are absent, so the
// setters are no-ops and the getters return their documented defaults.  Every
// call is still made in that configuration; only the round-trip assertions are
// gated on kSanitizerPresent.

// Live allocations are capped so that a run cannot exhaust memory and so that
// the hard heap limits below always exceed the backed heap.
inline constexpr size_t kMaxLiveAllocations = 1024;
inline constexpr size_t kMaxAllocationSize = 1 << 20;
inline constexpr size_t kMinHardLimit = size_t{8} << 30;
inline constexpr size_t kMaxLimit = size_t{64} << 30;

struct State;

struct Alloc {
  size_t size;
  int alignment_log2;

  template <typename Sink>
  friend void AbslStringify(Sink& sink, const Alloc& a) {
    absl::Format(&sink, "Alloc{.size=%v, .alignment_log2=%v}", a.size,
                 a.alignment_log2);
  }

  void Perform(State& state) const;
};

struct Free {
  size_t index;

  template <typename Sink>
  friend void AbslStringify(Sink& sink, const Free& f) {
    absl::Format(&sink, "Free{.index=%v}", f.index);
  }

  void Perform(State& state) const;
};

struct SetProfileSamplingInterval {
  int64_t interval;

  template <typename Sink>
  friend void AbslStringify(Sink& sink, const SetProfileSamplingInterval& s) {
    absl::Format(&sink, "SetProfileSamplingInterval{.interval=%v}", s.interval);
  }

  void Perform(State& state) const;
};

struct SetGuardedSamplingInterval {
  int64_t interval;

  template <typename Sink>
  friend void AbslStringify(Sink& sink, const SetGuardedSamplingInterval& s) {
    absl::Format(&sink, "SetGuardedSamplingInterval{.interval=%v}", s.interval);
  }

  void Perform(State& state) const;
};

struct SetMaxPerCpuCacheSize {
  int32_t bytes;

  template <typename Sink>
  friend void AbslStringify(Sink& sink, const SetMaxPerCpuCacheSize& s) {
    absl::Format(&sink, "SetMaxPerCpuCacheSize{.bytes=%v}", s.bytes);
  }

  void Perform(State& state) const;
};

struct SetMaxTotalThreadCacheBytes {
  int64_t bytes;

  template <typename Sink>
  friend void AbslStringify(Sink& sink, const SetMaxTotalThreadCacheBytes& s) {
    absl::Format(&sink, "SetMaxTotalThreadCacheBytes{.bytes=%v}", s.bytes);
  }

  void Perform(State& state) const;
};

struct SetBackgroundProcessActionsEnabled {
  bool enabled;

  template <typename Sink>
  friend void AbslStringify(Sink& sink,
                            const SetBackgroundProcessActionsEnabled& s) {
    absl::Format(&sink, "SetBackgroundProcessActionsEnabled{.enabled=%v}",
                 s.enabled);
  }

  void Perform(State& state) const;
};

struct SetBackgroundProcessSleepInterval {
  int64_t ms;

  template <typename Sink>
  friend void AbslStringify(Sink& sink,
                            const SetBackgroundProcessSleepInterval& s) {
    absl::Format(&sink, "SetBackgroundProcessSleepInterval{.ms=%v}", s.ms);
  }

  void Perform(State& state) const;
};

struct SetSkipSubreleaseShortInterval {
  int64_t ms;

  template <typename Sink>
  friend void AbslStringify(Sink& sink,
                            const SetSkipSubreleaseShortInterval& s) {
    absl::Format(&sink, "SetSkipSubreleaseShortInterval{.ms=%v}", s.ms);
  }

  void Perform(State& state) const;
};

struct SetSkipSubreleaseLongInterval {
  int64_t ms;

  template <typename Sink>
  friend void AbslStringify(Sink& sink,
                            const SetSkipSubreleaseLongInterval& s) {
    absl::Format(&sink, "SetSkipSubreleaseLongInterval{.ms=%v}", s.ms);
  }

  void Perform(State& state) const;
};

struct SetBackgroundReleaseRate {
  size_t bytes_per_second;

  template <typename Sink>
  friend void AbslStringify(Sink& sink, const SetBackgroundReleaseRate& s) {
    absl::Format(&sink, "SetBackgroundReleaseRate{.bytes_per_second=%v}",
                 s.bytes_per_second);
  }

  void Perform(State& state) const;
};

struct SetSoftLimit {
  size_t limit;

  template <typename Sink>
  friend void AbslStringify(Sink& sink, const SetSoftLimit& s) {
    absl::Format(&sink, "SetSoftLimit{.limit=%v}", s.limit);
  }

  void Perform(State& state) const;
};

// A hard limit below the backed heap aborts the process
// (PageAllocator::ShrinkToUsageLimitSlow), so the domain keeps hard limits at
// or above kMinHardLimit, far above what kMaxLiveAllocations objects of
// kMaxAllocationSize bytes can back.  Zero clears the limit.
struct SetHardLimit {
  size_t limit;

  template <typename Sink>
  friend void AbslStringify(Sink& sink, const SetHardLimit& s) {
    absl::Format(&sink, "SetHardLimit{.limit=%v}", s.limit);
  }

  void Perform(State& state) const;
};

struct ReleaseMemoryToSystem {
  size_t num_bytes;

  template <typename Sink>
  friend void AbslStringify(Sink& sink, const ReleaseMemoryToSystem& r) {
    absl::Format(&sink, "ReleaseMemoryToSystem{.num_bytes=%v}", r.num_bytes);
  }

  void Perform(State& state) const;
};

struct ReleaseCpuMemory {
  int cpu;

  template <typename Sink>
  friend void AbslStringify(Sink& sink, const ReleaseCpuMemory& r) {
    absl::Format(&sink, "ReleaseCpuMemory{.cpu=%v}", r.cpu);
  }

  void Perform(State& state) const;
};

struct MarkThreadIdle {
  template <typename Sink>
  friend void AbslStringify(Sink& sink, const MarkThreadIdle&) {
    absl::Format(&sink, "MarkThreadIdle{}");
  }

  void Perform(State& state) const;
};

struct MarkThreadBusy {
  template <typename Sink>
  friend void AbslStringify(Sink& sink, const MarkThreadBusy&) {
    absl::Format(&sink, "MarkThreadBusy{}");
  }

  void Perform(State& state) const;
};

struct CheckOwnership {
  size_t index;

  template <typename Sink>
  friend void AbslStringify(Sink& sink, const CheckOwnership& c) {
    absl::Format(&sink, "CheckOwnership{.index=%v}", c.index);
  }

  void Perform(State& state) const;
};

struct Stats {
  template <typename Sink>
  friend void AbslStringify(Sink& sink, const Stats&) {
    absl::Format(&sink, "Stats{}");
  }

  void Perform(State& state) const;
};

struct Pbtxt {
  template <typename Sink>
  friend void AbslStringify(Sink& sink, const Pbtxt&) {
    absl::Format(&sink, "Pbtxt{}");
  }

  void Perform(State& state) const;
};

using Instruction = std::variant<
    Alloc, Free, SetProfileSamplingInterval, SetGuardedSamplingInterval,
    SetMaxPerCpuCacheSize, SetMaxTotalThreadCacheBytes,
    SetBackgroundProcessActionsEnabled, SetBackgroundProcessSleepInterval,
    SetSkipSubreleaseShortInterval, SetSkipSubreleaseLongInterval,
    SetBackgroundReleaseRate, SetSoftLimit, SetHardLimit, ReleaseMemoryToSystem,
    ReleaseCpuMemory, MarkThreadIdle, MarkThreadBusy, CheckOwnership, Stats,
    Pbtxt>;

template <typename Sink>
void AbslStringify(Sink& sink, const Instruction& instruction) {
  std::visit([&](const auto& i) { absl::Format(&sink, "%v", i); }, instruction);
}

struct Allocation {
  void* ptr;
  size_t size;
  std::optional<std::align_val_t> alignment;
};

// Captures every tunable at construction and restores it at destruction so
// runs do not leak configuration into one another or into FuzzGetProperty.
struct State {
  State()
      : profile_sampling_interval(
            MallocExtension::GetProfileSamplingInterval()),
        guarded_sampling_interval(
            MallocExtension::GetGuardedSamplingInterval()),
        max_per_cpu_cache_size(MallocExtension::GetMaxPerCpuCacheSize()),
        max_total_thread_cache_bytes(
            MallocExtension::GetMaxTotalThreadCacheBytes()),
        background_process_actions_enabled(
            MallocExtension::GetBackgroundProcessActionsEnabled()),
        background_process_sleep_interval(
            MallocExtension::GetBackgroundProcessSleepInterval()),
        skip_subrelease_short_interval(
            MallocExtension::GetSkipSubreleaseShortInterval()),
        skip_subrelease_long_interval(
            MallocExtension::GetSkipSubreleaseLongInterval()),
        background_release_rate(MallocExtension::GetBackgroundReleaseRate()),
        soft_limit(
            MallocExtension::GetMemoryLimit(MallocExtension::LimitKind::kSoft)),
        hard_limit(
            MallocExtension::GetMemoryLimit(MallocExtension::LimitKind::kHard)),
        released_to_system(ReleasedToSystem()) {
    live.reserve(kMaxLiveAllocations);
  }

  ~State() {
    for (const Allocation& a : live) {
      Deallocate(a);
    }

    // Setting the hard limit clamps the soft limit, so restore hard first.
    MallocExtension::SetMemoryLimit(hard_limit,
                                    MallocExtension::LimitKind::kHard);
    MallocExtension::SetMemoryLimit(soft_limit,
                                    MallocExtension::LimitKind::kSoft);
    MallocExtension::SetBackgroundReleaseRate(background_release_rate);
    MallocExtension::SetSkipSubreleaseLongInterval(
        skip_subrelease_long_interval);
    MallocExtension::SetSkipSubreleaseShortInterval(
        skip_subrelease_short_interval);
    MallocExtension::SetBackgroundProcessSleepInterval(
        background_process_sleep_interval);
    MallocExtension::SetBackgroundProcessActionsEnabled(
        background_process_actions_enabled);
    MallocExtension::SetMaxTotalThreadCacheBytes(max_total_thread_cache_bytes);
    MallocExtension::SetMaxPerCpuCacheSize(max_per_cpu_cache_size);
    // Setting the profile interval rescales the guarded interval, so restore
    // profile first.
    MallocExtension::SetProfileSamplingInterval(profile_sampling_interval);
    MallocExtension::SetGuardedSamplingInterval(guarded_sampling_interval);
  }

  static void Deallocate(const Allocation& a) {
    if (a.alignment.has_value()) {
      ::operator delete(a.ptr, a.size, *a.alignment);
      return;
    }
    ::operator delete(a.ptr, a.size);
  }

  static size_t ReleasedToSystem() {
    return MallocExtension::GetNumericProperty(
               "tcmalloc.num_released_release_memory_to_system_bytes")
        .value_or(0);
  }

  void RunInstructions(const std::vector<Instruction>& instructions) {
    for (const Instruction& instruction : instructions) {
      std::visit([&](const auto& i) { i.Perform(*this); }, instruction);
    }
  }

  const int64_t profile_sampling_interval;
  const int64_t guarded_sampling_interval;
  const int32_t max_per_cpu_cache_size;
  const int64_t max_total_thread_cache_bytes;
  const bool background_process_actions_enabled;
  const absl::Duration background_process_sleep_interval;
  const absl::Duration skip_subrelease_short_interval;
  const absl::Duration skip_subrelease_long_interval;
  const MallocExtension::BytesPerSecond background_release_rate;
  const size_t soft_limit;
  const size_t hard_limit;

  size_t released_to_system;
  std::vector<Allocation> live;
};

void Alloc::Perform(State& state) const {
  if (state.live.size() >= kMaxLiveAllocations) {
    return;
  }

  Allocation a;
  a.size = std::clamp<size_t>(size, 1, kMaxAllocationSize);
  const size_t alignment = size_t{1} << alignment_log2;
  if (alignment > __STDCPP_DEFAULT_NEW_ALIGNMENT__) {
    a.alignment = static_cast<std::align_val_t>(alignment);
    a.ptr = ::operator new(a.size, *a.alignment);
    TC_CHECK_EQ(reinterpret_cast<uintptr_t>(a.ptr) % alignment, 0);
  } else {
    a.ptr = ::operator new(a.size);
  }
  // Touch the allocation so residency-based stats see it.
  *static_cast<char*>(a.ptr) = 1;

  TC_CHECK_GE(MallocExtension::GetEstimatedAllocatedSize(a.size), a.size);
  const std::optional<size_t> allocated =
      MallocExtension::GetAllocatedSize(a.ptr);
  TC_CHECK(allocated.has_value());
  TC_CHECK_GE(*allocated, a.size);
  state.live.push_back(a);
}

void Free::Perform(State& state) const {
  if (state.live.empty()) {
    return;
  }

  const size_t i = index % state.live.size();
  const Allocation a = state.live[i];
  state.live[i] = state.live.back();
  state.live.pop_back();
  State::Deallocate(a);
  // Ownership of a freed pointer depends on whether its span is still live, so
  // only exercise the lookup.
  (void)MallocExtension::GetOwnership(a.ptr);
}

void SetProfileSamplingInterval::Perform(State& state) const {
  MallocExtension::SetProfileSamplingInterval(interval);
  if (kSanitizerPresent) return;
  TC_CHECK_EQ(MallocExtension::GetProfileSamplingInterval(), interval);
}

void SetGuardedSamplingInterval::Perform(State& state) const {
  MallocExtension::SetGuardedSamplingInterval(interval);
  if (kSanitizerPresent) return;
  TC_CHECK_EQ(MallocExtension::GetGuardedSamplingInterval(), interval);
}

void SetMaxPerCpuCacheSize::Perform(State& state) const {
  MallocExtension::SetMaxPerCpuCacheSize(bytes);
  if (kSanitizerPresent) return;
  TC_CHECK_EQ(MallocExtension::GetMaxPerCpuCacheSize(), bytes);
}

void SetMaxTotalThreadCacheBytes::Perform(State& state) const {
  MallocExtension::SetMaxTotalThreadCacheBytes(bytes);
  if (kSanitizerPresent) return;
  TC_CHECK_EQ(MallocExtension::GetMaxTotalThreadCacheBytes(), bytes);
}

void SetBackgroundProcessActionsEnabled::Perform(State& state) const {
  MallocExtension::SetBackgroundProcessActionsEnabled(enabled);
  if (kSanitizerPresent) return;
  TC_CHECK_EQ(MallocExtension::GetBackgroundProcessActionsEnabled(), enabled);
}

void SetBackgroundProcessSleepInterval::Perform(State& state) const {
  const absl::Duration d = absl::Milliseconds(ms);
  MallocExtension::SetBackgroundProcessSleepInterval(d);
  if (kSanitizerPresent) return;
  TC_CHECK_EQ(MallocExtension::GetBackgroundProcessSleepInterval(), d);
}

void SetSkipSubreleaseShortInterval::Perform(State& state) const {
  const absl::Duration d = absl::Milliseconds(ms);
  MallocExtension::SetSkipSubreleaseShortInterval(d);
  if (kSanitizerPresent) return;
  TC_CHECK_EQ(MallocExtension::GetSkipSubreleaseShortInterval(), d);
}

void SetSkipSubreleaseLongInterval::Perform(State& state) const {
  const absl::Duration d = absl::Milliseconds(ms);
  MallocExtension::SetSkipSubreleaseLongInterval(d);
  if (kSanitizerPresent) return;
  TC_CHECK_EQ(MallocExtension::GetSkipSubreleaseLongInterval(), d);
}

void SetBackgroundReleaseRate::Perform(State& state) const {
  const auto rate =
      static_cast<MallocExtension::BytesPerSecond>(bytes_per_second);
  MallocExtension::SetBackgroundReleaseRate(rate);
  if (kSanitizerPresent) return;
  TC_CHECK_EQ(static_cast<size_t>(MallocExtension::GetBackgroundReleaseRate()),
              bytes_per_second);
}

// MallocExtension::SetMemoryLimit maps 0 to "no limit".
size_t EffectiveLimit(size_t limit) {
  return limit == 0 ? std::numeric_limits<size_t>::max() : limit;
}

void SetSoftLimit::Perform(State& state) const {
  MallocExtension::SetMemoryLimit(limit, MallocExtension::LimitKind::kSoft);
  if (kSanitizerPresent) return;
  // PageAllocator::set_limit clamps the soft limit to the hard limit.
  const size_t hard =
      MallocExtension::GetMemoryLimit(MallocExtension::LimitKind::kHard);
  const size_t expected = std::min(EffectiveLimit(limit), hard);
  TC_CHECK_EQ(
      MallocExtension::GetMemoryLimit(MallocExtension::LimitKind::kSoft),
      expected);
}

void SetHardLimit::Perform(State& state) const {
  TC_CHECK(limit == 0 || limit >= kMinHardLimit);
  MallocExtension::SetMemoryLimit(limit, MallocExtension::LimitKind::kHard);
  if (kSanitizerPresent) return;
  const size_t hard =
      MallocExtension::GetMemoryLimit(MallocExtension::LimitKind::kHard);
  TC_CHECK_EQ(hard, EffectiveLimit(limit));
  TC_CHECK_LE(
      MallocExtension::GetMemoryLimit(MallocExtension::LimitKind::kSoft), hard);
}

void ReleaseMemoryToSystem::Perform(State& state) const {
  MallocExtension::ReleaseMemoryToSystem(num_bytes);
  const size_t released = State::ReleasedToSystem();
  TC_CHECK_GE(released, state.released_to_system);
  state.released_to_system = released;
}

void ReleaseCpuMemory::Perform(State& state) const {
  const int num_cpus = tcmalloc_internal::NumCPUs();
  TC_CHECK_GT(num_cpus, 0);
  (void)MallocExtension::ReleaseCpuMemory(cpu % num_cpus);
}

void MarkThreadIdle::Perform(State& state) const {
  MallocExtension::MarkThreadIdle();
}

void MarkThreadBusy::Perform(State& state) const {
  MallocExtension::MarkThreadBusy();
}

void CheckOwnership::Perform(State& state) const {
  using Ownership = MallocExtension::Ownership;
  TC_CHECK(MallocExtension::GetOwnership(nullptr) == Ownership::kNotOwned);
  int on_stack;
  TC_CHECK(MallocExtension::GetOwnership(&on_stack) == Ownership::kNotOwned);
  if (state.live.empty()) {
    return;
  }
  const Allocation& a = state.live[index % state.live.size()];
  TC_CHECK(MallocExtension::GetOwnership(a.ptr) == Ownership::kOwned);
}

void Stats::Perform(State& state) const {
  const std::string stats = MallocExtension::GetStats();
  if (!kSanitizerPresent) {
    TC_CHECK(absl::StrContains(stats, "MALLOC:"));
  }

  const MallocExtension::PropertyMap properties =
      MallocExtension::GetProperties();
  for (const auto& [key, property] : properties) {
    // GetNumericProperty does not report experiments under sanitizers.
    if (kSanitizerPresent && absl::StartsWith(key, "tcmalloc.experiment.")) {
      continue;
    }
    // The value may have moved since GetProperties sampled it, so only require
    // that the key resolves.
    TC_CHECK(MallocExtension::GetNumericProperty(key).has_value(), "%s", key);
  }

  for (const absl::string_view key :
       {"generic.current_allocated_bytes", "generic.heap_size",
        "tcmalloc.pageheap_free_bytes", "tcmalloc.per_cpu_caches_active"}) {
    TC_CHECK(properties.contains(std::string(key)), "%s", key);
    TC_CHECK(MallocExtension::GetNumericProperty(key).has_value(), "%s", key);
  }
}

void Pbtxt::Perform(State& state) const {
  const std::string pbtxt = GetStatsInPbTxt();
  if (kSanitizerPresent) {
    return;
  }
  TC_CHECK(!pbtxt.empty());
}

void FuzzControlPlane(const std::vector<Instruction>& instructions) {
  State state;
  state.RunInstructions(instructions);
}

auto MillisecondsDomain() {
  return fuzztest::InRange<int64_t>(0,
                                    absl::ToInt64Milliseconds(absl::Hours(1)));
}

auto InstructionDomain() {
  return fuzztest::VariantOf(
      fuzztest::StructOf<Alloc>(
          fuzztest::InRange<size_t>(1, kMaxAllocationSize),
          fuzztest::InRange<int>(0, 16)),
      fuzztest::Arbitrary<Free>(),
      fuzztest::StructOf<SetProfileSamplingInterval>(
          fuzztest::InRange<int64_t>(-1, int64_t{1} << 30)),
      fuzztest::StructOf<SetGuardedSamplingInterval>(
          fuzztest::InRange<int64_t>(-1, int64_t{1} << 30)),
      fuzztest::StructOf<SetMaxPerCpuCacheSize>(
          fuzztest::InRange<int32_t>(0, 64 << 20)),
      fuzztest::StructOf<SetMaxTotalThreadCacheBytes>(
          fuzztest::InRange<int64_t>(0, int64_t{1} << 30)),
      fuzztest::Arbitrary<SetBackgroundProcessActionsEnabled>(),
      fuzztest::StructOf<SetBackgroundProcessSleepInterval>(
          MillisecondsDomain()),
      fuzztest::StructOf<SetSkipSubreleaseShortInterval>(MillisecondsDomain()),
      fuzztest::StructOf<SetSkipSubreleaseLongInterval>(MillisecondsDomain()),
      fuzztest::StructOf<SetBackgroundReleaseRate>(
          fuzztest::InRange<size_t>(0, size_t{1} << 30)),
      fuzztest::StructOf<SetSoftLimit>(fuzztest::InRange<size_t>(0, kMaxLimit)),
      fuzztest::StructOf<SetHardLimit>(
          fuzztest::OneOf(fuzztest::Just<size_t>(0),
                          fuzztest::InRange<size_t>(kMinHardLimit, kMaxLimit))),
      fuzztest::Arbitrary<ReleaseMemoryToSystem>(),
      fuzztest::StructOf<ReleaseCpuMemory>(fuzztest::NonNegative<int>()),
      fuzztest::Arbitrary<MarkThreadIdle>(),
      fuzztest::Arbitrary<MarkThreadBusy>(),
      fuzztest::Arbitrary<CheckOwnership>(), fuzztest::Arbitrary<Stats>(),
      fuzztest::Arbitrary<Pbtxt>());
}

FUZZ_TEST(MallocExtensionTest, FuzzControlPlane)
    .WithDomains(fuzztest::VectorOf(InstructionDomain()).WithMaxSize(128));

TEST(MallocExtensionTest, FuzzControlPlaneRegression) {
  FuzzControlPlane({
      Alloc{.size = 1 << 20, .alignment_log2 = 0},
      Alloc{.size = 4096, .alignment_log2 = 16},
      Stats{},
      CheckOwnership{.index = 1},
      SetProfileSamplingInterval{.interval = 1},
      SetGuardedSamplingInterval{.interval = 0},
      Alloc{.size = 64, .alignment_log2 = 3},
      SetSoftLimit{.limit = 1},
      SetHardLimit{.limit = kMinHardLimit},
      SetSoftLimit{.limit = kMaxLimit},
      Alloc{.size = 1 << 20, .alignment_log2 = 12},
      Free{.index = 0},
      ReleaseMemoryToSystem{.num_bytes = 1 << 20},
      ReleaseCpuMemory{.cpu = 0},
      MarkThreadIdle{},
      MarkThreadBusy{},
      SetHardLimit{.limit = 0},
      SetSkipSubreleaseShortInterval{.ms = 0},
      SetSkipSubreleaseLongInterval{.ms = 0},
      SetBackgroundReleaseRate{.bytes_per_second = 0},
      Pbtxt{},
      Free{.index = 0},
      Free{.index = 0},
      Stats{},
  });
}

}  // namespace
}  // namespace tcmalloc
