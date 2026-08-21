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

#include "tcmalloc/internal/page_allocation_status.h"

#include <optional>

#include "tcmalloc/internal/parameter_accessors.h"

namespace tcmalloc::tcmalloc_internal {

std::optional<PageAllocationStatus> GetPageAllocationStatus(const void* ptr) {
#ifndef __APPLE__
  if (&TCMalloc_Internal_GetPageAllocationStatus == nullptr) {
    return std::nullopt;
  }
  PageAllocationStatus status;
  if (!TCMalloc_Internal_GetPageAllocationStatus(ptr, &status)) {
    return std::nullopt;
  }
  return status;
#else
  return std::nullopt;
#endif
}

}  // namespace tcmalloc::tcmalloc_internal
