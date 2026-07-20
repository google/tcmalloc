// Copyright 2023 The TCMalloc Authors
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

#include "tcmalloc/internal/sysinfo.h"

#include <sched.h>

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <optional>
#include <string>
#include <vector>

#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "absl/base/internal/sysinfo.h"
#include "absl/random/random.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_join.h"
#include "absl/strings/string_view.h"
#include "absl/types/optional.h"
#include "tcmalloc/internal/allocation_guard.h"
#include "tcmalloc/internal/cpu_utils.h"

namespace tcmalloc {
namespace tcmalloc_internal {
namespace {

TEST(ParseCpulistTest, Empty) {
  absl::string_view empty("\n");

  const absl::optional<CpuSet> parsed =
      ParseCpulist([&](char* const buf, const size_t count) -> ssize_t {
        // Calculate how much data we have left to provide.
        const size_t to_copy = std::min(count, empty.size());

        // If none, we have no choice but to provide nothing.
        if (to_copy == 0) return 0;

        memcpy(buf, empty.data(), to_copy);
        empty.remove_prefix(to_copy);
        return to_copy;
      });

  // No CPUs should be active on this NUMA node.
  ASSERT_THAT(parsed, testing::Ne(std::nullopt));
  EXPECT_EQ(parsed->Count(), 0);
}

TEST(ParseCpulistTest, AtBounds) {
  std::string cpulist = absl::StrCat("0-", kMaxCpus - 1);

  const absl::optional<CpuSet> parsed =
      ParseCpulist([&](char* const buf, const size_t count) -> ssize_t {
        // Calculate how much data we have left to provide.
        const size_t to_copy = std::min(count, cpulist.size());

        // If none, we have no choice but to provide nothing.
        if (to_copy == 0) return 0;

        memcpy(buf, cpulist.data(), to_copy);
        cpulist.erase(0, to_copy);
        return to_copy;
      });

  // All CPUs should be active on this NUMA node.
  ASSERT_THAT(parsed, testing::Ne(std::nullopt));
  EXPECT_EQ(parsed->Count(), kMaxCpus);
}

TEST(ParseCpulistTest, NotInBounds) {
  std::string cpulist = absl::StrCat("0-", kMaxCpus);

  const absl::optional<CpuSet> parsed =
      ParseCpulist([&](char* const buf, const size_t count) -> ssize_t {
        // Calculate how much data we have left to provide.
        const size_t to_copy = std::min(count, cpulist.size());

        // If none, we have no choice but to provide nothing.
        if (to_copy == 0) return 0;

        memcpy(buf, cpulist.data(), to_copy);
        cpulist.erase(0, to_copy);
        return to_copy;
      });

  ASSERT_THAT(parsed, testing::Eq(std::nullopt));
}

TEST(NumCPUs, NoCache) {
  const int result = []() {
    AllocationGuard guard;
    return *sysinfo_internal::NumPossibleCPUsNoCache();
  }();

  // TODO(b/67389555): This test may fail if there are offlined CPUs.
  EXPECT_EQ(result, absl::base_internal::NumCPUs());
}

TEST(NumCPUs, Cached) {
  // TODO(b/67389555): This test may fail if there are offlined CPUs.
  EXPECT_EQ(NumCPUs(), absl::base_internal::NumCPUs());
}

}  // namespace
}  // namespace tcmalloc_internal
}  // namespace tcmalloc
