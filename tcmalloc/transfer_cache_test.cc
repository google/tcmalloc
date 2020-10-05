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

template <typename Env>
using TransferCacheTest = ::testing::Test;
TYPED_TEST_SUITE_P(TransferCacheTest);

TYPED_TEST_P(TransferCacheTest, IsolatedSmoke) {
  const int batch_size = TypeParam::kBatchSize;
  TypeParam e;
  EXPECT_CALL(e.central_freelist(), InsertRange).Times(0);
  EXPECT_CALL(e.central_freelist(), RemoveRange).Times(0);

  EXPECT_EQ(e.transfer_cache().GetHitRateStats().insert_hits, 0);
  EXPECT_EQ(e.transfer_cache().GetHitRateStats().insert_misses, 0);
  EXPECT_EQ(e.transfer_cache().GetHitRateStats().remove_hits, 0);
  EXPECT_EQ(e.transfer_cache().GetHitRateStats().remove_misses, 0);

  e.Insert(batch_size);
  EXPECT_EQ(e.transfer_cache().GetHitRateStats().insert_hits, 1);
  e.Insert(batch_size);
  EXPECT_EQ(e.transfer_cache().GetHitRateStats().insert_hits, 2);
  e.Remove(batch_size);
  EXPECT_EQ(e.transfer_cache().GetHitRateStats().remove_hits, 1);
  e.Remove(batch_size);
  EXPECT_EQ(e.transfer_cache().GetHitRateStats().remove_hits, 2);
}

TYPED_TEST_P(TransferCacheTest, FetchesFromFreelist) {
  const int batch_size = TypeParam::kBatchSize;
  TypeParam e;
  EXPECT_CALL(e.central_freelist(), InsertRange).Times(0);
  EXPECT_CALL(e.central_freelist(), RemoveRange).Times(1);
  e.Remove(batch_size);
  EXPECT_EQ(e.transfer_cache().GetHitRateStats().remove_misses, 1);
}

TYPED_TEST_P(TransferCacheTest, EvictsOtherCaches) {
  const int batch_size = TypeParam::kBatchSize;
  TypeParam e;

  EXPECT_CALL(e.transfer_cache_manager(), ShrinkCache).WillOnce([]() {
    return true;
  });
  EXPECT_CALL(e.central_freelist(), InsertRange).Times(0);

  while (e.transfer_cache().HasSpareCapacity()) {
    e.Insert(batch_size);
  }
  size_t old_hits = e.transfer_cache().GetHitRateStats().insert_hits;
  e.Insert(batch_size);
  EXPECT_EQ(e.transfer_cache().GetHitRateStats().insert_hits, old_hits + 1);
  EXPECT_EQ(e.transfer_cache().GetHitRateStats().insert_misses, 0);
}

TYPED_TEST_P(TransferCacheTest, PushesToFreelist) {
  const int batch_size = TypeParam::kBatchSize;
  TypeParam e;

  EXPECT_CALL(e.transfer_cache_manager(), ShrinkCache).WillOnce([]() {
    return false;
  });
  EXPECT_CALL(e.central_freelist(), InsertRange).Times(1);

  while (e.transfer_cache().HasSpareCapacity()) {
    e.Insert(batch_size);
  }
  size_t old_hits = e.transfer_cache().GetHitRateStats().insert_hits;
  e.Insert(batch_size);
  EXPECT_EQ(e.transfer_cache().GetHitRateStats().insert_hits, old_hits);
  EXPECT_EQ(e.transfer_cache().GetHitRateStats().insert_misses, 1);
}

TYPED_TEST_P(TransferCacheTest, WrappingWorks) {
  const int batch_size = TypeParam::kBatchSize;

  TypeParam env;
  EXPECT_CALL(env.transfer_cache_manager(), ShrinkCache).Times(0);

  while (env.transfer_cache().HasSpareCapacity()) {
    env.Insert(batch_size);
  }
  for (int i = 0; i < 100; ++i) {
    env.Remove(batch_size);
    env.Insert(batch_size);
  }
}

REGISTER_TYPED_TEST_SUITE_P(TransferCacheTest, IsolatedSmoke,
                            FetchesFromFreelist, EvictsOtherCaches,
                            PushesToFreelist, WrappingWorks);
template <typename Env>
using TransferCacheFuzzTest = ::testing::Test;
TYPED_TEST_SUITE_P(TransferCacheFuzzTest);

TYPED_TEST_P(TransferCacheFuzzTest, MultiThreadedUnbiased) {
  TypeParam env;
  ThreadManager threads;
  threads.Start(10, [&](int) { env.RandomlyPoke(); });

  auto start = absl::Now();
  while (start + absl::Seconds(0.3) > absl::Now()) env.RandomlyPoke();
  threads.Stop();
}

TYPED_TEST_P(TransferCacheFuzzTest, MultiThreadedBiasedInsert) {
  const int batch_size = TypeParam::kBatchSize;

  TypeParam env;
  ThreadManager threads;
  threads.Start(10, [&](int) { env.RandomlyPoke(); });

  auto start = absl::Now();
  while (start + absl::Seconds(5) > absl::Now()) env.Insert(batch_size);
  threads.Stop();
}

TYPED_TEST_P(TransferCacheFuzzTest, MultiThreadedBiasedRemove) {
  const int batch_size = TypeParam::kBatchSize;

  TypeParam env;
  ThreadManager threads;
  threads.Start(10, [&](int) { env.RandomlyPoke(); });

  auto start = absl::Now();
  while (start + absl::Seconds(5) > absl::Now()) env.Remove(batch_size);
  threads.Stop();
}

TYPED_TEST_P(TransferCacheFuzzTest, MultiThreadedBiasedShrink) {
  TypeParam env;
  ThreadManager threads;
  threads.Start(10, [&](int) { env.RandomlyPoke(); });

  auto start = absl::Now();
  while (start + absl::Seconds(5) > absl::Now()) env.Shrink();
  threads.Stop();
}

TYPED_TEST_P(TransferCacheFuzzTest, MultiThreadedBiasedGrow) {
  TypeParam env;
  ThreadManager threads;
  threads.Start(10, [&](int) { env.RandomlyPoke(); });

  auto start = absl::Now();
  while (start + absl::Seconds(5) > absl::Now()) env.Grow();
  threads.Stop();
}

REGISTER_TYPED_TEST_SUITE_P(TransferCacheFuzzTest, MultiThreadedUnbiased,
                            MultiThreadedBiasedInsert,
                            MultiThreadedBiasedRemove, MultiThreadedBiasedGrow,
                            MultiThreadedBiasedShrink);

namespace unit_tests {
using LegacyEnv =
    FakeTransferCacheEnvironment<internal_transfer_cache::TransferCache<
        MockCentralFreeList, MockTransferCacheManager>>;

using LockFreeEnv =
    FakeTransferCacheEnvironment<internal_transfer_cache::LockFreeTransferCache<
        MockCentralFreeList, MockTransferCacheManager>>;

using TransferCacheTypes = ::testing::Types<LegacyEnv, LockFreeEnv>;
INSTANTIATE_TYPED_TEST_SUITE_P(TransferCacheTest, TransferCacheTest,
                               TransferCacheTypes);
}  // namespace unit_tests

namespace fuzz_tests {
// Use the FakeCentralFreeList instead of the MockCentralFreeList for fuzz tests
// as it avoids the overheads of mocks and allows more iterations of the fuzzing
// itself.
using LockFreeEnv =
    FakeTransferCacheEnvironment<internal_transfer_cache::LockFreeTransferCache<
        FakeCentralFreeList, MockTransferCacheManager>>;

using TransferCacheFuzzTypes = ::testing::Types<LockFreeEnv>;
INSTANTIATE_TYPED_TEST_SUITE_P(TransferCacheFuzzTest, TransferCacheFuzzTest,
                               TransferCacheFuzzTypes);
}  // namespace fuzz_tests

}  // namespace
}  // namespace tcmalloc
