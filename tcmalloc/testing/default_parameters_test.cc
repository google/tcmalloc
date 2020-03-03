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

#include "absl/strings/str_format.h"
#include "tcmalloc/malloc_extension.h"

namespace tcmalloc {
namespace {

#if !defined(ADDRESS_SANITIZER) && !defined(MEMORY_SANITIZER) && \
    !defined(THREAD_SANITIZER)
constexpr int64_t kDefaultProfileSamplingRate =
#ifdef TCMALLOC_SMALL_BUT_SLOW
    512 << 10
#else
    2 << 20
#endif
    ;
constexpr int64_t kDefaultGuardedSamplingRate = 50 * kDefaultProfileSamplingRate;
constexpr int64_t kDefaultGuardedSampleParameter = 50;
#else
constexpr int64_t kDefaultProfileSamplingRate = -1;
constexpr int64_t kDefaultGuardedSamplingRate = -1;
constexpr int64_t kDefaultGuardedSampleParameter = -1;
#endif

bool TestProfileSamplingRate() {

  auto extension_value = MallocExtension::GetProfileSamplingRate();
  if (extension_value != kDefaultProfileSamplingRate) {
    absl::FPrintF(stderr, "ProfileSamplingRate: got %d, want %d\n",
                  extension_value, kDefaultProfileSamplingRate);
    return false;
  }

  return true;
}

bool TestGuardedSamplingRate() {

  auto extension_value = MallocExtension::GetGuardedSamplingRate();
  if (extension_value != kDefaultGuardedSamplingRate) {
    absl::FPrintF(stderr, "GuardedSamplingRate: got %d, want %d\n",
                  extension_value, kDefaultGuardedSamplingRate);
    return false;
  }

  return true;
}

}  // namespace
}  // namespace tcmalloc

int main() {
  // This test has minimal dependencies, to avoid perturbing the initial
  // parameters for TCMalloc.
  bool success = true;
  success = success & tcmalloc::TestProfileSamplingRate();
  success = success & tcmalloc::TestGuardedSamplingRate();

  if (success) {
    fprintf(stderr, "PASS");
    return 0;
  } else {
    return 1;
  }
}
