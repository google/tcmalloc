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

#include <cstddef>
#include <new>

#include "gtest/gtest.h"
#include "fuzztest/fuzztest.h"
#include "fuzztest/init_fuzztest.h"
#include "absl/numeric/bits.h"
#include "tcmalloc/common.h"
#include "tcmalloc/internal/logging.h"
#include "tcmalloc/internal/page_size.h"
#include "tcmalloc/tcmalloc.h"

namespace tcmalloc::tcmalloc_internal {
namespace {

inline constexpr size_t kFuzzingMaxSize = kMaxSize * 2 + 1;

size_t GetPageSizeLog2() { return absl::countr_zero(GetPageSize()); }

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

}  // namespace
}  // namespace tcmalloc::tcmalloc_internal

int main(int argc, char** argv) {
  testing::InitGoogleTest(&argc, argv);
  fuzztest::InitFuzzTest(&argc, &argv);

  return RUN_ALL_TESTS();
}
