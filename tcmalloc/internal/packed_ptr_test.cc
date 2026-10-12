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

#include "tcmalloc/internal/packed_ptr.h"

#include <cstdint>

#include "gtest/gtest.h"

namespace tcmalloc {
namespace tcmalloc_internal {
namespace {

struct alignas(2) Aligned2 {
  uint16_t value = 0;
};

struct alignas(64) Aligned64 {
  uint64_t value = 0;
};

TEST(PackedPtrTest, DefaultConstructed) {
  PackedPtr<Aligned2> p1;
  EXPECT_EQ(p1.Get(), nullptr);
  EXPECT_EQ(p1.Bits(), 0u);
  EXPECT_EQ(p1.Raw(), nullptr);

  PackedPtr<Aligned64, 6> p6;
  EXPECT_EQ(p6.Get(), nullptr);
  EXPECT_EQ(p6.Bits(), 0u);
  EXPECT_EQ(p6.Raw(), nullptr);
}

TEST(PackedPtrTest, UnpackedPointerConstructor) {
  Aligned2 item2;
  PackedPtr<Aligned2> p1(&item2);
  EXPECT_EQ(p1.Get(), &item2);
  EXPECT_EQ(p1.Bits(), 0u);

  Aligned64 item64;
  PackedPtr<Aligned64, 6> p6(&item64);
  EXPECT_EQ(p6.Get(), &item64);
  EXPECT_EQ(p6.Bits(), 0u);
}

TEST(PackedPtrTest, SingleBitRoundTripsThroughRaw) {
  Aligned2 items[3];
  for (Aligned2& item : items) {
    for (uintptr_t bits : {uintptr_t{0}, uintptr_t{1}}) {
      PackedPtr<Aligned2, 1> encoded(&item, bits);
      EXPECT_EQ(encoded.Get(), &item);
      EXPECT_EQ(encoded.Bits(), bits);

      PackedPtr<Aligned2, 1> decoded(encoded.Raw());
      EXPECT_EQ(decoded.Get(), &item);
      EXPECT_EQ(decoded.Bits(), bits);
    }
  }
}

TEST(PackedPtrTest, MultiBitRoundTripsThroughRaw) {
  Aligned64 items[3];
  for (uintptr_t bits = 0; bits < (uintptr_t{1} << 6); ++bits) {
    PackedPtr<Aligned64, 6> null_encoded(nullptr, bits);
    EXPECT_EQ(null_encoded.Get(), nullptr);
    EXPECT_EQ(null_encoded.Bits(), bits);

    PackedPtr<Aligned64, 6> null_decoded(null_encoded.Raw());
    EXPECT_EQ(null_decoded.Get(), nullptr);
    EXPECT_EQ(null_decoded.Bits(), bits);

    for (Aligned64& item : items) {
      PackedPtr<Aligned64, 6> encoded(&item, bits);
      EXPECT_EQ(encoded.Get(), &item);
      EXPECT_EQ(encoded.Bits(), bits);

      PackedPtr<Aligned64, 6> decoded(encoded.Raw());
      EXPECT_EQ(decoded.Get(), &item);
      EXPECT_EQ(decoded.Bits(), bits);
    }
  }
}

}  // namespace
}  // namespace tcmalloc_internal
}  // namespace tcmalloc
