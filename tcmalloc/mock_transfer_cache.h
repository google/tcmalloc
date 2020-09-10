// Copyright 2020 The TCMalloc Authors
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

#ifndef TCMALLOC_MOCK_TRANSFER_CACHE_H_
#define TCMALLOC_MOCK_TRANSFER_CACHE_H_

#include <stddef.h>

#include <random>

#include "gmock/gmock.h"
#include "absl/random/distributions.h"
#include "absl/random/random.h"
#include "tcmalloc/common.h"
#include "tcmalloc/mock_central_freelist.h"

namespace tcmalloc {

inline constexpr size_t kClassSize = 8;
inline constexpr size_t kNumToMove = 32;

// Mocks for TransferCacheManager to allow use in tests.
class RawMockTransferCacheManager {
 public:
  MOCK_METHOD(int, DetermineSizeClassToEvict, ());
  MOCK_METHOD(bool, ShrinkCache, (int size_class));
  MOCK_METHOD(bool, GrowCache, (int size_class));

  static size_t class_to_size(int size_class) { return kClassSize; }
  static size_t num_objects_to_move(int size_class) { return kNumToMove; }
  void* Alloc(size_t size) {
    memory_.emplace_back(::operator new(size));
    return memory_.back().get();
  }
  struct Free {
    void operator()(void* b) { ::operator delete(b); }
  };

  std::vector<std::unique_ptr<void, Free>> memory_;
};

using MockTransferCacheManager = testing::NiceMock<RawMockTransferCacheManager>;

// Wires up a largely functional TransferCache + TransferCacheManager +
// MockCentralFreeList.
//
// By default, it fills allocations and responds sensibly.  Because it backs
// onto malloc/free, it will detect leaks and memory misuse when run in asan or
// tsan.
//
// Exposes the underlying mocks to allow for more whitebox tests.
//
// Drains the cache and verifies that no data was lost in the destructor.
template <typename TransferCache>
class FakeTransferCacheEnvironment {
 public:
  FakeTransferCacheEnvironment()
      : manager_(), cache_(&manager_), allocs_(0), deallocs_(0) {
    cache_.Init(1);
    ON_CALL(central_freelist(), RemoveRange)
        .WillByDefault(
            [this](void** bufs, int n) { return AllocateBatch(bufs, n); });
    ON_CALL(central_freelist(), InsertRange)
        .WillByDefault(
            [this](void** bufs, int n) { return DeallocateBatch(bufs, n); });

    // We want single threaded tests to be deterministic, so we use a
    // deterministic generator.  Because we don't know about the threading for
    // our tests we cannot keep the generator in a local variable.
    ON_CALL(transfer_cache_manager(), ShrinkCache).WillByDefault([]() {
      thread_local std::mt19937 gen{0};
      return absl::Bernoulli(gen, 0.8);
    });
    ON_CALL(transfer_cache_manager(), GrowCache).WillByDefault([]() {
      thread_local std::mt19937 gen{0};
      return absl::Bernoulli(gen, 0.8);
    });
    ON_CALL(transfer_cache_manager(), DetermineSizeClassToEvict)
        .WillByDefault([]() {
          thread_local std::mt19937 gen{0};
          return absl::Uniform<size_t>(gen, 1, kNumClasses);
        });
  }

  ~FakeTransferCacheEnvironment() {
    Drain();
    EXPECT_EQ(allocs_.load(), deallocs_.load());
  }

  void Shrink() { cache_.ShrinkCache(); }
  void Grow() { cache_.GrowCache(); }

  void Insert(int n) {
    const int batch_size = MockTransferCacheManager::num_objects_to_move(1);
    void* bufs[kMaxObjectsToMove];
    while (n > 0) {
      int b = std::min(n, batch_size);
      AllocateBatch(bufs, b);
      cache_.InsertRange(bufs, b);
      n -= b;
    }
  }

  void Remove(int n) {
    const int batch_size = MockTransferCacheManager::num_objects_to_move(1);
    void* bufs[kMaxObjectsToMove];
    while (n > 0) {
      int b = std::min(n, batch_size);
      cache_.RemoveRange(bufs, b);
      DeallocateBatch(bufs, b);
      n -= b;
    }
  }

  void Drain() { Remove(cache_.tc_length()); }

  void RandomlyPoke() {
    const int batch_size = MockTransferCacheManager::num_objects_to_move(1);

    absl::BitGen gen;
    // We want a probabilistic steady state size:
    // - grow/shrink balance on average
    // - insert/remove balance on average
    double choice = absl::Uniform(gen, 0.0, 1.0);
    if (choice < 0.1) {
      Shrink();
    } else if (choice < 0.2) {
      Grow();
    } else if (choice < 0.6) {
      Insert(absl::Uniform(gen, 1, batch_size));
    } else {
      Remove(absl::Uniform(gen, 1, batch_size));
    }
  }

  TransferCache& transfer_cache() { return cache_.freelist(); }

  MockTransferCacheManager& transfer_cache_manager() { return manager_; }

  MockCentralFreeList& central_freelist() { return cache_.freelist(); }

 private:
  int AllocateBatch(void** bufs, int n) {
    allocs_.fetch_add(n);
    for (int i = 0; i < n; ++i) {
      bufs[i] = ::operator new(4);
    }
    return n;
  }

  int DeallocateBatch(void** bufs, int n) {
    deallocs_.fetch_add(n);
    for (int i = 0; i < n; ++i) {
      ::operator delete(bufs[i]);
    }
    return n;
  }

  MockTransferCacheManager manager_;
  TransferCache cache_;

  std::atomic<int64_t> allocs_;
  std::atomic<int64_t> deallocs_;
};

}  // namespace tcmalloc

#endif  // TCMALLOC_MOCK_TRANSFER_CACHE_H_
