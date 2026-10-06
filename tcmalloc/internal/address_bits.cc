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

#include "tcmalloc/internal/address_bits.h"

#include <cstdint>

GOOGLE_MALLOC_SECTION_BEGIN
namespace tcmalloc {
namespace tcmalloc_internal {

namespace {

// Returns the 1-based position of the most significant set bit in the
// current stack pointer.  Linux maps the main stack just below TASK_SIZE,
// the top of the user address space, so (modulo ASLR within the top
// region) this is the kernel's user virtual-address width.
int ProbeStackAddressMsb() {
  // A plain local: taking its address forces a stack slot; the value itself
  // is never read.
  int stack_marker;
  uintptr_t sp = reinterpret_cast<uintptr_t>(&stack_marker);
  if (sp == 0) {
    // Impossible on Linux; fall back to the compile-time maximum rather
    // than shifting by a bogus amount.
    return 8 * static_cast<int>(sizeof(void*));
  }
  // __builtin_clzll is undefined for 0, excluded above.
  return 64 - __builtin_clzll(sp);
}

}  // namespace

int EffectiveAddressBits() {
#if defined(__aarch64__) && defined(__linux__)
  // Cached: the address-space width is fixed for the life of the process.
  // Function-local statics are initialized exactly once, thread-safely.
  static const int kEffectiveBits = [] {
    int msb = ProbeStackAddressMsb();
    // aarch64 Linux kernels always provide at least a 39-bit user address
    // space (4K pages, VA_BITS=39 is the architectural minimum).
    if (msb < 39) msb = 39;
    // The allocator's data structures are sized for kAddressBits; the
    // effective width can never exceed it.
    if (msb > kAddressBits) msb = kAddressBits;
    return msb;
  }();
  return kEffectiveBits;
#else
  return kAddressBits;
#endif
}

}  // namespace tcmalloc_internal
}  // namespace tcmalloc
GOOGLE_MALLOC_SECTION_END
