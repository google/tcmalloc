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

#include "tcmalloc/internal/page_allocator_hooks.h"

#include <cstddef>
#include <cstdint>

#include "absl/base/attributes.h"
#include "absl/base/call_once.h"
#include "absl/base/optimization.h"
#include "tcmalloc/internal/config.h"
#include "tcmalloc/internal/hook_list.h"
#include "tcmalloc/internal/logging.h"
#include "tcmalloc/internal/memory_tag.h"

extern "C" {
ABSL_ATTRIBUTE_WEAK void TCMalloc_PageAllocator_InitAtFirstNew_Tracing();
}

GOOGLE_MALLOC_SECTION_BEGIN
namespace tcmalloc {
namespace tcmalloc_internal {

namespace {

void InitialNewHook(size_t start_page_index, size_t n, size_t align,
                    size_t objects_per_span, uint8_t density, MemoryTag tag);

void RemoveInitialHooksAndCallInitializers() {
  TC_CHECK(page_allocator_new_hooks.Remove(&InitialNewHook));
  if (TCMalloc_PageAllocator_InitAtFirstNew_Tracing != nullptr) {
    TCMalloc_PageAllocator_InitAtFirstNew_Tracing();
  }
}

void InitialNewHook(size_t start_page_index, size_t n, size_t align,
                    size_t objects_per_span, uint8_t density, MemoryTag tag) {
  ABSL_CONST_INIT static absl::once_flag once;
  absl::base_internal::LowLevelCallOnce(&once,
                                        RemoveInitialHooksAndCallInitializers);

  if (ABSL_PREDICT_FALSE(!page_allocator_new_hooks.empty())) {
    page_allocator_new_hooks.Invoke(start_page_index, n, align,
                                    objects_per_span, density, tag);
  }
}

}  // namespace

ABSL_CONST_INIT HookList<PageAllocatorNewHook> page_allocator_new_hooks{
    &InitialNewHook};
ABSL_CONST_INIT HookList<PageAllocatorDeleteHook> page_allocator_delete_hooks;

}  // namespace tcmalloc_internal
}  // namespace tcmalloc
GOOGLE_MALLOC_SECTION_END
