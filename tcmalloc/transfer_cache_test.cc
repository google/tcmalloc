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
#include <random>
#include <thread>

#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "absl/base/internal/spinlock.h"
#include "absl/random/distributions.h"
#include "absl/random/random.h"
#include "absl/synchronization/blocking_counter.h"
#include "absl/time/clock.h"
#include "absl/types/span.h"
#include "tcmalloc/common.h"
#include "tcmalloc/mock_central_freelist.h"
#include "tcmalloc/mock_transfer_cache.h"
#include "tcmalloc/static_vars.h"
#include "tcmalloc/transfer_cache_internals.h"

namespace tcmalloc {
namespace {

using TransferCache =
    internal_transfer_cache::TransferCache<MockCentralFreeList,
                                           MockTransferCacheManager>;

using Env = FakeTransferCacheEnvironment<TransferCache>;

TEST(TransferCache, IsolatedSmoke) {
  const int batch_size = MockTransferCacheManager::num_objects_to_move(1);
  Env e;
  EXPECT_CALL(e.central_freelist(), InsertRange).Times(0);
  EXPECT_CALL(e.central_freelist(), RemoveRange).Times(0);
  e.Insert(batch_size);
  e.Insert(batch_size);
  e.Remove(batch_size);
  e.Remove(batch_size);
}

TEST(TransferCache, FetchesFromFreelist) {
  const int batch_size = MockTransferCacheManager::num_objects_to_move(1);
  Env e;
  EXPECT_CALL(e.central_freelist(), InsertRange).Times(0);
  EXPECT_CALL(e.central_freelist(), RemoveRange).Times(1);
  e.Remove(batch_size);
}

TEST(TransferCache, EvictsOtherCaches) {
  const int batch_size = MockTransferCacheManager::num_objects_to_move(1);
  Env e;

  EXPECT_CALL(e.transfer_cache_manager(), ShrinkCache).WillOnce([]() {
    return true;
  });
  EXPECT_CALL(e.central_freelist(), InsertRange).Times(0);

  for (int i = 0; i < TransferCache::kInitialCapacityInBatches; ++i) {
    e.Insert(batch_size);
  }
  e.Insert(batch_size);
}

TEST(TransferCache, PushesToFreelist) {
  const int batch_size = MockTransferCacheManager::num_objects_to_move(1);
  Env e;

  EXPECT_CALL(e.transfer_cache_manager(), ShrinkCache).WillOnce([]() {
    return false;
  });
  EXPECT_CALL(e.central_freelist(), InsertRange).Times(1);

  for (int i = 0; i < TransferCache::kInitialCapacityInBatches; ++i) {
    e.Insert(batch_size);
  }
  e.Insert(batch_size);
}

}  // namespace
}  // namespace tcmalloc
