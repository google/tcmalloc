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

#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "absl/base/internal/spinlock.h"
#include "absl/types/span.h"
#include "tcmalloc/common.h"
#include "tcmalloc/mock_central_freelist.h"
#include "tcmalloc/static_vars.h"

namespace tcmalloc {
namespace {
class MockTransferCacheManager {
 public:
  MOCK_METHOD(int, DetermineSizeClassToEvict, ());
  MOCK_METHOD(bool, ShrinkCache, (int size_class));

  static size_t class_to_size(int size_class) { return 8; }
  static size_t num_objects_to_move(int size_class) { return 32; }
  void* Alloc(size_t size) {
    memory_.emplace_back(malloc(size));
    return memory_.back().get();
  }
  struct Free {
    void operator()(void* b) { free(b); }
  };

  std::vector<std::unique_ptr<void, Free>> memory_;
};

using TransferCache =
    internal_transfer_cache::TransferCache<MockCentralFreeList,
                                           MockTransferCacheManager>;

struct Batch {
  Batch() {
    for (int i = 0; i < kMaxObjectsToMove; ++i) {
      objs[i] = &(objs[i]);
    }
  }
  void* objs[kMaxObjectsToMove];
};

TEST(TransferCache, IsolatedSmoke) {
  const int batch_size = MockTransferCacheManager::num_objects_to_move(1);
  MockTransferCacheManager manager;
  TransferCache cache(&manager);
  cache.Init(1);
  Batch in, out;
  cache.InsertRange(absl::MakeSpan(in.objs, batch_size), batch_size);
  cache.InsertRange(absl::MakeSpan(in.objs + batch_size, batch_size),
                    batch_size);
  ASSERT_EQ(cache.RemoveRange(out.objs, batch_size), batch_size);
  ASSERT_EQ(cache.RemoveRange(out.objs + batch_size, batch_size), batch_size);
  for (int i = 0; i < batch_size; ++i) {
    EXPECT_NE(out.objs[i], out.objs[i + batch_size]);
    EXPECT_EQ(in.objs[i], out.objs[i + batch_size]);
    EXPECT_EQ(in.objs[i + batch_size], out.objs[i]);
  }
}

TEST(TransferCache, FetchesFromFreelist) {
  const int batch_size = MockTransferCacheManager::num_objects_to_move(1);
  MockTransferCacheManager manager;
  TransferCache cache(&manager);
  cache.Init(1);
  Batch in, out;
  EXPECT_CALL(cache.freelist(), RemoveRange).WillOnce([&](void** bufs, int n) {
    memcpy(bufs, in.objs, sizeof(void*) * n);
    return n;
  });
  ASSERT_EQ(cache.RemoveRange(out.objs, batch_size), batch_size);
  for (int i = 0; i < batch_size; ++i) {
    EXPECT_EQ(in.objs[i], out.objs[i]);
  }
}

TEST(TransferCache, EvictsOtherCaches) {
  const int batch_size = MockTransferCacheManager::num_objects_to_move(1);

  MockTransferCacheManager manager;
  TransferCache cache(&manager);
  cache.Init(1);

  Batch in, out;
  EXPECT_CALL(cache.freelist(), InsertRange).Times(0);
  EXPECT_CALL(manager, DetermineSizeClassToEvict).WillOnce([]() { return 13; });
  EXPECT_CALL(manager, ShrinkCache(13)).WillOnce([]() { return true; });

  for (int i = 0; i < TransferCache::kInitialCapacityInBatches; ++i) {
    cache.InsertRange(absl::MakeSpan(in.objs, batch_size), batch_size);
  }
  cache.InsertRange(absl::MakeSpan(in.objs + batch_size, batch_size),
                    batch_size);
  ASSERT_EQ(cache.RemoveRange(out.objs, batch_size), batch_size);
  for (int i = 0; i < batch_size; ++i) {
    EXPECT_EQ(in.objs[i + batch_size], out.objs[i]);
  }
}

TEST(TransferCache, PushesToFreelist) {
  const int batch_size = MockTransferCacheManager::num_objects_to_move(1);
  MockTransferCacheManager manager;
  TransferCache cache(&manager);
  cache.Init(1);
  Batch in, out;

  EXPECT_CALL(manager, DetermineSizeClassToEvict).WillOnce([]() { return 13; });
  EXPECT_CALL(manager, ShrinkCache(13)).WillOnce([]() { return false; });
  EXPECT_CALL(cache.freelist(), InsertRange).WillOnce([&](void** bufs, int n) {
    for (int i = 0; i < batch_size; ++i) {
      EXPECT_EQ(in.objs[i + batch_size], bufs[i]);
    }
  });

  for (int i = 0; i < TransferCache::kInitialCapacityInBatches; ++i) {
    cache.InsertRange(absl::MakeSpan(in.objs, batch_size), batch_size);
  }
  cache.InsertRange(absl::MakeSpan(in.objs + batch_size, batch_size),
                    batch_size);
}

}  // namespace
}  // namespace tcmalloc
