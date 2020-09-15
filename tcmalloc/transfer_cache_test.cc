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

#include "tcmalloc/transfer_cache.h"

#include <atomic>
#include <cstring>
#include <random>
#include <thread>

#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "absl/base/internal/spinlock.h"
#include "absl/random/distributions.h"
#include "absl/random/random.h"
#include "absl/time/clock.h"
#include "absl/types/span.h"
#include "tcmalloc/central_freelist.h"
#include "tcmalloc/common.h"
#include "tcmalloc/mock_central_freelist.h"
#include "tcmalloc/mock_transfer_cache.h"
#include "tcmalloc/static_vars.h"
#include "tcmalloc/testing/thread_manager.h"
#include "tcmalloc/transfer_cache_internals.h"

namespace tcmalloc {
namespace {

using MockTransferCacheEnv =
    FakeTransferCacheEnvironment<internal_transfer_cache::TransferCache<
        MockCentralFreeList, MockTransferCacheManager>>;

TEST(TransferCache, IsolatedSmoke) {
  const int batch_size = MockTransferCacheEnv::kBatchSize;
  MockTransferCacheEnv e;
  EXPECT_CALL(e.central_freelist(), InsertRange).Times(0);
  EXPECT_CALL(e.central_freelist(), RemoveRange).Times(0);
  e.Insert(batch_size);
  e.Insert(batch_size);
  e.Remove(batch_size);
  e.Remove(batch_size);
}

TEST(TransferCache, FetchesFromFreelist) {
  const int batch_size = MockTransferCacheEnv::kBatchSize;
  MockTransferCacheEnv e;
  EXPECT_CALL(e.central_freelist(), InsertRange).Times(0);
  EXPECT_CALL(e.central_freelist(), RemoveRange).Times(1);
  e.Remove(batch_size);
}

TEST(TransferCache, EvictsOtherCaches) {
  const int batch_size = MockTransferCacheEnv::kBatchSize;
  MockTransferCacheEnv e;

  EXPECT_CALL(e.transfer_cache_manager(), ShrinkCache).WillOnce([]() {
    return true;
  });
  EXPECT_CALL(e.central_freelist(), InsertRange).Times(0);

  while (e.transfer_cache().HasSpareCapacity()) {
    e.Insert(batch_size);
  }
  e.Insert(batch_size);
}

TEST(TransferCache, PushesToFreelist) {
  const int batch_size = MockTransferCacheEnv::kBatchSize;
  MockTransferCacheEnv e;

  EXPECT_CALL(e.transfer_cache_manager(), ShrinkCache).WillOnce([]() {
    return false;
  });
  EXPECT_CALL(e.central_freelist(), InsertRange).Times(1);

  while (e.transfer_cache().HasSpareCapacity()) {
    e.Insert(batch_size);
  }
  e.Insert(batch_size);
}

using LockFreeEnv =
    FakeTransferCacheEnvironment<internal_transfer_cache::LockFreeTransferCache<
        MockCentralFreeList, MockTransferCacheManager>>;

TEST(LockFreeTransferCache, IsolatedSmoke) {
  const int batch_size = LockFreeEnv::kBatchSize;
  LockFreeEnv env;
  env.Insert(batch_size);
  env.Insert(batch_size);
  env.Remove(batch_size);
  env.Remove(batch_size);
}

TEST(LockFreeTransferCache, FetchesFromFreelist) {
  const int batch_size = LockFreeEnv::kBatchSize;
  LockFreeEnv env;
  EXPECT_CALL(env.central_freelist(), RemoveRange).Times(1);
  env.Remove(batch_size);
}

TEST(LockFreeTransferCache, EvictsOtherCaches) {
  const int batch_size = LockFreeEnv::kBatchSize;
  LockFreeEnv env;

  EXPECT_CALL(env.transfer_cache_manager(), ShrinkCache).WillOnce([]() {
    return true;
  });
  EXPECT_CALL(env.central_freelist(), InsertRange).Times(0);

  while (env.transfer_cache().HasSpareCapacity()) {
    env.Insert(batch_size);
  }
  env.Insert(batch_size);
}

TEST(LockFreeTransferCache, PushesToFreelist) {
  const int batch_size = LockFreeEnv::kBatchSize;
  LockFreeEnv env;

  EXPECT_CALL(env.transfer_cache_manager(), ShrinkCache).WillOnce([]() {
    return false;
  });
  EXPECT_CALL(env.central_freelist(), InsertRange).Times(1);

  while (env.transfer_cache().HasSpareCapacity()) {
    env.Insert(batch_size);
  }
  env.Insert(batch_size);
}

TEST(LockFreeTransferCache, WrappingWorks) {
  const int batch_size = LockFreeEnv::kBatchSize;

  LockFreeEnv env;
  EXPECT_CALL(env.transfer_cache_manager(), ShrinkCache).Times(0);

  while (env.transfer_cache().HasSpareCapacity()) {
    env.Insert(batch_size);
  }
  for (int i = 0; i < 100; ++i) {
    env.Remove(batch_size);
    env.Insert(batch_size);
  }
}

TEST(LockFreeTransferCache, MultiThreadedUnbiased) {
  LockFreeEnv env;
  ThreadManager threads;
  threads.Start(10, [&](int) { env.RandomlyPoke(); });

  auto start = absl::Now();
  while (start + absl::Seconds(0.3) > absl::Now()) env.RandomlyPoke();
  threads.Stop();
}

TEST(LockFreeTransferCache, MultiThreadedBiasedInsert) {
  const int batch_size = LockFreeEnv::kBatchSize;

  LockFreeEnv env;
  ThreadManager threads;
  threads.Start(10, [&](int) { env.RandomlyPoke(); });

  auto start = absl::Now();
  while (start + absl::Seconds(5) > absl::Now()) env.Insert(batch_size);
  threads.Stop();
}

TEST(LockFreeTransferCache, MultiThreadedBiasedRemove) {
  const int batch_size = LockFreeEnv::kBatchSize;

  LockFreeEnv env;
  ThreadManager threads;
  threads.Start(10, [&](int) { env.RandomlyPoke(); });

  auto start = absl::Now();
  while (start + absl::Seconds(5) > absl::Now()) env.Remove(batch_size);
  threads.Stop();
}

TEST(LockFreeTransferCache, MultiThreadedBiasedShrink) {
  LockFreeEnv env;
  ThreadManager threads;
  threads.Start(10, [&](int) { env.RandomlyPoke(); });

  auto start = absl::Now();
  while (start + absl::Seconds(5) > absl::Now()) env.Shrink();
  threads.Stop();
}

TEST(LockFreeTransferCache, MultiThreadedBiasedGrow) {
  LockFreeEnv env;
  ThreadManager threads;
  threads.Start(10, [&](int) { env.RandomlyPoke(); });

  auto start = absl::Now();
  while (start + absl::Seconds(5) > absl::Now()) env.Grow();
  threads.Stop();
}

}  // namespace
}  // namespace tcmalloc
