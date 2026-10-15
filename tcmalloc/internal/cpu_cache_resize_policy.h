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
#include <iterator>

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

  // In/out: clock ticks since the fast path last grew the class, advanced by
  // policies as they scan for classes to shrink (see ShrinkScore).
  uint16_t quiescent_ticks = 0;

  // Output: planned capacity change in objects, positive to grow, negative to
  // shrink.
  int32_t delta = 0;
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

}  // namespace tcmalloc_internal
}  // namespace tcmalloc
GOOGLE_MALLOC_SECTION_END

#endif  // TCMALLOC_INTERNAL_CPU_CACHE_RESIZE_POLICY_H_
