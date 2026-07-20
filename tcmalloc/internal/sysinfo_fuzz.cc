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

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <optional>
#include <string>
#include <vector>

#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "fuzztest/fuzztest.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_join.h"
#include "absl/strings/string_view.h"
#include "tcmalloc/internal/cpu_utils.h"
#include "tcmalloc/internal/sysinfo.h"

namespace tcmalloc::tcmalloc_internal {
namespace {

void ParseInput(absl::string_view s) {
  const char* data = s.data();
  size_t size = s.size();

  std::optional<CpuSet> r =
      ParseCpulist([&](char* buf, size_t count) -> ssize_t {
        size_t to_read = std::min(size, count);
        if (to_read > 0) {
          memcpy(buf, data, to_read);
          data += to_read;
          size -= to_read;
        }
        return to_read;
      });
  (void)r;
}

FUZZ_TEST(SysinfoTest, ParseInput);

void FuzzCpuSetRoundTrip(const std::vector<int>& cpus_to_set,
                         const std::vector<size_t>& read_sizes) {
  CpuSet reference;
  reference.Zero();

  // Serialize the reference set into a cpulist-style string.
  for (int cpu : cpus_to_set) {
    if (cpu >= 0 && cpu < kMaxCpus) {
      reference.Set(cpu);
    }
  }

  std::vector<std::string> components;
  for (int cpu = 0; cpu < kMaxCpus; cpu++) {
    if (!reference.IsSet(cpu)) continue;

    const int start = cpu;
    int next = cpu + 1;
    while (next < kMaxCpus && reference.IsSet(next)) {
      cpu = next;
      next = cpu + 1;
    }

    if (cpu == start) {
      components.push_back(absl::StrCat(cpu));
    } else {
      components.push_back(absl::StrCat(start, "-", cpu));
    }
  }
  const std::string serialized = absl::StrJoin(components, ",");

  absl::string_view remaining(serialized);
  // Now parse that string using our ParseCpulist function, randomizing the
  // amount of data we provide to it from each read.
  size_t read_sizes_idx = 0;

  const std::optional<CpuSet> parsed =
      ParseCpulist([&](char* const buf, const size_t count) -> ssize_t {
        // Calculate how much data we have left to provide.
        const size_t max = std::min(count, remaining.size());

        // If none, we have no choice but to provide nothing.
        if (max == 0) return 0;

        // If we do have data, return a randomly sized subset of it to stress
        // the logic around reading partial values.
        size_t copy;
        if (read_sizes_idx < read_sizes.size()) {
          copy = (read_sizes[read_sizes_idx] % max) + 1;
          read_sizes_idx++;
        } else {
          copy = max;
        }

        memcpy(buf, remaining.data(), copy);
        remaining.remove_prefix(copy);
        return copy;
      });

  // We ought to have parsed the same set of CPUs that we serialized.
  ASSERT_THAT(parsed, testing::Ne(std::nullopt));
  EXPECT_TRUE(CPU_EQUAL_S(kCpuSetBytes, parsed->data(), reference.data()));
}

FUZZ_TEST(SysinfoTest, FuzzCpuSetRoundTrip)
    .WithDomains(fuzztest::VectorOf(fuzztest::InRange<int>(0, kMaxCpus - 1)),
                 fuzztest::VectorOf(fuzztest::Arbitrary<size_t>()));

}  // namespace
}  // namespace tcmalloc::tcmalloc_internal
