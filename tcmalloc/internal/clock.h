// Copyright 2021 The TCMalloc Authors
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

#ifndef TCMALLOC_INTERNAL_CLOCK_H_
#define TCMALLOC_INTERNAL_CLOCK_H_

#include <stdint.h>

#include <atomic>

#include "absl/base/internal/cycleclock.h"
#include "absl/time/time.h"
#include "tcmalloc/internal/config.h"

GOOGLE_MALLOC_SECTION_BEGIN
namespace tcmalloc {
namespace tcmalloc_internal {

// Represents an abstract clock. The now and freq functions are analogous to
// CycleClock::Now and CycleClock::Frequency, which will be the most commonly
// used implementations. Tests can use this interface to mock out the clock.
struct Clock {
  // Returns the current time in ticks (relative to an arbitrary time base).
  int64_t (*now)() = absl::base_internal::CycleClock::Now;

  // Returns the number of ticks per second.
  double (*freq)() = absl::base_internal::CycleClock::Frequency;

  struct Snapshot {
    int64_t now;
    double freq;
  };

  Snapshot GetSnapshot() const { return Snapshot{now(), freq()}; }
};

// Encapsulates a 32-bit cycle timestamp by right-shifting clock.now() by
// kShift bits.
//
// Granularity and Epoch:
// - At 2 GHz: 1 tick = ~0.524 ms; 32-bit epoch = ~26.0 days before wraparound.
// - At 3 GHz: 1 tick = ~0.349 ms; 32-bit epoch = ~17.3 days before wraparound.
// - At 4 GHz: 1 tick = ~0.262 ms; 32-bit epoch = ~13.0 days before wraparound.
class Cycles32 {
 public:
  static constexpr int kShift = 20;

  constexpr Cycles32() = default;
  explicit constexpr Cycles32(uint32_t val) : val_(val) {}

  // Updates the timestamp using clock.now() >> kShift.
  void Update(Clock clock = Clock{}) {
    uint32_t now_32 = static_cast<uint32_t>(clock.now() >> kShift);
    if (now_32 == 0) now_32 = 1;  // Reserve 0 as the uninitialized sentinel.
    val_.store(now_32, std::memory_order_relaxed);
  }

  // Resets the timestamp to 0 (uninitialized sentinel).
  void Reset() { val_.store(0, std::memory_order_relaxed); }

  // Returns true if the timestamp is initialized (val_ != 0).
  explicit operator bool() const {
    return val_.load(std::memory_order_relaxed) != 0;
  }

  // Returns elapsed time since the recorded tick using a pre-taken snapshot.
  // Returns absl::InfiniteDuration() if val_ == 0 (uninitialized).
  absl::Duration AsDuration(Clock::Snapshot snap) const {
    const uint32_t last = val_.load(std::memory_order_relaxed);
    if (last == 0) return absl::InfiniteDuration();
    const uint32_t now_32 = static_cast<uint32_t>(snap.now >> kShift);
    // Unsigned 32-bit modular subtraction across wraparound is safe.
    const uint32_t elapsed_ticks = now_32 - last;
    const double elapsed_cycles =
        static_cast<double>(elapsed_ticks) * (1 << kShift);
    return absl::Seconds(elapsed_cycles / snap.freq);
  }

  // Convenience overload: takes a fresh clock snapshot.
  absl::Duration AsDuration(Clock clock = Clock{}) const {
    return AsDuration(clock.GetSnapshot());
  }

  // Returns true if this timestamp occurred after or at the same time as other
  // across 32-bit wraparound boundaries (valid within half the epoch).
  bool TimeAfterOrEqual(const Cycles32& other) const {
    return TimeAfterOrEqual(raw(), other.raw());
  }

  uint32_t raw() const { return val_.load(std::memory_order_relaxed); }

 private:
  static bool TimeAfterOrEqual(uint32_t a, uint32_t b) {
    return static_cast<int32_t>(a - b) >= 0;
  }

  std::atomic<uint32_t> val_{0};
};

}  // namespace tcmalloc_internal
}  // namespace tcmalloc
GOOGLE_MALLOC_SECTION_END

#endif  // TCMALLOC_INTERNAL_CLOCK_H_
