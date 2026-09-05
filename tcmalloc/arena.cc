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

#include "tcmalloc/arena.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <new>

#include "absl/base/optimization.h"
#include "tcmalloc/common.h"
#include "tcmalloc/internal/allocation_guard.h"
#include "tcmalloc/internal/config.h"
#include "tcmalloc/internal/logging.h"
#include "tcmalloc/internal/memory_tag.h"
#include "tcmalloc/internal/system_allocator.h"
#include "tcmalloc/parameters.h"
#include "tcmalloc/span.h"
#include "tcmalloc/static_vars.h"

GOOGLE_MALLOC_SECTION_BEGIN
namespace tcmalloc {
namespace tcmalloc_internal {

namespace {
inline size_t AlignmentBytes(char* area, size_t align) {
  const size_t misalignment = reinterpret_cast<uintptr_t>(area) % align;
  return misalignment != 0 ? align - misalignment : 0;
}
}  // namespace

void Arena::StashRemainingFreeArea() {
  size_t block_align_bytes = AlignmentBytes(free_area_, alignof(Block));
  // If the remaining free area is large enough to hold a block AND allocate a
  // span (smallest allocation served via arena), add it to the freelist.
  // Otherwise, mark it as unavailable and drop it.
  if (freelist_blocks_ < kMaxFreelistBlocks &&
      free_avail_ >=
          block_align_bytes + std::max(sizeof(Span), sizeof(Block))) {
    bytes_unavailable_ += block_align_bytes;
    Block* b = reinterpret_cast<Block*>(free_area_ + block_align_bytes);
    b->next = freelist_;
    b->size = free_avail_ - block_align_bytes;
    freelist_ = b;
    freelist_blocks_++;
    freelist_bytes_unallocated_ += b->size;
  } else {
    bytes_unavailable_ += free_avail_;
  }
  free_area_ = nullptr;
  free_avail_ = 0;
}

Arena::Block* Arena::TryPopFromFreelist(size_t bytes, size_t align) {
  Block* cur = freelist_;
  Block* prev = nullptr;
  while (cur != nullptr) {
    char* area = reinterpret_cast<char*>(cur);
    size_t avail = cur->size;
    size_t align_bytes = AlignmentBytes(area, align);
    if (avail < align_bytes + bytes) {
      prev = cur;
      cur = cur->next;
      continue;
    }
    // Found a block on the freelist that can satisfy the request. Remove it
    // from the freelist and return it.
    if (prev == nullptr) {
      freelist_ = cur->next;
    } else {
      prev->next = cur->next;
    }
    freelist_blocks_--;
    freelist_bytes_unallocated_ -= cur->size;
    return cur;
  }
  return nullptr;
}

void Arena::AllocSlow(size_t bytes, size_t align) {
  // Make a new block on the freelist from any remaining free area.
  StashRemainingFreeArea();

  // Try to use a block from the freelist.
  if (Block* b = TryPopFromFreelist(bytes, align); b != nullptr) {
    free_area_ = reinterpret_cast<char*>(b);
    free_avail_ = b->size;
    return;
  }

  // If no block on the freelist can satisfy the request, allocate a new block
  // from the system allocator.
  auto& system_allocator = tc_globals.system_allocator();
  size_t ask = bytes > kAllocIncrement ? bytes : kAllocIncrement;
  auto [ptr, actual_size] = system_allocator.Allocate(
      ask, std::max(kPageSize, align), MemoryTag::kMetadata);
  free_area_ = reinterpret_cast<char*>(ptr);
  if (ABSL_PREDICT_FALSE(free_area_ == nullptr)) {
    TC_BUG(
        "FATAL ERROR: Out of memory trying to allocate internal tcmalloc "
        "data (bytes=%v, object-size=%v); is something preventing mmap from "
        "succeeding (sandbox, VSS limitations)?",
        kAllocIncrement, bytes);
  }

  if (Parameters::back_small_allocations() &&
      actual_size <= Parameters::back_size_threshold_bytes()) {
    system_allocator.Back(free_area_, actual_size);
  }

  blocks_++;
  free_avail_ = actual_size;
}

void* Arena::Alloc(size_t bytes, std::align_val_t alignment) {
  size_t align = static_cast<size_t>(alignment);
  TC_ASSERT_GT(align, 0);

  AllocationGuardSpinLockHolder l(arena_lock_);

  size_t alignment_bytes = AlignmentBytes(free_area_, align);
  if (ABSL_PREDICT_FALSE(free_avail_ < alignment_bytes + bytes)) {
    AllocSlow(bytes, align);
    alignment_bytes = AlignmentBytes(free_area_, align);
  }

  TC_ASSERT_GE(free_avail_, alignment_bytes + bytes);
  free_area_ += alignment_bytes;
  free_avail_ -= alignment_bytes;
  // TODO: b/201694482 - Consider whether to account for alignment bytes to
  // bytes_allocated or bytes_unavailable.
  bytes_allocated_ += alignment_bytes;

  TC_ASSERT_EQ(reinterpret_cast<uintptr_t>(free_area_) % align, 0);
  char* result = free_area_;
  free_area_ += bytes;
  free_avail_ -= bytes;
  bytes_allocated_ += bytes;
  return reinterpret_cast<void*>(result);
}

}  // namespace tcmalloc_internal
}  // namespace tcmalloc
GOOGLE_MALLOC_SECTION_END
