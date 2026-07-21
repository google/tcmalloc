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

#include <cstddef>
#include <vector>

#include "gtest/gtest.h"
#include "fuzztest/fuzztest.h"
#include "absl/types/span.h"
#include "tcmalloc/common.h"
#include "tcmalloc/internal/config.h"
#include "tcmalloc/internal/size_class_info.h"
#include "tcmalloc/sizemap.h"
#include "tcmalloc/tcmalloc_policy.h"

namespace tcmalloc::tcmalloc_internal {
namespace {

struct FuzzSizeClassInfo {
  uint32_t size;
  uint32_t pages;
  uint8_t num_to_move;
};

void FuzzSizeMap(const std::vector<FuzzSizeClassInfo>& fuzz_info) {
  if (fuzz_info.empty()) {
    return;
  }

  std::vector<SizeClassInfo> info;
  for (const auto& f : fuzz_info) {
    info.push_back(
        SizeClassInfo(f.size, Length(f.pages).in_bytes(), f.num_to_move));
  }

  SizeMap m;
  if (!m.Init(absl::MakeSpan(info))) {
    return;
  }

  // Validate that every size on [0, kMaxSize] maps to a size class that is
  // neither too big nor too small.
  int last_size_class = -1;
  for (size_t size = 0; size <= kMaxSize; size++) {
    const int size_class = m.SizeClass(CppPolicy(), size);
    EXPECT_GT(size_class, 0) << size;
    EXPECT_LT(size_class, kNumClasses) << size;

    const size_t s = m.class_to_size(size_class);
    EXPECT_LE(size, s);
    EXPECT_NE(s, 0) << size;

    EXPECT_LE(last_size_class, size_class);
    last_size_class = size_class;
  }
}

void FuzzGetSizeClass(size_t size) {
  SizeMap m;
  // Before m.Init(), SizeClass should always return 0 or the equivalent in a
  // non-zero NUMA partition.
  {
    if (auto [is_small, size_class] = m.GetSizeClass(CppPolicy(), size);
        is_small) {
      EXPECT_EQ(size_class % kNumBaseClasses, 0) << size;
      EXPECT_LT(size_class, kExpandedClassesStart) << size;
    } else {
      // We should only fail to lookup the size class when size is outside of
      // the size classes.
      EXPECT_GT(size, kMaxSize);
    }
  }

  // After m.Init(), GetSizeClass should return a size class.
  ASSERT_TRUE(m.Init(kSizeClasses.classes));
  {
    if (auto [is_small, size_class] = m.GetSizeClass(CppPolicy(), size);
        is_small) {
      const size_t mapped_size = m.class_to_size(size_class);
      // The size class needs to hold size.
      EXPECT_GE(mapped_size, size);
    } else {
      // We should only fail to lookup the size class when size is outside of
      // the size classes.
      EXPECT_GT(size, kMaxSize);
    }
  }
}

void FuzzGetSizeClassWithAlignment(size_t size, size_t alignment) {
  SizeMap m;
  // Before m.Init(), SizeClass should always return 0 or the equivalent in a
  // non-zero NUMA partition.
  {
    if (auto [is_small, size_class] =
            m.GetSizeClass(CppPolicy().AlignAs(alignment), size);
        is_small) {
      EXPECT_EQ(size_class % kNumBaseClasses, 0) << size << " " << alignment;
      EXPECT_LT(size_class, kExpandedClassesStart) << size << " " << alignment;
    } else if (alignment <= kPageSize) {
      // When alignment > kPageSize, we do not produce a size class.
      //
      // We should only fail to lookup the size class when size is large.
      EXPECT_GT(size, kMaxSize) << alignment;
    }
  }

  // After m.Init(), GetSizeClass should return a size class.
  ASSERT_TRUE(m.Init(kSizeClasses.classes));
  {
    if (auto [is_small, size_class] =
            m.GetSizeClass(CppPolicy().AlignAs(alignment), size);
        is_small) {
      const size_t mapped_size = m.class_to_size(size_class);
      // The size class needs to hold size.
      EXPECT_GE(mapped_size, size);
      // The size needs to be a multiple of alignment.
      EXPECT_EQ(mapped_size % alignment, 0);
    } else if (alignment <= kPageSize) {
      // When alignment > kPageSize, we do not produce a size class.
      //
      // We should only fail to lookup the size class when size is large.
      EXPECT_GT(size, kMaxSize) << alignment;
    }
  }
}

void FuzzSizeClass(size_t size) {
  if (size > kMaxSize) return;

  SizeMap m;
  // Before m.Init(), SizeClass should always return 0 or the equivalent in a
  // non-zero NUMA partition.
  {
    const uint32_t size_class = m.SizeClass(CppPolicy(), size);
    EXPECT_EQ(size_class % kNumBaseClasses, 0) << size;
    EXPECT_LT(size_class, kExpandedClassesStart) << size;
  }

  // After m.Init(), SizeClass should return a size class.
  ASSERT_TRUE(m.Init(kSizeClasses.classes));
  {
    uint32_t size_class = m.SizeClass(CppPolicy(), size);
    const size_t mapped_size = m.class_to_size(size_class);
    // The size class needs to hold size.
    EXPECT_GE(mapped_size, size);
  }
}

FUZZ_TEST(SizeMapTest, FuzzSizeMap);
FUZZ_TEST(SizeMapTest, FuzzGetSizeClass);
FUZZ_TEST(SizeMapTest, FuzzGetSizeClassWithAlignment)
    .WithDomains(fuzztest::Arbitrary<size_t>(),
                 fuzztest::Map([](size_t k) { return size_t{1} << k; },
                               fuzztest::InRange<size_t>(0, kHugePageShift)));
FUZZ_TEST(SizeMapTest, FuzzSizeClass);

}  // namespace
}  // namespace tcmalloc::tcmalloc_internal
