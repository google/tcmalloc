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

#ifndef TCMALLOC_METADATA_OBJECT_ALLOCATOR_H_
#define TCMALLOC_METADATA_OBJECT_ALLOCATOR_H_

#include <stddef.h>

#include <new>

#include "absl/base/attributes.h"
#include "absl/base/const_init.h"
#include "absl/base/dynamic_annotations.h"
#include "absl/base/internal/spinlock.h"
#include "absl/base/nullability.h"
#include "absl/base/optimization.h"
#include "absl/base/thread_annotations.h"
#include "absl/functional/function_ref.h"
#include "absl/types/span.h"
#include "tcmalloc/arena.h"
#include "tcmalloc/common.h"
#include "tcmalloc/internal/allocation_guard.h"
#include "tcmalloc/internal/config.h"

#ifdef ABSL_HAVE_ADDRESS_SANITIZER
#include <sanitizer/asan_interface.h>
#endif

GOOGLE_MALLOC_SECTION_BEGIN
namespace tcmalloc {
namespace tcmalloc_internal {

struct AllocatorStats {
  // Number of allocated but unfreed objects
  size_t in_use;
  // Number of objects created (both free and allocated)
  size_t total;
};

// Simple allocator for objects of a specified type.  External locking
// is required before accessing one of these objects.
template <class T, ArenaAlloc AllocType>
class MetadataObjectAllocator {
 public:
  constexpr MetadataObjectAllocator() = default;
  constexpr explicit MetadataObjectAllocator(
      Arena& arena ABSL_ATTRIBUTE_LIFETIME_BOUND)
      : arena_(&arena) {}

  void Init(Arena& arena) {
    TC_CHECK(!arena_);
    arena_ = &arena;
  }

  // Allocates storage for a T.
  //
  // Once New() has been invoked to allocate storage, it is no longer safe to
  // request an overaligned instance via NewWithSize as the underaligned result
  // may be freelisted.
  template <typename... Args>
  [[nodiscard]] ABSL_ATTRIBUTE_RETURNS_NONNULL T* New(Args&&... args) {
    return NewWithSize(sizeof(T), static_cast<std::align_val_t>(alignof(T)),
                       std::forward<Args>(args)...);
  }

  void Delete(T* absl_nonnull p) {
    p->~T();
    AllocationGuardSpinLockHolder l(metadata_lock_);
    DeleteMemory(p);
  }

  void Delete(absl::Span<T* absl_nonnull> ptrs) {
#ifdef TCMALLOC_INTERNAL_LEGACY_LOCKING2
    for (T* p : ptrs) {
      p->~T();
    }
    AllocationGuardSpinLockHolder l(metadata_lock_);
    for (T* p : ptrs) {
      DeleteMemory(p);
    }
#else
    if (ABSL_PREDICT_FALSE(ptrs.empty())) {
      return;
    }
    for (size_t i = 0; i < ptrs.size(); ++i) {
      ptrs[i]->~T();
      if (i > 0) {
        *(reinterpret_cast<void**>(ptrs[i])) = ptrs[i - 1];
      }
    }
#ifdef ABSL_HAVE_ADDRESS_SANITIZER
    for (size_t i = 1; i < ptrs.size(); ++i) {
      ASAN_POISON_MEMORY_REGION(ptrs[i], sizeof(*ptrs[i]));
    }
#endif
    AllocationGuardSpinLockHolder l(metadata_lock_);
    *(reinterpret_cast<void**>(ptrs[0])) = free_list_;
#ifdef ABSL_HAVE_ADDRESS_SANITIZER
    ASAN_POISON_MEMORY_REGION(ptrs[0], sizeof(*ptrs[0]));
#endif
    free_list_ = ptrs.back();
    stats_.in_use -= ptrs.size();
#endif
  }

  void Delete(absl::FunctionRef<T* absl_nullable()> callback) {
#ifdef TCMALLOC_INTERNAL_LEGACY_LOCKING2
    AllocationGuardSpinLockHolder l(metadata_lock_);
    while (T* p = callback()) {
      p->~T();
      DeleteMemory(p);
    }
#else
    T* first = callback();
    if (ABSL_PREDICT_FALSE(first == nullptr)) {
      return;
    }
    first->~T();
    T* last = first;
    size_t count = 1;
    while (T* p = callback()) {
      p->~T();
      *(reinterpret_cast<void**>(p)) = last;
#ifdef ABSL_HAVE_ADDRESS_SANITIZER
      ASAN_POISON_MEMORY_REGION(p, sizeof(*p));
#endif
      last = p;
      ++count;
    }
    AllocationGuardSpinLockHolder l(metadata_lock_);
    *(reinterpret_cast<void**>(first)) = free_list_;
#ifdef ABSL_HAVE_ADDRESS_SANITIZER
    ASAN_POISON_MEMORY_REGION(first, sizeof(*first));
#endif
    free_list_ = last;
    stats_.in_use -= count;
#endif
  }

  [[nodiscard]] AllocatorStats stats() const {
    AllocationGuardSpinLockHolder l(metadata_lock_);

    return stats_;
  }

 private:
  template <typename... Args>
  [[nodiscard]] ABSL_ATTRIBUTE_RETURNS_NONNULL T* NewWithSize(
      size_t size, std::align_val_t align, Args&&... args) {
    static_assert(sizeof(T) >= sizeof(void*),
                  "Object size must be at least pointer sized to fit intrusive "
                  "freelist link pointer");
    T* ret = LockAndAllocMemory(size, align);
    return new (ret) T(std::forward<Args>(args)...);
  }
  [[nodiscard]] ABSL_ATTRIBUTE_RETURNS_NONNULL T* LockAndAllocMemory(
      size_t size, std::align_val_t align) {
    TC_ASSERT_GE(static_cast<size_t>(align), alignof(T));

#ifdef TCMALLOC_INTERNAL_LEGACY_LOCKING2
    AllocationGuardSpinLockHolder l(metadata_lock_);

    // Consult free list
    T* result = free_list_;
    stats_.in_use++;
    if (ABSL_PREDICT_FALSE(result == nullptr)) {
      stats_.total++;
      result = reinterpret_cast<T*>(arena_->Alloc(AllocType, size, align));
      ABSL_ANNOTATE_MEMORY_IS_UNINITIALIZED(result, size);
      return result;
    } else {
#ifdef ABSL_HAVE_ADDRESS_SANITIZER
      // Unpoison the object on the freelist.
      ASAN_UNPOISON_MEMORY_REGION(result, size);
#endif
    }
    free_list_ = *(reinterpret_cast<T**>(free_list_));
    ABSL_ANNOTATE_MEMORY_IS_UNINITIALIZED(result, size);
    return result;
#else
    {
      AllocationGuardSpinLockHolder l(metadata_lock_);

      // Consult free list
      T* result = free_list_;
      stats_.in_use++;
      if (ABSL_PREDICT_TRUE(result != nullptr)) {
#ifdef ABSL_HAVE_ADDRESS_SANITIZER
        // Unpoison the object on the freelist.
        ASAN_UNPOISON_MEMORY_REGION(result, size);
#endif
        free_list_ = *(reinterpret_cast<T**>(free_list_));
        ABSL_ANNOTATE_MEMORY_IS_UNINITIALIZED(result, size);
        return result;
      }
      stats_.total++;
    }
    T* result = reinterpret_cast<T*>(arena_->Alloc(AllocType, size, align));
    ABSL_ANNOTATE_MEMORY_IS_UNINITIALIZED(result, size);
    return result;
#endif
  }

  void DeleteMemory(T* absl_nonnull p)
      ABSL_EXCLUSIVE_LOCKS_REQUIRED(metadata_lock_) {
    *(reinterpret_cast<void**>(p)) = free_list_;
#ifdef ABSL_HAVE_ADDRESS_SANITIZER
    // Poison the object on the freelist.  We do not dereference it after this
    // point.
    ASAN_POISON_MEMORY_REGION(p, sizeof(*p));
#endif
    free_list_ = p;
    stats_.in_use--;
  }

  // Arena from which to allocate memory
  Arena* arena_ = nullptr;

  mutable absl::base_internal::SpinLock metadata_lock_{
      absl::base_internal::SCHEDULE_KERNEL_ONLY};

  // Free list of already carved objects
  T* free_list_ ABSL_GUARDED_BY(metadata_lock_) = nullptr;

  AllocatorStats stats_ ABSL_GUARDED_BY(metadata_lock_) = {};
};

}  // namespace tcmalloc_internal
}  // namespace tcmalloc
GOOGLE_MALLOC_SECTION_END

#endif  // TCMALLOC_METADATA_OBJECT_ALLOCATOR_H_
