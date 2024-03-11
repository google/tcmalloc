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

#include <err.h>
#include <errno.h>  // IWYU pragma: keep
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <unistd.h>
#include <unwind.h>

#ifdef __x86_64__
#include <asm/prctl.h>
#include <sys/syscall.h>
#endif

#include "tcmalloc/internal/config.h"

// This is used by the compiler instrumentation.
uintptr_t __hwasan_shadow_memory_dynamic_address;

GOOGLE_MALLOC_SECTION_BEGIN
namespace tcmalloc::tcmalloc_internal {
namespace {

#if defined(__PIE__) || defined(__PIC__)
constexpr uintptr_t kPieBuild = true;
#else
constexpr uintptr_t kPieBuild = false;
#endif

#if defined(__x86_64__)
// Note: this is not necessary equal to kAddressBits since we need to cover
// everything kernel can mmap, rather than just the heap.
constexpr uintptr_t kAddressSpaceBits = 47;
#elif defined(__aarch64__)
constexpr uintptr_t kAddressSpaceBits = 48;
#else
#error "Unsupported platform."
#endif

constexpr uintptr_t kShadowShift = 4;
constexpr uintptr_t kShadowScale = 1 << kShadowShift;

// In pie builds we use 0 shadow offset since it's the most efficient to encode
// in instructions. In non-pie builds we cannot use 0 since the executable
// is at 0, instead we use 4GB-2MB because (1) <4GB offsets can be encoded
// efficiently on x86, (2) we want the smallest offset from 4GB to give as much
// memory as possible to the executable, and (3) 2MB alignment allows to use
// huge pages for shadow.
constexpr uintptr_t kShadowBase = kPieBuild ? 0 : (1ul << 32) - (2ul << 20);
constexpr uintptr_t kShadowOffset = kPieBuild ? 64 << 10 : 0;

void MapShadow() {
  void* const kShadowStart =
      reinterpret_cast<void*>(kShadowBase + kShadowOffset);
  constexpr uintptr_t kShadowSize =
      (1ul << kAddressSpaceBits) / kShadowScale - kShadowOffset;
#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0x100000
#endif
  if (kShadowStart !=
      mmap(kShadowStart, kShadowSize, PROT_READ | PROT_WRITE,
           MAP_FIXED_NOREPLACE | MAP_NORESERVE | MAP_PRIVATE | MAP_ANON, -1,
           0)) {
    err(1, "tcmalloc: selsan: mmap failed");
  }
  __hwasan_shadow_memory_dynamic_address = kShadowBase;
  madvise(kShadowStart, kShadowSize, MADV_DONTDUMP);
}

bool EnableTBI() {
#if defined(__x86_64__)
#ifndef ARCH_ENABLE_TAGGED_ADDR
#define ARCH_ENABLE_TAGGED_ADDR 0x4002
#endif
  return TEMP_FAILURE_RETRY(syscall(SYS_arch_prctl, ARCH_ENABLE_TAGGED_ADDR,
                                    /*LAM_U57_BITS*/ 6)) == 0;
#elif defined(__aarch64__)
#ifndef PR_SET_TAGGED_ADDR_CTRL
#define PR_SET_TAGGED_ADDR_CTRL 55
#endif
#ifndef PR_TAGGED_ADDR_ENABLE
#define PR_TAGGED_ADDR_ENABLE (1UL << 0)
#endif
  return prctl(PR_SET_TAGGED_ADDR_CTRL, PR_TAGGED_ADDR_ENABLE, 0, 0, 0) == 0;
#else
  return false;
#endif
}

void Init() {
  MapShadow();
  EnableTBI();
}

void CheckAccess(uintptr_t p, size_t n, bool write) {
  // Not implemented yet.
}

}  // namespace

extern "C" {

void __hwasan_loadN(uintptr_t p, size_t n) { CheckAccess(p, n, false); }
void __hwasan_load1(uintptr_t p) { CheckAccess(p, 1, false); }
void __hwasan_load2(uintptr_t p) { CheckAccess(p, 2, false); }
void __hwasan_load4(uintptr_t p) { CheckAccess(p, 4, false); }
void __hwasan_load8(uintptr_t p) { CheckAccess(p, 8, false); }
void __hwasan_load16(uintptr_t p) { CheckAccess(p, 16, false); }

void __hwasan_storeN(uintptr_t p, size_t n) { CheckAccess(p, n, true); }
void __hwasan_store1(uintptr_t p) { CheckAccess(p, 1, true); }
void __hwasan_store2(uintptr_t p) { CheckAccess(p, 2, true); }
void __hwasan_store4(uintptr_t p) { CheckAccess(p, 4, true); }
void __hwasan_store8(uintptr_t p) { CheckAccess(p, 8, true); }
void __hwasan_store16(uintptr_t p) { CheckAccess(p, 16, true); }

void __hwasan_tag_memory() {
  // Not implemented yet.
}

typedef _Unwind_Reason_Code PersonalityFn(int version, _Unwind_Action actions,
                                          uint64_t exception_class,
                                          _Unwind_Exception* unwind_exception,
                                          _Unwind_Context* context);

_Unwind_Reason_Code __hwasan_personality_wrapper(
    int version, _Unwind_Action actions, uint64_t exception_class,
    _Unwind_Exception* unwind_exception, _Unwind_Context* context,
    PersonalityFn* real_personality, void* get_gr, void* get_cfa) {
  // TODO: implement stack untagging once we do stack tagging.
  return real_personality ? real_personality(version, actions, exception_class,
                                             unwind_exception, context)
                          : _URC_CONTINUE_UNWIND;
}

void __hwasan_tag_mismatch_v2() {
  fprintf(stderr, "selsan: __hwasan_tag_mismatch_v2 is not implemented\n");
  abort();
}

void __hwasan_init() {}

}  // extern "C"

__attribute__((section(".preinit_array"), used)) static void (*init)() = Init;

}  // namespace tcmalloc::tcmalloc_internal
GOOGLE_MALLOC_SECTION_END
