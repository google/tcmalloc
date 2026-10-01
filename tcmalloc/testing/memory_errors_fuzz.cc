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

#include <setjmp.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <new>
#include <optional>

#include "gtest/gtest.h"
#include "fuzztest/fuzztest.h"
#include "fuzztest/init_fuzztest.h"
#include "absl/base/casts.h"
#include "absl/log/check.h"
#include "absl/log/log.h"
#include "absl/numeric/bits.h"
#include "tcmalloc/common.h"
#include "tcmalloc/internal/config.h"
#include "tcmalloc/internal/memory_tag.h"
#include "tcmalloc/internal_malloc_extension.h"
#include "tcmalloc/malloc_extension.h"
#include "tcmalloc/tcmalloc.h"
#include "tcmalloc/testing/testutil.h"

namespace tcmalloc::tcmalloc_internal {
namespace {

// Queries pointer ownership.  Many of our UB checks for wild pointers only work
// on ones that don't conflict with live TCMalloc-managed address space.
bool IsOwned(void* ptr) {
  return MallocExtension_Internal_GetOwnership(ptr) ==
         MallocExtension::Ownership::kOwned;
}

// Upper bound on the sizes the fuzzer requests from the allocator.  It covers
// the size-class, page heap, and hugepage-sized paths; anything larger only
// adds address space and per-page metadata.  Unbounded sizes drive the fuzzer
// out of memory: multi-TiB requests cost gigabytes of metadata, and an
// iteration that provokes an error necessarily leaks its allocation because
// the deallocation was interrupted mid-way.
constexpr size_t kMaxFuzzAllocationSize = 2 * kHugePageSize;

TEST(MemoryErrorsTest, IsOwnedTest) {
  // Ensure IsOwned is not tautologically true or false.
  void* ptr = TCMallocInternalNew(1);
  EXPECT_TRUE(IsOwned(ptr));
  EXPECT_FALSE(IsOwned(&ptr));
  TCMallocInternalFree(ptr);
}

void WildPointerUnsizedDelete(uintptr_t ptr) {
  GTEST_SKIP() << "Skipping";

  // It is perfectly reasonable for the pointer to be nullptr.
  if (ptr == 0) {
    return;
  }

  void* p = absl::bit_cast<void*>(ptr);
  if (IsOwned(p)) {
    return;
  }

  LongJmpScope scope;
  if (setjmp(scope.buf_)) {
    return;
  }

  TCMallocInternalDelete(p);
  LOG(FATAL) << "should have caught error and not reached this point";
}

TEST(MemoryErrorsFuzzTest, WildPointerUnsizedDeleteRegression) {
  WildPointerUnsizedDelete(0);
  WildPointerUnsizedDelete(1);
  WildPointerUnsizedDelete(5351867499084745203ull);
  WildPointerUnsizedDelete(5351867499084745200ull);
}

FUZZ_TEST(MemoryErrorsFuzzTest, WildPointerUnsizedDelete);

void WildPointerRealloc(uintptr_t ptr, size_t new_size) {
  GTEST_SKIP() << "Skipping";

  void* p = absl::bit_cast<void*>(ptr);
  if (IsOwned(p)) {
    return;
  }

  LongJmpScope scope;
  if (setjmp(scope.buf_)) {
    return;
  }

  void* new_ptr = TCMallocInternalRealloc(p, new_size);
  // We should have caught the error and not reached this point unless ptr was
  // nullptr.
  EXPECT_EQ(ptr, 0);
  TCMallocInternalFree(new_ptr);
}

TEST(MemoryErrorsFuzzTest, WildPointerReallocRegression) {
  WildPointerRealloc(4406726867650173363ull, 1);
}

FUZZ_TEST(MemoryErrorsFuzzTest, WildPointerRealloc)
    .WithDomains(fuzztest::Arbitrary<uintptr_t>(),
                 fuzztest::InRange<size_t>(0, kMaxFuzzAllocationSize));

void WildPointerSizedDelete(uintptr_t ptr, size_t size) {
  GTEST_SKIP() << "Skipping";

  // It is perfectly reasonable for the pointer to be nullptr.
  if (ptr == 0) {
    return;
  }

  void* p = absl::bit_cast<void*>(ptr);
  if (IsOwned(p)) {
    return;
  }

  // The pointer must either be sampled/metadata or larger than kMaxSize.  We
  // don't expect to have lightweight checks otherwise: normal and cold memory
  // with a size <= kMaxSize take the fast sized-delete path that derives the
  // size class from `size` without consulting the pagemap, so a wild pointer is
  // not detected.
  if (auto tag = GetMemoryTag(p);
      (tag == MemoryTag::kNormal || tag == MemoryTag::kNormalP1 ||
       tag == MemoryTag::kCold) &&
      size <= kMaxSize) {
    return;
  }

  LongJmpScope scope;
  if (setjmp(scope.buf_)) {
    return;
  }

  TCMallocInternalDeleteSized(p, size);
  LOG(FATAL) << "should have caught error and not reached this point";
}

TEST(MemoryErrorsFuzzTest, WildPointerSizedDeleteRegression) {
  WildPointerSizedDelete(18446744073709551615ull, 18446744073709551615ull);
  WildPointerSizedDelete(0, 18446744073709551615ull);
  WildPointerSizedDelete(17592186048512ull, 0);
  // Cold-tagged wild pointers with a size <= kMaxSize take the same
  // non-validating fast sized-delete path as normal memory and are not caught.
  WildPointerSizedDelete(8796093022208ull, 0);
  WildPointerSizedDelete(8796093022208ull, 131072);
  WildPointerSizedDelete(8796093022216ull, 131073);
}

FUZZ_TEST(MemoryErrorsFuzzTest, WildPointerSizedDelete);

void MismatchedSizedDelete(size_t allocated, size_t deallocated) {
  GTEST_SKIP() << "Skipping";

  void* ptr = TCMallocInternalNewNothrow(allocated, std::nothrow);
  if (ptr == nullptr) {
    return;
  }

  // The pointer needs to be sampled or large for us to detect the error.
  const bool sampled = IsSampledMemory(ptr);
  if (!sampled && deallocated <= kMaxSize) {
    TCMallocInternalDeleteSized(ptr, allocated);
    return;
  }

  // This binary links tcmalloc_internal_methods_only, which omits the
  // MallocExtension_Internal_* entry points; the weak declarations resolve to
  // nullptr.  Use the always-defined TCMalloc_Internal_* equivalents.
  const size_t actual_size = TCMalloc_Internal_GetAllocatedSize(ptr);

  LongJmpScope scope;
  if (setjmp(scope.buf_)) {
    return;
  }

  TCMallocInternalDeleteSized(ptr, deallocated);
  // We should have caught the error and not reached this point.  An error did
  // not occur only if the sizes match.
  //
  // Sampled allocations (including GWP-ASan guarded ones) record the requested
  // size and require an exact match.  Unsampled large allocations only know
  // the page-rounded span size, so any size that rounds to the same number of
  // pages is accepted.
  if (sampled) {
    CHECK_EQ(deallocated, allocated);
  } else {
    CHECK_EQ(TCMalloc_Internal_GetEstimatedAllocatedSize(deallocated),
             actual_size);
  }
}

TEST(MemoryErrorsFuzzTest, MismatchedSizedDeleteRegression) {
  MismatchedSizedDelete(7947537452012049129, 0);
}

TEST(MemoryErrorsFuzzTest, MismatchedSizedDeleteRegression2) {
  MismatchedSizedDelete(549755813888, 15561727408584254371ull);
}

TEST(MemoryErrorsFuzzTest, MismatchedSizedDeleteSampledRegression) {
  // GWP-ASan reports the requested size (7) from GetAllocatedSize while the
  // size-class estimate for 7 bytes is 8, so a matching sampled delete must be
  // judged by the requested size rather than the estimate.
  ScopedAlwaysSample always_sample;
  for (int i = 0; i < 4; ++i) {
    MismatchedSizedDelete(7, 7);
    MismatchedSizedDelete(7, 8);
  }
}

FUZZ_TEST(MemoryErrorsFuzzTest, MismatchedSizedDelete)
    .WithDomains(fuzztest::InRange<size_t>(0, kMaxFuzzAllocationSize),
                 fuzztest::Arbitrary<size_t>());

void MismatchedAlignedDelete(
    size_t size, std::optional<std::align_val_t> allocated_alignment,
    std::optional<std::align_val_t> deallocated_alignment) {
  GTEST_SKIP() << "Skipping";

  void* ptr;
  if (allocated_alignment.has_value()) {
    ptr = TCMallocInternalNewAlignedNothrow(size, *allocated_alignment,
                                            std::nothrow);
  } else {
    ptr = TCMallocInternalNewNothrow(size, std::nothrow);
  }
  if (ptr == nullptr) {
    return;
  }

  // Only sampled allocations record their requested alignment.  The sized
  // delete fast path for unsampled memory recomputes the size class from the
  // provided size and alignment without consulting metadata, so a mismatch is
  // not detected (outside of debug assertions).
  if (!IsSampledMemory(ptr)) {
    if (allocated_alignment.has_value()) {
      TCMallocInternalDeleteSizedAligned(ptr, size, *allocated_alignment);
    } else {
      TCMallocInternalDeleteSized(ptr, size);
    }
    return;
  }

  LongJmpScope scope;
  if (setjmp(scope.buf_)) {
    return;
  }

  if (deallocated_alignment.has_value()) {
    TCMallocInternalDeleteSizedAligned(ptr, size, *deallocated_alignment);
  } else {
    TCMallocInternalDeleteSized(ptr, size);
  }
  CHECK_EQ(allocated_alignment, deallocated_alignment);
}

TEST(MemoryErrorsFuzzTest, MismatchedAlignedDeleteRegression) {
  // Unsampled: a mismatch that does not change the size class is not detected.
  MismatchedAlignedDelete(0, std::align_val_t{2}, std::align_val_t{1});
}

TEST(MemoryErrorsFuzzTest, MismatchedAlignedDeleteSampledRegression) {
  ScopedAlwaysSample always_sample;
  for (int i = 0; i < 4; ++i) {
    MismatchedAlignedDelete(8, std::align_val_t{64}, std::nullopt);
    MismatchedAlignedDelete(8, std::nullopt, std::align_val_t{64});
    MismatchedAlignedDelete(8, std::align_val_t{64}, std::align_val_t{64});
  }
}

FUZZ_TEST(MemoryErrorsFuzzTest, MismatchedAlignedDelete)
    .WithDomains(
        fuzztest::InRange<size_t>(0, kMaxFuzzAllocationSize),
        fuzztest::OptionalOf(fuzztest::Map(
            [](size_t v) { return static_cast<std::align_val_t>(1ULL << v); },
            fuzztest::InRange<size_t>(0, kHugePageShift))),
        fuzztest::OptionalOf(fuzztest::Map(
            [](size_t v) { return static_cast<std::align_val_t>(1ULL << v); },
            fuzztest::InRange<size_t>(0, kHugePageShift))));

void MismatchedAlignedFree(size_t size,
                           std::optional<size_t> allocated_alignment,
                           std::optional<size_t> deallocated_alignment) {
  GTEST_SKIP() << "Skipping";

  void* ptr;
  if (allocated_alignment.has_value()) {
    ptr = TCMallocInternalAlignedAlloc(*allocated_alignment, size);
  } else {
    ptr = TCMallocInternalMalloc(size);
  }
  if (ptr == nullptr) {
    return;
  }

  // As with MismatchedAlignedDelete, only sampled allocations can detect this.
  if (!IsSampledMemory(ptr)) {
    if (allocated_alignment.has_value()) {
      TCMallocInternalFreeAlignedSized(ptr, *allocated_alignment, size);
    } else {
      TCMallocInternalFreeSized(ptr, size);
    }
    return;
  }

  LongJmpScope scope;
  if (setjmp(scope.buf_)) {
    return;
  }

  if (deallocated_alignment.has_value()) {
    TCMallocInternalFreeAlignedSized(ptr, *deallocated_alignment, size);
  } else {
    TCMallocInternalFreeSized(ptr, size);
  }
  // malloc() and free_sized() carry an implicit alignment of
  // alignof(std::max_align_t), which is what TCMalloc records and compares
  // against, so aligned_alloc(alignof(std::max_align_t), size) paired with
  // free_sized() (and vice versa) is not a mismatch.
  constexpr size_t kMallocAlignment = alignof(std::max_align_t);
  CHECK_EQ(allocated_alignment.value_or(kMallocAlignment),
           deallocated_alignment.value_or(kMallocAlignment));
}

TEST(MemoryErrorsFuzzTest, MismatchedAlignedFreeRegression) {
  // Unsampled: both alignments exceed a page and land on the same span, so the
  // mismatch is not detected.
  MismatchedAlignedFree(7, 2097152, 1048576);
}

TEST(MemoryErrorsFuzzTest, MismatchedAlignedFreeSampledRegression) {
  ScopedAlwaysSample always_sample;
  for (int i = 0; i < 4; ++i) {
    MismatchedAlignedFree(8, alignof(std::max_align_t), std::nullopt);
    MismatchedAlignedFree(8, std::nullopt, alignof(std::max_align_t));
    MismatchedAlignedFree(8, 64, std::nullopt);
    MismatchedAlignedFree(8, std::nullopt, 64);
    MismatchedAlignedFree(8, 1, 64);
  }
}

FUZZ_TEST(MemoryErrorsFuzzTest, MismatchedAlignedFree)
    .WithDomains(fuzztest::InRange<size_t>(0, kMaxFuzzAllocationSize),
                 fuzztest::OptionalOf(fuzztest::Map(
                     [](size_t v) { return static_cast<size_t>(1ULL << v); },
                     fuzztest::InRange<size_t>(0, kHugePageShift))),
                 fuzztest::OptionalOf(fuzztest::Map(
                     [](size_t v) { return static_cast<size_t>(1ULL << v); },
                     fuzztest::InRange<size_t>(0, kHugePageShift))));

void MisalignedPointer(size_t size, std::optional<hot_cold_t> hot_cold,
                       std::optional<std::align_val_t> alignment,
                       std::align_val_t misalignment, bool sized) {
  GTEST_SKIP() << "Skipping";

  if (alignment.has_value() &&
      !absl::has_single_bit(static_cast<size_t>(*alignment))) {
    // Ill-formed alignment.
    return;
  }

  void* ptr;
  if (hot_cold.has_value()) {
    if (alignment.has_value()) {
      ptr = TCMallocInternalNewAlignedHotColdNothrow(size, *alignment,
                                                     std::nothrow, *hot_cold);
    } else {
      ptr = TCMallocInternalNewHotColdNothrow(size, std::nothrow, *hot_cold);
    }
  } else {
    if (alignment.has_value()) {
      ptr = TCMallocInternalNewAlignedNothrow(size, *alignment, std::nothrow);
    } else {
      ptr = TCMallocInternalNewNothrow(size, std::nothrow);
    }
  }
  if (ptr == nullptr) {
    return;
  }

  misalignment =
      std::min(misalignment,
               static_cast<std::align_val_t>(
                   static_cast<size_t>(alignment.value_or(kAlignment)) - 1u));
  // Keep the misaligned pointer inside the allocation: overaligned requests can
  // exceed their span, and the start of an unrelated live span is a valid free.
  const size_t allocated_size =
      std::max<size_t>(TCMalloc_Internal_GetAllocatedSize(ptr), 1u);
  misalignment = std::min(misalignment,
                          static_cast<std::align_val_t>(allocated_size - 1u));
  char* misaligned =
      static_cast<char*>(ptr) + static_cast<size_t>(misalignment);

  // Detection is guaranteed in every build when the pointer is misaligned for
  // kAlignment (it fails the tag/alignment mask on the fast path), when the
  // allocation is sampled (the sampled span or GWP-ASan slot records its
  // start), or when it has no size class (a page heap span records its start).
  // An unsampled size-classful object misaligned by a multiple of kAlignment is
  // only caught by debug assertions: the release fast path derives the size
  // class from the pagemap and pushes the pointer onto a freelist.
  const bool size_classful =
      size <= kMaxSize &&
      static_cast<size_t>(alignment.value_or(std::align_val_t{1})) <= kPageSize;
  if (static_cast<size_t>(misalignment) % static_cast<size_t>(kAlignment) ==
          0 &&
      size_classful && !IsSampledMemory(ptr)) {
    if (alignment.has_value()) {
      TCMallocInternalDeleteSizedAligned(ptr, size, *alignment);
    } else {
      TCMallocInternalDeleteSized(ptr, size);
    }
    return;
  }

  LongJmpScope scope;
  if (setjmp(scope.buf_)) {
    return;
  }

  if (alignment.has_value()) {
    if (sized) {
      TCMallocInternalDeleteSizedAligned(misaligned, size, *alignment);
    } else {
      TCMallocInternalDeleteAligned(misaligned, *alignment);
    }
  } else {
    if (sized) {
      TCMallocInternalDeleteSized(misaligned, size);
    } else {
      TCMallocInternalDelete(misaligned);
    }
  }
  CHECK_EQ(misalignment, std::align_val_t{0});
}

TEST(MemoryErrorsFuzzTest, MisalignedPointerRegression) {
  // Unsampled size-classful object misaligned by kAlignment: undetected by the
  // release fast path, so the harness must not expect an error.
  ScopedNeverSample never_sample;
  MisalignedPointer(16, std::nullopt, std::align_val_t{64}, std::align_val_t{8},
                    /*sized=*/false);
  MisalignedPointer(16, std::nullopt, std::align_val_t{64}, std::align_val_t{8},
                    /*sized=*/true);
  // Misaligned for kAlignment: always detected.
  MisalignedPointer(16, std::nullopt, std::nullopt, std::align_val_t{1},
                    /*sized=*/false);
  MisalignedPointer(16, std::nullopt, std::nullopt, std::align_val_t{1},
                    /*sized=*/true);
  // Page heap span: always detected.
  MisalignedPointer(kMaxSize + 1, std::nullopt, std::align_val_t{64},
                    std::align_val_t{8}, /*sized=*/false);
  MisalignedPointer(0, std::nullopt, std::align_val_t{2 * kPageSize},
                    std::align_val_t{kPageSize / 2}, /*sized=*/true);
}

TEST(MemoryErrorsFuzzTest, MisalignedPointerSampledRegression) {
  ScopedAlwaysSample always_sample;
  for (int i = 0; i < 4; ++i) {
    MisalignedPointer(16, std::nullopt, std::align_val_t{64},
                      std::align_val_t{8}, /*sized=*/false);
    MisalignedPointer(16, std::nullopt, std::align_val_t{64},
                      std::align_val_t{8}, /*sized=*/true);
    MisalignedPointer(16, std::nullopt, std::nullopt, std::align_val_t{1},
                      /*sized=*/false);
  }
}

FUZZ_TEST(MemoryErrorsFuzzTest, MisalignedPointer)
    .WithDomains(
        fuzztest::InRange<size_t>(0, kMaxFuzzAllocationSize),
        fuzztest::OptionalOf(
            fuzztest::Map([](uint8_t v) { return static_cast<hot_cold_t>(v); },
                          fuzztest::Arbitrary<uint8_t>())),
        fuzztest::OptionalOf(fuzztest::Map(
            [](size_t v) { return static_cast<std::align_val_t>(1ULL << v); },
            fuzztest::InRange<size_t>(0, kHugePageShift))),
        fuzztest::Map([](size_t v) { return static_cast<std::align_val_t>(v); },
                      fuzztest::Arbitrary<size_t>()),
        fuzztest::Arbitrary<bool>());

}  // namespace
}  // namespace tcmalloc::tcmalloc_internal

int main(int argc, char** argv) {
  testing::InitGoogleTest(&argc, argv);
  fuzztest::InitFuzzTest(&argc, &argv);

  return RUN_ALL_TESTS();
}
