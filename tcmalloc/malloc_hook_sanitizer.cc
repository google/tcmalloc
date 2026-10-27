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

#include "absl/base/config.h"

#if (defined(ABSL_HAVE_ADDRESS_SANITIZER) ||   \
     defined(ABSL_HAVE_MEMORY_SANITIZER) ||    \
     defined(ABSL_HAVE_THREAD_SANITIZER) ||    \
     defined(ABSL_HAVE_HWADDRESS_SANITIZER) || \
     defined(ABSL_HAVE_DATAFLOW_SANITIZER) ||  \
     defined(ABSL_HAVE_LEAK_SANITIZER))
#include <sanitizer/allocator_interface.h>

#include <cstddef>
#include <optional>

#include "absl/base/optimization.h"
#include "tcmalloc/malloc_hook.h"
#include "tcmalloc/malloc_hook_invoke.h"

extern "C" {

// __sanitizer_malloc_hook and __sanitizer_free_hook are called
// by (de)allocation functions in sanitizer runtimes.
void __sanitizer_malloc_hook(const volatile void* ptr, size_t size) {
  if (ABSL_PREDICT_FALSE(!tcmalloc::tcmalloc_internal::new_hooks_.empty())) {
    tcmalloc::MallocHook::InvokeNewHook(
        {const_cast<void*>(ptr), size, size,
         tcmalloc::HookMemoryMutable::kImmutable});
  }
}

void __sanitizer_free_hook(const volatile void* ptr) {
  if (ABSL_PREDICT_FALSE(!tcmalloc::tcmalloc_internal::delete_hooks_.empty())) {
    tcmalloc::MallocHook::InvokeDeleteHook(
        {const_cast<void*>(ptr), std::nullopt,
         __sanitizer_get_allocated_size_fast(ptr),
         tcmalloc::HookMemoryMutable::kImmutable});
  }
}

}  // extern "C"
#endif
