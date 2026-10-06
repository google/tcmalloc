// Copyright 2024 The TCMalloc Authors
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

#ifndef TCMALLOC_INTERNAL_MEMORY_TAG_H_
#define TCMALLOC_INTERNAL_MEMORY_TAG_H_

#include <cstdint>

#include "absl/strings/string_view.h"
#include "tcmalloc/internal/address_bits.h"
#include "tcmalloc/internal/config.h"
#include "tcmalloc/internal/logging.h"

GOOGLE_MALLOC_SECTION_BEGIN
namespace tcmalloc::tcmalloc_internal {

enum class MemoryTag : uint8_t {
  // Sampled, infrequently allocated
  kSampled = 0x0,
  kSampledP1 = kSanitizerAddressSpace ? 0xf8 : 0x1,
  // Normal memory, NUMA or security partition 0
  kNormalP0 = kSanitizerAddressSpace ? 0x1 : 0x4,
  // Normal memory, NUMA or security partition 1
  kNormalP1 = kSanitizerAddressSpace ? 0xff : 0x6,
  // Normal memory
  kNormal = kNormalP0,
  // Cold
  kCold = 0x2,
  // Metadata
  kMetadata = 0x3,
};

inline uintptr_t TagShift() {
  // The tag bits ride just below the top of the *effective* user address
  // space, so that one binary keeps working on kernels built with a narrower
  // virtual address width (e.g. 39-bit Raspberry Pi OS kernels).  The values
  // are cached after first use; the width cannot change during the lifetime
  // of a process.  On a 48-bit kernel this evaluates to 42, exactly the
  // previous compile-time constant.
  static const uintptr_t kShift = [] {
    const uintptr_t shift = static_cast<uintptr_t>(EffectiveAddressBits()) - 4;
    return shift < 42 ? shift : 42;
  }();
  return kShift;
}

inline uintptr_t TagMask() {
  static const uintptr_t kMask =
      uintptr_t{kSanitizerAddressSpace ? 0x3 : 0x7} << TagShift();
  return kMask;
}

[[nodiscard]] inline MemoryTag GetMemoryTag(const void* ptr) {
  return static_cast<MemoryTag>((reinterpret_cast<uintptr_t>(ptr) & TagMask()) >>
                                TagShift());
}

[[nodiscard]] inline bool IsNormalMemory(const void* ptr) {
  // This is slightly faster than checking kNormalP0/P1 separetly.
  static_assert((static_cast<uint8_t>(MemoryTag::kNormalP0) &
                 (static_cast<uint8_t>(MemoryTag::kSampled) |
                  static_cast<uint8_t>(MemoryTag::kSampledP1) |
                  static_cast<uint8_t>(MemoryTag::kCold))) == 0);
  bool res = (static_cast<uintptr_t>(GetMemoryTag(ptr)) &
              static_cast<uintptr_t>(MemoryTag::kNormal)) != 0;
  TC_ASSERT(res == (GetMemoryTag(ptr) == MemoryTag::kNormalP0 ||
                    GetMemoryTag(ptr) == MemoryTag::kNormalP1),
            "ptr=%p res=%d tag=%d", ptr, res,
            static_cast<int>(GetMemoryTag(ptr)));
  return res;
}

[[nodiscard]] inline bool IsSampledMemory(const void* ptr) {
  bool res = (static_cast<uintptr_t>(GetMemoryTag(ptr)) &
              ~static_cast<uintptr_t>(MemoryTag::kSampledP1)) == 0;
  TC_ASSERT(res == (GetMemoryTag(ptr) == MemoryTag::kSampled ||
                    GetMemoryTag(ptr) == MemoryTag::kSampledP1),
            "ptr=%p res=%d tag=%d", ptr, res,
            static_cast<int>(GetMemoryTag(ptr)));
  return res;
}

[[nodiscard]] absl::string_view MemoryTagToLabel(MemoryTag tag);

}  // namespace tcmalloc::tcmalloc_internal
GOOGLE_MALLOC_SECTION_END

#endif  // TCMALLOC_INTERNAL_MEMORY_TAG_H_
