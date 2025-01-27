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

#include "tcmalloc/selsan/selsan.h"  // IWYU pragma: keep

#ifdef TCMALLOC_INTERNAL_SELSAN

#include <err.h>
#include <errno.h>  // IWYU pragma: keep
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <unistd.h>
#include <unwind.h>

#include <atomic>
#include <utility>

#ifdef __x86_64__
#include <asm/prctl.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <ucontext.h>
#endif

#include "absl/base/attributes.h"
#include "tcmalloc/internal/config.h"
#include "tcmalloc/internal/exponential_biased.h"
#include "tcmalloc/internal/logging.h"

// This is used by the compiler instrumentation.
uintptr_t __hwasan_shadow_memory_dynamic_address;

GOOGLE_MALLOC_SECTION_BEGIN
namespace tcmalloc::tcmalloc_internal::selsan {

// Implemented in tcmalloc.cc.
ABSL_ATTRIBUTE_WEAK std::pair<void*, size_t> HeapObjectInfo(void* ptr);

ABSL_CONST_INIT bool enabled = false;

namespace {

ABSL_CONST_INIT std::atomic<int> sampling_percent = 100;
ABSL_CONST_INIT Random rand{0};

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
    err(1, "tcmalloc: selsan: mmap(%p, 0x%zx) failed", kShadowStart,
        kShadowSize);
  }
  __hwasan_shadow_memory_dynamic_address = kShadowBase;
  madvise(kShadowStart, kShadowSize, MADV_DONTDUMP);
}

bool EnableTBI() {
#if defined(__x86_64__)
#ifdef TCMALLOC_INTERNAL_SELSAN_FAKE_MODE
  return true;
#else
#ifndef ARCH_ENABLE_TAGGED_ADDR
#define ARCH_ENABLE_TAGGED_ADDR 0x4002
#endif
  // Will fail if the CPU or kernel does not support Intel LAM.
  return TEMP_FAILURE_RETRY(syscall(SYS_arch_prctl, ARCH_ENABLE_TAGGED_ADDR,
                                    /*LAM_U57_BITS*/ 6)) == 0;
#endif
#elif defined(__aarch64__)
#ifndef PR_SET_TAGGED_ADDR_CTRL
#define PR_SET_TAGGED_ADDR_CTRL 55
#endif
#ifndef PR_TAGGED_ADDR_ENABLE
#define PR_TAGGED_ADDR_ENABLE (1UL << 0)
#endif
  TC_CHECK_EQ(0, prctl(PR_SET_TAGGED_ADDR_CTRL, PR_TAGGED_ADDR_ENABLE, 0, 0, 0),
              "errno=%d", errno);
  return true;
#else
  return false;
#endif
}

void Init() {
  MapShadow();
  if (HeapObjectInfo == nullptr) {
    return;  // don't have tcmalloc linked in
  }
  rand.Reset(getpid());
  enabled = EnableTBI();
}

void CheckAccess(uintptr_t p, size_t n, bool write) {
  // Not implemented yet.
}

void PrintTagMismatch(uintptr_t addr, size_t size, bool write) {
  uintptr_t ptr = addr & ((1ul << kTagShift) - 1);
  uintptr_t ptr_tag = addr >> kTagShift;
  uintptr_t mem_tag =
      *reinterpret_cast<uint8_t*>(kShadowBase + (ptr >> kShadowShift));
  fprintf(stderr,
          "WARNING: SelSan: %s tag-mismatch at addr %p ptr/mem tag:%zu/%zu "
          "size:%zu\n",
          write ? "write" : "read", reinterpret_cast<void*>(ptr), ptr_tag,
          mem_tag, size);
  if (HeapObjectInfo != nullptr) {
    auto [obj_start, obj_size] = HeapObjectInfo(reinterpret_cast<void*>(ptr));
    if (obj_start != nullptr) {
      fprintf(stderr, "Heap object %p-%p (size %zu, offset %zd)\n", obj_start,
              static_cast<char*>(obj_start) + obj_size, obj_size,
              ptr - reinterpret_cast<uintptr_t>(obj_start));
    }
  }
  RecordCrash("SelSan", "use-after-free or out-of-bounds access");
}

}  // namespace

int SamplingPercent() {
  return sampling_percent.load(std::memory_order_relaxed);
}

void SetSamplingPercent(int v) {
  sampling_percent.store(v, std::memory_order_relaxed);
}

bool ShouldSample() {
  const int percent = SamplingPercent();
  if (!enabled || percent <= 0) {
    return false;
  }
  if (percent >= 100) {
    return true;
  }
  return (rand.Next() % 100) < percent;
}

void PrintTextStats(Printer& out) {
  out.printf(R"(
------------------------------------------------
SelSan Status
------------------------------------------------
Enabled: %d
Sampling percent: %d%%

)",
             enabled, SamplingPercent());
}

void PrintPbtxtStats(PbtxtRegion& out) {
  auto selsan = out.CreateSubRegion("selsan");
  selsan.PrintRaw("status", enabled ? "SELSAN_ENABLED" : "SELSAN_DISABLED");
}

#ifdef __x86_64__
void SelsanTrapHandler(void* info, void* ctx) {
  // Tag mismatch is signalled using INT3 instruction + some NOP instructions
  // after it that encode access type/size. For the format and reference
  // implementation see:
  // https://github.com/llvm/llvm-project/blob/main/compiler-rt/lib/hwasan/hwasan_linux.cpp#L379-L396
  //
  // Note: we need to use process_vm_readv to read the code b/c we are not yet
  // sure this is hwasan trap. It may be some other INT3 instruction,
  // or an async SIGTRAP. Potentially RIP may be near page end and the next
  // page may be not mapped.
  const auto& mctx = (static_cast<ucontext_t*>(ctx))->uc_mcontext;
  unsigned char code[5];
  struct iovec local = {code, sizeof(code)};
  struct iovec remote = {reinterpret_cast<void*>(mctx.gregs[REG_RIP] - 1),
                         sizeof(code)};
  ssize_t n = process_vm_readv(getpid(), &local, 1, &remote, 1, 0);
  if (n != sizeof(code)) {
    return;
  }
  // Verify that we have INT3 (0xcc) + expected NOPs after it.
  if (code[0] != 0xcc || code[1] != 0x0f || code[2] != 0x1f ||
      code[3] != 0x40 || (code[4] & 0xc0) != 0x40 || (code[4] & 0xf) > 4) {
    return;
  }
  const bool write = (code[4] & 0x10) != 0;
  const size_t size = 1 << (code[4] & 0xf);
  const uintptr_t addr = mctx.gregs[REG_RDI];
  PrintTagMismatch(addr, size, write);
}
#endif  // #ifdef __x86_64__

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

void __hwasan_tag_memory(const volatile void* p, unsigned char tag,
                         size_t size) {
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

#ifdef __aarch64__
__attribute__((naked)) void __hwasan_tag_mismatch_v2() {
  // See the following links for the function interface:
  // https://github.com/llvm/llvm-project/blob/main/clang/docs/HardwareAssistedAddressSanitizerDesign.rst
  // https://github.com/llvm/llvm-project/blob/main/compiler-rt/lib/hwasan/hwasan_tag_mismatch_aarch64.S
  asm(
#ifdef __ARM_FEATURE_BTI_DEFAULT
      "hint 36"  // BTI_J
#endif
      "add x29, sp, #232"
#ifdef __GCC_HAVE_DWARF2_CFI_ASM
      R"(
        .cfi_def_cfa w29, 24
        .cfi_offset w30, -16
        .cfi_offset w29, -24
      )"
#endif
      R"(
        str x28, [sp, #224]
        stp x26, x27, [sp, #208]
        stp x24, x25, [sp, #192]
        stp x22, x23, [sp, #176]
        stp x20, x21, [sp, #160]
        stp x18, x19, [sp, #144]
        stp x16, x17, [sp, #128]
        stp x14, x15, [sp, #112]
        stp x12, x13, [sp, #96]
        stp x10, x11, [sp, #80]
        stp x8, x9, [sp, #64]
        stp x6, x7, [sp, #48]
        stp x4, x5, [sp, #32]
        stp x2, x3, [sp, #16]
        mov x2, sp
        bl TCMallocInternalTagMismatch
      )");
}

void TCMallocInternalTagMismatch(uintptr_t addr, uintptr_t type,
                                 uintptr_t* regs) {
  const size_t size = 1 << (type & 0xf);
  const bool write = (type & 0x10) != 0;
  PrintTagMismatch(addr, size, write);
  abort();
}
#endif  // #ifdef __aarch64__

void __hwasan_init() {}

}  // extern "C"

__attribute__((section(".preinit_array"), used)) static void (*init)() = Init;

}  // namespace tcmalloc::tcmalloc_internal::selsan
GOOGLE_MALLOC_SECTION_END
#endif  // #ifdef TCMALLOC_INTERNAL_SELSAN
