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

#include <atomic>
#include <thread>  // NOLINT(build/c++11)
#include <vector>

#include "gtest/gtest.h"
#include "absl/time/clock.h"
#include "absl/time/time.h"
#include "tcmalloc/internal/sysinfo.h"
#include "tcmalloc/malloc_extension.h"

namespace tcmalloc {
namespace {

// This test reclaims per-CPU memory while continuously creating and destroying
// a thread.  We should not access any thread related state after its
// destruction.
TEST(PercpuStaleTest, Integration) {
  const int num_cpus = tcmalloc_internal::NumCPUs();

  std::atomic<bool> stop{false};
  std::thread flusher([&]() {
    while (!stop.load(std::memory_order_acquire)) {
      for (int i = 0; i < num_cpus; ++i) {
        MallocExtension::ReleaseCpuMemory(i);
      }
    }
  });

  absl::Time start = absl::Now();
  while (absl::Now() - start < absl::Seconds(1)) {
    std::thread t([]() {
      void* p = ::operator new(16);
      ::operator delete(p);
    });
    t.join();
  }

  stop.store(true, std::memory_order_release);
  flusher.join();
}

}  // namespace
}  // namespace tcmalloc
