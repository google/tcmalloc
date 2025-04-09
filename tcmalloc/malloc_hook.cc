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

#include "absl/base/attributes.h"
#include "tcmalloc/internal/config.h"
#include "tcmalloc/internal/hook_list.h"

GOOGLE_MALLOC_SECTION_BEGIN
namespace tcmalloc {
namespace tcmalloc_internal {

ABSL_CONST_INIT HookList<MallocHook::SampledNewHook> sampled_new_hooks_;
ABSL_CONST_INIT HookList<MallocHook::SampledDeleteHook> sampled_delete_hooks_;

}  // namespace tcmalloc_internal

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

// Note: embedding the function calls inside the traversal of HookList would be
// very confusing, as it is legal for a hook to remove itself and add other
// hooks.  Doing traversal first, and then calling the hooks ensures we only
// call the hooks registered at the start.
#define INVOKE_HOOKS(HookType, hook_list, args)                           \
  do {                                                                    \
    HookType hooks[tcmalloc_internal::kHookListMaxValues];                \
    int num_hooks =                                                       \
        hook_list.Traverse(hooks, tcmalloc_internal::kHookListMaxValues); \
    for (int i = 0; i < num_hooks; ++i) {                                 \
      (*hooks[i]) args;                                                   \
    }                                                                     \
  } while (0)

void MallocHook::InvokeSampledNewHookSlow(const SampledAlloc& sampled_alloc) {
  INVOKE_HOOKS(SampledNewHook, tcmalloc_internal::sampled_new_hooks_,
               (sampled_alloc));
}

void MallocHook::InvokeSampledDeleteHookSlow(
    const
    SampledAlloc& sampled_alloc) {
  INVOKE_HOOKS(SampledDeleteHook, tcmalloc_internal::sampled_delete_hooks_,
               (sampled_alloc));
}

}  // namespace tcmalloc
GOOGLE_MALLOC_SECTION_END
