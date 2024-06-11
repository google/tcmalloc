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

#include "fuzztest/fuzztest.h"
#include "tcmalloc/internal/cpu_utils.h"
#include "tcmalloc/internal/sysinfo.h"

namespace tcmalloc::tcmalloc_internal {
namespace {

void ParseInput(const std::string& s) {
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

FUZZ_TEST(SysinfoTest, ParseInput)
    ;

}  // namespace
}  // namespace tcmalloc::tcmalloc_internal
