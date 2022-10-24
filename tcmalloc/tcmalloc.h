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

#include "absl/base/attributes.h"
#include "tcmalloc/internal/config.h"
#include "tcmalloc/internal/declarations.h"  // IWYU pragma: keep

extern "C" {

void* TCMallocInternalMalloc(size_t size) noexcept
    ABSL_ATTRIBUTE_SECTION(google_malloc);
void TCMallocInternalFree(void* ptr) noexcept
    ABSL_ATTRIBUTE_SECTION(google_malloc);
void TCMallocInternalFreeSized(void* ptr, size_t size) noexcept
    ABSL_ATTRIBUTE_SECTION(google_malloc);
void TCMallocInternalFreeAlignedSized(void* ptr, size_t align,
                                      size_t size) noexcept
    ABSL_ATTRIBUTE_SECTION(google_malloc);
void TCMallocInternalSdallocx(void* ptr, size_t size, int flags) noexcept
    ABSL_ATTRIBUTE_SECTION(google_malloc);
void* TCMallocInternalRealloc(void* ptr, size_t size) noexcept
    ABSL_ATTRIBUTE_SECTION(google_malloc);
void* TCMallocInternalCalloc(size_t n, size_t size) noexcept
    ABSL_ATTRIBUTE_SECTION(google_malloc);
void TCMallocInternalCfree(void* ptr) noexcept
    ABSL_ATTRIBUTE_SECTION(google_malloc);

void* TCMallocInternalAlignedAlloc(size_t align, size_t size) noexcept
    ABSL_ATTRIBUTE_SECTION(google_malloc);
void* TCMallocInternalMemalign(size_t align, size_t size) noexcept
    ABSL_ATTRIBUTE_SECTION(google_malloc);
int TCMallocInternalPosixMemalign(void** ptr, size_t align,
                                  size_t size) noexcept
    ABSL_ATTRIBUTE_SECTION(google_malloc);
void* TCMallocInternalValloc(size_t size) noexcept
    ABSL_ATTRIBUTE_SECTION(google_malloc);
void* TCMallocInternalPvalloc(size_t size) noexcept
    ABSL_ATTRIBUTE_SECTION(google_malloc);

void TCMallocInternalMallocStats(void) noexcept
    ABSL_ATTRIBUTE_SECTION(google_malloc);
#if defined(TCMALLOC_HAVE_MALLOC_TRIM)
int TCMallocInternalMallocTrim(size_t pad) noexcept
    ABSL_ATTRIBUTE_SECTION(google_malloc);
#endif
int TCMallocInternalMallOpt(int cmd, int value) noexcept
    ABSL_ATTRIBUTE_SECTION(google_malloc);
#if defined(TCMALLOC_HAVE_STRUCT_MALLINFO)
struct mallinfo TCMallocInternalMallInfo(void) noexcept
    ABSL_ATTRIBUTE_SECTION(google_malloc);
#endif
#if defined(TCMALLOC_HAVE_STRUCT_MALLINFO2)
struct mallinfo2 TCMallocInternalMallInfo2(void) noexcept
    ABSL_ATTRIBUTE_SECTION(google_malloc);
#endif
int TCMallocInternalMallocInfo(int opts, FILE* fp) noexcept
    ABSL_ATTRIBUTE_SECTION(google_malloc);

// This is an alias for MallocExtension::GetAllocatedSize().
// It is equivalent to
//    OS X: malloc_size()
//    glibc: malloc_usable_size()
//    Windows: _msize()
size_t TCMallocInternalMallocSize(void* ptr) noexcept
    ABSL_ATTRIBUTE_SECTION(google_malloc);

#ifdef __cplusplus
void* TCMallocInternalNew(size_t size) ABSL_ATTRIBUTE_SECTION(google_malloc);
void* TCMallocInternalNewAligned(size_t size, std::align_val_t alignment)
    ABSL_ATTRIBUTE_SECTION(google_malloc);
void* TCMallocInternalNewNothrow(size_t size, const std::nothrow_t&) noexcept
    ABSL_ATTRIBUTE_SECTION(google_malloc);
void* TCMallocInternalNewAlignedNothrow(size_t size, std::align_val_t alignment,
                                        const std::nothrow_t&) noexcept
    ABSL_ATTRIBUTE_SECTION(google_malloc);
void TCMallocInternalDelete(void* p) noexcept
    ABSL_ATTRIBUTE_SECTION(google_malloc);
void TCMallocInternalDeleteAligned(void* p, std::align_val_t alignment) noexcept
    ABSL_ATTRIBUTE_SECTION(google_malloc);
void TCMallocInternalDeleteSized(void* p, size_t size) noexcept
    ABSL_ATTRIBUTE_SECTION(google_malloc);
void TCMallocInternalDeleteSizedAligned(void* p, size_t t,
                                        std::align_val_t alignment) noexcept
    ABSL_ATTRIBUTE_SECTION(google_malloc);
void TCMallocInternalDeleteNothrow(void* p, const std::nothrow_t&) noexcept
    ABSL_ATTRIBUTE_SECTION(google_malloc);
void TCMallocInternalDeleteAlignedNothrow(void* p, std::align_val_t alignment,
                                          const std::nothrow_t&) noexcept
    ABSL_ATTRIBUTE_SECTION(google_malloc);
void* TCMallocInternalNewArray(size_t size)
    ABSL_ATTRIBUTE_SECTION(google_malloc);
void* TCMallocInternalNewArrayAligned(size_t size, std::align_val_t alignment)
    ABSL_ATTRIBUTE_SECTION(google_malloc);
void* TCMallocInternalNewArrayNothrow(size_t size,
                                      const std::nothrow_t&) noexcept
    ABSL_ATTRIBUTE_SECTION(google_malloc);
void* TCMallocInternalNewArrayAlignedNothrow(size_t size,
                                             std::align_val_t alignment,
                                             const std::nothrow_t&) noexcept
    ABSL_ATTRIBUTE_SECTION(google_malloc);
void TCMallocInternalDeleteArray(void* p) noexcept
    ABSL_ATTRIBUTE_SECTION(google_malloc);
void TCMallocInternalDeleteArrayAligned(void* p,
                                        std::align_val_t alignment) noexcept
    ABSL_ATTRIBUTE_SECTION(google_malloc);
void TCMallocInternalDeleteArraySized(void* p, size_t size) noexcept
    ABSL_ATTRIBUTE_SECTION(google_malloc);
void TCMallocInternalDeleteArraySizedAligned(
    void* p, size_t t, std::align_val_t alignment) noexcept
    ABSL_ATTRIBUTE_SECTION(google_malloc);
void TCMallocInternalDeleteArrayNothrow(void* p, const std::nothrow_t&) noexcept
    ABSL_ATTRIBUTE_SECTION(google_malloc);
void TCMallocInternalDeleteArrayAlignedNothrow(void* p,
                                               std::align_val_t alignment,
                                               const std::nothrow_t&) noexcept
    ABSL_ATTRIBUTE_SECTION(google_malloc);
#endif

}  // extern "C"

#endif  // TCMALLOC_TCMALLOC_H_
