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

#include <stdio.h>
#include <stdlib.h>

#include "gtest/gtest.h"
#include "absl/strings/str_format.h"
#include "tcmalloc/internal/linked_list.h"
#include "tcmalloc/internal/memory_stats.h"
#include "tcmalloc/malloc_extension.h"

namespace tcmalloc {
namespace {

int64_t VirtualProcessSize() {
  tcmalloc::tcmalloc_internal::MemoryStats stats;

  if (tcmalloc::tcmalloc_internal::GetMemoryStats(&stats)) {
    return stats.vss;
  } else {
    return 0;
  }
}

// This exercises memory fragmentation in the presence of calls to
// ReleaseMemoryToSystem().
//
// We need to consider returned memory for reuse (PageHeap::large_.returned) if
// it is a better fit than a a non-returned range (PageHeap::large_.normal).
// Greedily preferring normal could cause us to fragment a larger memory block
// in the normal list when we have a better returned candidate.
//
// If we have smaller, long-lived allocations interspersed with larger,
// short-lived allocations, we might encounter fragmentation.
TEST(LargeSmallFrag, Test) {
  static const int kLarge = 32 << 20;
  static const int kSmall = 1000;
  static const int kNumSmall = kLarge / kSmall + 10;

  // Force a small sample to initialize tagged page allocator.
  constexpr int64_t kAlloc = 8192;
  const int64_t num_allocs =
      32 * MallocExtension::GetProfileSamplingRate() / kAlloc;
  for (int64_t i = 0; i < num_allocs; ++i) {
    ::operator delete(::operator new(kAlloc));
  }

  // Chew up all possible memory that could be used to allocate
  // small objects.
  const int64_t vsize = VirtualProcessSize() / 1024 / 1024;
  LinkedList small;
  small.Init();
  while (VirtualProcessSize() / 1024 / 1024 == vsize) {
    small.Push(::operator new(kSmall));
  }

  // How much memory usage should be allowed (include some slop).
  //
  // Note that because of vagaries of internal allocation policies,
  // the retained small object may be placed in the space "reserved"
  // for the second large object.  That will cause the next iteration
  // to allocate a third large-object space. Therefore we allow the
  // virtual memory to grow to 3 * kLarge.
  int64_t allowed = VirtualProcessSize() + 3*kLarge + (10 << 20);

  // Fragmentation loop
  for (int iter = 0; iter < 1000; iter++) {
    ::operator delete(::operator new(kLarge));

    // Allocate some small objects and keep the middle one
    void* objects[kNumSmall];
    for (int i = 0; i < kNumSmall; i++) {
      objects[i] = ::operator new(kSmall);
    }
    for (int i = 0; i < kNumSmall; i++) {
      if (i == 50) {
        small.Push(objects[i]);
      } else {
        ::operator delete(objects[i]);
      }
    }
    allowed += 2*kSmall;

    MallocExtension::ReleaseMemoryToSystem(
        std::numeric_limits<size_t>::max());  // Simulate scavenging
    absl::FPrintF(stderr, "Iteration %5d ; Allowed: %d ; VSS %8.0f MB\n", iter,
                  allowed, VirtualProcessSize() / 1048576.0);
    EXPECT_LE(VirtualProcessSize(), allowed);
  }

  void* ptr;
  while (small.TryPop(&ptr)) {
    ::operator delete(ptr);
  }
}

}  // namespace
}  // namespace tcmalloc
