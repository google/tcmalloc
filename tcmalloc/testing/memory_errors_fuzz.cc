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

void WildPointerUnsizedDelete(uintptr_t ptr) {
  GTEST_SKIP() << "Skipping";

  // It is perfectly reasonable for the pointer to be nullptr.
  if (ptr == 0) {
    return;
  }

  LongJmpScope scope;
  if (setjmp(scope.buf_)) {
    return;
  }

  TCMallocInternalDelete(absl::bit_cast<void*>(ptr));
  // We should have caught the error and not reached this point.
  EXPECT_TRUE(false);
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

  LongJmpScope scope;
  if (setjmp(scope.buf_)) {
    return;
  }

  void* new_ptr = TCMallocInternalRealloc(absl::bit_cast<void*>(ptr), new_size);
  // We should have caught the error and not reached this point unless ptr was
  // nullptr.
  EXPECT_EQ(ptr, 0);
  TCMallocInternalFree(new_ptr);
}

TEST(MemoryErrorsFuzzTest, WildPointerReallocRegression) {
  WildPointerRealloc(4406726867650173363ull, 1);
}

FUZZ_TEST(MemoryErrorsFuzzTest, WildPointerRealloc);

void WildPointerSizedDelete(uintptr_t ptr, size_t size) {
  GTEST_SKIP() << "Skipping";

  // It is perfectly reasonable for the pointer to be nullptr.
  if (ptr == 0) {
    return;
  }

  // The pointer must either be non-normal or larger than kMaxSize.  We don't
  // expect to have lightweight checks otherwise.
  if (auto tag = GetMemoryTag(absl::bit_cast<void*>(ptr));
      (tag == MemoryTag::kNormal || tag == MemoryTag::kNormalP1) &&
      size <= kMaxSize) {
    return;
  }

  LongJmpScope scope;
  if (setjmp(scope.buf_)) {
    return;
  }

  TCMallocInternalDeleteSized(absl::bit_cast<void*>(ptr), size);
  // We should have caught the error and not reached this point.
  EXPECT_TRUE(false);
}

TEST(MemoryErrorsFuzzTest, WildPointerSizedDeleteRegression) {
  WildPointerSizedDelete(18446744073709551615ull, 18446744073709551615ull);
  WildPointerSizedDelete(0, 18446744073709551615ull);
  WildPointerSizedDelete(17592186048512ull, 0);
}

FUZZ_TEST(MemoryErrorsFuzzTest, WildPointerSizedDelete);

void MismatchedSizedDelete(size_t allocated, size_t deallocated) {
  GTEST_SKIP() << "Skipping";

  void* ptr = TCMallocInternalNewNothrow(allocated, std::nothrow);
  if (ptr == nullptr) {
    return;
  }

  // The pointer needs to be sampled or large for us to detect the error.
  if (GetMemoryTag(ptr) != MemoryTag::kSampled && deallocated <= kMaxSize) {
    TCMallocInternalDeleteSized(ptr, allocated);
    return;
  }

  const size_t actual_size = MallocExtension_Internal_GetAllocatedSize(ptr);

  LongJmpScope scope;
  if (setjmp(scope.buf_)) {
    return;
  }

  TCMallocInternalDeleteSized(ptr, deallocated);
  // We should have caught the error and not reached this point.  An error did
  // not occur only if the sizes match.
  EXPECT_EQ(MallocExtension_Internal_GetEstimatedAllocatedSize(deallocated),
            actual_size);
}

TEST(MemoryErrorsFuzzTest, MismatchedSizedDeleteRegression) {
  MismatchedSizedDelete(7947537452012049129, 0);
  MismatchedSizedDelete(549755813888, 15561727408584254371ull);
}

FUZZ_TEST(MemoryErrorsFuzzTest, MismatchedSizedDelete);

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

  LongJmpScope scope;
  if (setjmp(scope.buf_)) {
    return;
  }

  if (deallocated_alignment.has_value()) {
    TCMallocInternalDeleteSizedAligned(ptr, size, *deallocated_alignment);
  } else {
    TCMallocInternalDeleteSized(ptr, size);
  }
  EXPECT_EQ(allocated_alignment, deallocated_alignment);
}

FUZZ_TEST(MemoryErrorsFuzzTest, MismatchedAlignedDelete)
    .WithDomains(
        fuzztest::Arbitrary<size_t>(),
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

  LongJmpScope scope;
  if (setjmp(scope.buf_)) {
    return;
  }

  if (deallocated_alignment.has_value()) {
    TCMallocInternalFreeAlignedSized(ptr, *deallocated_alignment, size);
  } else {
    TCMallocInternalFreeSized(ptr, size);
  }
  EXPECT_EQ(allocated_alignment, deallocated_alignment);
}

FUZZ_TEST(MemoryErrorsFuzzTest, MismatchedAlignedFree)
    .WithDomains(fuzztest::Arbitrary<size_t>(),
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
  char* misaligned =
      static_cast<char*>(ptr) + static_cast<size_t>(misalignment);

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
  EXPECT_EQ(misalignment, std::align_val_t{0});
}

FUZZ_TEST(MemoryErrorsFuzzTest, MisalignedPointer)
    .WithDomains(
        fuzztest::Arbitrary<size_t>(),
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
