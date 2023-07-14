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

#include "tcmalloc/guarded_page_allocator.h"

#include <signal.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <memory>
#include <string>
#include <thread>  // NOLINT(build/c++11)
#include <vector>

#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "absl/base/attributes.h"
#include "absl/base/casts.h"
#include "absl/base/internal/spinlock.h"
#include "absl/base/internal/sysinfo.h"
#include "absl/container/flat_hash_set.h"
#include "absl/memory/memory.h"
#include "absl/numeric/bits.h"
#include "absl/time/clock.h"
#include "absl/time/time.h"
#include "tcmalloc/common.h"
#include "tcmalloc/internal/logging.h"
#include "tcmalloc/internal/page_size.h"
#include "tcmalloc/internal/sysinfo.h"
#include "tcmalloc/malloc_extension.h"
#include "tcmalloc/static_vars.h"
#include "tcmalloc/testing/testutil.h"

namespace tcmalloc {
namespace tcmalloc_internal {
namespace {

static constexpr size_t kMaxGpaPages = GuardedPageAllocator::kGpaMaxPages;

// Size of pages used by GuardedPageAllocator.
static size_t PageSize() {
  static const size_t page_size =
      std::max(kPageSize, static_cast<size_t>(GetPageSize()));
  return page_size;
}

class GuardedPageAllocatorTest : public testing::Test {
 protected:
  GuardedPageAllocatorTest() {
    absl::base_internal::SpinLockHolder h(&pageheap_lock);
    gpa_.Init(kMaxGpaPages, kMaxGpaPages);
    gpa_.AllowAllocations();
  }

  explicit GuardedPageAllocatorTest(size_t num_pages) {
    absl::base_internal::SpinLockHolder h(&pageheap_lock);
    gpa_.Init(num_pages, kMaxGpaPages);
    gpa_.AllowAllocations();
  }

  ~GuardedPageAllocatorTest() override { gpa_.Destroy(); }

  GuardedPageAllocator gpa_;
};

class GuardedPageAllocatorParamTest
    : public GuardedPageAllocatorTest,
      public testing::WithParamInterface<size_t> {
 protected:
  GuardedPageAllocatorParamTest() : GuardedPageAllocatorTest(GetParam()) {}
};

TEST_F(GuardedPageAllocatorTest, SingleAllocDealloc) {
  auto alloc_with_status = gpa_.Allocate(PageSize(), 0);
  EXPECT_EQ(alloc_with_status.status, Profile::Sample::GuardedStatus::Guarded);
  EXPECT_EQ(gpa_.SuccessfulAllocations(), 1);
  char* buf = static_cast<char*>(alloc_with_status.alloc);
  EXPECT_NE(buf, nullptr);
  EXPECT_TRUE(gpa_.PointerIsMine(buf));
  memset(buf, 'A', PageSize());
  EXPECT_DEATH(buf[-1] = 'A', "");
  EXPECT_DEATH(buf[PageSize()] = 'A', "");
  gpa_.Deallocate(buf);
  EXPECT_DEATH(buf[0] = 'B', "");
  EXPECT_DEATH(buf[PageSize() / 2] = 'B', "");
  EXPECT_DEATH(buf[PageSize() - 1] = 'B', "");
}

TEST_F(GuardedPageAllocatorTest, NoAlignmentProvided) {
  constexpr size_t kLargeObjectAlignment =
      std::max(static_cast<size_t>(kAlignment),
               static_cast<size_t>(__STDCPP_DEFAULT_NEW_ALIGNMENT__));

  int allocation_count = 0;
  for (size_t base_size = 1; base_size <= 64; base_size <<= 1) {
    for (size_t size : {base_size, base_size + 1}) {
      SCOPED_TRACE(size);

      constexpr int kElements = 10;
      std::array<void*, kElements> ptrs;

      // Make several allocation attempts to encounter left/right-alignment in
      // the guarded region.
      for (int i = 0; i < kElements; i++) {
        auto alloc_with_status = gpa_.Allocate(size, 0);
        EXPECT_EQ(alloc_with_status.status,
                  Profile::Sample::GuardedStatus::Guarded);
        ptrs[i] = alloc_with_status.alloc;
        EXPECT_NE(ptrs[i], nullptr);
        EXPECT_TRUE(gpa_.PointerIsMine(ptrs[i]));
        ++allocation_count;

        size_t observed_alignment =
            1 << absl::countr_zero(absl::bit_cast<uintptr_t>(ptrs[i]));
        EXPECT_GE(observed_alignment, std::min(size, kLargeObjectAlignment));
      }

      for (void* ptr : ptrs) {
        gpa_.Deallocate(ptr);
      }
    }
  }
  EXPECT_EQ(gpa_.SuccessfulAllocations(), allocation_count);
}

TEST_F(GuardedPageAllocatorTest, AllocDeallocAligned) {
  for (size_t align = 1; align <= PageSize(); align <<= 1) {
    constexpr size_t alloc_size = 1;
    auto alloc_with_status = gpa_.Allocate(alloc_size, align);
    EXPECT_EQ(alloc_with_status.status,
              Profile::Sample::GuardedStatus::Guarded);
    EXPECT_NE(alloc_with_status.alloc, nullptr);
    EXPECT_TRUE(gpa_.PointerIsMine(alloc_with_status.alloc));
    EXPECT_EQ(reinterpret_cast<uintptr_t>(alloc_with_status.alloc) % align, 0);
  }
  EXPECT_EQ(gpa_.SuccessfulAllocations(), (32 - __builtin_clz(PageSize())));
}

TEST_P(GuardedPageAllocatorParamTest, AllocDeallocAllPages) {
  size_t num_pages = GetParam();
  char* bufs[kMaxGpaPages];
  for (size_t i = 0; i < num_pages; i++) {
    auto alloc_with_status = gpa_.Allocate(1, 0);
    EXPECT_EQ(alloc_with_status.status,
              Profile::Sample::GuardedStatus::Guarded);
    bufs[i] = reinterpret_cast<char*>(alloc_with_status.alloc);
    EXPECT_NE(bufs[i], nullptr);
    EXPECT_TRUE(gpa_.PointerIsMine(bufs[i]));
  }
  EXPECT_EQ(gpa_.SuccessfulAllocations(), num_pages);
  auto alloc_with_status = gpa_.Allocate(1, 0);
  EXPECT_EQ(alloc_with_status.status,
            Profile::Sample::GuardedStatus::NoAvailableSlots);
  EXPECT_EQ(alloc_with_status.alloc, nullptr);
  gpa_.Deallocate(bufs[0]);
  alloc_with_status = gpa_.Allocate(1, 0);
  EXPECT_EQ(alloc_with_status.status, Profile::Sample::GuardedStatus::Guarded);
  bufs[0] = reinterpret_cast<char*>(alloc_with_status.alloc);
  EXPECT_NE(bufs[0], nullptr);
  EXPECT_TRUE(gpa_.PointerIsMine(bufs[0]));
  EXPECT_EQ(gpa_.SuccessfulAllocations(), num_pages + 1);
  for (size_t i = 0; i < num_pages; i++) {
    bufs[i][0] = 'A';
    gpa_.Deallocate(bufs[i]);
  }
}
INSTANTIATE_TEST_SUITE_P(VaryNumPages, GuardedPageAllocatorParamTest,
                         testing::Values(1, kMaxGpaPages / 2, kMaxGpaPages));

TEST_F(GuardedPageAllocatorTest, PointerIsMine) {
  auto alloc_with_status = gpa_.Allocate(1, 0);
  EXPECT_EQ(alloc_with_status.status, Profile::Sample::GuardedStatus::Guarded);
  EXPECT_EQ(gpa_.SuccessfulAllocations(), 1);
  void* buf = alloc_with_status.alloc;
  int stack_var;
  auto malloc_ptr = absl::make_unique<char>();
  EXPECT_TRUE(gpa_.PointerIsMine(buf));
  EXPECT_FALSE(gpa_.PointerIsMine(&stack_var));
  EXPECT_FALSE(gpa_.PointerIsMine(malloc_ptr.get()));
}

TEST_F(GuardedPageAllocatorTest, Print) {
  char buf[1024] = {};
  Printer out(buf, sizeof(buf));
  gpa_.Print(&out);
  EXPECT_THAT(buf, testing::ContainsRegex("GWP-ASan Status"));
}

// Test that no pages are double-allocated or left unallocated, and that no
// extra pages are allocated when there's concurrent calls to Allocate().
TEST_F(GuardedPageAllocatorTest, ThreadedAllocCount) {
  constexpr size_t kNumThreads = 2;
  void* allocations[kNumThreads][kMaxGpaPages];
  {
    std::vector<std::thread> threads;
    threads.reserve(kNumThreads);
    for (size_t i = 0; i < kNumThreads; i++) {
      threads.push_back(std::thread([this, &allocations, i]() {
        for (size_t j = 0; j < kMaxGpaPages; j++) {
          allocations[i][j] = gpa_.Allocate(1, 0).alloc;
        }
      }));
    }

    for (auto& t : threads) {
      t.join();
    }
  }
  absl::flat_hash_set<void*> allocations_set;
  for (size_t i = 0; i < kNumThreads; i++) {
    for (size_t j = 0; j < kMaxGpaPages; j++) {
      allocations_set.insert(allocations[i][j]);
    }
  }
  allocations_set.erase(nullptr);
  EXPECT_EQ(allocations_set.size(), kMaxGpaPages);
  EXPECT_EQ(gpa_.SuccessfulAllocations(), kMaxGpaPages);
}

// Test that allocator remains in consistent state under high contention and
// doesn't double-allocate pages or fail to deallocate pages.
TEST_F(GuardedPageAllocatorTest, ThreadedHighContention) {
  const size_t kNumThreads = 4 * NumCPUs();
  {
    std::vector<std::thread> threads;
    threads.reserve(kNumThreads);
    for (size_t i = 0; i < kNumThreads; i++) {
      threads.push_back(std::thread([this]() {
        char* buf;
        while (true) {
          auto alloc_with_status = gpa_.Allocate(1, 0);
          if (alloc_with_status.status ==
              Profile::Sample::GuardedStatus::Guarded) {
            buf = reinterpret_cast<char*>(alloc_with_status.alloc);
            EXPECT_NE(buf, nullptr);
            break;
          }
          absl::SleepFor(absl::Nanoseconds(5000));
        }

        // Verify that no other thread has access to this page.
        EXPECT_EQ(buf[0], 0);

        // Mark this page and allow some time for another thread to potentially
        // gain access to this page.
        buf[0] = 'A';
        absl::SleepFor(absl::Nanoseconds(5000));

        // Unmark this page and deallocate.
        buf[0] = 0;
        gpa_.Deallocate(buf);
      }));
    }

    for (auto& t : threads) {
      t.join();
    }
  }
  // Verify all pages have been deallocated now that all threads are done.
  for (size_t i = 0; i < kMaxGpaPages; i++) {
    auto alloc_with_status = gpa_.Allocate(1, 0);
    EXPECT_EQ(alloc_with_status.status,
              Profile::Sample::GuardedStatus::Guarded);
    EXPECT_NE(alloc_with_status.alloc, nullptr);
  }
}

TEST_F(GuardedPageAllocatorTest, SignalHandlerStackConsumption) {
  // Test the signal handler stack consumption. Since it runs on potentially
  // limited signal stack, the consumption is important. If the test fails,
  // the numbers may need to be updated. Reducing stack usage is always good,
  // increasing may indicate a problem. Avoid setting too high slack,
  // since it will prevent detection of usage changes in future.
  auto ptr = tc_globals.guardedpage_allocator().Allocate(1, 0);
  if (ptr.status != Profile::Sample::GuardedStatus::Guarded) {
    GTEST_SKIP() << "did not get a guarded allocation";
  }
  ASSERT_NE(ptr.alloc, nullptr);
  tc_globals.guardedpage_allocator().Deallocate(ptr.alloc);
  static void* addr;
  addr = ptr.alloc;
  constexpr size_t kStackSize = 1 << 20;
  void* altstack = mmap(nullptr, kStackSize, PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  ASSERT_NE(altstack, MAP_FAILED);
  stack_t sigstk = {};
  sigstk.ss_sp = altstack;
  sigstk.ss_size = kStackSize;
  stack_t old_sigstk;
  EXPECT_EQ(sigaltstack(&sigstk, &old_sigstk), 0);
  struct sigaction act = {};
  act.sa_flags = SA_SIGINFO | SA_ONSTACK;
  act.sa_sigaction = [](int sig, siginfo_t* info, void* ctx) {
    info->si_addr = addr;
    SegvHandler(SIGSEGV, info, ctx);
  };
  struct sigaction oldact;
  ASSERT_EQ(sigaction(SIGUSR1, &act, &oldact), 0);
  constexpr char kFillValue = 0xe1;
  memset(altstack, kFillValue, kStackSize);
  for (int i = 0; i < 10; ++i) {
    ASSERT_EQ(syscall(SYS_tgkill, getpid(), syscall(SYS_gettid), SIGUSR1), 0);
  }
  ASSERT_EQ(sigaltstack(&old_sigstk, nullptr), 0);
  ASSERT_EQ(sigaction(SIGUSR1, &oldact, nullptr), 0);
  size_t usage = kStackSize;
  for (;
       usage && static_cast<char*>(altstack)[kStackSize - usage] == kFillValue;
       --usage) {
  }
#if defined(__x86_64__)
#if defined(NDEBUG)
  constexpr size_t kExpectedUsage = 12800;
  constexpr size_t kUsageSlack = 25;
#else
  constexpr size_t kExpectedUsage = 14400;
  constexpr size_t kUsageSlack = 45;
#endif
#elif defined(__aarch64__)
#if defined(NDEBUG)
  constexpr size_t kExpectedUsage = 12500;
  constexpr size_t kUsageSlack = 10;
#else
  constexpr size_t kExpectedUsage = 16000;
  constexpr size_t kUsageSlack = 30;
#endif
#else
  constexpr size_t kExpectedUsage = 100000;
  constexpr size_t kUsageSlack = 95;
#endif
  printf("stack usage: %zu\n", usage);
  EXPECT_GT(usage, 0);
  EXPECT_LT(usage, kExpectedUsage);
  EXPECT_GT(usage, kExpectedUsage * (100 - kUsageSlack) / 100);
}

TEST_F(GuardedPageAllocatorTest, DeleteSizeCheck) {
#ifdef NDEBUG
  GTEST_SKIP() << "requires debug build";
#endif
  for (size_t i = 0; i < 1000; ++i) {
    void* ptr = ::operator new(1000);
    if (!tc_globals.guardedpage_allocator().PointerIsMine(ptr)) {
      ::operator delete(ptr);
      continue;
    }
    EXPECT_DEATH(sized_delete(ptr, 2000);, "size check failed 1000 2000");
    ::operator delete(ptr);
    return;
  }
  GTEST_SKIP() << "can't get a guarded allocation, giving up";
}

ABSL_CONST_INIT ABSL_ATTRIBUTE_UNUSED GuardedPageAllocator
    gpa_is_constant_initializable;

}  // namespace
}  // namespace tcmalloc_internal
}  // namespace tcmalloc
