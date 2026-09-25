// Copyright 2025 The TCMalloc Authors
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

#include <malloc.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <new>

#include "gtest/gtest.h"
#include "fuzztest/fuzztest.h"
#include "fuzztest/init_fuzztest.h"
#include "absl/numeric/bits.h"
#include "tcmalloc/common.h"
#include "tcmalloc/internal/config.h"
#include "tcmalloc/internal/logging.h"
#include "tcmalloc/tcmalloc.h"

namespace tcmalloc::tcmalloc_internal {
namespace {

inline constexpr size_t kFuzzingMaxSize = kMaxSize * 2 + 1;

size_t GetPageSizeLog2() { return kHugePageShift; }

size_t PickDeleteSize(size_t min, size_t max, double scale) {
  TC_ASSERT_LE(min, max);
  TC_ASSERT_GE(max, min);
  TC_ASSERT(scale >= 0.0 && scale <= 1.0);
  const size_t result =
      min + static_cast<size_t>(static_cast<double>(max - min) * scale);
  TC_ASSERT(result >= min && result <= max);
  return result;
}

void MallocFreeSized(size_t size) {
  void* ptr = TCMallocInternalMalloc(size);
  ASSERT_TRUE(ptr != nullptr);
  TCMallocInternalFreeSized(ptr, size);
}

FUZZ_TEST(TCMalloc, MallocFreeSized)
    .WithDomains(fuzztest::InRange(size_t{0}, kFuzzingMaxSize));

void MallocReallocFreeSized(size_t a, size_t b) {
  void* ptr = TCMallocInternalMalloc(a);
  ASSERT_TRUE(ptr != nullptr);
  ptr = TCMallocInternalRealloc(ptr, b);
  ASSERT_TRUE(b == 0 || ptr != nullptr);
  TCMallocInternalFreeSized(ptr, b);
}

FUZZ_TEST(TCMalloc, MallocReallocFreeSized)
    .WithDomains(fuzztest::InRange(size_t{0}, kFuzzingMaxSize),
                 fuzztest::InRange(size_t{0}, kFuzzingMaxSize));

void AlignedAllocFreeAlignedSized(size_t alignment_log2, size_t size) {
  const size_t alignment = size_t{1} << static_cast<int>(alignment_log2);
  void* ptr = TCMallocInternalAlignedAlloc(alignment, size);
  ASSERT_TRUE(ptr != nullptr);
  TCMallocInternalFreeAlignedSized(ptr, alignment, size);
}

FUZZ_TEST(TCMalloc, AlignedAllocFreeAlignedSized)
    .WithDomains(fuzztest::InRange(size_t{0}, GetPageSizeLog2()),
                 fuzztest::InRange(size_t{0}, kFuzzingMaxSize));

void AllocAtLeastFreeAllocatedSize(size_t size) {
  const auto result = TCMallocInternalAllocAtLeast(size);
  ASSERT_TRUE(result.ptr != nullptr);
  TCMallocInternalFreeSized(result.ptr, result.size);
}

FUZZ_TEST(TCMalloc, AllocAtLeastFreeAllocatedSize)
    .WithDomains(fuzztest::InRange(size_t{0}, kFuzzingMaxSize));

void AllocAtLeastFreeRequestedSize(size_t size) {
  const auto result = TCMallocInternalAllocAtLeast(size);
  ASSERT_TRUE(result.ptr != nullptr);
  TCMallocInternalFreeSized(result.ptr, size);
}

FUZZ_TEST(TCMalloc, AllocAtLeastFreeRequestedSize)
    .WithDomains(fuzztest::InRange(size_t{0}, kFuzzingMaxSize));

void AllocAtLeastFreeSized(size_t size, double delete_size_scale) {
  const auto result = TCMallocInternalAllocAtLeast(size);
  ASSERT_TRUE(result.ptr != nullptr);
  TCMallocInternalFreeSized(
      result.ptr, PickDeleteSize(size, result.size, delete_size_scale));
}

FUZZ_TEST(TCMalloc, AllocAtLeastFreeSized)
    .WithDomains(fuzztest::InRange(size_t{0}, kFuzzingMaxSize),
                 fuzztest::InRange(0.0, 1.0));

void AlignedAllocAtLeastFreeAllocatedSizeAligned(size_t alignment_log2,
                                                 size_t size) {
  const size_t alignment = size_t{1} << static_cast<int>(alignment_log2);
  const auto result = TCMallocInternalAlignedAllocAtLeast(alignment, size);
  ASSERT_TRUE(result.ptr != nullptr);
  TCMallocInternalFreeAlignedSized(result.ptr, alignment, result.size);
}

FUZZ_TEST(TCMalloc, AlignedAllocAtLeastFreeAllocatedSizeAligned)
    .WithDomains(fuzztest::InRange(size_t{0}, GetPageSizeLog2()),
                 fuzztest::InRange(size_t{0}, kFuzzingMaxSize));

void AlignedAllocAtLeastFreeRequestedSizeAligned(size_t alignment_log2,
                                                 size_t size) {
  const size_t alignment = size_t{1} << static_cast<int>(alignment_log2);
  const auto result = TCMallocInternalAlignedAllocAtLeast(alignment, size);
  ASSERT_TRUE(result.ptr != nullptr);
  TCMallocInternalFreeAlignedSized(result.ptr, alignment, size);
}

FUZZ_TEST(TCMalloc, AlignedAllocAtLeastFreeRequestedSizeAligned)
    .WithDomains(fuzztest::InRange(size_t{0}, GetPageSizeLog2()),
                 fuzztest::InRange(size_t{0}, kFuzzingMaxSize));

void AlignedAllocAtLeastFreeSizedAligned(size_t alignment_log2, size_t size,
                                         double delete_size_scale) {
  const size_t alignment = size_t{1} << static_cast<int>(alignment_log2);
  const auto result = TCMallocInternalAlignedAllocAtLeast(alignment, size);
  ASSERT_TRUE(result.ptr != nullptr);
  TCMallocInternalFreeAlignedSized(
      result.ptr, alignment,
      PickDeleteSize(size, result.size, delete_size_scale));
}

FUZZ_TEST(TCMalloc, AlignedAllocAtLeastFreeSizedAligned)
    .WithDomains(fuzztest::InRange(size_t{0}, GetPageSizeLog2()),
                 fuzztest::InRange(size_t{0}, kFuzzingMaxSize),
                 fuzztest::InRange(0.0, 1.0));

void NewSizedDelete(size_t size) {
  void* ptr = TCMallocInternalNew(size);
  TCMallocInternalDeleteSized(ptr, size);
}

FUZZ_TEST(TCMalloc, NewSizedDelete)
    .WithDomains(fuzztest::InRange(size_t{0}, kFuzzingMaxSize));

void AlignedNewDelete(size_t size, std::align_val_t align) {
  void* ptr = TCMallocInternalNewAligned(size, align);
  ASSERT_TRUE(ptr != nullptr);
  TCMallocInternalDeleteAligned(ptr, align);
}

FUZZ_TEST(TCMalloc, AlignedNewDelete)
    .WithDomains(fuzztest::InRange(size_t{0}, kFuzzingMaxSize),
                 fuzztest::Map(
                     [](size_t v) {
                       return static_cast<std::align_val_t>(1ULL << v);
                     },
                     fuzztest::InRange<size_t>(0, GetPageSizeLog2())));

void AlignedNewSizedDelete(size_t size, std::align_val_t align) {
  void* ptr = TCMallocInternalNewAligned(size, align);
  ASSERT_TRUE(ptr != nullptr);
  TCMallocInternalDeleteSizedAligned(ptr, size, align);
}

TEST(TCMallocFuzzTest, AlignedNewSizedDeleteRegression) {
  AlignedNewSizedDelete(0, std::align_val_t{1});
  AlignedNewSizedDelete(0, std::align_val_t{2097152});
}

FUZZ_TEST(TCMalloc, AlignedNewSizedDelete)
    .WithDomains(fuzztest::InRange(size_t{0}, kFuzzingMaxSize),
                 fuzztest::Map(
                     [](size_t v) {
                       return static_cast<std::align_val_t>(1ULL << v);
                     },
                     fuzztest::InRange<size_t>(0, GetPageSizeLog2())));

void SizeReturningNewDeleteAllocatedSize(size_t size) {
  const auto ptr = TCMallocInternalSizeReturningNew(size);
  TCMallocInternalDeleteSized(ptr.p, ptr.n);
}

FUZZ_TEST(TCMalloc, SizeReturningNewDeleteAllocatedSize)
    .WithDomains(fuzztest::InRange(size_t{0}, kFuzzingMaxSize));

void SizeReturningNewDeleteRequestedSize(size_t size) {
  const auto ptr = TCMallocInternalSizeReturningNew(size);
  TCMallocInternalDeleteSized(ptr.p, size);
}

FUZZ_TEST(TCMalloc, SizeReturningNewDeleteRequestedSize)
    .WithDomains(fuzztest::InRange(size_t{0}, kFuzzingMaxSize));

void SizeReturningNewDeleteSized(size_t size, double delete_size_scale) {
  const auto ptr = TCMallocInternalSizeReturningNew(size);
  TCMallocInternalDeleteSized(ptr.p,
                              PickDeleteSize(size, ptr.n, delete_size_scale));
}

FUZZ_TEST(TCMalloc, SizeReturningNewDeleteSized)
    .WithDomains(fuzztest::InRange(size_t{0}, kFuzzingMaxSize),
                 fuzztest::InRange(0.0, 1.0));

void SizeReturningNewAlignedDeleteAllocatedSizeAligned(size_t alignment_log2,
                                                       size_t size) {
  const size_t alignment = size_t{1} << static_cast<int>(alignment_log2);
  const auto ptr = TCMallocInternalSizeReturningNewAligned(
      size, static_cast<std::align_val_t>(alignment));
  TCMallocInternalDeleteSizedAligned(ptr.p, ptr.n,
                                     static_cast<std::align_val_t>(alignment));
}

FUZZ_TEST(TCMalloc, SizeReturningNewAlignedDeleteAllocatedSizeAligned)
    .WithDomains(fuzztest::InRange(size_t{0}, GetPageSizeLog2()),
                 fuzztest::InRange(size_t{0}, kFuzzingMaxSize));

void SizeReturningNewAlignedDeleteRequestedSizeAligned(size_t alignment_log2,
                                                       size_t size) {
  const size_t alignment = size_t{1} << static_cast<int>(alignment_log2);
  const auto ptr = TCMallocInternalSizeReturningNewAligned(
      size, static_cast<std::align_val_t>(alignment));
  TCMallocInternalDeleteSizedAligned(ptr.p, size,
                                     static_cast<std::align_val_t>(alignment));
}

FUZZ_TEST(TCMalloc, SizeReturningNewAlignedDeleteRequestedSizeAligned)
    .WithDomains(fuzztest::InRange(size_t{0}, GetPageSizeLog2()),
                 fuzztest::InRange(size_t{0}, kFuzzingMaxSize));

void SizeReturningNewAlignedDeleteSizedAligned(size_t alignment_log2,
                                               size_t size,
                                               double delete_size_scale) {
  const size_t alignment = size_t{1} << static_cast<int>(alignment_log2);
  const auto ptr = TCMallocInternalSizeReturningNewAligned(
      size, static_cast<std::align_val_t>(alignment));
  TCMallocInternalDeleteSizedAligned(
      ptr.p, PickDeleteSize(size, ptr.n, delete_size_scale),
      static_cast<std::align_val_t>(alignment));
}

FUZZ_TEST(TCMalloc, SizeReturningNewAlignedDeleteSizedAligned)
    .WithDomains(fuzztest::InRange(size_t{0}, GetPageSizeLog2()),
                 fuzztest::InRange(size_t{0}, kFuzzingMaxSize),
                 fuzztest::InRange(0.0, 1.0));

size_t GetSystemPageSize() { return static_cast<size_t>(getpagesize()); }

bool IsAligned(const void* ptr, size_t alignment) {
  return reinterpret_cast<uintptr_t>(ptr) % alignment == 0;
}

// Returns an element size such that n * elem_size overflows size_t.
size_t OverflowingElementSize(size_t n) {
  TC_ASSERT_GE(n, 2);
  return std::numeric_limits<size_t>::max() / n + 1;
}

void PosixMemalignFreeAlignedSized(size_t alignment_log2, size_t size) {
  const size_t alignment = size_t{1} << static_cast<int>(alignment_log2);
  char sentinel;
  void* ptr = &sentinel;
  const int result = TCMallocInternalPosixMemalign(&ptr, alignment, size);
  if (alignment < sizeof(void*)) {
    ASSERT_EQ(result, EINVAL);
    ASSERT_EQ(ptr, &sentinel);
    return;
  }
  ASSERT_EQ(result, 0);
  ASSERT_TRUE(ptr != nullptr);
  ASSERT_TRUE(IsAligned(ptr, alignment));
  ASSERT_GE(TCMallocInternalMallocSize(ptr), size);
  TCMallocInternalFreeAlignedSized(ptr, alignment, size);
}

FUZZ_TEST(TCMalloc, PosixMemalignFreeAlignedSized)
    .WithDomains(fuzztest::InRange(size_t{0}, GetPageSizeLog2()),
                 fuzztest::InRange(size_t{0}, kFuzzingMaxSize));

void PosixMemalignRejectsBadAlignment(size_t alignment, size_t size) {
  char sentinel;
  void* ptr = &sentinel;
  const int result = TCMallocInternalPosixMemalign(&ptr, alignment, size);
  if (alignment % sizeof(void*) != 0 || !absl::has_single_bit(alignment)) {
    ASSERT_EQ(result, EINVAL);
    ASSERT_EQ(ptr, &sentinel);
    return;
  }
  ASSERT_EQ(result, 0);
  ASSERT_TRUE(IsAligned(ptr, alignment));
  TCMallocInternalFree(ptr);
}

FUZZ_TEST(TCMalloc, PosixMemalignRejectsBadAlignment)
    .WithDomains(fuzztest::InRange(size_t{0}, size_t{1} << kHugePageShift),
                 fuzztest::InRange(size_t{0}, kFuzzingMaxSize));

void MemalignFree(size_t alignment_log2, size_t size) {
  const size_t alignment = size_t{1} << static_cast<int>(alignment_log2);
  void* ptr = TCMallocInternalMemalign(alignment, size);
  ASSERT_TRUE(ptr != nullptr);
  ASSERT_TRUE(IsAligned(ptr, alignment));
  ASSERT_GE(TCMallocInternalMallocSize(ptr), size);
  TCMallocInternalFree(ptr);
}

FUZZ_TEST(TCMalloc, MemalignFree)
    .WithDomains(fuzztest::InRange(size_t{0}, GetPageSizeLog2()),
                 fuzztest::InRange(size_t{0}, kFuzzingMaxSize));

void VallocFree(size_t size) {
  void* ptr = TCMallocInternalValloc(size);
  ASSERT_TRUE(ptr != nullptr);
  ASSERT_TRUE(IsAligned(ptr, GetSystemPageSize()));
  ASSERT_GE(TCMallocInternalMallocSize(ptr), size);
  TCMallocInternalFree(ptr);
}

FUZZ_TEST(TCMalloc, VallocFree)
    .WithDomains(fuzztest::InRange(size_t{0}, kFuzzingMaxSize));

void PvallocFree(size_t size) {
  const size_t page_size = GetSystemPageSize();
  void* ptr = TCMallocInternalPvalloc(size);
  ASSERT_TRUE(ptr != nullptr);
  ASSERT_TRUE(IsAligned(ptr, page_size));
  // pvalloc rounds the request up to a whole page, and pvalloc(0) is one page.
  const size_t rounded =
      std::max(size + page_size - 1, page_size) & ~(page_size - 1);
  ASSERT_GE(TCMallocInternalMallocSize(ptr), rounded);
  TCMallocInternalFree(ptr);
}

FUZZ_TEST(TCMalloc, PvallocFree)
    .WithDomains(fuzztest::InRange(size_t{0}, kFuzzingMaxSize));

void CallocCfree(size_t n, size_t elem_size) {
  void* ptr = TCMallocInternalCalloc(n, elem_size);
  ASSERT_TRUE(ptr != nullptr);
  const size_t size = n * elem_size;
  ASSERT_GE(TCMallocInternalMallocSize(ptr), size);
  const auto* bytes = static_cast<const unsigned char*>(ptr);
  ASSERT_TRUE(
      std::all_of(bytes, bytes + size, [](unsigned char b) { return b == 0; }));
  TCMallocInternalCfree(ptr);
}

FUZZ_TEST(TCMalloc, CallocCfree)
    .WithDomains(fuzztest::InRange(size_t{0}, size_t{1024}),
                 fuzztest::InRange(size_t{0}, size_t{512}));

void CallocOverflow(size_t n) {
  errno = 0;
  void* ptr = TCMallocInternalCalloc(n, OverflowingElementSize(n));
  ASSERT_TRUE(ptr == nullptr);
  ASSERT_EQ(errno, ENOMEM);
}

FUZZ_TEST(TCMalloc, CallocOverflow)
    .WithDomains(fuzztest::InRange(size_t{2},
                                   std::numeric_limits<size_t>::max()));

void FillPattern(void* ptr, size_t size) {
  auto* bytes = static_cast<unsigned char*>(ptr);
  for (size_t i = 0; i < size; ++i) {
    bytes[i] = static_cast<unsigned char>(i);
  }
}

bool MatchesPattern(const void* ptr, size_t size) {
  const auto* bytes = static_cast<const unsigned char*>(ptr);
  for (size_t i = 0; i < size; ++i) {
    if (bytes[i] != static_cast<unsigned char>(i)) return false;
  }
  return true;
}

void MallocReallocArrayFree(size_t old_size, size_t n, size_t elem_size) {
  void* ptr = TCMallocInternalMalloc(old_size);
  ASSERT_TRUE(ptr != nullptr);
  FillPattern(ptr, old_size);
  const size_t new_size = n * elem_size;
  ptr = TCMallocInternalReallocArray(ptr, n, elem_size);
  if (new_size == 0) {
    ASSERT_TRUE(ptr == nullptr);
    return;
  }
  ASSERT_TRUE(ptr != nullptr);
  ASSERT_GE(TCMallocInternalMallocSize(ptr), new_size);
  ASSERT_TRUE(MatchesPattern(ptr, std::min(old_size, new_size)));
  TCMallocInternalFree(ptr);
}

FUZZ_TEST(TCMalloc, MallocReallocArrayFree)
    .WithDomains(fuzztest::InRange(size_t{0}, kFuzzingMaxSize),
                 fuzztest::InRange(size_t{0}, size_t{1024}),
                 fuzztest::InRange(size_t{0}, size_t{512}));

void ReallocArrayOverflowKeepsOriginal(size_t old_size, size_t n) {
  void* ptr = TCMallocInternalMalloc(old_size);
  ASSERT_TRUE(ptr != nullptr);
  FillPattern(ptr, old_size);
  errno = 0;
  ASSERT_TRUE(TCMallocInternalReallocArray(ptr, n, OverflowingElementSize(n)) ==
              nullptr);
  ASSERT_EQ(errno, ENOMEM);
  ASSERT_TRUE(MatchesPattern(ptr, old_size));
  TCMallocInternalFreeSized(ptr, old_size);
}

FUZZ_TEST(TCMalloc, ReallocArrayOverflowKeepsOriginal)
    .WithDomains(fuzztest::InRange(size_t{0}, kFuzzingMaxSize),
                 fuzztest::InRange(size_t{2},
                                   std::numeric_limits<size_t>::max()));

void ReallocArrayFromNull(size_t n, size_t elem_size) {
  void* ptr = TCMallocInternalReallocArray(nullptr, n, elem_size);
  ASSERT_TRUE(ptr != nullptr);
  ASSERT_GE(TCMallocInternalMallocSize(ptr), n * elem_size);
  TCMallocInternalFree(ptr);
}

FUZZ_TEST(TCMalloc, ReallocArrayFromNull)
    .WithDomains(fuzztest::InRange(size_t{0}, size_t{1024}),
                 fuzztest::InRange(size_t{0}, size_t{512}));

void MallocSdallocx(size_t size) {
  void* ptr = TCMallocInternalMalloc(size);
  ASSERT_TRUE(ptr != nullptr);
  TCMallocInternalSdallocx(ptr, size, /*flags=*/0);
}

FUZZ_TEST(TCMalloc, MallocSdallocx)
    .WithDomains(fuzztest::InRange(size_t{0}, kFuzzingMaxSize));

// MALLOCX_LG_ALIGN(0) is indistinguishable from "no alignment", and a sized
// deallocation of an aligned_alloc() object must carry its alignment, so the
// domain starts at 1.
void AlignedAllocSdallocx(size_t alignment_log2, size_t size) {
  const size_t alignment = size_t{1} << static_cast<int>(alignment_log2);
  void* ptr = TCMallocInternalAlignedAlloc(alignment, size);
  ASSERT_TRUE(ptr != nullptr);
  TCMallocInternalSdallocx(ptr, size,
                           MALLOCX_LG_ALIGN(static_cast<int>(alignment_log2)));
}

FUZZ_TEST(TCMalloc, AlignedAllocSdallocx)
    .WithDomains(fuzztest::InRange(size_t{1}, GetPageSizeLog2()),
                 fuzztest::InRange(size_t{0}, kFuzzingMaxSize));

void NallocxIsMonotonic(size_t alignment_log2, size_t a, size_t b) {
  const int flags = MALLOCX_LG_ALIGN(static_cast<int>(alignment_log2));
  const size_t lo = std::min(a, b);
  const size_t hi = std::max(a, b);
  ASSERT_GE(nallocx(lo, flags), lo);
  ASSERT_LE(nallocx(lo, flags), nallocx(hi, flags));
}

FUZZ_TEST(TCMalloc, NallocxIsMonotonic)
    .WithDomains(fuzztest::InRange(size_t{0}, GetPageSizeLog2()),
                 fuzztest::InRange(size_t{0}, kFuzzingMaxSize),
                 fuzztest::InRange(size_t{0}, kFuzzingMaxSize));

void MallocSizeAndOwnership(size_t size) {
  using Ownership = MallocExtension::Ownership;
  ASSERT_EQ(TCMallocInternalMallocSize(nullptr), size_t{0});
  ASSERT_EQ(MallocExtension::GetOwnership(nullptr), Ownership::kNotOwned);
  ASSERT_EQ(MallocExtension::GetOwnership(&size), Ownership::kNotOwned);
  void* ptr = TCMallocInternalMalloc(size);
  ASSERT_TRUE(ptr != nullptr);
  ASSERT_EQ(MallocExtension::GetOwnership(ptr), Ownership::kOwned);
  ASSERT_GE(TCMallocInternalMallocSize(ptr), size);
  TCMallocInternalFreeSized(ptr, size);
}

FUZZ_TEST(TCMalloc, MallocSizeAndOwnership)
    .WithDomains(fuzztest::InRange(size_t{0}, kFuzzingMaxSize));

void MallInfoTrimOpt(size_t size, size_t pad, int cmd, int value) {
  void* ptr = TCMallocInternalMalloc(size);
  ASSERT_TRUE(ptr != nullptr);
  const size_t allocated = TCMallocInternalMallocSize(ptr);
#if defined(TCMALLOC_HAVE_STRUCT_MALLINFO)
  // The int-typed fields truncate, but this process stays far below 2 GiB.
  const struct mallinfo info = TCMallocInternalMallInfo();
  ASSERT_GE(info.arena, static_cast<int>(allocated));
  ASSERT_LE(info.uordblks + info.fordblks, info.arena);
#endif
#if defined(TCMALLOC_HAVE_STRUCT_MALLINFO2)
  const struct mallinfo2 info2 = TCMallocInternalMallInfo2();
  ASSERT_GE(info2.arena, allocated);
  ASSERT_LE(info2.uordblks + info2.fordblks, info2.arena);
#endif
  const int trimmed = TCMallocInternalMallocTrim(pad);
  ASSERT_TRUE(trimmed == 0 || trimmed == 1);
  ASSERT_EQ(TCMallocInternalMallOpt(cmd, value), 1);
  TCMallocInternalFree(ptr);
}

FUZZ_TEST(TCMalloc, MallInfoTrimOpt)
    .WithDomains(fuzztest::InRange(size_t{0}, kFuzzingMaxSize),
                 fuzztest::Arbitrary<size_t>(), fuzztest::Arbitrary<int>(),
                 fuzztest::Arbitrary<int>());

TEST(TCMallocFuzzTest, MallocStatsAndMallocInfo) {
  void* ptr = TCMallocInternalMalloc(1);
  ASSERT_TRUE(ptr != nullptr);
  TCMallocInternalMallocStats();
  char* buf;
  size_t len;
  FILE* fp = open_memstream(&buf, &len);
  ASSERT_TRUE(fp != nullptr);
  EXPECT_EQ(TCMallocInternalMallocInfo(0, fp), 0);
  ASSERT_EQ(fclose(fp), 0);
  EXPECT_STREQ(buf, "<malloc></malloc>\n");
  free(buf);
  TCMallocInternalFreeSized(ptr, 1);
}

void NewNothrowDelete(size_t size) {
  void* ptr = TCMallocInternalNewNothrow(size, std::nothrow);
  ASSERT_TRUE(ptr != nullptr);
  TCMallocInternalDeleteNothrow(ptr, std::nothrow);
  ptr = TCMallocInternalNewNothrow(size, std::nothrow);
  ASSERT_TRUE(ptr != nullptr);
  TCMallocInternalDeleteSized(ptr, size);
}

FUZZ_TEST(TCMalloc, NewNothrowDelete)
    .WithDomains(fuzztest::InRange(size_t{0}, kFuzzingMaxSize));

void NewAlignedNothrowDeleteAligned(size_t size, size_t alignment_log2) {
  const auto align = static_cast<std::align_val_t>(size_t{1} << alignment_log2);
  void* ptr = TCMallocInternalNewAlignedNothrow(size, align, std::nothrow);
  ASSERT_TRUE(ptr != nullptr);
  ASSERT_TRUE(IsAligned(ptr, static_cast<size_t>(align)));
  TCMallocInternalDeleteAlignedNothrow(ptr, align, std::nothrow);
  ptr = TCMallocInternalNewAlignedNothrow(size, align, std::nothrow);
  ASSERT_TRUE(ptr != nullptr);
  TCMallocInternalDeleteSizedAligned(ptr, size, align);
}

FUZZ_TEST(TCMalloc, NewAlignedNothrowDeleteAligned)
    .WithDomains(fuzztest::InRange(size_t{0}, kFuzzingMaxSize),
                 fuzztest::InRange(size_t{0}, GetPageSizeLog2()));

void NewHotColdDeleteSized(size_t size, uint8_t hot_cold) {
  const auto access = static_cast<hot_cold_t>(hot_cold);
  void* ptr = TCMallocInternalNewHotCold(size, access);
  ASSERT_TRUE(ptr != nullptr);
  TCMallocInternalDeleteSized(ptr, size);
  ptr = TCMallocInternalNewHotColdNothrow(size, std::nothrow, access);
  ASSERT_TRUE(ptr != nullptr);
  TCMallocInternalDelete(ptr);
}

FUZZ_TEST(TCMalloc, NewHotColdDeleteSized)
    .WithDomains(fuzztest::InRange(size_t{0}, kFuzzingMaxSize),
                 fuzztest::Arbitrary<uint8_t>());

void NewAlignedHotColdDeleteSizedAligned(size_t size, size_t alignment_log2,
                                         uint8_t hot_cold) {
  const auto align = static_cast<std::align_val_t>(size_t{1} << alignment_log2);
  const auto access = static_cast<hot_cold_t>(hot_cold);
  void* ptr = TCMallocInternalNewAlignedHotCold(size, align, access);
  ASSERT_TRUE(ptr != nullptr);
  ASSERT_TRUE(IsAligned(ptr, static_cast<size_t>(align)));
  TCMallocInternalDeleteSizedAligned(ptr, size, align);
  ptr = TCMallocInternalNewAlignedHotColdNothrow(size, align, std::nothrow,
                                                 access);
  ASSERT_TRUE(ptr != nullptr);
  ASSERT_TRUE(IsAligned(ptr, static_cast<size_t>(align)));
  TCMallocInternalDeleteAligned(ptr, align);
}

FUZZ_TEST(TCMalloc, NewAlignedHotColdDeleteSizedAligned)
    .WithDomains(fuzztest::InRange(size_t{0}, kFuzzingMaxSize),
                 fuzztest::InRange(size_t{0}, GetPageSizeLog2()),
                 fuzztest::Arbitrary<uint8_t>());

void NewArrayDeleteArray(size_t size, uint8_t hot_cold) {
  const auto access = static_cast<hot_cold_t>(hot_cold);
  void* ptr = TCMallocInternalNewArray(size);
  ASSERT_TRUE(ptr != nullptr);
  TCMallocInternalDeleteArraySized(ptr, size);
  ptr = TCMallocInternalNewArrayNothrow(size, std::nothrow);
  ASSERT_TRUE(ptr != nullptr);
  TCMallocInternalDeleteArrayNothrow(ptr, std::nothrow);
  ptr = TCMallocInternalNewArrayHotCold(size, access);
  ASSERT_TRUE(ptr != nullptr);
  TCMallocInternalDeleteArray(ptr);
  ptr = TCMallocInternalNewArrayHotColdNothrow(size, std::nothrow, access);
  ASSERT_TRUE(ptr != nullptr);
  TCMallocInternalDeleteArraySized(ptr, size);
}

FUZZ_TEST(TCMalloc, NewArrayDeleteArray)
    .WithDomains(fuzztest::InRange(size_t{0}, kFuzzingMaxSize),
                 fuzztest::Arbitrary<uint8_t>());

void NewArrayAlignedDeleteArrayAligned(size_t size, size_t alignment_log2,
                                       uint8_t hot_cold) {
  const auto align = static_cast<std::align_val_t>(size_t{1} << alignment_log2);
  const auto access = static_cast<hot_cold_t>(hot_cold);
  void* ptr = TCMallocInternalNewArrayAligned(size, align);
  ASSERT_TRUE(ptr != nullptr);
  ASSERT_TRUE(IsAligned(ptr, static_cast<size_t>(align)));
  TCMallocInternalDeleteArraySizedAligned(ptr, size, align);
  ptr = TCMallocInternalNewArrayAlignedNothrow(size, align, std::nothrow);
  ASSERT_TRUE(ptr != nullptr);
  TCMallocInternalDeleteArrayAlignedNothrow(ptr, align, std::nothrow);
  ptr = TCMallocInternalNewArrayAlignedHotCold(size, align, access);
  ASSERT_TRUE(ptr != nullptr);
  TCMallocInternalDeleteArrayAligned(ptr, align);
  ptr = TCMallocInternalNewArrayAlignedHotColdNothrow(size, align, std::nothrow,
                                                      access);
  ASSERT_TRUE(ptr != nullptr);
  TCMallocInternalDeleteArraySizedAligned(ptr, size, align);
}

FUZZ_TEST(TCMalloc, NewArrayAlignedDeleteArrayAligned)
    .WithDomains(fuzztest::InRange(size_t{0}, kFuzzingMaxSize),
                 fuzztest::InRange(size_t{0}, GetPageSizeLog2()),
                 fuzztest::Arbitrary<uint8_t>());

}  // namespace
}  // namespace tcmalloc::tcmalloc_internal

int main(int argc, char** argv) {
  testing::InitGoogleTest(&argc, argv);
  fuzztest::InitFuzzTest(&argc, &argv);

  return RUN_ALL_TESTS();
}
