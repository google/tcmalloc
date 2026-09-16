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

#include <atomic>

#include "absl/base/attributes.h"
#include "absl/base/call_once.h"
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

static void InitialNewHook(const MallocHook::NewInfo& info) {
  ABSL_CONST_INIT static absl::once_flag once;
  absl::base_internal::LowLevelCallOnce(&once,
                                        RemoveInitialHooksAndCallInitializers);
  MallocHook::InvokeNewHook(info);
}

ABSL_CONST_INIT HookList<MallocHook::NewHook> new_hooks_{&InitialNewHook};
ABSL_CONST_INIT HookList<MallocHook::DeleteHook> delete_hooks_;
// new_hooks_ starts out with InitialNewHook installed.
ABSL_CONST_INIT std::atomic<int> new_delete_hook_count_{1};

ABSL_CONST_INIT HookList<MallocHook::SampledNewHook> sampled_new_hooks_;
ABSL_CONST_INIT HookList<MallocHook::SampledDeleteHook> sampled_delete_hooks_;

void RemoveInitialHooksAndCallInitializers() {
  ABSL_RAW_CHECK(MallocHook::RemoveNewHook(&InitialNewHook), "");
  // HeapLeakChecker need to get control on the first memory allocation. One can
  // add other modules by following the same weak/strong function pattern.
  MallocHook_InitAtFirstAllocation_HeapLeakChecker();
  MallocHook_InitAtFirstAllocation_ForTesting();
}

namespace {

// AddHook/RemoveHook wrap HookList::Add/Remove for new_hooks_ and
// delete_hooks_, keeping new_delete_hook_count_ nonzero whenever either list
// is nonempty.
//
// The count is adjusted outside of HookList's hooklist_spinlock_, so we cannot
// recompute it from the lists (a concurrent Remove on the other list could
// observe this list as empty and publish a stale value).  Instead, we count
// successful insertions and removals, which commute: we increment before the
// hook becomes visible in the list and decrement after it is gone, so the count
// is an upper bound on the number of installed hooks at every instant and
// exact once no Add/Remove is in flight.
//
// Relaxed RMWs suffice.  HookList::Add publishes the hook with a release store
// that is sequenced after our increment, so a reader that observes the hook
// (with the acquire loads in HookList::Traverse) also observes a nonzero count.
template <typename T>
bool AddHook(HookList<T>& list, T hook) {
  new_delete_hook_count_.fetch_add(1, std::memory_order_relaxed);
  if (!list.Add(hook)) {
    new_delete_hook_count_.fetch_sub(1, std::memory_order_relaxed);
    return false;
  }
  MallocHook_HooksChanged();
  return true;
}

template <typename T>
bool RemoveHook(HookList<T>& list, T hook) {
  if (!list.Remove(hook)) {
    return false;
  }
  new_delete_hook_count_.fetch_sub(1, std::memory_order_relaxed);
  MallocHook_HooksChanged();
  return true;
}

}  // namespace
}  // namespace tcmalloc_internal

bool MallocHook::AddNewHook(NewHook hook) {
  return tcmalloc_internal::AddHook(tcmalloc_internal::new_hooks_, hook);
}

bool MallocHook::RemoveNewHook(NewHook hook) {
  return tcmalloc_internal::RemoveHook(tcmalloc_internal::new_hooks_, hook);
}

bool MallocHook::AddDeleteHook(DeleteHook hook) {
  return tcmalloc_internal::AddHook(tcmalloc_internal::delete_hooks_, hook);
}

bool MallocHook::RemoveDeleteHook(DeleteHook hook) {
  return tcmalloc_internal::RemoveHook(tcmalloc_internal::delete_hooks_, hook);
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
