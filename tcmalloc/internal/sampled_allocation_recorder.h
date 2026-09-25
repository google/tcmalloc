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

#include "absl/base/attributes.h"
#include "absl/base/const_init.h"
#include "absl/base/internal/spinlock.h"
#include "absl/base/thread_annotations.h"
#include "absl/functional/function_ref.h"
#include "tcmalloc/internal/allocation_guard.h"
#include "tcmalloc/internal/config.h"
#include "tcmalloc/internal/logging.h"

GOOGLE_MALLOC_SECTION_BEGIN
namespace tcmalloc {
namespace tcmalloc_internal {

// Sample<T> that has members required for linking samples in the linked list of
// samples maintained by the SampleRecorder.  Type T defines the sampled data.
template <typename T>
struct Sample {
  // Guards the ability to restore the sample to a pristine state.  This
  // prevents races with sampling and resurrecting an object.
  absl::base_internal::SpinLock lock{absl::base_internal::SCHEDULE_KERNEL_ONLY};
  T* next = nullptr;
  T* dead ABSL_GUARDED_BY(lock) = nullptr;
};

// Holds samples and their associated stack traces.
//
// Thread safe.
template <typename T, typename AllocatorT>
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
  void PushDead(T* sample);
  template <typename... Targs>
  T* PopDead(Targs&&... args);

  // Intrusive lock free linked lists for tracking samples.
  //
  // `all_` records all samples (they are never removed from this list) and is
  // terminated with a `nullptr`.
  //
  // `graveyard_.dead` is a circular linked list.  When it is empty,
  // `graveyard_.dead == &graveyard`.  The list is circular so that
  // every item on it (even the last) has a non-null dead pointer.  This allows
  // `Iterate` to determine if a given sample is live or dead using only
  // information on the sample itself.
  //
  // For example, nodes [A, B, C, D, E] with [A, C, E] alive and [B, D] dead
  // looks like this (G is the Graveyard):
  //
  //           +---+    +---+    +---+    +---+    +---+
  //    all -->| A |--->| B |--->| C |--->| D |--->| E |
  //           |   |    |   |    |   |    |   |    |   |
  //   +---+   |   | +->|   |-+  |   | +->|   |-+  |   |
  //   | G |   +---+ |  +---+ |  +---+ |  +---+ |  +---+
  //   |   |         |        |        |        |
  //   |   | --------+        +--------+        |
  //   +---+                                    |
  //     ^                                      |
  //     +--------------------------------------+
  //
  std::atomic<T*> all_ = nullptr;
  T graveyard_;

  Allocator* allocator_ = nullptr;
};

template <typename T, typename Allocator>
constexpr SampleRecorder<T, Allocator>::SampleRecorder(Allocator& allocator) {
  Init(allocator);
}

template <typename T, typename Allocator>
constexpr void SampleRecorder<T, Allocator>::Init(Allocator& allocator)
    ABSL_NO_THREAD_SAFETY_ANALYSIS {
  TC_CHECK(!allocator_);
  allocator_ = &allocator;
  graveyard_.dead = &graveyard_;
}

template <typename T, typename Allocator>
SampleRecorder<T, Allocator>::~SampleRecorder() {
  T* s = all_.load(std::memory_order_acquire);
  while (s != nullptr) {
    T* next = s->next;
    allocator_->Delete(s);
    s = next;
  }
}

template <typename T, typename Allocator>
void SampleRecorder<T, Allocator>::PushNew(T* sample) {
  sample->next = all_.load(std::memory_order_relaxed);
  while (!all_.compare_exchange_weak(sample->next, sample,
                                     std::memory_order_release,
                                     std::memory_order_relaxed)) {
  }
}

template <typename T, typename Allocator>
void SampleRecorder<T, Allocator>::PushDead(T* sample) {
  AllocationGuardSpinLockHolder graveyard_lock(graveyard_.lock);
  AllocationGuardSpinLockHolder sample_lock(sample->lock);
  sample->dead = graveyard_.dead;
  graveyard_.dead = sample;
}

template <typename T, typename Allocator>
template <typename... Targs>
T* SampleRecorder<T, Allocator>::PopDead(Targs&&... args) {
  AllocationGuard enforce_no_alloc;
  T* sample;
  {
    absl::base_internal::SpinLockHolder graveyard_lock(graveyard_.lock);

    // The list is circular, so eventually it collapses down to
    //   graveyard_.dead == &graveyard_
    // when it is empty.
    sample = graveyard_.dead;
    if (ABSL_PREDICT_FALSE(sample == &graveyard_)) return nullptr;

    graveyard_.dead = sample->dead;
  }
  absl::base_internal::SpinLockHolder sample_lock(sample->lock);
  // TODO(b/73749855): This could be moved out but requires updating our lock
  // annotations.
  sample->PrepareForSampling(std::forward<Targs>(args)...);
  sample->dead = nullptr;
  return sample;
}

template <typename T, typename Allocator>
template <typename... Targs>
T* SampleRecorder<T, Allocator>::Register(Targs&&... args) {
  T* sample = PopDead(std::forward<Targs>(args)...);
  if (ABSL_PREDICT_FALSE(sample == nullptr)) {
    // Resurrection failed.  Hire a new warlock.
    sample = allocator_->New(std::forward<Targs>(args)...);
    PushNew(sample);
  }

  return sample;
}

template <typename T, typename Allocator>
void SampleRecorder<T, Allocator>::Unregister(T* sample) {
  PushDead(sample);
}

template <typename T, typename Allocator>
void SampleRecorder<T, Allocator>::UnregisterAll() {
  AllocationGuardSpinLockHolder graveyard_lock(graveyard_.lock);
  T* sample = all_.load(std::memory_order_acquire);
  while (sample != nullptr) {
    {
      AllocationGuardSpinLockHolder sample_lock(sample->lock);
      if (sample->dead == nullptr) {
        sample->dead = graveyard_.dead;
        graveyard_.dead = sample;
      }
    }
    sample = sample->next;
  }
}

template <typename T, typename Allocator>
void SampleRecorder<T, Allocator>::Iterate(
    const absl::FunctionRef<void(const T& sample)>& f) {
  T* s = all_.load(std::memory_order_acquire);
  while (s != nullptr) {
    AllocationGuardSpinLockHolder l(s->lock);
    if (s->dead == nullptr) {
      f(*s);
    }
    s = s->next;
  }
}

}  // namespace tcmalloc_internal
}  // namespace tcmalloc
GOOGLE_MALLOC_SECTION_END

#endif  // TCMALLOC_INTERNAL_SAMPLED_ALLOCATION_RECORDER_H_
