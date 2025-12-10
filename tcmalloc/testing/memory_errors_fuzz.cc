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

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <new>

#include "gtest/gtest.h"
#include "fuzztest/fuzztest.h"
#include "fuzztest/init_fuzztest.h"
#include "absl/base/casts.h"
#include "tcmalloc/common.h"
#include "tcmalloc/internal/memory_tag.h"
#include "tcmalloc/internal_malloc_extension.h"
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
      tag != MemoryTag::kNormal && tag != MemoryTag::kNormalP1 &&
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
}

FUZZ_TEST(MemoryErrorsFuzzTest, WildPointerSizedDelete);

void MismatchedSizedDelete(size_t allocated, size_t deallocated) {
  GTEST_SKIP() << "Skipping";

  void* ptr = TCMallocInternalNewNothrow(allocated, std::nothrow);
  if (ptr == nullptr) {
    return;
  }

  // The pointer needs to be sampled or large for us to detect the error.
  if (GetMemoryTag(ptr) != MemoryTag::kSampled && allocated <= kMaxSize &&
      deallocated <= kMaxSize) {
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

}  // namespace
}  // namespace tcmalloc::tcmalloc_internal

int main(int argc, char** argv) {
  testing::InitGoogleTest(&argc, argv);
  fuzztest::InitFuzzTest(&argc, &argv);

  return RUN_ALL_TESTS();
}
