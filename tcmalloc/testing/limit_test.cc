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

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>

#include <limits>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "absl/strings/match.h"
#include "absl/strings/str_format.h"
#include "absl/strings/string_view.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "tcmalloc/common.h"
#include "tcmalloc/internal/logging.h"
#include "tcmalloc/internal/parameter_accessors.h"
#include "tcmalloc/malloc_extension.h"
#include "tcmalloc/testing/testutil.h"

namespace tcmalloc {
namespace {

using ::testing::ContainsRegex;
using ::testing::HasSubstr;

void DumpHeapStats(absl::string_view label) {
  std::string buffer = MallocExtension::GetStats();
  absl::FPrintF(stderr, "%s\n%s\n", label, buffer);
}

// Fixture for friend access to MallocExtension.
class LimitTest : public ::testing::Test {
 protected:
  LimitTest() {
    stats_buffer_.reserve(3 << 20);
    stats_pbtxt_.reserve(3 << 20);
  }

  void SetLimit(size_t limit, bool is_hard) {
    MallocExtension::MemoryLimit v;
    v.limit = limit;
    v.hard = is_hard;
    MallocExtension::SetMemoryLimit(v);
  }

  size_t GetLimit(bool is_hard) {
    MallocExtension::MemoryLimit v = MallocExtension::GetMemoryLimit();
    if (v.hard == is_hard) {
      return v.limit;
    } else {
      // Return no limit, as we do not have a limit matching is_hard.
      return std::numeric_limits<size_t>::max();
    }
  }

  // avoid fragmentation in local caches
  void *malloc_pages(size_t bytes) {
    CHECK_CONDITION(bytes % kPageSize == 0);
    void *ptr;
    CHECK_CONDITION(posix_memalign(&ptr, kPageSize, bytes) == 0);
    return ptr;
  }

  size_t physical_memory_used() {
    std::map<std::string, MallocExtension::Property> m =
        MallocExtension::GetProperties();
    auto i = m.find("generic.physical_memory_used");
    CHECK_CONDITION(i != m.end());
    return i->second.value;
  }

  // Returns a human-readable stats representation.  This is backed by
  // stats_buffer_, to avoid allocating while potentially under a memory limit.
  absl::string_view GetStats() {
    size_t capacity = stats_buffer_.capacity();
    stats_buffer_.resize(capacity);
    char *data = stats_buffer_.data();

    int actual_size = TCMalloc_Internal_GetStats(data, capacity);
    stats_buffer_.erase(actual_size);
    return absl::string_view(data, actual_size);
  }

  // Returns a pbtxt-based stats representation.  This is backed by
  // stats_pbtxt_, to avoid allocating while potentially under a memory limit.
  absl::string_view GetStatsInPbTxt() {
    size_t capacity = stats_pbtxt_.capacity();
    stats_pbtxt_.resize(capacity);
    char *data = stats_pbtxt_.data();

    int actual_size = MallocExtension_Internal_GetStatsInPbtxt(data, capacity);
    stats_pbtxt_.erase(actual_size);
    return absl::string_view(data, actual_size);
  }

  std::string stats_buffer_;
  std::string stats_pbtxt_;
};

TEST_F(LimitTest, LimitRespected) {
  static const size_t kLim = 4ul * 1024 * 1024 * 1024;
  SetLimit(kLim, false);

  absl::string_view statsBuf = GetStats();
  absl::string_view statsPbtxt = GetStatsInPbTxt();

  char buf[512];
  absl::SNPrintF(buf, sizeof(buf), "PARAMETER desired_usage_limit_bytes %u",
                 kLim);
  EXPECT_THAT(statsBuf, HasSubstr(buf));
  EXPECT_THAT(statsBuf, HasSubstr("Number of times limit was hit: 0"));

  absl::SNPrintF(buf, sizeof(buf), "desired_usage_limit_bytes: %u", kLim);
  EXPECT_THAT(statsPbtxt, HasSubstr(buf));
  EXPECT_THAT(statsPbtxt, HasSubstr("hard_limit: false"));
  EXPECT_THAT(statsPbtxt, HasSubstr("limit_hits: 0"));

  // Avoid failing due to usage by test itself.
  static const size_t kLimForUse = kLim * 9 / 10;
  // First allocate many small objects...
  size_t used = 0;
  std::vector<void *> ptrs;
  while (used < kLimForUse) {
    ptrs.push_back(malloc_pages(kPageSize));
    used += kPageSize;
  }
  DumpHeapStats("after allocating small objects");
  // return much of the space, fragmented...
  bool ret = false;
  for (auto &p : ptrs) {
    if (ret) {
      free(p);
      p = nullptr;
      used -= kPageSize;
    }
    ret = !ret;
  }
  DumpHeapStats("after freeing many small objects");
  // Now ensure we can re use it for large allocations.

  while (used < kLimForUse) {
    const size_t large = kPageSize * 10;
    ptrs.push_back(malloc_pages(large));
    used += large;
  }
  DumpHeapStats("after allocating large objects");
  EXPECT_LE(physical_memory_used(), kLim);

  statsBuf = GetStats();
  statsPbtxt = GetStatsInPbTxt();
  // The HugePageAwareAllocator hits the limit more than once.
  EXPECT_THAT(statsBuf,
              ContainsRegex(R"(Number of times limit was hit: [1-9]\d*)"));
  EXPECT_THAT(statsPbtxt, ContainsRegex(R"(limit_hits: [1-9]\d*)"));

  for (auto p : ptrs) {
    free(p);
  }
}

TEST_F(LimitTest, HardLimitRespected) {
  static const size_t kLim = 400 << 20;
  SetLimit(kLim, true);

  absl::string_view statsBuf = GetStats();
  absl::string_view statsPbtxt = GetStatsInPbTxt();

  // Avoid gmock matchers, as they require a std::string which may allocate.
  char buf[512];
  absl::SNPrintF(buf, sizeof(buf),
                 "PARAMETER desired_usage_limit_bytes %u (hard)", kLim);
  EXPECT_TRUE(absl::StrContains(statsBuf, buf)) << statsBuf;

  absl::SNPrintF(buf, sizeof(buf), "desired_usage_limit_bytes: %u", kLim);
  EXPECT_TRUE(absl::StrContains(statsPbtxt, buf)) << statsPbtxt;
  EXPECT_TRUE(absl::StrContains(statsPbtxt, "hard_limit: true")) << statsPbtxt;

  ASSERT_DEATH(malloc_pages(400 << 20), "limit");

  SetLimit(std::numeric_limits<size_t>::max(), false);
}

TEST_F(LimitTest, HardLimitRespectsNoSubrelease) {
  static const size_t kLim = 300 << 20;
  SetLimit(kLim, true);
  TCMalloc_Internal_SetHPAASubrelease(false);
  EXPECT_FALSE(TCMalloc_Internal_GetHPAASubrelease());

  absl::string_view statsBuf = GetStats();
  absl::string_view statsPbtxt = GetStatsInPbTxt();

  char buf[512];
  absl::SNPrintF(buf, sizeof(buf),
                 "PARAMETER desired_usage_limit_bytes %u (hard)", kLim);
  EXPECT_THAT(statsBuf, HasSubstr(buf));

  absl::SNPrintF(buf, sizeof(buf), "desired_usage_limit_bytes: %u", kLim);
  EXPECT_THAT(statsPbtxt, HasSubstr(buf));
  EXPECT_THAT(statsPbtxt, HasSubstr("hard_limit: true"));

  ASSERT_DEATH(
      []() {
        // Allocate a bunch of medium objects, free half of them to cause some
        // fragmentation, then allocate some large objects. If we subrelease we
        // could stay under our hard limit, but if we don't then we should go
        // over.
        std::vector<void *> ptrs;
        constexpr size_t kNumMediumObjs = 400;
        constexpr size_t kNumLargeObjs = 200;
        for (size_t i = 0; i < kNumMediumObjs; i++) {
          ptrs.push_back(::operator new(512 << 10));
        }
        DumpHeapStats("after allocating medium objects");
        for (size_t i = 0; i < ptrs.size(); i++) {
          if (i % 2) continue;
          ::operator delete(ptrs[i]);
          ptrs[i] = static_cast<void *>(nullptr);
        }
        DumpHeapStats("after freeing half of medium objects");
        for (size_t i = 0; i < kNumLargeObjs; i++) {
          ptrs.push_back(::operator new(1 << 20));
        }
        DumpHeapStats("after allocating large objects");
        while (!ptrs.empty()) {
          ::operator delete(ptrs.back());
          ptrs.pop_back();
        }
        DumpHeapStats("after freeing all objects");
      }(),
      "limit");
  SetLimit(std::numeric_limits<size_t>::max(), false);
}

}  // namespace
}  // namespace tcmalloc
