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
#include <new>

#include "gtest/gtest.h"
#include "benchmark/benchmark.h"
#include "tcmalloc/internal/logging.h"
#include "tcmalloc/internal/parameter_accessors.h"
#include "tcmalloc/malloc_extension.h"
#include "tcmalloc/static_vars.h"
#include "tcmalloc/testing/test_allocator_harness.h"
#include "tcmalloc/testing/thread_manager.h"

namespace tcmalloc {
namespace {

int64_t ProfileSize(ProfileType type) {
  int64_t total = 0;

  MallocExtension::SnapshotCurrent(type).Iterate(
      [&](const Profile::Sample& e) { total += e.sum; });
  return total;
}

class ScopedPeakGrowthFraction {
 public:
  explicit ScopedPeakGrowthFraction(double temporary_value)
      : previous_(TCMalloc_Internal_GetPeakSamplingHeapGrowthFraction()) {
    TCMalloc_Internal_SetPeakSamplingHeapGrowthFraction(temporary_value);
  }

  ~ScopedPeakGrowthFraction() {
    TCMalloc_Internal_SetPeakSamplingHeapGrowthFraction(previous_);
  }

 private:
  double previous_;
};

TEST(HeapProfilingTest, PeakHeapTracking) {
  // Adjust high watermark threshold for our scenario, to be independent of
  // changes to the default.  As we use a random value for choosing our next
  // sampling point, we may overweight some allocations above their true size.
  ScopedPeakGrowthFraction s(1.25);

  int64_t start_peak_sz = ProfileSize(ProfileType::kPeakHeap);

  // make a large allocation to force a new peak heap sample
  // (total live: 50MiB)
  void* first = ::operator new(50 << 20);
  // TODO(b/183453911): Remove workaround for GCC 10.x deleting operator new,
  // https://gcc.gnu.org/bugzilla/show_bug.cgi?id=94295.
  benchmark::DoNotOptimize(first);
  int64_t peak_after_first = ProfileSize(ProfileType::kPeakHeap);
  EXPECT_NEAR(peak_after_first, start_peak_sz + (50 << 20), 10 << 20);

  // a small allocation shouldn't increase the peak
  // (total live: 54MiB)
  void* second = ::operator new(4 << 20);
  benchmark::DoNotOptimize(second);
  int64_t peak_after_second = ProfileSize(ProfileType::kPeakHeap);
  EXPECT_EQ(peak_after_second, peak_after_first);

  // but a large one should
  // (total live: 254MiB)
  void* third = ::operator new(200 << 20);
  benchmark::DoNotOptimize(third);
  int64_t peak_after_third = ProfileSize(ProfileType::kPeakHeap);
  EXPECT_NEAR(peak_after_third, peak_after_second + (200 << 20), 10 << 20);

  // freeing everything shouldn't affect the peak
  // (total live: 0MiB)
  ::operator delete(first);
  EXPECT_EQ(ProfileSize(ProfileType::kPeakHeap), peak_after_third);

  ::operator delete(second);
  EXPECT_EQ(ProfileSize(ProfileType::kPeakHeap), peak_after_third);

  ::operator delete(third);
  EXPECT_EQ(ProfileSize(ProfileType::kPeakHeap), peak_after_third);

  // going back up less than previous peak shouldn't affect the peak
  // (total live: 200MiB)
  void* fourth = ::operator new(100 << 20);
  benchmark::DoNotOptimize(fourth);
  void* fifth = ::operator new(100 << 20);
  benchmark::DoNotOptimize(fifth);
  EXPECT_EQ(ProfileSize(ProfileType::kPeakHeap), peak_after_third);

  // passing the old peak significantly, even with many small allocations,
  // should generate a new one
  // (total live: 200MiB + 256MiB = 456MiB, 80% over the 254MiB peak)
  void* bitsy[1 << 10];
  for (int i = 0; i < 1 << 10; i++) {
    bitsy[i] = ::operator new(1 << 18);
    benchmark::DoNotOptimize(bitsy[i]);
  }
  EXPECT_GT(ProfileSize(ProfileType::kPeakHeap), peak_after_third);

  ::operator delete(fourth);
  ::operator delete(fifth);
  for (int i = 0; i < 1 << 10; i++) {
    ::operator delete(bitsy[i]);
  }
}

// Verify that heap profiling sessions concurrent with allocations/deallocations
// do not crash, as they all use `Static::sampled_allocation_recorder_`.
TEST(HeapProfilingTest, GetHeapProfileWhileAllocAndDealloc) {
  const int kThreads = 10;
  ThreadManager manager;
  AllocatorHarness harness(kThreads);

  // Some threads are busy with allocating and deallocating.
  manager.Start(kThreads, [&](int thread_id) { harness.Run(thread_id); });

  // Another two threads are busy with getting the heap profiles.
  manager.Start(
      2, [&](int) { MallocExtension::SnapshotCurrent(ProfileType::kHeap); });

  absl::SleepFor(absl::Seconds(3));
  manager.Stop();
}

TEST(HeapProfilingTest, AllocateDifferentSizes) {
  const int num_allocations = 1000;
  const size_t requested_size1 = (1 << 19) + 1;
  const size_t requested_size2 = (1 << 20) + 1;
  int requested_size1_count = 0;
  int requested_size2_count = 0;

  // First allocate some large objects at a specific size, verify through heap
  // profile, and deallocate them.
  void* allocations1[num_allocations];
  for (int i = 0; i < num_allocations; i++) {
    allocations1[i] = ::operator new(requested_size1);
  }

  MallocExtension::SnapshotCurrent(ProfileType::kHeap)
      .Iterate([&](const Profile::Sample& s) {
        if (s.requested_size == requested_size1) requested_size1_count++;
        if (s.requested_size == requested_size2) requested_size2_count++;
      });

  EXPECT_GT(requested_size1_count, 0);
  EXPECT_EQ(requested_size2_count, 0);
  requested_size1_count = 0;

  for (int i = 0; i < num_allocations; i++) {
    ::operator delete(allocations1[i]);
  }

  // Next allocate some large objects at a different size, verify through heap
  // profile, and deallocate them.
  void* allocations2[num_allocations];
  for (int i = 0; i < num_allocations; i++) {
    allocations2[i] = ::operator new(requested_size2);
  }

  MallocExtension::SnapshotCurrent(ProfileType::kHeap)
      .Iterate([&](const Profile::Sample& s) {
        if (s.requested_size == requested_size1) requested_size1_count++;
        if (s.requested_size == requested_size2) requested_size2_count++;
      });

  EXPECT_EQ(requested_size1_count, 0);
  EXPECT_GT(requested_size2_count, 0);

  for (int i = 0; i < num_allocations; i++) {
    ::operator delete(allocations2[i]);
  }
}

}  // namespace
}  // namespace tcmalloc
