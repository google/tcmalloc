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
#include "tcmalloc/malloc_extension.h"

using ::tcmalloc::tcmalloc_internal::GetMemoryTag;
using ::tcmalloc::tcmalloc_internal::MemoryTagToLabel;

extern "C" {
void* __alloc_token_0_malloc(size_t) noexcept;
void* __alloc_token_1_malloc(size_t) noexcept;
void* __alloc_token_0__Znwm(size_t);
}

int main() {
  tcmalloc::MallocExtension::SetProfileSamplingInterval(-1);
  void* ptr_0 = __alloc_token_0_malloc(8);
  void* ptr_1 = __alloc_token_1_malloc(8);
  void* ptr_0_sz0 = __alloc_token_0_malloc(0);
  void* ptr_1_sz0 = __alloc_token_1_malloc(0);
  void* new_0 = __alloc_token_0__Znwm(8);
  void* new_0_large = __alloc_token_0__Znwm(512 << 10);

  int mode = tcmalloc::MallocExtension::GetNumericProperty(
                 "tcmalloc.security_partitioning_active")
                 .value_or(0);
  bool security_partitioning = !!mode;

  absl::string_view tag_0 = MemoryTagToLabel(GetMemoryTag(ptr_0));
  absl::string_view tag_1 = MemoryTagToLabel(GetMemoryTag(ptr_1));
  absl::string_view tag_0_sz0 = MemoryTagToLabel(GetMemoryTag(ptr_0_sz0));
  absl::string_view tag_1_sz0 = MemoryTagToLabel(GetMemoryTag(ptr_1_sz0));

  absl::PrintF("security_partitioning:%d\n", security_partitioning);
  absl::PrintF("security_partitioning_mode:%d\n", mode);
  absl::PrintF("ptr_0_tag:%s\n", tag_0);
  absl::PrintF("ptr_1_tag:%s\n", tag_1);
  absl::PrintF("ptr_0_sz0_tag:%s\n", tag_0_sz0);
  absl::PrintF("ptr_1_sz0_tag:%s\n", tag_1_sz0);
  absl::PrintF("new_0_tag:%s\n", MemoryTagToLabel(GetMemoryTag(new_0)));
  absl::PrintF("new_0_large_tag:%s\n",
               MemoryTagToLabel(GetMemoryTag(new_0_large)));

  ::operator delete(new_0_large);
  ::operator delete(new_0);
  free(ptr_0);
  free(ptr_1);
  free(ptr_0_sz0);
  free(ptr_1_sz0);
  return 0;
}
