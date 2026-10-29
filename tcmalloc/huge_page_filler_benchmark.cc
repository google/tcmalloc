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

// Benchmarks the HugePageFiller treatment pass, in particular the tracker
// selection it performs under pageheap_lock, against heaps of up to tens of
// thousands of trackers.  Pageflags and residency are faked so that the pass
// is not dominated by syscalls.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <numeric>
#include <optional>
#include <random>
#include <utility>
#include <vector>

#include "absl/base/attributes.h"
#include "absl/strings/string_view.h"
#include "absl/time/time.h"
#include "benchmark/benchmark.h"
#include "tcmalloc/common.h"
#include "tcmalloc/huge_page_filler.h"
#include "tcmalloc/huge_page_options.h"
#include "tcmalloc/huge_page_subrelease.h"
#include "tcmalloc/huge_page_tracker.h"
#include "tcmalloc/huge_page_treatment.h"
#include "tcmalloc/huge_pages.h"
#include "tcmalloc/internal/clock.h"
#include "tcmalloc/internal/config.h"
#include "tcmalloc/internal/logging.h"
#include "tcmalloc/internal/memory_tag.h"
#include "tcmalloc/internal/pageflags.h"
#include "tcmalloc/internal/range_tracker.h"
#include "tcmalloc/internal/residency.h"
#include "tcmalloc/pages.h"
#include "tcmalloc/span.h"

GOOGLE_MALLOC_SECTION_BEGIN
namespace tcmalloc {
namespace tcmalloc_internal {
namespace {

// Reports every hugepage as hugepage backed with nothing stale.
class BackedPageFlags final : public PageFlagsBase {
 public:
  std::optional<PageStats> Get(const void* addr, size_t size) override {
    return std::nullopt;
  }
  PageFlagsBitmaps GetSinglePageBitmaps(const void* addr) override {
    return {};
  }
  std::optional<bool> IsHugepageBacked(const void* addr) override {
    return true;
  }
};

class EmptyResidency final : public Residency {
 public:
  std::optional<Info> Get(const void* addr, size_t size) override {
    return std::nullopt;
  }
  SinglePageBitmaps GetUnbackedAndSwappedBitmaps(const void* addr) override {
    return {};
  }
  size_t GetHardwarePagesInHugePage() const override {
    return kHugePageSize / kPageSize;
  }
};

class NoopModify final : public MemoryModifyFunction {
 public:
  [[nodiscard]] MemoryModifyStatus operator()(Range r) override {
    return {.success = true, .error_number = 0};
  }
};

class NoopTag final : public MemoryTagFunction {
 public:
  void operator()(Range r, std::optional<absl::string_view> name) override {}
};

// A clock the benchmark controls, so that every pass sees the tracker ages it
// set up rather than the wall clock.
class BenchClock {
 public:
  static int64_t now() { return kNow; }
  static double freq() { return kFreq; }

 private:
  static constexpr double kFreq = 1e9;
  static constexpr int64_t kNow = int64_t{1} << 50;
};

// A filler holding `n` trackers, each with one sparse page allocated so that
// all of them sit on the regular sparse lists the treatment pass walks.
class FillerHeap {
 public:
  explicit FillerHeap(size_t n)
      : filler_(Clock{.now = BenchClock::now, .freq = BenchClock::freq},
                MemoryTag::kNormal, unback_, collapse_, set_anon_vma_name_,
                SubreleaseUnbackedMode::kDisabled) {
    trackers_.reserve(n);
    for (size_t i = 0; i < n; ++i) {
      // Addresses are never dereferenced; they only need to be distinct
      // hugepage-aligned values.
      HugePage hp =
          HugePageContaining(reinterpret_cast<void*>((i + 1) * kHugePageSize));
      auto pt = std::make_unique<PageTracker>(hp, /*was_donated=*/false,
                                              BenchClock::now());
      {
        PageHeapSpinLockHolder l;
        (void)pt->Get(Length(1), kInfo, PreferBackedPages::kDisabled);
      }
      filler_.Contribute(pt.get(), /*donated=*/false, kInfo);
      trackers_.push_back(std::move(pt));
    }
  }

  ~FillerHeap() {
    for (auto& pt : trackers_) {
      PageHeapSpinLockHolder l;
      PageTracker* released =
          filler_.Put(pt.get(), Range(pt->location().first_page(), Length(1)),
                      kInfo, FillerAppendOnFree::kDisabled);
      TC_CHECK_EQ(released, pt.get());
    }
    TC_CHECK_EQ(filler_.size(), NHugePages(0));
  }

  size_t size() const { return trackers_.size(); }

  // Records every tracker as hugepage backed `age(i)` before now, where i is
  // the tracker's creation index.
  template <typename AgeFn>
  void RecordBacked(AgeFn age) {
    PageHeapSpinLockHolder l;
    for (size_t i = 0; i < trackers_.size(); ++i) {
      const double record_time =
          BenchClock::now() -
          absl::ToDoubleSeconds(age(i)) * BenchClock::freq();
      trackers_[i]->SetHugePageResidencyState({.record_time = record_time,
                                               .maybe_hugepage_backed = true,
                                               .entry_valid = true});
    }
  }

  // Forgets every record, as for trackers the treatment has never observed.
  void ClearRecords() {
    PageHeapSpinLockHolder l;
    for (auto& pt : trackers_) {
      pt->SetHugePageResidencyState({});
    }
  }

  void Treat() ABSL_NO_THREAD_SAFETY_ANALYSIS {
    pageheap_lock.lock();
    filler_.TreatHugepageTrackers(
        EnableCollapse::kDisabled, EnableUnfilteredCollapse::kDisabled,
        ReleaseStalePages::kDisabled, PreserveDonatedOnCollapse::kDisabled,
        CollapseReleasedHugePages::kDisabled, &pageflags_, &residency_);
    pageheap_lock.unlock();
  }

 private:
  static constexpr SpanAllocInfo kInfo = {
      .objects_per_span = 1, .density = AccessDensityPrediction::kSparse};

  NoopModify unback_;
  NoopModify collapse_;
  NoopTag set_anon_vma_name_;
  BackedPageFlags pageflags_;
  EmptyResidency residency_;
  HugePageFiller<PageTracker> filler_;
  std::vector<std::unique_ptr<PageTracker>> trackers_;
};

// Steady state between sweeps: every tracker was recorded within
// kRecordInterval, so the pass walks the lists and selects nothing.
void BM_TreatAllFresh(benchmark::State& state) {
  FillerHeap heap(state.range(0));
  heap.RecordBacked([](size_t) { return absl::Seconds(1); });
  for (auto _ : state) {
    heap.Treat();
  }
  state.SetItemsProcessed(state.iterations() * heap.size());
}
BENCHMARK(BM_TreatAllFresh)->Range(1 << 10, 1 << 16);

// Every tracker is a collapse candidate, as after a restart or when nothing
// is hugepage backed: the pass fills its kTotalTrackersToScan-entry heap and,
// with every later tracker ranking no lower than the heap's top, displaces an
// entry per tracker.  Pinning only the final selection keeps a displacement
// from touching the evicted tracker.
void BM_TreatAllCandidates(benchmark::State& state) {
  FillerHeap heap(state.range(0));
  for (auto _ : state) {
    state.PauseTiming();
    heap.ClearRecords();
    state.ResumeTiming();
    heap.Treat();
  }
  state.SetItemsProcessed(state.iterations() * heap.size());
}
BENCHMARK(BM_TreatAllCandidates)->Range(1 << 10, 1 << 16);

}  // namespace
}  // namespace tcmalloc_internal
}  // namespace tcmalloc
GOOGLE_MALLOC_SECTION_END
