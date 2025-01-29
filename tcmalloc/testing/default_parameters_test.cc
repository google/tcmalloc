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
#include <stdint.h>

#include <cstdio>

#include "absl/strings/str_format.h"
#include "absl/time/time.h"
#include "tcmalloc/malloc_extension.h"

namespace tcmalloc {
namespace {

constexpr int64_t kDefaultProfileSamplingInterval =
#if defined(TCMALLOC_INTERNAL_SMALL_BUT_SLOW)
    512 << 10
#else
    2 << 20
#endif
    ;
#ifdef NDEBUG
constexpr int64_t kDefaultGuardedSampleParameter = 50;
#else
constexpr int64_t kDefaultGuardedSampleParameter = 5;
#endif
constexpr int64_t kDefaultGuardedSamplingInterval =
    kDefaultGuardedSampleParameter * kDefaultProfileSamplingInterval;
constexpr absl::Duration kDefaultSkipSubreleaseShortInterval =
#if defined(TCMALLOC_INTERNAL_SMALL_BUT_SLOW)
    absl::ZeroDuration()
#else
    absl::Seconds(60)
#endif
    ;
constexpr MallocExtension::BytesPerSecond kDefaultBackgroundReleaseRate{
    0
};
constexpr absl::Duration kDefaultSkipSubreleaseLongInterval =
#if defined(TCMALLOC_INTERNAL_SMALL_BUT_SLOW)
    absl::ZeroDuration()
#else
    absl::Seconds(300)
#endif
    ;

bool TestProfileSamplingInterval() {

  auto extension_value = MallocExtension::GetProfileSamplingInterval();
  if (extension_value != kDefaultProfileSamplingInterval) {
    absl::FPrintF(stderr, "ProfileSamplingInterval: got %d, want %d\n",
                  extension_value, kDefaultProfileSamplingInterval);
    return false;
  }

  return true;
}

bool TestGuardedSamplingInterval() {

  auto extension_value = MallocExtension::GetGuardedSamplingInterval();
  if (extension_value != kDefaultGuardedSamplingInterval) {
    absl::FPrintF(stderr, "GuardedSamplingInterval: got %d, want %d\n",
                  extension_value, kDefaultGuardedSamplingInterval);
    return false;
  }

  return true;
}

bool TestBackgroundReleaseRate() {

  auto extension_value = MallocExtension::GetBackgroundReleaseRate();
  if (extension_value != kDefaultBackgroundReleaseRate) {
    absl::FPrintF(stderr, "BackgroundReleaseRate: got %d, want %d\n",
                  extension_value, kDefaultBackgroundReleaseRate);
    return false;
  }

  return true;
}

bool TestSkipSubreleaseIntervals() {

  auto short_interval_extension_value =
      MallocExtension::GetSkipSubreleaseShortInterval();
  if (short_interval_extension_value != kDefaultSkipSubreleaseShortInterval) {
    absl::FPrintF(stderr, "Skip Subrelease Short Interval: got %d, want %d\n",
                  absl::ToInt64Seconds(short_interval_extension_value),
                  absl::ToInt64Seconds(kDefaultSkipSubreleaseShortInterval));
    return false;
  }
  auto long_interval_extension_value =
      MallocExtension::GetSkipSubreleaseLongInterval();
  if (long_interval_extension_value != kDefaultSkipSubreleaseLongInterval) {
    absl::FPrintF(stderr, "Skip Subrelease Long Interval: got %d, want %d\n",
                  absl::ToInt64Seconds(long_interval_extension_value),
                  absl::ToInt64Seconds(kDefaultSkipSubreleaseLongInterval));
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
  success = success & tcmalloc::TestProfileSamplingInterval();
  success = success & tcmalloc::TestGuardedSamplingInterval();
  success = success & tcmalloc::TestBackgroundReleaseRate();
  success = success & tcmalloc::TestSkipSubreleaseIntervals();

  if (success) {
    fprintf(stderr, "PASS");
    return 0;
  } else {
    return 1;
  }
}
