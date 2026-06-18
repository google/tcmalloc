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

#ifndef TCMALLOC_INTERNAL_SCOPED_ALLOW_ALLOCATION_H_
#define TCMALLOC_INTERNAL_SCOPED_ALLOW_ALLOCATION_H_

#include "tcmalloc/internal/allocation_guard.h"

namespace tcmalloc::tcmalloc_internal {

class ScopedAllocationAllow {
 public:
  ScopedAllocationAllow() {
#ifndef NDEBUG
    --AllocationGuard::disallowed_;
#endif
  }
  ~ScopedAllocationAllow() {
#ifndef NDEBUG
    ++AllocationGuard::disallowed_;
#endif
  }
};

}  // namespace tcmalloc::tcmalloc_internal

#endif  // TCMALLOC_INTERNAL_SCOPED_ALLOW_ALLOCATION_H_
