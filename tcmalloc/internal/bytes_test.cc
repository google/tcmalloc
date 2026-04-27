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

#include "tcmalloc/internal/bytes.h"

#include "benchmark/benchmark.h"
#include "gtest/gtest.h"

namespace tcmalloc {
namespace tcmalloc_internal {
namespace {

TEST(BytesTest, ImplicitConversion) {
  Bytes b(10);
  EXPECT_EQ(b.raw_num(), 10);
}

TEST(BytesTest, Arithmetic) {
  Bytes b1(10);
  Bytes b2(20);

  Bytes b3 = b1 + b2;
  EXPECT_EQ(b3.raw_num(), 30);

  b1 += b2;
  EXPECT_EQ(b1.raw_num(), 30);

  Bytes b4 = b2 - Bytes(5);
  EXPECT_EQ(b4.raw_num(), 15);

  b2 -= Bytes(5);
  EXPECT_EQ(b2.raw_num(), 15);

  Bytes b5 = b1 * 2;
  EXPECT_EQ(b5.raw_num(), 60);

  Bytes b6 = 2 * b1;
  EXPECT_EQ(b6.raw_num(), 60);

  b1 *= 2;
  EXPECT_EQ(b1.raw_num(), 60);

  Bytes b7 = b1 / 2;
  EXPECT_EQ(b7.raw_num(), 30);

  b1 /= 2;
  EXPECT_EQ(b1.raw_num(), 30);
}

TEST(BytesTest, Comparison) {
  Bytes b1(10);
  Bytes b2(20);

  EXPECT_LT(b1, b2);
  EXPECT_LE(b1, b2);
  EXPECT_GT(b2, b1);
  EXPECT_GE(b2, b1);
  EXPECT_NE(b1, b2);
  EXPECT_EQ(b1, Bytes(10));
}

#ifndef NDEBUG
TEST(BytesDeathTest, Overflow) {
  Bytes b = Bytes::max();
  EXPECT_DEATH(b += Bytes(1), "CHECK");
  EXPECT_DEATH(b *= 2, "CHECK");
}

TEST(BytesDeathTest, Underflow) {
  Bytes b(10);
  EXPECT_DEATH(b -= Bytes(11), "CHECK");
}

TEST(BytesDeathTest, DivideByZero) {
  Bytes lhs(5), rhs(0);
  size_t s = 0;

  benchmark::DoNotOptimize(rhs);
  benchmark::DoNotOptimize(s);

  Bytes result;
  size_t sresult = 0;

  EXPECT_DEATH({ sresult = lhs / rhs; }, "CHECK");
  EXPECT_DEATH({ result = lhs % rhs; }, "CHECK");
  EXPECT_DEATH({ result = lhs / s; }, "CHECK");

  benchmark::DoNotOptimize(result);
  benchmark::DoNotOptimize(sresult);
}
#endif

}  // namespace
}  // namespace tcmalloc_internal
}  // namespace tcmalloc
