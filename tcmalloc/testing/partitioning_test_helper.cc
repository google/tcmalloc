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

#include <cstdlib>

#include "absl/strings/str_format.h"
#include "absl/strings/string_view.h"
#include "tcmalloc/internal/memory_tag.h"
#include "tcmalloc/internal/parameter_accessors.h"
#include "tcmalloc/malloc_extension.h"

using ::tcmalloc::tcmalloc_internal::GetMemoryTag;
using ::tcmalloc::tcmalloc_internal::MemoryTagToLabel;

extern "C" {
void* __alloc_token_0_malloc(size_t) noexcept;
void* __alloc_token_1_malloc(size_t) noexcept;
}

int main() {
  void* ptr_0 = __alloc_token_0_malloc(8);
  void* ptr_1 = __alloc_token_1_malloc(8);

  bool heap_partitioning = TCMalloc_Internal_GetHeapPartitioning();
  bool security_partitioning = tcmalloc::MallocExtension::GetNumericProperty(
                                   "tcmalloc.security_partitioning_active")
                                   .value_or(0);

  absl::string_view tag_0 = MemoryTagToLabel(GetMemoryTag(ptr_0));
  absl::string_view tag_1 = MemoryTagToLabel(GetMemoryTag(ptr_1));

  absl::PrintF("heap_partitioning:%d\n", heap_partitioning);
  absl::PrintF("security_partitioning:%d\n", security_partitioning);
  absl::PrintF("ptr_0_tag:%s\n", tag_0);
  absl::PrintF("ptr_1_tag:%s\n", tag_1);

  free(ptr_0);
  free(ptr_1);
  return 0;
}
