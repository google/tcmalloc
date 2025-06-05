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

#ifndef TCMALLOC_MALLOC_HOOK_INVOKE_H_
#define TCMALLOC_MALLOC_HOOK_INVOKE_H_

#include "tcmalloc/internal/config.h"
#include "tcmalloc/internal/hook_list.h"
#include "tcmalloc/malloc_hook.h"

GOOGLE_MALLOC_SECTION_BEGIN
namespace tcmalloc {
namespace tcmalloc_internal {

extern HookList<MallocHook::NewHook> new_hooks_;
extern HookList<MallocHook::DeleteHook> delete_hooks_;

extern HookList<MallocHook::SampledNewHook> sampled_new_hooks_;
extern HookList<MallocHook::SampledDeleteHook> sampled_delete_hooks_;

}  // namespace tcmalloc_internal

inline void MallocHook::InvokeNewHook(const NewInfo& info) {
  if (!tcmalloc_internal::new_hooks_.empty()) {
    InvokeNewHookSlow(info);
  }
}

inline void MallocHook::InvokeDeleteHook(const DeleteInfo& info) {
  if (!tcmalloc_internal::delete_hooks_.empty()) {
    InvokeDeleteHookSlow(info);
  }
}

inline void MallocHook::InvokeSampledNewHook(
    const SampledAlloc& sampled_alloc) {
  if (!tcmalloc_internal::sampled_new_hooks_.empty()) {
    InvokeSampledNewHookSlow(sampled_alloc);
  }
}

inline void MallocHook::InvokeSampledDeleteHook(
    const
    SampledAlloc& sampled_alloc) {
  if (!tcmalloc_internal::sampled_delete_hooks_.empty()) {
    InvokeSampledDeleteHookSlow(sampled_alloc);
  }
}

}  // namespace tcmalloc
GOOGLE_MALLOC_SECTION_END

#endif  // TCMALLOC_MALLOC_HOOK_INVOKE_H_
