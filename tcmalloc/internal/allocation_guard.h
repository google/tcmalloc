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

#ifndef TCMALLOC_INTERNAL_ALLOCATION_GUARD_H_
#define TCMALLOC_INTERNAL_ALLOCATION_GUARD_H_

#include "absl/base/attributes.h"
#include "absl/base/internal/spinlock.h"
#include "absl/base/thread_annotations.h"
#include "tcmalloc/internal/config.h"
#include "tcmalloc/malloc_hook.h"

GOOGLE_MALLOC_SECTION_BEGIN
namespace tcmalloc::tcmalloc_internal {

// In debug mode, ensures that no allocations will occur on the current thread
// while this class is in scope.
class AllocationGuard {
 public:
  AllocationGuard() {
#ifndef NDEBUG
    if (disallowed_ == 0) {
      (void)MallocHook::AddNewHook(Hook);
    }
    ++disallowed_;
#endif
  }
  ~AllocationGuard() {
#ifndef NDEBUG
    --disallowed_;
    if (disallowed_ == 0) {
      (void)MallocHook::RemoveNewHook(Hook);
    }
#endif
  }

 private:
  static void Hook(const MallocHook::NewInfo& info) {
    if (disallowed_ > 0) abort();
  }

  ABSL_CONST_INIT static thread_local int disallowed_;
};

// A SpinLockHolder that also enforces no allocations while the lock is held in
// debug mode.
class ABSL_SCOPED_LOCKABLE AllocationGuardSpinLockHolder {
 public:
  explicit AllocationGuardSpinLockHolder(absl::base_internal::SpinLock* l)
      ABSL_EXCLUSIVE_LOCK_FUNCTION(l)
      : lock_holder_(l) {
#ifndef NDEBUG
    if (l->IsCooperative()) {
      abort();
    }
#endif  // NDEBUG
  }

  inline ~AllocationGuardSpinLockHolder() ABSL_UNLOCK_FUNCTION() = default;

 private:
  absl::base_internal::SpinLockHolder lock_holder_;
  // In debug mode, enforces no allocations.
  AllocationGuard enforce_no_alloc_;
};

}  // namespace tcmalloc::tcmalloc_internal
GOOGLE_MALLOC_SECTION_END

#endif  // TCMALLOC_INTERNAL_ALLOCATION_GUARD_H_
