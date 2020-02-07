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
//
// This tests ReadStackTraces and ReadGrowthStackTraces.  It does this
// by doing a bunch of allocations and then calling those functions.
// A driver shell-script can call this, and then call pprof, and
// verify the expected output.  The output is written to
// argv[1].heap and argv[1].growth

#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <string>
#include <vector>

#include "benchmark/benchmark.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "absl/base/attributes.h"
#include "absl/debugging/symbolize.h"
#include "absl/strings/str_cat.h"
#include "absl/types/optional.h"
#include "tcmalloc/internal/logging.h"
#include "tcmalloc/malloc_extension.h"
#include "tcmalloc/testing/testutil.h"

namespace tcmalloc {
namespace {

bool StackMatches(const char *target, const void *const *stack, size_t len) {
  char buf[256];

  for (size_t i = 0; i < len; ++i) {
    if (!absl::Symbolize(stack[i], buf, sizeof(buf))) continue;
    if (strstr(buf, target) != nullptr) return true;
  }

  return false;
}

template <bool CheckSize>
size_t CountMatchingBytes(const char *target, Profile profile) {
  size_t sum = 0;
  profile.Iterate([&](const Profile::Sample &e) {
    if (e.requested_size == 10000 || !CheckSize) {
      if (StackMatches(target, e.stack, e.depth)) {
        sum += static_cast<size_t>(e.sum);
      }
    }
  });

  return sum;
}

ABSL_ATTRIBUTE_NOINLINE static void *AllocateAllocate(bool align) {
  void* p;
  if (align) {
    // A 10000 byte allocation aligned to 2K will use a 10K size class
    // and get 'charged' identically to malloc(10000).
    CHECK_CONDITION(posix_memalign(&p, 2048, 10000) == 0);
  } else {
    p = malloc(10000);
  }
  benchmark::DoNotOptimize(p);
  return p;
}

class SamplingTest : public testing::TestWithParam<int64_t> {};

TEST_P(SamplingTest, ParamChange) {
  static const size_t kIters = 80 * 1000;
  std::vector<void *> allocs;
  allocs.reserve(kIters * 2);

  ScopedGuardedSamplingRate gs(-1);
  size_t bytes;
  {
    ScopedProfileSamplingRate s(GetParam());
    for (int i = 0; i < kIters; ++i) {
      // Sample a mix of aligned and unaligned
      allocs.push_back(AllocateAllocate(i % 20 == 0));
    }

    bytes = CountMatchingBytes<true>(
        "AllocateAllocate",
        MallocExtension::SnapshotCurrent(ProfileType::kHeap));
    if (GetParam() > 0) {
      EXPECT_LE(500 * 1024 * 1024, bytes);
      EXPECT_GE(1000 * 1024 * 1024, bytes);
    } else {
      EXPECT_EQ(0, bytes);
    }
  }

  // We change back the samping parameter (~ScopedProfileSamplingRate above) and
  // allocate more, *without* deleting the old allocs--we should sample at the
  // new rate, and reweighting should correctly blend samples from before and
  // after the change.
  for (int i = 0; i < kIters; ++i) {
    allocs.push_back(AllocateAllocate(i % 20 == 0));
  }

  bytes = CountMatchingBytes<true>(
      "AllocateAllocate", MallocExtension::SnapshotCurrent(ProfileType::kHeap));
  if (GetParam() > 0) {
    EXPECT_LE(1000 * 1024 * 1024, bytes);
    EXPECT_GE(2000 * 1024 * 1024, bytes);
  } else {
    // samples that don't exist can't be reweighted properly
    EXPECT_LE(500 * 1024 * 1024, bytes);
    EXPECT_GE(1000 * 1024 * 1024, bytes);
  }

  for (auto p : allocs) {
    free(p);
  }
}

INSTANTIATE_TEST_SUITE_P(SampleParameters, SamplingTest,
                         testing::Values(0, 100000),
                         testing::PrintToStringParamName());

ABSL_ATTRIBUTE_NOINLINE static void *AllocateZeroByte() {
  void *p = ::operator new(0);
  ::benchmark::DoNotOptimize(p);
  return p;
}

TEST(Sampling, AlwaysSampling) {
  ScopedGuardedSamplingRate gs(-1);
  ScopedProfileSamplingRate s(1);

  static const size_t kIters = 80 * 1000;
  std::vector<void *> allocs;
  allocs.reserve(kIters);
  for (int i = 0; i < kIters; ++i) {
    allocs.push_back(AllocateZeroByte());
  }
  const absl::optional<size_t> alloc_size =
      MallocExtension::GetAllocatedSize(allocs[0]);
  ASSERT_THAT(alloc_size, testing::Ne(absl::nullopt));
  EXPECT_GT(*alloc_size, 0);

  size_t bytes = CountMatchingBytes<false>(
      "AllocateZeroByte", MallocExtension::SnapshotCurrent(ProfileType::kHeap));
  EXPECT_EQ(*alloc_size * kIters, bytes);

  for (void *p : allocs) {
    ::operator delete(p);
  }
}

}  // namespace
}  // namespace tcmalloc
