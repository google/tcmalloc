// Copyright 2026 The TCMalloc Authors
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

#include <algorithm>
#include <cstdint>
#include <iterator>
#include <vector>

#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "absl/base/internal/spinlock.h"
#include "absl/base/thread_annotations.h"
#include "absl/container/flat_hash_set.h"
#include "absl/numeric/bits.h"
#include "absl/types/span.h"
#include "tcmalloc/common.h"
#include "tcmalloc/internal/central_freelist_hooks.h"
#include "tcmalloc/internal/logging.h"
#include "tcmalloc/internal/parameter_accessors.h"
#include "tcmalloc/malloc_extension.h"
#include "tcmalloc/malloc_hook.h"

constexpr size_t kMaxTraceObjects =
    tcmalloc::tcmalloc_internal::kMaxObjectsToMove;

static absl::base_internal::SpinLock spinlock(
    absl::base_internal::SCHEDULE_KERNEL_ONLY);

static size_t remove_range_count ABSL_GUARDED_BY(spinlock) = 0;
static const void* remove_range_objects[kMaxTraceObjects] ABSL_GUARDED_BY(
    spinlock);

constexpr size_t kMaxNewObjects = 10000;
static size_t new_hook_count ABSL_GUARDED_BY(spinlock) = 0;
static const void* new_hook_objects[kMaxNewObjects] ABSL_GUARDED_BY(spinlock);

static uint64_t first_remove_seq ABSL_GUARDED_BY(spinlock) = 0;

// Record multiple batches to handle different caching behaviors.
static void RecordRemoveRangeBatchHook(size_t size_class,
                                       absl::Span<void*> batch, uint64_t seq) {
  absl::base_internal::SpinLockHolder l(spinlock);

  if (first_remove_seq == 0) {
    first_remove_seq = seq;
  }
  if (remove_range_count + batch.size() <= kMaxTraceObjects) {
    for (void* ptr : batch) {
      remove_range_objects[remove_range_count++] = ptr;
    }
  }
}

// Record multiple allocations to increase the chance of finding a match.
static void RecordNewHook(const tcmalloc::MallocHook::NewInfo& info) {
  if (info.allocated_size > tcmalloc::tcmalloc_internal::kMaxSize) {
    return;
  }

  absl::base_internal::SpinLockHolder l(spinlock);
  if (new_hook_count < kMaxNewObjects) {
    new_hook_objects[new_hook_count++] = info.ptr;
  }
}

extern "C" void MallocHook_InitAtFirstAllocation_ForTesting() {
  TC_CHECK(tcmalloc::MallocHook::AddNewHook(RecordNewHook));
}

extern "C" void TCMalloc_CentralFreeList_InitAtFirstRemoveRange_Tracing() {
  TC_CHECK(tcmalloc::tcmalloc_internal::central_freelist_remove_range_hooks.Add(
      RecordRemoveRangeBatchHook));
}

namespace tcmalloc {
namespace {

TEST(CentralFreeListTracingTest, CalledEarly) {
  if (tcmalloc_internal::kSanitizerPresent) {
    GTEST_SKIP() << "Skipping under sanitizers";
  }

  // Verify that our first callback from RemoveRange corresponds to an size
  // class-ful object we returned via NewHook.

  // Google Test will certainly allocate before here, but guarantee that we
  // have.
  ::operator delete(::operator new(16));

  // Stop recording allocations.
  TC_CHECK(tcmalloc::MallocHook::RemoveNewHook(RecordNewHook));
  TC_CHECK(
      tcmalloc::tcmalloc_internal::central_freelist_remove_range_hooks.Remove(
          RecordRemoveRangeBatchHook));

  absl::flat_hash_set<const void*> cfl;
  cfl.reserve(kMaxTraceObjects);

  bool found = false;
  {
    absl::base_internal::SpinLockHolder l(spinlock);

    for (size_t i = 0; i < remove_range_count; ++i) {
      cfl.insert(remove_range_objects[i]);
    }

    for (size_t i = 0; i < new_hook_count; ++i) {
      if (cfl.contains(new_hook_objects[i])) {
        found = true;
        break;
      }
    }

    EXPECT_FALSE(cfl.empty());
    EXPECT_NE(new_hook_count, 0);
    EXPECT_GT(first_remove_seq, 0);
  }

  EXPECT_TRUE(found) << "None of the early allocations were found in the "
                        "central freelist batch";
}

TEST(CentralFreeListTracingTest, SnapshotCentralFreeLists) {
  if (tcmalloc_internal::kSanitizerPresent) {
    GTEST_SKIP() << "Skipping under sanitizers";
  }
  ASSERT_NE(TCMalloc_Internal_SnapshotCentralFreeLists, nullptr);
  ASSERT_NE(TCMalloc_Internal_GetCflSubbucketPrioritization, nullptr);
  (void)TCMalloc_Internal_GetCflSubbucketPrioritization();

  const int64_t prev_sampling = MallocExtension::GetProfileSamplingInterval();
  MallocExtension::SetProfileSamplingInterval(0);

  constexpr size_t kAllocSize = 1024;
  constexpr size_t kTotalAllocs = 16384;
  std::vector<void*> live;
  live.reserve(kTotalAllocs);
  for (size_t i = 0; i < kTotalAllocs; ++i) {
    live.push_back(::operator new(kAllocSize));
  }
  // Free every second object in the upper three quarters (6 MiB total) so the
  // lower quarter keeps full spans while the freed objects overflow frontend
  // caches into CentralFreeList without emptying any span.
  std::vector<void*> kept;
  kept.reserve(kTotalAllocs);
  for (size_t i = 0; i < kTotalAllocs; ++i) {
    if (i >= kTotalAllocs / 4 && (i % 2) == 1) {
      ::operator delete(live[i], kAllocSize);
    } else {
      kept.push_back(live[i]);
    }
  }
  MallocExtension::MarkThreadIdle();

  std::vector<tcmalloc_internal::TracerSizeClassInfo> classes;
  TCMalloc_Internal_GetSizeClasses(&classes);

  tcmalloc_internal::CentralFreeListSnapshot snap;
  TCMalloc_Internal_SnapshotCentralFreeLists(&snap);
  EXPECT_EQ(snap.class_seqs.size(), classes.size());

  size_t matched_kept = 0;
  bool saw_full_span = false;
  bool saw_nonempty_span = false;
  for (const auto& s : snap.spans) {
    ASSERT_LT(s.size_class, classes.size());
    const size_t obj_size = classes[s.size_class].size;
    if (obj_size != kAllocSize) {
      continue;
    }
    if (s.bitmap_word_offset == UINT32_MAX) {
      EXPECT_EQ(s.allocated_objects, s.objects_per_span);
      saw_full_span = true;
    } else {
      saw_nonempty_span = true;
      const size_t words = (s.objects_per_span + 63) / 64;
      ASSERT_LE(s.bitmap_word_offset + words, snap.allocated_bitmaps.size());
      size_t bits = 0;
      for (size_t w = 0; w < words; ++w) {
        bits +=
            absl::popcount(snap.allocated_bitmaps[s.bitmap_word_offset + w]);
      }
      EXPECT_EQ(bits, s.allocated_objects);
    }
  }

  // Every kept allocation must sit on an allocated slot in the snapshot.
  std::vector<const tcmalloc_internal::CentralFreeListSpanSnapshot*> by_addr;
  by_addr.reserve(snap.spans.size());
  for (const auto& s : snap.spans) {
    by_addr.push_back(&s);
  }
  std::sort(by_addr.begin(), by_addr.end(), [](const auto* a, const auto* b) {
    return a->start_address < b->start_address;
  });
  for (void* p : kept) {
    const uintptr_t addr = reinterpret_cast<uintptr_t>(p);
    auto it = std::upper_bound(
        by_addr.begin(), by_addr.end(), addr,
        [](uintptr_t a, const auto* s) { return a < s->start_address; });
    if (it == by_addr.begin()) {
      continue;
    }
    const auto& s = **std::prev(it);
    const size_t span_bytes = classes[s.size_class].span_size_in_bytes;
    if (addr >= s.start_address && addr < s.start_address + span_bytes) {
      const size_t obj_size = classes[s.size_class].size;
      const size_t idx = (addr - s.start_address) / obj_size;
      if (s.bitmap_word_offset == UINT32_MAX) {
        ++matched_kept;
      } else {
        const uint64_t word =
            snap.allocated_bitmaps[s.bitmap_word_offset + idx / 64];
        if ((word & (uint64_t{1} << (idx % 64))) != 0) {
          ++matched_kept;
        }
      }
    }
  }
  EXPECT_EQ(matched_kept, kept.size());
  EXPECT_TRUE(saw_full_span);
  EXPECT_TRUE(saw_nonempty_span);

  for (void* p : kept) {
    ::operator delete(p, kAllocSize);
  }
  MallocExtension::SetProfileSamplingInterval(prev_sampling);
}

}  // namespace
}  // namespace tcmalloc
