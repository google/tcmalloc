// Copyright 2026 The TCMalloc Authors
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
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "gtest/gtest.h"
#include "fuzztest/fuzztest.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_format.h"
#include "absl/strings/string_view.h"
#include "tcmalloc/internal/proc_maps.h"

namespace tcmalloc::tcmalloc_internal {
namespace {

void FuzzParseNumaMapsLine(absl::string_view line) {
  std::optional<NumaMapEntry> entry = ParseNumaMapsLine(line);
  (void)entry;
}

FUZZ_TEST(ProcMapsFuzzTest, FuzzParseNumaMapsLine);

void FuzzParseNumaMapsLineRoundTrip(
    uintptr_t start_addr, absl::string_view policy,
    const std::vector<std::string>& attributes,
    const std::vector<std::pair<uint32_t, uint64_t>>& node_counts) {
  NumaMapEntry expected;
  expected.start_addr = start_addr;

  std::string line = absl::StrFormat("%x %s", start_addr, policy);
  for (absl::string_view attr : attributes) {
    absl::StrAppend(&line, " ", attr);
  }
  for (const auto& [node, pages] : node_counts) {
    expected.node_pages[std::min<size_t>(
        node, NumaMapEntry::kNumaNodeBuckets - 1)] += pages;
    absl::StrAppend(&line, " N", node, "=", pages);
  }

  std::optional<NumaMapEntry> actual = ParseNumaMapsLine(line);
  ASSERT_TRUE(actual.has_value());
  EXPECT_EQ(*actual, expected);
}

FUZZ_TEST(ProcMapsFuzzTest, FuzzParseNumaMapsLineRoundTrip)
    .WithDomains(
        fuzztest::Arbitrary<uintptr_t>(),
        fuzztest::ElementOf<std::string>({"default", "bind:0", "bind:0,1",
                                          "prefer:0", "interleave:0-3",
                                          "local"}),
        fuzztest::VectorOf(fuzztest::ElementOf<std::string>(
            {"anon=16", "dirty=16", "mapped=32", "mapmax=4", "active=0",
             "file=/lib/libc.so.6", "heap", "stack", "huge",
             "kernelpagesize_kB=4"})),
        fuzztest::VectorOf(
            fuzztest::PairOf(fuzztest::InRange<uint32_t>(0, 1024),
                             fuzztest::InRange<uint64_t>(0, uint64_t{1} << 40)))
            .WithMaxSize(64));

void FuzzReadLines(absl::string_view s, size_t max_chunk_size) {
  const char* data = s.data();
  size_t size = s.size();

  ReadLines(
      [&](char* buf, size_t count) -> ssize_t {
        size_t to_read = std::min({size, count, max_chunk_size});
        if (to_read > 0) {
          memcpy(buf, data, to_read);
          data += to_read;
          size -= to_read;
        }
        return to_read;
      },
      [&](absl::string_view line) {
        std::optional<NumaMapEntry> entry = ParseNumaMapsLine(line);
        (void)entry;
      });
}

FUZZ_TEST(ProcMapsFuzzTest, FuzzReadLines)
    .WithDomains(
        fuzztest::String(),
        fuzztest::InRange<size_t>(1, 2 * ProcMapsIterator::Buffer::kBufSize));

}  // namespace
}  // namespace tcmalloc::tcmalloc_internal
