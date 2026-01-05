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
#include <string>

#include "fuzztest/fuzztest.h"
#include "tcmalloc/internal/memory_stats.h"

namespace tcmalloc::tcmalloc_internal {
namespace {

void ParseStatmFuzz(const std::string& s) {
  const char* data = s.data();
  size_t size = s.size();

  MemoryStats stats;
  (void)ParseStatm(
      [&](char* buf, size_t count) -> ssize_t {
        size_t to_read = std::min(size, count);
        if (to_read > 0) {
          memcpy(buf, data, to_read);
          data += to_read;
          size -= to_read;
        }
        return to_read;
      },
      &stats);
}

FUZZ_TEST(MemoryStatsTest, ParseStatmFuzz);

void ParseSmapsRollupFuzz(const std::string& s) {
  const char* data = s.data();
  size_t size = s.size();

  (void)ParseSmapsRollup([&](char* buf, size_t count) -> ssize_t {
    size_t to_read = std::min(size, count);
    if (to_read > 0) {
      memcpy(buf, data, to_read);
      data += to_read;
      size -= to_read;
    }
    return to_read;
  });
}

FUZZ_TEST(MemoryStatsTest, ParseSmapsRollupFuzz);

}  // namespace
}  // namespace tcmalloc::tcmalloc_internal
