// Copyright 2019 The TCMalloc Authors
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

#include "tcmalloc/span.h"

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#include <algorithm>
#include <memory>
#include <new>
#include <string>
#include <utility>
#include <vector>

#include "benchmark/benchmark.h"
#include "gtest/gtest.h"
#include "absl/base/internal/spinlock.h"
#include "absl/base/optimization.h"
#include "absl/container/flat_hash_map.h"
#include "absl/container/flat_hash_set.h"
#include "absl/random/random.h"
#include "absl/types/span.h"
#include "tcmalloc/common.h"
#include "tcmalloc/internal/logging.h"
#include "tcmalloc/pages.h"
#include "tcmalloc/static_vars.h"

namespace tcmalloc {
namespace tcmalloc_internal {
namespace {

TEST(SpanMathTest, CalcReciprocalAndOffsetToIdx) {
  const std::vector<size_t> test_sizes = {
      8,   16,  24,   32,   48,   64,   80,    96,    128,
      256, 512, 1024, 2048, 4096, 8192, 16384, 32768, 65536};

  for (size_t size : test_sizes) {
    const uint32_t reciprocal = CalcReciprocal(size);
    const size_t max_objects = std::min<size_t>(1000, (32 * 8192) / size);

    for (size_t i = 0; i < max_objects; ++i) {
      const uintptr_t offset = i * size;
      const uint32_t calculated_idx = OffsetToIdx(offset, reciprocal);
      EXPECT_EQ(calculated_idx, i);
    }
  }
}

TEST(SpanMetaTest, IsValid) {
  SpanMeta empty{};
  EXPECT_FALSE(empty.IsValid());

  SpanMeta freed = SpanMeta::Freed();
  EXPECT_FALSE(freed.IsValid());
  EXPECT_TRUE(freed.IsFreed());

  SpanMeta small{};
  small.is_small = 1;
  EXPECT_TRUE(small.IsValid());

  SpanMeta medium{};
  medium.num_pages_or_sampled_index = 5;
  EXPECT_TRUE(medium.IsValid());

  SpanMeta sampled{};
  sampled.is_sampled = 1;
  sampled.num_pages_or_sampled_index = 0;
  EXPECT_TRUE(sampled.IsValid());
  EXPECT_FALSE(sampled.IsFreed());
}

}  // namespace
}  // namespace tcmalloc_internal
}  // namespace tcmalloc
