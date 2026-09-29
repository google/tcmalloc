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

#include <algorithm>
#include <cstdint>

#include "absl/strings/string_view.h"
#include "tcmalloc/internal/config.h"
#include "tcmalloc/internal/logging.h"

GOOGLE_MALLOC_SECTION_BEGIN
namespace tcmalloc::tcmalloc_internal {

enum class MemoryTag : uint8_t {
  // Sampled, infrequently allocated
  kSampledOrCold = 0x0,
  kSampledOrColdP1 = kSanitizerAddressSpace ? 0xf8 : 0x1,
  // Normal memory, NUMA or security partition 0
  kNormalP0 = kSanitizerAddressSpace ? 0x1 : 0x4,
  // Normal memory, NUMA or security partition 1
  kNormalP1 = kSanitizerAddressSpace ? 0xff : 0x6,
  // Normal memory
  kNormal = kNormalP0,
  // Metadata
  kMetadata = 0x3,
};

inline constexpr uintptr_t kTagShift = std::min(kAddressBits - 4, 42);
inline constexpr uintptr_t kTagMask =
    uintptr_t{kSanitizerAddressSpace ? 0x3 : 0x7} << kTagShift;

// Each kSampledOrCold{,P1} partition is split by the bit below the tag:
//   bit 41 == 0: the GWP-ASan guarded pool, and nothing else.
//   bit 41 == 1: the page heap (sampled spans and cold objects).
// The sized-free fast path uses this to exclude guarded pointers, which may be
// right-aligned in their page, at no extra cost.  See kColdFastFreeMask.
//
// Under TSan on x86-64, tag 0 only overlaps TSan's low app range [0, 2 TiB),
// where bit 41 is always 0, so bit 40 is used instead.
#if defined(ABSL_HAVE_THREAD_SANITIZER) && defined(__x86_64__)
inline constexpr uintptr_t kSubRegionShift = kTagShift - 2;
#else
inline constexpr uintptr_t kSubRegionShift = kTagShift - 1;
#endif
inline constexpr uintptr_t kPageHeapSubRegionBit = uintptr_t{1}
                                                   << kSubRegionShift;

// Only meaningful for pointers tagged kSampledOrCold{,P1}.
inline bool InPageHeapSubRegion(const void* ptr) {
  return (reinterpret_cast<uintptr_t>(ptr) & kPageHeapSubRegionBit) != 0;
}
inline bool InGuardedSubRegion(const void* ptr) {
  return !InPageHeapSubRegion(ptr);
}

constexpr bool IsSampledOrColdTag(MemoryTag tag) {
  return tag == MemoryTag::kSampledOrCold || tag == MemoryTag::kSampledOrColdP1;
}

// Which half of a kSampledOrCold partition an mmap comes from.
enum class SubRegion { kPageHeap, kGuarded };

inline MemoryTag GetMemoryTag(const void* ptr) {
  return static_cast<MemoryTag>((reinterpret_cast<uintptr_t>(ptr) & kTagMask) >>
                                kTagShift);
}

inline bool IsNormalMemory(const void* ptr) {
  // This is slightly faster than checking kNormalP0/P1 separately.
  static_assert((static_cast<uint8_t>(MemoryTag::kNormalP0) &
                 (static_cast<uint8_t>(MemoryTag::kSampledOrCold) |
                  static_cast<uint8_t>(MemoryTag::kSampledOrColdP1))) == 0);
  bool res = (static_cast<uintptr_t>(GetMemoryTag(ptr)) &
              static_cast<uintptr_t>(MemoryTag::kNormal)) != 0;
  TC_ASSERT(res == (GetMemoryTag(ptr) == MemoryTag::kNormalP0 ||
                    GetMemoryTag(ptr) == MemoryTag::kNormalP1),
            "ptr=%p res=%d tag=%d", ptr, res,
            static_cast<int>(GetMemoryTag(ptr)));
  return res;
}

inline bool IsSampledOrColdMemory(MemoryTag tag) {
  return tag == MemoryTag::kSampledOrCold || tag == MemoryTag::kSampledOrColdP1;
}

inline bool IsSampledOrColdMemory(const void* ptr) {
  bool res = (static_cast<uintptr_t>(GetMemoryTag(ptr)) &
              ~static_cast<uintptr_t>(MemoryTag::kSampledOrColdP1)) == 0;
  TC_ASSERT(res == IsSampledOrColdMemory(GetMemoryTag(ptr)),
            "ptr=%p res=%d tag=%d", ptr, res,
            static_cast<int>(GetMemoryTag(ptr)));
  return res;
}

absl::string_view MemoryTagToLabel(MemoryTag tag);

}  // namespace tcmalloc::tcmalloc_internal
GOOGLE_MALLOC_SECTION_END

#endif  // TCMALLOC_INTERNAL_MEMORY_TAG_H_
