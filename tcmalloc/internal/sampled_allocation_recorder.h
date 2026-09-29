// Copyright 2018 The Abseil Authors.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
// -----------------------------------------------------------------------------
// File: sampled_allocation_recorder.h
// -----------------------------------------------------------------------------
//
// This header file defines a lock-free linked list for recording TCMalloc
// sampled allocations collected from a random/stochastic process.

#ifndef TCMALLOC_INTERNAL_SAMPLED_ALLOCATION_RECORDER_H_
#define TCMALLOC_INTERNAL_SAMPLED_ALLOCATION_RECORDER_H_

#include <atomic>
#include <cstddef>
#include <cstdint>

#include "absl/base/attributes.h"
#include "absl/base/internal/spinlock.h"
#include "absl/base/optimization.h"
#include "absl/base/thread_annotations.h"
#include "absl/functional/function_ref.h"
#include "tcmalloc/internal/allocation_guard.h"
#include "tcmalloc/internal/config.h"
#include "tcmalloc/internal/logging.h"
#include "tcmalloc/internal/two_level_array.h"

GOOGLE_MALLOC_SECTION_BEGIN
namespace tcmalloc {
namespace tcmalloc_internal {

// Sample<T> that has members required for tracking samples maintained by the
// SampleRecorder.  Type T defines the sampled data.
template <typename T>
struct Sample {
  T* dead = nullptr;
  // Index in TwoLevelArray `all_`. Used by PageMap to map page metadata to
  // the sampled allocation without storing a 64-bit pointer.
  uint32_t sampled_index = 0;
  // Guards the ability to restore the sample to a pristine state.  This
  // prevents races with sampling and resurrecting an object.
  absl::base_internal::SpinLock lock{absl::base_internal::SCHEDULE_KERNEL_ONLY};
  bool live ABSL_GUARDED_BY(lock) = false;
};

// Holds samples and their associated stack traces.
//
// Thread safe.
template <typename T, typename AllocatorT, typename ArrayAllocator>
class SampleRecorder {
 public:
  using Allocator = AllocatorT;

  constexpr SampleRecorder() = default;
  constexpr explicit SampleRecorder(
      Allocator& allocator ABSL_ATTRIBUTE_LIFETIME_BOUND);
  constexpr void Init(Allocator& allocator);
  ~SampleRecorder();

  SampleRecorder(const SampleRecorder&) = delete;
  SampleRecorder& operator=(const SampleRecorder&) = delete;

  SampleRecorder(SampleRecorder&&) = delete;
  SampleRecorder& operator=(SampleRecorder&&) = delete;

  // Registers for sampling.  Returns an opaque registration info.
  template <typename... Targs>
  T* Register(Targs&&... args);

  // Unregisters the sample.
  void Unregister(T* sample);

  // Returns the sample at the given index.
  T* Map(size_t index) const;

  // Unregisters any live samples starting from `all_`. Note that if there are
  // any samples added in front of `all_` in other threads after this function
  // reads `all_`, they won't be cleaned up. External synchronization is
  // required if the intended outcome is to have no live sample after this call.
  // Extra care must be taken when `Unregister()` is invoked concurrently with
  // this function to avoid a dead sample (updated by this function) being
  // passed to `Unregister()` which assumes the sample is live.
  void UnregisterAll();

  // Iterates over all the registered samples.
  void Iterate(const absl::FunctionRef<void(const T& sample)>& f);

 private:
  void PushNew(T* sample);
  T* PopDead();

  absl::base_internal::SpinLock graveyard_lock_{
      absl::base_internal::SCHEDULE_KERNEL_ONLY};
  // Singly-linked freelist of dead samples available for reuse.
  T* graveyard_ = nullptr;

  absl::base_internal::SpinLock all_lock_{
      absl::base_internal::SCHEDULE_KERNEL_ONLY};
  // Append-only array of all allocated samples ever created (dead or alive).
  TwoLevelArray<T*, ArrayAllocator> all_;
  // Atomic mirror of all_.size() allowing lock-free readers in Iterate() and
  // Map() without acquiring all_lock_.
  std::atomic<size_t> all_size_ = 0;

  Allocator* allocator_ = nullptr;
};

template <typename T, typename Allocator, typename ArrayAllocator>
constexpr SampleRecorder<T, Allocator, ArrayAllocator>::SampleRecorder(
    Allocator& allocator) {
  Init(allocator);
}

template <typename T, typename Allocator, typename ArrayAllocator>
constexpr void SampleRecorder<T, Allocator, ArrayAllocator>::Init(
    Allocator& allocator) ABSL_NO_THREAD_SAFETY_ANALYSIS {
  TC_CHECK(!allocator_);
  allocator_ = &allocator;
  // 5 buckets (31 sampled objects) is pretty arbitrary.
  // This use is not performance-critical, and we just need some number.
  all_.Init(5);
}

template <typename T, typename Allocator, typename ArrayAllocator>
SampleRecorder<T, Allocator, ArrayAllocator>::~SampleRecorder() {
  all_.ForEach([&](T* sample) { allocator_->Delete(sample); });
}

template <typename T, typename Allocator, typename ArrayAllocator>
T* SampleRecorder<T, Allocator, ArrayAllocator>::Map(size_t index) const {
  TC_ASSERT_LT(index, all_size_.load(std::memory_order_relaxed));
  return all_[index];
}

template <typename T, typename Allocator, typename ArrayAllocator>
void SampleRecorder<T, Allocator, ArrayAllocator>::PushNew(T* sample) {
  AllocationGuardSpinLockHolder l(all_lock_);
  sample->sampled_index = all_.size();
  all_.push_back(sample);
  all_size_.store(all_.size(), std::memory_order_release);
}

template <typename T, typename Allocator, typename ArrayAllocator>
T* SampleRecorder<T, Allocator, ArrayAllocator>::PopDead() {
  AllocationGuardSpinLockHolder graveyard_lock(graveyard_lock_);
  auto* sample = graveyard_;
  if (ABSL_PREDICT_FALSE(sample == nullptr)) {
    return nullptr;
  }
  graveyard_ = sample->dead;
  sample->dead = nullptr;
  return sample;
}

template <typename T, typename Allocator, typename ArrayAllocator>
template <typename... Targs>
T* SampleRecorder<T, Allocator, ArrayAllocator>::Register(Targs&&... args) {
  T* sample = PopDead();
  if (ABSL_PREDICT_TRUE(sample != nullptr)) {
    sample->PrepareForSampling(std::forward<Targs>(args)...);
  } else {
    sample = allocator_->New(std::forward<Targs>(args)...);
    PushNew(sample);
  }
  AllocationGuardSpinLockHolder sample_lock(sample->lock);
  sample->live = true;
  return sample;
}

template <typename T, typename Allocator, typename ArrayAllocator>
void SampleRecorder<T, Allocator, ArrayAllocator>::Unregister(T* sample) {
  {
    AllocationGuardSpinLockHolder sample_lock(sample->lock);
    sample->live = false;
  }
  AllocationGuardSpinLockHolder graveyard_lock(graveyard_lock_);
  sample->dead = graveyard_;
  graveyard_ = sample;
}

template <typename T, typename Allocator, typename ArrayAllocator>
void SampleRecorder<T, Allocator, ArrayAllocator>::UnregisterAll() {
  AllocationGuardSpinLockHolder l(all_lock_);
  AllocationGuardSpinLockHolder graveyard_lock(graveyard_lock_);
  all_.ForEach([&](T* sample) {
    AllocationGuardSpinLockHolder sample_lock(sample->lock);
    if (!sample->live) {
      return;
    }
    sample->live = false;
    sample->dead = graveyard_;
    graveyard_ = sample;
  });
}

template <typename T, typename Allocator, typename ArrayAllocator>
void SampleRecorder<T, Allocator, ArrayAllocator>::Iterate(
    const absl::FunctionRef<void(const T& sample)>& f) {
  // Iterate lock-free over already-allocated samples without taking all_lock_
  // to avoid contention with concurrent allocations.
  const uint32_t size = all_size_.load(std::memory_order_acquire);
  all_.ForEachUpTo(size, [&](T* sample) {
    AllocationGuardSpinLockHolder l(sample->lock);
    if (sample->live) {
      f(*sample);
    }
  });
}

}  // namespace tcmalloc_internal
}  // namespace tcmalloc
GOOGLE_MALLOC_SECTION_END

#endif  // TCMALLOC_INTERNAL_SAMPLED_ALLOCATION_RECORDER_H_
