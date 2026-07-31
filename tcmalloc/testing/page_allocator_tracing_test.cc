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

#include <stddef.h>
#include <stdint.h>

#include <limits>

#include "gtest/gtest.h"
#include "absl/base/internal/spinlock.h"
#include "absl/base/macros.h"
#include "absl/base/thread_annotations.h"
#include "absl/container/flat_hash_set.h"
#include "tcmalloc/common.h"
#include "tcmalloc/internal/config.h"
#include "tcmalloc/internal/logging.h"
#include "tcmalloc/internal/memory_tag.h"
#include "tcmalloc/internal/page_allocator_hooks.h"
#include "tcmalloc/malloc_extension.h"
#include "tcmalloc/malloc_hook.h"
#include "tcmalloc/stats.h"
#include "tcmalloc/testing/testutil.h"

constexpr size_t kMaxTraceObjects = 10000;

static absl::base_internal::SpinLock spinlock(
    absl::base_internal::SCHEDULE_KERNEL_ONLY);

static size_t page_allocator_new_count ABSL_GUARDED_BY(spinlock) = 0;
static size_t page_allocator_new_pages[kMaxTraceObjects] ABSL_GUARDED_BY(
    spinlock);

struct ReleaseRecord {
  size_t num_pages;
  size_t released;
  tcmalloc::tcmalloc_internal::PageReleaseReason reason;
};

static size_t page_allocator_release_count ABSL_GUARDED_BY(spinlock) = 0;
static ReleaseRecord
    page_allocator_release_records[kMaxTraceObjects] ABSL_GUARDED_BY(spinlock);

constexpr size_t kMaxNewObjects = 10000;
static size_t new_hook_count ABSL_GUARDED_BY(spinlock) = 0;
static const void* new_hook_objects[kMaxNewObjects] ABSL_GUARDED_BY(spinlock);

static void RecordPageAllocatorNewHook(
    size_t start_page_index, size_t n, size_t align, size_t objects_per_span,
    uint8_t density, tcmalloc::tcmalloc_internal::MemoryTag tag) {
  absl::base_internal::SpinLockHolder l(spinlock);
  for (size_t i = 0; i < n; ++i) {
    if (page_allocator_new_count < kMaxTraceObjects) {
      page_allocator_new_pages[page_allocator_new_count++] =
          start_page_index + i;
    }
  }
}

static void RecordPageAllocatorReleaseHook(
    size_t num_pages, size_t released,
    tcmalloc::tcmalloc_internal::PageReleaseReason reason) {
  absl::base_internal::SpinLockHolder l(spinlock);
  if (page_allocator_release_count < kMaxTraceObjects) {
    page_allocator_release_records[page_allocator_release_count++] = {
        num_pages, released, reason};
  }
}

static void RecordNewHook(const tcmalloc::MallocHook::NewInfo& info) {
  absl::base_internal::SpinLockHolder l(spinlock);
  if (new_hook_count < kMaxNewObjects) {
    new_hook_objects[new_hook_count++] = info.ptr;
  }
}

extern "C" void MallocHook_InitAtFirstAllocation_ForTesting() {
  TC_CHECK(tcmalloc::MallocHook::AddNewHook(RecordNewHook));
}

extern "C" void TCMalloc_PageAllocator_InitAtFirstNew_Tracing() {
  TC_CHECK(tcmalloc::tcmalloc_internal::page_allocator_new_hooks.Add(
      RecordPageAllocatorNewHook));
  TC_CHECK(tcmalloc::tcmalloc_internal::page_allocator_release_hooks.Add(
      RecordPageAllocatorReleaseHook));
}

namespace tcmalloc {
namespace {

TEST(PageAllocatorTracingTest, PageAllocatorNewHook) {
  if (tcmalloc_internal::kSanitizerPresent) {
    GTEST_SKIP() << "Skipping under sanitizers";
  }

  // Verify that our first callback from PageAllocator::New corresponds to an
  // object we returned via NewHook.

  // Google Test will certainly allocate before here, but guarantee that we
  // have allocations of various sizes, including above kMaxSize.
  constexpr size_t kSizes[] = {
      16,
      1024,
      tcmalloc::tcmalloc_internal::kMaxSize + 1024,
      3 * 1024 * 1024,
  };
  void* ptrs[ABSL_ARRAYSIZE(kSizes)];
  for (size_t i = 0; i < ABSL_ARRAYSIZE(kSizes); ++i) {
    ptrs[i] = ::operator new(kSizes[i]);
  }
  for (size_t i = 0; i < ABSL_ARRAYSIZE(kSizes); ++i) {
    ::operator delete(ptrs[i]);
  }

  // Stop recording allocations.
  TC_CHECK(tcmalloc::MallocHook::RemoveNewHook(RecordNewHook));
  TC_CHECK(tcmalloc::tcmalloc_internal::page_allocator_new_hooks.Remove(
      RecordPageAllocatorNewHook));

  absl::flat_hash_set<size_t> pages;
  pages.reserve(kMaxTraceObjects);

  bool found = false;
  {
    absl::base_internal::SpinLockHolder l(spinlock);

    for (size_t i = 0; i < page_allocator_new_count; ++i) {
      pages.insert(page_allocator_new_pages[i]);
    }

    for (size_t i = 0; i < new_hook_count; ++i) {
      const size_t page_index =
          reinterpret_cast<uintptr_t>(new_hook_objects[i]) >>
          tcmalloc::tcmalloc_internal::kPageShift;
      if (pages.contains(page_index)) {
        found = true;
      }
    }

    // Large allocations (> kMaxSize) allocate a new span from PageAllocator,
    // so we can strictly assert that each large allocation is in pages.
    for (size_t i = 0; i < ABSL_ARRAYSIZE(kSizes); ++i) {
      if (kSizes[i] > tcmalloc::tcmalloc_internal::kMaxSize) {
        const size_t large_page_index = reinterpret_cast<uintptr_t>(ptrs[i]) >>
                                        tcmalloc::tcmalloc_internal::kPageShift;
        EXPECT_TRUE(pages.contains(large_page_index))
            << "Large allocation of size " << kSizes[i]
            << " was not found in the page allocator batch";
      }
    }

    EXPECT_FALSE(pages.empty());
    EXPECT_NE(new_hook_count, 0);
  }

  EXPECT_TRUE(found) << "None of the early allocations were found in the "
                        "page allocator batch";
}

TEST(PageAllocatorTracingTest, PageAllocatorReleaseHook) {
  if (tcmalloc_internal::kSanitizerPresent) {
    GTEST_SKIP() << "Skipping under sanitizers";
  }

  tcmalloc::ScopedBackgroundReleaseRate disable_release(
      tcmalloc::MallocExtension::BytesPerSecond{0});

  (void)tcmalloc_internal::page_allocator_release_hooks.Add(
      RecordPageAllocatorReleaseHook);

  // Allocate and delete some memory so there are free pages to release.
  constexpr size_t kSize = 10 * 1024 * 1024;
  void* ptr = ::operator new(kSize);
  ::operator delete(ptr);

  {
    absl::base_internal::SpinLockHolder l(spinlock);
    page_allocator_release_count = 0;
  }

  // Explicitly request memory to be released to the system.
  tcmalloc::MallocExtension::ReleaseMemoryToSystem(
      std::numeric_limits<size_t>::max());

  TC_CHECK(tcmalloc::tcmalloc_internal::page_allocator_release_hooks.Remove(
      RecordPageAllocatorReleaseHook));

  absl::base_internal::SpinLockHolder l(spinlock);
  EXPECT_GT(page_allocator_release_count, 0);
  bool found_release_to_system = false;
  for (size_t i = 0; i < page_allocator_release_count; ++i) {
    if (page_allocator_release_records[i].reason ==
        tcmalloc::tcmalloc_internal::PageReleaseReason::
            kReleaseMemoryToSystem) {
      found_release_to_system = true;
      EXPECT_GT(page_allocator_release_records[i].num_pages, 0);
      EXPECT_GT(page_allocator_release_records[i].released, 0);
    }
  }
  EXPECT_TRUE(found_release_to_system)
      << "Expected to observe PageReleaseReason::kReleaseMemoryToSystem";
}

}  // namespace
}  // namespace tcmalloc
