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
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <new>
#include <optional>
#include <thread>  // NOLINT(build/c++11)
#include <vector>

#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "absl/types/span.h"
#include "tcmalloc/internal/config.h"
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

  GwpAsanState unaligned_state;
  unaligned_state.RecordInvalidFree(kPtr2,
                                    absl::MakeConstSpan(&dealloc_frame2, 1));
  ASSERT_TRUE(unaligned_state.triggered());
  EXPECT_EQ(unaligned_state.type(), GwpAsanState::Type::kInvalidFree);
  EXPECT_EQ(unaligned_state.ptr(), kPtr2);
  EXPECT_FALSE(unaligned_state.actual_alignment().has_value());
  EXPECT_FALSE(unaligned_state.expected_alignment().has_value());
  EXPECT_FALSE(unaligned_state.AllocationStack().has_value());
  ASSERT_TRUE(unaligned_state.DeallocationStack().has_value());
  EXPECT_THAT(*unaligned_state.DeallocationStack(),
              ElementsAre(dealloc_frame2));
}

TEST(GwpAsanStateTest, PreservesFirstTriggeredErrorAgainstLaterRecord) {
  GwpAsanState state;

  void* first_alloc[] = {reinterpret_cast<void*>(uintptr_t{0x1111}),
                         reinterpret_cast<void*>(uintptr_t{0x2222})};
  void* first_dealloc[] = {reinterpret_cast<void*>(uintptr_t{0x3333}),
                           reinterpret_cast<void*>(uintptr_t{0x4444})};
  void* later_stack[] = {reinterpret_cast<void*>(uintptr_t{0x9999})};
  const void* kPtr1 = reinterpret_cast<const void*>(uintptr_t{0xAAA0});
  const void* kPtr2 = reinterpret_cast<const void*>(uintptr_t{0xBBB0});

  state.RecordMismatch(kPtr1, /*provided_min=*/8, /*provided_max=*/8,
                       /*minimum=*/16, /*maximum=*/32,
                       absl::MakeConstSpan(first_alloc),
                       absl::MakeConstSpan(first_dealloc));

  state.RecordDoubleFree(kPtr2, absl::MakeConstSpan(later_stack));
  state.RecordInvalidFree(kPtr2, absl::MakeConstSpan(later_stack));
  state.RecordMismatchedFree(kPtr2, AllocationType::Malloc, AllocationType::New,
                             absl::MakeConstSpan(later_stack),
                             absl::MakeConstSpan(later_stack));

  ASSERT_TRUE(state.triggered());
  EXPECT_EQ(state.type(), GwpAsanState::Type::kMismatchedDelete);
  EXPECT_EQ(state.ptr(), kPtr1);
  EXPECT_EQ(state.provided_min(), 8);
  EXPECT_EQ(state.provided_max(), 8);
  EXPECT_EQ(state.minimum_size(), 16);
  EXPECT_EQ(state.maximum_size(), 32);
  ASSERT_TRUE(state.AllocationStack().has_value());
  EXPECT_THAT(*state.AllocationStack(),
              ElementsAre(first_alloc[0], first_alloc[1]));
  ASSERT_TRUE(state.DeallocationStack().has_value());
  EXPECT_THAT(*state.DeallocationStack(),
              ElementsAre(first_dealloc[0], first_dealloc[1]));
}

TEST(GwpAsanStateTest, ConcurrentRecordPreservesSingleConsistentError) {
  GwpAsanState state;
  constexpr int kThreads = 8;
  std::atomic<int> ready{0};
  std::atomic<bool> start{false};
  std::vector<std::thread> threads;
  threads.reserve(kThreads);

  for (int t = 0; t < kThreads; ++t) {
    threads.emplace_back([&, t]() {
      const uintptr_t tag = uintptr_t{0x10000} * (t + 1);
      void* local_stack[] = {reinterpret_cast<void*>(tag + 1),
                             reinterpret_cast<void*>(tag + 2)};
      ready.fetch_add(1, std::memory_order_release);
      while (!start.load(std::memory_order_acquire)) {
        std::this_thread::yield();
      }
      state.RecordDoubleFree(reinterpret_cast<const void*>(tag),
                             absl::MakeConstSpan(local_stack));
      EXPECT_TRUE(state.triggered());
      local_stack[0] = nullptr;
      local_stack[1] = nullptr;
    });
  }

  while (ready.load(std::memory_order_acquire) < kThreads) {
    std::this_thread::yield();
  }
  start.store(true, std::memory_order_release);
  for (auto& thread : threads) {
    thread.join();
  }

  ASSERT_TRUE(state.triggered());
  EXPECT_EQ(state.type(), GwpAsanState::Type::kDoubleFree);
  const uintptr_t winner_tag = reinterpret_cast<uintptr_t>(state.ptr());
  EXPECT_EQ(winner_tag % uintptr_t{0x10000}, 0u);
  EXPECT_GE(winner_tag / uintptr_t{0x10000}, 1u);
  EXPECT_LE(winner_tag / uintptr_t{0x10000}, static_cast<uintptr_t>(kThreads));
  EXPECT_FALSE(state.AllocationStack().has_value());
  ASSERT_TRUE(state.DeallocationStack().has_value());
  EXPECT_THAT(*state.DeallocationStack(),
              ElementsAre(reinterpret_cast<void*>(winner_tag + 1),
                          reinterpret_cast<void*>(winner_tag + 2)));
}

}  // namespace
}  // namespace tcmalloc::tcmalloc_internal
