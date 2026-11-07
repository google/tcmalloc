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
#include <vector>

#include "gtest/gtest.h"
#include "fuzztest/fuzztest.h"
#include "absl/strings/string_view.h"
#include "tcmalloc/internal/proc_maps.h"

namespace tcmalloc::tcmalloc_internal {
namespace {

void FuzzParseNumaMapsLine(absl::string_view line) {
  uintptr_t start_addr = 0;
  std::optional<int> node = ParseNumaMapsLine(line, start_addr);
  if (node.has_value()) {
    EXPECT_GE(*node, 0);
  }
}

FUZZ_TEST(ProcMapsFuzzTest, FuzzParseNumaMapsLine);

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
        uintptr_t start_addr = 0;
        std::optional<int> node = ParseNumaMapsLine(line, start_addr);
        if (node.has_value()) {
          EXPECT_GE(*node, 0);
        }
      });
}

FUZZ_TEST(ProcMapsFuzzTest, FuzzReadLines)
    .WithDomains(
        fuzztest::String(),
        fuzztest::InRange<size_t>(1, 2 * ProcMapsIterator::Buffer::kBufSize));

}  // namespace
}  // namespace tcmalloc::tcmalloc_internal
