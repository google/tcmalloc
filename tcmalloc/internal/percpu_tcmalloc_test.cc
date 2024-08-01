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

#include "tcmalloc/internal/percpu_tcmalloc.h"

#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <stddef.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <unistd.h>

#include <cstring>

#include "absl/functional/function_ref.h"
#include "tcmalloc/internal/cpu_utils.h"
#include "tcmalloc/internal/percpu.h"

#if defined(__linux__)
#include <linux/param.h>
#else
#include <sys/param.h>
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <new>
#include <optional>
#include <string>
#include <thread>  // NOLINT(build/c++11)
#include <tuple>
#include <utility>
#include <vector>

#include "benchmark/benchmark.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "absl/base/call_once.h"
#include "absl/base/thread_annotations.h"
#include "absl/container/fixed_array.h"
#include "absl/container/flat_hash_set.h"
#include "absl/random/random.h"
#include "absl/random/seed_sequences.h"
#include "absl/synchronization/mutex.h"
#include "absl/time/clock.h"
#include "absl/time/time.h"
#include "absl/types/span.h"
#include "tcmalloc/internal/affinity.h"
#include "tcmalloc/internal/config.h"
#include "tcmalloc/internal/logging.h"
#include "tcmalloc/internal/page_size.h"
#include "tcmalloc/internal/sysinfo.h"
#include "tcmalloc/internal/util.h"
#include "tcmalloc/malloc_extension.h"
#include "tcmalloc/testing/testutil.h"

namespace tcmalloc {
namespace tcmalloc_internal {
namespace subtle {
namespace percpu {
namespace {

using testing::Each;
using testing::UnorderedElementsAreArray;

constexpr size_t kStressSlabs = 5;
constexpr size_t kStressCapacity = 4;
constexpr size_t kMaxStressCapacity = kStressCapacity * 2;

constexpr size_t kShift = 18;
typedef class TcmallocSlab<kStressSlabs> TcmallocSlab;

void* AllocSlabs(absl::FunctionRef<void*(size_t, std::align_val_t)> alloc,
                 size_t raw_shift) {
  Shift shift = ToShiftType(raw_shift);
  const size_t slabs_size = GetSlabsAllocSize(shift, NumCPUs());
  return alloc(slabs_size, kPhysicalPageAlign);
}

void InitSlab(TcmallocSlab& slab,
              absl::FunctionRef<void*(size_t, std::align_val_t)> alloc,
              absl::FunctionRef<size_t(size_t)> capacity, size_t raw_shift) {
  void* slabs = AllocSlabs(alloc, raw_shift);
  slab.Init(alloc, slabs, capacity, ToShiftType(raw_shift));
}

struct GetMaxCapacity {
  size_t operator()(size_t size_class) const {
    if (size_class >= kStressSlabs) return 0;
    return max_capacities[size_class].load(std::memory_order_relaxed);
  }

  const std::atomic<size_t>* max_capacities;
};

class TcmallocSlabTest : public testing::Test {
 public:
  TcmallocSlabTest() {
// Ignore false-positive warning in GCC. For more information, see:
// https://gcc.gnu.org/bugzilla/show_bug.cgi?id=96003
#pragma GCC diagnostic ignored "-Wnonnull"
    InitSlab(
        slab_,
        [&](size_t size, std::align_val_t align) {
          return ByteCountingMalloc(size, align);
        },
        [](size_t) { return kCapacity; }, kShift);
  }

  ~TcmallocSlabTest() override { slab_.Destroy(sized_aligned_delete); }

  void* ByteCountingMalloc(size_t size, std::align_val_t alignment) {
    void* ptr = ::operator new(size, alignment);
    // Emulate obtaining memory as if we got it from mmap (zero'd).
    memset(ptr, 0, size);
    if (static_cast<size_t>(alignment) >= GetPageSize()) {
      madvise(ptr, size, MADV_DONTNEED);
    }
    metadata_bytes_ += size;
    return ptr;
  }

  TcmallocSlab slab_;
  static constexpr size_t kCapacity = 10;
  size_t metadata_bytes_ = 0;
};

TEST_F(TcmallocSlabTest, Metadata) {
  PerCPUMetadataState r = slab_.MetadataMemoryUsage();

  ASSERT_GT(metadata_bytes_, 0);
  EXPECT_EQ(r.virtual_size, metadata_bytes_);
  EXPECT_EQ(r.resident_size, 0);

  if (!IsFast()) {
    GTEST_SKIP() << "Need fast percpu. Skipping.";
    return;
  }

  // Initialize a core.  Verify that the increased RSS is proportional to a
  // core.
  slab_.InitCpu(0, [](size_t size_class) { return kCapacity; });

  r = slab_.MetadataMemoryUsage();
  // We may fault a whole hugepage, so round up the expected per-core share to
  // a full hugepage.
  size_t expected = r.virtual_size / NumCPUs();
  expected = (expected + kHugePageSize - 1) & ~(kHugePageSize - 1);

  // A single core may be less than the full slab for that core, since we do
  // not touch every page within the slab.
  EXPECT_GE(expected, r.resident_size);
  // We expect to have touched at least one page, so resident size should be a
  // non-zero number of bytes.
  EXPECT_GT(r.resident_size, 0);

  // Read stats from the slab.  This will fault additional memory.
  for (int cpu = 0, n = NumCPUs(); cpu < n; ++cpu) {
    // To inhibit optimization, verify the values are sensible.
    for (int size_class = 1; size_class < kStressSlabs; ++size_class) {
      EXPECT_EQ(0, slab_.Length(cpu, size_class));
      EXPECT_EQ(0, slab_.Capacity(cpu, size_class));
    }
  }

  PerCPUMetadataState post_stats = slab_.MetadataMemoryUsage();
  EXPECT_LE(post_stats.resident_size, metadata_bytes_);
  EXPECT_GT(post_stats.resident_size, r.resident_size);
}

TEST_F(TcmallocSlabTest, Unit) {
  if (MallocExtension::PerCpuCachesActive()) {
    // This test unregisters rseq temporarily, as to decrease flakiness.
    GTEST_SKIP() << "per-CPU TCMalloc is incompatible with unregistering rseq";
  }

  if (!IsFast()) {
    GTEST_SKIP() << "Need fast percpu. Skipping.";
    return;
  }

  // Decide if we should expect a push or pop to be the first action on the CPU
  // slab to trigger initialization.
  absl::FixedArray<bool, 0> initialized(NumCPUs(), false);

  void* objects[kCapacity];
  void* object_ptrs[kCapacity];
  for (int i = 0; i < kCapacity; ++i) {
    object_ptrs[i] = &objects[i];
  }

  for (auto cpu : AllowedCpus()) {
    SCOPED_TRACE(cpu);

    // Temporarily fake being on the given CPU.
    ScopedFakeCpuId fake_cpu_id(cpu);

    for (size_t size_class = 1; size_class < kStressSlabs; ++size_class) {
      SCOPED_TRACE(size_class);

      // Check new slab state.
      ASSERT_EQ(slab_.Length(cpu, size_class), 0);
      ASSERT_EQ(slab_.Capacity(cpu, size_class), 0);

      if (!initialized[cpu]) {
        ASSERT_EQ(slab_.Pop(size_class), nullptr);
        slab_.InitCpu(cpu, [](size_t size_class) { return kCapacity; });
        initialized[cpu] = true;
      }

      // Test that operations on uncached slab fail.
      ASSERT_EQ(slab_.Pop(size_class), nullptr);
      EXPECT_FALSE(slab_.Push(size_class, &objects[0]));
      EXPECT_FALSE(slab_.Push(size_class, &objects[0]));
      EXPECT_FALSE(slab_.Push(size_class, &objects[0]));
      const auto max_capacity = [](uint8_t shift) { return kCapacity; };
      ASSERT_EQ(slab_.Grow(cpu, size_class, 1, max_capacity), 0);
      {
        auto [got_cpu, cached] = slab_.CacheCpuSlab();
        ASSERT_TRUE(cached);
        ASSERT_EQ(got_cpu, cpu);
      }
      {
        auto [got_cpu, cached] = slab_.CacheCpuSlab();
        ASSERT_FALSE(cached);
        ASSERT_EQ(got_cpu, cpu);
      }

      // Grow capacity to kCapacity / 2.
      ASSERT_EQ(slab_.Grow(cpu, size_class, kCapacity / 2, max_capacity),
                kCapacity / 2);
      ASSERT_EQ(slab_.Length(cpu, size_class), 0);
      ASSERT_EQ(slab_.Capacity(cpu, size_class), kCapacity / 2);
      ASSERT_EQ(slab_.Pop(size_class), nullptr);
      ASSERT_TRUE(slab_.Push(size_class, &objects[0]));

      ASSERT_EQ(slab_.Length(cpu, size_class), 1);
      ASSERT_EQ(slab_.Capacity(cpu, size_class), kCapacity / 2);
      ASSERT_EQ(slab_.Pop(size_class), &objects[0]);
      ASSERT_EQ(slab_.Length(cpu, size_class), 0);
      for (size_t i = 0; i < kCapacity / 2; ++i) {
        ASSERT_TRUE(slab_.Push(size_class, &objects[i]));
        ASSERT_EQ(slab_.Length(cpu, size_class), i + 1);
      }
      EXPECT_FALSE(slab_.Push(size_class, &objects[0]));
      for (size_t i = kCapacity / 2; i > 0; --i) {
        ASSERT_EQ(slab_.Pop(size_class), &objects[i - 1]);
        ASSERT_EQ(slab_.Length(cpu, size_class), i - 1);
      }

      // Grow capacity to kCapacity and ensure that grow don't overflow max
      // capacity.
      ASSERT_EQ(slab_.Grow(cpu, size_class, kCapacity, max_capacity),
                kCapacity / 2);
      ASSERT_EQ(slab_.Capacity(cpu, size_class), kCapacity);
      for (size_t i = 0; i < kCapacity; ++i) {
        ASSERT_TRUE(slab_.Push(size_class, &objects[i]));
        ASSERT_EQ(slab_.Length(cpu, size_class), i + 1);
      }
      EXPECT_FALSE(slab_.Push(size_class, &objects[0]));
      for (size_t i = kCapacity; i > 0; --i) {
        ASSERT_EQ(slab_.Pop(size_class), &objects[i - 1]);
        ASSERT_EQ(slab_.Length(cpu, size_class), i - 1);
      }

      // Test Drain.
      ASSERT_TRUE(slab_.Push(size_class, &objects[0]));
      ASSERT_TRUE(slab_.Push(size_class, &objects[1]));

      slab_.Drain(cpu, [size_class, cpu, &objects](
                           int cpu_arg, size_t size_class_arg, void** batch,
                           size_t size, size_t cap) {
        ASSERT_EQ(cpu, cpu_arg);
        if (size_class == size_class_arg) {
          ASSERT_EQ(size, 2);
          ASSERT_EQ(cap, 10);
          ASSERT_EQ(batch[0], &objects[0]);
          ASSERT_EQ(batch[1], &objects[1]);
        } else {
          ASSERT_EQ(size, 0);
          ASSERT_EQ(cap, 0);
        }
      });
      ASSERT_EQ(slab_.Length(cpu, size_class), 0);
      ASSERT_EQ(slab_.Capacity(cpu, size_class), 0);

      // Test PushBatch/PopBatch.
      void* batch[kCapacity + 1];
      for (size_t i = 0; i < kCapacity; ++i) {
        batch[i] = &objects[i];
      }
      void* slabs_result[kCapacity + 1];
      ASSERT_EQ(slab_.PopBatch(size_class, batch, kCapacity), 0);
      ASSERT_EQ(slab_.PushBatch(size_class, batch, kCapacity), 0);
      ASSERT_EQ(slab_.Grow(cpu, size_class, kCapacity / 2, max_capacity),
                kCapacity / 2);
      ASSERT_EQ(slab_.PopBatch(size_class, batch, kCapacity), 0);
      // Push a batch of size i into empty slab.
      for (size_t i = 1; i < kCapacity; ++i) {
        const size_t expect = std::min(i, kCapacity / 2);
        ASSERT_EQ(slab_.PushBatch(size_class, batch, i), expect);
        ASSERT_EQ(slab_.Length(cpu, size_class), expect);
        for (size_t j = 0; j < expect; ++j) {
          slabs_result[j] = slab_.Pop(size_class);
        }
        ASSERT_THAT(
            std::vector<void*>(&slabs_result[0], &slabs_result[expect]),
            UnorderedElementsAreArray(&object_ptrs[i - expect], expect));
        ASSERT_EQ(slab_.Pop(size_class), nullptr);
      }
      // Push a batch of size i into non-empty slab.
      for (size_t i = 1; i < kCapacity / 2; ++i) {
        const size_t expect = std::min(i, kCapacity / 2 - i);
        ASSERT_EQ(slab_.PushBatch(size_class, batch, i), i);
        ASSERT_EQ(slab_.PushBatch(size_class, batch, i), expect);
        ASSERT_EQ(slab_.Length(cpu, size_class), i + expect);
        // Because slabs are LIFO fill in this array from the end.
        for (int j = i + expect - 1; j >= 0; --j) {
          slabs_result[j] = slab_.Pop(size_class);
        }
        ASSERT_THAT(std::vector<void*>(&slabs_result[0], &slabs_result[i]),
                    UnorderedElementsAreArray(&object_ptrs[0], i));
        ASSERT_THAT(
            std::vector<void*>(&slabs_result[i], &slabs_result[i + expect]),
            UnorderedElementsAreArray(&object_ptrs[i - expect], expect));
        ASSERT_EQ(slab_.Pop(size_class), nullptr);
      }
      for (size_t i = 0; i < kCapacity + 1; ++i) {
        batch[i] = nullptr;
      }
      // Pop all elements in a single batch.
      for (size_t i = 1; i < kCapacity / 2; ++i) {
        for (size_t j = 0; j < i; ++j) {
          ASSERT_TRUE(slab_.Push(size_class, &objects[j]));
        }
        ASSERT_EQ(slab_.PopBatch(size_class, batch, i), i);
        ASSERT_EQ(slab_.Length(cpu, size_class), 0);
        ASSERT_EQ(slab_.Pop(size_class), nullptr);

        ASSERT_THAT(absl::MakeSpan(&batch[0], i),
                    UnorderedElementsAreArray(&object_ptrs[0], i));
        ASSERT_THAT(absl::MakeSpan(&batch[i], kCapacity - i), Each(nullptr));
        for (size_t j = 0; j < kCapacity + 1; ++j) {
          batch[j] = nullptr;
        }
      }
      // Pop half of elements in a single batch.
      for (size_t i = 1; i < kCapacity / 2; ++i) {
        for (size_t j = 0; j < i; ++j) {
          ASSERT_TRUE(slab_.Push(size_class, &objects[j]));
        }
        size_t want = std::max<size_t>(1, i / 2);
        ASSERT_EQ(slab_.PopBatch(size_class, batch, want), want);
        ASSERT_EQ(slab_.Length(cpu, size_class), i - want);

        for (size_t j = 0; j < i - want; ++j) {
          ASSERT_EQ(slab_.Pop(size_class), &objects[i - want - j - 1]);
        }

        ASSERT_EQ(slab_.Pop(size_class), nullptr);

        ASSERT_GE(i, want);
        ASSERT_THAT(absl::MakeSpan(&batch[0], want),
                    UnorderedElementsAreArray(&object_ptrs[i - want], want));
        ASSERT_THAT(absl::MakeSpan(&batch[want], kCapacity - want),
                    Each(nullptr));
        for (size_t j = 0; j < kCapacity + 1; ++j) {
          batch[j] = nullptr;
        }
      }
      // Pop 2x elements in a single batch.
      for (size_t i = 1; i < kCapacity / 2; ++i) {
        for (size_t j = 0; j < i; ++j) {
          ASSERT_TRUE(slab_.Push(size_class, &objects[j]));
        }
        ASSERT_EQ(slab_.PopBatch(size_class, batch, i * 2), i);
        ASSERT_EQ(slab_.Length(cpu, size_class), 0);
        ASSERT_EQ(slab_.Pop(size_class), nullptr);

        ASSERT_THAT(absl::MakeSpan(&batch[0], i),
                    UnorderedElementsAreArray(&object_ptrs[0], i));
        ASSERT_THAT(absl::MakeSpan(&batch[i], kCapacity - i), Each(nullptr));
        for (size_t j = 0; j < kCapacity + 1; ++j) {
          batch[j] = nullptr;
        }
      }

      slab_.Drain(cpu,
                  [size_class, cpu](int cpu_arg, size_t size_class_arg,
                                    void** batch, size_t size, size_t cap) {
                    ASSERT_EQ(cpu, cpu_arg);
                    if (size_class == size_class_arg) {
                      ASSERT_EQ(size, 0);
                      ASSERT_EQ(cap, 5);
                    } else {
                      ASSERT_EQ(size, 0);
                      ASSERT_EQ(cap, 0);
                    }
                  });
      ASSERT_EQ(slab_.Length(cpu, size_class), 0);
      ASSERT_EQ(slab_.Capacity(cpu, size_class), 0);
      slab_.UncacheCpuSlab();
    }
  }
}

void* allocator(size_t bytes, std::align_val_t alignment) {
  void* ptr = ::operator new(bytes, alignment);
  memset(ptr, 0, bytes);
  return ptr;
}

TEST_F(TcmallocSlabTest, ResizeMaxCapacities) {
  if (MallocExtension::PerCpuCachesActive()) {
    // This test unregisters rseq temporarily, as to decrease flakiness.
    GTEST_SKIP() << "per-CPU TCMalloc is incompatible with unregistering rseq";
  }

  if (!IsFast()) {
    GTEST_SKIP() << "Need fast percpu. Skipping.";
    return;
  }
  constexpr int kCpu = 1;
  constexpr int kSizeClassToGrow = 1;
  constexpr int kSizeClassToShrink = 2;
  ASSERT_LT(kSizeClassToShrink, kStressSlabs);
  ASSERT_LT(kSizeClassToGrow, kStressSlabs);

  ScopedFakeCpuId fake_cpu_id(kCpu);
  slab_.InitCpu(kCpu, [](size_t size_class) { return kCapacity; });
  {
    auto [got_cpu, cached] = slab_.CacheCpuSlab();
    ASSERT_TRUE(cached);
    ASSERT_EQ(got_cpu, kCpu);
  }

  size_t max_capacity[kStressSlabs] = {0};
  max_capacity[kSizeClassToShrink] = kCapacity;
  max_capacity[kSizeClassToGrow] = kCapacity;

  // Make sure that the slab may grow the available maximum capacity.
  EXPECT_EQ(slab_.Grow(kCpu, kSizeClassToGrow, max_capacity[kSizeClassToGrow],
                       [&](uint8_t) { return max_capacity[kSizeClassToGrow]; }),
            max_capacity[kSizeClassToGrow]);
  EXPECT_EQ(
      slab_.Grow(kCpu, kSizeClassToShrink, max_capacity[kSizeClassToShrink],
                 [&](uint8_t) { return max_capacity[kSizeClassToShrink]; }),
      max_capacity[kSizeClassToShrink]);

  for (int i = 0; i < kCapacity; ++i) {
    PerSizeClassMaxCapacity new_max_capacity[2];
    new_max_capacity[0] = PerSizeClassMaxCapacity{
        .size_class = kSizeClassToGrow,
        .max_capacity = max_capacity[kSizeClassToGrow] + 1};
    new_max_capacity[1] = PerSizeClassMaxCapacity{
        .size_class = kSizeClassToShrink,
        .max_capacity = max_capacity[kSizeClassToShrink] - 1};
    void* slabs = AllocSlabs(allocator, kShift);
    const auto [old_slabs, old_slabs_size] = slab_.UpdateMaxCapacities(
        slabs, [&](size_t size_class) { return max_capacity[size_class]; },
        [&](int size, uint16_t cap) { max_capacity[size] = cap; },
        [](int cpu) { return cpu == kCpu; },
        [&](int cpu, size_t size_class, void** batch, size_t size, size_t cap) {
          EXPECT_EQ(size, 0);
        },
        new_max_capacity,
        /*classes_to_resize=*/2);
    ASSERT_NE(old_slabs, nullptr);
    mprotect(old_slabs, old_slabs_size, PROT_READ | PROT_WRITE);
    sized_aligned_delete(old_slabs, old_slabs_size,
                         std::align_val_t{EXEC_PAGESIZE});

    // Make sure that the capacity is zero as UpdateMaxCapacity should
    // initialize slabs.
    EXPECT_EQ(slab_.Capacity(kCpu, kSizeClassToGrow), 0);
    EXPECT_EQ(slab_.Capacity(kCpu, kSizeClassToShrink), 0);

    // Make sure that the slab may grow the available maximum capacity.
    EXPECT_EQ(
        slab_.Grow(kCpu, kSizeClassToGrow, max_capacity[kSizeClassToGrow],
                   [&](uint8_t) { return max_capacity[kSizeClassToGrow]; }),
        max_capacity[kSizeClassToGrow]);
    EXPECT_EQ(
        slab_.Grow(kCpu, kSizeClassToShrink, max_capacity[kSizeClassToShrink],
                   [&](uint8_t) { return max_capacity[kSizeClassToShrink]; }),
        max_capacity[kSizeClassToShrink]);
  }

  EXPECT_EQ(max_capacity[kSizeClassToShrink], 0);
  EXPECT_EQ(max_capacity[kSizeClassToGrow], 2 * kCapacity);
}

TEST_F(TcmallocSlabTest, ShrinkEmptyCache) {
  if (MallocExtension::PerCpuCachesActive()) {
    // This test unregisters rseq temporarily, as to decrease flakiness.
    GTEST_SKIP() << "per-CPU TCMalloc is incompatible with unregistering rseq";
  }

  if (!IsFast()) {
    GTEST_SKIP() << "Need fast percpu. Skipping.";
    return;
  }
  constexpr int kCpu = 1;
  constexpr int kSizeClass = 1;
  slab_.InitCpu(kCpu, [](size_t size_class) { return kCapacity; });
  slab_.StopCpu(kCpu);
  EXPECT_EQ(
      slab_.ShrinkOtherCache(kCpu, kSizeClass, /*len=*/1,
                             [](size_t size_class, void** batch, size_t n) {
                               EXPECT_LT(size_class, kStressSlabs);
                               EXPECT_LE(n, kStressCapacity);
                               EXPECT_GT(n, 0);
                               for (size_t i = 0; i < n; ++i) {
                                 EXPECT_NE(batch[i], nullptr);
                               }
                             }),
      0);
  slab_.StartCpu(kCpu);
}

TEST_F(TcmallocSlabTest, SimulatedMadviseFailure) {
  if (!IsFast()) {
    GTEST_SKIP() << "Need fast percpu. Skipping.";
    return;
  }

  // Initialize a core.
  slab_.InitCpu(0, [](size_t size_class) { return kCapacity; });

  auto trigger_resize = [&](size_t shift) {
    // We are deliberately simulating madvise failing, so ignore the return
    // value.
    auto alloc = [&](size_t size, std::align_val_t alignment) {
      return ByteCountingMalloc(size, alignment);
    };
    void* slabs = AllocSlabs(alloc, shift);
    (void)slab_.ResizeSlabs(
        subtle::percpu::ToShiftType(shift), slabs,
        [](size_t) { return kCapacity / 2; }, [](int cpu) { return cpu == 0; },
        [&](int cpu, size_t size_class, void** batch, size_t size, size_t cap) {
          EXPECT_EQ(size, 0);
          EXPECT_EQ(cap, 0);
        });
  };

  // We need to switch from one size (kShift) to another (kShift - 1) and back.
  trigger_resize(kShift - 1);
  trigger_resize(kShift);
}

struct Context {
  TcmallocSlab* slab;
  std::vector<std::vector<void*>>* blocks;
  absl::Span<absl::Mutex> mutexes;
  std::atomic<size_t>* capacity;
  std::atomic<bool>* stop;
  absl::Span<absl::once_flag> init;
  absl::Span<std::atomic<bool>> has_init;
  std::atomic<size_t>* max_capacity;

  GetMaxCapacity GetMaxCapacityFunctor() const { return {max_capacity}; }
};

void InitCpuOnce(Context& ctx, int cpu) {
  if (cpu < 0) {
    cpu = ctx.slab->CacheCpuSlab().first;
    if (cpu < 0) {
      return;
    }
  }
  absl::base_internal::LowLevelCallOnce(&ctx.init[cpu], [&]() {
    absl::MutexLock lock(&ctx.mutexes[cpu]);
    ctx.slab->InitCpu(cpu, ctx.GetMaxCapacityFunctor());
    ctx.has_init[cpu].store(true, std::memory_order_relaxed);
  });
}

int GetResizedMaxCapacities(Context& ctx,
                            PerSizeClassMaxCapacity* new_max_capacity) {
  std::atomic<size_t>* max_capacity = ctx.max_capacity;
  absl::BitGen rnd;
  size_t to_shrink = absl::Uniform<int32_t>(rnd, 0, kStressSlabs);
  size_t to_grow = absl::Uniform<int32_t>(rnd, 0, kStressSlabs);
  if (to_shrink == to_grow || max_capacity[to_shrink] == 0 ||
      max_capacity[to_grow].load(std::memory_order_relaxed) ==
          kMaxStressCapacity - 1)
    return 0;
  new_max_capacity[0] = PerSizeClassMaxCapacity{
      .size_class = to_shrink,
      .max_capacity =
          max_capacity[to_shrink].load(std::memory_order_relaxed) - 1};
  new_max_capacity[1] = PerSizeClassMaxCapacity{
      .size_class = to_grow,
      .max_capacity =
          max_capacity[to_grow].load(std::memory_order_relaxed) + 1};
  return 2;
}

// TODO(b/213923453): move to an environment style of test, as in
// FakeTransferCacheEnvironment.
void StressThread(size_t thread_id,
                  Context& ctx) ABSL_NO_THREAD_SAFETY_ANALYSIS {
  EXPECT_TRUE(IsFast());

  std::vector<void*>& block = (*ctx.blocks)[thread_id];

  const int num_cpus = NumCPUs();
  absl::BitGen rnd(absl::SeedSeq({thread_id}));
  while (!*ctx.stop) {
    size_t size_class = absl::Uniform<int32_t>(rnd, 1, kStressSlabs);
    const int what = absl::Uniform<int32_t>(rnd, 0, 91);
    if (what < 10) {
      if (!block.empty()) {
        if (ctx.slab->Push(size_class, block.back())) {
          block.pop_back();
        } else {
          InitCpuOnce(ctx, -1);
        }
      }
    } else if (what < 20) {
      if (void* item = ctx.slab->Pop(size_class)) {
        // Ensure that we never return a null item which could be indicative
        // of a bug in lazy InitCpu initialization (b/148973091, b/147974701).
        EXPECT_NE(item, nullptr);
        block.push_back(item);
      } else {
        InitCpuOnce(ctx, -1);
      }
    } else if (what < 30) {
      if (!block.empty()) {
        void* batch[kStressCapacity];
        size_t n = absl::Uniform<int32_t>(
                       rnd, 0, std::min(block.size(), kStressCapacity)) +
                   1;
        for (size_t i = 0; i < n; ++i) {
          batch[i] = block.back();
          block.pop_back();
        }
        size_t pushed = ctx.slab->PushBatch(size_class, batch, n);
        EXPECT_LE(pushed, n);
        for (size_t i = 0; i < n - pushed; ++i) {
          block.push_back(batch[i]);
        }
      }
    } else if (what < 40) {
      void* batch[kStressCapacity];
      size_t n = absl::Uniform<int32_t>(rnd, 0, kStressCapacity) + 1;
      size_t popped = ctx.slab->PopBatch(size_class, batch, n);
      EXPECT_LE(popped, n);
      for (size_t i = 0; i < popped; ++i) {
        block.push_back(batch[i]);
      }
    } else if (what < 50) {
      size_t n = absl::Uniform<int32_t>(rnd, 0, kStressCapacity) + 1;
      for (;;) {
        size_t c = ctx.capacity->load();
        n = std::min(n, c);
        if (n == 0) {
          break;
        }
        if (ctx.capacity->compare_exchange_weak(c, c - n)) {
          break;
        }
      }
      size_t res = 0;
      if (n != 0) {
        const int cpu = ctx.slab->CacheCpuSlab().first;
        if (cpu >= 0) {
          // Grow mutates the header array and must be operating on
          // an initialized core.
          InitCpuOnce(ctx, cpu);

          res = ctx.slab->Grow(cpu, size_class, n, [&](uint8_t shift) {
            return ctx.GetMaxCapacityFunctor()(size_class);
          });
          EXPECT_LE(res, n);
        }
        ctx.capacity->fetch_add(n - res);
      }
    } else if (what < 60) {
      int cpu = absl::Uniform<int32_t>(rnd, 0, num_cpus);
      absl::MutexLock lock(&ctx.mutexes[cpu]);
      size_t len = ctx.slab->Length(cpu, size_class);
      EXPECT_LE(len, kMaxStressCapacity);
      size_t cap = ctx.slab->Capacity(cpu, size_class);
      EXPECT_LE(cap, kMaxStressCapacity);
      EXPECT_LE(len, cap);
    } else if (what < 70) {
      int cpu = absl::Uniform<int32_t>(rnd, 0, num_cpus);

      // ShrinkOtherCache mutates the header array and must be operating on an
      // initialized core.
      InitCpuOnce(ctx, cpu);

      absl::MutexLock lock(&ctx.mutexes[cpu]);
      size_t to_shrink = absl::Uniform<int32_t>(rnd, 0, kStressCapacity) + 1;
      ctx.slab->StopCpu(cpu);
      size_t total_shrunk = ctx.slab->ShrinkOtherCache(
          cpu, size_class, to_shrink,
          [&block](size_t size_class, void** batch, size_t n) {
            EXPECT_LT(size_class, kStressSlabs);
            EXPECT_LE(n, kStressCapacity);
            EXPECT_GT(n, 0);
            for (size_t i = 0; i < n; ++i) {
              EXPECT_NE(batch[i], nullptr);
              block.push_back(batch[i]);
            }
          });
      ctx.slab->StartCpu(cpu);
      EXPECT_LE(total_shrunk, to_shrink);
      EXPECT_LE(0, total_shrunk);
      ctx.capacity->fetch_add(total_shrunk);
    } else if (what < 80) {
      size_t to_grow = absl::Uniform<int32_t>(rnd, 0, kStressCapacity) + 1;
      for (;;) {
        size_t c = ctx.capacity->load();
        to_grow = std::min(to_grow, c);
        if (to_grow == 0) {
          break;
        }
        if (ctx.capacity->compare_exchange_weak(c, c - to_grow)) {
          break;
        }
      }
      if (to_grow != 0) {
        int cpu = absl::Uniform<int32_t>(rnd, 0, num_cpus);

        // GrowOtherCache mutates the header array and must be operating on an
        // initialized core.
        InitCpuOnce(ctx, cpu);

        absl::MutexLock lock(&ctx.mutexes[cpu]);
        ctx.slab->StopCpu(cpu);
        size_t grown = ctx.slab->GrowOtherCache(
            cpu, size_class, to_grow,
            [&](uint8_t) { return ctx.GetMaxCapacityFunctor()(size_class); });
        ctx.slab->StartCpu(cpu);
        EXPECT_LE(grown, to_grow);
        EXPECT_GE(grown, 0);
        ctx.capacity->fetch_add(to_grow - grown);
      }
    } else {
      int cpu = absl::Uniform<int32_t>(rnd, 0, num_cpus);
      // Flip coin on whether to unregister rseq on this thread.
      const bool unregister = absl::Bernoulli(rnd, 0.5);

      // Drain mutates the header array and must be operating on an initialized
      // core.
      InitCpuOnce(ctx, cpu);

      {
        absl::MutexLock lock(&ctx.mutexes[cpu]);
        std::optional<ScopedUnregisterRseq> scoped_rseq;
        if (unregister) {
          scoped_rseq.emplace();
          TC_ASSERT(!IsFastNoInit());
        }

        ctx.slab->Drain(
            cpu, [&block, &ctx, cpu](int cpu_arg, size_t size_class,
                                     void** batch, size_t size, size_t cap) {
              EXPECT_EQ(cpu, cpu_arg);
              EXPECT_LT(size_class, kStressSlabs);
              EXPECT_LE(size, kMaxStressCapacity);
              EXPECT_LE(cap, kMaxStressCapacity);
              for (size_t i = 0; i < size; ++i) {
                EXPECT_NE(batch[i], nullptr);
                block.push_back(batch[i]);
              }
              ctx.capacity->fetch_add(cap);
            });
      }

      // Verify we re-registered with rseq as required.
      TC_ASSERT(IsFastNoInit());
    }
  }
}

void ResizeMaxCapacitiesThread(
    Context& ctx, TcmallocSlab::DrainHandler drain_handler,
    absl::Span<std::pair<void*, size_t>> old_slabs_span)
    ABSL_NO_THREAD_SAFETY_ANALYSIS {
  absl::BitGen rnd;
  const size_t num_cpus = NumCPUs();

  while (!*ctx.stop) {
    for (size_t cpu = 0; cpu < num_cpus; ++cpu) ctx.mutexes[cpu].Lock();
    PerSizeClassMaxCapacity new_max_capacity[2];
    int to_resize = GetResizedMaxCapacities(ctx, new_max_capacity);
    size_t old_slabs_idx = 0;

    uint8_t shift = ctx.slab->GetShift();
    void* slabs = AllocSlabs(allocator, shift);
    const auto [old_slabs, old_slabs_size] = ctx.slab->UpdateMaxCapacities(
        slabs, ctx.GetMaxCapacityFunctor(),
        [&](int size, uint16_t cap) {
          ctx.max_capacity[size].store(cap, std::memory_order_relaxed);
        },
        [&](size_t cpu) {
          return ctx.has_init[cpu].load(std::memory_order_relaxed);
        },
        drain_handler, new_max_capacity, to_resize);
    for (size_t cpu = 0; cpu < num_cpus; ++cpu) ctx.mutexes[cpu].Unlock();
    ASSERT_NE(old_slabs, nullptr);
    // We sometimes don't madvise away the old slabs in order to simulate
    // madvise failing.
    const bool simulate_madvise_failure = absl::Bernoulli(rnd, 0.1);
    if (!simulate_madvise_failure) {
      // Verify that we do not write to an old slab, as this may indicate a bug.
      mprotect(old_slabs, old_slabs_size, PROT_READ);
      // It's important that we do this here in order to uncover any potential
      // correctness issues due to madvising away the old slabs.
      // TODO(b/214241843): we should be able to just do one MADV_DONTNEED once
      // the kernel enables huge zero pages.
      madvise(old_slabs, old_slabs_size, MADV_NOHUGEPAGE);
      madvise(old_slabs, old_slabs_size, MADV_DONTNEED);

      // Verify that old_slabs is now non-resident.
      const int fd = signal_safe_open("/proc/self/pageflags", O_RDONLY);
      if (fd < 0) continue;

      // /proc/self/pageflags is an array. Each entry is a bitvector of size 64.
      // To index the array, divide the virtual address by the pagesize. The
      // 64b word has bit fields set.
      const uintptr_t start_addr = reinterpret_cast<uintptr_t>(old_slabs);
      constexpr size_t kPhysicalPageSize = EXEC_PAGESIZE;
      for (uintptr_t addr = start_addr; addr < start_addr + old_slabs_size;
           addr += kPhysicalPageSize) {
        ASSERT_EQ(addr % kPhysicalPageSize, 0);
        // Offset in /proc/self/pageflags.
        const off64_t offset = addr / kPhysicalPageSize * 8;
        uint64_t entry = 0;
        // Ignore false-positive warning in GCC.
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wattribute-warning"
#endif
        const int64_t bytes_read = pread(fd, &entry, sizeof(entry), offset);
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif
        ASSERT_EQ(bytes_read, sizeof(entry));
        constexpr uint64_t kExpectedBits =
            (uint64_t{1} << KPF_ZERO_PAGE) | (uint64_t{1} << KPF_NOPAGE);
        ASSERT_NE(entry & kExpectedBits, 0)
            << entry << " " << addr << " " << start_addr;
      }
      signal_safe_close(fd);
    }

    // Delete the old slab from 100 iterations ago.
    if (old_slabs_span[old_slabs_idx].first != nullptr) {
      auto [old_slabs, old_slabs_size] = old_slabs_span[old_slabs_idx];

      mprotect(old_slabs, old_slabs_size, PROT_READ | PROT_WRITE);
      sized_aligned_delete(old_slabs, old_slabs_size,
                           std::align_val_t{EXEC_PAGESIZE});
    }
    old_slabs_span[old_slabs_idx] = {old_slabs, old_slabs_size};
    if (++old_slabs_idx == old_slabs_span.size()) old_slabs_idx = 0;
  }
}

constexpr size_t kResizeInitialShift = 14;
constexpr size_t kResizeMaxShift = 18;

void ResizeSlabsThread(Context& ctx, TcmallocSlab::DrainHandler drain_handler,
                       absl::Span<std::pair<void*, size_t>> old_slabs_span)
    ABSL_NO_THREAD_SAFETY_ANALYSIS {
  absl::BitGen rnd;
  const size_t num_cpus = NumCPUs();
  size_t shift = kResizeInitialShift;
  size_t old_slabs_idx = 0;
  for (int i = 0; i < 10; ++i) {
    if (shift == kResizeInitialShift) {
      ++shift;
    } else if (shift == kResizeMaxShift) {
      --shift;
    } else {
      const bool grow = absl::Bernoulli(rnd, 0.5);
      if (grow) {
        ++shift;
      } else {
        --shift;
      }
    }
    for (size_t cpu = 0; cpu < num_cpus; ++cpu) ctx.mutexes[cpu].Lock();
    void* slabs = AllocSlabs(allocator, shift);
    const auto [old_slabs, old_slabs_size] = ctx.slab->ResizeSlabs(
        ToShiftType(shift), slabs, ctx.GetMaxCapacityFunctor(),
        [&](size_t cpu) {
          return ctx.has_init[cpu].load(std::memory_order_relaxed);
        },
        drain_handler);
    for (size_t cpu = 0; cpu < num_cpus; ++cpu) ctx.mutexes[cpu].Unlock();
    ASSERT_NE(old_slabs, nullptr);
    // We sometimes don't madvise away the old slabs in order to simulate
    // madvise failing.
    const bool simulate_madvise_failure = absl::Bernoulli(rnd, 0.1);
    if (!simulate_madvise_failure) {
      // Verify that we do not write to an old slab, as this may indicate a bug.
      mprotect(old_slabs, old_slabs_size, PROT_READ);
      // It's important that we do this here in order to uncover any potential
      // correctness issues due to madvising away the old slabs.
      // TODO(b/214241843): we should be able to just do one MADV_DONTNEED once
      // the kernel enables huge zero pages.
      madvise(old_slabs, old_slabs_size, MADV_NOHUGEPAGE);
      madvise(old_slabs, old_slabs_size, MADV_DONTNEED);

      // Verify that old_slabs is now non-resident.
      const int fd = signal_safe_open("/proc/self/pageflags", O_RDONLY);
      if (fd < 0) continue;

      // /proc/self/pageflags is an array. Each entry is a bitvector of size 64.
      // To index the array, divide the virtual address by the pagesize. The
      // 64b word has bit fields set.
      const uintptr_t start_addr = reinterpret_cast<uintptr_t>(old_slabs);
      constexpr size_t kPhysicalPageSize = EXEC_PAGESIZE;
      for (uintptr_t addr = start_addr; addr < start_addr + old_slabs_size;
           addr += kPhysicalPageSize) {
        ASSERT_EQ(addr % kPhysicalPageSize, 0);
        // Offset in /proc/self/pageflags.
        const off64_t offset = addr / kPhysicalPageSize * 8;
        uint64_t entry = 0;
// Ignore false-positive warning in GCC.
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wattribute-warning"
#endif
        const int64_t bytes_read = pread(fd, &entry, sizeof(entry), offset);
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif
        ASSERT_EQ(bytes_read, sizeof(entry));
        constexpr uint64_t kExpectedBits =
            (uint64_t{1} << KPF_ZERO_PAGE) | (uint64_t{1} << KPF_NOPAGE);
        ASSERT_NE(entry & kExpectedBits, 0)
            << entry << " " << addr << " " << start_addr;
      }
      signal_safe_close(fd);
    }

    // Delete the old slab from 100 iterations ago.
    if (old_slabs_span[old_slabs_idx].first != nullptr) {
      auto [old_slabs, old_slabs_size] = old_slabs_span[old_slabs_idx];

      mprotect(old_slabs, old_slabs_size, PROT_READ | PROT_WRITE);
      sized_aligned_delete(old_slabs, old_slabs_size,
                           std::align_val_t{EXEC_PAGESIZE});
    }
    old_slabs_span[old_slabs_idx] = {old_slabs, old_slabs_size};
    if (++old_slabs_idx == old_slabs_span.size()) old_slabs_idx = 0;
  }
}

class StressThreadTest : public testing::TestWithParam<std::tuple<bool, bool>> {
};

TEST_P(StressThreadTest, Stress) {
  // The test creates 2 * NumCPUs() threads each executing all possible
  // operations on TcmallocSlab. Depending on the test param, we may grow the
  // slabs a few times while stress threads are running. After that we verify
  // that no objects lost/duplicated and that total capacity is preserved.

  if (!IsFast()) {
    GTEST_SKIP() << "Need fast percpu. Skipping.";
    return;
  }

  const bool resize = std::get<0>(GetParam());
  const bool pin_cpu = std::get<1>(GetParam());

  TcmallocSlab slab;
  size_t shift = resize ? kResizeInitialShift : kShift;
  std::vector<std::thread> threads;
  const size_t num_cpus = NumCPUs();
  const size_t n_stress_threads = 2 * num_cpus;
  const size_t n_threads = n_stress_threads + resize;
  std::atomic<size_t> max_capacity[kStressSlabs];

  for (size_t size_class = 0; size_class < kStressSlabs; ++size_class) {
    max_capacity[size_class].store(kStressCapacity, std::memory_order_relaxed);
  }

  // once_flag's protect InitCpu on a CPU.
  std::vector<absl::once_flag> init(num_cpus);
  // Tracks whether init has occurred on a CPU for use in ResizeSlabs.
  std::vector<std::atomic<bool>> has_init(num_cpus);

  // Mutexes protect Drain operation on a CPU.
  std::vector<absl::Mutex> mutexes(num_cpus);
  // Give each thread an initial set of local objects.
  std::vector<std::vector<void*>> blocks(n_stress_threads);
  for (size_t i = 0; i < blocks.size(); ++i) {
    for (size_t j = 0; j < kStressCapacity; ++j) {
      blocks[i].push_back(reinterpret_cast<void*>(
          (i * kStressCapacity + j + 1) * sizeof(void*)));
    }
  }
  std::atomic<bool> stop(false);
  // Total capacity shared between all size classes and all CPUs.
  const size_t kTotalCapacity = blocks.size() * kStressCapacity * 3 / 4;
  std::atomic<size_t> capacity(kTotalCapacity);
  Context ctx = {&slab,
                 &blocks,
                 absl::MakeSpan(mutexes),
                 &capacity,
                 &stop,
                 absl::MakeSpan(init),
                 absl::MakeSpan(has_init),
                 &max_capacity[0]};
  InitSlab(slab, allocator, ctx.GetMaxCapacityFunctor(), shift);
  // Create threads and let them work for 5 seconds while we may or not also be
  // resizing the slab.
  threads.reserve(n_threads);
  for (size_t t = 0; t < n_stress_threads; ++t) {
    threads.push_back(std::thread(StressThread, t, std::ref(ctx)));
  }
  // Collect objects and capacity from all slabs in Drain in ResizeSlabs.
  absl::flat_hash_set<void*> objects;
  const auto drain_handler = [&objects, &ctx](int cpu, size_t size_class,
                                              void** batch, size_t size,
                                              size_t cap) {
    for (size_t i = 0; i < size; ++i) {
      objects.insert(batch[i]);
    }
    ctx.capacity->fetch_add(cap);
  };

  std::array<std::pair<void*, size_t>, 100> max_cap_slabs_array{};
  threads.push_back(std::thread(ResizeMaxCapacitiesThread, std::ref(ctx),
                                std::ref(drain_handler),
                                absl::MakeSpan(max_cap_slabs_array)));

  // Keep track of old slabs so we can free the memory. We technically could
  // have a sleeping StressThread access any of the old slabs, but it's very
  // inefficient to keep all the old slabs around so we just keep 100.
  std::array<std::pair<void*, size_t>, 100> old_slabs_arr{};
  if (resize) {
    threads.push_back(std::thread(ResizeSlabsThread, std::ref(ctx),
                                  std::ref(drain_handler),
                                  absl::MakeSpan(old_slabs_arr)));
  }
  if (pin_cpu) {
    // Regression test for a livelock when a thread keeps running on cpu 0.
    absl::SleepFor(absl::Seconds(1));
    CpuSet cpus;
    cpus.Zero();
    cpus.Set(0);
    (void)cpus.SetAffinity(0);
    absl::SleepFor(absl::Seconds(1));
  } else {
    absl::SleepFor(absl::Seconds(5));
  }
  stop = true;
  for (auto& t : threads) {
    t.join();
  }
  for (int cpu = 0; cpu < num_cpus; ++cpu) {
    slab.Drain(cpu, drain_handler);
    for (size_t size_class = 1; size_class < kStressSlabs; ++size_class) {
      EXPECT_EQ(slab.Length(cpu, size_class), 0);
      EXPECT_EQ(slab.Capacity(cpu, size_class), 0);
    }
  }
  for (const auto& b : blocks) {
    for (auto o : b) {
      objects.insert(o);
    }
  }
  EXPECT_EQ(objects.size(), blocks.size() * kStressCapacity);
  EXPECT_EQ(capacity.load(), kTotalCapacity);
  void* deleted_slabs = slab.Destroy(sized_aligned_delete);

  for (const auto& [old_slabs, old_slabs_size] : max_cap_slabs_array) {
    if (old_slabs == nullptr || old_slabs == deleted_slabs) continue;

    mprotect(old_slabs, old_slabs_size, PROT_READ | PROT_WRITE);
    sized_aligned_delete(old_slabs, old_slabs_size,
                         std::align_val_t{EXEC_PAGESIZE});
  }

  for (const auto& [old_slabs, old_slabs_size] : old_slabs_arr) {
    if (old_slabs == nullptr || old_slabs == deleted_slabs) continue;

    mprotect(old_slabs, old_slabs_size, PROT_READ | PROT_WRITE);
    sized_aligned_delete(old_slabs, old_slabs_size,
                         std::align_val_t{EXEC_PAGESIZE});
  }
}

INSTANTIATE_TEST_SUITE_P(
    Group, StressThreadTest, testing::Combine(testing::Bool(), testing::Bool()),
    [](const testing::TestParamInfo<StressThreadTest::ParamType> info) {
      return std::string(std::get<0>(info.param) ? "" : "No") + "Resize_" +
             (std::get<1>(info.param) ? "" : "No") + "Pin";
    });

TEST(TcmallocSlab, SMP) {
  // For the other tests here to be meaningful, we need multiple cores.
  ASSERT_GT(NumCPUs(), 1);
}

#if ABSL_INTERNAL_HAVE_ELF_SYMBOLIZE
int FilterElfHeader(struct dl_phdr_info* info, size_t size, void* data) {
  *reinterpret_cast<uintptr_t*>(data) =
      reinterpret_cast<uintptr_t>(info->dlpi_addr);
  // No further iteration wanted.
  return 1;
}
#endif

TEST(TcmallocSlab, CriticalSectionMetadata) {
// We cannot inhibit --gc-sections, except on GCC or Clang 9-or-newer.
#if defined(__clang_major__) && __clang_major__ < 9
  GTEST_SKIP() << "--gc-sections cannot be inhibited on this compiler.";
#endif

#if !TCMALLOC_INTERNAL_PERCPU_USE_RSEQ
  GTEST_SKIP() << "rseq is not enabled in this build.";
#endif

  // We expect that restartable sequence critical sections (rseq_cs) are in the
  // __rseq_cs section (by convention, not hard requirement).  Additionally, for
  // each entry in that section, there should be a pointer to it in
  // __rseq_cs_ptr_array.
#if ABSL_INTERNAL_HAVE_ELF_SYMBOLIZE
  uintptr_t relocation = 0;
  dl_iterate_phdr(FilterElfHeader, &relocation);

  int fd = tcmalloc_internal::signal_safe_open("/proc/self/exe", O_RDONLY);
  ASSERT_NE(fd, -1);

  const kernel_rseq_cs* cs_start = nullptr;
  const kernel_rseq_cs* cs_end = nullptr;

  const kernel_rseq_cs** cs_array_start = nullptr;
  const kernel_rseq_cs** cs_array_end = nullptr;

  absl::debugging_internal::ForEachSection(
      fd, [&](const absl::string_view name, const ElfW(Shdr) & hdr) {
        uintptr_t start = relocation + reinterpret_cast<uintptr_t>(hdr.sh_addr);
        uintptr_t end =
            relocation + reinterpret_cast<uintptr_t>(hdr.sh_addr + hdr.sh_size);

        if (name == "__rseq_cs") {
          EXPECT_EQ(cs_start, nullptr);
          EXPECT_EQ(start % alignof(kernel_rseq_cs), 0);
          EXPECT_EQ(end % alignof(kernel_rseq_cs), 0);
          EXPECT_LT(start, end) << "__rseq_cs must not be empty";

          cs_start = reinterpret_cast<const kernel_rseq_cs*>(start);
          cs_end = reinterpret_cast<const kernel_rseq_cs*>(end);
        } else if (name == "__rseq_cs_ptr_array") {
          EXPECT_EQ(cs_array_start, nullptr);
          EXPECT_EQ(start % alignof(kernel_rseq_cs*), 0);
          EXPECT_EQ(end % alignof(kernel_rseq_cs*), 0);
          EXPECT_LT(start, end) << "__rseq_cs_ptr_array must not be empty";

          cs_array_start = reinterpret_cast<const kernel_rseq_cs**>(start);
          cs_array_end = reinterpret_cast<const kernel_rseq_cs**>(end);
        }

        return true;
      });

  close(fd);

  // The length of the array in multiples of rseq_cs should be the same as the
  // length of the array of pointers.
  ASSERT_EQ(cs_end - cs_start, cs_array_end - cs_array_start);

  // The array should not be empty.
  ASSERT_NE(cs_start, nullptr);

  absl::flat_hash_set<const kernel_rseq_cs*> cs_pointers;
  for (auto* ptr = cs_start; ptr != cs_end; ++ptr) {
    cs_pointers.insert(ptr);
  }

  absl::flat_hash_set<const kernel_rseq_cs*> cs_array_pointers;
  for (auto** ptr = cs_array_start; ptr != cs_array_end; ++ptr) {
    // __rseq_cs_ptr_array should have no duplicates.
    EXPECT_TRUE(cs_array_pointers.insert(*ptr).second);
  }

  EXPECT_THAT(cs_pointers, ::testing::ContainerEq(cs_array_pointers));
#endif
}

void BM_PushPop(benchmark::State& state) {
  TC_CHECK(IsFast());
  constexpr int kCpu = 0;
  constexpr size_t kSizeClass = 0;
  // Fake being on the given CPU. This allows Grow to succeed for
  // kCpu/kSizeClass, and then we Push/Pop repeatedly on kCpu/kSizeClass.
  // Note that no other thread has access to `slab` so we don't need to worry
  // about races.
  ScopedFakeCpuId fake_cpu_id(kCpu);
  constexpr int kBatchSize = 32;
  TcmallocSlab slab;

#pragma GCC diagnostic ignored "-Wnonnull"
  const auto get_capacity = [](size_t size_class) -> size_t {
    return kBatchSize;
  };
  InitSlab(slab, allocator, get_capacity, kShift);
  for (int cpu = 0, n = NumCPUs(); cpu < n; ++cpu) {
    slab.InitCpu(cpu, get_capacity);
  }
  auto [cpu, _] = slab.CacheCpuSlab();
  TC_CHECK_EQ(cpu, kCpu);

  TC_CHECK_EQ(slab.Grow(kCpu, kSizeClass, kBatchSize,
                        [](uint8_t shift) { return kBatchSize; }),
              kBatchSize);
  void* batch[kBatchSize];
  for (int i = 0; i < kBatchSize; i++) {
    batch[i] = &batch[i];
  }
  for (auto _ : state) {
    for (size_t x = 0; x < kBatchSize; x++) {
      TC_CHECK(slab.Push(kSizeClass, batch[x]));
    }
    for (size_t x = 0; x < kBatchSize; x++) {
      TC_CHECK(slab.Pop(kSizeClass) == batch[kBatchSize - x - 1]);
    }
  }
}
BENCHMARK(BM_PushPop);

void BM_PushPopBatch(benchmark::State& state) {
  TC_CHECK(IsFast());
  constexpr int kCpu = 0;
  constexpr size_t kSizeClass = 0;
  // Fake being on the given CPU. This allows Grow to succeed for
  // kCpu/kSizeClass, and then we Push/PopBatch repeatedly on kCpu/kSizeClass.
  // Note that no other thread has access to `slab` so we don't need to worry
  // about races.
  ScopedFakeCpuId fake_cpu_id(kCpu);
  constexpr int kBatchSize = 32;
  TcmallocSlab slab;
  const auto get_capacity = [](size_t size_class) -> size_t {
    return kBatchSize;
  };
  InitSlab(slab, allocator, get_capacity, kShift);
  for (int cpu = 0, n = NumCPUs(); cpu < n; ++cpu) {
    slab.InitCpu(cpu, get_capacity);
  }
  auto [cpu, _] = slab.CacheCpuSlab();
  TC_CHECK_EQ(cpu, kCpu);
  TC_CHECK_EQ(slab.Grow(kCpu, kSizeClass, kBatchSize,
                        [](uint8_t shift) { return kBatchSize; }),
              kBatchSize);
  void* batch[kBatchSize];
  for (int i = 0; i < kBatchSize; i++) {
    batch[i] = &batch[i];
  }
  for (auto _ : state) {
    TC_CHECK_EQ(slab.PushBatch(kSizeClass, batch, kBatchSize), kBatchSize);
    TC_CHECK_EQ(slab.PopBatch(kSizeClass, batch, kBatchSize), kBatchSize);
  }
}
BENCHMARK(BM_PushPopBatch);

}  // namespace
}  // namespace percpu
}  // namespace subtle
}  // namespace tcmalloc_internal
}  // namespace tcmalloc
