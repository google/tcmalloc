// Copyright 2025 The TCMalloc Authors
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

#include "tcmalloc/malloc_hook.h"

#include <cstddef>
#include <optional>

#include "absl/base/attributes.h"
#include "absl/base/call_once.h"
#include "absl/base/optimization.h"
#include "tcmalloc/internal/config.h"
#include "tcmalloc/internal/hook_list.h"
#include "tcmalloc/malloc_hook_invoke.h"

extern "C" {

ABSL_ATTRIBUTE_WEAK void MallocHook_InitAtFirstAllocation_HeapLeakChecker() {
  // Do nothing
}
ABSL_ATTRIBUTE_WEAK void MallocHook_InitAtFirstAllocation_ForTesting() {
  // Do nothing
}
ABSL_ATTRIBUTE_WEAK void MallocHook_HooksChanged() {
  // Do Nothing
}

}  // extern "C"

GOOGLE_MALLOC_SECTION_BEGIN
namespace tcmalloc {
namespace tcmalloc_internal {

static void RemoveInitialHooksAndCallInitializers();

void InitAtFirstAllocation() {
  ABSL_CONST_INIT static absl::once_flag once;
  absl::base_internal::LowLevelCallOnce(&once,
                                        RemoveInitialHooksAndCallInitializers);
}

static void InitialNewHook(const MallocHook::NewInfo& info) {
  InitAtFirstAllocation();
  MallocHook::InvokeNewHook(info);
}

ABSL_CONST_INIT HookList<MallocHook::NewHook> new_hooks_{&InitialNewHook};
ABSL_CONST_INIT HookList<MallocHook::DeleteHook> delete_hooks_;

ABSL_CONST_INIT HookList<MallocHook::SampledNewHook> sampled_new_hooks_;
ABSL_CONST_INIT HookList<MallocHook::SampledDeleteHook> sampled_delete_hooks_;

void RemoveInitialHooksAndCallInitializers() {
  ABSL_RAW_CHECK(MallocHook::RemoveNewHook(&InitialNewHook), "");
  // HeapLeakChecker need to get control on the first memory allocation. One can
  // add other modules by following the same weak/strong function pattern.
  MallocHook_InitAtFirstAllocation_HeapLeakChecker();
  MallocHook_InitAtFirstAllocation_ForTesting();
}

}  // namespace tcmalloc_internal

bool MallocHook::AddNewHook(NewHook hook) {
  bool ok = tcmalloc_internal::new_hooks_.Add(hook);
  if (ok) {
    MallocHook_HooksChanged();
  }
  return ok;
}

bool MallocHook::RemoveNewHook(NewHook hook) {
  bool ok = tcmalloc_internal::new_hooks_.Remove(hook);
  if (ok) {
    MallocHook_HooksChanged();
  }
  return ok;
}

bool MallocHook::AddDeleteHook(DeleteHook hook) {
  bool ok = tcmalloc_internal::delete_hooks_.Add(hook);
  if (ok) {
    MallocHook_HooksChanged();
  }
  return ok;
}

bool MallocHook::RemoveDeleteHook(DeleteHook hook) {
  bool ok = tcmalloc_internal::delete_hooks_.Remove(hook);
  if (ok) {
    MallocHook_HooksChanged();
  }
  return ok;
}

bool MallocHook::AddSampledNewHook(SampledNewHook hook) {
  return tcmalloc_internal::sampled_new_hooks_.Add(hook);
}

bool MallocHook::RemoveSampledNewHook(SampledNewHook hook) {
  return tcmalloc_internal::sampled_new_hooks_.Remove(hook);
}

bool MallocHook::AddSampledDeleteHook(SampledDeleteHook hook) {
  return tcmalloc_internal::sampled_delete_hooks_.Add(hook);
}

bool MallocHook::RemoveSampledDeleteHook(SampledDeleteHook hook) {
  return tcmalloc_internal::sampled_delete_hooks_.Remove(hook);
}

}  // namespace tcmalloc
GOOGLE_MALLOC_SECTION_END

#if (defined(ABSL_HAVE_ADDRESS_SANITIZER) ||   \
     defined(ABSL_HAVE_MEMORY_SANITIZER) ||    \
     defined(ABSL_HAVE_THREAD_SANITIZER) ||    \
     defined(ABSL_HAVE_HWADDRESS_SANITIZER) || \
     defined(ABSL_HAVE_DATAFLOW_SANITIZER) ||  \
     defined(ABSL_HAVE_LEAK_SANITIZER))
#include <sanitizer/allocator_interface.h>

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
