// Copyright 2023 The TCMalloc Authors
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

#include "tcmalloc/internal/allocation_guard.h"

#include "absl/base/attributes.h"
#include "absl/base/const_init.h"
#include "absl/base/internal/spinlock.h"
#include "absl/base/thread_annotations.h"
#include "tcmalloc/internal/config.h"
#include "tcmalloc/malloc_hook.h"

GOOGLE_MALLOC_SECTION_BEGIN
namespace tcmalloc::tcmalloc_internal {

#ifndef NDEBUG
namespace {
ABSL_CONST_INIT absl::base_internal::SpinLock guard_lock_(
    absl::base_internal::SCHEDULE_KERNEL_ONLY);
ABSL_CONST_INIT int active_guards_ ABSL_GUARDED_BY(guard_lock_) = 0;
ABSL_CONST_INIT bool hook_installed_ ABSL_GUARDED_BY(guard_lock_) = false;
}  // namespace

ABSL_CONST_INIT thread_local int AllocationGuard::disallowed_ = 0;

void AllocationGuard::RegisterThreadGuard() {
  AllocationGuardSpinLockHolder l(guard_lock_);
  if (active_guards_++ == 0) {
    hook_installed_ = MallocHook::AddNewHook(Hook);
  }
}

void AllocationGuard::UnregisterThreadGuard() {
  AllocationGuardSpinLockHolder l(guard_lock_);
  if (--active_guards_ == 0 && hook_installed_) {
    (void)MallocHook::RemoveNewHook(Hook);
    hook_installed_ = false;
  }
}
#endif

}  // namespace tcmalloc::tcmalloc_internal
GOOGLE_MALLOC_SECTION_END
