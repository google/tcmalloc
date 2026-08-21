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

#ifndef TCMALLOC_INTERNAL_IS_ALIGNED_TO_H_
#define TCMALLOC_INTERNAL_IS_ALIGNED_TO_H_

#include <cstddef>
#include <cstdint>
#include <new>

#include "absl/numeric/bits.h"
#include "tcmalloc/internal/config.h"
#include "tcmalloc/internal/logging.h"

GOOGLE_MALLOC_SECTION_BEGIN
namespace tcmalloc::tcmalloc_internal {

constexpr bool IsAlignedTo(uintptr_t val, size_t alignment) {
  TC_ASSERT(absl::has_single_bit(alignment),
            "Alignment %zu needs to be a power of two", alignment);
  return (val & (alignment - 1)) == 0;
}

constexpr bool IsAlignedTo(uintptr_t val, std::align_val_t alignment) {
  return IsAlignedTo(val, static_cast<size_t>(alignment));
}

inline bool IsAlignedTo(const void* addr, size_t alignment) {
  return IsAlignedTo(reinterpret_cast<uintptr_t>(addr), alignment);
}

inline bool IsAlignedTo(const void* addr, std::align_val_t alignment) {
  return IsAlignedTo(reinterpret_cast<uintptr_t>(addr),
                     static_cast<size_t>(alignment));
}

}  // namespace tcmalloc::tcmalloc_internal
GOOGLE_MALLOC_SECTION_END

#endif  // TCMALLOC_INTERNAL_IS_ALIGNED_TO_H_
