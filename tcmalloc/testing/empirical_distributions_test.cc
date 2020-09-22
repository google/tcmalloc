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

#include "tcmalloc/testing/empirical_distributions.h"

#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "absl/random/random.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_format.h"

namespace tcmalloc {
namespace {

std::vector<double> Normalize(absl::Span<const double> xs) {
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

// Not really a test, just summary stats for our distributions.

std::string Summarize(const EmpiricalProfile& data) {
  double alloc_rate = 0;
  double num_live = 0;
  double bytes = 0;
  for (const auto& e : data) {
    alloc_rate += e.alloc_rate;
    num_live += e.num_live;
    bytes += e.num_live * e.size;
  }

  return absl::StrFormat("%zu sizes, %e objs/s, %e objs per heap (%e)",
                         data.size(), alloc_rate, num_live, bytes);
}

TEST(Empirical, Stats) {
  absl::PrintF("beta: %s\n", Summarize(empirical_distributions::Beta()));
  absl::PrintF("bravo: %s\n", Summarize(empirical_distributions::Bravo()));
  absl::PrintF("charlie: %s\n", Summarize(empirical_distributions::Charlie()));
  absl::PrintF("echo: %s\n", Summarize(empirical_distributions::Echo()));
  absl::PrintF("merced: %s\n", Summarize(empirical_distributions::Merced()));
  absl::PrintF("sierra: %s\n", Summarize(empirical_distributions::Sierra()));
  absl::PrintF("sigma: %s\n", Summarize(empirical_distributions::Sigma()));
  absl::PrintF("uniform: %s\n", Summarize(empirical_distributions::Uniform()));
}

TEST(DistributionTest, AdjustableSampler) {
  std::vector<double> weights;
  for (auto i : empirical_distributions::Merced()) {
    weights.push_back(i.alloc_rate);
  }

  const size_t N = weights.size();
  AdjustableSampler mine(weights);
  AdjustableSampler mine_zeroes(N);
  for (size_t i = 0; i < N; ++i) {
    mine_zeroes.AdjustWeight(i, weights[i]);
  }
  EXPECT_EQ(mine, mine_zeroes);

  // Make sure the above works for the trivial distribution.
  AdjustableSampler s1(1);
  s1.SetWeight(0, 3.07);
  AdjustableSampler s2(std::vector<double>({3.07}));
  EXPECT_EQ(s1, s2);

  std::vector<double> counts(N, 0);
  absl::BitGen rng;
  for (int i = 0; i < 5 * 1000 * 1000; ++i) {
    counts[mine(rng)]++;
    EXPECT_EQ(0, s1(rng));
  }

  // could do much better stat testing
  auto nweights = Normalize(weights);
  auto ncounts = Normalize(counts);
  for (size_t i = 0; i < N; ++i) {
    double ratio = static_cast<double>(ncounts[i] / nweights[i]);
    ratio = std::max(ratio, 1 / ratio);
    if (counts[i] > 1000) {
      EXPECT_LE(ratio, 1.25);
    }
  }
}

}  // namespace
}  // namespace tcmalloc
