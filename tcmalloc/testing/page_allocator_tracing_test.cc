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
#include <sys/mman.h>

#include <atomic>
#include <cstring>
#include <limits>
#include <thread>
#include <vector>

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
#include "tcmalloc/page_allocator.h"
#include "tcmalloc/static_vars.h"
#include "tcmalloc/stats.h"
#include "tcmalloc/testing/testutil.h"

constexpr size_t kMaxTraceObjects = 10000;

static absl::base_internal::SpinLock spinlock(
    absl::base_internal::SCHEDULE_KERNEL_ONLY);

static size_t page_allocator_new_count ABSL_GUARDED_BY(spinlock) = 0;
static size_t page_allocator_new_pages[kMaxTraceObjects] ABSL_GUARDED_BY(
    spinlock);

static size_t page_allocator_delete_count ABSL_GUARDED_BY(spinlock) = 0;
static size_t page_allocator_delete_pages[kMaxTraceObjects] ABSL_GUARDED_BY(
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

static size_t delete_hook_count ABSL_GUARDED_BY(spinlock) = 0;
static const void* delete_hook_objects[kMaxNewObjects] ABSL_GUARDED_BY(
    spinlock);

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

static void RecordPageAllocatorDeleteHook(
    size_t start_page_index, size_t n, size_t objects_per_span, uint8_t density,
    tcmalloc::tcmalloc_internal::MemoryTag tag) {
  absl::base_internal::SpinLockHolder l(spinlock);
  for (size_t i = 0; i < n; ++i) {
    if (page_allocator_delete_count < kMaxTraceObjects) {
      page_allocator_delete_pages[page_allocator_delete_count++] =
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

static void RecordDeleteHook(const tcmalloc::MallocHook::DeleteInfo& info) {
  absl::base_internal::SpinLockHolder l(spinlock);
  if (delete_hook_count < kMaxNewObjects) {
    delete_hook_objects[delete_hook_count++] = info.ptr;
  }
}

extern "C" void MallocHook_InitAtFirstAllocation_ForTesting() {
  TC_CHECK(tcmalloc::MallocHook::AddNewHook(RecordNewHook));
  TC_CHECK(tcmalloc::MallocHook::AddDeleteHook(RecordDeleteHook));
}

extern "C" void TCMalloc_PageAllocator_InitAtFirstNew_Tracing() {
  TC_CHECK(tcmalloc::tcmalloc_internal::page_allocator_new_hooks.Add(
      RecordPageAllocatorNewHook));
  TC_CHECK(tcmalloc::tcmalloc_internal::page_allocator_delete_hooks.Add(
      RecordPageAllocatorDeleteHook));
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

  // Stop recording allocations and deallocations.
  TC_CHECK(tcmalloc::MallocHook::RemoveNewHook(RecordNewHook));
  TC_CHECK(tcmalloc::MallocHook::RemoveDeleteHook(RecordDeleteHook));
  TC_CHECK(tcmalloc::tcmalloc_internal::page_allocator_new_hooks.Remove(
      RecordPageAllocatorNewHook));
  TC_CHECK(tcmalloc::tcmalloc_internal::page_allocator_delete_hooks.Remove(
      RecordPageAllocatorDeleteHook));

  absl::flat_hash_set<size_t> new_pages;
  new_pages.reserve(kMaxTraceObjects);
  absl::flat_hash_set<size_t> delete_pages;
  delete_pages.reserve(kMaxTraceObjects);

  bool found_new = false;
  bool found_delete = false;
  {
    absl::base_internal::SpinLockHolder l(spinlock);

    for (size_t i = 0; i < page_allocator_new_count; ++i) {
      new_pages.insert(page_allocator_new_pages[i]);
    }
    for (size_t i = 0; i < page_allocator_delete_count; ++i) {
      delete_pages.insert(page_allocator_delete_pages[i]);
    }

    for (size_t i = 0; i < new_hook_count; ++i) {
      const size_t page_index =
          reinterpret_cast<uintptr_t>(new_hook_objects[i]) >>
          tcmalloc::tcmalloc_internal::kPageShift;
      if (new_pages.contains(page_index)) {
        found_new = true;
      }
    }
    for (size_t i = 0; i < delete_hook_count; ++i) {
      const size_t page_index =
          reinterpret_cast<uintptr_t>(delete_hook_objects[i]) >>
          tcmalloc::tcmalloc_internal::kPageShift;
      if (delete_pages.contains(page_index)) {
        found_delete = true;
      }
    }

    // Large allocations (> kMaxSize) allocate a new span from PageAllocator,
    // so we can strictly assert that each large allocation is in new_pages and
    // delete_pages.
    for (size_t i = 0; i < ABSL_ARRAYSIZE(kSizes); ++i) {
      if (kSizes[i] > tcmalloc::tcmalloc_internal::kMaxSize) {
        const size_t large_page_index = reinterpret_cast<uintptr_t>(ptrs[i]) >>
                                        tcmalloc::tcmalloc_internal::kPageShift;
        EXPECT_TRUE(new_pages.contains(large_page_index))
            << "Large allocation of size " << kSizes[i]
            << " was not found in the page allocator new batch";
        EXPECT_TRUE(delete_pages.contains(large_page_index))
            << "Large allocation of size " << kSizes[i]
            << " was not found in the page allocator delete batch";
      }
    }

    EXPECT_FALSE(new_pages.empty());
    EXPECT_NE(new_hook_count, 0);
    EXPECT_FALSE(delete_pages.empty());
    EXPECT_NE(delete_hook_count, 0);
  }

  EXPECT_TRUE(found_new) << "None of the early allocations were found in the "
                            "page allocator new batch";
  EXPECT_TRUE(found_delete)
      << "None of the early deallocations were found in the "
         "page allocator delete batch";
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

using ::tcmalloc::tcmalloc_internal::PageAllocator;
using ::tcmalloc::tcmalloc_internal::pageheap_lock;
using ::tcmalloc::tcmalloc_internal::tc_globals;

TEST(PageAllocatorTracingTest, BufferAllocatedOutsideLock) {
  if (tcmalloc_internal::kSanitizerPresent) {
    GTEST_SKIP() << "Skipping under sanitizers";
  }
  auto& allocator = tc_globals.page_allocator();
  allocator.DisableTracing();

  static std::atomic<bool> lock_held{false};
  static std::atomic<size_t> alloc_count{0};
  static std::atomic<size_t> dealloc_count{0};

  allocator.SetTracingBufferHooksForTesting(
      [](size_t size) -> void* {
        if (pageheap_lock.IsHeld()) {
          lock_held.store(true, std::memory_order_relaxed);
        }
        alloc_count.fetch_add(1, std::memory_order_relaxed);
        return mmap(nullptr, size, PROT_READ | PROT_WRITE,
                    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
      },
      [](void* ptr, size_t size) {
        dealloc_count.fetch_add(1, std::memory_order_relaxed);
        munmap(ptr, size);
      });

  EXPECT_TRUE(allocator.EnableTracing(1024 * 1024));
  EXPECT_TRUE(allocator.tracing_enabled());
  EXPECT_NE(allocator.tracing_buffer(), nullptr);
  EXPECT_EQ(allocator.tracing_buffer_size(), 1024 * 1024);
  EXPECT_FALSE(lock_held.load(std::memory_order_relaxed))
      << "Tracing buffer was allocated while holding pageheap_lock!";

  allocator.DisableTracing();
  EXPECT_FALSE(allocator.tracing_enabled());
  EXPECT_EQ(allocator.tracing_buffer(), nullptr);
  EXPECT_EQ(allocator.tracing_buffer_size(), 0);
  EXPECT_EQ(alloc_count.load(), 1);
  EXPECT_EQ(dealloc_count.load(), 1);

  allocator.SetTracingBufferHooksForTesting(nullptr, nullptr);
}

TEST(PageAllocatorTracingTest, ConcurrentEnableTracing) {
  if (tcmalloc_internal::kSanitizerPresent) {
    GTEST_SKIP() << "Skipping under sanitizers";
  }
  auto& allocator = tc_globals.page_allocator();
  allocator.DisableTracing();

  static std::atomic<bool> lock_held{false};
  static std::atomic<size_t> alloc_count{0};
  static std::atomic<size_t> dealloc_count{0};

  allocator.SetTracingBufferHooksForTesting(
      [](size_t size) -> void* {
        if (pageheap_lock.IsHeld()) {
          lock_held.store(true, std::memory_order_relaxed);
        }
        alloc_count.fetch_add(1, std::memory_order_relaxed);
        return mmap(nullptr, size, PROT_READ | PROT_WRITE,
                    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
      },
      [](void* ptr, size_t size) {
        dealloc_count.fetch_add(1, std::memory_order_relaxed);
        munmap(ptr, size);
      });

  constexpr int kNumThreads = 16;
  std::vector<std::thread> threads;
  threads.reserve(kNumThreads);
  std::atomic<int> success_count{0};
  std::atomic<bool> start_signal{false};

  for (int i = 0; i < kNumThreads; ++i) {
    threads.emplace_back([&]() {
      while (!start_signal.load(std::memory_order_acquire)) {
      }
      if (allocator.EnableTracing(1024 * 1024)) {
        success_count.fetch_add(1, std::memory_order_relaxed);
      }
    });
  }

  start_signal.store(true, std::memory_order_release);
  for (auto& t : threads) {
    t.join();
  }

  EXPECT_EQ(success_count.load(), 1);
  EXPECT_TRUE(allocator.tracing_enabled());
  EXPECT_NE(allocator.tracing_buffer(), nullptr);
  EXPECT_FALSE(lock_held.load(std::memory_order_relaxed))
      << "Tracing buffer was allocated while holding pageheap_lock!";

  allocator.DisableTracing();
  EXPECT_FALSE(allocator.tracing_enabled());
  EXPECT_EQ(allocator.tracing_buffer(), nullptr);
  EXPECT_EQ(alloc_count.load(), dealloc_count.load())
      << "Mismatch between allocated and deallocated buffers (leak detected)";

  allocator.SetTracingBufferHooksForTesting(nullptr, nullptr);
}

TEST(PageAllocatorTracingTest, DefaultMmapAllocation) {
  if (tcmalloc_internal::kSanitizerPresent) {
    GTEST_SKIP() << "Skipping under sanitizers";
  }
  auto& allocator = tc_globals.page_allocator();
  allocator.DisableTracing();
  allocator.SetTracingBufferHooksForTesting(nullptr, nullptr);

  EXPECT_TRUE(allocator.EnableTracing());
  EXPECT_TRUE(allocator.tracing_enabled());
  void* buf = allocator.tracing_buffer();
  EXPECT_NE(buf, nullptr);
  EXPECT_EQ(allocator.tracing_buffer_size(),
            PageAllocator::kDefaultTracingBufferSize);

  memset(buf, 0xAB, 4096);
  EXPECT_EQ(*reinterpret_cast<uint8_t*>(buf), 0xAB);

  allocator.DisableTracing();
  EXPECT_FALSE(allocator.tracing_enabled());
  EXPECT_EQ(allocator.tracing_buffer(), nullptr);
  EXPECT_EQ(allocator.tracing_buffer_size(), 0);
}

}  // namespace
}  // namespace tcmalloc
