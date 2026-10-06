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

#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "absl/base/attributes.h"
#include "absl/base/const_init.h"
#include "absl/flags/flag.h"
#include "absl/random/random.h"
#include "absl/synchronization/mutex.h"
#include "benchmark/benchmark.h"
#include "tcmalloc/common.h"
#include "tcmalloc/huge_page_aware_allocator.h"
#include "tcmalloc/huge_pages.h"
#include "tcmalloc/internal/allocation_guard.h"
#include "tcmalloc/internal/config.h"
#include "tcmalloc/internal/logging.h"
#include "tcmalloc/internal/page_size.h"
#include "tcmalloc/internal/sysinfo.h"
#include "tcmalloc/pages.h"
#include "tcmalloc/span.h"
#include "tcmalloc/stats.h"

ABSL_FLAG(uint64_t, growth_bytes, 1024 * 1024 * 1024,
          "total size of growth for grow/shrink benchmark");

ABSL_FLAG(double, shrink_factor, 25,
          "ratio of peak to valley in growth benchmark");

GOOGLE_MALLOC_SECTION_BEGIN
namespace tcmalloc {
namespace tcmalloc_internal {
namespace {

int64_t pagesize = GetPageSize();

void Touch(PageId p) {
  // a tcmalloc-page may contain more than an actual kernel page
  volatile char* base = reinterpret_cast<char*>(p.start_addr());
  static size_t kActualPages = std::max<size_t>(kPageSize / pagesize, 1);
  for (int i = 0; i < kActualPages; ++i) {
    base[i * pagesize] = 1;
  }
}

void Touch(Span* s) {
  for (PageId p = s->first_page(); p <= s->last_page(); ++p) {
    Touch(p);
  }
}

class BenchmarkAllocator {
 public:
  char buf_[sizeof(HugePageAwareAllocator)];
  HugePageAwareAllocator* alloc_;
  Span* prime_;
  static constexpr size_t kObjectsPerSpan = 1;
  BenchmarkAllocator() = default;
  ~BenchmarkAllocator() = default;

  void Reset() {
    alloc_ = new (buf_) HugePageAwareAllocator(
        huge_page_allocator_internal::HugePageAwareAllocatorOptions{
            MemoryTag::kNormal});
    prime_ = New(Length(1));
  }

  void Done() { Delete(prime_); }

  Span* New(Length n) {
    return alloc_->New(n, {kObjectsPerSpan, AccessDensityPrediction::kSparse});
  }

  void Delete(Span* s) {
    PageAllocatorInterface::AllocationState a{
        Range(s->first_page(), s->num_pages()),
        s->donated(),
    };
    alloc_->forwarder().DeleteSpan(s);
    PageHeapSpinLockHolder l;
    alloc_->Delete(a, {.objects_per_span = kObjectsPerSpan,
                       .density = AccessDensityPrediction::kSparse});
  }

  BackingStats stats() const {
    PageHeapSpinLockHolder l;
    return alloc_->stats();
  }

  // Release until either free/used <= frac or we stop making progress.
  void ReleaseToFrac(double frac) {
    PageHeapSpinLockHolder l;
    while (true) {
      BackingStats stats = alloc_->stats();
      const size_t used =
          stats.system_bytes - stats.free_bytes - stats.unmapped_bytes;
      const double free = stats.free_bytes;
      if (free / used <= frac) break;
      if (Length(0) ==
          alloc_->ReleaseAtLeastNPages(
              kMaxPages, /*reason=*/PageReleaseReason::kReleaseMemoryToSystem))
        break;
    }
  }
};

BenchmarkAllocator ba;

static void BM_AllocFree(benchmark::State& state) {
  const Length len(state.range(0));
  ba.Reset();
  for (auto s : state) {
    Span* alloc = ba.New(len);
    benchmark::DoNotOptimize(alloc);
    ba.Delete(alloc);
  }
  state.SetItemsProcessed(state.iterations());
  state.SetBytesProcessed(len.in_bytes() * state.iterations());

  ba.Done();
}

BENCHMARK(BM_AllocFree)
    ->Range(1, 1024)
    ->Arg((kPagesPerHugePage / 2).raw_num())
    ->Arg((kPagesPerHugePage / 2 + Length(1)).raw_num())
    ->Arg((kPagesPerHugePage).raw_num())
    ->Arg((kPagesPerHugePage - Length(1)).raw_num());

static void BM_AllocLoaded(benchmark::State& state) {
  const int nspans = state.range(0);
  ba.Reset();
  std::vector<Span*> spans;
  absl::BitGen rng;
  for (int i = 0; i < nspans; ++i) {
    auto len = Length(absl::LogUniform<int32_t>(rng, 0, (1 << 9) - 1) + 1);
    spans.push_back(ba.New(len));
  }

  Length total;
  for (auto s : state) {
    auto len = Length(absl::LogUniform<int32_t>(rng, 0, (1 << 9) - 1) + 1);
    total += len;
    size_t index = absl::Uniform<int32_t>(rng, 0, spans.size());
    ba.Delete(spans[index]);
    spans[index] = ba.New(len);
  }
  for (auto s : spans) {
    ba.Delete(s);
  }
  ba.Done();
  state.SetItemsProcessed(state.iterations());
  state.SetBytesProcessed(total.in_bytes());
}

BENCHMARK(BM_AllocLoaded)->Range(100, 6400);

static void BM_Growth(benchmark::State& state) {
  ABSL_CONST_INIT static absl::Mutex m(absl::kConstInit);
  absl::InsecureBitGen gen;
  std::vector<Span*> spans;
  Length lifetime_alloc;
  Length total;

  const Length target =
      LengthFromBytes(absl::GetFlag(FLAGS_growth_bytes)) / state.threads();
  const Length min = target / absl::GetFlag(FLAGS_shrink_factor);

  if (state.thread_index() == 0) {
    ba.Reset();
  }

  for (auto _ : state) {
    while (total < target) {
      auto k = Length(absl::LogUniform<uint64_t>(gen, 1, 256));

      total += k;
      lifetime_alloc += k;
      Span* s;
      {
        // Why this lock? Because otherwise we just contend on the spinlock,
        // which is less scalable.
        absl::MutexLock l(m);
        s = ba.New(k);
      }
      TC_CHECK_EQ(k, s->num_pages());

      spans.push_back(s);
      // Ensure we pay for page faults.
      Touch(s);
    }
    // Synchronize to all thread's peak
    state.PauseTiming();

    // and make random choices while we're not counting anyway
    std::shuffle(spans.begin(), spans.end(), gen);

    state.ResumeTiming();
    while (total >= min) {
      Span* s = spans.back();
      spans.pop_back();
      total -= s->num_pages();
      {
        absl::MutexLock l(m);
        ba.Delete(s);
      }
    }
    state.PauseTiming();
    if (state.thread_index() == 0) {
      ba.ReleaseToFrac(0.01);
    }
    state.ResumeTiming();
  }

  for (auto s : spans) {
    ba.Delete(s);
  }

  if (state.thread_index() == 0) {
    ba.Done();
  }

  state.SetBytesProcessed(lifetime_alloc.in_bytes());
}

BENCHMARK(BM_Growth)->ThreadRange(1, NumCPUs());

static void BM_GetStats(benchmark::State& state) {
  if (state.thread_index() == 0) {
    ba.Reset();
  }
  for (auto _ : state) {
    benchmark::DoNotOptimize(ba.stats());
  }
}
BENCHMARK(BM_GetStats)->Threads(1)->ThreadPerCpu();

}  // namespace
}  // namespace tcmalloc_internal
}  // namespace tcmalloc
GOOGLE_MALLOC_SECTION_END
