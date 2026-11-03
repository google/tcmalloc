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
#include "tcmalloc/internal/config.h"

GOOGLE_MALLOC_SECTION_BEGIN
namespace tcmalloc::tcmalloc_internal {

#ifndef NDEBUG
ABSL_CONST_INIT thread_local int AllocationGuard::disallowed_ = 0;
ABSL_CONST_INIT absl::base_internal::SpinLock AllocationGuard::hook_lock_(
    absl::base_internal::SCHEDULE_KERNEL_ONLY);
ABSL_CONST_INIT int AllocationGuard::active_guard_threads_ = 0;
#endif

}  // namespace tcmalloc::tcmalloc_internal
GOOGLE_MALLOC_SECTION_END
