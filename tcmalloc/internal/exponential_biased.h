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

#ifndef TCMALLOC_INTERNAL_EXPONENTIAL_BIASED_H_
#define TCMALLOC_INTERNAL_EXPONENTIAL_BIASED_H_

#include <atomic>
#include <cstdint>
#include <type_traits>

namespace tcmalloc {
namespace tcmalloc_internal {

class ExponentialBiased {
 public:
  static constexpr int kStateBits = 48;
  static constexpr int kRandomBits = 32;

  class State {
   public:
    constexpr explicit State(uint64_t seed) : value_(seed) {}

   private:
    friend class ExponentialBiased;
    uint64_t value_;
  };
  static_assert(std::is_trivially_copyable_v<State>);

  [[nodiscard]] static constexpr State NextRandom(State rnd);
  [[nodiscard]] static constexpr uint32_t GetRandom(State rnd);
};

// Returns the next prng value.
// pRNG is: aX+b mod c with a = 0x5DEECE66D, b =  0xB, c = 1<<48
// This is the lrand64 generator.
constexpr ExponentialBiased::State ExponentialBiased::NextRandom(State rnd) {
  const uint64_t prng_mult = UINT64_C(0x5DEECE66D);
  const uint64_t prng_add = 0xB;
  const uint64_t prng_mod_mask = ~((~static_cast<uint64_t>(0)) << kStateBits);
  return State{(prng_mult * rnd.value_ + prng_add) & prng_mod_mask};
}

// Extracts higher-quality random bits.
// The raw state stepped by NextRandom has poor low-bit randomness
// and cannot be used directly for arithmetic or modulo operations.
constexpr uint32_t ExponentialBiased::GetRandom(State rnd) {
  return static_cast<uint32_t>(rnd.value_ >> (kStateBits - kRandomBits));
}

// Convenience wrapper to initialize a seed and return a sequence of
// pseudo-random values. Thread-safety: thread safe.
class Random {
 public:
  constexpr explicit Random(uint64_t seed)
      : state_(ExponentialBiased::State{seed}) {}

  // Return the next pseudo-random value.
  [[nodiscard]] uint32_t Next();

  // Reset internal state with provided seed.
  void Reset(uint64_t seed);

 private:
  static_assert(std::atomic<ExponentialBiased::State>::is_always_lock_free);
  std::atomic<ExponentialBiased::State> state_;
};

inline uint32_t Random::Next() {
  ExponentialBiased::State r = state_.load(std::memory_order_relaxed);
  r = ExponentialBiased::NextRandom(r);
  state_.store(r, std::memory_order_relaxed);
  return ExponentialBiased::GetRandom(r);
}

inline void Random::Reset(uint64_t seed) {
  state_.store(ExponentialBiased::State{seed}, std::memory_order_relaxed);
}

}  // namespace tcmalloc_internal
}  // namespace tcmalloc

#endif  // TCMALLOC_INTERNAL_EXPONENTIAL_BIASED_H_
