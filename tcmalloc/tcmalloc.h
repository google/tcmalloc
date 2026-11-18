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
// This is the exported interface from tcmalloc.  For most users,
// tcmalloc just overrides existing libc functionality, and thus this
// .h file isn't needed.  But we also provide the tcmalloc allocation
// routines through their own, dedicated name -- so people can wrap
// their own malloc functions around tcmalloc routines, perhaps.
// These are exported here.

#ifndef TCMALLOC_TCMALLOC_H_
#define TCMALLOC_TCMALLOC_H_

#include <malloc.h>
#include <stddef.h>
#include <stdio.h>

#include <new>

#include "absl/base/attributes.h"
#include "tcmalloc/alloc_at_least.h"
#include "tcmalloc/internal/config.h"
#include "tcmalloc/internal/declarations.h"  // IWYU pragma: keep
#include "tcmalloc/malloc_extension.h"

extern "C" {

[[maybe_unused]] [[nodiscard]] ABSL_ATTRIBUTE_SECTION(google_malloc)  //
    void* TCMallocInternalMalloc(size_t size) noexcept;
[[maybe_unused]] ABSL_ATTRIBUTE_SECTION(google_malloc)  //
    void TCMallocInternalFree(void* ptr) noexcept;
[[maybe_unused]] ABSL_ATTRIBUTE_SECTION(google_malloc)  //
    void TCMallocInternalFreeSized(void* ptr, size_t size) noexcept;
[[maybe_unused]] ABSL_ATTRIBUTE_SECTION(google_malloc)  //
    void TCMallocInternalFreeAlignedSized(void* ptr, size_t align,
                                          size_t size) noexcept;
[[maybe_unused]] ABSL_ATTRIBUTE_SECTION(google_malloc)  //
    void TCMallocInternalSdallocx(void* ptr, size_t size, int flags) noexcept;
[[maybe_unused]] [[nodiscard]] ABSL_ATTRIBUTE_SECTION(google_malloc)  //
    void* TCMallocInternalRealloc(void* ptr, size_t size) noexcept;
[[maybe_unused]] [[nodiscard]] ABSL_ATTRIBUTE_SECTION(google_malloc)  //
    void* TCMallocInternalReallocArray(void* ptr, size_t n,
                                       size_t size) noexcept;
[[maybe_unused]] [[nodiscard]] ABSL_ATTRIBUTE_SECTION(google_malloc)  //
    void* TCMallocInternalCalloc(size_t n, size_t size) noexcept;
[[maybe_unused]] ABSL_ATTRIBUTE_SECTION(google_malloc)  //
    void TCMallocInternalCfree(void* ptr) noexcept;

[[maybe_unused]] [[nodiscard]] ABSL_ATTRIBUTE_SECTION(google_malloc)  //
    void* TCMallocInternalAlignedAlloc(size_t align, size_t size) noexcept;
[[maybe_unused]] [[nodiscard]] ABSL_ATTRIBUTE_SECTION(google_malloc)  //
    void* TCMallocInternalMemalign(size_t align, size_t size) noexcept;
[[maybe_unused]] [[nodiscard]] ABSL_ATTRIBUTE_SECTION(google_malloc)  //
    int TCMallocInternalPosixMemalign(void** ptr, size_t align,
                                      size_t size) noexcept;
[[maybe_unused]] [[nodiscard]] ABSL_ATTRIBUTE_SECTION(google_malloc)  //
    void* TCMallocInternalValloc(size_t size) noexcept;
[[maybe_unused]] [[nodiscard]] ABSL_ATTRIBUTE_SECTION(google_malloc)  //
    void* TCMallocInternalPvalloc(size_t size) noexcept;

[[maybe_unused]] ABSL_ATTRIBUTE_SECTION(google_malloc)  //
    void TCMallocInternalMallocStats(void) noexcept;
[[maybe_unused]] ABSL_ATTRIBUTE_SECTION(google_malloc)  //
    int TCMallocInternalMallocTrim(size_t pad) noexcept;
[[maybe_unused]] ABSL_ATTRIBUTE_SECTION(google_malloc)  //
    int TCMallocInternalMallOpt(int cmd, int value) noexcept;
#if defined(TCMALLOC_HAVE_STRUCT_MALLINFO)
[[maybe_unused]] [[nodiscard]] ABSL_ATTRIBUTE_SECTION(google_malloc)  //
    struct mallinfo TCMallocInternalMallInfo(void) noexcept;
#endif
#if defined(TCMALLOC_HAVE_STRUCT_MALLINFO2)
[[maybe_unused]] [[nodiscard]] ABSL_ATTRIBUTE_SECTION(google_malloc)  //
    struct mallinfo2 TCMallocInternalMallInfo2(void) noexcept;
#endif
[[maybe_unused]] ABSL_ATTRIBUTE_SECTION(google_malloc)  //
    int TCMallocInternalMallocInfo(int opts, FILE* fp) noexcept;

[[maybe_unused]] [[nodiscard]] ABSL_ATTRIBUTE_SECTION(google_malloc)
    alloc_result_t TCMallocInternalAllocAtLeast(size_t min_size) noexcept;
[[maybe_unused]] [[nodiscard]] ABSL_ATTRIBUTE_SECTION(google_malloc)
    alloc_result_t
    TCMallocInternalAlignedAllocAtLeast(size_t alignment,
                                        size_t min_size) noexcept;

// This is an alias for MallocExtension::GetAllocatedSize().
// It is equivalent to
//    OS X: malloc_size()
//    glibc: malloc_usable_size()
//    Windows: _msize()
[[maybe_unused]] [[nodiscard]] ABSL_ATTRIBUTE_SECTION(google_malloc) size_t
    TCMallocInternalMallocSize(void* ptr) noexcept;

#ifdef __cplusplus
[[maybe_unused]] [[nodiscard]] ABSL_ATTRIBUTE_SECTION(google_malloc)  //
    void* TCMallocInternalNew(size_t size);
[[maybe_unused]] [[nodiscard]] ABSL_ATTRIBUTE_SECTION(google_malloc)  //
    void* TCMallocInternalNewAligned(size_t size, std::align_val_t alignment);
[[maybe_unused]] [[nodiscard]] ABSL_ATTRIBUTE_SECTION(google_malloc)  //
    void* TCMallocInternalNewNothrow(size_t size,
                                     const std::nothrow_t&) noexcept;
[[maybe_unused]] [[nodiscard]] ABSL_ATTRIBUTE_SECTION(google_malloc)  //
    void* TCMallocInternalNewAlignedNothrow(size_t size,
                                            std::align_val_t alignment,
                                            const std::nothrow_t&) noexcept;
[[maybe_unused]] [[nodiscard]] ABSL_ATTRIBUTE_SECTION(google_malloc)  //
    void* TCMallocInternalNewHotCold(size_t size,
                                     tcmalloc::hot_cold_t hot_cold);
[[maybe_unused]] [[nodiscard]] ABSL_ATTRIBUTE_SECTION(google_malloc)  //
    void* TCMallocInternalNewAlignedHotCold(size_t size,
                                            std::align_val_t alignment,
                                            tcmalloc::hot_cold_t hot_cold);
[[maybe_unused]] [[nodiscard]] ABSL_ATTRIBUTE_SECTION(google_malloc)  //
    void* TCMallocInternalNewHotColdNothrow(
        size_t size, const std::nothrow_t&,
        tcmalloc::hot_cold_t hot_cold) noexcept;
[[maybe_unused]] [[nodiscard]] ABSL_ATTRIBUTE_SECTION(google_malloc)  //
    void* TCMallocInternalNewAlignedHotColdNothrow(
        size_t size, std::align_val_t alignment, const std::nothrow_t&,
        tcmalloc::hot_cold_t hot_cold) noexcept;
[[maybe_unused]] ABSL_ATTRIBUTE_SECTION(google_malloc)  //
    void TCMallocInternalDelete(void* p) noexcept;
[[maybe_unused]] ABSL_ATTRIBUTE_SECTION(google_malloc)  //
    void TCMallocInternalDeleteAligned(void* p,
                                       std::align_val_t alignment) noexcept;
[[maybe_unused]] ABSL_ATTRIBUTE_SECTION(google_malloc)  //
    void TCMallocInternalDeleteSized(void* p, size_t size) noexcept;
[[maybe_unused]] ABSL_ATTRIBUTE_SECTION(google_malloc)  //
    void TCMallocInternalDeleteSizedAligned(
        void* p, size_t t, std::align_val_t alignment) noexcept;
[[maybe_unused]] ABSL_ATTRIBUTE_SECTION(google_malloc)  //
    void TCMallocInternalDeleteNothrow(void* p, const std::nothrow_t&) noexcept;
[[maybe_unused]] ABSL_ATTRIBUTE_SECTION(google_malloc)  //
    void TCMallocInternalDeleteAlignedNothrow(void* p,
                                              std::align_val_t alignment,
                                              const std::nothrow_t&) noexcept;
[[maybe_unused]] [[nodiscard]] ABSL_ATTRIBUTE_SECTION(google_malloc)  //
    void* TCMallocInternalNewArray(size_t size);
[[maybe_unused]] [[nodiscard]] ABSL_ATTRIBUTE_SECTION(google_malloc)  //
    void* TCMallocInternalNewArrayAligned(size_t size,
                                          std::align_val_t alignment);
[[maybe_unused]] [[nodiscard]] ABSL_ATTRIBUTE_SECTION(google_malloc)  //
    void* TCMallocInternalNewArrayNothrow(size_t size,
                                          const std::nothrow_t&) noexcept;
[[maybe_unused]] [[nodiscard]] ABSL_ATTRIBUTE_SECTION(google_malloc)  //
    void* TCMallocInternalNewArrayAlignedNothrow(
        size_t size, std::align_val_t alignment,
        const std::nothrow_t&) noexcept;

[[maybe_unused]] [[nodiscard]] ABSL_ATTRIBUTE_SECTION(google_malloc)  //
    void* TCMallocInternalNewArrayHotCold(size_t size,
                                          tcmalloc::hot_cold_t hot_cold);

[[maybe_unused]] [[nodiscard]] ABSL_ATTRIBUTE_SECTION(google_malloc)  //
    void* TCMallocInternalNewArrayAlignedHotCold(size_t size,
                                                 std::align_val_t alignment,
                                                 tcmalloc::hot_cold_t hot_cold);

[[maybe_unused]] [[nodiscard]] ABSL_ATTRIBUTE_SECTION(google_malloc)  //
    void* TCMallocInternalNewArrayHotColdNothrow(
        size_t size, const std::nothrow_t& nt,
        tcmalloc::hot_cold_t hot_cold) noexcept;

[[maybe_unused]] [[nodiscard]] ABSL_ATTRIBUTE_SECTION(google_malloc)  //
    void* TCMallocInternalNewArrayAlignedHotColdNothrow(
        size_t size, std::align_val_t alignment, const std::nothrow_t& nt,
        tcmalloc::hot_cold_t hot_cold) noexcept;

[[maybe_unused]] ABSL_ATTRIBUTE_SECTION(google_malloc)  //
    void TCMallocInternalDeleteArray(void* p) noexcept;
[[maybe_unused]] ABSL_ATTRIBUTE_SECTION(google_malloc)  //
    void TCMallocInternalDeleteArrayAligned(
        void* p, std::align_val_t alignment) noexcept;
[[maybe_unused]] ABSL_ATTRIBUTE_SECTION(google_malloc)  //
    void TCMallocInternalDeleteArraySized(void* p, size_t size) noexcept;
[[maybe_unused]] ABSL_ATTRIBUTE_SECTION(google_malloc)  //
    void TCMallocInternalDeleteArraySizedAligned(
        void* p, size_t t, std::align_val_t alignment) noexcept;
[[maybe_unused]] ABSL_ATTRIBUTE_SECTION(google_malloc)  //
    void TCMallocInternalDeleteArrayNothrow(void* p,
                                            const std::nothrow_t&) noexcept;
[[maybe_unused]] ABSL_ATTRIBUTE_SECTION(google_malloc)  //
    void TCMallocInternalDeleteArrayAlignedNothrow(
        void* p, std::align_val_t alignment, const std::nothrow_t&) noexcept;
[[maybe_unused]] [[nodiscard]] ABSL_ATTRIBUTE_SECTION(google_malloc)
    __sized_ptr_t TCMallocInternalSizeReturningNew(size_t size);
[[maybe_unused]] [[nodiscard]] ABSL_ATTRIBUTE_SECTION(google_malloc)
    __sized_ptr_t
    TCMallocInternalSizeReturningNewAligned(size_t size,
                                            std::align_val_t alignment);

#endif

//
// Partitioned Variants for Clang's -fsanitize=alloc-token.
//

#define DECLARE_ALLOC_TOKEN_NEW(id)                                       \
  void* TCMallocInternal_alloc_token_##id##__Znwm(size_t);                \
  void* TCMallocInternal_alloc_token_##id##__Znam(size_t);                \
  void* TCMallocInternal_alloc_token_##id##__ZnwmRKSt9nothrow_t(          \
      size_t size, const std::nothrow_t&) noexcept;                       \
  void* TCMallocInternal_alloc_token_##id##__ZnamRKSt9nothrow_t(          \
      size_t size, const std::nothrow_t&) noexcept;                       \
  void* TCMallocInternal_alloc_token_##id##__ZnwmSt11align_val_t(         \
      size_t, std::align_val_t);                                          \
  void* TCMallocInternal_alloc_token_##id##__ZnamSt11align_val_t(         \
      size_t, std::align_val_t);                                          \
  void*                                                                   \
  TCMallocInternal_alloc_token_##id##__ZnwmSt11align_val_tRKSt9nothrow_t( \
      size_t, std::align_val_t, const std::nothrow_t&) noexcept;          \
  void*                                                                   \
  TCMallocInternal_alloc_token_##id##__ZnamSt11align_val_tRKSt9nothrow_t( \
      size_t, std::align_val_t, const std::nothrow_t&) noexcept;

#define DECLARE_ALLOC_TOKEN_NEW_EXTENSION(id)                                           \
  void* TCMallocInternal_alloc_token_##id##__Znwm12__hot_cold_t(size_t,                 \
                                                                __hot_cold_t);          \
  void* TCMallocInternal_alloc_token_##id##__ZnwmRKSt9nothrow_t12__hot_cold_t(          \
      size_t, const std::nothrow_t&, __hot_cold_t) noexcept;                            \
  void*                                                                                 \
  TCMallocInternal_alloc_token_##id##__ZnwmSt11align_val_t12__hot_cold_t(               \
      size_t, std::align_val_t, __hot_cold_t);                                          \
  void*                                                                                 \
  TCMallocInternal_alloc_token_##id##__ZnwmSt11align_val_tRKSt9nothrow_t12__hot_cold_t( \
      size_t, std::align_val_t, const std::nothrow_t&, __hot_cold_t) noexcept;          \
  void* TCMallocInternal_alloc_token_##id##__Znam12__hot_cold_t(size_t,                 \
                                                                __hot_cold_t);          \
  void* TCMallocInternal_alloc_token_##id##__ZnamRKSt9nothrow_t12__hot_cold_t(          \
      size_t, const std::nothrow_t&, __hot_cold_t) noexcept;                            \
  void*                                                                                 \
  TCMallocInternal_alloc_token_##id##__ZnamSt11align_val_t12__hot_cold_t(               \
      size_t, std::align_val_t, __hot_cold_t);                                          \
  void*                                                                                 \
  TCMallocInternal_alloc_token_##id##__ZnamSt11align_val_tRKSt9nothrow_t12__hot_cold_t( \
      size_t, std::align_val_t, const std::nothrow_t&, __hot_cold_t) noexcept;          \
  __sized_ptr_t TCMallocInternal_alloc_token_##id##___size_returning_new(               \
      size_t);                                                                          \
  __sized_ptr_t                                                                         \
  TCMallocInternal_alloc_token_##id##___size_returning_new_aligned(                     \
      size_t, std::align_val_t);                                                        \
  __sized_ptr_t                                                                         \
  TCMallocInternal_alloc_token_##id##___size_returning_new_hot_cold(                    \
      size_t, __hot_cold_t);                                                            \
  __sized_ptr_t                                                                         \
  TCMallocInternal_alloc_token_##id##___size_returning_new_aligned_hot_cold(            \
      size_t, std::align_val_t, __hot_cold_t);

#define DECLARE_ALLOC_TOKEN_STDLIB(id)                                         \
  void* TCMallocInternal_alloc_token_##id##_malloc(size_t) noexcept;           \
  void* TCMallocInternal_alloc_token_##id##_realloc(void*, size_t) noexcept;   \
  void* TCMallocInternal_alloc_token_##id##_reallocarray(void*, size_t,        \
                                                         size_t) noexcept;     \
  void* TCMallocInternal_alloc_token_##id##_calloc(size_t, size_t) noexcept;   \
  void* TCMallocInternal_alloc_token_##id##_memalign(size_t, size_t) noexcept; \
  void* TCMallocInternal_alloc_token_##id##_aligned_alloc(size_t,              \
                                                          size_t) noexcept;    \
  void* TCMallocInternal_alloc_token_##id##_valloc(size_t) noexcept;           \
  void* TCMallocInternal_alloc_token_##id##_pvalloc(size_t) noexcept;          \
  int TCMallocInternal_alloc_token_##id##_posix_memalign(void**, size_t,       \
                                                         size_t) noexcept;

#define DECLARE_ALLOC_TOKEN_VARIANTS(id) \
  DECLARE_ALLOC_TOKEN_NEW(id)            \
  DECLARE_ALLOC_TOKEN_NEW_EXTENSION(id)  \
  DECLARE_ALLOC_TOKEN_STDLIB(id)

#ifdef __SANITIZE_ALLOC_TOKEN__
#ifndef ALLOC_TOKEN_MAX
#error "Define ALLOC_TOKEN_MAX to match -falloc-token-max=<max number of IDs>"
#endif
static_assert(ALLOC_TOKEN_MAX == 2);
#endif  // __SANITIZE_ALLOC_TOKEN__

DECLARE_ALLOC_TOKEN_VARIANTS(0)
DECLARE_ALLOC_TOKEN_VARIANTS(1)
#ifdef ALLOC_TOKEN_FALLBACK
// Define the functions for the fallback token ID if overridden with -mllvm
// -alloc-token-fallback=N; should fall outside the range of normal token IDs.
static_assert(ALLOC_TOKEN_FALLBACK >= ALLOC_TOKEN_MAX);
DECLARE_ALLOC_TOKEN_VARIANTS(ALLOC_TOKEN_FALLBACK)
#endif

[[nodiscard]] size_t TCMalloc_Internal_GetAllocatedSize(const void* ptr);
[[nodiscard]] size_t TCMalloc_Internal_GetEstimatedAllocatedSize(size_t size);
void TCMalloc_Internal_MarkThreadBusy();

}  // extern "C"

#endif  // TCMALLOC_TCMALLOC_H_
