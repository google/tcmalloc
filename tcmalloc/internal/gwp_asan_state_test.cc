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
#include <thread>
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

TEST(GwpAsanStateTest, PreservesFirstTriggeredErrorAgainstConcurrentOverwrite) {
  GwpAsanState state;

  void* first_alloc[] = {reinterpret_cast<void*>(uintptr_t{0x1111}),
                         reinterpret_cast<void*>(uintptr_t{0x2222})};
  void* first_dealloc[] = {reinterpret_cast<void*>(uintptr_t{0x3333}),
                           reinterpret_cast<void*>(uintptr_t{0x4444})};
  const void* kFirstPtr = reinterpret_cast<const void*>(uintptr_t{0xAAA0});

  state.RecordMismatch(kFirstPtr, /*provided_min=*/16, /*provided_max=*/16,
                       /*minimum=*/32, /*maximum=*/64,
                       absl::MakeConstSpan(first_alloc),
                       absl::MakeConstSpan(first_dealloc));

  // Mutate caller buffers to verify internal storage is used.
  first_alloc[0] = nullptr;
  first_dealloc[0] = nullptr;

  // Simulate a second thread concurrently hitting another corrupted object and
  // calling RecordDoubleFree / RecordInvalidFree / RecordMismatchedFree before
  // process abort completes.
  void* second_dealloc[] = {reinterpret_cast<void*>(uintptr_t{0x9999})};
  const void* kSecondPtr = reinterpret_cast<const void*>(uintptr_t{0xBBB0});
  state.RecordDoubleFree(kSecondPtr, absl::MakeConstSpan(second_dealloc));
  state.RecordInvalidFree(kSecondPtr, absl::MakeConstSpan(second_dealloc));
  state.RecordMismatchedFree(
      kSecondPtr, AllocationType::Malloc, AllocationType::New,
      absl::MakeConstSpan(second_dealloc), absl::MakeConstSpan(second_dealloc));

  ASSERT_TRUE(state.triggered());
  EXPECT_EQ(state.type(), GwpAsanState::Type::kMismatchedDelete);
  EXPECT_EQ(state.ptr(), kFirstPtr);
  EXPECT_EQ(state.provided_min(), 16);
  EXPECT_EQ(state.provided_max(), 16);
  EXPECT_EQ(state.minimum_size(), 32);
  EXPECT_EQ(state.maximum_size(), 64);

  ASSERT_TRUE(state.AllocationStack().has_value());
  EXPECT_THAT(*state.AllocationStack(),
              ElementsAre(reinterpret_cast<void*>(uintptr_t{0x1111}),
                          reinterpret_cast<void*>(uintptr_t{0x2222})));
  ASSERT_TRUE(state.DeallocationStack().has_value());
  EXPECT_THAT(*state.DeallocationStack(),
              ElementsAre(reinterpret_cast<void*>(uintptr_t{0x3333}),
                          reinterpret_cast<void*>(uintptr_t{0x4444})));
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
}

TEST(GwpAsanStateTest, ConcurrentRecordPreservesSingleConsistentWinner) {
  constexpr int kThreads = 8;
  GwpAsanState state;
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
      }
      state.RecordDoubleFree(reinterpret_cast<const void*>(tag),
                             absl::MakeConstSpan(local_stack));
      local_stack[0] = nullptr;
      local_stack[1] = nullptr;
    });
  }

  while (ready.load(std::memory_order_acquire) < kThreads) {
  }
  start.store(true, std::memory_order_release);
  for (auto& th : threads) {
    th.join();
  }

  ASSERT_TRUE(state.triggered());
  EXPECT_EQ(state.type(), GwpAsanState::Type::kDoubleFree);
  const auto winner_tag = reinterpret_cast<uintptr_t>(state.ptr());
  EXPECT_GE(winner_tag, uintptr_t{0x10000});
  EXPECT_LE(winner_tag, uintptr_t{0x10000} * kThreads);
  EXPECT_EQ(winner_tag % uintptr_t{0x10000}, 0u);
  ASSERT_TRUE(state.DeallocationStack().has_value());
  EXPECT_THAT(*state.DeallocationStack(),
              ElementsAre(reinterpret_cast<void*>(winner_tag + 1),
                          reinterpret_cast<void*>(winner_tag + 2)));
}

}  // namespace
}  // namespace tcmalloc::tcmalloc_internal
