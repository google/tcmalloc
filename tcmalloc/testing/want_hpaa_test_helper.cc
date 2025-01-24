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

#include <cstdio>
#include <string>

#include "absl/log/check.h"
#include "absl/strings/match.h"
#include "absl/strings/numbers.h"
#include "absl/strings/str_split.h"
#include "absl/strings/string_view.h"
#include "absl/strings/strip.h"
#include "tcmalloc/malloc_extension.h"

int main(int argc, char** argv) {
  // Why examine mallocz data, rather than just call decide_want_hpaa?  We want
  // as close of an end-to-end validation as we can get.
  std::string input = tcmalloc::MallocExtension::GetStats();
  bool hpaa = false;
  int subrelease = -1;
  for (absl::string_view line : absl::StrSplit(input, '\n')) {
    if (absl::StrContains(line, "Begin SAMPLED page allocator")) {
      // Stop when we reach the end of the main page allocator. We don't
      // want to look at the sampled or cold allocator parameters for this
      // test.
      break;
    }

    constexpr absl::string_view kHPAAToken = "HugePageAware";
    hpaa |= absl::StrContains(line, kHPAAToken);

    if (absl::ConsumePrefix(&line, "PARAMETER hpaa_subrelease ")) {
      CHECK(absl::SimpleAtoi(line, &subrelease)) << "Could not parse: " << line;
    }
  }

  printf(hpaa ? "HPAA" : "NoHPAA");
  if (hpaa) {
    CHECK_NE(subrelease, -1) << "subrelease parameter not found in mallocz";
    if (subrelease) {
      printf("|subrelease");
    }
  } else {
    CHECK_EQ(subrelease, -1)
        << "found unexpected subrelease parameter for non-HPAA allocator";
  }

  return 0;
}
