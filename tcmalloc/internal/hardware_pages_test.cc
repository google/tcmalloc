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

#include "tcmalloc/internal/hardware_pages.h"

#include <cstddef>
#include <string>

#include "benchmark/benchmark.h"
#include "gtest/gtest.h"
#include "absl/strings/str_cat.h"
#include "tcmalloc/internal/page_size.h"

namespace tcmalloc {
namespace tcmalloc_internal {
namespace {

TEST(HardwareLengthTest, Value) {
  HardwareLength hl(10);
  EXPECT_EQ(hl.raw_num(), 10);
  EXPECT_EQ(hl.in_bytes(), 10 * GetPageSize());
}

TEST(HardwareLengthTest, LiteralConstructor) {
  HardwareLength hl = NHardwarePages(5);
  EXPECT_EQ(hl.raw_num(), 5);
}

TEST(HardwareLengthTest, Arithmetic) {
  HardwareLength hl1(10);
  HardwareLength hl2(20);

  HardwareLength hl3 = hl1 + hl2;
  EXPECT_EQ(hl3.raw_num(), 30);

  hl1 += hl2;
  EXPECT_EQ(hl1.raw_num(), 30);

  HardwareLength hl4 = hl2 - HardwareLength(5);
  EXPECT_EQ(hl4.raw_num(), 15);

  hl2 -= HardwareLength(5);
  EXPECT_EQ(hl2.raw_num(), 15);

  HardwareLength hl5 = hl1 * 2;
  EXPECT_EQ(hl5.raw_num(), 60);

  HardwareLength hl6 = 2 * hl1;
  EXPECT_EQ(hl6.raw_num(), 60);

  hl1 *= 2;
  EXPECT_EQ(hl1.raw_num(), 60);

  HardwareLength hl7 = hl1 / 2;
  EXPECT_EQ(hl7.raw_num(), 30);

  hl1 /= 2;
  EXPECT_EQ(hl1.raw_num(), 30);

  EXPECT_EQ(HardwareLength(10) / HardwareLength(2), 5);
  EXPECT_EQ((HardwareLength(11) % HardwareLength(3)).raw_num(), 2);
}

TEST(HardwareLengthTest, IncrementDecrement) {
  HardwareLength hl(5);
  EXPECT_EQ((++hl).raw_num(), 6);
  EXPECT_EQ((hl++).raw_num(), 6);
  EXPECT_EQ(hl.raw_num(), 7);

  EXPECT_EQ((--hl).raw_num(), 6);
  EXPECT_EQ((hl--).raw_num(), 6);
  EXPECT_EQ(hl.raw_num(), 5);
}

TEST(HardwareLengthTest, Comparison) {
  HardwareLength hl1(10);
  HardwareLength hl2(20);

  EXPECT_LT(hl1, hl2);
  EXPECT_LE(hl1, hl2);
  EXPECT_GT(hl2, hl1);
  EXPECT_GE(hl2, hl1);
  EXPECT_NE(hl1, hl2);
  EXPECT_EQ(hl1, HardwareLength(10));
}

TEST(HardwareLengthTest, AbslStringify) {
  HardwareLength hl(42);
  EXPECT_EQ(absl::StrCat(hl), "42");
}

TEST(HardwareLengthTest, FlagSupport) {
  HardwareLength hl;
  std::string error;
  EXPECT_TRUE(AbslParseFlag("123", &hl, &error));
  EXPECT_EQ(hl.raw_num(), 123);
  EXPECT_EQ(AbslUnparseFlag(hl), "123");
  EXPECT_FALSE(AbslParseFlag("invalid", &hl, &error));
}

#ifndef NDEBUG
TEST(HardwareLengthDeathTest, Overflow) {
  HardwareLength hl = HardwareLength::max();
  EXPECT_DEATH(hl += HardwareLength(1), "CHECK");
  EXPECT_DEATH(hl *= 2, "CHECK");
}

TEST(HardwareLengthDeathTest, Underflow) {
  HardwareLength hl(10);
  EXPECT_DEATH(hl -= HardwareLength(11), "CHECK");
}

TEST(HardwareLengthDeathTest, DivideByZero) {
  HardwareLength lhs(5), rhs(0);
  size_t s = 0;

  benchmark::DoNotOptimize(rhs);
  benchmark::DoNotOptimize(s);

  HardwareLength result;
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
