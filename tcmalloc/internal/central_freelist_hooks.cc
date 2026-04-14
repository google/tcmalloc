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

#include "tcmalloc/internal/central_freelist_hooks.h"

#include <cstddef>

#include "absl/base/attributes.h"
#include "absl/base/call_once.h"
#include "absl/base/optimization.h"
#include "absl/types/span.h"
#include "tcmalloc/internal/config.h"
#include "tcmalloc/internal/hook_list.h"
#include "tcmalloc/internal/logging.h"

extern "C" {
ABSL_ATTRIBUTE_WEAK void
TCMalloc_CentralFreeList_InitAtFirstRemoveRange_Tracing();
}

GOOGLE_MALLOC_SECTION_BEGIN
namespace tcmalloc {
namespace tcmalloc_internal {

namespace {

void InitialRemoveRangeHook(size_t size_class, absl::Span<void*> batch);

void RemoveInitialHooksAndCallInitializers() {
  TC_CHECK(central_freelist_remove_range_hooks.Remove(&InitialRemoveRangeHook));
  if (TCMalloc_CentralFreeList_InitAtFirstRemoveRange_Tracing != nullptr) {
    TCMalloc_CentralFreeList_InitAtFirstRemoveRange_Tracing();
  }
}

void InitialRemoveRangeHook(size_t size_class, absl::Span<void*> batch) {
  ABSL_CONST_INIT static absl::once_flag once;
  absl::base_internal::LowLevelCallOnce(&once,
                                        RemoveInitialHooksAndCallInitializers);

  if (ABSL_PREDICT_FALSE(!central_freelist_remove_range_hooks.empty())) {
    central_freelist_remove_range_hooks.Invoke(size_class, batch);
  }
}

}  // namespace

ABSL_CONST_INIT HookList<CentralFreelistInsertRangeHook>
    central_freelist_insert_range_hooks;
ABSL_CONST_INIT HookList<CentralFreelistRemoveRangeHook>
    central_freelist_remove_range_hooks{&InitialRemoveRangeHook};

}  // namespace tcmalloc_internal
}  // namespace tcmalloc
GOOGLE_MALLOC_SECTION_END
