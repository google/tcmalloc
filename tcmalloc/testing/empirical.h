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
//
// Generate load for malloc based on an empirical distribution of size
// and lifetimes.  That is, given samples telling us that (for a given
// binary, say) we see 16-byte mallocs at 10 Hz and 128-byte at 5 Hz,
// and that 90% of live objects are 16-byte objects, we can generate a
// sequence of calls to malloc and free which replicate both of those
// properties.
//
// Note that
//   a) we do this only in a relative sense: we can't guarantee a precise
//      rate of allocation (that depends on cost of malloc, cpu time,
//      etc, etc, etc.) We can guarantee we make twice as many malloc(16)
//      calls as malloc(128)s.
//
//   b) We pick one simple distribution that has these properties (and is
//      nicely randomized, is a difficult malloc test, etc.) It is by no
//      means unique; it by no means replicates the app's behavior exactly.
//
//   c) We make no attempt to replicate inter-thread/inter-CPU
//      behavior (producer/consumer, etc). This class simply creates
//      the behavior we like local to a single thread.

#ifndef TCMALLOC_TESTING_EMPIRICAL_H_
#define TCMALLOC_TESTING_EMPIRICAL_H_

#include <stddef.h>

#include <iterator>
#include <vector>

#include "absl/algorithm/container.h"
#include "absl/functional/function_ref.h"
#include "absl/random/discrete_distribution.h"
#include "absl/random/random.h"
#include "absl/random/uniform_real_distribution.h"
#include "absl/types/span.h"
#include "tcmalloc/internal/logging.h"

namespace tcmalloc {

// This is like WeightedPicker from util/random--an arbitrary discrete
// distribution, which can efficiently change the probability of any given item
// but:
// - faster
// - supports floating point weights.
class AdjustableSampler {
 public:
  // weights are all zeroes
  explicit AdjustableSampler(size_t n)
      : n_(NextPowerOfTwo(n)), tree_(2 * n_ - 1, 0) {
    CHECK_CONDITION(n >= 1);
  }

  explicit AdjustableSampler(const std::vector<double> &weights)
      : n_(NextPowerOfTwo(weights.size())), tree_(2 * n_ - 1, 0) {
    CHECK_CONDITION(!weights.empty());
    absl::c_copy(weights, tree_.begin() + (n_ - 1));

    size_t i = n_ - 1;
    // post decrement
    while (i-- > 0) {
      tree_[i] = tree_[2 * i + 1] + tree_[2 * i + 2];
    }
  }

  // Sample from the distribution on {0, 1, ..., n_ - 1} with probabilities:
  // p_i = w_i / Sum(w_i)
  template <typename Generator>
  size_t operator()(Generator &g) const {  // NOLINT(runtime/references)
    return SampleWeight(
        absl::uniform_real_distribution<double>(0, tree_[0])(g));
  }

  void AdjustWeight(size_t i, double delta) { SetWeight(i, weight(i) + delta); }

  void SetWeight(size_t i, double x) {
    i += n_ - 1;
    tree_[i] = x;
    while (i != 0) {
      i = (i - 1) / 2;
      tree_[i] = tree_[2 * i + 1] + tree_[2 * i + 2];
    }
  }

  double weight(size_t i) const { return tree_[i + n_ - 1]; }

  double TotalWeight() const { return tree_[0]; }

  bool operator==(const AdjustableSampler &rhs) const {
    return n_ == rhs.n_ && tree_ == rhs.tree_;
  }

 private:
  const size_t n_;

  // If x is a uniform number in [0, Sum(w_i)), samples the above distribution.
  size_t SampleWeight(double x) const {
    // We want to efficiently sample the above distribution, *and* be able
    // to change the weights that determine it.  We do this by forming a tree
    // where each leaf is a value (and its weight), and each internal node
    // has the sum of the weights of its descendants.  It is easy to update
    // this tree as weights change.
    //
    // The key invariant: each subtree's weight, divided by the total tree
    // weight, is exactly the probability a sample lands within that subtree.
    //
    // So to sample, all we need to do is walk down the tree, using the relative
    // weights of each left/right child to guide the probability of each
    // decision.
    size_t i = 0;
    while (i < (n_ - 1)) {
      // INVARIANT: at this point, x is uniform in [0, W), where W is the weight
      // of the subtree rooted at i.
      // W = left + right (but we don't need to actually read right.)
      // As discussed above, we want to go left with probability left / W.
      const double left = tree_[2 * i + 1];
      if (x <= left) {
        // Conditioning on this branch, x is now uniform in [0, left).
        i = 2 * i + 1;
      } else {
        // x is uniform in [left, left + right), so shift it down.
        x -= left;
        i = 2 * i + 2;
      }
    }

    return (i - (n_ - 1));
  }

  static size_t NextPowerOfTwo(size_t x) {
    size_t n = 1;
    while (n < x) {
      n *= 2;
    }
    return n;
  }
  // tree_[n_ - 1]...tree[2n_-2] = weights[0...n_-1]
  // parent of tree_[i] = tree_[(i - 1) / 2]
  // (children of tree_[i] = tree_[2i + 1, 2i + 2])
  // invariant: for internal nodes,
  //     tree_[i] = sum(weights[j] | j in subtree rooted at i)
  std::vector<double> tree_;
};

// This is the main simulator: takes a profile of data and a target size,
// and generates malloc load matching that.
class EmpiricalData {
 public:
  struct Entry {
    size_t size;
    // The units here actually don't matter so long as they're consistent
    // between Entries in a given Empirical.

    // Rate at which calls to malloc(<size>) are made
    double alloc_rate;
    // Number (at steady state) of extant <size>-sized objects
    double num_live;
  };

  // Allocates ~total_mem bytes, to put us in a "steady state".
  EmpiricalData(size_t seed, const absl::Span<const Entry> weights,
                size_t total_mem, absl::FunctionRef<void *(size_t)> alloc,
                absl::FunctionRef<void(void *, size_t)> dealloc);

  ~EmpiricalData();

  // Total memory consumed by objects
  size_t usage() const { return usage_; }

  // How many objects have we made in our total life?
  // Note that these *DO* include startup allocations.
  size_t total_num_allocated() const { return total_num_allocated_; }
  size_t total_bytes_allocated() const { return total_bytes_allocated_; }
  // Allocate or deallocate the next object
  void Next();

  // Empirical stats for the lifetime of this simulation (not including
  // startup allocations.)
  std::vector<Entry> Actual() const;

  absl::BitGen *const rng() { return &rng_; }

 private:
  absl::BitGen rng_;

  struct SizeState {
    const size_t size;
    const double birth_rate;
    const double death_rate;
    size_t total;
    std::vector<void *> objs;
  };

  void DoBirth(const size_t i);
  void DoDeath(const size_t i);

  absl::FunctionRef<void *(size_t)> alloc_;
  absl::FunctionRef<void(void *, size_t)> dealloc_;

  size_t usage_;
  size_t num_live_;
  size_t total_num_allocated_;
  size_t total_bytes_allocated_;
  std::vector<SizeState> state_;

  absl::discrete_distribution<size_t> birth_sampler_;
  double total_birth_rate_;
  AdjustableSampler death_sampler_;
};

using EmpiricalProfile = absl::Span<const EmpiricalData::Entry>;

}  // namespace tcmalloc

#endif  // TCMALLOC_TESTING_EMPIRICAL_H_
