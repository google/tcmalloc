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

#include <functional>
#include <string>

#include "gtest/gtest.h"
#include "absl/base/attributes.h"
#include "absl/base/const_init.h"
#include "absl/base/internal/low_level_alloc.h"
#include "absl/base/internal/spinlock.h"
#include "absl/base/thread_annotations.h"
#include "absl/container/flat_hash_set.h"
#include "absl/time/clock.h"
#include "absl/time/time.h"
#include "benchmark/benchmark.h"
#include "tcmalloc/internal/logging.h"
#include "tcmalloc/internal/parameter_accessors.h"
#include "tcmalloc/malloc_extension.h"
#include "tcmalloc/sampled_allocation.h"
#include "tcmalloc/static_vars.h"
#include "tcmalloc/testing/test_allocator_harness.h"
#include "tcmalloc/testing/thread_manager.h"

namespace tcmalloc {
namespace {

// Verify that heap profiling sessions concurrent with allocations/deallocations
// do not crash, as they all use `Static::sampled_allocation_recorder_`. Also
// check that the data in the sample make sense.
TEST(HeapProfilingTest, GetHeapProfileWhileAllocAndDealloc) {
  const int kThreads = 10;
  ThreadManager manager;
  AllocatorHarness harness(kThreads);

  // Some threads are busy with allocating and deallocating.
  manager.Start(kThreads, [&](int thread_id) { harness.Run(thread_id); });

  // Another few threads busy with iterating different kinds of heap profiles.
  for (auto t : {
           ProfileType::kHeap,
           ProfileType::kFragmentation,
           ProfileType::kPeakHeap,
       }) {
    manager.Start(2, [&](int) {
      MallocExtension::SnapshotCurrent(t).Iterate(
          [&](const Profile::Sample& s) {
            // Inspect a few fields in the sample.
            EXPECT_GT(s.sum, 0);
            EXPECT_GT(s.depth, 0);
            EXPECT_GT(s.requested_size, 0);
            EXPECT_GT(s.allocated_size, 0);
          });
    });
  }

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
