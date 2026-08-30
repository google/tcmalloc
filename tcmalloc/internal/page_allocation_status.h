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

#ifndef TCMALLOC_INTERNAL_PAGE_ALLOCATION_STATUS_H_
#define TCMALLOC_INTERNAL_PAGE_ALLOCATION_STATUS_H_

#include <optional>

#include "tcmalloc/internal/range_tracker.h"

namespace tcmalloc::tcmalloc_internal {

struct PageAllocationStatus {
  Bitmap<512> allocated;
};

// Queries TCMalloc for the page allocation status of the hugepage starting at
// `ptr`.
//
// Returns a PageAllocationStatus if `ptr` is hugepage-aligned and managed by
// TCMalloc, or std::nullopt otherwise (including under sanitizers or if
// TCMalloc is not linked).
[[nodiscard]] std::optional<PageAllocationStatus> GetPageAllocationStatus(
    const void* ptr);

}  // namespace tcmalloc::tcmalloc_internal

#endif  // TCMALLOC_INTERNAL_PAGE_ALLOCATION_STATUS_H_
