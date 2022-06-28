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
namespace {

inline void PossiblyTouchAllocated(void* allocated, bool touch_allocated) {
  uint8_t* access_ptr = static_cast<uint8_t*>(allocated);
  if (touch_allocated) {
    *access_ptr = 0;
    // Prevent the compiler from register optimizing the load by telling it that
    // access_ptr might change
    asm volatile("" : [access_ptr] "+r"(access_ptr));
    auto byte = *(access_ptr);
    // Prevent the compiler from removing the load since the result is unused
    asm volatile("" : : [byte] "r"(byte));
  }
}

}  // namespace

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
                             absl::FunctionRef<void*(size_t)> alloc,
                             absl::FunctionRef<void(void*, size_t)> dealloc,
                             bool record_and_replay_mode, bool touch_allocated)
    : rng_(absl::SeedSeq{seed}),
      alloc_(alloc),
      dealloc_(dealloc),
      usage_(0),
      num_live_(0),
      total_num_allocated_(0),
      total_bytes_allocated_(0),
      birth_sampler_(BirthRateDistribution(weights)),
      total_birth_rate_(0),
      death_sampler_(weights.size()),
      touch_allocated_(touch_allocated) {
  // First, compute average live count for each size in a heap of size
  // <total_mem>.
  double total = 0;
  for (const auto& w : weights) {
    total += w.num_live * w.size;
  }
  const double scale = total_mem / total;
  std::vector<double> avg_counts;
  // now sum(w.num_live * scale * w.size) = total_mem as desired.
  for (const auto& w : weights) {
    const double count = w.num_live * scale;
    avg_counts.push_back(count);
  }

  // Little's Law says that avg_count = birth rate * (average lifespan).
  // We want birth rates to match the input weights (any absolute rate will do,
  // we have one gauge parameter which is the rate virtual time passes:
  // so we can use the absolute rates given without normalization.
  // We just then have to pick lifespans to match the desired avg_count.
  for (int i = 0; i < weights.size(); ++i) {
    const double avg_count = avg_counts[i];
    const Entry& w = weights[i];
    total_birth_rate_ += w.alloc_rate;
    const double lifespan = avg_count / w.alloc_rate;
    const double death_rate = 1 / lifespan;
    state_.push_back({w.size, w.alloc_rate, death_rate, 0, {}});
    state_.back().objs.reserve(avg_count * 2);
  }

  // Now, we generate the initial sample. We have to sample from the *live*
  // distribution, not the *allocation* one, so we can't use the birth sampler;
  // make a new one from the live counts.  (We could run the full birth/death
  // loop until we made it up to size but that's much slower.)
  absl::discrete_distribution<int> live_dist(avg_counts.begin(),
                                             avg_counts.end());

  while (usage_ < total_mem) {
    int i = live_dist(rng_);
    DoBirth(i);
  }

  if (record_and_replay_mode) {
    SnapshotLiveObjects();
  }

  for (auto& s : state_) {
    // Don't count initial sample towards allocations (skews data).
    s.total = 0;
  }
}

EmpiricalData::~EmpiricalData() {
  for (auto& s : state_) {
    const size_t size = s.size;
    for (auto p : s.objs) {
      dealloc_(p, size);
    }
  }
}

void* EmpiricalData::DoBirth(const size_t i) {
  SizeState& s = state_[i];
  // We have an extra live object, so the overall death rate goes up.
  death_sampler_.AdjustWeight(i, s.death_rate);
  const size_t size = s.size;
  usage_ += size;
  total_num_allocated_++;
  total_bytes_allocated_ += size;
  num_live_++;
  void* p = alloc_(size);
  s.objs.push_back(p);
  s.total++;
  return p;
}

void EmpiricalData::DoDeath(const size_t i) {
  SizeState& s = state_[i];
  CHECK_CONDITION(!s.objs.empty());
  const int obj =
      absl::uniform_int_distribution<int>(0, s.objs.size() - 1)(rng_);
  death_sampler_.AdjustWeight(i, -s.death_rate);
  const size_t size = s.size;
  usage_ -= size;
  num_live_--;
  void* p = s.objs[obj];
  s.objs[obj] = s.objs.back();
  s.objs.pop_back();
  dealloc_(p, size);
}

void EmpiricalData::RecordBirth(const size_t i) {
  birth_or_death_sizes_.push_back(i);
  SizeState& s = state_[i];
  death_sampler_.AdjustWeight(i, s.death_rate);
  // We only care about keeping the number of objects correct when building the
  // trace.  When we replay we will actually push the allocated address but
  // when building the trace we can just push nullptr to keep the length of live
  // object lists consistent with what it should have been after a true birth.
  s.objs.push_back(nullptr);
  s.total++;
}

void* EmpiricalData::ReplayBirth(const size_t i) {
  SizeState& s = state_[i];
  const size_t size = s.size;
  usage_ += size;
  total_num_allocated_++;
  total_bytes_allocated_ += size;
  num_live_++;
  void* p = alloc_(size);
  s.objs.push_back(p);
  s.total++;
  return p;
}

void EmpiricalData::RecordDeath(const size_t i) {
  SizeState& s = state_[i];
  CHECK_CONDITION(!s.objs.empty());
  birth_or_death_sizes_.push_back(i);
  auto to_free = absl::uniform_int_distribution<int>(
      0, std::max(0, static_cast<int>(s.objs.size()) - 1))(rng_);
  death_sampler_.AdjustWeight(i, -s.death_rate);
  s.objs[to_free] = s.objs.back();
  s.objs.pop_back();
  death_objects_.push_back(to_free);
}

void EmpiricalData::ReplayDeath(const size_t i, uint64_t index) {
  SizeState& s = state_[i];
  CHECK_CONDITION(!s.objs.empty());
  void* p = s.objs[index];
  s.objs[index] = s.objs.back();
  s.objs.pop_back();
  dealloc_(p, s.size);
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
    void* allocated = DoBirth(i);
    PossiblyTouchAllocated(allocated, touch_allocated_);
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

void EmpiricalData::RecordNext() {
  const double B = total_birth_rate_;
  const double T = death_sampler_.TotalWeight();
  const double Both = B + T;
  absl::uniform_real_distribution<double> which(0, Both);
  bool do_birth = which(rng_) < B;
  birth_or_death_.push_back(do_birth);

  if (do_birth) {
    size_t i = birth_sampler_(rng_);
    RecordBirth(i);
  } else {
    size_t i = death_sampler_(rng_);
    RecordDeath(i);
  }
}

void EmpiricalData::ReplayNext() {
  bool do_birth = birth_or_death_[birth_or_death_index_];
  if (do_birth) {
    void* allocated = ReplayBirth(birth_or_death_sizes_[birth_or_death_index_]);
    PossiblyTouchAllocated(allocated, touch_allocated_);
  } else {
    ReplayDeath(birth_or_death_sizes_[birth_or_death_index_],
                death_objects_[death_object_index_]);
    __builtin_prefetch(death_object_pointers_[death_object_index_], 1, 3);
    death_object_index_++;
  }
  birth_or_death_index_++;
}

void EmpiricalData::SnapshotLiveObjects() {
  for (const auto& s : state_) {
    snapshot_state_.push_back(
        {s.size, s.birth_rate, s.death_rate, s.total, s.objs});
  }
}

void EmpiricalData::RestoreSnapshot() {
  for (int i = 0; i < snapshot_state_.size(); i++) {
    state_[i].objs = snapshot_state_[i].objs;
  }
}

void EmpiricalData::ReserveSizeClassObjects() {
  // Keep a running sum and high water mark for the delta in the size class
  // object arrays.
  std::vector<int32_t> max_object_size_delta(state_.size(), 0);
  std::vector<int32_t> cur_object_size_delta(state_.size(), 0);
  for (int i = 0; i < birth_or_death_.size(); i++) {
    auto size_class = birth_or_death_sizes_[i];
    if (birth_or_death_[i]) {
      cur_object_size_delta[size_class]++;
      max_object_size_delta[size_class] = std::max(
          max_object_size_delta[size_class], cur_object_size_delta[size_class]);
    } else {
      cur_object_size_delta[size_class]--;
    }
  }

  for (int i = 0; i < state_.size(); i++) {
    state_[i].objs.reserve(state_[i].objs.size() + max_object_size_delta[i]);
  }
}

void EmpiricalData::BuildDeathObjectPointers() {
  constexpr uint32_t kPrefetchDistance = 64;

  // This is a bit ugly but because the below code can create pointers past the
  // end of the current objects arrays we need to first need to reserve their
  // capacity at the maximum capacity they will ever hit to ensure they won't
  // grow and possibly be reallocated.  They will never grow beyond the size
  // calculated by this function.
  ReserveSizeClassObjects();

  // The easiest way to compute the prefetch objects is to get the pointers
  // corresponding to each death_objects_[] and then rotating the array so the
  // N + prefetch_distance object is stored at index N.
  uint32_t death_index = 0;
  for (int i = 0; i < birth_or_death_.size(); i++) {
    // Skip births
    if (birth_or_death_[i]) {
      continue;
    }
    SizeState& s = state_[birth_or_death_sizes_[i]];
    death_object_pointers_.push_back(&s.objs[death_objects_[death_index++]]);
  }
  std::rotate(death_object_pointers_.begin(),
              death_object_pointers_.begin() + kPrefetchDistance,
              death_object_pointers_.end());
}

void EmpiricalData::RepairToSnapshotState() {
  // Compared to the number of live objects when the snapshot was taken each
  // size state either
  // 1) Contains the same number of live objects as when the snapshot was taken,
  //    requiring no action.
  // 2) Contains a smaller number of live objects, requiring a (likely small)
  //    number of true allocations.
  // 3) Contains a larger number of live objects, requiring a (likely small)
  //    number of true deallocations.
  for (int i = 0; i < state_.size(); i++) {
    while (state_[i].objs.size() < snapshot_state_[i].objs.size()) {
      DoBirth(i);
    }
    while (state_[i].objs.size() > snapshot_state_[i].objs.size()) {
      DoDeath(i);
    }
  }
}

void EmpiricalData::RestartTraceIfNecessary() {
  if (birth_or_death_index_ == birth_or_death_.size()) {
    // As the snapshotted lists of live objects will contain addresses which
    // have already been freed we can't just call RestoreSnapshot().  Instead
    // let's do the necessary allocations / deallocations to end up with the
    // identical number of live objects we had when initially building the
    // trace.
    RepairToSnapshotState();
    // After the above call we can safely run through the recorded trace
    // again.
    birth_or_death_index_ = 0;
    death_object_index_ = 0;
  }
}

std::vector<EmpiricalData::Entry> EmpiricalData::Actual() const {
  std::vector<Entry> data;
  data.reserve(state_.size());
  for (const auto& s : state_) {
    data.push_back({s.size, static_cast<double>(s.total),
                    static_cast<double>(s.objs.size())});
  }
  return data;
}

}  // namespace tcmalloc
