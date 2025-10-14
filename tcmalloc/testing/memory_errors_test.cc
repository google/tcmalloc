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
#include <optional>
#include <string>
#include <tuple>
#include <vector>

#include "benchmark/benchmark.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "absl/base/attributes.h"
#include "absl/base/casts.h"
#include "absl/base/optimization.h"
#include "absl/numeric/bits.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/string_view.h"
#include "tcmalloc/common.h"
#include "tcmalloc/guarded_allocations.h"
#include "tcmalloc/guarded_page_allocator.h"
#include "tcmalloc/internal/config.h"
#include "tcmalloc/internal/declarations.h"
#include "tcmalloc/internal/logging.h"
#include "tcmalloc/malloc_extension.h"
#include "tcmalloc/static_vars.h"
#include "tcmalloc/testing/testutil.h"

namespace tcmalloc {

using tcmalloc_internal::kPageShift;
using tcmalloc_internal::kPageSize;
using tcmalloc_internal::kSanitizerPresent;
using tcmalloc_internal::tc_globals;

class GuardedAllocAlignmentTest : public testing::Test, ScopedAlwaysSample {
 public:
  GuardedAllocAlignmentTest() {
    MallocExtension::ActivateGuardedSampling();
    tc_globals.guardedpage_allocator().Reset();
  }
};

namespace {

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
    ::operator delete(p, static_cast<std::align_val_t>(align));
  }
}

}  // namespace

// By placing this class in the tcmalloc namespace, it may call the private
// method StackTraceFilter::Reset as a friend.
class TcMallocTest : public testing::Test {
 protected:
  TcMallocTest() {
    // Start with clean state to avoid hash collisions.
    tc_globals.guardedpage_allocator().Reset();
    MallocExtension::SetGuardedSamplingInterval(
        10 * MallocExtension::GetProfileSamplingInterval());

    // Prevent SEGV handler from writing XML properties in death tests.
    unsetenv("XML_OUTPUT_FILE");
  }
};

namespace {

class ReadWriteTcMallocTest
    : public TcMallocTest,
      public testing::WithParamInterface<bool /* write_test */> {};

TEST_P(ReadWriteTcMallocTest, UnderflowDetected) {
#if ABSL_HAVE_ADDRESS_SANITIZER
  GTEST_SKIP() << "Test requires GWP-ASan";
#endif  // ABSL_HAVE_ADDRESS_SANITIZER

  const bool write_test = GetParam();
  auto RepeatUnderflow = [&]() {
    for (int i = 0; i < 1000000; i++) {
      auto buf = std::make_unique<char[]>(kPageSize / 2);
      volatile char* buf_minus_1 = buf.get() - 1;
      benchmark::DoNotOptimize(buf_minus_1);
      // TCMalloc may crash without a GWP-ASan report if we underflow a regular
      // allocation.  Make sure we have a guarded allocation.
      if (tc_globals.guardedpage_allocator().PointerIsMine(buf.get())) {
        if (write_test) {
          *buf_minus_1 = 'A';
        } else {
          volatile char sink = *buf_minus_1;
          benchmark::DoNotOptimize(sink);
        }
      }
    }
  };
  std::string expected_output = absl::StrCat(
      "Buffer underflow ",
#if !defined(__riscv)
      write_test ? "\\(write\\)" : "\\(read\\)",
#else
      "\\(read or write: indeterminate\\)",
#endif
      " occurs in thread [0-9]+ at"
  );
  EXPECT_DEATH(RepeatUnderflow(), expected_output);
}

TEST_P(ReadWriteTcMallocTest, OverflowDetected) {
#if ABSL_HAVE_ADDRESS_SANITIZER
  GTEST_SKIP() << "Test requires GWP-ASan";
#endif  // ABSL_HAVE_ADDRESS_SANITIZER

  const bool write_test = GetParam();
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
      }
    }
  };
  std::string expected_output = absl::StrCat(
      "Buffer overflow ",
#if !defined(__riscv)
      write_test ? "\\(write\\)" : "\\(read\\)",
#else
      "\\(read or write: indeterminate\\)",
#endif
      " occurs in thread [0-9]+ at"
  );
  EXPECT_DEATH(RepeatOverflow(), expected_output);
}

TEST_P(ReadWriteTcMallocTest, ZeroByteAccessDetected) {
#if ABSL_HAVE_ADDRESS_SANITIZER
  GTEST_SKIP() << "Test requires GWP-ASan";
#endif  // ABSL_HAVE_ADDRESS_SANITIZER

  const bool write_test = GetParam();
  auto RepeatZeroByteAccess = [&]() {
    for (int i = 0; i < 100000000; i++) {
      auto buf = std::make_unique<char[]>(0);
      benchmark::DoNotOptimize(buf);

      // Ensure it's a GWP-ASan guarded allocation.
      const auto& gpa = tc_globals.guardedpage_allocator();
      if (gpa.PointerIsMine(buf.get())) {
        // Ensure it's a zero-byte allocation.
        TC_CHECK_EQ(gpa.GetRequestedSize(buf.get()), 0);
        if (write_test) {
          buf[123] = 'A';
          benchmark::DoNotOptimize(buf[123]);
        } else {
          volatile char sink = buf[123];
          benchmark::DoNotOptimize(sink);
        }
      }
    }
  };
  std::string expected_output = absl::StrCat(
      // For a zero-byte allocation, any access is an overflow of that 0-byte
      // region.
      "Buffer overflow ",
#if !defined(__riscv)
      write_test ? "\\(write\\)" : "\\(read\\)",
#else
      "\\(read or write: indeterminate\\)",
#endif
      " occurs in thread [0-9]+ at"
  );
  EXPECT_DEATH(RepeatZeroByteAccess(), expected_output);
}

TEST_P(ReadWriteTcMallocTest, UseAfterFreeDetected) {
  const bool write_test = GetParam();
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
  std::string expected_output = absl::StrCat(
      "Use-after-free ",
#if !defined(__riscv)
      write_test ? "\\(write\\)" : "\\(read\\)",
#else
      "\\(read or write: indeterminate\\)",
#endif
      " occurs in thread [0-9]+ at"
      "|heap-use-after-free");
  EXPECT_DEATH(RepeatUseAfterFree(), expected_output);
}

INSTANTIATE_TEST_SUITE_P(rwtmt, ReadWriteTcMallocTest, testing::Bool());

// Double free triggers an ASSERT within TCMalloc in non-opt builds.  So only
// run this test for opt builds.
#ifdef NDEBUG
TEST_F(TcMallocTest, DoubleFreeDetected) {
#if ABSL_HAVE_ADDRESS_SANITIZER
  GTEST_SKIP() << "Test requires GWP-ASan";
#endif

  auto RepeatDoubleFree = []() {
    for (int i = 0; i < 1000000; i++) {
      void* buf = ::operator new(kPageSize);
      ::operator delete(buf);
      benchmark::DoNotOptimize(buf);
      // TCMalloc often SEGVs on double free (without GWP-ASan report). Make
      // sure we have a guarded allocation before double-freeing.
      if (tc_globals.guardedpage_allocator().PointerIsMine(buf)) {
        ::operator delete(buf);
      }
    }
  };
  std::string expected_output =
      absl::StrCat("Double free occurs in thread [0-9]+ at"
      );
  EXPECT_DEATH(RepeatDoubleFree(), expected_output);
}
#endif

TEST_F(TcMallocTest, OverflowWriteDetectedAtFree) {
  auto RepeatOverflowWrite = [&]() {
    for (int i = 0; i < 1000000; i++) {
      // Make buffer smaller than kPageSize to test detection-at-free of write
      // overflows.
      constexpr size_t kSize = kPageSize - 1;
      auto sink_buf = std::make_unique<char[]>(kSize);
      benchmark::DoNotOptimize(sink_buf);
      sink_buf[kSize] = '\0';
      benchmark::DoNotOptimize(sink_buf[kSize]);
    }
  };
  std::string expected_output = absl::StrCat(
      "Buffer overflow \\(write\\) detected in thread [0-9]+ at free"
      "|heap-buffer-overflow");
  EXPECT_DEATH(RepeatOverflowWrite(), expected_output);
}

TEST_F(TcMallocTest, ReallocNoFalsePositive) {
  for (int i = 0; i < 1000000; i++) {
    auto sink_buf = reinterpret_cast<char*>(malloc(kPageSize - 1));
    benchmark::DoNotOptimize(sink_buf);
    sink_buf = reinterpret_cast<char*>(realloc(sink_buf, kPageSize));
    sink_buf[kPageSize - 1] = '\0';
    benchmark::DoNotOptimize(sink_buf);
    free(sink_buf);
  }
}

TEST_F(TcMallocTest, OffsetAndLength) {
#if ABSL_HAVE_ADDRESS_SANITIZER
  GTEST_SKIP() << "Test requires GWP-ASan";
#endif  // ABSL_HAVE_ADDRESS_SANITIZER

  auto RepeatUseAfterFree = [&](size_t buffer_len, off_t access_offset) {
    for (int i = 0; i < 1000000; i++) {
      void* buf = ::operator new(buffer_len);
      ::operator delete(buf);
      // TCMalloc may crash without a GWP-ASan report if we overflow a regular
      // allocation.  Make sure we have a guarded allocation.
      benchmark::DoNotOptimize(buf);
      if (tc_globals.guardedpage_allocator().PointerIsMine(buf)) {
        volatile char sink = static_cast<char*>(buf)[access_offset];
        benchmark::DoNotOptimize(sink);
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
    EXPECT_DEATH(RepeatUseAfterFree(4096, 4096),
                 ">>> Access at offset 4096 into buffer of length 4096");
  }
}

// Ensure non-GWP-ASan segfaults also crash.
TEST_F(TcMallocTest, NonGwpAsanSegv) {
  int* volatile p = static_cast<int*>(
      mmap(nullptr, kPageSize, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
  EXPECT_DEATH((*p)++, "");
  munmap(p, kPageSize);
}

// Verify memory is aligned suitably according to compiler assumptions
// (b/201199449).
TEST_F(TcMallocTest, b201199449_AlignedObjectConstruction) {
  ScopedAlwaysSample always_sample;

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

#if !ABSL_HAVE_ADDRESS_SANITIZER
  EXPECT_TRUE(allocated) << "Failed to allocate with GWP-ASan";
#endif  // !ABSL_HAVE_ADDRESS_SANITIZER
}

TEST_F(TcMallocTest, DoubleFree) {
  ScopedGuardedSamplingInterval gs(-1);
  ScopedProfileSamplingInterval s(1);
  auto DoubleFree = []() {
    void* buf = ::operator new(42);
    benchmark::DoNotOptimize(buf);
    ::operator delete(buf);
    benchmark::DoNotOptimize(buf);
    ::operator delete(buf);
  };
  EXPECT_DEATH(
      DoubleFree(),
      absl::StrCat("(Possible double free detected"
                   ")|attempting double-free"));
}

TEST_F(TcMallocTest, LargeDoubleFree) {
  ScopedGuardedSamplingInterval gs(-1);
  ScopedProfileSamplingInterval s(1);
  auto DoubleFree = []() {
    void* buf = ::operator new(tcmalloc_internal::kMaxSize + 1);
    benchmark::DoNotOptimize(buf);
    ::operator delete(buf);
    benchmark::DoNotOptimize(buf);
    ::operator delete(buf);
  };
  EXPECT_DEATH(
      DoubleFree(),
      absl::StrCat("(Possible double free detected"
                   ")|attempting double-free"));
}

TEST_F(TcMallocTest, ReallocLarger) {
  // Note: sizes are chosen so that size + 2 access below
  // does not write out of actual allocation bounds.
  for (size_t size : {2, 29, 60, 505}) {
    EXPECT_DEATH(
        {
          fprintf(stderr, "size=%zu\n", size);
          ScopedAlwaysSample always_sample;
          for (size_t i = 0; i < 10000; ++i) {
            char* volatile ptr = static_cast<char*>(malloc(size));
            ptr = static_cast<char*>(realloc(ptr, size + 1));
            ptr[size + 2] = 'A';
            free(ptr);
          }
        },
        "has detected a memory error|heap-buffer-overflow");
  }
}

TEST_F(TcMallocTest, ReallocSmaller) {
#ifdef ABSL_HAVE_ADDRESS_SANITIZER
  GTEST_SKIP() << "ASan will trap ahead of us";
#endif
  for (size_t size : {8, 29, 60, 505}) {
    SCOPED_TRACE(absl::StrCat("size=", size));
    EXPECT_DEATH(
        {
          ScopedAlwaysSample always_sample;
          for (size_t i = 0; i < 10000; ++i) {
            char* volatile ptr = static_cast<char*>(malloc(size));
            ptr = static_cast<char*>(realloc(ptr, size - 1));
            ptr[size - 1] = 'A';
            free(ptr);
          }
        },
        "has detected a memory error");
  }
}

TEST_F(TcMallocTest, ReallocUseAfterFree) {
#ifdef ABSL_HAVE_ADDRESS_SANITIZER
  GTEST_SKIP() << "ASan will trap ahead of us";
#endif
  for (size_t size : {8, 29, 60, 505}) {
    SCOPED_TRACE(absl::StrCat("size=", size));
    EXPECT_DEATH(
        {
          ScopedAlwaysSample always_sample;
          for (size_t i = 0; i < 10000; ++i) {
            char* volatile old_ptr = static_cast<char*>(malloc(size));
            void* volatile new_ptr = realloc(old_ptr, size - 1);
            old_ptr[0] = 'A';
            free(new_ptr);
          }
        },
        "has detected a memory error");
  }
}

TEST_F(TcMallocTest, MismatchedSampled) {
#ifdef ABSL_HAVE_ADDRESS_SANITIZER
  GTEST_SKIP() << "ASan will trap ahead of us";
#endif

  // Sampled allocations should be able to provide a richer error (precise
  // size/allocation stack) when there is a mismatch in provided sizes.

  constexpr size_t kSizes[] = {0u,
                               8u,
                               64u,
                               tcmalloc_internal::kPageSize,
                               tcmalloc_internal::kMaxSize - 1u,
                               tcmalloc_internal::kMaxSize,
                               tcmalloc_internal::kMaxSize + 1u,
                               tcmalloc_internal::kHugePageSize - 1u,
                               tcmalloc_internal::kHugePageSize,
                               tcmalloc_internal::kHugePageSize + 1u};
  for (const size_t size : kSizes) {
    const size_t likely_size = MallocExtension::GetEstimatedAllocatedSize(size);
    SCOPED_TRACE(absl::StrCat("size=", size));

    // Hot-cold doesn't affect the sizes directly, stress our size classes.
    constexpr std::optional<hot_cold_t> hot_cold_set[] = {
        std::nullopt,
        hot_cold_t{0},
        hot_cold_t{128},
        hot_cold_t{255},
    };

    for (const auto hot_cold : hot_cold_set) {
      for (const bool size_returning : {true, false}) {
        SCOPED_TRACE(absl::StrCat("size_returning=", size_returning));

        std::string size_range;
        if (size_returning && size != likely_size) {
          size_range = absl::StrCat(size, " - ", likely_size);
        } else {
          size_range = absl::StrCat(size);
        }

        EXPECT_DEATH(
            {
              ScopedAlwaysSample always_sample;
              ScopedGuardedSamplingInterval gs(-1);

              sized_ptr_t r;
              if (hot_cold.has_value()) {
                if (size_returning) {
                  r = __size_returning_new_hot_cold(size, *hot_cold);
                } else {
                  r.p = ::operator new(size, *hot_cold);
                  r.n = size;
                }
              } else {
                if (size_returning) {
                  r = __size_returning_new(size);
                } else {
                  r.p = ::operator new(size);
                  r.n = size;
                }
              }

              ::operator delete(r.p, r.n + 1);
            },
            absl::StrCat(
                "(Mismatched-size-delete.*mismatched-sized-delete.md.*of .* "
                "bytes \\(expected ",
                size_range,
                " bytes"
                ")"
#ifndef NDEBUG
                "|(Mismatched-size-delete.*of [0-9]+ bytes \\(expected between "
                "\\[[0-9]+, [0-9]+\\] bytes\\)"
                ")"
#endif
                ));
      }
    }
  }
}

TEST_F(TcMallocTest, MismatchedDeleteTooLarge) {
#ifdef ABSL_HAVE_ADDRESS_SANITIZER
  GTEST_SKIP() << "ASan will trap ahead of us";
#endif

  auto RoundUp = [](size_t size) {
    // We only do our checks on "large" (>kMaxSize) deallocations.
    size_t r = size + tcmalloc_internal::kMaxSize + 1u;
    // We use multiple pages for spans of kPageSize/kMaxSize in small-but-slow.
    // Adjust the threshold needed to ensure we're larger than the span.
    if (tcmalloc_internal::kPageShift == 12) {
      r += 64 << 10;
    }
    return r;
  };

  constexpr size_t kSizes[] = {0u,
                               8u,
                               64u,
                               tcmalloc_internal::kPageSize,
                               tcmalloc_internal::kMaxSize - 1u,
                               tcmalloc_internal::kMaxSize,
                               tcmalloc_internal::kMaxSize + 1u,
                               tcmalloc_internal::kHugePageSize - 1u,
                               tcmalloc_internal::kHugePageSize,
                               tcmalloc_internal::kHugePageSize + 1u};
  for (const size_t size : kSizes) {
    const size_t likely_size = MallocExtension::GetEstimatedAllocatedSize(size);
    SCOPED_TRACE(absl::StrCat("size=", size));

    // Hot-cold doesn't affect the sizes directly, but since the cold size
    // classes immediately follow the hot ones, it lets us stress our fixups to
    // the minimum size class.
    constexpr std::optional<hot_cold_t> hot_cold_set[] = {
        std::nullopt,
        hot_cold_t{0},
        hot_cold_t{128},
        hot_cold_t{255},
    };

    for (const auto hot_cold : hot_cold_set) {
      for (const bool size_returning : {true, false}) {
        SCOPED_TRACE(absl::StrCat("size_returning=", size_returning));
        EXPECT_DEATH(
            {
              sized_ptr_t r;
              if (hot_cold.has_value()) {
                if (size_returning) {
                  r = __size_returning_new_hot_cold(size, *hot_cold);
                } else {
                  r.p = ::operator new(size, *hot_cold);
                  r.n = size;
                }
              } else {
                if (size_returning) {
                  r = __size_returning_new(size);
                } else {
                  r.p = ::operator new(size);
                  r.n = size;
                }
              }

              ::operator delete(r.p, RoundUp(r.n));
            },
            absl::StrCat(
                "(Mismatched-size-delete.*mismatched-sized-delete.md.*of .* "
                "bytes \\(expected.* (",
                size, "|", likely_size,
                ").*bytes"
                ")"
#ifndef NDEBUG
                "|(Mismatched-size-delete.*of [0-9]+ bytes \\(expected between "
                "\\[[0-9]+, [0-9]+\\] bytes\\)"
                ")"
#endif
                ));
      }
    }
  }
}

TEST_F(TcMallocTest, MismatchedDeleteTooSmall) {
#ifdef ABSL_HAVE_ADDRESS_SANITIZER
  GTEST_SKIP() << "ASan will trap ahead of us";
#endif

  using tcmalloc_internal::kHugePageSize;
  using tcmalloc_internal::kMaxSize;
  using tcmalloc_internal::kPageSize;

  constexpr size_t kSizes[] = {kHugePageSize - 1u, kHugePageSize,
                               kHugePageSize + 1u};
  for (size_t size : kSizes) {
    SCOPED_TRACE(absl::StrCat("size=", size));
    const size_t likely_size = MallocExtension::GetEstimatedAllocatedSize(size);

    for (bool size_returning : {true, false}) {
      SCOPED_TRACE(absl::StrCat("size_returning=", size_returning));
      EXPECT_DEATH(
          {
            sized_ptr_t r;
            if (size_returning) {
              r = __size_returning_new(size);
            } else {
              r.p = ::operator new(size);
              r.n = size;
            }

            ::operator delete(r.p, std::max(kMaxSize, r.n - kPageSize));
          },
          absl::StrCat(
              "(Mismatched-size-delete.*mismatched-sized-delete.md.*of"
              ")"
#ifndef NDEBUG
              "|(Mismatched-size-delete.*of [0-9]+ bytes \\(expected between "
              "\\[[0-9]+, [0-9]+\\] bytes\\)"
              ")"
#endif
              ));
    }
  }
}

TEST_F(TcMallocTest, MismatchedSizeClassInFreelistInsertion) {
#ifdef ABSL_HAVE_ADDRESS_SANITIZER
  GTEST_SKIP() << "ASan will trap ahead of us";
#endif
  // Ensure GWP-ASAN doesn't catch the issue before we do, so that we validate
  // the checks during freelist insertion.
  ScopedNeverSample never_sample;

  std::vector<void*> ptrs;
  // Allocate enough enough to fill the CPU cache.
  for (int i = 0; i < 10000; ++i) {
    ptrs.push_back(::operator new(16));
  }
  EXPECT_DEATH(
      {
        // Delete the objects with the wrong size.
        for (void* ptr : ptrs) {
          ::operator delete(ptr, 5);
        }
      },
      absl::StrCat(
          "(Mismatched-size-class.*size argument in the range \\[1, "
          "8\\].*allocations with sizes \\[9, 16\\]"
          ")|"
#ifndef NDEBUG
          "(Mismatched-size-delete.*of 5 bytes \\(expected between \\[0, 16\\] "
          "bytes\\)"
          ")|"
#endif
          "alloc-dealloc-mismatch"));
}

TEST_F(TcMallocTest, DoubleFreeInFreelistInsertion) {
#ifdef ABSL_HAVE_ADDRESS_SANITIZER
  GTEST_SKIP() << "ASan will trap ahead of us";
#endif
#ifndef NDEBUG
  GTEST_SKIP() << "CorrectSize will trap ahead of us";
#endif
  // Ensure GWP-ASAN doesn't catch the issue before we do, so that we validate
  // the checks during freelist insertion.
  ScopedNeverSample never_sample;
  const size_t kSize = tcmalloc_internal::kPageSize / 2;

  std::vector<void*> ptrs;
  // Allocate enough enough to fill the CPU cache.
  for (int i = 0; i < 10000; ++i) {
    ptrs.push_back(::operator new(kSize));
  }
  EXPECT_DEATH(
      {
        // Delete the objects.
        for (void* ptr : ptrs) {
          ::operator delete(ptr, kSize);
        }
        // Now double-free.
        for (void* ptr : ptrs) {
          ::operator delete(ptr, kSize);
        }
      },
      absl::StrCat(
          "(CHECK in ReportDoubleFree: Possible double free detected of "
          ")"));
}

TEST_F(TcMallocTest, CorruptedPointer) {
  constexpr size_t kSizes[] = {
      8u,
      64u,
      1024u,
      tcmalloc_internal::kPageSize,
      tcmalloc_internal::kMaxSize,
      tcmalloc_internal::kMaxSize + 1,
      tcmalloc_internal::kHugePageSize,
  };

  constexpr size_t kMisalignment[] = {
      1,
      2,
      3,
      4,
      static_cast<size_t>(tcmalloc_internal::kAlignment) - 1,
#ifdef NDEBUG
      ABSL_CACHELINE_SIZE,
#ifndef ABSL_HAVE_ADDRESS_SANITIZER
      tcmalloc_internal::kPageSize / 2,
#endif  // ABSL_HAVE_ADDRESS_SANITIZER
#endif  // NDEBUG
  };

  constexpr std::optional<tcmalloc::hot_cold_t> kAccessDensities[] = {
      std::nullopt,
      tcmalloc::hot_cold_t{0},
      tcmalloc::hot_cold_t{255},
  };

  for (const bool sampled : {true
#ifndef NDEBUG
                             ,
                             false
#endif
       }) {
    SCOPED_TRACE(absl::StrCat("sampled=", sampled));

    for (const bool guarded : {true, false}) {
      SCOPED_TRACE(absl::StrCat("guarded=", guarded));
      if (!sampled && guarded) {
        continue;
      }

      for (const size_t size : kSizes) {
        for (const size_t misalignment : kMisalignment) {
          if (misalignment >= size) {
            continue;
          }

          for (const auto hot_cold : kAccessDensities) {
            SCOPED_TRACE(absl::StrCat(
                "size=", size, ",misalignment=", misalignment, ",hot_cold=",
                !hot_cold.has_value() ? "nullopt" : absl::StrCat(*hot_cold)));
            EXPECT_DEATH(
                {
                  ScopedProfileSamplingInterval sampling(sampled ? 1 : 0);
                  ScopedGuardedSamplingInterval gs(guarded ? 0 : -1);

                  for (size_t i = 0; i < 10000; ++i) {
                    // TODO(b/404341539): Cover aligned operator new.
                    char* ptr;
                    if (hot_cold.has_value()) {
                      ptr = static_cast<char*>(::operator new(size, *hot_cold));
                    } else {
                      ptr = static_cast<char*>(::operator new(size));
                    }
                    sized_delete(ptr + misalignment, size);
                  }
                },
                absl::StrCat(
                    "attempting free on address which was not "
                    "malloc|alloc-dealloc-mismatch.*INVALID|"
                    "(Attempted to free corrupted pointer"
                    ")"));
          }
        }
      }
    }
  }
}

TEST_F(TcMallocTest, CorruptedPointerFixed) {
#ifdef ABSL_HAVE_ADDRESS_SANITIZER
  GTEST_SKIP() << "Skipped under sanitizers";
#endif
#ifndef NDEBUG
  GTEST_SKIP() << "Skipping with debug assertions";
#endif

  constexpr size_t kSizes[] = {
      8u, 64u, 1024u, tcmalloc_internal::kPageSize, tcmalloc_internal::kMaxSize,
  };

  constexpr uintptr_t kAlignmentMask =
      ~(static_cast<uintptr_t>(tcmalloc_internal::kAlignment) - 1u);

  constexpr size_t kMisalignment[] = {
      1, 2, 3, 4, static_cast<size_t>(tcmalloc_internal::kAlignment) - 1u,
  };

  // Sampled deallocations will detect misalignment.
  ScopedNeverSample never_sample;

  int same_pointers = 0;
  for (const size_t size : kSizes) {
    for (const size_t misalignment : kMisalignment) {
      ASSERT_EQ(misalignment & kAlignmentMask, 0);
      char* p = static_cast<char*>(::operator new(size));
      const uintptr_t pu = absl::bit_cast<uintptr_t>(p);
      sized_delete(p + misalignment, size);

      char* q = static_cast<char*>(::operator new(size));
      const uintptr_t qu = absl::bit_cast<uintptr_t>(q);
      sized_delete(q, size);

      // We generally expect back to back deallocation-allocation to produce the
      // same pointer, but we might end up sampling in between, migrating across
      // cores, etc.
      if ((pu & kAlignmentMask) == (qu & kAlignmentMask)) {
        same_pointers++;
        EXPECT_EQ(pu, qu);
      }
    }
  }

  // We should have gotten some back to back allocations.
  EXPECT_GT(same_pointers, 0);
}

TEST_F(TcMallocTest, AllocationDeallocationConfusion) {
  ScopedAlwaysSample always_sample;

  EXPECT_DEATH(
      {
        void* p = malloc(1);
        benchmark::DoNotOptimize(p);
        ::operator delete(p);
      },
      absl::StrCat(
          "(alloc-dealloc-mismatch \\(malloc vs operator delete\\))|"
          "(Deallocating 0x[0-9a-f]+ with new, expected malloc"
          ")"));

  EXPECT_DEATH(
      {
        void* p = aligned_alloc(1, 1);
        benchmark::DoNotOptimize(p);
        ::operator delete(p);
      },
      absl::StrCat(
          "(alloc-dealloc-mismatch \\(malloc vs operator delete\\))|"
          "(Deallocating 0x[0-9a-f]+ with new, expected aligned-malloc"
          ")"));

  EXPECT_DEATH(
      {
        void* p = ::operator new(1);
        benchmark::DoNotOptimize(p);
        free(p);
      },
      absl::StrCat(
          "(alloc-dealloc-mismatch \\(operator new vs free\\))|"
          "(Deallocating 0x[0-9a-f]+ with malloc, expected new"
          ")"));
}

TEST_F(TcMallocTest, DeleteWithoutAlignment) {
  ScopedAlwaysSample always_sample;

  constexpr size_t size = ABSL_CACHELINE_SIZE;
  constexpr std::align_val_t align{ABSL_CACHELINE_SIZE};
  void* newed_with_align = ::operator new(size, align);

  // TODO(b/404341539): Change ??? to <nil> or similar.
  EXPECT_DEATH(
      { ::operator delete(newed_with_align); },
      absl::StrCat(
          // ASan
          "(new-delete-type-mismatch on .*\\n(?:.*\\n)*.*alignment of the "
          "allocated type:   ",
          align,
          " bytes;.*\\n(?:.*\\n)*.*alignment of the deallocated type: "
          "default-aligned)|"
          // GWP-ASan
          "(Deallocating 0x[0-9a-f]+ with alignment "
          "\\?\\?\\?, expected ",
          align,
          ")"));

  ::operator delete(newed_with_align, align);
}

TEST_F(TcMallocTest, DeleteWithAlignment) {
  ScopedAlwaysSample always_sample;

  constexpr size_t size = ABSL_CACHELINE_SIZE;
  constexpr std::align_val_t align{__STDCPP_DEFAULT_NEW_ALIGNMENT__};
  void* newed_without_align = ::operator new(size);

  EXPECT_DEATH(
      { ::operator delete(newed_without_align, align); },
      absl::StrCat(
          // ASan
          "(new-delete-type-mismatch on "
          ".*\\n(?:.*\\n)*.*alignment of the allocated type:   "
          "default-aligned;"
          ".*\\n(?:.*\\n)*.*alignment of the deallocated type: ",
          align, " bytes)|",
          // GWP-ASan
          "(Deallocating 0x[0-9a-f]+ with alignment ", align,
          ", expected \\?\\?\\?"
          ")"));

  ::operator delete(newed_without_align);
}

TEST_F(TcMallocTest, DeleteWrongAlignment) {
  ScopedAlwaysSample always_sample;

  constexpr size_t size = ABSL_CACHELINE_SIZE;
  constexpr std::align_val_t align{ABSL_CACHELINE_SIZE};
  void* newed_with_align = ::operator new(size, align);

  EXPECT_DEATH(
      {
        ::operator delete(newed_with_align,
                          std::align_val_t{ABSL_CACHELINE_SIZE / 2});
      },
      // ASan
      absl::StrCat(
          "(new-delete-type-mismatch on .*\\n(?:.*\\n)*.*alignment of the "
          "allocated type:   ",
          align, " bytes;.*\\n(?:.*\\n)*.*alignment of the deallocated type: ",
          ABSL_CACHELINE_SIZE / 2,
          " bytes)|"
          // GWP-ASan
          "(Deallocating 0x[0-9a-f]+ with alignment ",
          ABSL_CACHELINE_SIZE / 2, ", expected ", align,
          ")"));

  ::operator delete(newed_with_align, align);
}

TEST_F(TcMallocTest, FreeWithoutAlignment) {
  if (kSanitizerPresent) {
    // TODO(b/441095602): Remove this skip.
    GTEST_SKIP() << "Skipping under sanitizers pending "
                    "https://github.com/llvm/llvm-project/issues/144435";
  }
  ScopedAlwaysSample always_sample;

  constexpr size_t size = ABSL_CACHELINE_SIZE;
  constexpr size_t align = ABSL_CACHELINE_SIZE;
  void* malloc_with_align = aligned_alloc(align, size);

  EXPECT_DEATH(
      { free_sized(malloc_with_align, size); },
      // GWP-ASan
      absl::StrCat(
          "(Deallocating 0x[0-9a-f]+ with alignment \\?\\?\\?, expected ",
          align,
          ")"));

  free_aligned_sized(malloc_with_align, align, size);
}

TEST_F(TcMallocTest, FreeWithSameAlignment) {
  if (kSanitizerPresent) {
    GTEST_SKIP() << "Skipping under sanitizers pending "
                    "https://github.com/llvm/llvm-project/issues/144435";
  }
  ScopedAlwaysSample always_sample;

  constexpr size_t size = ABSL_CACHELINE_SIZE;
  constexpr size_t align = alignof(std::max_align_t);
  void* malloc_with_align = aligned_alloc(align, size);
  void* malloc_without_align = malloc(size);

  // TODO(b/404341539): Distinguish this case and require free_align_sized.
  free_sized(malloc_with_align, size);
  free_aligned_sized(malloc_without_align, alignof(std::max_align_t), size);
}

TEST_F(TcMallocTest, FreeWithAlignment) {
  if (kSanitizerPresent) {
    // TODO(b/441095602): Remove this skip.
    GTEST_SKIP() << "Skipping under sanitizers pending "
                    "https://github.com/llvm/llvm-project/issues/144435";
  }
  ScopedAlwaysSample always_sample;

  constexpr size_t size = ABSL_CACHELINE_SIZE;
  constexpr size_t align = ABSL_CACHELINE_SIZE;
  void* malloc_without_align = malloc(size);

  EXPECT_DEATH(
      { free_aligned_sized(malloc_without_align, align, size); },
      // GWP-ASan
      absl::StrCat(
          "(Deallocating 0x[0-9a-f]+ with alignment ", align, ", expected ",
          alignof(std::max_align_t),
          ")"));

  free_sized(malloc_without_align, size);
}

}  // namespace
}  // namespace tcmalloc
