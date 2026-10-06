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

#ifndef TCMALLOC_INTERNAL_ADDRESS_BITS_H_
#define TCMALLOC_INTERNAL_ADDRESS_BITS_H_

#include "tcmalloc/internal/config.h"

GOOGLE_MALLOC_SECTION_BEGIN
namespace tcmalloc {
namespace tcmalloc_internal {

// Effective user virtual-address width, in bits.
//
// This is the width of user addresses the running kernel actually provides.
// kAddressBits (tcmalloc/internal/config.h) remains the *compile-time
// maximum* that the allocator's data structures (page map, span encodings,
// statistics) are sized for.
//
// On Linux/aarch64, production kernels are commonly built with a narrower
// user address space than the 48-bit architectural maximum (e.g. Raspberry
// Pi OS ships CONFIG_ARM64_VA_BITS=39, Android GKI likewise).  A binary that
// assumes 48 bits generates mmap hints the kernel rejects, and fails at
// startup (see https://github.com/google/tcmalloc/issues/82).  To keep one
// binary working across kernels, the width is detected at runtime from the
// address of the current stack: the kernel places the stack just below the
// top of the user address space, so the most significant set bit of a stack
// address reveals the width.  This is the same probe sanitizer runtimes
// (e.g. TSan's VMA size detection) use; it needs no syscalls and parses no
// /proc files.
//
// The detected value is clamped to [39, kAddressBits]: aarch64 Linux always
// provides at least a 39-bit user address space, and the allocator can never
// use more than its compile-time maximum.  On a 48-bit kernel the result is
// 48 and every downstream computation is bit-identical to before.
//
// On all other platforms EffectiveAddressBits() == kAddressBits.
//
// The result is cached after the first call; the address-space width cannot
// change during the lifetime of a process.  Detection itself performs no
// allocation and takes no locks, so it is safe to call during early
// allocator initialization.
int EffectiveAddressBits();

}  // namespace tcmalloc_internal
}  // namespace tcmalloc
GOOGLE_MALLOC_SECTION_END

#endif  // TCMALLOC_INTERNAL_ADDRESS_BITS_H_
