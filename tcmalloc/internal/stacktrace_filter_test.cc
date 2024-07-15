// Copyright 2022 The TCMalloc Authors
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

#include "tcmalloc/internal/stacktrace_filter.h"

#include <cstddef>
#include <cstdint>

#include "gtest/gtest.h"
#include "absl/container/flat_hash_set.h"
#include "absl/types/span.h"

namespace tcmalloc {
namespace tcmalloc_internal {

class StackTraceFilterTest : public testing::Test {
 protected:
  using DefaultFilter = StackTraceFilter<256, 1>;

  void SetUp() override {
    absl::flat_hash_set<size_t> hashes;
    absl::flat_hash_set<size_t> hash_bases;

    auto initialize_unique_stack_trace = [&](void*& val) {
      uint64_t pc = 0;
      while (true) {
        ++pc;
        // Checking for wrap around (unique stack trace never found)
        ASSERT_NE(pc, 0);
        val = reinterpret_cast<void*>(pc);
        size_t hash = DefaultFilter::GetFirstHash({&val, 1});
        size_t hash_base = GetFirstIndex({&val, 1});
        if (!hash_bases.contains(hash_base) && !hashes.contains(hash)) {
          hashes.insert(hash);
          hash_bases.insert(hash_base);
          break;
        }
      }
    };

    initialize_unique_stack_trace(stack_trace1_val_);
    initialize_unique_stack_trace(stack_trace2_val_);
    initialize_unique_stack_trace(stack_trace3_val_);

    // Ensure no collisions among test set (the initializer above should ensure
    // this already).
    ASSERT_NE(DefaultFilter::GetFirstHash(stack_trace1_),
              DefaultFilter::GetFirstHash(stack_trace2_));
    ASSERT_NE(GetFirstIndex(stack_trace1_), GetFirstIndex(stack_trace2_));
    ASSERT_NE(DefaultFilter::GetFirstHash(stack_trace1_),
              DefaultFilter::GetFirstHash(stack_trace3_));
    ASSERT_NE(GetFirstIndex(stack_trace1_), GetFirstIndex(stack_trace3_));
    ASSERT_NE(DefaultFilter::GetFirstHash(stack_trace2_),
              DefaultFilter::GetFirstHash(stack_trace3_));
    ASSERT_NE(GetFirstIndex(stack_trace2_), GetFirstIndex(stack_trace3_));
  }

  void InitializeColliderStackTrace() {
    absl::flat_hash_set<size_t> hashes;
    absl::flat_hash_set<size_t> hash_bases;

    // Do not add base of stack_trace1_, because that is the match that is being
    // created.
    hashes.insert(DefaultFilter::GetFirstHash(stack_trace1_));
    hashes.insert(DefaultFilter::GetFirstHash(stack_trace2_));
    hash_bases.insert(GetFirstIndex(stack_trace2_));
    hashes.insert(DefaultFilter::GetFirstHash(stack_trace3_));
    hash_bases.insert(GetFirstIndex(stack_trace3_));

    size_t hash1_base = GetFirstIndex(stack_trace1_);
    uint64_t pc = reinterpret_cast<uint64_t>(stack_trace1_[0]);
    size_t collider_hash;
    size_t collider_hash_base;
    while (true) {
      ++pc;
      // Checking for wrap around
      ASSERT_NE(pc, 0);
      collider_stack_trace_val_ = reinterpret_cast<void*>(pc);
      collider_hash = DefaultFilter::GetFirstHash(collider_stack_trace_);
      collider_hash_base = GetFirstIndex(collider_stack_trace_);
      // if a possible match, check to avoid collisions with others
      if (hash1_base == collider_hash_base && !hashes.contains(collider_hash) &&
          !hash_bases.contains(collider_hash_base)) {
        break;
      }
    }

    // Double check the work above
    ASSERT_NE(DefaultFilter::GetFirstHash(stack_trace1_),
              DefaultFilter::GetFirstHash(collider_stack_trace_));
    ASSERT_EQ(GetFirstIndex(stack_trace1_),
              GetFirstIndex(collider_stack_trace_));
    ASSERT_NE(DefaultFilter::GetFirstHash(stack_trace2_),
              DefaultFilter::GetFirstHash(collider_stack_trace_));
    ASSERT_NE(GetFirstIndex(stack_trace2_),
              GetFirstIndex(collider_stack_trace_));
    ASSERT_NE(DefaultFilter::GetFirstHash(stack_trace3_),
              DefaultFilter::GetFirstHash(collider_stack_trace_));
    ASSERT_NE(GetFirstIndex(stack_trace3_),
              GetFirstIndex(collider_stack_trace_));
  }

  static size_t GetFirstIndex(absl::Span<void* const> stack_trace) {
    return DefaultFilter::GetFirstHash(stack_trace) % DefaultFilter::kSize;
  }

  void* stack_trace1_val_ = nullptr;
  absl::Span<void* const> stack_trace1_{&stack_trace1_val_, 1};
  void* stack_trace2_val_ = nullptr;
  absl::Span<void* const> stack_trace2_{&stack_trace2_val_, 1};
  void* stack_trace3_val_ = nullptr;
  absl::Span<void* const> stack_trace3_{&stack_trace3_val_, 1};
  void* collider_stack_trace_val_ = nullptr;
  absl::Span<void* const> collider_stack_trace_{&collider_stack_trace_val_, 1};
};

namespace {

// This test proves that class can be owned by a constexpr constructor class.
// This is required as the class will be instantiated within
// tcmalloc::tcmalloc_internal::Static.
TEST_F(StackTraceFilterTest, ConstexprConstructor) {
  class Wrapper {
   public:
    constexpr Wrapper() = default;
    DefaultFilter filter_;
  };

  // Instantiate
  [[maybe_unused]] Wrapper wrapper;
}

TEST_F(StackTraceFilterTest, InitialState) {
  DefaultFilter filter;
  EXPECT_FALSE(filter.Contains(stack_trace1_));
}

TEST_F(StackTraceFilterTest, AddRemove) {
  DefaultFilter filter;
  for (int i = 0; i < 100; ++i) {
    filter.Add(stack_trace1_, i);
    EXPECT_EQ(i > 0, filter.Contains(stack_trace1_));
  }
  for (int i = 0; i < 100; ++i) {
    filter.Add(stack_trace1_, -i);
  }
  EXPECT_FALSE(filter.Contains(stack_trace1_));
}

TEST_F(StackTraceFilterTest, CollisionFalsePositive) {
  InitializeColliderStackTrace();
  // False positive because of collision ...
  DefaultFilter filter;
  filter.Add(stack_trace1_, 1);
  EXPECT_TRUE(filter.Contains(stack_trace1_));
  EXPECT_TRUE(filter.Contains(collider_stack_trace_));

  filter.Add(stack_trace1_, -1);
  EXPECT_FALSE(filter.Contains(stack_trace1_));
  EXPECT_FALSE(filter.Contains(collider_stack_trace_));

  filter.Add(collider_stack_trace_, 1);
  EXPECT_TRUE(filter.Contains(stack_trace1_));
  EXPECT_TRUE(filter.Contains(collider_stack_trace_));
}

TEST_F(StackTraceFilterTest, CollisionMultiHash) {
  InitializeColliderStackTrace();
  // ... but with additional hash functions the probability of collision
  // decreases.
  StackTraceFilter<256, 10> filter;
  filter.Add(stack_trace1_, 1);
  EXPECT_TRUE(filter.Contains(stack_trace1_));
  EXPECT_FALSE(filter.Contains(collider_stack_trace_));

  filter.Add(collider_stack_trace_, 1);
  EXPECT_TRUE(filter.Contains(stack_trace1_));
  EXPECT_TRUE(filter.Contains(collider_stack_trace_));

  filter.Add(stack_trace1_, -1);
  EXPECT_FALSE(filter.Contains(stack_trace1_));
  EXPECT_TRUE(filter.Contains(collider_stack_trace_));

  filter.Add(collider_stack_trace_, -1);
  EXPECT_FALSE(filter.Contains(stack_trace1_));
  EXPECT_FALSE(filter.Contains(collider_stack_trace_));
}

// Just test that integer wrap around does not trigger any hardening checks,
// because for our CBF wrap around is benign.
TEST_F(StackTraceFilterTest, IntegerWrapAround) {
  DefaultFilter filter;
  filter.Add(stack_trace1_, 1);
  EXPECT_TRUE(filter.Contains(stack_trace1_));
  filter.Add(stack_trace1_, ~0u);
  EXPECT_FALSE(filter.Contains(stack_trace1_));
}

class DecayingStackTraceFilterTest : public StackTraceFilterTest {
 protected:
  static constexpr size_t kDecaySteps = 3;
  using DefaultFilter = DecayingStackTraceFilter<256, 10, kDecaySteps>;
};

TEST_F(DecayingStackTraceFilterTest, AddAndDecay) {
  DefaultFilter filter;
  for (int i = 0; i < 10; ++i) {
    filter.Add(stack_trace1_, i);
    for (int j = 0; j < kDecaySteps; ++j) {
      EXPECT_TRUE(filter.Contains(stack_trace1_));
      filter.Decay();
    }
    if (i) {
      // If non-zero addition, we need to negate it as well.
      EXPECT_TRUE(filter.Contains(stack_trace1_));
      filter.Add(stack_trace1_, -i);
    }
    EXPECT_FALSE(filter.Contains(stack_trace1_));
  }
}

TEST_F(DecayingStackTraceFilterTest, AddAndAdd) {
  DefaultFilter filter;
  for (int i = 0; i < 10; ++i) {
    filter.Add(stack_trace1_, i);
    for (int j = 0; j < kDecaySteps; ++j) {
      EXPECT_TRUE(filter.Contains(stack_trace1_));
      filter.Add(stack_trace2_, 0);  // implies decay
    }
    if (i) {
      // If non-zero addition, we need to negate it as well.
      EXPECT_TRUE(filter.Contains(stack_trace1_));
      filter.Add(stack_trace1_, -i);
    }
    EXPECT_FALSE(filter.Contains(stack_trace1_));
    EXPECT_TRUE(filter.Contains(stack_trace2_));
  }
}

TEST_F(DecayingStackTraceFilterTest, DecayAll) {
  DefaultFilter filter;
  filter.Add(stack_trace1_, 1);
  filter.Add(stack_trace1_, -1);
  EXPECT_TRUE(filter.Contains(stack_trace1_));
  filter.DecayAll();
  EXPECT_FALSE(filter.Contains(stack_trace1_));
}

}  // namespace
}  // namespace tcmalloc_internal
}  // namespace tcmalloc
