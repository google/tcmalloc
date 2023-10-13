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
#include <stdlib.h>
#include <sys/mman.h>

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <memory>
#include <new>
#include <string>
#include <tuple>

#include "benchmark/benchmark.h"
#include "gtest/gtest.h"
#include "absl/base/attributes.h"
#include "absl/base/casts.h"
#include "absl/numeric/bits.h"
#include "absl/strings/str_cat.h"
#include "tcmalloc/common.h"
#include "tcmalloc/guarded_allocations.h"
#include "tcmalloc/guarded_page_allocator.h"
#include "tcmalloc/internal/logging.h"
#include "tcmalloc/internal/stacktrace_filter.h"
#include "tcmalloc/malloc_extension.h"
#include "tcmalloc/static_vars.h"
#include "tcmalloc/testing/testutil.h"

namespace tcmalloc {
namespace {

using tcmalloc_internal::kPageShift;
using tcmalloc_internal::kPageSize;
using tcmalloc_internal::tc_globals;

class GuardedAllocAlignmentTest : public testing::Test {
 protected:
  GuardedAllocAlignmentTest() {
    profile_sampling_rate_ = MallocExtension::GetProfileSamplingRate();
    guarded_sample_rate_ = MallocExtension::GetGuardedSamplingRate();
    MallocExtension::SetProfileSamplingRate(1);  // Always do heapz samples.
    MallocExtension::SetGuardedSamplingRate(
        0);  // TODO(b/201336703): Guard every heapz sample.
    MallocExtension::ActivateGuardedSampling();

    // Eat up unsampled bytes remaining to flush the new sample rates.
    while (true) {
      void* p = ::operator new(kPageSize);
      if (tc_globals.guardedpage_allocator().PointerIsMine(p)) {
        ::operator delete(p);
        break;
      }
      ::operator delete(p);
    }

    // Ensure subsequent allocations are guarded.
    void* p = ::operator new(1);
    CHECK_CONDITION(tc_globals.guardedpage_allocator().PointerIsMine(p));
    ::operator delete(p);
  }

  ~GuardedAllocAlignmentTest() override {
    MallocExtension::SetProfileSamplingRate(profile_sampling_rate_);
    MallocExtension::SetGuardedSamplingRate(guarded_sample_rate_);
  }

 private:
  int64_t profile_sampling_rate_;
  int32_t guarded_sample_rate_;
};

TEST_F(GuardedAllocAlignmentTest, Malloc) {
  for (size_t lg = 0; lg <= kPageShift; lg++) {
    size_t base_size = size_t{1} << lg;
    const size_t sizes[] = {base_size - 1, base_size, base_size + 1};
    for (size_t size : sizes) {
      void* p = malloc(size);
      // TCMalloc currently always aligns mallocs to alignof(std::max_align_t),
      // even for small sizes.  If this ever changes, we can reduce the expected
      // alignment here for sizes < alignof(std::max_align_t).
      EXPECT_EQ(reinterpret_cast<uintptr_t>(p) % alignof(std::max_align_t), 0);
      free(p);
    }
  }
}

TEST_F(GuardedAllocAlignmentTest, PosixMemalign) {
  for (size_t align = sizeof(void*); align <= kPageSize; align <<= 1) {
    void* p = nullptr;
    EXPECT_EQ(posix_memalign(&p, align, 1), 0);
    EXPECT_EQ(reinterpret_cast<uintptr_t>(p) % align, 0);
    benchmark::DoNotOptimize(p);
    free(p);
  }
}

TEST_F(GuardedAllocAlignmentTest, New) {
  for (size_t lg = 0; lg <= kPageShift; lg++) {
    size_t base_size = size_t{1} << lg;
    const size_t sizes[] = {base_size - 1, base_size, base_size + 1};
    for (size_t size : sizes) {
      void* p = ::operator new(size);

      // In the absence of a user-specified alignment, the required alignment
      // for operator new is never larger than the size rounded up to the next
      // power of 2.  GuardedPageAllocator uses this fact to minimize alignment
      // padding between the end of small allocations and their guard pages.
      size_t expected_align = std::min(
          absl::bit_ceil(size),
          std::max(static_cast<size_t>(tcmalloc_internal::kAlignment),
                   static_cast<size_t>(__STDCPP_DEFAULT_NEW_ALIGNMENT__)));

      EXPECT_EQ(reinterpret_cast<uintptr_t>(p) % expected_align, 0);
      ::operator delete(p);
    }
  }
}

TEST_F(GuardedAllocAlignmentTest, AlignedNew) {
  for (size_t align = 1; align <= kPageSize; align <<= 1) {
    void* p = ::operator new(1, static_cast<std::align_val_t>(align));
    EXPECT_EQ(reinterpret_cast<uintptr_t>(p) % align, 0);
    ::operator delete(p);
  }
}

}  // namespace

// By placing this class in the tcmalloc namespace, it may call the private
// method StackTraceFilter::Reset as a friend.
class TcMallocTest : public testing::Test {
 protected:
  TcMallocTest() {
    MallocExtension::SetGuardedSamplingRate(
        10 * MallocExtension::GetProfileSamplingRate());

    // Prevent SEGV handler from writing XML properties in death tests.
    unsetenv("XML_OUTPUT_FILE");
  }

  void MaybeResetStackTraceFilter(bool improved_coverage_enabled) {
    if (!improved_coverage_enabled) {
      return;
    }
    ++reset_request_count_;
    // Only reset when the requests (expected to be matched to detected guards)
    // exceeds the maximum number of guards for a single location provided by
    // Improved Guarded Sampling.
    if (reset_request_count_ >=
        tcmalloc_internal::kMaxGuardsPerStackTraceSignature) {
      tc_globals.stacktrace_filter().Reset();
      reset_request_count_ = 0;
    }
  }

 private:
  // Maintain the number of times the reset was requested. The count is compared
  // against the kMaxGuardsPerStackTraceSignature threshold, allowing for
  // conservatively resetting the filter. This avoids exceeding the maximum
  // number of guards for a single location when improved guarded sampling is
  // enabled.
  size_t reset_request_count_ = 0;
};

namespace {

class ReadWriteTcMallocTest
    : public TcMallocTest,
      public testing::WithParamInterface<std::tuple<
          bool /* write_test */, bool /* improved_coverage_enabled */>> {};

TEST_P(ReadWriteTcMallocTest, UnderflowDetected) {
  const bool write_test = std::get<0>(GetParam());
  const bool improved_coverage_enabled = std::get<1>(GetParam());
  ScopedImprovedGuardedSampling scoped_improved_guarded_sampling(
      improved_coverage_enabled);
  auto RepeatUnderflow = [&]() {
    for (int i = 0; i < 1000000; i++) {
      auto buf = std::make_unique<char[]>(kPageSize / 2);
      benchmark::DoNotOptimize(buf);
      // TCMalloc may crash without a GWP-ASan report if we underflow a regular
      // allocation.  Make sure we have a guarded allocation.
      if (tc_globals.guardedpage_allocator().PointerIsMine(buf.get())) {
        if (write_test) {
          buf[-1] = 'A';
        } else {
          volatile char sink = buf[-1];
          benchmark::DoNotOptimize(sink);
        }
        MaybeResetStackTraceFilter(improved_coverage_enabled);
      }
    }
  };
  std::string expected_output =
      absl::StrCat("Buffer underflow ",
#if !defined(__riscv)
                   write_test ? "\\(write\\)" : "\\(read\\)",
#else
                   "\\(read or write: indeterminate\\)",
#endif
                   " occurs in thread [0-9]+ at");
  EXPECT_DEATH(RepeatUnderflow(), expected_output);
}

TEST_P(ReadWriteTcMallocTest, OverflowDetected) {
  const bool write_test = std::get<0>(GetParam());
  const bool improved_coverage_enabled = std::get<1>(GetParam());
  ScopedImprovedGuardedSampling scoped_improved_guarded_sampling(
      improved_coverage_enabled);
  auto RepeatOverflow = [&]() {
    for (int i = 0; i < 1000000; i++) {
      auto buf = std::make_unique<char[]>(kPageSize / 2);
      benchmark::DoNotOptimize(buf);
      // TCMalloc may crash without a GWP-ASan report if we overflow a regular
      // allocation.  Make sure we have a guarded allocation.
      if (tc_globals.guardedpage_allocator().PointerIsMine(buf.get())) {
        if (write_test) {
          buf[kPageSize / 2] = 'A';
          benchmark::DoNotOptimize(buf[kPageSize / 2]);
        } else {
          volatile char sink = buf[kPageSize / 2];
          benchmark::DoNotOptimize(sink);
        }
        MaybeResetStackTraceFilter(improved_coverage_enabled);
      }
    }
  };
  std::string expected_output =
      absl::StrCat("Buffer overflow ",
#if !defined(__riscv)
                   write_test ? "\\(write\\)" : "\\(read\\)",
#else
                   "\\(read or write: indeterminate\\)",
#endif
                   " occurs in thread [0-9]+ at");
  EXPECT_DEATH(RepeatOverflow(), expected_output);
}

TEST_P(ReadWriteTcMallocTest, UseAfterFreeDetected) {
  const bool write_test = std::get<0>(GetParam());
  const bool improved_coverage_enabled = std::get<1>(GetParam());
  ScopedImprovedGuardedSampling scoped_improved_guarded_sampling(
      improved_coverage_enabled);
  auto RepeatUseAfterFree = [&]() {
    for (int i = 0; i < 1000000; i++) {
      char* sink_buf = new char[kPageSize];
      benchmark::DoNotOptimize(sink_buf);
      delete[] sink_buf;
      if (write_test) {
        sink_buf[0] = 'A';
      } else {
        volatile char sink = sink_buf[0];
        benchmark::DoNotOptimize(sink);
      }
    }
  };
  std::string expected_output =
      absl::StrCat("Use-after-free ",
#if !defined(__riscv)
                   write_test ? "\\(write\\)" : "\\(read\\)",
#else
                   "\\(read or write: indeterminate\\)",
#endif
                   " occurs in thread [0-9]+ at");
  EXPECT_DEATH(RepeatUseAfterFree(), expected_output);
}

INSTANTIATE_TEST_SUITE_P(rwtmt, ReadWriteTcMallocTest,
                         testing::Combine(testing::Bool(), testing::Bool()));

class ParameterizedTcMallocTest
    : public TcMallocTest,
      public testing::WithParamInterface<bool /* improved_coverage_enabled */> {
};

// Double free triggers an ASSERT within TCMalloc in non-opt builds.  So only
// run this test for opt builds.
#ifdef NDEBUG
TEST_P(ParameterizedTcMallocTest, DoubleFreeDetected) {
  ScopedImprovedGuardedSampling scoped_improved_guarded_sampling(GetParam());
  auto RepeatDoubleFree = []() {
    for (int i = 0; i < 1000000; i++) {
      void* buf = ::operator new(kPageSize);
      ::operator delete(buf);
      // TCMalloc often SEGVs on double free (without GWP-ASan report). Make
      // sure we have a guarded allocation before double-freeing.
      if (tc_globals.guardedpage_allocator().PointerIsMine(buf)) {
        ::operator delete(buf);
      }
    }
  };
  EXPECT_DEATH(RepeatDoubleFree(), "Double free occurs in thread [0-9]+ at");
}
#endif

TEST_P(ParameterizedTcMallocTest, OverflowWriteDetectedAtFree) {
  ScopedImprovedGuardedSampling scoped_improved_guarded_sampling(GetParam());
  auto RepeatOverflowWrite = [&]() {
    for (int i = 0; i < 1000000; i++) {
      // Make buffer smaller than kPageSize to test detection-at-free of write
      // overflows.
      constexpr size_t kSize = kPageSize - 1;
      auto sink_buf = std::make_unique<char[]>(kSize);
      benchmark::DoNotOptimize(sink_buf);
      sink_buf[kSize] = '\0';
      benchmark::DoNotOptimize(sink_buf[kSize]);
      MaybeResetStackTraceFilter(GetParam());
    }
  };
  EXPECT_DEATH(RepeatOverflowWrite(),
               "Buffer overflow \\(write\\) detected in thread [0-9]+ at free");
}

TEST_P(ParameterizedTcMallocTest, ReallocNoFalsePositive) {
  ScopedImprovedGuardedSampling scoped_improved_guarded_sampling(GetParam());
  for (int i = 0; i < 1000000; i++) {
    auto sink_buf = reinterpret_cast<char*>(malloc(kPageSize - 1));
    benchmark::DoNotOptimize(sink_buf);
    sink_buf = reinterpret_cast<char*>(realloc(sink_buf, kPageSize));
    sink_buf[kPageSize - 1] = '\0';
    benchmark::DoNotOptimize(sink_buf);
    free(sink_buf);
  }
}

TEST_P(ParameterizedTcMallocTest, OffsetAndLength) {
  bool improved_coverage_enabled = GetParam();
  ScopedImprovedGuardedSampling scoped_improved_guarded_sampling(
      improved_coverage_enabled);
  auto RepeatUseAfterFree = [&](size_t buffer_len, off_t access_offset) {
    for (int i = 0; i < 1000000; i++) {
      void* buf = ::operator new(buffer_len);
      ::operator delete(buf);
      // TCMalloc may crash without a GWP-ASan report if we overflow a regular
      // allocation.  Make sure we have a guarded allocation.
      if (tc_globals.guardedpage_allocator().PointerIsMine(buf)) {
        volatile char sink = static_cast<char*>(buf)[access_offset];
        benchmark::DoNotOptimize(sink);
        MaybeResetStackTraceFilter(improved_coverage_enabled);
      }
    }
  };
  EXPECT_DEATH(RepeatUseAfterFree(3999, -42),
               ">>> Access at offset -42 into buffer of length 3999");
  if (kPageSize > 4096) {
    EXPECT_DEATH(RepeatUseAfterFree(6543, 1221),
                 ">>> Access at offset 1221 into buffer of length 6543");
    EXPECT_DEATH(RepeatUseAfterFree(8192, 8484),
                 ">>> Access at offset 8484 into buffer of length 8192");
  }
}

// Ensure non-GWP-ASan segfaults also crash.
TEST_P(ParameterizedTcMallocTest, NonGwpAsanSegv) {
  ScopedImprovedGuardedSampling scoped_improved_guarded_sampling(GetParam());
  int* volatile p = static_cast<int*>(
      mmap(nullptr, kPageSize, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
  EXPECT_DEATH((*p)++, "");
  munmap(p, kPageSize);
}

// Verify memory is aligned suitably according to compiler assumptions
// (b/201199449).
TEST_P(ParameterizedTcMallocTest, b201199449_AlignedObjectConstruction) {
  ScopedAlwaysSample always_sample;
  ScopedImprovedGuardedSampling scoped_improved_guarded_sampling(GetParam());

  struct A {
    char c[__STDCPP_DEFAULT_NEW_ALIGNMENT__ + 1];
  };

  bool allocated = false;
  for (int i = 0; i < 1000; i++) {
    auto a = std::make_unique<A>();
    benchmark::DoNotOptimize(a.get());

    // Verify alignment
    EXPECT_EQ(
        absl::bit_cast<uintptr_t>(a.get()) % __STDCPP_DEFAULT_NEW_ALIGNMENT__,
        0);

    if (tc_globals.guardedpage_allocator().PointerIsMine(a.get())) {
      allocated = true;
      break;
    }
  }

  EXPECT_TRUE(allocated) << "Failed to allocate with GWP-ASan";
}

TEST_P(ParameterizedTcMallocTest, DoubleFree) {
  ScopedGuardedSamplingRate gs(-1);
  ScopedProfileSamplingRate s(1);
  ScopedImprovedGuardedSampling scoped_improved_guarded_sampling(GetParam());
  auto DoubleFree = []() {
    void* buf = ::operator new(42);
    ::operator delete(buf);
    ::operator delete(buf);
  };
  EXPECT_DEATH(DoubleFree(),
               "span != "
               "nullptr|Span::Unsample\\(\\)|Span::IN_USE|"
               "InvokeHooksAndFreePages\\(\\)");
}

TEST_P(ParameterizedTcMallocTest, ReallocLarger) {
  // Note: sizes are chosen so that size + 2 access below
  // does not write out of actual allocation bounds.
  for (size_t size : {2, 29, 60, 505}) {
    EXPECT_DEATH(
        {
          fprintf(stderr, "size=%zu\n", size);
          ScopedAlwaysSample always_sample;
          ScopedImprovedGuardedSampling scoped_improved_guarded_sampling(
              GetParam());
          for (size_t i = 0; i < 10000; ++i) {
            char* volatile ptr = static_cast<char*>(malloc(size));
            ptr = static_cast<char*>(realloc(ptr, size + 1));
            ptr[size + 2] = 'A';
            free(ptr);
            MaybeResetStackTraceFilter(GetParam());
          }
        },
        "SIGSEGV");
  }
}

TEST_P(ParameterizedTcMallocTest, ReallocSmaller) {
#ifdef ABSL_HAVE_ADDRESS_SANITIZER
  GTEST_SKIP() << "ASan will trap ahead of us";
#endif
  for (size_t size : {8, 29, 60, 505}) {
    SCOPED_TRACE(absl::StrCat("size=", size));
    EXPECT_DEATH(
        {
          ScopedAlwaysSample always_sample;
          ScopedImprovedGuardedSampling scoped_improved_guarded_sampling(
              GetParam());
          for (size_t i = 0; i < 10000; ++i) {
            char* volatile ptr = static_cast<char*>(malloc(size));
            ptr = static_cast<char*>(realloc(ptr, size - 1));
            ptr[size - 1] = 'A';
            free(ptr);
            MaybeResetStackTraceFilter(GetParam());
          }
        },
        "SIGSEGV");
  }
}

TEST_P(ParameterizedTcMallocTest, ReallocUseAfterFree) {
#ifdef ABSL_HAVE_ADDRESS_SANITIZER
  GTEST_SKIP() << "ASan will trap ahead of us";
#endif
  for (size_t size : {8, 29, 60, 505}) {
    SCOPED_TRACE(absl::StrCat("size=", size));
    EXPECT_DEATH(
        {
          ScopedAlwaysSample always_sample;
          ScopedImprovedGuardedSampling scoped_improved_guarded_sampling(
              GetParam());
          for (size_t i = 0; i < 10000; ++i) {
            char* volatile old_ptr = static_cast<char*>(malloc(size));
            void* volatile new_ptr = realloc(old_ptr, size - 1);
            old_ptr[0] = 'A';
            free(new_ptr);
          }
        },
        "SIGSEGV");
  }
}

INSTANTIATE_TEST_SUITE_P(ptmt, ParameterizedTcMallocTest, testing::Bool());

}  // namespace
}  // namespace tcmalloc
