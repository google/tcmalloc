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

#ifndef TCMALLOC_INTERNAL_DELAY_INJECTION_H_
#define TCMALLOC_INTERNAL_DELAY_INJECTION_H_

#include <atomic>
#include <cstdint>

#include "absl/base/internal/cycleclock.h"
#include "absl/base/optimization.h"

// TODO(b/29448043): Remove this latency injection functionality.

namespace tcmalloc {
namespace tcmalloc_internal {

class ScopedDelay {
 public:
  explicit ScopedDelay(std::atomic<int64_t>& delay_cycles) {
#ifdef TCMALLOC_INTERNAL_LATENCY_INJECTION
    int64_t cycles = delay_cycles.load(std::memory_order_relaxed);
    if (ABSL_PREDICT_TRUE(cycles <= 0)) {
      return;
    }
    int64_t start = absl::base_internal::CycleClock::Now();
    while (absl::base_internal::CycleClock::Now() - start < cycles) {
#if defined(__x86_64__)
      __asm__ __volatile__("pause\n");
#elif defined(__aarch64__)
      __asm__ __volatile__("yield\n");
#endif
    }
#endif
  }
  ~ScopedDelay() = default;

  // Global atomics for the four injection points
  static std::atomic<int64_t> page_heap_delay;
  static std::atomic<int64_t> central_freelist_delay;
  static std::atomic<int64_t> update_max_capacities_delay;
  static std::atomic<int64_t> resize_slabs_delay;
};

}  // namespace tcmalloc_internal
}  // namespace tcmalloc

#endif  // TCMALLOC_INTERNAL_DELAY_INJECTION_H_
