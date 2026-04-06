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

#include "tcmalloc/internal/percpu_state.h"

#include <atomic>
#include <cstddef>
#include <thread>  // NOLINT(build/c++11)
#include <vector>

#include "gtest/gtest.h"
#include "absl/base/attributes.h"
#include "absl/base/nullability.h"
#include "tcmalloc/internal/allocation_guard.h"
#include "tcmalloc/internal/logging.h"

namespace tcmalloc::tcmalloc_internal {
namespace {

TEST(PerCpuState, DoubleInit) {
  // This should not crash or leak memory.
  AllocationGuard g;
  PerCpuState::state().Init();
  PerCpuState::state().Init();
}

constexpr size_t kFakeThreadCacheSize = 8;
ABSL_CONST_INIT std::atomic<int> callbacks{0};

extern "C" void TCMalloc_Internal_DestroyThreadCache(
    ThreadCache* absl_nullable cache) {
  TC_CHECK_NE(cache, nullptr);

  ::operator delete(cache, kFakeThreadCacheSize);
  callbacks.fetch_add(1, std::memory_order_relaxed);
}

TEST(PerCpuState, Callbacks) {
  PerCpuState::state().Init();

  callbacks.store(0, std::memory_order_relaxed);

  const int kThreads = 5;
  std::vector<std::thread> threads;
  threads.reserve(kThreads);
  for (int i = 0; i < kThreads; ++i) {
    threads.emplace_back(
        [&](const int thread_id) {
          void* ptr;

          if (thread_id == 3) {
            // This thread should remain nullptr to ensure we don't get a
            // callback.
            ptr = nullptr;
          } else {
            // Simulate a `ThreadCache` instance by allocating some memory.  We
            // will see a leak if we don't delete it.
            ptr = ::operator new(kFakeThreadCacheSize);
          }

          PerCpuState::state().RegisterThreadCache(
              static_cast<ThreadCache*>(ptr));
        },
        i);
  }

  for (auto& thread : threads) {
    thread.join();
  }

  EXPECT_EQ(callbacks.load(std::memory_order_relaxed), kThreads - 1);
}

}  // namespace
}  // namespace tcmalloc::tcmalloc_internal
