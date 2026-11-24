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

#ifndef TCMALLOC_INTERNAL_FAKE_CLOCK_H_
#define TCMALLOC_INTERNAL_FAKE_CLOCK_H_

#include <stddef.h>
#include <stdint.h>

#include <atomic>

#include "absl/time/time.h"
#include "tcmalloc/internal/clock.h"

namespace tcmalloc::tcmalloc_internal {

// A clock that can be used with the Clock interface (see GetClock()),
// but is controllable from tests in order to not depend on wall-clock time.
class FakeClock {
 public:
  FakeClock() = default;
  [[nodiscard]] static int64_t now() {
    now_calls_.fetch_add(1, std::memory_order_relaxed);
    return clock_.load(std::memory_order_relaxed);
  }
  [[nodiscard]] static double freq() {
    freq_calls_.fetch_add(1, std::memory_order_relaxed);
    return absl::ToDoubleNanoseconds(absl::Seconds(2));
  }
  static void Advance(absl::Duration d) {
    clock_.fetch_add(static_cast<int64_t>(absl::ToDoubleSeconds(d) * freq()),
                     std::memory_order_relaxed);
  }
  static void ResetClock() {
    clock_.store(1234, std::memory_order_relaxed);
    now_calls_.store(0, std::memory_order_relaxed);
    freq_calls_.store(0, std::memory_order_relaxed);
  }
  [[nodiscard]] static size_t now_calls() {
    return now_calls_.load(std::memory_order_relaxed);
  }
  [[nodiscard]] static size_t freq_calls() {
    return freq_calls_.load(std::memory_order_relaxed);
  }
  static void ResetCalls() {
    now_calls_.store(0, std::memory_order_relaxed);
    freq_calls_.store(0, std::memory_order_relaxed);
  }

  [[nodiscard]] static Clock GetClock() {
    return {.now = FakeClock::now, .freq = FakeClock::freq};
  }

 private:
  static std::atomic<int64_t> clock_;
  static std::atomic<size_t> now_calls_;
  static std::atomic<size_t> freq_calls_;
};

}  // namespace tcmalloc::tcmalloc_internal

#endif  // TCMALLOC_INTERNAL_FAKE_CLOCK_H_
