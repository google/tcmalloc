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
#include "tcmalloc/static_vars.h"

namespace tcmalloc {
namespace {

struct Batch {
  Batch() {
    for (int i = 0; i < kMaxObjectsToMove; ++i) {
      objs[i] = &(objs[i]);
    }
  }
  void* objs[kMaxObjectsToMove];
};

// Until we break the dep on the central freelist, it is *imperative* that
// we never spill to the centeral freelist in our tests.

TEST(TransferCache, DISABLED_IsolatedSmoke) {
  const int batch_size = Static::sizemap()->num_objects_to_move(1);
  TransferCache cache(nullptr);
  {
    absl::base_internal::SpinLockHolder l(&pageheap_lock);
    cache.Init(1);
  }
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

}  // namespace
}  // namespace tcmalloc
