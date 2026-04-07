// Copyright 2025 The TCMalloc Authors
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

#include <malloc.h>
#include <stddef.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <new>
#include <vector>

#include "gtest/gtest.h"
#include "absl/base/attributes.h"
#include "tcmalloc/internal/config.h"
#include "tcmalloc/internal/logging.h"
#include "tcmalloc/malloc_extension.h"
#include "tcmalloc/malloc_hook.h"

namespace {

inline ABSL_ATTRIBUTE_ALWAYS_INLINE void wrap_delete(void* ptr, size_t size) {
  operator delete(ptr);
}

inline ABSL_ATTRIBUTE_ALWAYS_INLINE void wrap_delete_array(void* ptr,
                                                           size_t size) {
  operator delete[](ptr);
}

inline ABSL_ATTRIBUTE_ALWAYS_INLINE void wrap_sized_delete(void* ptr,
                                                           size_t size) {
#ifdef __cpp_sized_deallocation
  operator delete(ptr, size);
#else
  operator delete(ptr);
#endif
}

inline ABSL_ATTRIBUTE_ALWAYS_INLINE void wrap_sized_delete_array(void* ptr,
                                                                 size_t size) {
#ifdef __cpp_sized_deallocation
  operator delete[](ptr, size);
#else
  operator delete[](ptr);
#endif
}

inline ABSL_ATTRIBUTE_ALWAYS_INLINE void* wrap_new_cold(size_t size) {
  return operator new(size, tcmalloc::hot_cold_t(0));
}

inline ABSL_ATTRIBUTE_ALWAYS_INLINE void* wrap_new_array_aligned_cold_nothrow(
    size_t size) {
  return operator new[](size, std::align_val_t(64), std::nothrow,
                        tcmalloc::hot_cold_t(0));
}

inline ABSL_ATTRIBUTE_ALWAYS_INLINE void* wrap_new_aligned(size_t size) {
  return operator new(size, std::align_val_t(64));
}

inline ABSL_ATTRIBUTE_ALWAYS_INLINE void wrap_delete_aligned(void* ptr,
                                                             size_t size) {
#ifdef __cpp_sized_deallocation
  operator delete(ptr, size, std::align_val_t(64));
#else
  operator delete(ptr, std::align_val_t(64));
#endif
}

inline ABSL_ATTRIBUTE_ALWAYS_INLINE void wrap_delete_aligned_array(
    void* ptr, size_t size) {
#ifdef __cpp_sized_deallocation
  operator delete[](ptr, size, std::align_val_t(64));
#else
  operator delete[](ptr, std::align_val_t(64));
#endif
}

inline ABSL_ATTRIBUTE_ALWAYS_INLINE void* wrap_new_nothrow(size_t size) {
  return operator new(size, std::nothrow);
}

inline ABSL_ATTRIBUTE_ALWAYS_INLINE void wrap_delete_nothrow(void* ptr,
                                                             size_t size) {
  operator delete(ptr, std::nothrow);
}

inline ABSL_ATTRIBUTE_ALWAYS_INLINE void* wrap_calloc(size_t size) {
  return calloc(size, 1);
}

inline ABSL_ATTRIBUTE_ALWAYS_INLINE void wrap_free(void* ptr, size_t size) {
  free(ptr);
}

inline ABSL_ATTRIBUTE_ALWAYS_INLINE void wrap_free_aligned_sized(void* ptr,
                                                                 size_t size) {
  free_aligned_sized(ptr, 64, size);
}

inline ABSL_ATTRIBUTE_ALWAYS_INLINE void wrap_sdallocx(void* ptr, size_t size) {
  sdallocx(ptr, size, 0);
}

inline ABSL_ATTRIBUTE_ALWAYS_INLINE void* wrap_realloc(size_t size) {
  return realloc(malloc(1), size);
}

inline ABSL_ATTRIBUTE_ALWAYS_INLINE void* wrap_reallocarray(size_t size) {
  return reallocarray(malloc(1), size, 1);
}

inline ABSL_ATTRIBUTE_ALWAYS_INLINE void* wrap_posix_memalign(size_t size) {
  void* res;
  if (posix_memalign(&res, 64, size)) {
    abort();
  }
  return res;
}

inline ABSL_ATTRIBUTE_ALWAYS_INLINE void* wrap_aligned_alloc(size_t size) {
  return aligned_alloc(64, size);
}

inline ABSL_ATTRIBUTE_ALWAYS_INLINE void* wrap_memalign(size_t size) {
  return memalign(64, size);
}

inline ABSL_ATTRIBUTE_ALWAYS_INLINE void* wrap_size_returning_operator_new(
    size_t size) {
  return __size_returning_new(size).p;
}

inline ABSL_ATTRIBUTE_ALWAYS_INLINE void*
wrap_size_returning_operator_new_nothrow(size_t size) {
  return tcmalloc_size_returning_operator_new_nothrow(size).p;
}

inline ABSL_ATTRIBUTE_ALWAYS_INLINE void*
wrap_size_returning_operator_new_hot_cold(size_t size) {
  return __size_returning_new_hot_cold(size, tcmalloc::hot_cold_t(0)).p;
}

inline ABSL_ATTRIBUTE_ALWAYS_INLINE void*
wrap_size_returning_operator_new_hot_cold_nothrow(size_t size) {
  return tcmalloc_size_returning_operator_new_hot_cold_nothrow(
             size, tcmalloc::hot_cold_t(0))
      .p;
}

inline ABSL_ATTRIBUTE_ALWAYS_INLINE void*
wrap_size_returning_operator_new_aligned(size_t size) {
  return __size_returning_new_aligned(size, std::align_val_t(64)).p;
}

inline ABSL_ATTRIBUTE_ALWAYS_INLINE void*
wrap_size_returning_operator_new_aligned_nothrow(size_t size) {
  return tcmalloc_size_returning_operator_new_aligned_nothrow(
             size, std::align_val_t(64))
      .p;
}

inline ABSL_ATTRIBUTE_ALWAYS_INLINE void*
wrap_size_returning_operator_new_aligned_hot_cold(size_t size) {
  return __size_returning_new_aligned_hot_cold(size, std::align_val_t(64),
                                               tcmalloc::hot_cold_t(0))
      .p;
}

inline ABSL_ATTRIBUTE_ALWAYS_INLINE void*
wrap_size_returning_operator_new_aligned_hot_cold_nothrow(size_t size) {
  return tcmalloc_size_returning_operator_new_aligned_hot_cold_nothrow(
             size, std::align_val_t(64), tcmalloc::hot_cold_t(0))
      .p;
}

inline ABSL_ATTRIBUTE_ALWAYS_INLINE void* wrap_std_allocator_allocate(
    size_t size) {
  return std::allocator<char>().allocate(size);
}

#if defined(_LIBCPP_VERSION) && _LIBCPP_VERSION >= 150000
inline ABSL_ATTRIBUTE_ALWAYS_INLINE void*
wrap_std_allocator_allocate_at_least_internal(size_t size) {
  std::allocator<char> a;
  return std::__allocate_at_least(a, size).ptr;  // NOLINT
}
#endif

#if defined(__cpp_lib_allocate_at_least) && \
    __cpp_lib_allocate_at_least >= 202302L
inline ABSL_ATTRIBUTE_ALWAYS_INLINE void* wrap_std_allocator_allocate_at_least(
    size_t size) {
  return std::allocate_at_least(std::allocator<char>(), size).ptr;
}
#endif

inline ABSL_ATTRIBUTE_ALWAYS_INLINE void wrap_std_allocator_deallocate(
    void* ptr, size_t size) {
  std::allocator<char>().deallocate(static_cast<char*>(ptr), size);
}

extern "C" ABSL_ATTRIBUTE_NOINLINE void* __alloc_token_9_posix_memalign_test(
    size_t size) {
  void* res;
  if (posix_memalign(&res, 64, size)) {
    abort();
  }
  return res;
}

extern "C" ABSL_ATTRIBUTE_NOINLINE void* __alloc_token_0_calloc_test(
    size_t size) {
  return calloc(size, 1);
}

extern "C" ABSL_ATTRIBUTE_NOINLINE void* __alloc_token_9_realloc_test(
    size_t size) {
  return realloc(malloc(1), size);
}

extern "C" ABSL_ATTRIBUTE_NOINLINE void* __alloc_token_9__Znwm_test(
    size_t size) {
  return operator new(size);
}

void ReentrantNewHook(const tcmalloc::MallocHook::NewInfo& info) {
  ::operator delete(::operator new(1));
}

void ReentrantDeleteHook(const tcmalloc::MallocHook::DeleteInfo& info) {
  ::operator delete(::operator new(1));
}

template <void*(alloc)(size_t), void(free_fn)(void*, size_t)>
inline ABSL_ATTRIBUTE_ALWAYS_INLINE void test() {
  SCOPED_TRACE(__PRETTY_FUNCTION__);

#if !defined(NDEBUG) || defined(TCMALLOC_INTERNAL_WITH_ASSERTIONS)
  EXPECT_DEATH(
      {
        TC_CHECK(tcmalloc::MallocHook::AddNewHook(ReentrantNewHook));
        void* volatile ptr = alloc(16);
        free_fn(ptr, 16);
      },
      "tcmalloc_reentrancy_count");

  EXPECT_DEATH(
      {
        TC_CHECK(tcmalloc::MallocHook::AddDeleteHook(ReentrantDeleteHook));
        void* volatile ptr = alloc(16);
        free_fn(ptr, 16);
      },
      "tcmalloc_reentrancy_count");
#endif
}

TEST(ReentrancyTest, Matrix) {
  if (tcmalloc::tcmalloc_internal::kSanitizerPresent) {
    GTEST_SKIP() << "Skipping under sanitizers";
  }

  GTEST_SKIP() << "Skipping due to pthread_setspecific issue";

#if !defined(NDEBUG) || defined(TCMALLOC_INTERNAL_WITH_ASSERTIONS)
  test<operator new, wrap_delete>();
  test<operator new, wrap_sized_delete>();
  test<operator new[], wrap_sized_delete_array>();
  test<operator new[], wrap_delete_array>();
  test<wrap_new_cold, wrap_sized_delete>();
  test<wrap_new_array_aligned_cold_nothrow, wrap_delete_aligned_array>();
  test<wrap_new_aligned, wrap_delete_aligned>();
  test<wrap_new_nothrow, wrap_delete_nothrow>();
  test<wrap_size_returning_operator_new, wrap_sized_delete>();
  test<wrap_size_returning_operator_new_nothrow, wrap_sized_delete>();
  test<wrap_size_returning_operator_new_hot_cold, wrap_sized_delete>();
  test<wrap_size_returning_operator_new_hot_cold_nothrow, wrap_sized_delete>();
  test<wrap_size_returning_operator_new_aligned, wrap_delete_aligned>();
  test<wrap_size_returning_operator_new_aligned_nothrow, wrap_delete_aligned>();
  test<wrap_size_returning_operator_new_aligned_hot_cold,
       wrap_delete_aligned>();
  test<wrap_size_returning_operator_new_aligned_hot_cold_nothrow,
       wrap_delete_aligned>();
  test<wrap_std_allocator_allocate, wrap_std_allocator_deallocate>();
#if defined(__cpp_lib_allocate_at_least) && \
    __cpp_lib_allocate_at_least >= 202302L
  test<wrap_std_allocator_allocate_at_least, wrap_std_allocator_deallocate>();
#endif
#if defined(_LIBCPP_VERSION) && _LIBCPP_VERSION >= 150000
  test<wrap_std_allocator_allocate_at_least_internal,
       wrap_std_allocator_deallocate>();
#endif
  test<malloc, wrap_free>();
  test<malloc, free_sized>();
  test<valloc, wrap_free>();
  test<malloc, wrap_sdallocx>();
  test<wrap_realloc, wrap_free>();
  test<wrap_reallocarray, wrap_free>();
  test<wrap_calloc, wrap_free>();
  test<wrap_posix_memalign, wrap_free>();
  test<wrap_aligned_alloc, wrap_free>();
  test<wrap_aligned_alloc, wrap_free_aligned_sized>();
  test<wrap_memalign, wrap_free>();
  // Allocation functions renamed by LLVM heap partitioning.
  test<__alloc_token_9_posix_memalign_test, wrap_free>();
  test<__alloc_token_0_calloc_test, wrap_free>();
  test<__alloc_token_9_realloc_test, wrap_free>();
  test<__alloc_token_9__Znwm_test, wrap_delete>();
#if defined(__x86_64__)
  // pvalloc is not present in some versions of libc.
  test<pvalloc, wrap_free>();
#endif
#endif
}

TEST(ReentrancyTest, ReallocSizeChange) {
  if (tcmalloc::tcmalloc_internal::kSanitizerPresent) {
    GTEST_SKIP() << "Skipping under sanitizers";
  }

  GTEST_SKIP() << "Skipping due to pthread_setspecific issue";

  void* ptr = malloc(16);

#if !defined(NDEBUG) || defined(TCMALLOC_INTERNAL_WITH_ASSERTIONS)
  EXPECT_DEATH(
      {
        TC_CHECK(tcmalloc::MallocHook::AddNewHook(ReentrantNewHook));
        TC_CHECK(tcmalloc::MallocHook::AddDeleteHook(ReentrantDeleteHook));
        TC_CHECK_EQ(realloc(ptr, 0), nullptr);
      },
      "tcmalloc_reentrancy_count");

  EXPECT_DEATH(
      {
        TC_CHECK(tcmalloc::MallocHook::AddNewHook(ReentrantNewHook));
        TC_CHECK(tcmalloc::MallocHook::AddDeleteHook(ReentrantDeleteHook));
        TC_CHECK_NE(realloc(ptr, 8), nullptr);
      },
      "tcmalloc_reentrancy_count");

  EXPECT_DEATH(
      {
        TC_CHECK(tcmalloc::MallocHook::AddNewHook(ReentrantNewHook));
        TC_CHECK(tcmalloc::MallocHook::AddDeleteHook(ReentrantDeleteHook));
        TC_CHECK_NE(realloc(ptr, 64), nullptr);
      },
      "tcmalloc_reentrancy_count");

#endif

  free(ptr);
}

}  // namespace
