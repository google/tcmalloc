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

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <random>
#include <thread>
#include <vector>

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
namespace tcmalloc_internal {
namespace {

template <typename Env>
using TransferCacheTest = ::testing::Test;
TYPED_TEST_SUITE_P(TransferCacheTest);

TYPED_TEST_P(TransferCacheTest, IsolatedSmoke) {
  const int batch_size = TypeParam::kBatchSize;
  TypeParam e;
  EXPECT_CALL(e.central_freelist(), InsertRange)
      .Times(e.transfer_cache().IsFlexible() ? 0 : 1);
  EXPECT_CALL(e.central_freelist(), RemoveRange)
      .Times(e.transfer_cache().IsFlexible() ? 0 : 1);

  TransferCacheStats stats = e.transfer_cache().GetStats();
  EXPECT_EQ(stats.insert_hits, 0);
  EXPECT_EQ(stats.insert_misses, 0);
  EXPECT_EQ(stats.insert_non_batch_misses, 0);
  EXPECT_EQ(stats.remove_hits, 0);
  EXPECT_EQ(stats.remove_misses, 0);
  EXPECT_EQ(stats.remove_non_batch_misses, 0);
  EXPECT_EQ(stats.used, 0);

  int capacity = e.transfer_cache().CapacityNeeded(kSizeClass).capacity;
  EXPECT_EQ(stats.capacity, capacity);
  EXPECT_EQ(stats.max_capacity, e.transfer_cache().max_capacity());

  e.Insert(batch_size);
  stats = e.transfer_cache().GetStats();
  EXPECT_EQ(stats.insert_hits, 1);
  int used_expected = batch_size;
  EXPECT_EQ(stats.used, used_expected);
  EXPECT_EQ(stats.capacity, capacity);

  e.Insert(batch_size);
  stats = e.transfer_cache().GetStats();
  EXPECT_EQ(stats.insert_hits, 2);
  used_expected += batch_size;
  EXPECT_EQ(stats.used, used_expected);
  EXPECT_EQ(stats.capacity, capacity);

  e.Insert(batch_size - 1);
  stats = e.transfer_cache().GetStats();
  if (e.transfer_cache().IsFlexible()) {
    EXPECT_EQ(stats.insert_hits, 3);
    EXPECT_EQ(stats.insert_misses, 0);
    EXPECT_EQ(stats.insert_non_batch_misses, 0);
    used_expected += batch_size - 1;
    EXPECT_EQ(stats.used, used_expected);
  } else {
    EXPECT_EQ(stats.insert_hits, 2);
    EXPECT_EQ(stats.insert_misses, 1);
    EXPECT_EQ(stats.insert_non_batch_misses, 1);
    EXPECT_EQ(stats.used, used_expected);
  }
  EXPECT_EQ(stats.capacity, capacity);
  EXPECT_LE(capacity, e.transfer_cache().max_capacity());

  e.Remove(batch_size);
  stats = e.transfer_cache().GetStats();
  EXPECT_EQ(stats.remove_hits, 1);
  used_expected -= batch_size;
  EXPECT_EQ(stats.used, used_expected);
  EXPECT_EQ(stats.capacity, capacity);

  e.Remove(batch_size);
  stats = e.transfer_cache().GetStats();
  EXPECT_EQ(stats.remove_hits, 2);
  used_expected -= batch_size;
  EXPECT_EQ(stats.used, used_expected);
  EXPECT_EQ(stats.capacity, capacity);

  e.Remove(batch_size - 1);
  stats = e.transfer_cache().GetStats();
  if (e.transfer_cache().IsFlexible()) {
    EXPECT_EQ(stats.remove_hits, 3);
    EXPECT_EQ(stats.remove_misses, 0);
    EXPECT_EQ(stats.remove_non_batch_misses, 0);
    used_expected -= (batch_size - 1);
    EXPECT_EQ(stats.used, used_expected);
  } else {
    EXPECT_EQ(stats.remove_hits, 2);
    EXPECT_EQ(stats.remove_misses, 1);
    EXPECT_EQ(stats.remove_non_batch_misses, 1);
    EXPECT_EQ(stats.used, used_expected);
  }
  EXPECT_EQ(stats.capacity, capacity);
  EXPECT_EQ(stats.max_capacity, e.transfer_cache().max_capacity());
}

TYPED_TEST_P(TransferCacheTest, ReadStats) {
  const int batch_size = TypeParam::kBatchSize;
  TypeParam e;
  EXPECT_CALL(e.central_freelist(), InsertRange).Times(0);
  EXPECT_CALL(e.central_freelist(), RemoveRange).Times(0);

  // Ensure there is at least one insert hit/remove hit, so we can assert a
  // non-tautology in t2.
  e.Insert(batch_size);
  e.Remove(batch_size);

  TransferCacheStats stats = e.transfer_cache().GetStats();
  ASSERT_EQ(stats.insert_hits, 1);
  ASSERT_EQ(stats.insert_misses, 0);
  ASSERT_EQ(stats.insert_non_batch_misses, 0);
  ASSERT_EQ(stats.remove_hits, 1);
  ASSERT_EQ(stats.remove_misses, 0);
  ASSERT_EQ(stats.remove_non_batch_misses, 0);

  std::atomic<bool> stop{false};

  std::thread t1([&]() {
    while (!stop.load(std::memory_order_acquire)) {
      e.Insert(batch_size);
      e.Remove(batch_size);
    }
  });

  std::thread t2([&]() {
    while (!stop.load(std::memory_order_acquire)) {
      TransferCacheStats stats = e.transfer_cache().GetStats();
      CHECK_CONDITION(stats.insert_hits >= 1);
      CHECK_CONDITION(stats.insert_misses == 0);
      CHECK_CONDITION(stats.insert_non_batch_misses == 0);
      CHECK_CONDITION(stats.remove_hits >= 1);
      CHECK_CONDITION(stats.remove_misses == 0);
      CHECK_CONDITION(stats.remove_non_batch_misses == 0);
    }
  });

  absl::SleepFor(absl::Seconds(1));
  stop.store(true, std::memory_order_release);

  t1.join();
  t2.join();
}

TYPED_TEST_P(TransferCacheTest, SingleItemSmoke) {
  const int batch_size = TypeParam::kBatchSize;
  if (batch_size == 1) {
    GTEST_SKIP() << "skipping trivial batch size";
  }
  TypeParam e;
  const int actions = e.transfer_cache().IsFlexible() ? 2 : 0;
  EXPECT_CALL(e.central_freelist(), InsertRange).Times(2 - actions);
  EXPECT_CALL(e.central_freelist(), RemoveRange).Times(2 - actions);

  e.Insert(1);
  e.Insert(1);
  EXPECT_EQ(e.transfer_cache().GetStats().insert_hits, actions);
  e.Remove(1);
  e.Remove(1);
  EXPECT_EQ(e.transfer_cache().GetStats().remove_hits, actions);
}

TYPED_TEST_P(TransferCacheTest, FetchesFromFreelist) {
  const int batch_size = TypeParam::kBatchSize;
  TypeParam e;
  EXPECT_CALL(e.central_freelist(), InsertRange).Times(0);
  EXPECT_CALL(e.central_freelist(), RemoveRange).Times(1);
  e.Remove(batch_size);
  EXPECT_EQ(e.transfer_cache().GetStats().remove_misses, 1);
}

TYPED_TEST_P(TransferCacheTest, PartialFetchFromFreelist) {
  const int batch_size = TypeParam::kBatchSize;
  TypeParam e;
  EXPECT_CALL(e.central_freelist(), InsertRange).Times(0);
  EXPECT_CALL(e.central_freelist(), RemoveRange)
      .Times(2)
      .WillOnce([&](void** batch, int n) {
        int returned = static_cast<FakeCentralFreeList&>(e.central_freelist())
                           .RemoveRange(batch, std::min(batch_size / 2, n));
        // Overwrite the elements of batch that were not populated by
        // RemoveRange.
        memset(batch + returned, 0x3f, sizeof(*batch) * (n - returned));
        return returned;
      });
  e.Remove(batch_size);
  EXPECT_EQ(e.transfer_cache().GetStats().remove_misses, 2);
}

TYPED_TEST_P(TransferCacheTest, EvictsOtherCaches) {
  const int batch_size = TypeParam::kBatchSize;
  TypeParam e;

  EXPECT_CALL(e.transfer_cache_manager(), ShrinkCache).WillOnce([]() {
    return true;
  });
  EXPECT_CALL(e.central_freelist(), InsertRange).Times(0);

  while (e.transfer_cache().HasSpareCapacity(kSizeClass)) {
    e.Insert(batch_size);
  }
  size_t old_hits = e.transfer_cache().GetStats().insert_hits;
  e.Insert(batch_size);
  EXPECT_EQ(e.transfer_cache().GetStats().insert_hits, old_hits + 1);
  EXPECT_EQ(e.transfer_cache().GetStats().insert_misses, 0);
}

TYPED_TEST_P(TransferCacheTest, EvictsOtherCachesFlex) {
  const int batch_size = TypeParam::kBatchSize;
  TypeParam e;

  EXPECT_CALL(e.transfer_cache_manager(), ShrinkCache).WillRepeatedly([]() {
    return true;
  });
  if (e.transfer_cache().IsFlexible()) {
    EXPECT_CALL(e.central_freelist(), InsertRange).Times(0);
  } else {
    EXPECT_CALL(e.central_freelist(), InsertRange).Times(batch_size - 1);
  }
  EXPECT_EQ(e.transfer_cache().GetStats().insert_hits, 0);
  EXPECT_EQ(e.transfer_cache().GetStats().insert_misses, 0);

  int total = 0;
  for (int i = 1; i <= batch_size; i++) {
    e.Insert(i);
    total += i;
  }

  if (e.transfer_cache().IsFlexible()) {
    EXPECT_EQ(e.transfer_cache().tc_length(), total);
    EXPECT_EQ(e.transfer_cache().GetStats().insert_hits, batch_size);
    EXPECT_EQ(e.transfer_cache().GetStats().insert_misses, 0);
  } else {
    EXPECT_EQ(e.transfer_cache().tc_length(), 1 * batch_size);
    EXPECT_EQ(e.transfer_cache().GetStats().insert_hits, 1);
    EXPECT_EQ(e.transfer_cache().GetStats().insert_misses, batch_size - 1);
  }
}

// Similar to EvictsOtherCachesFlex, but with full cache.
TYPED_TEST_P(TransferCacheTest, FullCacheFlex) {
  const int batch_size = TypeParam::kBatchSize;
  TypeParam e;

  EXPECT_CALL(e.transfer_cache_manager(), ShrinkCache).WillRepeatedly([]() {
    return true;
  });
  if (e.transfer_cache().IsFlexible()) {
    EXPECT_CALL(e.central_freelist(), InsertRange).Times(0);
  } else {
    EXPECT_CALL(e.central_freelist(), InsertRange)
        .Times(testing::AtLeast(batch_size));
  }

  while (e.transfer_cache().HasSpareCapacity(kSizeClass)) {
    e.Insert(batch_size);
  }
  for (int i = 1; i < batch_size + 2; i++) {
    e.Insert(i);
  }
}

TYPED_TEST_P(TransferCacheTest, PushesToFreelist) {
  const int batch_size = TypeParam::kBatchSize;
  TypeParam e;

  EXPECT_CALL(e.transfer_cache_manager(), ShrinkCache).WillOnce([]() {
    return false;
  });
  EXPECT_CALL(e.central_freelist(), InsertRange).Times(1);

  while (e.transfer_cache().HasSpareCapacity(kSizeClass)) {
    e.Insert(batch_size);
  }
  size_t old_hits = e.transfer_cache().GetStats().insert_hits;
  e.Insert(batch_size);
  EXPECT_EQ(e.transfer_cache().GetStats().insert_hits, old_hits);
  EXPECT_EQ(e.transfer_cache().GetStats().insert_misses, 1);
}

TYPED_TEST_P(TransferCacheTest, WrappingWorks) {
  const int batch_size = TypeParam::kBatchSize;

  TypeParam env;
  EXPECT_CALL(env.transfer_cache_manager(), ShrinkCache).Times(0);

  while (env.transfer_cache().HasSpareCapacity(kSizeClass)) {
    env.Insert(batch_size);
  }
  for (int i = 0; i < 100; ++i) {
    env.Remove(batch_size);
    env.Insert(batch_size);
  }
}

TYPED_TEST_P(TransferCacheTest, WrappingFlex) {
  const int batch_size = TypeParam::kBatchSize;

  TypeParam env;
  EXPECT_CALL(env.transfer_cache_manager(), ShrinkCache).Times(0);
  if (env.transfer_cache().IsFlexible()) {
    EXPECT_CALL(env.central_freelist(), InsertRange).Times(0);
    EXPECT_CALL(env.central_freelist(), RemoveRange).Times(0);
  }

  while (env.transfer_cache().HasSpareCapacity(kSizeClass)) {
    env.Insert(batch_size);
  }
  for (int i = 0; i < 100; ++i) {
    for (size_t size = 1; size < batch_size + 2; size++) {
      env.Remove(size);
      env.Insert(size);
    }
  }
}

TYPED_TEST_P(TransferCacheTest, Plunder) {
  TypeParam env;
  if (!std::is_same<typename TypeParam::TransferCache,
                    internal_transfer_cache::RingBufferTransferCache<
                        typename TypeParam::FreeList,
                        typename TypeParam::Manager>>::value) {
    GTEST_SKIP() << "Skipping ring-specific test.";
  }

  // Fill in some elements.
  env.Insert(TypeParam::kBatchSize);
  env.Insert(TypeParam::kBatchSize);
  ASSERT_EQ(env.transfer_cache().tc_length(), 2 * TypeParam::kBatchSize);
  // Previously the transfer cache was empty, so we're not plundering yet.
  env.transfer_cache().TryPlunder(kSizeClass);
  ASSERT_EQ(env.transfer_cache().tc_length(), 2 * TypeParam::kBatchSize);
  // But now all these elements will be plundered.
  env.transfer_cache().TryPlunder(kSizeClass);
  ASSERT_EQ(env.transfer_cache().tc_length(), 0);

  // Stock up again.
  env.Insert(TypeParam::kBatchSize);
  env.Insert(TypeParam::kBatchSize);
  ASSERT_EQ(env.transfer_cache().tc_length(), 2 * TypeParam::kBatchSize);
  // Plundering doesn't do anything as the cache was empty after the last
  // plunder call.
  env.transfer_cache().TryPlunder(kSizeClass);
  ASSERT_EQ(env.transfer_cache().tc_length(), 2 * TypeParam::kBatchSize);

  void* buf[TypeParam::kBatchSize];
  // -1 +1, this sets the low_water_mark (the lowest end-state after a
  // call to RemoveRange to 1 batch.
  (void)env.transfer_cache().RemoveRange(kSizeClass, buf,
                                         TypeParam::kBatchSize);
  ASSERT_EQ(env.transfer_cache().tc_length(), TypeParam::kBatchSize);
  env.transfer_cache().InsertRange(kSizeClass, {buf, TypeParam::kBatchSize});
  ASSERT_EQ(env.transfer_cache().tc_length(), 2 * TypeParam::kBatchSize);
  // We have one batch, and this is the same as the low water mark, so nothing
  // gets plundered.
  env.transfer_cache().TryPlunder(kSizeClass);
  ASSERT_EQ(env.transfer_cache().tc_length(), TypeParam::kBatchSize);
  // If we plunder immediately the low_water_mark is at the current size
  // gets plundered.
  env.transfer_cache().TryPlunder(kSizeClass);
  ASSERT_EQ(env.transfer_cache().tc_length(), 0);

  // Fill it up completely.
  while (env.transfer_cache().tc_length() <
         env.transfer_cache().max_capacity()) {
    env.Insert(TypeParam::kBatchSize);
  }
  // low water mark should still be zero, so no plundering.
  env.transfer_cache().TryPlunder(kSizeClass);
  ASSERT_EQ(env.transfer_cache().tc_length(),
            env.transfer_cache().max_capacity());

  // Inserting a one-element batch means we return one batch to the freelist and
  // then insert only one element after. I.e. the cache size should shrink.
  env.Insert(1);
  ASSERT_EQ(env.transfer_cache().tc_length(),
            env.transfer_cache().max_capacity() - TypeParam::kBatchSize + 1);
  // If we fill up the cache again, plundering should respect that.
  env.Insert(TypeParam::kBatchSize - 1);
  ASSERT_EQ(env.transfer_cache().tc_length(),
            env.transfer_cache().max_capacity());
  env.transfer_cache().TryPlunder(kSizeClass);
  ASSERT_EQ(env.transfer_cache().tc_length(), TypeParam::kBatchSize - 1);
}

// PickCoprimeBatchSize picks a batch size in [2, max_batch_size) that is
// coprime with 2^32.  We choose the largest possible batch size within that
// constraint to minimize the number of iterations of insert/remove required.
static size_t PickCoprimeBatchSize(size_t max_batch_size) {
  while (max_batch_size > 1) {
    if ((size_t{1} << 32) % max_batch_size != 0) {
      return max_batch_size;
    }
    max_batch_size--;
  }

  return max_batch_size;
}

TEST(RingBufferTest, b172283201) {
  // This test is designed to exercise the wraparound behavior for the
  // RingBufferTransferCache, which manages its indices in uint32_t's.  Because
  // it uses a non-standard batch size (kBatchSize) as part of
  // PickCoprimeBatchSize, it triggers a TransferCache miss to the
  // CentralFreeList, which is uninteresting for exercising b/172283201.

  // For performance reasons, limit to optimized builds.
#if !defined(NDEBUG)
  GTEST_SKIP() << "skipping long running test on debug build";
#elif defined(THREAD_SANITIZER)
  // This test is single threaded, so thread sanitizer will not be useful.
  GTEST_SKIP() << "skipping under thread sanitizer, which slows test execution";
#endif

  using EnvType = FakeTransferCacheEnvironment<
      internal_transfer_cache::RingBufferTransferCache<
          MockCentralFreeList, MockTransferCacheManager>>;
  EnvType env;

  // We pick the largest value <= EnvType::kBatchSize to use as a batch size,
  // such that it is prime relative to 2^32.  This ensures that when we
  // encounter a wraparound, the last operation actually spans both ends of the
  // buffer.
  const size_t batch_size = PickCoprimeBatchSize(EnvType::kBatchSize);
  ASSERT_GT(batch_size, 0);
  ASSERT_NE((size_t{1} << 32) % batch_size, 0) << batch_size;
  // For ease of comparison, allocate a buffer of char's.  We will use these to
  // generate unique addresses.  Since we assert that we will never miss in the
  // TransferCache and go to the CentralFreeList, these do not need to be valid
  // objects for deallocation.
  std::vector<char> buffer(batch_size);
  std::vector<void*> pointers;
  pointers.reserve(batch_size);
  for (size_t i = 0; i < batch_size; i++) {
    pointers.push_back(&buffer[i]);
  }

  // To produce wraparound in the RingBufferTransferCache, we fill up the cache
  // completely and then keep inserting new elements. This makes the cache
  // return old elements to the freelist and eventually wrap around.
  EXPECT_CALL(env.central_freelist(), RemoveRange).Times(0);
  // We do return items to the freelist, don't try to actually free them.
  ON_CALL(env.central_freelist(), InsertRange).WillByDefault(testing::Return());
  ON_CALL(env.transfer_cache_manager(), DetermineSizeClassToEvict)
      .WillByDefault(testing::Return(kSizeClass));

  // First fill up the cache to its capacity.

  while (env.transfer_cache().HasSpareCapacity(kSizeClass) ||
         env.transfer_cache().GrowCache(kSizeClass)) {
    env.transfer_cache().InsertRange(kSizeClass, absl::MakeSpan(pointers));
  }

  // The current size of the transfer cache is close to its capacity. Insert
  // enough batches to make sure we wrap around twice (1 batch size should wrap
  // around as we are full currently, then insert the same amount of items
  // again, then one more wrap around).
  const size_t kObjects = env.transfer_cache().tc_length() + 2 * batch_size;

  // From now on, calls to InsertRange() should result in a corresponding call
  // to the freelist whenever the cache is full. This doesn't happen on every
  // call, as we return up to num_to_move (i.e. kBatchSize) items to the free
  // list in one batch.
  EXPECT_CALL(env.central_freelist(),
              InsertRange(testing::SizeIs(EnvType::kBatchSize)))
      .Times(testing::AnyNumber());
  for (size_t i = 0; i < kObjects; i += batch_size) {
    env.transfer_cache().InsertRange(kSizeClass, absl::MakeSpan(pointers));
  }
  // Manually drain the items in the transfercache, otherwise the destructor
  // will try to free them.
  std::vector<void*> to_free(batch_size);
  size_t N = env.transfer_cache().tc_length();
  while (N > 0) {
    const size_t to_remove = std::min(N, batch_size);
    const size_t removed =
        env.transfer_cache().RemoveRange(kSizeClass, to_free.data(), to_remove);
    ASSERT_THAT(removed, testing::Le(to_remove));
    ASSERT_THAT(removed, testing::Gt(0));
    N -= removed;
  }
  ASSERT_EQ(env.transfer_cache().tc_length(), 0);
}

REGISTER_TYPED_TEST_SUITE_P(TransferCacheTest, IsolatedSmoke, ReadStats,
                            FetchesFromFreelist, PartialFetchFromFreelist,
                            EvictsOtherCaches, PushesToFreelist, WrappingWorks,
                            SingleItemSmoke, EvictsOtherCachesFlex,
                            FullCacheFlex, WrappingFlex, Plunder);
template <typename Env>
using FuzzTest = ::testing::Test;
TYPED_TEST_SUITE_P(FuzzTest);

TYPED_TEST_P(FuzzTest, MultiThreadedUnbiased) {
  TypeParam env;
  ThreadManager threads;
  threads.Start(10, [&](int) { env.RandomlyPoke(); });

  auto start = absl::Now();
  while (start + absl::Seconds(0.3) > absl::Now()) env.RandomlyPoke();
  threads.Stop();
}

TYPED_TEST_P(FuzzTest, MultiThreadedBiasedInsert) {
  const int batch_size = TypeParam::kBatchSize;

  TypeParam env;
  ThreadManager threads;
  threads.Start(10, [&](int) { env.RandomlyPoke(); });

  auto start = absl::Now();
  while (start + absl::Seconds(5) > absl::Now()) env.Insert(batch_size);
  threads.Stop();
}

TYPED_TEST_P(FuzzTest, MultiThreadedBiasedRemove) {
  const int batch_size = TypeParam::kBatchSize;

  TypeParam env;
  ThreadManager threads;
  threads.Start(10, [&](int) { env.RandomlyPoke(); });

  auto start = absl::Now();
  while (start + absl::Seconds(5) > absl::Now()) env.Remove(batch_size);
  threads.Stop();
}

TYPED_TEST_P(FuzzTest, MultiThreadedBiasedShrink) {
  TypeParam env;
  ThreadManager threads;
  threads.Start(10, [&](int) { env.RandomlyPoke(); });

  auto start = absl::Now();
  while (start + absl::Seconds(5) > absl::Now()) env.Shrink();
  threads.Stop();
}

TYPED_TEST_P(FuzzTest, MultiThreadedBiasedGrow) {
  TypeParam env;
  ThreadManager threads;
  threads.Start(10, [&](int) { env.RandomlyPoke(); });

  auto start = absl::Now();
  while (start + absl::Seconds(5) > absl::Now()) env.Grow();
  threads.Stop();
}

REGISTER_TYPED_TEST_SUITE_P(FuzzTest, MultiThreadedUnbiased,
                            MultiThreadedBiasedInsert,
                            MultiThreadedBiasedRemove, MultiThreadedBiasedGrow,
                            MultiThreadedBiasedShrink);

TEST(ShardedTransferCacheManagerTest, DefaultConstructorDisables) {
  ShardedTransferCacheManager manager(nullptr, nullptr);
  for (int size_class = 0; size_class < kNumClasses; ++size_class) {
    EXPECT_FALSE(manager.should_use(size_class));
  }
}

TEST(ShardedTransferCacheManagerTest, ShardsOnDemand) {
  FakeShardedTransferCacheEnvironment env;
  FakeShardedTransferCacheEnvironment::ShardedManager& manager =
      env.sharded_manager();

  EXPECT_FALSE(manager.shard_initialized(0));
  EXPECT_FALSE(manager.shard_initialized(1));

  size_t metadata = env.MetadataAllocated();
  // We already allocated some data for the sharded transfer cache.
  EXPECT_GT(metadata, 0);

  // Push something onto cpu 0/shard0.
  {
    void* ptr;
    env.central_freelist().AllocateBatch(&ptr, 1);
    env.SetCurrentCpu(0);
    manager.Push(kSizeClass, ptr);
    EXPECT_TRUE(manager.shard_initialized(0));
    EXPECT_EQ(manager.tc_length(0, kSizeClass), 1);
    EXPECT_FALSE(manager.shard_initialized(1));
    EXPECT_GT(env.MetadataAllocated(), metadata);
    metadata = env.MetadataAllocated();
  }

  // Popping again should work, but not deinitialize the shard.
  {
    void* ptr = manager.Pop(kSizeClass);
    ASSERT_NE(ptr, nullptr);
    env.central_freelist().FreeBatch({&ptr, 1});
    EXPECT_TRUE(manager.shard_initialized(0));
    EXPECT_EQ(manager.tc_length(0, kSizeClass), 0);
    EXPECT_FALSE(manager.shard_initialized(1));
    EXPECT_EQ(env.MetadataAllocated(), metadata);
  }

  // Push something onto cpu 1, also shard 0.
  {
    void* ptr;
    env.central_freelist().AllocateBatch(&ptr, 1);
    env.SetCurrentCpu(1);
    manager.Push(kSizeClass, ptr);
    EXPECT_TRUE(manager.shard_initialized(0));
    EXPECT_EQ(manager.tc_length(1, kSizeClass), 1);
    EXPECT_FALSE(manager.shard_initialized(1));
    // No new metadata allocated
    EXPECT_EQ(env.MetadataAllocated(), metadata);
  }

  // Push something onto cpu 2/shard 1.
  {
    void* ptr;
    env.central_freelist().AllocateBatch(&ptr, 1);
    env.SetCurrentCpu(2);
    manager.Push(kSizeClass, ptr);
    EXPECT_TRUE(manager.shard_initialized(0));
    EXPECT_TRUE(manager.shard_initialized(1));
    EXPECT_EQ(manager.tc_length(2, kSizeClass), 1);
    EXPECT_GT(env.MetadataAllocated(), metadata);
  }
}

namespace unit_tests {
using Env = FakeTransferCacheEnvironment<internal_transfer_cache::TransferCache<
    MockCentralFreeList, MockTransferCacheManager>>;
INSTANTIATE_TYPED_TEST_SUITE_P(TransferCache, TransferCacheTest,
                               ::testing::Types<Env>);

using RingBufferEnv = FakeTransferCacheEnvironment<
    internal_transfer_cache::RingBufferTransferCache<MockCentralFreeList,
                                                     MockTransferCacheManager>>;
INSTANTIATE_TYPED_TEST_SUITE_P(RingBuffer, TransferCacheTest,
                               ::testing::Types<RingBufferEnv>);

}  // namespace unit_tests

namespace fuzz_tests {
// Use the FakeCentralFreeList instead of the MockCentralFreeList for fuzz tests
// as it avoids the overheads of mocks and allows more iterations of the fuzzing
// itself.
using Env = FakeTransferCacheEnvironment<internal_transfer_cache::TransferCache<
    MockCentralFreeList, MockTransferCacheManager>>;
INSTANTIATE_TYPED_TEST_SUITE_P(TransferCache, FuzzTest, ::testing::Types<Env>);

using RingBufferEnv = FakeTransferCacheEnvironment<
    internal_transfer_cache::RingBufferTransferCache<MockCentralFreeList,
                                                     MockTransferCacheManager>>;
INSTANTIATE_TYPED_TEST_SUITE_P(RingBuffer, FuzzTest,
                               ::testing::Types<RingBufferEnv>);
}  // namespace fuzz_tests

namespace leak_tests {

template <typename Env>
using TwoSizeClassTest = ::testing::Test;
TYPED_TEST_SUITE_P(TwoSizeClassTest);

TYPED_TEST_P(TwoSizeClassTest, NoLeaks) {
  TypeParam env;

  // The point of this test is to see that adding "random" amounts of
  // allocations to the transfer caches behaves correctly, even in the case that
  // there are multiple size classes interacting by stealing from each other.

  // Fill all caches to their maximum without starting to steal from each other.
  for (int size_class = 1; size_class < TypeParam::Manager::kSizeClasses;
       ++size_class) {
    const size_t batch_size =
        TypeParam::Manager::num_objects_to_move(size_class);
    while (env.transfer_cache_manager().HasSpareCapacity(size_class)) {
      env.Insert(size_class, batch_size);
    }
  }

  // Count the number of batches currently in the cache.
  auto count_batches = [&env]() {
    int batch_count = 0;
    for (int size_class = 1; size_class < TypeParam::Manager::kSizeClasses;
         ++size_class) {
      const size_t batch_size =
          TypeParam::Manager::num_objects_to_move(size_class);
      batch_count +=
          env.transfer_cache_manager().tc_length(size_class) / batch_size;
    }
    return batch_count;
  };

  absl::BitGen bitgen;
  const int max_batches = count_batches();
  int expected_batches = max_batches;
  for (int i = 0; i < 100; ++i) {
    {
      // First remove.
      const int size_class =
          absl::Uniform<int>(bitgen, 1, TypeParam::Manager::kSizeClasses);
      const size_t batch_size =
          TypeParam::Manager::num_objects_to_move(size_class);
      if (env.transfer_cache_manager().tc_length(size_class) >= batch_size) {
        env.Remove(size_class, batch_size);
        --expected_batches;
      }
      const int current_batches = count_batches();
      EXPECT_EQ(current_batches, expected_batches) << "iteration " << i;
    }
    {
      // Then add in another size class.
      const int size_class =
          absl::Uniform<int>(bitgen, 1, TypeParam::Manager::kSizeClasses);
      // Evict from the "next" size class, skipping 0.
      // This makes sure we are always evicting from somewhere if at all
      // possible.
      env.transfer_cache_manager().evicting_from_ =
          1 + size_class % (TypeParam::Manager::kSizeClasses - 1);
      if (expected_batches < max_batches) {
        const size_t batch_size =
            TypeParam::Manager::num_objects_to_move(size_class);
        env.Insert(size_class, batch_size);
        ++expected_batches;
      }
      const int current_batches = count_batches();
      EXPECT_EQ(current_batches, expected_batches) << "iteration " << i;
    }
  }
}

REGISTER_TYPED_TEST_SUITE_P(TwoSizeClassTest, NoLeaks);

using TwoTransferCacheEnv =
    TwoSizeClassEnv<internal_transfer_cache::TransferCache>;
INSTANTIATE_TYPED_TEST_SUITE_P(TransferCache, TwoSizeClassTest,
                               ::testing::Types<TwoTransferCacheEnv>);

using TwoRingBufferEnv =
    TwoSizeClassEnv<internal_transfer_cache::RingBufferTransferCache>;
INSTANTIATE_TYPED_TEST_SUITE_P(RingBuffer, TwoSizeClassTest,
                               ::testing::Types<TwoRingBufferEnv>);

}  // namespace leak_tests

namespace resize_tests {

template <typename Env>
using RealTransferCacheTest = ::testing::Test;
TYPED_TEST_SUITE_P(RealTransferCacheTest);

TYPED_TEST_P(RealTransferCacheTest, ResizeOccurs) {
  TypeParam env;

  // Enable background resizing of transfer caches.
  env.transfer_cache_manager().SetResizeCachesInBackground(true);
  constexpr int kSizeClass = 1;

  TransferCacheStats stats = env.transfer_cache_manager().GetStats(kSizeClass);
  EXPECT_EQ(stats.insert_misses, 0);
  EXPECT_EQ(stats.insert_non_batch_misses, 0);
  EXPECT_EQ(stats.remove_misses, 0);
  EXPECT_EQ(stats.remove_non_batch_misses, 0);

  const int initial_capacity = stats.capacity;

  // Count capacity (measured in batches) currently allowed in the cache.
  auto count_batches = [&env]() {
    int batch_count = 0;
    for (int size_class = 0; size_class < kNumClasses; ++size_class) {
      const size_t batch_size =
          env.transfer_cache_manager().num_objects_to_move(size_class);
      const int capacity =
          env.transfer_cache_manager().GetStats(size_class).capacity;
      batch_count += batch_size > 0 ? capacity / batch_size : 0;
    }
    return batch_count;
  };

  const int total_capacity = count_batches();

  const size_t batch_size =
      env.transfer_cache_manager().num_objects_to_move(kSizeClass);

  while (env.transfer_cache_manager().HasSpareCapacity(kSizeClass)) {
    env.Insert(kSizeClass, batch_size);
  }
  stats = env.transfer_cache_manager().GetStats(kSizeClass);
  EXPECT_EQ(stats.insert_misses, 0);
  EXPECT_EQ(stats.insert_non_batch_misses, 0);
  EXPECT_EQ(stats.remove_misses, 0);
  EXPECT_EQ(stats.remove_non_batch_misses, 0);

  // Try resizing caches.
  env.transfer_cache_manager().TryResizingCaches();
  stats = env.transfer_cache_manager().GetStats(kSizeClass);
  // As the number of misses encountered yet is zero, capacity shouldn't have
  // grown.
  EXPECT_EQ(stats.capacity, initial_capacity);
  // Make sure we did not lose the overall capacity. The total capacity in
  // batches should be the same as before.
  EXPECT_EQ(count_batches(), total_capacity);

  // Try inserting a batch to make sure that the caches are not resized and that
  // we encounter a miss.
  env.Insert(kSizeClass, batch_size);
  stats = env.transfer_cache_manager().GetStats(kSizeClass);
  EXPECT_EQ(stats.insert_misses, 1);
  EXPECT_EQ(stats.insert_non_batch_misses, 0);
  EXPECT_EQ(stats.remove_misses, 0);
  EXPECT_EQ(stats.remove_non_batch_misses, 0);
  EXPECT_EQ(stats.capacity, initial_capacity);

  // Try resizing caches again.
  env.transfer_cache_manager().TryResizingCaches();
  stats = env.transfer_cache_manager().GetStats(kSizeClass);
  // Make sure that the capacity has grown by a batch size.
  EXPECT_EQ(stats.capacity, initial_capacity + batch_size);
  // Make sure we did not lose the overall capacity. The total capacity in
  // batches should be the same as before.
  EXPECT_EQ(count_batches(), total_capacity);

  env.Insert(kSizeClass, batch_size);
  stats = env.transfer_cache_manager().GetStats(kSizeClass);
  // Capacity grew during the last resize operation. So, we shouldn't have
  // encountered a miss here.
  EXPECT_EQ(stats.insert_misses, 1);
  EXPECT_EQ(stats.insert_non_batch_misses, 0);
  EXPECT_EQ(stats.remove_misses, 0);
  EXPECT_EQ(stats.remove_non_batch_misses, 0);
  EXPECT_EQ(stats.capacity, initial_capacity + batch_size);
}

TYPED_TEST_P(RealTransferCacheTest, ResizeMaxSizeClasses) {
  // The point of this test is to make sure that we resize up to
  // kMaxSizeClassesToResize number of transfer caches. Specifically, we want to
  // check that, even if we have some caches that have been resized to
  // their maximum capacity and have suffered higher misses during this resize
  // interval, we continue to grow up to kMaxSizeClassesToResize caches even if
  // it means growing caches that might have suffered fewer misses.

  TypeParam env;

  // Enable background resizing of transfer caches.
  env.transfer_cache_manager().SetResizeCachesInBackground(true);

  // First, we resize kMaxSizeClassesToResize transfer caches to their maximum
  // capacity. We keep inserting objects to these transfer caches until none of
  // the caches may further be resized.
  bool done = false;
  while (!done) {
    done = true;
    for (int size_class = 1;
         size_class <= TypeParam::Manager::kMaxSizeClassesToResize;
         ++size_class) {
      if (env.transfer_cache_manager().CanIncreaseCapacity(size_class) ||
          env.transfer_cache_manager().HasSpareCapacity(size_class)) {
        const size_t batch_size =
            env.transfer_cache_manager().num_objects_to_move(size_class);
        env.Insert(size_class, batch_size);
        done = false;
      }
    }
    env.transfer_cache_manager().TryResizingCaches();
  }

  // Collect the stats to check that we have indeed grown capacity of
  // kMaxSizeClassesToResize transfer caches to their maximum capacity.
  for (int size_class = 1;
       size_class <= TypeParam::Manager::kMaxSizeClassesToResize;
       ++size_class) {
    TransferCacheStats stats =
        env.transfer_cache_manager().GetStats(size_class);
    EXPECT_EQ(stats.capacity, stats.max_capacity);
  }

  // Insert objects to the previous transfer caches. We perform two insert
  // operations so that they incur two additional misses.
  for (int size_class = 1;
       size_class <= TypeParam::Manager::kMaxSizeClassesToResize;
       ++size_class) {
    TransferCacheStats stats =
        env.transfer_cache_manager().GetStats(size_class);
    const int misses = stats.insert_misses;
    const size_t batch_size =
        env.transfer_cache_manager().num_objects_to_move(size_class);
    env.Insert(size_class, batch_size);
    env.Insert(size_class, batch_size);
    stats = env.transfer_cache_manager().GetStats(size_class);
    EXPECT_EQ(stats.insert_misses, misses + 2);
  }

  // We insert objects for the size class kMaxSizeClassesToResize + 1.
  // Eventually, we want to make sure that we can resize this transfer cache
  // even though other tranfer caches (which were resized to their maximum
  // capacity) suffered more misses.
  const int class_to_resize = TypeParam::Manager::kMaxSizeClassesToResize + 1;
  const size_t batch_size =
      env.transfer_cache_manager().num_objects_to_move(class_to_resize);
  TransferCacheStats stats =
      env.transfer_cache_manager().GetStats(class_to_resize);
  const int initial_capacity = stats.capacity;
  while (env.transfer_cache_manager().HasSpareCapacity(class_to_resize)) {
    env.Insert(class_to_resize, batch_size);
  }

  // Number of misses should be zero.
  stats = env.transfer_cache_manager().GetStats(class_to_resize);
  EXPECT_EQ(stats.insert_misses, 0);
  EXPECT_EQ(stats.insert_non_batch_misses, 0);
  EXPECT_EQ(stats.remove_misses, 0);
  EXPECT_EQ(stats.remove_non_batch_misses, 0);

  // Insert a single batch_size number of objects for the size class we want to
  // resize.
  env.Insert(class_to_resize, batch_size);

  // We should have incurred one insert miss.
  stats = env.transfer_cache_manager().GetStats(class_to_resize);
  EXPECT_EQ(stats.insert_misses, 1);
  EXPECT_EQ(stats.insert_non_batch_misses, 0);
  EXPECT_EQ(stats.remove_misses, 0);
  EXPECT_EQ(stats.remove_non_batch_misses, 0);
  EXPECT_EQ(stats.capacity, initial_capacity);

  // Try resizing caches again. Transfer caches [1, kMaxSizeClassesToResize +
  // 1) were grown to their full capacity; so they can not be further resized
  // even though they incurred more misses. Our resize mechanism should continue
  // to look for caches that suffered misses during the interval and grow them.
  env.transfer_cache_manager().TryResizingCaches();
  stats = env.transfer_cache_manager().GetStats(class_to_resize);
  // Make sure that the capacity has grown by a batch size.
  EXPECT_EQ(stats.capacity, initial_capacity + batch_size);
}

TYPED_TEST_P(RealTransferCacheTest, StressResize) {
  TypeParam env;

  // Enable background resizing of transfer caches.
  env.transfer_cache_manager().SetResizeCachesInBackground(true);

  // Count capacity (measured in batches) currently allowed in the cache.
  auto count_batches = [&env]() {
    int batch_count = 0;
    for (int size_class = 0; size_class < kNumClasses; ++size_class) {
      const size_t batch_size =
          env.transfer_cache_manager().num_objects_to_move(size_class);
      const int capacity =
          env.transfer_cache_manager().GetStats(size_class).capacity;
      batch_count += batch_size > 0 ? capacity / batch_size : 0;
    }
    return batch_count;
  };

  const int total_capacity = count_batches();

  ThreadManager threads;
  threads.Start(5, [&](int) { env.RandomlyPoke(); });

  auto start = absl::Now();
  while (start + absl::Seconds(5) > absl::Now()) {
    env.transfer_cache_manager().TryResizingCaches();
    absl::SleepFor(absl::Milliseconds(10));
  }
  threads.Stop();

  EXPECT_EQ(count_batches(), total_capacity);
}

REGISTER_TYPED_TEST_SUITE_P(RealTransferCacheTest, ResizeOccurs,
                            ResizeMaxSizeClasses, StressResize);

using TransferCacheRealEnv = MultiSizeClassTransferCacheEnvironment<
    internal_transfer_cache::TransferCache<CentralFreeList,
                                           FakeMultiClassTransferCacheManager>>;
INSTANTIATE_TYPED_TEST_SUITE_P(TransferCache, RealTransferCacheTest,
                               ::testing::Types<TransferCacheRealEnv>);

using RingTransferCacheRealEnv = MultiSizeClassTransferCacheEnvironment<
    internal_transfer_cache::RingBufferTransferCache<
        CentralFreeList, FakeMultiClassRingBufferManager>>;
INSTANTIATE_TYPED_TEST_SUITE_P(RingBuffer, RealTransferCacheTest,
                               ::testing::Types<RingTransferCacheRealEnv>);

}  // namespace resize_tests

}  // namespace
}  // namespace tcmalloc_internal
}  // namespace tcmalloc
