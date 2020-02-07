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

#include <stdint.h>
#include <stdlib.h>

#include <memory>

#include "gtest/gtest.h"
#include "tcmalloc/internal/logging.h"
#include "tcmalloc/malloc_extension.h"
#include "tcmalloc/static_vars.h"

namespace tcmalloc {
namespace {

int64_t ProfileSize(ProfileType type) {
  int64_t total = 0;

  MallocExtension::SnapshotCurrent(type).Iterate(
      [&](const Profile::Sample &e) { total += e.sum; });
  return total;
}

TEST(HeapProfilingTest, PeakHeapTracking) {
  int64_t start_peak_sz = ProfileSize(ProfileType::kPeakHeap);

  // make a large allocation to force a new peak heap sample
  // (total live: 50MiB)
  void *first = malloc(50 << 20);
  int64_t peak_after_first = ProfileSize(ProfileType::kPeakHeap);
  EXPECT_NEAR(peak_after_first, start_peak_sz + (50 << 20), 10 << 20);

  // a small allocation shouldn't increase the peak
  // (total live: 54MiB)
  void *second = malloc(4 << 20);
  int64_t peak_after_second = ProfileSize(ProfileType::kPeakHeap);
  EXPECT_EQ(peak_after_second, peak_after_first);

  // but a large one should
  // (total live: 254MiB)
  void *third = malloc(200 << 20);
  int64_t peak_after_third = ProfileSize(ProfileType::kPeakHeap);
  EXPECT_NEAR(peak_after_third, peak_after_second + (200 << 20), 10 << 20);

  // freeing everything shouldn't affect the peak
  // (total live: 0MiB)
  free(first);
  EXPECT_EQ(ProfileSize(ProfileType::kPeakHeap), peak_after_third);

  free(second);
  EXPECT_EQ(ProfileSize(ProfileType::kPeakHeap), peak_after_third);

  free(third);
  EXPECT_EQ(ProfileSize(ProfileType::kPeakHeap), peak_after_third);

  // going back up less than previous peak shouldn't affect the peak
  // (total live: 200MiB)
  void *fourth = malloc(100 << 20);
  void *fifth = malloc(100 << 20);
  EXPECT_EQ(ProfileSize(ProfileType::kPeakHeap), peak_after_third);

  // passing the old peak significantly, even with many small allocations,
  // should generate a new one
  // (total live: 200MiB + 256MiB = 456MiB, 80% over the 254MiB peak)
  void *bitsy[1 << 10];
  for (int i = 0; i < 1 << 10; i++) {
    bitsy[i] = malloc(1 << 18);
  }
  EXPECT_GT(ProfileSize(ProfileType::kPeakHeap), peak_after_third);

  free(fourth);
  free(fifth);
  for (int i = 0; i < 1 << 10; i++) {
    free(bitsy[i]);
  }
}

}  // namespace
}  // namespace tcmalloc
