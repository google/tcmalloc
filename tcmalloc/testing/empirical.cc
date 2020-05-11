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

#include <memory>
#include <vector>

#include "absl/random/discrete_distribution.h"
#include "absl/random/seed_sequences.h"
#include "absl/random/uniform_int_distribution.h"

// Implementations of functions.
namespace tcmalloc {

static absl::discrete_distribution<size_t> BirthRateDistribution(
    const absl::Span<const EmpiricalData::Entry> weights) {
  std::vector<double> v;
  v.reserve(weights.size());
  for (auto w : weights) {
    v.push_back(w.alloc_rate);
  }

  return absl::discrete_distribution<size_t>(v.begin(), v.end());
}

EmpiricalData::EmpiricalData(size_t seed, const absl::Span<const Entry> weights,
                             size_t total_mem,
                             absl::FunctionRef<void *(size_t)> alloc,
                             absl::FunctionRef<void(void *, size_t)> dealloc)
    : rng_(absl::SeedSeq{seed}),
      alloc_(alloc),
      dealloc_(dealloc),
      usage_(0),
      num_live_(0),
      total_num_allocated_(0),
      total_bytes_allocated_(0),
      birth_sampler_(BirthRateDistribution(weights)),
      total_birth_rate_(0),
      death_sampler_(weights.size()) {
  // First, compute average live count for each size in a heap of size
  // <total_mem>.
  double total = 0;
  for (const auto &w : weights) {
    total += w.num_live * w.size;
  }
  const double scale = total_mem / total;
  std::vector<double> avg_counts;
  double total_avg_counts = 0;
  // now sum(w.num_live * scale * w.size) = total_mem as desired.
  for (const auto &w : weights) {
    const double count = w.num_live * scale;
    avg_counts.push_back(count);
    total_avg_counts += count;
  }

  // Little's Law says that avg_count = birth rate * (average lifespan).
  // We want birth rates to match the input weights (any absolute rate will do,
  // we have one gauge parameter which is the rate virtual time passes:
  // so we can use the absolute rates given without normalization.
  // We just then have to pick lifespans to match the desired avg_count.
  for (int i = 0; i < weights.size(); ++i) {
    const double avg_count = avg_counts[i];
    const Entry &w = weights[i];
    total_birth_rate_ += w.alloc_rate;
    const double lifespan = avg_count / w.alloc_rate;
    const double death_rate = 1 / lifespan;
    state_.push_back({w.size, w.alloc_rate, death_rate, 0, {}});
    state_.back().objs.reserve(avg_count * 2);
  }

  // Now, we generate the initial sample. We have to sample from the *live*
  // distribution, not the *allocation* one, so we can't use the birth sampler;
  // make a new one from the live counts.  (We could run the full birth/death
  // loop until we made it up to size but that's mucah slower.)
  absl::discrete_distribution<int> live_dist(avg_counts.begin(),
                                             avg_counts.end());

  while (usage_ < total_mem) {
    int i = live_dist(rng_);
    DoBirth(i);
  }

  for (auto &s : state_) {
    // Don't count initial sample towards allocations (skews data).
    s.total = 0;
  }
}

EmpiricalData::~EmpiricalData() {
  for (auto &s : state_) {
    const size_t size = s.size;
    for (auto p : s.objs) {
      dealloc_(p, size);
    }
  }
}

void EmpiricalData::DoBirth(const size_t i) {
  SizeState &s = state_[i];
  // We have an extra live object, so the overall death rate goes up.
  death_sampler_.AdjustWeight(i, s.death_rate);
  const size_t size = s.size;
  usage_ += size;
  total_num_allocated_++;
  total_bytes_allocated_ += size;
  num_live_++;
  void *p = alloc_(size);
  s.objs.push_back(p);
  s.total++;
}

void EmpiricalData::DoDeath(const size_t i) {
  SizeState &s = state_[i];
  CHECK_CONDITION(!s.objs.empty());
  const int obj =
      absl::uniform_int_distribution<int>(0, s.objs.size() - 1)(rng_);
  death_sampler_.AdjustWeight(i, -s.death_rate);
  const size_t size = s.size;
  usage_ -= size;
  num_live_--;
  void *p = s.objs[obj];
  s.objs[obj] = s.objs.back();
  s.objs.pop_back();
  dealloc_(p, size);
}

void EmpiricalData::Next() {
  // The code here is very simple, but the logic is complicated. We rely on
  // three key facts about independent distributions Exp(A) and Exp(B) (that is,
  // exponential distributions with rate parameters A and B.)
  // (1) Memorylessness: p(Exp(A) = T + t | Exp(A) > T) ~ Exp(A) (in t)
  // (2) min(Exp(A), Exp(B)) ~ Exp(A + B) = Exp(C)
  // (3) p(Exp(C) = Exp(A)  | Exp(C) = t) = A / C = A / (A + B)
  // (These generalize obviously to multiple exponentials.)
  //
  // Objects of size i are being born at rate b_i; that means
  // births arrive at rate B = Sum(b_i).
  const double B = total_birth_rate_;
  // *right now* we have n_i objects of size i, each dying at rate d_i; so
  // objects of size i are dying (overall) at rate t_i, and
  // total deaths arrive at rate T = Sum(t_i).
  const double T = death_sampler_.TotalWeight();
  // So (2) tells us with probability B/(B + T), the next action is a birth.
  // Otherwise it is a death. (3) lets us handle each independently.
  const double Both = B + T;
  // TODO(ahh): make absl::bernoulli distribution support ratio probabilities
  // efficiently (no divide).
  absl::uniform_real_distribution<double> which(0, Both);
  if (which(rng_) < B) {
    // Birth :)
    // (3) says that our birth came from size i with probability prop. to b_i.
    size_t i = birth_sampler_(rng_);
    DoBirth(i);
  } else {
    // Death :(
    // We maintain death_sampler_.weight(i) = t_i = n_i * d_i.
    // (3) says that the death comes from size i with probability prop. to t_i,
    // and furthmore that we can pick the dead object uniformly.
    size_t i = death_sampler_(rng_);
    DoDeath(i);
  }
  // (1) says we are now done and can re-sample the next event independently.
}

std::vector<EmpiricalData::Entry> EmpiricalData::Actual() const {
  std::vector<Entry> data;
  for (const auto &s : state_) {
    data.push_back({s.size, static_cast<double>(s.total),
                    static_cast<double>(s.objs.size())});
  }
  return data;
}

}  // namespace tcmalloc
