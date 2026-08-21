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

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "fuzztest/fuzztest.h"
#include "absl/types/span.h"
#include "tcmalloc/internal/range_tracker.h"

namespace tcmalloc::tcmalloc_internal {
namespace {

void FuzzBitmapPopBatch(const std::array<bool, 128>& bits_to_set,
                        size_t limit) {
  constexpr size_t N = 128;
  Bitmap<N> map1;
  Bitmap<N> map2;

  for (size_t i = 0; i < N; ++i) {
    if (bits_to_set[i]) {
      map1.SetBit(i);
      map2.SetBit(i);
    }
  }

  std::vector<size_t> offsets1, offsets2;
  size_t popped1 =
      map1.PopBatch([&](size_t v) { offsets1.push_back(v); }, limit);
  EXPECT_EQ(offsets1.size(), popped1);

  while (!map2.IsZero() && offsets2.size() < limit) {
    size_t offset = map2.FindSet(0);
    offsets2.push_back(offset);
    map2.ClearBit(offset);
  }

  EXPECT_THAT(offsets1, testing::Eq(offsets2));

  for (size_t i = 0; i < N; ++i) {
    EXPECT_EQ(map1.GetBit(i), map2.GetBit(i));
  }
  EXPECT_EQ(map1.IsZero(), map2.IsZero());
}

FUZZ_TEST(BitmapFuzzTest, FuzzBitmapPopBatch)
    .WithDomains(fuzztest::Arbitrary<std::array<bool, 128>>(),
                 fuzztest::InRange<size_t>(0, 128));

void FuzzBitmapCountBits(const std::array<bool, 253>& bits_to_set, size_t start,
                         size_t length) {
  constexpr size_t N = 253;
  if (start > N || start + length > N) {
    return;
  }

  Bitmap<N> map;
  for (size_t i = 0; i < N; ++i) {
    if (bits_to_set[i]) {
      map.SetBit(i);
    }
  }

  size_t expected = 0;
  for (size_t j = 0; j < length; j++) {
    size_t idx = start + j;
    if (bits_to_set[idx]) {
      expected++;
    }
  }

  EXPECT_EQ(expected, map.CountBits(start, length));
}

FUZZ_TEST(BitmapFuzzTest, FuzzBitmapCountBits)
    .WithDomains(fuzztest::Arbitrary<std::array<bool, 253>>(),
                 fuzztest::InRange<size_t>(0, 253),
                 fuzztest::InRange<size_t>(0, 253));

void FuzzBitmapCopyBits(const std::array<bool, 256>& src_bits,
                        const std::array<bool, 256>& dst_initial_bits,
                        size_t src_offset, size_t dst_offset, size_t length) {
  constexpr size_t N = 256;
  if (src_offset > N || src_offset + length > N || dst_offset > N ||
      dst_offset + length > N) {
    return;
  }

  Bitmap<N> src_map;
  Bitmap<N> dst_map;
  std::array<bool, N> expected_dst = dst_initial_bits;

  for (size_t i = 0; i < N; ++i) {
    if (src_bits[i]) {
      src_map.SetBit(i);
    }
    if (dst_initial_bits[i]) {
      dst_map.SetBit(i);
    }
  }

  for (size_t i = 0; i < length; ++i) {
    expected_dst[dst_offset + i] = src_bits[src_offset + i];
  }

  CopyBits(dst_map, dst_offset, src_map, src_offset, length);

  for (size_t i = 0; i < N; ++i) {
    EXPECT_EQ(dst_map.GetBit(i), expected_dst[i]);
  }
}

FUZZ_TEST(BitmapFuzzTest, FuzzBitmapCopyBits)
    .WithDomains(fuzztest::Arbitrary<std::array<bool, 256>>(),
                 fuzztest::Arbitrary<std::array<bool, 256>>(),
                 fuzztest::InRange<size_t>(0, 256),
                 fuzztest::InRange<size_t>(0, 256),
                 fuzztest::InRange<size_t>(0, 256));

void FuzzBitmapCopyBitsDifferentSizes(
    const std::array<bool, 127>& src_bits,
    const std::array<bool, 300>& dst_initial_bits, size_t src_offset,
    size_t dst_offset, size_t length) {
  constexpr size_t SrcN = 127;
  constexpr size_t DstN = 300;
  if (src_offset > SrcN || src_offset + length > SrcN || dst_offset > DstN ||
      dst_offset + length > DstN) {
    return;
  }

  Bitmap<SrcN> src_map;
  Bitmap<DstN> dst_map;
  std::array<bool, DstN> expected_dst = dst_initial_bits;

  for (size_t i = 0; i < SrcN; ++i) {
    if (src_bits[i]) {
      src_map.SetBit(i);
    }
  }
  for (size_t i = 0; i < DstN; ++i) {
    if (dst_initial_bits[i]) {
      dst_map.SetBit(i);
    }
  }

  for (size_t i = 0; i < length; ++i) {
    expected_dst[dst_offset + i] = src_bits[src_offset + i];
  }

  CopyBits(dst_map, dst_offset, src_map, src_offset, length);

  for (size_t i = 0; i < DstN; ++i) {
    EXPECT_EQ(dst_map.GetBit(i), expected_dst[i]);
  }
}

FUZZ_TEST(BitmapFuzzTest, FuzzBitmapCopyBitsDifferentSizes)
    .WithDomains(fuzztest::Arbitrary<std::array<bool, 127>>(),
                 fuzztest::Arbitrary<std::array<bool, 300>>(),
                 fuzztest::InRange<size_t>(0, 127),
                 fuzztest::InRange<size_t>(0, 300),
                 fuzztest::InRange<size_t>(0, 300));

void FuzzBitmapCopyBitsContractingSizes(
    const std::array<bool, 300>& src_bits,
    const std::array<bool, 127>& dst_initial_bits, size_t src_offset,
    size_t dst_offset, size_t length) {
  constexpr size_t SrcN = 300;
  constexpr size_t DstN = 127;
  if (src_offset > SrcN || src_offset + length > SrcN || dst_offset > DstN ||
      dst_offset + length > DstN) {
    return;
  }

  Bitmap<SrcN> src_map;
  Bitmap<DstN> dst_map;
  std::array<bool, DstN> expected_dst = dst_initial_bits;

  for (size_t i = 0; i < SrcN; ++i) {
    if (src_bits[i]) {
      src_map.SetBit(i);
    }
  }
  for (size_t i = 0; i < DstN; ++i) {
    if (dst_initial_bits[i]) {
      dst_map.SetBit(i);
    }
  }

  for (size_t i = 0; i < length; ++i) {
    expected_dst[dst_offset + i] = src_bits[src_offset + i];
  }

  CopyBits(dst_map, dst_offset, src_map, src_offset, length);

  for (size_t i = 0; i < DstN; ++i) {
    EXPECT_EQ(dst_map.GetBit(i), expected_dst[i]);
  }
}

FUZZ_TEST(BitmapFuzzTest, FuzzBitmapCopyBitsContractingSizes)
    .WithDomains(fuzztest::Arbitrary<std::array<bool, 300>>(),
                 fuzztest::Arbitrary<std::array<bool, 127>>(),
                 fuzztest::InRange<size_t>(0, 300),
                 fuzztest::InRange<size_t>(0, 127),
                 fuzztest::InRange<size_t>(0, 127));

}  // namespace
}  // namespace tcmalloc::tcmalloc_internal
