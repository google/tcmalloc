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

#ifndef TCMALLOC_INTERNAL_CPU_CACHE_RESIZE_POLICY_H_
#define TCMALLOC_INTERNAL_CPU_CACHE_RESIZE_POLICY_H_

#include <stddef.h>
#include <stdint.h>

#include <algorithm>
#include <cmath>
#include <iterator>
#include <limits>

#include "absl/types/span.h"
#include "tcmalloc/internal/config.h"
#include "tcmalloc/internal/logging.h"

GOOGLE_MALLOC_SECTION_BEGIN
namespace tcmalloc {
namespace tcmalloc_internal {

// A per-CPU cache has a byte budget (its capacity) that is divided among its
// size classes; the division is revisited periodically by a resize policy.
// CpuCache snapshots every size class of one CPU into an array of
// SizeClassResizeState, indexed by size class with entry 0 unused, while it
// holds that CPU's resize lock with the CPU stopped, asks the policy for a
// plan, and applies the planned deltas to the slab.  Policies are pure
// functions of the snapshot: they touch no allocator state, so they are unit
// testable in isolation and safe to run under the lock.
struct SizeClassResizeState {
  // Inputs.
  //
  // Bytes per object.
  uint32_t object_size = 0;
  // Objects moved per transfer cache batch.
  uint16_t batch_length = 0;
  // Current capacity, in objects.
  uint16_t capacity = 0;
  // Capacity limit imposed by the slab layout, in objects.
  uint16_t max_capacity = 0;
  // Objects currently cached.
  uint16_t length = 0;
  // Grow() attempts during the interval that found no unallocated bytes.
  size_t capacity_misses = 0;
  // Underflows and overflows during the interval.
  size_t misses = 0;

  // In/out: clock ticks since the fast path last grew the class, advanced by
  // policies as they scan for classes to shrink (see ShrinkScore).
  uint16_t quiescent_ticks = 0;

  // Output: planned capacity change in objects, positive to grow, negative to
  // shrink.
  int32_t delta = 0;
  // Output: whether the fast path may grow the class by a batch on an
  // overflow followed by an underflow until the next plan.
  bool allow_fast_path_growth = true;
};

// Width of the quiescent ticks counter.
inline constexpr int kQuiescentTicksBits = 15;
inline constexpr uint32_t kMaxQuiescentTicks = (1u << kQuiescentTicksBits) - 1;

// Summary of a resize plan.
struct SizeClassResizePlan {
  // Unallocated bytes left after the planned shrinks and grows.
  size_t available_bytes = 0;
  // Number of size classes the policy tried to grow.
  size_t num_resizes = 0;
};

// Clock-like priority for shrinking a size class.
//
// Each size class has a quiescent ticks counter that is incremented every
// time a shrink scan passes it and reset to 0 when the fast path grows the
// class.  A counter of 0 means we have just tried to grow the class, so it
// makes little sense to shrink it back; the higher the counter, the longer
// ago we grew the class and the more probable it is that the full capacity is
// unused.
//
// The score is the number of ticks a class must have accumulated before it is
// shrunk: the higher the score, the less we want to shrink the class.  It is
// skewed toward larger size classes: smaller classes are usually used more
// actively and we also benefit less from shrinking them (we recover less
// capacity).  We also avoid shrinking full freelists, as that evicts an object
// to the backing cache, and freelists that are just above batch size, as
// shrinking them would disable transfer cache batching.
//
// Note: the following numbers are based solely on intuition, common sense and
// benchmarking results.
[[nodiscard]] inline uint32_t ShrinkScore(size_t object_size, size_t length,
                                          size_t capacity,
                                          size_t batch_length) {
  if (object_size <= 144) {
    return 2 + (length >= capacity) +
           (length >= batch_length && length < 2 * batch_length);
  }
  if (object_size <= 1024) {
    return 1 + (length >= capacity) +
           (length >= batch_length && length < 2 * batch_length);
  }
  if (object_size <= (64 << 10)) {
    return length >= capacity;
  }
  return 0;
}

// Resizes size classes by their capacity misses: a capacity miss is a fast
// path overflow or underflow on which the class wanted to grow but the cache
// had no unallocated bytes to give it.
//
// Each plan takes the kMaxSizeClassesToGrow classes with the most capacity
// misses in the interval (preferring smaller classes on ties; classes already
// at their maximum capacity are passed over and extend the window) and grows
// each of them, in that order, by up to a batch.  Growth is funded from the
// cache's unallocated bytes; when those run short, other classes are shrunk
// one object at a time in a clock-like scan (see ShrinkScore) that resumes
// where the previous scan stopped.  Classes with no misses are never grown,
// but no class is ever shrunk without a miss elsewhere to pay for.
//
// NumClasses is the number of size classes including the unused class 0.
template <size_t NumClasses>
class MissCountResizePolicy {
 public:
  // Size classes, in decreasing order of misses, considered for growth per
  // plan.
  static constexpr size_t kMaxSizeClassesToGrow = 5;
  static_assert(kMaxSizeClassesToGrow < NumClasses);

  // Plans the resize of one CPU's size classes.  <states> is indexed by size
  // class with entry 0 unused; <available_bytes> is the cache's unallocated
  // capacity; <next_steal> is the hand of the clock-like shrink scan, which
  // persists across plans.  On return, each state's delta holds its planned
  // capacity change and quiescent_ticks has been advanced for the classes the
  // scan visited.
  static SizeClassResizePlan Plan(absl::Span<SizeClassResizeState> states,
                                  size_t available_bytes, size_t& next_steal);

 private:
  // Planned capacity of a class, objects.
  [[nodiscard]] static size_t PlannedCapacity(
      const SizeClassResizeState& state) {
    return static_cast<size_t>(static_cast<int32_t>(state.capacity) +
                               state.delta);
  }

  // Advances the clock of <state> and shrinks it by one object if the clock
  // has reached its shrink score.  Returns the bytes of capacity released.
  [[nodiscard]] static size_t ShrinkOne(SizeClassResizeState& state);

  // Shrinks size classes in clock order, starting at <next_steal>, until
  // <bytes> have been released or every class has been visited once.  Classes
  // in <candidates> with misses are not shrunk.  Returns the bytes released.
  [[nodiscard]] static size_t Steal(absl::Span<SizeClassResizeState> states,
                                    absl::Span<const uint16_t> candidates,
                                    size_t bytes, size_t& next_steal);
};

template <size_t NumClasses>
SizeClassResizePlan MissCountResizePolicy<NumClasses>::Plan(
    absl::Span<SizeClassResizeState> states, size_t available_bytes,
    size_t& next_steal) {
  TC_ASSERT_EQ(states.size(), NumClasses);
  for (SizeClassResizeState& state : states) {
    state.delta = 0;
    // Growth on the fast path is never gated by this policy.
    state.allow_fast_path_growth = true;
  }

  // Order the size classes by misses in the interval, highest first.  In case
  // of a tie, prefer growing smaller size classes.
  uint16_t order[NumClasses - 1];
  for (size_t size_class = 1; size_class < NumClasses; ++size_class) {
    order[size_class - 1] = size_class;
  }
  std::sort(std::begin(order), std::end(order), [&](uint16_t a, uint16_t b) {
    if (states[a].capacity_misses == states[b].capacity_misses) {
      return a < b;
    }
    return states[a].capacity_misses > states[b].capacity_misses;
  });

  SizeClassResizePlan plan;
  size_t window = kMaxSizeClassesToGrow;
  for (size_t i = 0; i < window; ++i) {
    SizeClassResizeState& grow = states[order[i]];
    // If the size class with the largest misses has none, break.  The other
    // size classes have suffered no misses either.
    if (grow.capacity_misses == 0) break;

    // If we are already at the maximum capacity, there is nothing to grow:
    // consider one more class, otherwise resizing would stop working once the
    // top classes are all at their maximum.
    TC_ASSERT_LE(PlannedCapacity(grow), grow.max_capacity);
    const size_t can_grow = grow.max_capacity - PlannedCapacity(grow);
    if (can_grow == 0) {
      if (window < NumClasses - 1) {
        ++window;
      }
      continue;
    }

    ++plan.num_resizes;

    // We would like to grow the class by a batch; steal from other classes
    // what the unallocated bytes do not cover.
    const size_t need_bytes =
        std::min<size_t>(can_grow, grow.batch_length) * grow.object_size;
    if (need_bytes > available_bytes) {
      available_bytes += Steal(states, absl::MakeConstSpan(order, window),
                               need_bytes - available_bytes, next_steal);
    }
    const size_t acquired =
        std::min<size_t>(can_grow, available_bytes / grow.object_size);
    grow.delta += acquired;
    available_bytes -= acquired * grow.object_size;
  }
  plan.available_bytes = available_bytes;
  return plan;
}

template <size_t NumClasses>
size_t MissCountResizePolicy<NumClasses>::ShrinkOne(
    SizeClassResizeState& state) {
  const size_t capacity = PlannedCapacity(state);
  if (capacity == 0) {
    return 0;  // Nothing to steal.
  }
  // Shrinking a full freelist evicts an object, so the length cannot exceed
  // the planned capacity.
  const size_t length = std::min<size_t>(state.length, capacity);
  const uint32_t score =
      ShrinkScore(state.object_size, length, capacity, state.batch_length);
  const uint32_t ticks = state.quiescent_ticks;
  state.quiescent_ticks = (ticks + 1) & kMaxQuiescentTicks;
  if (ticks < score) {
    return 0;
  }
  --state.delta;
  return state.object_size;
}

template <size_t NumClasses>
size_t MissCountResizePolicy<NumClasses>::Steal(
    absl::Span<SizeClassResizeState> states,
    absl::Span<const uint16_t> candidates, size_t bytes, size_t& next_steal) {
  // Steal from other size classes.  Try to go in a nice circle.  Complicated
  // by size classes actually being 1-indexed.
  size_t acquired = 0;
  size_t source = next_steal;
  for (size_t i = 1; i < NumClasses; ++i, ++source) {
    if (source >= NumClasses) {
      source = 1;
    }
    // Don't shrink classes we want to grow.
    bool skip = false;
    for (uint16_t candidate : candidates) {
      if (candidate == source && states[candidate].capacity_misses != 0) {
        skip = true;
        break;
      }
    }
    if (skip) {
      continue;
    }
    acquired += ShrinkOne(states[source]);
    if (acquired >= bytes) {
      // Can't steal any more or don't need to.
      break;
    }
  }
  next_steal = source;
  return acquired;
}

// Resizes size classes by miss density: the misses (underflows and overflows)
// a class took per second per KiB of capacity it held during the interval.
//
// Rationale.  A miss moves about half of a class's capacity between the
// per-CPU cache and the backing cache.  For a class whose free list length
// drifts rather than oscillating inside a bounded working set -- the common
// case in the allocation traces we have examined, where the range of the
// length over a window of t seconds grows as roughly t^0.56 with no
// characteristic frequency -- the miss rate scales as 1 / capacity, so the
// density is the number of misses per second that one more KiB of capacity
// would save: the marginal value of a KiB.  Spending a byte budget to minimize
// misses means equalizing marginal values across classes, and a target
// density shared by every class and every CPU also decides how many bytes to
// hold at all: capacity that does not save kTargetMissDensity misses per
// second per KiB is returned.  The target is set from the cost ratio of the
// two resources: 1 KiB of cache costs on the order of 0.15 us of CPU per
// second (one core against a few GiB of RAM) and a miss costs 0.5-2 us, so
// the break-even density is 0.08-0.3 misses/s/KiB.  0.1 is the largest value
// at which replayed traces still sat on the flat part of the miss-versus-bytes
// frontier.
//
// Each plan moves every class toward the capacity at which its density would
// equal the target, capacity * sqrt(density / kTargetMissDensity), by at most
// a factor of two per interval.  Classes at or above the target may grow, in
// whole batches so that refills keep using the transfer cache, and may also
// grow on the fast path.  Classes between kShrinkMissDensity and the target
// hold, so that a well-sized class is not disturbed by sampling noise.
// Classes below kShrinkMissDensity halve, to no less than two batches (below
// that refills bypass the transfer cache; the fast path grows a class that
// far unconditionally).  An empty class that missed is funded first.  Growth
// is paid from the cache's unallocated bytes and then, lowest density first,
// from classes whose density is at most a quarter of the grower's, each
// shrunk by at most half and to no less than two batches.
//
// NumClasses is the number of size classes including the unused class 0.
template <size_t NumClasses>
class MissDensityResizePolicy {
 public:
  // Marginal value of per-CPU cache capacity, in misses saved per second per
  // KiB of capacity, at which a size class is correctly sized.
#ifndef TCMALLOC_INTERNAL_TARGET_MISS_DENSITY
#define TCMALLOC_INTERNAL_TARGET_MISS_DENSITY 0.1
#endif
  static constexpr double kTargetMissDensity =
      TCMALLOC_INTERNAL_TARGET_MISS_DENSITY;
  // Density below which a size class is shrunk.
  static constexpr double kShrinkMissDensity = kTargetMissDensity / 4;

  // Plans the resize of one CPU's size classes.  <states> is indexed by size
  // class with entry 0 unused; <available_bytes> is the cache's unallocated
  // capacity; <interval_seconds> is the time over which the misses were
  // counted.  On return, each state's delta holds its planned capacity change
  // and allow_fast_path_growth whether the class may grow on the fast path.
  static SizeClassResizePlan Plan(absl::Span<SizeClassResizeState> states,
                                  size_t available_bytes,
                                  double interval_seconds);

 private:
  struct Candidate {
    uint16_t size_class;
    // Planned capacity, objects.
    size_t target;
    double density;
  };

  // Planned capacity of a class, objects.
  [[nodiscard]] static size_t PlannedCapacity(
      const SizeClassResizeState& state) {
    return static_cast<size_t>(static_cast<int32_t>(state.capacity) +
                               state.delta);
  }

  // Smallest capacity a class is shrunk to, objects.
  [[nodiscard]] static size_t Floor(const SizeClassResizeState& state) {
    return std::min<size_t>(2 * state.batch_length, state.max_capacity);
  }
};

template <size_t NumClasses>
SizeClassResizePlan MissDensityResizePolicy<NumClasses>::Plan(
    absl::Span<SizeClassResizeState> states, size_t available_bytes,
    double interval_seconds) {
  TC_ASSERT_EQ(states.size(), NumClasses);
  TC_ASSERT_GT(interval_seconds, 0);
  SizeClassResizePlan plan;

  Candidate candidates[NumClasses - 1];
  size_t num_candidates = 0;
  bool any_action = false;
  for (size_t size_class = 1; size_class < NumClasses; ++size_class) {
    SizeClassResizeState& state = states[size_class];
    state.delta = 0;
    state.allow_fast_path_growth = true;
    if (state.max_capacity == 0) continue;
    const size_t capacity = state.capacity;
    const size_t floor = Floor(state);
    Candidate& candidate = candidates[num_candidates++];
    candidate.size_class = size_class;
    candidate.target = capacity;
    if (capacity == 0) {
      // An empty class grows on its first miss.  If it missed during the
      // interval and is still empty, the cache is out of unallocated bytes:
      // fund it ahead of everything else.
      candidate.density =
          state.misses > 0 ? std::numeric_limits<double>::infinity() : 0.0;
      if (state.misses > 0) candidate.target = floor;
    } else {
      const double kib =
          static_cast<double>(capacity) * state.object_size / 1024.0;
      candidate.density = state.misses / (interval_seconds * kib);
      if (candidate.density >= kTargetMissDensity) {
        double want =
            capacity * std::sqrt(candidate.density / kTargetMissDensity);
        want = std::min(want, 2.0 * capacity);
        // Grow in whole batches so that refills can use the transfer cache.
        size_t target = static_cast<size_t>(want);
        target = (target + state.batch_length - 1) / state.batch_length *
                 state.batch_length;
        candidate.target = std::min<size_t>(target, state.max_capacity);
      } else {
        state.allow_fast_path_growth = false;
        if (candidate.density < kShrinkMissDensity && capacity > floor) {
          // Below kShrinkMissDensity, capacity * sqrt(density / target) is
          // under half the capacity, so the halving bound always binds.
          candidate.target = std::max(capacity / 2, floor);
        }
      }
    }
    any_action |= candidate.target != capacity;
  }
  plan.available_bytes = available_bytes;
  if (!any_action) {
    return plan;
  }

  // Highest density first.
  std::sort(candidates, candidates + num_candidates,
            [](const Candidate& a, const Candidate& b) {
              if (a.density == b.density) return a.size_class < b.size_class;
              return a.density > b.density;
            });

  // Shrink first, so that the released bytes can fund growth.
  for (size_t i = num_candidates; i-- > 0;) {
    const Candidate& candidate = candidates[i];
    SizeClassResizeState& state = states[candidate.size_class];
    if (candidate.target >= state.capacity) continue;
    const size_t count = state.capacity - candidate.target;
    state.delta -= count;
    available_bytes += count * state.object_size;
    ++plan.num_resizes;
  }

  for (size_t i = 0; i < num_candidates; ++i) {
    const Candidate& candidate = candidates[i];
    SizeClassResizeState& grow = states[candidate.size_class];
    const size_t capacity = PlannedCapacity(grow);
    if (candidate.target <= capacity) continue;
    const size_t size = grow.object_size;
    const size_t need = candidate.target - capacity;
    if (available_bytes < need * size) {
      // The per-CPU limit binds: take the bytes from the classes worth the
      // least, lowest density first, shrinking each by at most half and to
      // no less than two batches, and only from classes whose density is
      // well below this one's.
      for (size_t j = num_candidates;
           j-- > i + 1 && available_bytes < need * size;) {
        const Candidate& victim = candidates[j];
        if (!(victim.density * 4 <= candidate.density)) break;
        SizeClassResizeState& victim_state = states[victim.size_class];
        const size_t victim_capacity = PlannedCapacity(victim_state);
        const size_t victim_floor =
            std::max(Floor(victim_state), victim_capacity / 2);
        if (victim_capacity <= victim_floor) continue;
        const size_t victim_size = victim_state.object_size;
        const size_t take = std::min(
            victim_capacity - victim_floor,
            (need * size - available_bytes + victim_size - 1) / victim_size);
        victim_state.delta -= take;
        available_bytes += take * victim_size;
        ++plan.num_resizes;
      }
    }
    const size_t can = std::min(need, available_bytes / size);
    if (can == 0) continue;
    grow.delta += can;
    available_bytes -= can * size;
    ++plan.num_resizes;
  }
  plan.available_bytes = available_bytes;
  return plan;
}

}  // namespace tcmalloc_internal
}  // namespace tcmalloc
GOOGLE_MALLOC_SECTION_END

#endif  // TCMALLOC_INTERNAL_CPU_CACHE_RESIZE_POLICY_H_
