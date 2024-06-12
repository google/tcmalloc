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

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <functional>
#include <memory>
#include <vector>

#include "benchmark/benchmark.h"
#include "tcmalloc/common.h"
#include "tcmalloc/guarded_page_allocator.h"
#include "tcmalloc/internal/config.h"
#include "tcmalloc/internal/logging.h"
#include "tcmalloc/internal/page_size.h"
#include "tcmalloc/malloc_extension.h"
#include "tcmalloc/pages.h"
#include "tcmalloc/static_vars.h"

GOOGLE_MALLOC_SECTION_BEGIN
namespace tcmalloc {
namespace tcmalloc_internal {
namespace {
using GuardedStatus = Profile::Sample::GuardedStatus;

constexpr size_t kMaxGpaPages = GuardedPageAllocator::kGpaMaxPages;

// Size of pages used by GuardedPageAllocator. See GuardedPageAllocator::Init().
size_t GetGpaPageSize() {
  static const size_t page_size =
      std::max(kPageSize, static_cast<size_t>(GetPageSize()));
  return page_size;
}

inline auto& GetStackTrace(size_t first_frame) {
  thread_local StackTrace* s = new StackTrace;
  s->stack[0] = reinterpret_cast<void*>(first_frame + 42);
  s->depth = 1;
  return *s;
}

std::unique_ptr<GuardedPageAllocator, void (*)(GuardedPageAllocator*)>
GetGuardedPageAllocator() {
  static GuardedPageAllocator* gpa = [] {
    auto gpa = new GuardedPageAllocator;
    PageHeapSpinLockHolder l;
    gpa->Init(kMaxGpaPages, kMaxGpaPages);
    gpa->AllowAllocations();
    // Benchmark should always sample.
    MallocExtension::SetProfileSamplingInterval(1);
    MallocExtension::SetGuardedSamplingInterval(1);
    return gpa;
  }();
  return {gpa, +[](GuardedPageAllocator* gpa) {
            // We can't reset GuardedPageAllocator before the benchmark in
            // multi-threaded mode as it might race with pre-initialization and
            // concurrent Reset() from all other benchmark threads. Instead,
            // just reset after each benchmark.
            gpa->Reset();
          }};
}

// Benchmark for guarded page allocation overhead only, to focus on free slot
// selection, mprotect() overhead, and allocation metadata storage.
void BM_AllocDealloc(benchmark::State& state) {
  const size_t alloc_size = state.range(0);
  auto gpa = GetGuardedPageAllocator();
  for (auto _ : state) {
    char* ptr = reinterpret_cast<char*>(
        gpa->Allocate(alloc_size, 0, GetStackTrace(0)).alloc);
    TC_CHECK_NE(ptr, nullptr);
    ptr[0] = 'X';               // Page fault first page.
    ptr[alloc_size - 1] = 'X';  // Page fault last page.
    gpa->Deallocate(ptr);
  }
}

BENCHMARK(BM_AllocDealloc)->Range(1, GetGpaPageSize());
BENCHMARK(BM_AllocDealloc)->Arg(1)->ThreadRange(1, kMaxGpaPages);

auto& GetReserved() {
  static auto* ret =
      new std::vector<std::unique_ptr<void, std::function<void(void*)>>>;
  return *ret;
}

// Exhaust the pool first so we do not profile allocation overhead.
void ReservePool(const benchmark::State&) {
  TC_CHECK(GetReserved().empty());
  auto* gpa = GetGuardedPageAllocator().release();  // do not Reset()
  auto deleter = [gpa](void* p) { gpa->Deallocate(p); };

  for (size_t stack_idx = 0;;) {
    auto alloc = gpa->TrySample(1, 0, Length(1), GetStackTrace(stack_idx));
    switch (alloc.status) {
      case GuardedStatus::NoAvailableSlots:
        TC_CHECK(!GetReserved().empty());
        return;
      case GuardedStatus::RateLimited:
        // Emulate that non-guarded sampling happened.
        tc_globals.total_sampled_count_.Add(1);
        break;
      case GuardedStatus::Filtered:
        // The filter is rejecting the stack trace, give it one more unique
        // stack trace.
        stack_idx++;
        break;
      default:
        if (alloc.alloc) GetReserved().emplace_back(alloc.alloc, deleter);
        break;
    }
  }
}

void ReleasePool(const benchmark::State&) {
  TC_CHECK(!GetReserved().empty());
  GetReserved().clear();
}

// Benchmark that includes sampling-decision overhead.
void BM_TrySample(benchmark::State& state) {
  TC_CHECK(!GetReserved().empty());
  // Shared between benchmark threads. We don't care if one of the threads
  // doesn't see one of the statuses.
  static std::atomic<bool> seen_filtered;
  seen_filtered = false;

  const size_t alloc_size = state.range(0);
  auto gpa = GetGuardedPageAllocator();
  size_t stack_idx = 0;

  for (auto _ : state) {
    StackTrace& stack_trace = GetStackTrace(stack_idx++ % GetReserved().size());
    auto alloc = gpa->TrySample(alloc_size, 0, Length(1), stack_trace);

    switch (alloc.status) {
      case GuardedStatus::RateLimited:
        tc_globals.total_sampled_count_.Add(1);
        break;
      case GuardedStatus::Filtered:
        seen_filtered.store(true, std::memory_order_relaxed);
        break;
      default:
        TC_CHECK_NE(alloc.status, GuardedStatus::Guarded);
        TC_CHECK_NE(alloc.status, GuardedStatus::LargerThanOnePage);
        TC_CHECK_NE(alloc.status, GuardedStatus::Disabled);
        TC_CHECK_NE(alloc.status, GuardedStatus::MProtectFailed);
        TC_CHECK_NE(alloc.status, GuardedStatus::TooSmall);
        break;
    }
    TC_CHECK_EQ(alloc.alloc, nullptr);
  }

  if (state.iterations() > 1000) TC_CHECK(seen_filtered);
}

BENCHMARK(BM_TrySample)
    ->Range(1, GetGpaPageSize())
    ->Setup(ReservePool)
    ->Teardown(ReleasePool);
BENCHMARK(BM_TrySample)
    ->Arg(1)
    ->ThreadRange(1, kMaxGpaPages)
    ->Setup(ReservePool)
    ->Teardown(ReleasePool);

}  // namespace
}  // namespace tcmalloc_internal
}  // namespace tcmalloc
GOOGLE_MALLOC_SECTION_END
