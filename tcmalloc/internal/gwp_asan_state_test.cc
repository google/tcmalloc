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

#include "tcmalloc/internal/gwp_asan_state.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <new>
#include <optional>
#include <vector>

#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "absl/types/span.h"
#include "tcmalloc/internal/logging.h"
#include "tcmalloc/malloc_extension.h"

namespace tcmalloc::tcmalloc_internal {
namespace {

using ::testing::ElementsAre;

TEST(GwpAsanStateTest, CopiesAndClampsLocalStackBuffers) {
  GwpAsanState state;
  EXPECT_FALSE(state.triggered());

  constexpr size_t kCallerDepth = kMaxStackDepth + 8;
  std::vector<void*> alloc_buf(kCallerDepth);
  std::vector<void*> dealloc_buf(kCallerDepth);
  for (size_t i = 0; i < kCallerDepth; ++i) {
    alloc_buf[i] = reinterpret_cast<void*>(uintptr_t{0x1000} + i);
    dealloc_buf[i] = reinterpret_cast<void*>(uintptr_t{0x2000} + i);
  }

  const void* kPtr = reinterpret_cast<const void*>(uintptr_t{0xCAFE0000});
  state.RecordMismatch(kPtr, /*provided_min=*/8, /*provided_max=*/8,
                       /*minimum=*/16, /*maximum=*/32,
                       absl::MakeConstSpan(alloc_buf),
                       absl::MakeConstSpan(dealloc_buf));

  // Mutate the caller's temporary stack buffers after recording.
  std::fill(alloc_buf.begin(), alloc_buf.end(), nullptr);
  std::fill(dealloc_buf.begin(), dealloc_buf.end(), nullptr);

  ASSERT_TRUE(state.triggered());
  EXPECT_EQ(state.type(), GwpAsanState::Type::kMismatchedDelete);
  EXPECT_EQ(state.ptr(), kPtr);
  EXPECT_EQ(state.provided_min(), 8);
  EXPECT_EQ(state.provided_max(), 8);
  EXPECT_EQ(state.minimum_size(), 16);
  EXPECT_EQ(state.maximum_size(), 32);

  std::optional<absl::Span<void* const>> alloc_stack = state.AllocationStack();
  ASSERT_TRUE(alloc_stack.has_value());
  ASSERT_EQ(alloc_stack->size(), kMaxStackDepth);
  for (size_t i = 0; i < kMaxStackDepth; ++i) {
    EXPECT_EQ((*alloc_stack)[i],
              reinterpret_cast<void*>(uintptr_t{0x1000} + i));
  }

  std::optional<absl::Span<void* const>> dealloc_stack =
      state.DeallocationStack();
  ASSERT_TRUE(dealloc_stack.has_value());
  ASSERT_EQ(dealloc_stack->size(), kMaxStackDepth);
  for (size_t i = 0; i < kMaxStackDepth; ++i) {
    EXPECT_EQ((*dealloc_stack)[i],
              reinterpret_cast<void*>(uintptr_t{0x2000} + i));
  }
}

TEST(GwpAsanStateTest, EmptyStackSpan) {
  GwpAsanState state;
  const void* kPtr = reinterpret_cast<const void*>(uintptr_t{0x1234});
  state.RecordDoubleFree(kPtr, {});

  ASSERT_TRUE(state.triggered());
  EXPECT_EQ(state.type(), GwpAsanState::Type::kDoubleFree);
  EXPECT_EQ(state.ptr(), kPtr);
  EXPECT_FALSE(state.AllocationStack().has_value());
  ASSERT_TRUE(state.DeallocationStack().has_value());
  EXPECT_TRUE(state.DeallocationStack()->empty());

  GwpAsanState mismatched;
  void* frame = reinterpret_cast<void*>(uintptr_t{0x5678});
  mismatched.RecordMismatchedFree(kPtr, AllocationType::Malloc,
                                  AllocationType::New,
                                  absl::MakeConstSpan(&frame, 1), {});
  ASSERT_TRUE(mismatched.triggered());
  EXPECT_EQ(mismatched.type(), GwpAsanState::Type::kMismatchedFree);
  EXPECT_EQ(mismatched.alloc_type(), AllocationType::Malloc);
  EXPECT_EQ(mismatched.dealloc_type(), AllocationType::New);
  ASSERT_TRUE(mismatched.AllocationStack().has_value());
  EXPECT_THAT(*mismatched.AllocationStack(), ElementsAre(frame));
  ASSERT_TRUE(mismatched.DeallocationStack().has_value());
  EXPECT_TRUE(mismatched.DeallocationStack()->empty());

  GwpAsanState invalid_free;
  invalid_free.RecordInvalidFree(kPtr, {});
  ASSERT_TRUE(invalid_free.triggered());
  EXPECT_EQ(invalid_free.type(), GwpAsanState::Type::kInvalidFree);
  EXPECT_EQ(invalid_free.ptr(), kPtr);
  EXPECT_FALSE(invalid_free.AllocationStack().has_value());
  EXPECT_FALSE(invalid_free.actual_alignment().has_value());
  EXPECT_FALSE(invalid_free.expected_alignment().has_value());
  ASSERT_TRUE(invalid_free.DeallocationStack().has_value());
  EXPECT_TRUE(invalid_free.DeallocationStack()->empty());
}

TEST(GwpAsanStateTest, RecordInvalidFreeResetsOptionalFields) {
  GwpAsanState state;
  void* alloc_frame = reinterpret_cast<void*>(uintptr_t{0x1111});
  void* dealloc_frame1 = reinterpret_cast<void*>(uintptr_t{0x2222});
  void* dealloc_frame2 = reinterpret_cast<void*>(uintptr_t{0x3333});
  const void* kPtr1 = reinterpret_cast<const void*>(uintptr_t{0xAAA0});
  const void* kPtr2 = reinterpret_cast<const void*>(uintptr_t{0xBBB0});

  state.RecordInvalidFree(kPtr1, std::align_val_t{16}, std::align_val_t{32},
                          absl::MakeConstSpan(&alloc_frame, 1),
                          absl::MakeConstSpan(&dealloc_frame1, 1));
  ASSERT_TRUE(state.triggered());
  EXPECT_EQ(state.type(), GwpAsanState::Type::kInvalidFree);
  EXPECT_EQ(state.ptr(), kPtr1);
  EXPECT_EQ(state.actual_alignment(), std::align_val_t{16});
  EXPECT_EQ(state.expected_alignment(), std::align_val_t{32});
  ASSERT_TRUE(state.AllocationStack().has_value());
  EXPECT_THAT(*state.AllocationStack(), ElementsAre(alloc_frame));
  ASSERT_TRUE(state.DeallocationStack().has_value());
  EXPECT_THAT(*state.DeallocationStack(), ElementsAre(dealloc_frame1));

  state.RecordInvalidFree(kPtr2, absl::MakeConstSpan(&dealloc_frame2, 1));
  ASSERT_TRUE(state.triggered());
  EXPECT_EQ(state.type(), GwpAsanState::Type::kInvalidFree);
  EXPECT_EQ(state.ptr(), kPtr2);
  EXPECT_FALSE(state.actual_alignment().has_value());
  EXPECT_FALSE(state.expected_alignment().has_value());
  EXPECT_FALSE(state.AllocationStack().has_value());
  ASSERT_TRUE(state.DeallocationStack().has_value());
  EXPECT_THAT(*state.DeallocationStack(), ElementsAre(dealloc_frame2));
}

}  // namespace
}  // namespace tcmalloc::tcmalloc_internal
