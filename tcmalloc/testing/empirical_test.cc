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

#include "tcmalloc/testing/empirical.h"

#include <algorithm>
#include <new>
#include <string>
#include <tuple>

#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "absl/random/random.h"
#include "absl/strings/str_cat.h"
#include "tcmalloc/testing/testutil.h"

// Koenig lookup
namespace tcmalloc {
void PrintTo(const EmpiricalData::Entry &e, ::std::ostream *os) {
  *os << "{" << e.size << " bytes, " << e.alloc_rate << "/" << e.num_live
      << "}";
}

namespace {

void *alloc(size_t s) { return ::operator new(s); }

using testing::Pointwise;

const std::vector<EmpiricalData::Entry> &dummy() {
  static std::vector<EmpiricalData::Entry> e = {{8, 1000, 100 * 1000},
                                                {16, 1000, 1000},
                                                {64, 100, 1000},
                                                {64 * 1024, 10, 800}};

  return e;
}

MATCHER(EntrySizeEq, "have equal size") {
  return std::get<0>(arg).size == std::get<1>(arg).size;
}

std::vector<double> Normalize(const std::vector<double> &xs) {
  double total = 0;
  for (double x : xs) {
    total += x;
  }
  double inv = 1 / total;
  std::vector<double> ret;
  for (double x : xs) {
    ret.push_back(x * inv);
  }

  return ret;
}

std::vector<double> GetRates(const std::vector<EmpiricalData::Entry> &es) {
  std::vector<double> ret;
  for (auto e : es) {
    ret.push_back(e.alloc_rate);
  }

  return ret;
}

std::vector<double> GetCounts(const std::vector<EmpiricalData::Entry> &es) {
  std::vector<double> ret;
  for (auto e : es) {
    ret.push_back(e.num_live);
  }

  return ret;
}

MATCHER_P(DoubleRelEq, err,
          absl::StrCat(negation ? "aren't" : "are", " within ",
                       absl::SixDigits(err * 100), "% of each other")) {
  double a = std::get<0>(arg);
  double b = std::get<1>(arg);
  return (std::max(a / b, b / a) - 1) < err;
}

TEST(Empirical, Basic) {
  size_t kSize = 128 * 1024 * 1024;
  auto const &expected = dummy();
  absl::BitGen rng;
  EmpiricalData data(absl::Uniform<uint32_t>(rng), expected, kSize, alloc,
                     sized_delete);

  for (int i = 0; i < 100; ++i) {
    for (int j = 0; j < 100 * 1000; ++j) {
      data.Next();
    }

    SCOPED_TRACE(absl::StrCat("Rep ", i));
    EXPECT_THAT(std::make_tuple(data.usage(), kSize), DoubleRelEq(0.2));
    auto actual = data.Actual();
    // check basic form of return
    ASSERT_THAT(actual, Pointwise(EntrySizeEq(), expected));
    ASSERT_THAT(Normalize(GetRates(actual)),
                Pointwise(DoubleRelEq(0.35), Normalize(GetRates(expected))));

    ASSERT_THAT(Normalize(GetCounts(actual)),
                Pointwise(DoubleRelEq(0.2), Normalize(GetCounts(expected))));
  }
}

}  // namespace
}  // namespace tcmalloc
