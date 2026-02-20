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
#include <malloc.h>

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <new>
#include <variant>
#include <vector>

#include "gtest/gtest.h"
#include "fuzztest/fuzztest.h"
#include "tcmalloc/internal/memory_tag.h"
#include "tcmalloc/malloc_extension.h"
#include "tcmalloc/parameters.h"
#include "tcmalloc/testing/testutil.h"

#if defined(__SANITIZE_ALLOC_TOKEN__)
#define ATTRIBUTE_NO_SANITIZE_ALLOC_TOKEN \
  __attribute__((no_sanitize("alloc-token")))
#else
#define ATTRIBUTE_NO_SANITIZE_ALLOC_TOKEN
#endif

#define DECLARE_ALLOC_TOKEN_NEW(id)                                          \
  void* __alloc_token_##id##__Znwm(size_t);                                  \
  void* __alloc_token_##id##__Znam(size_t);                                  \
  void* __alloc_token_##id##__ZnwmRKSt9nothrow_t(                            \
      size_t size, const std::nothrow_t&) noexcept;                          \
  void* __alloc_token_##id##__ZnamRKSt9nothrow_t(size_t,                     \
                                                 const std::nothrow_t&);     \
  void* __alloc_token_##id##__ZnwmSt11align_val_t(size_t, std::align_val_t); \
  void* __alloc_token_##id##__ZnamSt11align_val_t(size_t, std::align_val_t); \
  void* __alloc_token_##id##__ZnwmSt11align_val_tRKSt9nothrow_t(             \
      size_t, std::align_val_t, const std::nothrow_t&) noexcept;             \
  void* __alloc_token_##id##__ZnamSt11align_val_tRKSt9nothrow_t(             \
      size_t, std::align_val_t, const std::nothrow_t&) noexcept;

#define DECLARE_ALLOC_TOKEN_NEW_EXTENSION(id)                                  \
  void* __alloc_token_##id##__Znwm12__hot_cold_t(size_t, __hot_cold_t);        \
  void* __alloc_token_##id##__ZnwmRKSt9nothrow_t12__hot_cold_t(                \
      size_t, const std::nothrow_t&, __hot_cold_t) noexcept;                   \
  void* __alloc_token_##id##__ZnwmSt11align_val_t12__hot_cold_t(               \
      size_t, std::align_val_t, __hot_cold_t);                                 \
  void* __alloc_token_##id##__ZnwmSt11align_val_tRKSt9nothrow_t12__hot_cold_t( \
      size_t, std::align_val_t, const std::nothrow_t&, __hot_cold_t) noexcept; \
  void* __alloc_token_##id##__Znam12__hot_cold_t(size_t, __hot_cold_t);        \
  void* __alloc_token_##id##__ZnamRKSt9nothrow_t12__hot_cold_t(                \
      size_t, const std::nothrow_t&, __hot_cold_t) noexcept;                   \
  void* __alloc_token_##id##__ZnamSt11align_val_t12__hot_cold_t(               \
      size_t, std::align_val_t, __hot_cold_t);                                 \
  void* __alloc_token_##id##__ZnamSt11align_val_tRKSt9nothrow_t12__hot_cold_t( \
      size_t, std::align_val_t, const std::nothrow_t&, __hot_cold_t) noexcept; \
  __sized_ptr_t __alloc_token_##id##___size_returning_new(size_t);             \
  __sized_ptr_t __alloc_token_##id##___size_returning_new_aligned(             \
      size_t, std::align_val_t);                                               \
  __sized_ptr_t __alloc_token_##id##___size_returning_new_hot_cold(            \
      size_t, __hot_cold_t);                                                   \
  __sized_ptr_t __alloc_token_##id##___size_returning_new_aligned_hot_cold(    \
      size_t, std::align_val_t, __hot_cold_t);

#define DECLARE_ALLOC_TOKEN_STDLIB(id)                                     \
  void* __alloc_token_##id##_malloc(size_t) noexcept;                      \
  void* __alloc_token_##id##_realloc(void*, size_t) noexcept;              \
  void* __alloc_token_##id##_reallocarray(void*, size_t, size_t) noexcept; \
  void* __alloc_token_##id##_calloc(size_t, size_t) noexcept;              \
  void* __alloc_token_##id##_memalign(size_t, size_t) noexcept;            \
  void* __alloc_token_##id##_aligned_alloc(size_t, size_t) noexcept;       \
  void* __alloc_token_##id##_valloc(size_t) noexcept;                      \
  void* __alloc_token_##id##_pvalloc(size_t) noexcept;                     \
  int __alloc_token_##id##_posix_memalign(void**, size_t, size_t) noexcept;

namespace {

extern "C" {
DECLARE_ALLOC_TOKEN_NEW(0)
DECLARE_ALLOC_TOKEN_STDLIB(0)
DECLARE_ALLOC_TOKEN_NEW_EXTENSION(0)
DECLARE_ALLOC_TOKEN_NEW(1)
DECLARE_ALLOC_TOKEN_STDLIB(1)
DECLARE_ALLOC_TOKEN_NEW_EXTENSION(1)
}  // extern "C"

using fuzztest::Arbitrary;
using fuzztest::ElementOf;
using fuzztest::FlatMap;
using fuzztest::InRange;
using fuzztest::Just;
using fuzztest::Map;
using fuzztest::OneOf;
using fuzztest::StructOf;
using fuzztest::VariantOf;
using fuzztest::VectorOf;

enum class AllocTokenId { NO_ALLOC_TOKEN, ID0, ID1 };
enum class AllocType {
  MALLOC,
  NEW,
  NEW_ARRAY,
  CALLOC,
  MEMALIGN,
  NEW_NOTHROW,
  NEW_ARRAY_NOTHROW,
  NEW_ALIGNED,
  NEW_ARRAY_ALIGNED,
  NEW_ALIGNED_NOTHROW,
  NEW_ARRAY_ALIGNED_NOTHROW,
  NEW_HOT_COLD,
  NEW_NOTHROW_HOT_COLD,
  NEW_ALIGNED_HOT_COLD,
  NEW_ALIGNED_NOTHROW_HOT_COLD,
  NEW_ARRAY_HOT_COLD,
  NEW_ARRAY_NOTHROW_HOT_COLD,
  NEW_ARRAY_ALIGNED_HOT_COLD,
  NEW_ARRAY_ALIGNED_NOTHROW_HOT_COLD,
  SIZE_RETURNING_NEW,
  SIZE_RETURNING_NEW_ALIGNED,
  SIZE_RETURNING_NEW_HOT_COLD,
  SIZE_RETURNING_NEW_ALIGNED_HOT_COLD,
};

// Represents an allocation action
struct AllocationOp {
  AllocTokenId token_id;
  AllocType type;
  size_t size;
  size_t alignment = 0;  // for MEMALIGN and NEW_ALIGNED*
  __hot_cold_t hot_cold = static_cast<__hot_cold_t>(255);
};

// Represents a deallocation action, targeting an index in our live_allocs
// vector
struct DeallocationOp {
  size_t index;
  bool is_sized;
};

using Action = std::variant<AllocationOp, DeallocationOp>;

// Keeps track of a single live allocation
struct AllocationRecord {
  void* ptr;
  AllocType type;
  size_t size;           // For sized delete
  size_t alignment = 0;  // For aligned delete
};

// Helper to call the correct allocation function
ATTRIBUTE_NO_SANITIZE_ALLOC_TOKEN void* PerformAllocate(
    const AllocationOp& op) {
  const auto alignment = static_cast<std::align_val_t>(op.alignment);
  switch (op.token_id) {
    case AllocTokenId::NO_ALLOC_TOKEN:
      switch (op.type) {
        case AllocType::MALLOC:
          return malloc(op.size);
        case AllocType::NEW:
          return ::operator new(op.size);
        case AllocType::NEW_ARRAY:
          return ::operator new[](op.size);
        case AllocType::CALLOC:
          return calloc(1, op.size);
        case AllocType::MEMALIGN:
          return memalign(op.alignment, op.size);
        case AllocType::NEW_NOTHROW:
          return ::operator new(op.size, std::nothrow);
        case AllocType::NEW_ARRAY_NOTHROW:
          return ::operator new[](op.size, std::nothrow);
        case AllocType::NEW_ALIGNED:
          return ::operator new(op.size, alignment);
        case AllocType::NEW_ARRAY_ALIGNED:
          return ::operator new[](op.size, alignment);
        case AllocType::NEW_ALIGNED_NOTHROW:
          return ::operator new(op.size, alignment, std::nothrow);
        case AllocType::NEW_ARRAY_ALIGNED_NOTHROW:
          return ::operator new[](op.size, alignment, std::nothrow);
        case AllocType::NEW_HOT_COLD:
          return ::operator new(op.size, op.hot_cold);
        case AllocType::NEW_NOTHROW_HOT_COLD:
          return ::operator new(op.size, std::nothrow, op.hot_cold);
        case AllocType::NEW_ALIGNED_HOT_COLD:
          return ::operator new(op.size, alignment, op.hot_cold);
        case AllocType::NEW_ALIGNED_NOTHROW_HOT_COLD:
          return ::operator new(op.size, alignment, std::nothrow, op.hot_cold);
        case AllocType::NEW_ARRAY_HOT_COLD:
          return ::operator new[](op.size, op.hot_cold);
        case AllocType::NEW_ARRAY_NOTHROW_HOT_COLD:
          return ::operator new[](op.size, std::nothrow, op.hot_cold);
        case AllocType::NEW_ARRAY_ALIGNED_HOT_COLD:
          return ::operator new[](op.size, alignment, op.hot_cold);
        case AllocType::NEW_ARRAY_ALIGNED_NOTHROW_HOT_COLD:
          return ::operator new[](op.size, alignment, std::nothrow,
                                  op.hot_cold);
        case AllocType::SIZE_RETURNING_NEW:
          return __size_returning_new(op.size).p;
        case AllocType::SIZE_RETURNING_NEW_ALIGNED:
          return __size_returning_new_aligned(op.size, alignment).p;
        case AllocType::SIZE_RETURNING_NEW_HOT_COLD:
          return __size_returning_new_hot_cold(op.size, op.hot_cold).p;
        case AllocType::SIZE_RETURNING_NEW_ALIGNED_HOT_COLD:
          return __size_returning_new_aligned_hot_cold(op.size, alignment,
                                                       op.hot_cold)
              .p;
      }
      break;
    case AllocTokenId::ID0:
      switch (op.type) {
        case AllocType::MALLOC:
          return __alloc_token_0_malloc(op.size);
        case AllocType::NEW:
          return __alloc_token_0__Znwm(op.size);
        case AllocType::NEW_ARRAY:
          return __alloc_token_0__Znam(op.size);
        case AllocType::CALLOC:
          return __alloc_token_0_calloc(1, op.size);
        case AllocType::MEMALIGN:
          return __alloc_token_0_memalign(op.alignment, op.size);
        case AllocType::NEW_NOTHROW:
          return __alloc_token_0__ZnwmRKSt9nothrow_t(op.size, std::nothrow);
        case AllocType::NEW_ARRAY_NOTHROW:
          return __alloc_token_0__ZnamRKSt9nothrow_t(op.size, std::nothrow);
        case AllocType::NEW_ALIGNED:
          return __alloc_token_0__ZnwmSt11align_val_t(op.size, alignment);
        case AllocType::NEW_ARRAY_ALIGNED:
          return __alloc_token_0__ZnamSt11align_val_t(op.size, alignment);
        case AllocType::NEW_ALIGNED_NOTHROW:
          return __alloc_token_0__ZnwmSt11align_val_tRKSt9nothrow_t(
              op.size, alignment, std::nothrow);
        case AllocType::NEW_ARRAY_ALIGNED_NOTHROW:
          return __alloc_token_0__ZnamSt11align_val_tRKSt9nothrow_t(
              op.size, alignment, std::nothrow);
        case AllocType::NEW_HOT_COLD:
          return __alloc_token_0__Znwm12__hot_cold_t(op.size, op.hot_cold);
        case AllocType::NEW_NOTHROW_HOT_COLD:
          return __alloc_token_0__ZnwmRKSt9nothrow_t12__hot_cold_t(
              op.size, std::nothrow, op.hot_cold);
        case AllocType::NEW_ALIGNED_HOT_COLD:
          return __alloc_token_0__ZnwmSt11align_val_t12__hot_cold_t(
              op.size, alignment, op.hot_cold);
        case AllocType::NEW_ALIGNED_NOTHROW_HOT_COLD:
          return __alloc_token_0__ZnwmSt11align_val_tRKSt9nothrow_t12__hot_cold_t(
              op.size, alignment, std::nothrow, op.hot_cold);
        case AllocType::NEW_ARRAY_HOT_COLD:
          return __alloc_token_0__Znam12__hot_cold_t(op.size, op.hot_cold);
        case AllocType::NEW_ARRAY_NOTHROW_HOT_COLD:
          return __alloc_token_0__ZnamRKSt9nothrow_t12__hot_cold_t(
              op.size, std::nothrow, op.hot_cold);
        case AllocType::NEW_ARRAY_ALIGNED_HOT_COLD:
          return __alloc_token_0__ZnamSt11align_val_t12__hot_cold_t(
              op.size, alignment, op.hot_cold);
        case AllocType::NEW_ARRAY_ALIGNED_NOTHROW_HOT_COLD:
          return __alloc_token_0__ZnamSt11align_val_tRKSt9nothrow_t12__hot_cold_t(
              op.size, alignment, std::nothrow, op.hot_cold);
        case AllocType::SIZE_RETURNING_NEW:
          return __alloc_token_0___size_returning_new(op.size).p;
        case AllocType::SIZE_RETURNING_NEW_ALIGNED:
          return __alloc_token_0___size_returning_new_aligned(op.size,
                                                              alignment)
              .p;
        case AllocType::SIZE_RETURNING_NEW_HOT_COLD:
          return __alloc_token_0___size_returning_new_hot_cold(op.size,
                                                               op.hot_cold)
              .p;
        case AllocType::SIZE_RETURNING_NEW_ALIGNED_HOT_COLD:
          return __alloc_token_0___size_returning_new_aligned_hot_cold(
                     op.size, alignment, op.hot_cold)
              .p;
      }
      break;
    case AllocTokenId::ID1:
      switch (op.type) {
        case AllocType::MALLOC:
          return __alloc_token_1_malloc(op.size);
        case AllocType::NEW:
          return __alloc_token_1__Znwm(op.size);
        case AllocType::NEW_ARRAY:
          return __alloc_token_1__Znam(op.size);
        case AllocType::CALLOC:
          return __alloc_token_1_calloc(1, op.size);
        case AllocType::MEMALIGN:
          return __alloc_token_1_memalign(op.alignment, op.size);
        case AllocType::NEW_NOTHROW:
          return __alloc_token_1__ZnwmRKSt9nothrow_t(op.size, std::nothrow);
        case AllocType::NEW_ARRAY_NOTHROW:
          return __alloc_token_1__ZnamRKSt9nothrow_t(op.size, std::nothrow);
        case AllocType::NEW_ALIGNED:
          return __alloc_token_1__ZnwmSt11align_val_t(op.size, alignment);
        case AllocType::NEW_ARRAY_ALIGNED:
          return __alloc_token_1__ZnamSt11align_val_t(op.size, alignment);
        case AllocType::NEW_ALIGNED_NOTHROW:
          return __alloc_token_1__ZnwmSt11align_val_tRKSt9nothrow_t(
              op.size, alignment, std::nothrow);
        case AllocType::NEW_ARRAY_ALIGNED_NOTHROW:
          return __alloc_token_1__ZnamSt11align_val_tRKSt9nothrow_t(
              op.size, alignment, std::nothrow);
        case AllocType::NEW_HOT_COLD:
          return __alloc_token_1__Znwm12__hot_cold_t(op.size, op.hot_cold);
        case AllocType::NEW_NOTHROW_HOT_COLD:
          return __alloc_token_1__ZnwmRKSt9nothrow_t12__hot_cold_t(
              op.size, std::nothrow, op.hot_cold);
        case AllocType::NEW_ALIGNED_HOT_COLD:
          return __alloc_token_1__ZnwmSt11align_val_t12__hot_cold_t(
              op.size, alignment, op.hot_cold);
        case AllocType::NEW_ALIGNED_NOTHROW_HOT_COLD:
          return __alloc_token_1__ZnwmSt11align_val_tRKSt9nothrow_t12__hot_cold_t(
              op.size, alignment, std::nothrow, op.hot_cold);
        case AllocType::NEW_ARRAY_HOT_COLD:
          return __alloc_token_1__Znam12__hot_cold_t(op.size, op.hot_cold);
        case AllocType::NEW_ARRAY_NOTHROW_HOT_COLD:
          return __alloc_token_1__ZnamRKSt9nothrow_t12__hot_cold_t(
              op.size, std::nothrow, op.hot_cold);
        case AllocType::NEW_ARRAY_ALIGNED_HOT_COLD:
          return __alloc_token_1__ZnamSt11align_val_t12__hot_cold_t(
              op.size, alignment, op.hot_cold);
        case AllocType::NEW_ARRAY_ALIGNED_NOTHROW_HOT_COLD:
          return __alloc_token_1__ZnamSt11align_val_tRKSt9nothrow_t12__hot_cold_t(
              op.size, alignment, std::nothrow, op.hot_cold);
        case AllocType::SIZE_RETURNING_NEW:
          return __alloc_token_1___size_returning_new(op.size).p;
        case AllocType::SIZE_RETURNING_NEW_ALIGNED:
          return __alloc_token_1___size_returning_new_aligned(op.size,
                                                              alignment)
              .p;
        case AllocType::SIZE_RETURNING_NEW_HOT_COLD:
          return __alloc_token_1___size_returning_new_hot_cold(op.size,
                                                               op.hot_cold)
              .p;
        case AllocType::SIZE_RETURNING_NEW_ALIGNED_HOT_COLD:
          return __alloc_token_1___size_returning_new_aligned_hot_cold(
                     op.size, alignment, op.hot_cold)
              .p;
      }
      break;
  }
  return nullptr;  // Should never happen
}

// Helper to call the correct deallocation function
void PerformDeallocate(const AllocationRecord& record, bool is_sized) {
  if (!record.ptr) return;
  switch (record.type) {
    case AllocType::MALLOC:
    case AllocType::CALLOC:
      if (is_sized) {
        free_sized(record.ptr, record.size);
      } else {
        free(record.ptr);
      }
      break;
    case AllocType::MEMALIGN:
      if (is_sized) {
        free_aligned_sized(record.ptr, record.alignment, record.size);
      } else {
        free(record.ptr);
      }
      break;
    case AllocType::NEW:
    case AllocType::NEW_NOTHROW:
    case AllocType::NEW_HOT_COLD:
    case AllocType::NEW_NOTHROW_HOT_COLD:
    case AllocType::SIZE_RETURNING_NEW:
    case AllocType::SIZE_RETURNING_NEW_HOT_COLD:
      if (is_sized) {
        ::operator delete(record.ptr, record.size);
      } else {
        ::operator delete(record.ptr);
      }
      break;
    case AllocType::NEW_ARRAY:
    case AllocType::NEW_ARRAY_NOTHROW:
    case AllocType::NEW_ARRAY_HOT_COLD:
    case AllocType::NEW_ARRAY_NOTHROW_HOT_COLD:
      if (is_sized) {
        ::operator delete[](record.ptr, record.size);
      } else {
        ::operator delete[](record.ptr);
      }
      break;
    case AllocType::NEW_ALIGNED:
    case AllocType::NEW_ALIGNED_NOTHROW:
    case AllocType::NEW_ALIGNED_HOT_COLD:
    case AllocType::NEW_ALIGNED_NOTHROW_HOT_COLD:
    case AllocType::SIZE_RETURNING_NEW_ALIGNED:
    case AllocType::SIZE_RETURNING_NEW_ALIGNED_HOT_COLD:
      if (is_sized) {
        ::operator delete(record.ptr, record.size,
                          static_cast<std::align_val_t>(record.alignment));
      } else {
        ::operator delete(record.ptr,
                          static_cast<std::align_val_t>(record.alignment));
      }
      break;
    case AllocType::NEW_ARRAY_ALIGNED:
    case AllocType::NEW_ARRAY_ALIGNED_NOTHROW:
    case AllocType::NEW_ARRAY_ALIGNED_HOT_COLD:
    case AllocType::NEW_ARRAY_ALIGNED_NOTHROW_HOT_COLD:
      ::operator delete[](record.ptr,
                          static_cast<std::align_val_t>(record.alignment));
      break;
  }
}

static inline bool IsCold(tcmalloc::tcmalloc_internal::MemoryTag tag) {
  return tag == tcmalloc::tcmalloc_internal::MemoryTag::kCold;
}

static inline bool IsPartitionZero(tcmalloc::tcmalloc_internal::MemoryTag tag) {
  return tag == tcmalloc::tcmalloc_internal::MemoryTag::kNormalP0;
}

static inline bool IsPartitionOne(tcmalloc::tcmalloc_internal::MemoryTag tag) {
  return tag == tcmalloc::tcmalloc_internal::MemoryTag::kNormalP1;
}

void RandomizedAllocateAndDeallocateFuzzTest(
    const std::vector<Action>& actions) {
  std::vector<AllocationRecord> live_allocs;
  live_allocs.reserve(actions.size());
  tcmalloc::ScopedNeverSample never_sample;

  for (const auto& action : actions) {
    if (std::holds_alternative<AllocationOp>(action)) {
      const auto& alloc_op = std::get<AllocationOp>(action);
      void* ptr = PerformAllocate(alloc_op);
      if (ptr != nullptr) {
        const auto tag = tcmalloc::tcmalloc_internal::GetMemoryTag(ptr);
        bool security_partition =
            tcmalloc::tcmalloc_internal::Parameters::heap_partitioning();
        if (alloc_op.token_id == AllocTokenId::ID0 &&
            alloc_op.hot_cold < tcmalloc::kDefaultMinHotAccessHint) {
          EXPECT_TRUE(IsCold(tag));
        } else if (alloc_op.token_id == AllocTokenId::ID0) {
          EXPECT_TRUE(IsPartitionZero(tag));
        } else if (security_partition) {
          EXPECT_TRUE(IsPartitionOne(tag));
        } else if (alloc_op.hot_cold < tcmalloc::kDefaultMinHotAccessHint) {
          EXPECT_TRUE(IsCold(tag));
        } else {
          // When security option is off, the same set of size classes
          // are used so the resulting pointer's tag will be kNormalP0
          // even the static security token is set to 1:
          EXPECT_TRUE(IsPartitionZero(tag));
        }

        live_allocs.push_back(
            {ptr, alloc_op.type, alloc_op.size, alloc_op.alignment});
      }
    } else if (std::holds_alternative<DeallocationOp>(action)) {
      const auto& dealloc_op = std::get<DeallocationOp>(action);
      if (!live_allocs.empty()) {
        size_t index = dealloc_op.index % live_allocs.size();
        AllocationRecord record = live_allocs[index];

        PerformDeallocate(record, dealloc_op.is_sized);

        // Remove from live_allocs
        live_allocs[index] = live_allocs.back();
        live_allocs.pop_back();
      }
    }
  }

  for (const auto& record : live_allocs) {
    PerformDeallocate(record, false);
  }
}

// FuzzTest Domain Definitions

auto AllocTokenIdDomain() {
  return ElementOf<AllocTokenId>(
      {AllocTokenId::NO_ALLOC_TOKEN, AllocTokenId::ID0, AllocTokenId::ID1});
}

auto AllocTypeDomain() {
  return ElementOf<AllocType>({AllocType::MALLOC,
                               AllocType::NEW,
                               AllocType::NEW_ARRAY,
                               AllocType::CALLOC,
                               AllocType::MEMALIGN,
                               AllocType::NEW_NOTHROW,
                               AllocType::NEW_ARRAY_NOTHROW,
                               AllocType::NEW_ALIGNED,
                               AllocType::NEW_ARRAY_ALIGNED,
                               AllocType::NEW_ALIGNED_NOTHROW,
                               AllocType::NEW_ARRAY_ALIGNED_NOTHROW,
                               AllocType::NEW_HOT_COLD,
                               AllocType::NEW_NOTHROW_HOT_COLD,
                               AllocType::NEW_ALIGNED_HOT_COLD,
                               AllocType::NEW_ALIGNED_NOTHROW_HOT_COLD,
                               AllocType::NEW_ARRAY_HOT_COLD,
                               AllocType::NEW_ARRAY_NOTHROW_HOT_COLD,
                               AllocType::NEW_ARRAY_ALIGNED_HOT_COLD,
                               AllocType::NEW_ARRAY_ALIGNED_NOTHROW_HOT_COLD,
                               AllocType::SIZE_RETURNING_NEW,
                               AllocType::SIZE_RETURNING_NEW_ALIGNED,
                               AllocType::SIZE_RETURNING_NEW_HOT_COLD,
                               AllocType::SIZE_RETURNING_NEW_ALIGNED_HOT_COLD});
}

auto SizeDomain() {
  return OneOf(InRange<size_t>(1, 1024),
               ElementOf<size_t>({2048, 4096, 8192, 16384, 32768}));
}

auto AlignmentDomain() { return ElementOf<size_t>({8, 16, 32, 64, 128}); }
auto HotColdDomain() {
  return ElementOf<__hot_cold_t>(
      {static_cast<__hot_cold_t>(0), static_cast<__hot_cold_t>(255)});
}

auto AllocationOpDomain() {
  return FlatMap(
      [](AllocTokenId token_id, AllocType type,
         size_t size) -> fuzztest::Domain<AllocationOp> {
        bool needs_hot_cold = false;
        bool needs_alignment = false;
        switch (type) {
          case AllocType::MEMALIGN:
          case AllocType::NEW_ALIGNED:
          case AllocType::NEW_ARRAY_ALIGNED:
          case AllocType::NEW_ALIGNED_NOTHROW:
          case AllocType::NEW_ARRAY_ALIGNED_NOTHROW:
          case AllocType::SIZE_RETURNING_NEW_ALIGNED:
            needs_alignment = true;
            break;
          case AllocType::NEW_HOT_COLD:
          case AllocType::NEW_NOTHROW_HOT_COLD:
          case AllocType::NEW_ARRAY_HOT_COLD:
          case AllocType::NEW_ARRAY_NOTHROW_HOT_COLD:
          case AllocType::SIZE_RETURNING_NEW_HOT_COLD:
            needs_hot_cold = true;
            break;
          case AllocType::NEW_ALIGNED_HOT_COLD:
          case AllocType::NEW_ALIGNED_NOTHROW_HOT_COLD:
          case AllocType::NEW_ARRAY_ALIGNED_HOT_COLD:
          case AllocType::NEW_ARRAY_ALIGNED_NOTHROW_HOT_COLD:
          case AllocType::SIZE_RETURNING_NEW_ALIGNED_HOT_COLD:
            needs_alignment = true;
            needs_hot_cold = true;
            break;
          default:
            break;
        }
        if (needs_alignment && needs_hot_cold) {
          return Map(
              [=](size_t alignment, __hot_cold_t hot_cold) {
                return AllocationOp{token_id, type, size, alignment, hot_cold};
              },
              AlignmentDomain(), HotColdDomain());
        } else if (needs_alignment) {
          return Map(
              [=](size_t alignment) {
                return AllocationOp{token_id, type, size, alignment};
              },
              AlignmentDomain());
        } else if (needs_hot_cold) {
          return Map(
              [=](__hot_cold_t hot_cold) {
                return AllocationOp{token_id, type, size, 0, hot_cold};
              },
              HotColdDomain());
        } else {
          return Just(AllocationOp{token_id, type, size, 0});
        }
      },
      AllocTokenIdDomain(), AllocTypeDomain(), SizeDomain());
}

auto DeallocationOpDomain() {
  // Arbitrary index, will be modulo'd in the test.
  return StructOf<DeallocationOp>(Arbitrary<size_t>(), Arbitrary<bool>());
}

auto ActionDomain() {
  return VariantOf(AllocationOpDomain(), DeallocationOpDomain());
}

FUZZ_TEST(SecurityHeapPartitioning, RandomizedAllocateAndDeallocateFuzzTest)
    .WithDomains(VectorOf(ActionDomain()).WithMinSize(1).WithMaxSize(512));

}  // namespace
