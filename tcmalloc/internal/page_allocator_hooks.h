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

#ifndef TCMALLOC_INTERNAL_PAGE_ALLOCATOR_HOOKS_H_
#define TCMALLOC_INTERNAL_PAGE_ALLOCATOR_HOOKS_H_

#include <cstddef>
#include <cstdint>

#include "tcmalloc/internal/config.h"
#include "tcmalloc/internal/hook_list.h"
#include "tcmalloc/internal/memory_tag.h"

GOOGLE_MALLOC_SECTION_BEGIN
namespace tcmalloc {
namespace tcmalloc_internal {

enum class PageReleaseReason : uint8_t;

// Synchronization contract:
// All PageAllocator hooks are guaranteed to be invoked without pageheap_lock
// held (ABSL_LOCKS_EXCLUDED(pageheap_lock)). Hook implementations are free to
// perform memory allocations, deallocations, or query allocator statistics, but
// must manage their own reentrancy safety if doing so.
//
// Hook invoked after Span allocation attempts in PageAllocator::New and
// PageAllocator::NewAligned.
// - start_page_index is 0 if allocation returned nullptr.
// - n is the requested size in pages.
// - align is the requested alignment in pages (1 for standard unaligned New,
//   or e.g. kPagesPerHugePage for NewAligned).
// - objects_per_span and density are from SpanAllocInfo.
//
using PageAllocatorNewHook = void (*)(size_t start_page_index, size_t n,
                                      size_t align, size_t objects_per_span,
                                      uint8_t density, MemoryTag tag);

// Hook invoked before a Span or allocation state is deleted in
// PageAllocator::Delete.
using PageAllocatorDeleteHook = void (*)(size_t start_page_index, size_t n,
                                         size_t objects_per_span,
                                         uint8_t density, MemoryTag tag);

// Hook invoked after memory release attempts in
// PageAllocator::ReleaseAtLeastNPages.
// - num_pages is the requested number of pages to release.
// - released is the actual number of pages released.
// - reason is the PageReleaseReason of the release attempt.
using PageAllocatorReleaseHook = void (*)(size_t num_pages, size_t released,
                                          PageReleaseReason reason);

extern HookList<PageAllocatorNewHook> page_allocator_new_hooks;
extern HookList<PageAllocatorDeleteHook> page_allocator_delete_hooks;
extern HookList<PageAllocatorReleaseHook> page_allocator_release_hooks;

}  // namespace tcmalloc_internal
}  // namespace tcmalloc
GOOGLE_MALLOC_SECTION_END

#endif  // TCMALLOC_INTERNAL_PAGE_ALLOCATOR_HOOKS_H_
