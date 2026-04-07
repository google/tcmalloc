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

#ifndef TCMALLOC_INTERNAL_PERCPU_STATE_H_
#define TCMALLOC_INTERNAL_PERCPU_STATE_H_

#include <pthread.h>

#include "absl/base/attributes.h"
#include "absl/base/call_once.h"
#include "absl/base/nullability.h"
#include "tcmalloc/internal/config.h"

GOOGLE_MALLOC_SECTION_BEGIN
namespace tcmalloc::tcmalloc_internal {

class ThreadCache;

extern "C" ABSL_ATTRIBUTE_WEAK void TCMalloc_Internal_DestroyThreadCache(
    ThreadCache* absl_nullable);

// PerCpuState receives thread creation/destruction notifications, managing the
// registration with the pthread library using a single pthread_key_t
// registration.
//
// TODO(b/478927694): Include actual per-core state.
class PerCpuState {
 public:
  static constexpr PerCpuState& state() { return g; }

  void Init();

  ThreadCache* absl_nullable GetThreadCache() const;

  // Registers an instance for destruction.  `nullptr` unregisters.
  void RegisterThreadCache(ThreadCache* absl_nullable);

 private:
  constexpr PerCpuState() = default;

  ABSL_CONST_INIT static PerCpuState g;

  static void HandleThreadExit(void* ptr);

  absl::once_flag f_;
  pthread_key_t key_{};
  // We also store a copy of per-thread data in a `thread_local` variable since
  // it is faster to read than `pthread_getspecific`.  We use
  // pthread_setspecific to manage destroying the thread cache, since many
  // `__cxa_thread_atexit` implementations allocate.
  //
  // We also give a hint to the compiler to use the "initial exec" TLS model.
  // This is faster than the default TLS model, at the cost that you cannot
  // dlopen this library.  (To see the difference, look at the CPU use of
  // __tls_get_addr with and without this attribute.)
  //
  // Since using dlopen on a malloc replacement is asking for trouble in any
  // case, that's a good tradeoff for us.
  ABSL_CONST_INIT static thread_local ThreadCache* thread_local_data_
      ABSL_ATTRIBUTE_INITIAL_EXEC;
};

inline void PerCpuState::Init() {
  absl::base_internal::LowLevelCallOnce(
      &f_, [&]() { pthread_key_create(&key_, HandleThreadExit); });
}

inline ThreadCache* absl_nullable ABSL_ATTRIBUTE_ALWAYS_INLINE
PerCpuState::GetThreadCache() const {
  return thread_local_data_;
}

inline void PerCpuState::RegisterThreadCache(ThreadCache* absl_nullable cache) {
  if (cache == nullptr) {
    // Use &g as a sentinel so that we always get a callback.
    pthread_setspecific(key_, &g);
    thread_local_data_ = nullptr;
  } else {
    pthread_setspecific(key_, cache);
    thread_local_data_ = cache;
  }
}

inline void PerCpuState::HandleThreadExit(void* ptr) {
  if (ptr == &g) {
    // TODO(b/478927694): Take advantage of this callback.
    return;
  }

  thread_local_data_ = nullptr;
  if (&TCMalloc_Internal_DestroyThreadCache != nullptr) {
    ThreadCache* cache = static_cast<ThreadCache*>(ptr);
    TCMalloc_Internal_DestroyThreadCache(cache);
  }
}

}  // namespace tcmalloc::tcmalloc_internal
GOOGLE_MALLOC_SECTION_END

#endif  // TCMALLOC_INTERNAL_PERCPU_STATE_H_
