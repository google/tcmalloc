// Copyright 2019 The TCMalloc Authors
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
//
// This .h file imports the code that causes tcmalloc to override libc
// versions of malloc/free/new/delete/etc.  That is, it provides the
// logic that makes it so calls to malloc(10) go through tcmalloc,
// rather than the default (libc) malloc.
//
// Every libc has its own way of doing this, and sometimes the compiler
// matters too, so we have a different file for each libc, and often
// for different compilers and OS's.

#ifndef TCMALLOC_LIBC_OVERRIDE_H_
#define TCMALLOC_LIBC_OVERRIDE_H_

#include <features.h>
#include <malloc.h>
#include <stddef.h>

#include <cstdio>
#include <cstdlib>
#include <new>

#include "tcmalloc/alloc_at_least.h"
#include "tcmalloc/malloc_extension.h"  // IWYU pragma: keep
#include "tcmalloc/tcmalloc.h"  // IWYU pragma: keep

#define TCMALLOC_ALIAS(tc_fn) \
  __attribute__((alias(#tc_fn), visibility("default")))

// NOLINTBEGIN(misc-definitions-in-headers)

#if defined(__GLIBC__)

#define TCMALLOC_NOTHROW noexcept

extern "C" {

void* __libc_malloc(size_t size) TCMALLOC_NOTHROW
    TCMALLOC_ALIAS(TCMallocInternalMalloc);
void __libc_free(void* ptr) TCMALLOC_NOTHROW
    TCMALLOC_ALIAS(TCMallocInternalFree);
void* __libc_realloc(void* ptr, size_t size) TCMALLOC_NOTHROW
    TCMALLOC_ALIAS(TCMallocInternalRealloc);
void* __libc_calloc(size_t n, size_t size) TCMALLOC_NOTHROW
    TCMALLOC_ALIAS(TCMallocInternalCalloc);
void __libc_cfree(void* ptr) TCMALLOC_NOTHROW
    TCMALLOC_ALIAS(TCMallocInternalCfree);
void* __libc_memalign(size_t align, size_t s) TCMALLOC_NOTHROW
    TCMALLOC_ALIAS(TCMallocInternalMemalign);
void* __libc_valloc(size_t size) TCMALLOC_NOTHROW
    TCMALLOC_ALIAS(TCMallocInternalValloc);
void* __libc_pvalloc(size_t size) TCMALLOC_NOTHROW
    TCMALLOC_ALIAS(TCMallocInternalPvalloc);
int __posix_memalign(void** r, size_t a, size_t s) TCMALLOC_NOTHROW
    TCMALLOC_ALIAS(TCMallocInternalPosixMemalign);

// We also have to hook libc malloc.  While our work with weak symbols
// should make sure libc malloc is never called in most situations, it
// can be worked around by shared libraries with the DEEPBIND
// environment variable set.  The below hooks libc to call our malloc
// routines even in that situation.  In other situations, this hook
// should never be called.

static void* glibc_override_malloc(size_t size, const void* caller) {
  return TCMallocInternalMalloc(size);
}
static void* glibc_override_realloc(void* ptr, size_t size,
                                    const void* caller) {
  return TCMallocInternalRealloc(ptr, size);
}
static void glibc_override_free(void* ptr, const void* caller) {
  TCMallocInternalFree(ptr);
}
static void* glibc_override_memalign(size_t align, size_t size,
                                     const void* caller) {
  return TCMallocInternalMemalign(align, size);
}

// We should be using __malloc_initialize_hook here.  (See
// http://swoolley.org/man.cgi/3/malloc_hook.)  However, this causes weird
// linker errors with programs that link with -static, so instead we just assign
// the vars directly at static-constructor time.  That should serve the same
// effect of making sure the hooks are set before the first malloc call the
// program makes.

// Glibc-2.14 and above make __malloc_hook and friends volatile
#ifndef __MALLOC_HOOK_VOLATILE
#define __MALLOC_HOOK_VOLATILE /**/
#endif

void* (*__MALLOC_HOOK_VOLATILE __malloc_hook)(size_t, const void*) =
    &glibc_override_malloc;
void* (*__MALLOC_HOOK_VOLATILE __realloc_hook)(void*, size_t, const void*) =
    &glibc_override_realloc;
void (*__MALLOC_HOOK_VOLATILE __free_hook)(void*,
                                           const void*) = &glibc_override_free;
void* (*__MALLOC_HOOK_VOLATILE __memalign_hook)(size_t, size_t, const void*) =
    &glibc_override_memalign;

}  // extern "C"

#else

#define TCMALLOC_NOTHROW

#endif  // defined(__GLIBC__)

void* operator new(size_t size) noexcept(false)
    TCMALLOC_ALIAS(TCMallocInternalNew);
void operator delete(void* p) noexcept TCMALLOC_ALIAS(TCMallocInternalDelete);
void operator delete(void* p, size_t size) noexcept
    TCMALLOC_ALIAS(TCMallocInternalDeleteSized);
void* operator new[](size_t size) noexcept(false)
    TCMALLOC_ALIAS(TCMallocInternalNewArray);
void operator delete[](void* p) noexcept
    TCMALLOC_ALIAS(TCMallocInternalDeleteArray);
void operator delete[](void* p, size_t size) noexcept
    TCMALLOC_ALIAS(TCMallocInternalDeleteArraySized);
void* operator new(size_t size, const std::nothrow_t& nt) noexcept
    TCMALLOC_ALIAS(TCMallocInternalNewNothrow);
void* operator new[](size_t size, const std::nothrow_t& nt) noexcept
    TCMALLOC_ALIAS(TCMallocInternalNewArrayNothrow);
void operator delete(void* p, const std::nothrow_t& nt) noexcept
    TCMALLOC_ALIAS(TCMallocInternalDeleteNothrow);
void operator delete[](void* p, const std::nothrow_t& nt) noexcept
    TCMALLOC_ALIAS(TCMallocInternalDeleteArrayNothrow);

void* operator new(size_t size, std::align_val_t alignment) noexcept(false)
    TCMALLOC_ALIAS(TCMallocInternalNewAligned);
void* operator new(size_t size, std::align_val_t alignment,
                   const std::nothrow_t&) noexcept
    TCMALLOC_ALIAS(TCMallocInternalNewAlignedNothrow);
void operator delete(void* p, std::align_val_t alignment) noexcept
    TCMALLOC_ALIAS(TCMallocInternalDeleteAligned);
void operator delete(void* p, std::align_val_t alignment,
                     const std::nothrow_t&) noexcept
    TCMALLOC_ALIAS(TCMallocInternalDeleteAlignedNothrow);
void operator delete(void* p, size_t size, std::align_val_t alignment) noexcept
    TCMALLOC_ALIAS(TCMallocInternalDeleteSizedAligned);
void* operator new[](size_t size, std::align_val_t alignment) noexcept(false)
    TCMALLOC_ALIAS(TCMallocInternalNewArrayAligned);
void* operator new[](size_t size, std::align_val_t alignment,
                     const std::nothrow_t&) noexcept
    TCMALLOC_ALIAS(TCMallocInternalNewArrayAlignedNothrow);
void operator delete[](void* p, std::align_val_t alignment) noexcept
    TCMALLOC_ALIAS(TCMallocInternalDeleteArrayAligned);
void operator delete[](void* p, std::align_val_t alignment,
                       const std::nothrow_t&) noexcept
    TCMALLOC_ALIAS(TCMallocInternalDeleteArrayAlignedNothrow);
void operator delete[](void* p, size_t size,
                       std::align_val_t alignment) noexcept
    TCMALLOC_ALIAS(TCMallocInternalDeleteArraySizedAligned);

extern "C" {

void* malloc(size_t size) TCMALLOC_NOTHROW
    TCMALLOC_ALIAS(TCMallocInternalMalloc);
void free(void* ptr) TCMALLOC_NOTHROW TCMALLOC_ALIAS(TCMallocInternalFree);
void free_sized(void* ptr, size_t size) TCMALLOC_NOTHROW
    TCMALLOC_ALIAS(TCMallocInternalFreeSized);
void free_aligned_sized(void* ptr, size_t align, size_t size) TCMALLOC_NOTHROW
    TCMALLOC_ALIAS(TCMallocInternalFreeAlignedSized);
void sdallocx(void* ptr, size_t size, int flags) noexcept
    TCMALLOC_ALIAS(TCMallocInternalSdallocx);
void* realloc(void* ptr, size_t size) TCMALLOC_NOTHROW
    TCMALLOC_ALIAS(TCMallocInternalRealloc);
void* reallocarray(void* ptr, size_t n, size_t size) TCMALLOC_NOTHROW
    TCMALLOC_ALIAS(TCMallocInternalReallocArray);
void* calloc(size_t n, size_t size) TCMALLOC_NOTHROW
    TCMALLOC_ALIAS(TCMallocInternalCalloc);
void cfree(void* ptr) TCMALLOC_NOTHROW TCMALLOC_ALIAS(TCMallocInternalCfree);
void* memalign(size_t align, size_t s) TCMALLOC_NOTHROW
    TCMALLOC_ALIAS(TCMallocInternalMemalign);
void* aligned_alloc(size_t align, size_t s) TCMALLOC_NOTHROW
    TCMALLOC_ALIAS(TCMallocInternalAlignedAlloc);
void* valloc(size_t size) TCMALLOC_NOTHROW
    TCMALLOC_ALIAS(TCMallocInternalValloc);
void* pvalloc(size_t size) TCMALLOC_NOTHROW
    TCMALLOC_ALIAS(TCMallocInternalPvalloc);
int posix_memalign(void** r, size_t a, size_t s) TCMALLOC_NOTHROW
    TCMALLOC_ALIAS(TCMallocInternalPosixMemalign);
void malloc_stats(void) TCMALLOC_NOTHROW
    TCMALLOC_ALIAS(TCMallocInternalMallocStats);
int malloc_trim(size_t pad) TCMALLOC_NOTHROW
    TCMALLOC_ALIAS(TCMallocInternalMallocTrim);
int mallopt(int cmd, int value) TCMALLOC_NOTHROW
    TCMALLOC_ALIAS(TCMallocInternalMallOpt);
#ifdef TCMALLOC_HAVE_STRUCT_MALLINFO
struct mallinfo mallinfo(void) TCMALLOC_NOTHROW
    TCMALLOC_ALIAS(TCMallocInternalMallInfo);
#endif
#ifdef TCMALLOC_HAVE_STRUCT_MALLINFO2
struct mallinfo2 mallinfo2(void) TCMALLOC_NOTHROW
    TCMALLOC_ALIAS(TCMallocInternalMallInfo2);
#endif
int malloc_info(int opts, FILE* fp) TCMALLOC_NOTHROW
    TCMALLOC_ALIAS(TCMallocInternalMallocInfo);
size_t malloc_size(void* p) TCMALLOC_NOTHROW
    TCMALLOC_ALIAS(TCMallocInternalMallocSize);
size_t malloc_usable_size(void* p) TCMALLOC_NOTHROW
    TCMALLOC_ALIAS(TCMallocInternalMallocSize);

alloc_result_t alloc_at_least(size_t min_size) TCMALLOC_NOTHROW
    TCMALLOC_ALIAS(TCMallocInternalAllocAtLeast);
alloc_result_t aligned_alloc_at_least(size_t alignment,
                                      size_t min_size) TCMALLOC_NOTHROW
    TCMALLOC_ALIAS(TCMallocInternalAlignedAllocAtLeast);

//
// Partitioned Variants for Clang's -fsanitize=alloc-token.
//
#ifdef __SANITIZE_ALLOC_TOKEN__
#define DEFINE_ALLOC_TOKEN_NEW(id)                                          \
  void* __alloc_token_##id##__Znwm(size_t)                                  \
      TCMALLOC_ALIAS(TCMallocInternalNew);                                  \
  void* __alloc_token_##id##__Znam(size_t)                                  \
      TCMALLOC_ALIAS(TCMallocInternalNewArray);                             \
  void* __alloc_token_##id##__ZnwmRKSt9nothrow_t(size_t,                    \
                                                 const std::nothrow_t&)     \
      TCMALLOC_ALIAS(TCMallocInternalNewNothrow);                           \
  void* __alloc_token_##id##__ZnamRKSt9nothrow_t(size_t,                    \
                                                 const std::nothrow_t&)     \
      TCMALLOC_ALIAS(TCMallocInternalNewArrayNothrow);                      \
  void* __alloc_token_##id##__ZnwmSt11align_val_t(size_t, std::align_val_t) \
      TCMALLOC_ALIAS(TCMallocInternalNewAligned);                           \
  void* __alloc_token_##id##__ZnamSt11align_val_t(size_t, std::align_val_t) \
      TCMALLOC_ALIAS(TCMallocInternalNewArrayAligned);                      \
  void* __alloc_token_##id##__ZnwmSt11align_val_tRKSt9nothrow_t(            \
      size_t, std::align_val_t, const std::nothrow_t&)                      \
      TCMALLOC_ALIAS(TCMallocInternalNewAlignedNothrow);                    \
  void* __alloc_token_##id##__ZnamSt11align_val_tRKSt9nothrow_t(            \
      size_t, std::align_val_t, const std::nothrow_t&)                      \
      TCMALLOC_ALIAS(TCMallocInternalNewArrayAlignedNothrow);

#ifndef TCMALLOC_INTERNAL_METHODS_ONLY
#define DEFINE_ALLOC_TOKEN_NEW_EXTENSION(id)                                                   \
  void* __alloc_token_##id##__Znwm12__hot_cold_t(size_t, __hot_cold_t)                         \
      TCMALLOC_ALIAS(_Znwm12__hot_cold_t);                                                     \
  void* __alloc_token_##id##__ZnwmRKSt9nothrow_t12__hot_cold_t(                                \
      size_t, const std::nothrow_t&,                                                           \
      __hot_cold_t) noexcept TCMALLOC_ALIAS(_ZnwmRKSt9nothrow_t12__hot_cold_t);                \
  void* __alloc_token_##id##__ZnwmSt11align_val_t12__hot_cold_t(                               \
      size_t, std::align_val_t, __hot_cold_t)                                                  \
      TCMALLOC_ALIAS(_ZnwmSt11align_val_t12__hot_cold_t);                                      \
  void* __alloc_token_##id##__ZnwmSt11align_val_tRKSt9nothrow_t12__hot_cold_t(                 \
      size_t, std::align_val_t, const std::nothrow_t&,                                         \
      __hot_cold_t) noexcept TCMALLOC_ALIAS(_ZnwmSt11align_val_tRKSt9nothrow_t12__hot_cold_t); \
  void* __alloc_token_##id##__Znam12__hot_cold_t(size_t, __hot_cold_t)                         \
      TCMALLOC_ALIAS(_Znam12__hot_cold_t);                                                     \
  void* __alloc_token_##id##__ZnamRKSt9nothrow_t12__hot_cold_t(                                \
      size_t, const std::nothrow_t&,                                                           \
      __hot_cold_t) noexcept TCMALLOC_ALIAS(_ZnamRKSt9nothrow_t12__hot_cold_t);                \
  void* __alloc_token_##id##__ZnamSt11align_val_t12__hot_cold_t(                               \
      size_t, std::align_val_t, __hot_cold_t)                                                  \
      TCMALLOC_ALIAS(_ZnamSt11align_val_t12__hot_cold_t);                                      \
  void* __alloc_token_##id##__ZnamSt11align_val_tRKSt9nothrow_t12__hot_cold_t(                 \
      size_t, std::align_val_t, const std::nothrow_t&,                                         \
      __hot_cold_t) noexcept TCMALLOC_ALIAS(_ZnamSt11align_val_tRKSt9nothrow_t12__hot_cold_t); \
  __sized_ptr_t __alloc_token_##id##___size_returning_new(size_t)                              \
      TCMALLOC_ALIAS(__size_returning_new);                                                    \
  __sized_ptr_t __alloc_token_##id##___size_returning_new_aligned(                             \
      size_t, std::align_val_t) TCMALLOC_ALIAS(__size_returning_new_aligned);                  \
  __sized_ptr_t __alloc_token_##id##___size_returning_new_hot_cold(                            \
      size_t, __hot_cold_t) TCMALLOC_ALIAS(__size_returning_new_hot_cold);                     \
  __sized_ptr_t __alloc_token_##id##___size_returning_new_aligned_hot_cold(                    \
      size_t, std::align_val_t, __hot_cold_t)                                                  \
      TCMALLOC_ALIAS(__size_returning_new_aligned_hot_cold);
#else
#define ALLOC_TOKEN_NEW_EXTENSION(id)
#endif  // TCMALLOC_INTERNAL_METHODS_ONLY

#define DEFINE_ALLOC_TOKEN_STDLIB(id)                                \
  void* __alloc_token_##id##_malloc(size_t)                          \
      TCMALLOC_NOTHROW TCMALLOC_ALIAS(TCMallocInternalMalloc);       \
  void* __alloc_token_##id##_realloc(void*, size_t)                  \
      TCMALLOC_NOTHROW TCMALLOC_ALIAS(TCMallocInternalRealloc);      \
  void* __alloc_token_##id##_reallocarray(void*, size_t, size_t)     \
      TCMALLOC_NOTHROW TCMALLOC_ALIAS(TCMallocInternalReallocArray); \
  void* __alloc_token_##id##_calloc(size_t, size_t)                  \
      TCMALLOC_NOTHROW TCMALLOC_ALIAS(TCMallocInternalCalloc);       \
  void* __alloc_token_##id##_memalign(size_t, size_t)                \
      TCMALLOC_NOTHROW TCMALLOC_ALIAS(TCMallocInternalMemalign);     \
  void* __alloc_token_##id##_aligned_alloc(size_t, size_t)           \
      TCMALLOC_NOTHROW TCMALLOC_ALIAS(TCMallocInternalAlignedAlloc); \
  void* __alloc_token_##id##_valloc(size_t)                          \
      TCMALLOC_NOTHROW TCMALLOC_ALIAS(TCMallocInternalValloc);       \
  void* __alloc_token_##id##_pvalloc(size_t)                         \
      TCMALLOC_NOTHROW TCMALLOC_ALIAS(TCMallocInternalPvalloc);      \
  int __alloc_token_##id##_posix_memalign(void**, size_t, size_t)    \
      TCMALLOC_NOTHROW TCMALLOC_ALIAS(TCMallocInternalPosixMemalign);

#define DEFINE_ALLOC_TOKEN_VARIANTS(id) \
  DEFINE_ALLOC_TOKEN_NEW(id)            \
  DEFINE_ALLOC_TOKEN_NEW_EXTENSION(id)  \
  DEFINE_ALLOC_TOKEN_STDLIB(id)

#ifndef ALLOC_TOKEN_MAX
#error "Define ALLOC_TOKEN_MAX to match -falloc-token-max=<max number of IDs>"
#endif
static_assert(ALLOC_TOKEN_MAX == 2);
DEFINE_ALLOC_TOKEN_VARIANTS(0)
DEFINE_ALLOC_TOKEN_VARIANTS(1)
#ifdef ALLOC_TOKEN_FALLBACK
// Define the functions for the fallback token ID if overridden with -mllvm
// -alloc-token-fallback=N; should fall outside the range of normal token IDs.
static_assert(ALLOC_TOKEN_FALLBACK >= ALLOC_TOKEN_MAX);
DEFINE_ALLOC_TOKEN_VARIANTS(ALLOC_TOKEN_FALLBACK)
#endif

#endif  // __SANITIZE_ALLOC_TOKEN__

}  // extern "C"

// NOLINTEND(misc-definitions-in-headers)

#endif  // TCMALLOC_LIBC_OVERRIDE_H_
