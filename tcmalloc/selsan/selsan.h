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

#ifndef TCMALLOC_SELSAN_SELSAN_H_
#define TCMALLOC_SELSAN_SELSAN_H_

#include <stddef.h>

#include "tcmalloc/internal/config.h"
#include "tcmalloc/internal/logging.h"

GOOGLE_MALLOC_SECTION_BEGIN
namespace tcmalloc::tcmalloc_internal::selsan {

#ifdef TCMALLOC_INTERNAL_SELSAN

#if defined(__PIE__) || defined(__PIC__)
inline constexpr uintptr_t kPieBuild = true;
#else
inline constexpr uintptr_t kPieBuild = false;
#endif

#if defined(__x86_64__)
// Note: this is not necessary equal to kAddressBits since we need to cover
// everything kernel can mmap, rather than just the heap.
inline constexpr uintptr_t kAddressSpaceBits = 47;
inline constexpr uintptr_t kTagShift = 57;
inline constexpr uintptr_t kTagResetMask = 1ul << 63;
#elif defined(__aarch64__)
inline constexpr uintptr_t kAddressSpaceBits = 48;
inline constexpr uintptr_t kTagShift = 56;
inline constexpr uintptr_t kTagResetMask = 0;
#else
#error "Unsupported platform."
#endif

inline constexpr uintptr_t kShadowShift = 4;
inline constexpr uintptr_t kShadowScale = 1 << kShadowShift;

// In pie builds we use 0 shadow offset since it's the most efficient to encode
// in instructions. In non-pie builds we cannot use 0 since the executable
// is at 0, instead we use 4GB-2MB because (1) <4GB offsets can be encoded
// efficiently on x86, (2) we want the smallest offset from 4GB to give as much
// memory as possible to the executable, and (3) 2MB alignment allows to use
// huge pages for shadow.
#ifdef TCMALLOC_SELSAN_TEST_SHADOW_OVERRIDE
extern uintptr_t kShadowBase;
#else
inline constexpr uintptr_t kShadowBase =
    kPieBuild ? 0 : (1ul << 32) - (2ul << 20);
// Hex representation of the const for source grepping and compiler flags.
static_assert(kPieBuild || kShadowBase == 0xffe00000);
#endif
inline constexpr uintptr_t kShadowOffset = kPieBuild ? 64 << 10 : 0;

inline ABSL_ATTRIBUTE_ALWAYS_INLINE bool IsEnabled() {
  extern bool enabled;
  return enabled;
}

inline ABSL_ATTRIBUTE_ALWAYS_INLINE size_t RoundUpObjectSize(size_t size) {
  TC_ASSERT(IsEnabled());
  return (size + kShadowScale - 1) & ~(kShadowScale - 1);
}

inline ABSL_ATTRIBUTE_ALWAYS_INLINE void SetTag(uintptr_t ptr, size_t size,
                                                unsigned char tag) {
  TC_ASSERT_NE(size, 0);
  uintptr_t off = (ptr << (64 - kTagShift)) >> (64 - kTagShift + kShadowShift);
  auto* p = reinterpret_cast<unsigned char*>(kShadowBase + off);
  for (size_t i = 0; i < RoundUpObjectSize(size) / kShadowScale; i++) {
    p[i] = tag;
  }
}

inline ABSL_ATTRIBUTE_ALWAYS_INLINE void* RemoveTag(const void* ptr) {
  return reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(ptr) &
                                 ((1ul << kTagShift) - 1));
}

inline ABSL_ATTRIBUTE_ALWAYS_INLINE void* ResetTag(void* ptr, size_t size) {
  TC_ASSERT(IsEnabled());
  TC_ASSERT_EQ(size % kShadowScale, 0);
  SetTag(reinterpret_cast<uintptr_t>(ptr), size, 0);
  return RemoveTag(ptr);
}

inline ABSL_ATTRIBUTE_ALWAYS_INLINE void* UpdateTag(void* ptr, size_t size) {
  TC_ASSERT(IsEnabled());
  uintptr_t p = reinterpret_cast<uintptr_t>(ptr);
  p += 1ul << (kTagShift + 1);
  p &= ~kTagResetMask;
  SetTag(reinterpret_cast<uintptr_t>(ptr), size, p >> 56);
  return reinterpret_cast<void*>(p);
}

void PrintTextStats(Printer* out);
void PrintPbtxtStats(PbtxtRegion* out);

#else  // #ifdef TCMALLOC_INTERNAL_SELSAN

inline size_t RoundUpObjectSize(size_t size) { return size; }
inline void* ResetTag(void* ptr, size_t size) { return ptr; }
inline void* UpdateTag(void* ptr, size_t size) { return ptr; }
inline void* RemoveTag(const void* ptr) { return const_cast<void*>(ptr); }
inline bool IsEnabled() { return false; }
inline void PrintTextStats(Printer* out) {}
inline void PrintPbtxtStats(PbtxtRegion* out) {}

#endif  // #ifdef TCMALLOC_INTERNAL_SELSAN

}  // namespace tcmalloc::tcmalloc_internal::selsan
GOOGLE_MALLOC_SECTION_END

#endif  // TCMALLOC_SELSAN_SELSAN_H_
