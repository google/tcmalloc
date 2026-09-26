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
//
// TCMalloc's `TcmallocSlab` is subtle and requires care for memory ordering and
// considering thread preemptions in a multithreaded environment.  Nevertheless,
// we can test many of our invariants in a single-threaded context, using
// FuzzTest to drive a series of "instructions" that specify different
// operations.  No sequence of instructions reachable with a fuzzer should be
// able to violate our invariants.

#include <sys/mman.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <new>
#include <optional>
#include <type_traits>
#include <variant>
#include <vector>

#include "gtest/gtest.h"
#include "fuzztest/fuzztest.h"
#include "absl/container/flat_hash_set.h"
#include "absl/functional/function_ref.h"
#include "absl/log/check.h"
#include "absl/strings/str_format.h"
#include "tcmalloc/internal/affinity.h"
#include "tcmalloc/internal/config.h"
#include "tcmalloc/internal/logging.h"
#include "tcmalloc/internal/percpu.h"
#include "tcmalloc/internal/percpu_tcmalloc.h"
#include "tcmalloc/internal/sysinfo.h"
#include "tcmalloc/malloc_extension.h"
#include "tcmalloc/testing/testutil.h"

GOOGLE_MALLOC_SECTION_BEGIN
namespace tcmalloc::tcmalloc_internal::subtle::percpu {
namespace {

// Arbitrary parameters for the slab.  kNumClasses has to be a compile-time
// constant, but the others could be driven at runtime by a fuzzer.
constexpr size_t kNumClasses = 5;
constexpr size_t kMaxCapacity = 16;
constexpr size_t kMaxBatchSize = 16;
// Slot offsets in a slab header are uint16_t, so InitCpuImpl rejects per-CPU
// regions larger than (1 << 16) * sizeof(void*) bytes: Shift{19} is the
// largest legal shift.  Shift{12} is the small-but-slow production minimum.
// Spanning the whole range makes ResizeSlabs cross hugepage boundaries in
// both directions on hosts with few CPUs.
constexpr uint8_t kMinShift = 12;
constexpr uint8_t kMaxShift = 19;
static_assert((size_t{1} << kMaxShift) < kHugePageSize,
              "ReleaseSlabMetadataForDrainedCpus requires a slab smaller than "
              "a hugepage");

using SlabsType = TcmallocSlab<kNumClasses>;

void* Malloc(size_t size, std::align_val_t alignment) {
  void* ptr = ::operator new(size, alignment);
  memset(ptr, 0, size);
  return ptr;
}

// The slabs region is carved out of a hugepage-aligned block one hugepage
// larger than GetSlabsAllocSize, so the fuzzer chooses the offset of the slabs
// within a hugepage and hence which hugepages are entirely covered by slabs
// (and eligible for ReleaseSlabMetadataForDrainedCpus).  The block is freed
// with the alignment it was allocated with, not SlabAlignment(shift).
struct SlabsBlock {
  void* base;
  size_t base_size;
  void* slabs;
};

struct State {
  const size_t num_cpus;
  int current_cpu = 0;
  ScopedFakeCpuId active_cpu;
  SlabsType slab;
  SlabsBlock slabs_block;
  std::vector<bool> cpu_initialized;
  std::vector<bool> cpu_stopped;
  std::array<size_t, kNumClasses> max_capacity;
  // Expected value of slab.Capacity(cpu, size_class), accumulated from the
  // increments and decrements the slab reports.
  std::vector<std::array<uint16_t, kNumClasses>> expected_capacity;
  // MetadataMemoryUsage().virtual_size less the slabs allocation, which is
  // fixed for the lifetime of the slab.
  size_t fixed_metadata_bytes;

  absl::flat_hash_set<void*> allocated_objects[kNumClasses];
  std::vector<void*> available_objects[kNumClasses];

  State(uint8_t initial_shift, uint8_t slabs_offset);

  ~State();

  size_t MaxCapacity(size_t size_class) const {
    if (size_class == 0 || size_class >= kNumClasses) return 0;
    return max_capacity[size_class];
  }

  SlabsBlock AllocateSlabs(Shift shift, uint8_t slabs_offset) const;
  static void FreeSlabs(const SlabsBlock& block);
  // Replaces slabs_block with the block that now backs the slab and frees the
  // old one, which the slab must have returned as `old_slabs`.
  void SwapSlabs(const SlabsBlock& new_block, void* old_slabs,
                 size_t old_slabs_size);

  void EnsureCpuInitialized(int cpu);
  bool IsDrained(int cpu) const;
  void CheckInvariants();
  void CheckMetadataUsage();
  void CheckValidObject(void* obj, size_t sc) const;
  // Shared by every DrainHandler: the reported capacity must match the model
  // and the objects must be ours.
  void HandleDrain(int cpu, size_t size_class, void** batch, size_t size,
                   size_t cap);

  // Allocates an object from the underlying malloc implementation that we can
  // freelist.
  void* AllocateObject(size_t size_class);
  void FreeObject(void* obj, size_t size_class);
};

// We select different sizes for different size classes, so that ASan can notice
// if we confuse objects across size classes.
static size_t FakeSizeForSizeClass(size_t size_class) {
  return 16 + size_class;
}

State::State(uint8_t initial_shift, uint8_t slabs_offset)
    : num_cpus(NumCPUs()),
      active_cpu(current_cpu),
      cpu_initialized(num_cpus, false),
      cpu_stopped(num_cpus, false),
      expected_capacity(num_cpus) {
  for (size_t sc = 0; sc < kNumClasses; ++sc) {
    max_capacity[sc] = (sc == 0) ? 0 : kMaxCapacity;
  }
  for (auto& caps : expected_capacity) {
    caps.fill(0);
  }
  const Shift shift = ToShiftType(initial_shift);
  slabs_block = AllocateSlabs(shift, slabs_offset);
  slab.Init(
      Malloc, slabs_block.slabs, [this](size_t sc) { return MaxCapacity(sc); },
      shift);
  const PerCPUMetadataState usage = slab.MetadataMemoryUsage();
  const size_t slabs_size = GetSlabsAllocSize(shift, num_cpus);
  TC_CHECK_GE(usage.virtual_size,
              slabs_size + num_cpus * SlabsType::GetCpuStateSize());
  fixed_metadata_bytes = usage.virtual_size - slabs_size;
  CheckMetadataUsage();
  EnsureCpuInitialized(current_cpu);
}

SlabsBlock State::AllocateSlabs(Shift shift, uint8_t slabs_offset) const {
  const size_t slab_bytes = size_t{1} << ToUint8(shift);
  const size_t offset =
      (slabs_offset % (kHugePageSize / slab_bytes)) * slab_bytes;
  SlabsBlock block;
  block.base_size = GetSlabsAllocSize(shift, num_cpus) + kHugePageSize;
  block.base = Malloc(block.base_size, std::align_val_t{kHugePageSize});
  block.slabs = static_cast<char*>(block.base) + offset;
  TC_CHECK_EQ(reinterpret_cast<uintptr_t>(block.slabs) %
                  static_cast<size_t>(SlabAlignment(shift)),
              0);
  return block;
}

void State::FreeSlabs(const SlabsBlock& block) {
  sized_aligned_delete(block.base, block.base_size,
                       std::align_val_t{kHugePageSize});
}

void State::SwapSlabs(const SlabsBlock& new_block, void* old_slabs,
                      size_t old_slabs_size) {
  TC_CHECK(old_slabs == slabs_block.slabs);
  TC_CHECK_EQ(old_slabs_size, slabs_block.base_size - kHugePageSize);
  FreeSlabs(slabs_block);
  slabs_block = new_block;
  // Both ResizeSlabs and UpdateMaxCapacities start every CPU at capacity 0 in
  // the new slabs.
  for (auto& caps : expected_capacity) {
    caps.fill(0);
  }
  CheckMetadataUsage();
}

void* State::AllocateObject(size_t size_class) {
  void* obj = ::operator new(FakeSizeForSizeClass(size_class));
  allocated_objects[size_class].insert(obj);
  return obj;
}

void State::FreeObject(void* obj, size_t size_class) {
  auto it = allocated_objects[size_class].find(obj);
  TC_CHECK(it != allocated_objects[size_class].end());
  allocated_objects[size_class].erase(it);

  ::operator delete(obj, FakeSizeForSizeClass(size_class));
}

void State::EnsureCpuInitialized(int cpu) {
  if (cpu >= 0 && cpu < num_cpus && !cpu_initialized[cpu] &&
      !cpu_stopped[cpu]) {
    slab.InitCpu(cpu, [this](size_t sc) { return MaxCapacity(sc); });
    cpu_initialized[cpu] = true;
  }
}

bool State::IsDrained(int cpu) const {
  return absl::c_all_of(expected_capacity[cpu],
                        [](uint16_t cap) { return cap == 0; });
}

void State::CheckInvariants() {
  for (size_t sc = 1; sc < kNumClasses; ++sc) {
    size_t total_in_slabs = 0;
    const size_t max_cap = MaxCapacity(sc);
    for (int cpu = 0; cpu < num_cpus; ++cpu) {
      const size_t len = slab.Length(cpu, sc);
      const size_t cap = slab.Capacity(cpu, sc);
      TC_CHECK_LE(len, cap);
      TC_CHECK_LE(cap, max_cap);
      TC_CHECK_EQ(cap, expected_capacity[cpu][sc], "cpu=%d size_class=%v", cpu,
                  sc);
      total_in_slabs += len;
    }
    TC_CHECK_EQ(available_objects[sc].size() + total_in_slabs,
                allocated_objects[sc].size());
  }
}

void State::CheckMetadataUsage() {
  const PerCPUMetadataState usage = slab.MetadataMemoryUsage();
  const size_t slabs_size =
      GetSlabsAllocSize(ToShiftType(slab.GetShift()), num_cpus);
  TC_CHECK_EQ(usage.virtual_size, fixed_metadata_bytes + slabs_size);
  TC_CHECK_LE(usage.resident_size, slabs_size);
}

void State::CheckValidObject(void* obj, size_t sc) const {
  TC_CHECK_NE(obj, nullptr);
  TC_CHECK_EQ(reinterpret_cast<uintptr_t>(obj) & 1, 0);
  TC_CHECK(allocated_objects[sc].contains(obj));
}

void State::HandleDrain(int cpu, size_t size_class, void** batch, size_t size,
                        size_t cap) {
  TC_CHECK_LT(size_class, kNumClasses);
  TC_CHECK_LE(size, cap);
  TC_CHECK_EQ(cap, expected_capacity[cpu][size_class]);
  for (size_t i = 0; i < size; ++i) {
    CheckValidObject(batch[i], size_class);
    available_objects[size_class].push_back(batch[i]);
  }
}

State::~State() {
  // Teardown: restart stopped CPUs and drain all CPUs to recover all objects.
  for (size_t cpu = 0; cpu < num_cpus; ++cpu) {
    if (cpu_stopped[cpu]) {
      slab.StartCpu(cpu);
      cpu_stopped[cpu] = false;
    }
    slab.Drain(cpu, [&](int drained_cpu, size_t size_class, void** batch,
                        size_t size, size_t cap) {
      TC_CHECK_EQ(drained_cpu, cpu);
      HandleDrain(drained_cpu, size_class, batch, size, cap);
    });
  }

  for (int cpu = 0; cpu < num_cpus; ++cpu) {
    for (size_t sc = 1; sc < kNumClasses; ++sc) {
      TC_CHECK_EQ(slab.Length(cpu, sc), 0);
      TC_CHECK_EQ(slab.Capacity(cpu, sc), 0);
    }
  }

  void* freed_slabs =
      slab.Destroy([this](void* ptr, size_t size, std::align_val_t alignment) {
        if (ptr == slabs_block.slabs) {
          TC_CHECK_EQ(size, slabs_block.base_size - kHugePageSize);
          FreeSlabs(slabs_block);
          return;
        }
        sized_aligned_delete(ptr, size, alignment);
      });
  TC_CHECK(freed_slabs == slabs_block.slabs);

  // Free mock objects.
  for (int sc = 1; sc < kNumClasses; ++sc) {
    TC_CHECK_EQ(available_objects[sc].size(), allocated_objects[sc].size());

    for (void* obj : allocated_objects[sc]) {
      // We're going through the hashtable itself.  Don't use FreeObject to
      // avoid invalidating our own iterators.
      ::operator delete(obj, FakeSizeForSizeClass(sc));
    }
  }
}

// We define a number of instructions that the fuzzer engine can select to drive
// the API of `TcmallocSlab`.  The AbslStringify instances write out valid C++
// code so that the reproducer instructions produced by fuzztest can be
// copy-and-pasted verbatim and run as a regression test.
struct Push {
  unsigned size_class;

  template <typename Sink>
  friend void AbslStringify(Sink& sink, const Push& p) {
    absl::Format(&sink, "Push{.size_class=%v}", p.size_class);
  }

  void Perform(State& state) const {
    const size_t sc = 1 + (size_class % (kNumClasses - 1));
    if (state.available_objects[sc].empty()) {
      state.available_objects[sc].push_back(state.AllocateObject(sc));
    }
    void* item = state.available_objects[sc].back();
    bool pushed = state.slab.Push(sc, item);
    if (!pushed) {
      auto [got_cpu, cached] = state.slab.CacheCpuSlab();
      if (cached && got_cpu >= 0 && !state.cpu_stopped[got_cpu]) {
        state.EnsureCpuInitialized(got_cpu);
        pushed = state.slab.Push(sc, item);
      }
    }
    if (pushed) {
      state.available_objects[sc].pop_back();
    }
  }
};

struct Pop {
  unsigned size_class;

  template <typename Sink>
  friend void AbslStringify(Sink& sink, const Pop& p) {
    absl::Format(&sink, "Pop{.size_class=%v}", p.size_class);
  }

  void Perform(State& state) const {
    const size_t sc = 1 + (size_class % (kNumClasses - 1));
    void* item = state.slab.Pop(sc);
    if (item == nullptr) {
      auto [got_cpu, cached] = state.slab.CacheCpuSlab();
      if (cached && got_cpu >= 0 && !state.cpu_stopped[got_cpu]) {
        state.EnsureCpuInitialized(got_cpu);
        item = state.slab.Pop(sc);
      }
    }
    if (item != nullptr) {
      state.CheckValidObject(item, sc);
      state.available_objects[sc].push_back(item);
    }
  }
};

struct PushBatch {
  unsigned size_class;
  uint8_t count;

  template <typename Sink>
  friend void AbslStringify(Sink& sink, const PushBatch& p) {
    absl::Format(&sink, "PushBatch{.size_class=%v, .count=%v}", p.size_class,
                 p.count);
  }

  void Perform(State& state) const {
    const size_t sc = 1 + (size_class % (kNumClasses - 1));
    const size_t count = 1 + (this->count % kMaxBatchSize);
    void* batch[kMaxBatchSize];
    for (size_t i = 0; i < count; ++i) {
      batch[i] = state.AllocateObject(sc);
    }
    size_t pushed = state.slab.PushBatch(sc, batch, count);
    TC_CHECK_LE(pushed, count);
    if (pushed == 0) {
      auto [got_cpu, cached] = state.slab.CacheCpuSlab();
      if (cached && got_cpu >= 0 && !state.cpu_stopped[got_cpu]) {
        state.EnsureCpuInitialized(got_cpu);
        pushed = state.slab.PushBatch(sc, batch, count);
        TC_CHECK_LE(pushed, count);
      }
    }
    for (size_t i = 0; i < count - pushed; ++i) {
      state.FreeObject(batch[i], sc);
    }
  }
};

struct PopBatch {
  unsigned size_class;
  uint8_t count;

  template <typename Sink>
  friend void AbslStringify(Sink& sink, const PopBatch& p) {
    absl::Format(&sink, "PopBatch{.size_class=%v, .count=%v}", p.size_class,
                 p.count);
  }

  void Perform(State& state) const {
    const size_t sc = 1 + (size_class % (kNumClasses - 1));
    const size_t count = 1 + (this->count % kMaxBatchSize);
    void* batch[kMaxBatchSize];
    size_t popped = state.slab.PopBatch(sc, batch, count);
    TC_CHECK_LE(popped, count);
    if (popped == 0) {
      auto [got_cpu, cached] = state.slab.CacheCpuSlab();
      if (cached && got_cpu >= 0 && !state.cpu_stopped[got_cpu]) {
        state.EnsureCpuInitialized(got_cpu);
        popped = state.slab.PopBatch(sc, batch, count);
        TC_CHECK_LE(popped, count);
      }
    }
    for (size_t i = 0; i < popped; ++i) {
      state.CheckValidObject(batch[i], sc);
      state.available_objects[sc].push_back(batch[i]);
    }
  }
};

struct Grow {
  unsigned size_class;
  uint8_t len;

  template <typename Sink>
  friend void AbslStringify(Sink& sink, const Grow& g) {
    absl::Format(&sink, "Grow{.size_class=%v, .len=%v}", g.size_class, g.len);
  }

  void Perform(State& state) const {
    const size_t sc = 1 + (size_class % (kNumClasses - 1));
    const size_t len = 1 + (this->len % kMaxCapacity);
    if (state.cpu_stopped[state.current_cpu]) {
      return;
    }
    state.EnsureCpuInitialized(state.current_cpu);
    size_t grew = state.slab.Grow(
        state.current_cpu, sc, len,
        [&state, sc](uint8_t) { return state.MaxCapacity(sc); });
    TC_CHECK_LE(grew, len);
    state.expected_capacity[state.current_cpu][sc] += grew;
  }
};

struct GrowOtherClass {
  uint8_t cpu_index;
  unsigned size_class;
  uint8_t len;

  template <typename Sink>
  friend void AbslStringify(Sink& sink, const GrowOtherClass& g) {
    absl::Format(&sink,
                 "GrowOtherClass{.cpu_index=%v, .size_class=%v, .len=%v}",
                 g.cpu_index, g.size_class, g.len);
  }

  void Perform(State& state) const {
    const int target_cpu = cpu_index % state.num_cpus;
    const size_t sc = 1 + (size_class % (kNumClasses - 1));
    const size_t len = 1 + (this->len % kMaxCapacity);
    if (!state.cpu_initialized[target_cpu]) {
      if (state.cpu_stopped[target_cpu]) {
        return;
      }
      state.EnsureCpuInitialized(target_cpu);
    }
    const bool was_stopped = state.cpu_stopped[target_cpu];
    if (!was_stopped) {
      state.slab.StopCpu(target_cpu);
      state.cpu_stopped[target_cpu] = true;
    }
    size_t grew = state.slab.GrowOtherCache(
        target_cpu, sc, len,
        [&state, sc](uint8_t) { return state.MaxCapacity(sc); });
    TC_CHECK_LE(grew, len);
    state.expected_capacity[target_cpu][sc] += grew;
    if (!was_stopped) {
      state.slab.StartCpu(target_cpu);
      state.cpu_stopped[target_cpu] = false;
    }
  }
};

struct ShrinkOtherCache {
  uint8_t cpu_index;
  unsigned size_class;
  uint8_t len;

  template <typename Sink>
  friend void AbslStringify(Sink& sink, const ShrinkOtherCache& s) {
    absl::Format(&sink,
                 "ShrinkOtherCache{.cpu_index=%v, .size_class=%v, .len=%v}",
                 s.cpu_index, s.size_class, s.len);
  }

  void Perform(State& state) const {
    const int target_cpu = cpu_index % state.num_cpus;
    const size_t sc = 1 + (size_class % (kNumClasses - 1));
    const size_t len = 1 + (this->len % kMaxCapacity);
    if (!state.cpu_initialized[target_cpu]) {
      if (state.cpu_stopped[target_cpu]) {
        return;
      }
      state.EnsureCpuInitialized(target_cpu);
    }
    const bool was_stopped = state.cpu_stopped[target_cpu];
    if (!was_stopped) {
      state.slab.StopCpu(target_cpu);
      state.cpu_stopped[target_cpu] = true;
    }
    size_t shrunk = state.slab.ShrinkOtherCache(
        target_cpu, sc, len, [&](size_t size_class, void** batch, size_t size) {
          TC_CHECK_EQ(size_class, sc);
          for (size_t i = 0; i < size; ++i) {
            state.CheckValidObject(batch[i], size_class);
            state.available_objects[size_class].push_back(batch[i]);
          }
        });
    TC_CHECK_LE(shrunk, len);
    TC_CHECK_LE(shrunk, state.expected_capacity[target_cpu][sc]);
    state.expected_capacity[target_cpu][sc] -= shrunk;
    if (!was_stopped) {
      state.slab.StartCpu(target_cpu);
      state.cpu_stopped[target_cpu] = false;
    }
  }
};

struct Drain {
  uint8_t cpu_index;

  template <typename Sink>
  friend void AbslStringify(Sink& sink, const Drain& d) {
    absl::Format(&sink, "Drain{.cpu_index=%v}", d.cpu_index);
  }

  void Perform(State& state) const {
    const int target_cpu = cpu_index % state.num_cpus;
    if (state.cpu_stopped[target_cpu]) {
      return;
    }
    state.slab.Drain(target_cpu, [&](int cpu, size_t size_class, void** batch,
                                     size_t size, size_t cap) {
      TC_CHECK_EQ(cpu, target_cpu);
      state.HandleDrain(cpu, size_class, batch, size, cap);
    });
    state.expected_capacity[target_cpu].fill(0);

    for (size_t sc = 1; sc < kNumClasses; ++sc) {
      TC_CHECK_EQ(state.slab.Length(target_cpu, sc), 0);
      TC_CHECK_EQ(state.slab.Capacity(target_cpu, sc), 0);
    }
  }
};

struct ReleasePerCPUSlabMetadata {
  bool madvise_fail;

  template <typename Sink>
  friend void AbslStringify(Sink& sink, const ReleasePerCPUSlabMetadata& r) {
    absl::Format(&sink, "ReleasePerCPUSlabMetadata{.madvise_fail=%v}",
                 r.madvise_fail);
  }

  void Perform(State& state) const {
    // Requires all CPUs to be started (it will stop them itself).
    for (int cpu = 0; cpu < state.num_cpus; ++cpu) {
      if (state.cpu_stopped[cpu]) {
        state.slab.StartCpu(cpu);
        state.cpu_stopped[cpu] = false;
      }
    }

    const size_t slab_bytes = size_t{1} << state.slab.GetShift();
    const uintptr_t slabs_start =
        reinterpret_cast<uintptr_t>(state.slabs_block.slabs);
    const uintptr_t slabs_end = slabs_start + state.num_cpus * slab_bytes;
    auto cpu_at = [&](uintptr_t addr) {
      return static_cast<int>((addr - slabs_start) / slab_bytes);
    };

    // Model: a hugepage is released iff it lies entirely within the slabs
    // region, at least one CPU in it is populated, and every populated CPU in
    // it has zero capacity (and therefore no objects).  Partial hugepages at
    // either end are never released, since that would tear a neighbor.
    absl::flat_hash_set<uintptr_t> expected;
    for (uintptr_t hp = slabs_start & ~(kHugePageSize - 1); hp < slabs_end;
         hp += kHugePageSize) {
      if (hp < slabs_start || hp + kHugePageSize > slabs_end) continue;
      bool any_populated = false;
      bool all_drained = true;
      for (uintptr_t addr = hp; addr < hp + kHugePageSize; addr += slab_bytes) {
        const int cpu = cpu_at(addr);
        if (!state.cpu_initialized[cpu]) continue;
        any_populated = true;
        all_drained &= state.IsDrained(cpu);
      }
      if (any_populated && all_drained) expected.insert(hp);
    }

    // ReleaseSlabMetadataForDrainedCpus holds an AllocationGuard while calling
    // back, so size the bookkeeping up front.
    std::vector<bool> released_cpu(state.num_cpus, false);
    std::vector<bool> unpopulated_cpu(state.num_cpus, false);
    size_t released_bytes = 0;
    bool all_madvise_ok = true;

    state.slab.ReleaseSlabMetadataForDrainedCpus(
        [&state](int cpu) { return state.cpu_initialized[cpu]; },
        [&](int cpu) {
          // Unpopulate follows the madvise of the hugepage holding `cpu`.
          TC_CHECK(released_cpu[cpu]);
          TC_CHECK(!unpopulated_cpu[cpu]);
          unpopulated_cpu[cpu] = true;
          state.cpu_initialized[cpu] = false;
          TC_CHECK(state.IsDrained(cpu));
        },
        [&](void* slab_addr, size_t slab_size) {
          const uintptr_t start = reinterpret_cast<uintptr_t>(slab_addr);
          TC_CHECK_EQ(start % kHugePageSize, 0);
          TC_CHECK_EQ(slab_size % kHugePageSize, 0);
          TC_CHECK_GT(slab_size, 0);
          TC_CHECK_GE(start, slabs_start);
          TC_CHECK_LE(start + slab_size, slabs_end);
          for (uintptr_t hp = start; hp < start + slab_size;
               hp += kHugePageSize) {
            TC_CHECK_EQ(expected.erase(hp), 1,
                        "unexpected or duplicate release of hugepage");
          }
          // Never release a CPU that still has capacity or objects, whether or
          // not our model considers it populated.
          for (uintptr_t addr = start; addr < start + slab_size;
               addr += slab_bytes) {
            const int cpu = cpu_at(addr);
            for (size_t sc = 1; sc < kNumClasses; ++sc) {
              TC_CHECK_EQ(state.slab.Length(cpu, sc), 0);
              TC_CHECK_EQ(state.slab.Capacity(cpu, sc), 0);
            }
            TC_CHECK(!released_cpu[cpu]);
            released_cpu[cpu] = true;
          }
          if (madvise_fail) {
            // Simulate that the madvise failed.
            all_madvise_ok = false;
            return -1;
          }
          madvise(slab_addr, slab_size, MADV_NOHUGEPAGE);
          const int rc = madvise(slab_addr, slab_size, MADV_DONTNEED);
          if (rc != 0) {
            all_madvise_ok = false;
            return rc;
          }
          released_bytes += slab_size;
          return 0;
        });

    TC_CHECK(expected.empty(), "%v hugepage(s) were not released",
             expected.size());
    for (int cpu = 0; cpu < state.num_cpus; ++cpu) {
      const bool released = released_cpu[cpu];
      const bool unpopulated = unpopulated_cpu[cpu];
      TC_CHECK_EQ(released, unpopulated, "cpu=%d", cpu);
    }
    if (all_madvise_ok && released_bytes > 0) {
      // Nothing has touched the released hugepages since MADV_DONTNEED.
      const size_t slabs_size = slabs_end - slabs_start;
      const PerCPUMetadataState usage = state.slab.MetadataMemoryUsage();
      TC_CHECK_LE(usage.resident_size, slabs_size - released_bytes);
    }
    state.CheckMetadataUsage();
  }
};

struct ResizeSlabs {
  uint8_t shift_index;
  uint8_t slabs_offset;

  template <typename Sink>
  friend void AbslStringify(Sink& sink, const ResizeSlabs& r) {
    absl::Format(&sink, "ResizeSlabs{.shift_index=%v, .slabs_offset=%v}",
                 r.shift_index, r.slabs_offset);
  }

  void Perform(State& state) const {
    if (absl::c_any_of(state.cpu_stopped, [](bool s) { return s; })) {
      return;
    }
    const uint8_t current_shift = state.slab.GetShift();
    uint8_t target_shift =
        kMinShift + (shift_index % (kMaxShift - kMinShift + 1));
    if (target_shift == current_shift) {
      target_shift =
          (current_shift == kMaxShift) ? kMinShift : current_shift + 1;
    }
    const Shift new_shift = ToShiftType(target_shift);
    const SlabsBlock new_block = state.AllocateSlabs(new_shift, slabs_offset);
    const auto [old_slabs, old_slabs_size] = state.slab.ResizeSlabs(
        new_shift, new_block.slabs,
        [&state](size_t sc) { return state.MaxCapacity(sc); },
        [&state](size_t cpu) { return state.cpu_initialized[cpu]; },
        [&state](int cpu, size_t size_class, void** batch, size_t size,
                 size_t cap) {
          state.HandleDrain(cpu, size_class, batch, size, cap);
        });
    TC_CHECK_EQ(state.slab.GetShift(), target_shift);
    TC_CHECK_EQ(old_slabs_size,
                GetSlabsAllocSize(ToShiftType(current_shift), state.num_cpus));
    state.SwapSlabs(new_block, old_slabs, old_slabs_size);
    auto [got_cpu, cached] = state.slab.CacheCpuSlab();
    if (cached && got_cpu >= 0 && !state.cpu_stopped[got_cpu]) {
      state.EnsureCpuInitialized(got_cpu);
    }
  }
};

struct UpdateMaxCapacities {
  unsigned size_class;
  uint8_t new_max_capacity;
  uint8_t slabs_offset;

  template <typename Sink>
  friend void AbslStringify(Sink& sink, const UpdateMaxCapacities& u) {
    absl::Format(&sink,
                 "UpdateMaxCapacities{.size_class=%v, .new_max_capacity=%v, "
                 ".slabs_offset=%v}",
                 u.size_class, u.new_max_capacity, u.slabs_offset);
  }

  void Perform(State& state) const {
    if (absl::c_any_of(state.cpu_stopped, [](bool s) { return s; })) {
      return;
    }
    const size_t sc = 1 + (size_class % (kNumClasses - 1));
    const size_t target_cap = 1 + (new_max_capacity % kMaxCapacity);
    PerSizeClassMaxCapacity new_caps[1] = {
        {.size_class = sc, .max_capacity = target_cap}};
    const Shift shift = ToShiftType(state.slab.GetShift());
    const SlabsBlock new_block = state.AllocateSlabs(shift, slabs_offset);
    const auto [old_slabs, old_slabs_size] = state.slab.UpdateMaxCapacities(
        new_block.slabs,
        [&state, sc, target_cap](size_t size_class) {
          // Return the new max capacity for the size class we want to grow.
          if (size_class == sc) return target_cap;
          // Since other size classes' max capacities are not changed, we can
          // just return their current max capacities.
          return state.MaxCapacity(size_class);
        },
        [&state](int size_class, uint16_t cap) {
          state.max_capacity[size_class] = cap;
        },
        [&state](size_t cpu) { return state.cpu_initialized[cpu]; },
        [&state](int cpu, size_t size_class, void** batch, size_t size,
                 size_t cap) {
          state.HandleDrain(cpu, size_class, batch, size, cap);
        },
        new_caps, 1);
    TC_CHECK_EQ(state.max_capacity[sc], target_cap);
    TC_CHECK_EQ(old_slabs_size, GetSlabsAllocSize(shift, state.num_cpus));
    state.SwapSlabs(new_block, old_slabs, old_slabs_size);
    auto [got_cpu, cached] = state.slab.CacheCpuSlab();
    if (cached && got_cpu >= 0 && !state.cpu_stopped[got_cpu]) {
      state.EnsureCpuInitialized(got_cpu);
    }
  }
};

struct CacheCpuSlab {
  template <typename Sink>
  friend void AbslStringify(Sink& sink, const CacheCpuSlab&) {
    absl::Format(&sink, "CacheCpuSlab{}");
  }

  void Perform(State& state) const {
    auto [got_cpu, cached] = state.slab.CacheCpuSlab();
    if (cached && got_cpu >= 0 && !state.cpu_stopped[got_cpu]) {
      state.EnsureCpuInitialized(got_cpu);
    }
  }
};

struct UncacheCpuSlab {
  template <typename Sink>
  friend void AbslStringify(Sink& sink, const UncacheCpuSlab&) {
    absl::Format(&sink, "UncacheCpuSlab{}");
  }

  void Perform(State& state) const { state.slab.UncacheCpuSlab(); }
};

struct SwitchCpu {
  uint8_t cpu_index;

  template <typename Sink>
  friend void AbslStringify(Sink& sink, const SwitchCpu& s) {
    absl::Format(&sink, "SwitchCpu{.cpu_index=%v}", s.cpu_index);
  }

  void Perform(State& state) const {
    const int target_cpu = cpu_index % state.num_cpus;
    state.slab.UncacheCpuSlab();
    state.active_cpu.SwitchTo(target_cpu);
    state.current_cpu = target_cpu;
    auto [got_cpu, cached] = state.slab.CacheCpuSlab();
    if (cached && got_cpu >= 0 && !state.cpu_stopped[got_cpu]) {
      state.EnsureCpuInitialized(got_cpu);
    }
  }
};

struct StopCpu {
  uint8_t cpu_index;

  template <typename Sink>
  friend void AbslStringify(Sink& sink, const StopCpu& s) {
    absl::Format(&sink, "StopCpu{.cpu_index=%v}", s.cpu_index);
  }

  void Perform(State& state) const {
    const int target_cpu = cpu_index % state.num_cpus;
    if (state.cpu_stopped[target_cpu]) {
      return;
    }
    state.slab.StopCpu(target_cpu);
    state.cpu_stopped[target_cpu] = true;
  }
};

struct StartCpu {
  uint8_t cpu_index;

  template <typename Sink>
  friend void AbslStringify(Sink& sink, const StartCpu& s) {
    absl::Format(&sink, "StartCpu{.cpu_index=%v}", s.cpu_index);
  }

  void Perform(State& state) const {
    const int target_cpu = cpu_index % state.num_cpus;
    if (!state.cpu_stopped[target_cpu]) {
      return;
    }
    state.slab.StartCpu(target_cpu);
    state.cpu_stopped[target_cpu] = false;
  }
};

using Instruction =
    std::variant<Push, Pop, PushBatch, PopBatch, Grow, GrowOtherClass,
                 ShrinkOtherCache, Drain, ReleasePerCPUSlabMetadata,
                 ResizeSlabs, UpdateMaxCapacities, CacheCpuSlab, UncacheCpuSlab,
                 SwitchCpu, StopCpu, StartCpu>;

template <typename Sink>
void AbslStringify(Sink& sink, const Instruction& i) {
  std::visit([&](auto&& arg) { absl::Format(&sink, "%v", arg); }, i);
}

// `initial_shift` is the slab shift to Init with, in [kMinShift, kMaxShift].
// `slabs_offset` selects the slabs' offset within a hugepage (see SlabsBlock).
void FuzzPercpuTcmalloc(uint8_t initial_shift, uint8_t slabs_offset,
                        const std::vector<Instruction>& instructions) {
  if (MallocExtension::PerCpuCachesActive()) {
    return;
  }
  if (!subtle::percpu::IsFast()) {
    // TODO(b/271282540): Run self-test to ensure this matches with our
    // expectations for whether rseq is available rather than skipping.
    return;
  }

  State state(initial_shift, slabs_offset);

  for (const auto& instruction : instructions) {
    std::visit([&](auto&& arg) { arg.Perform(state); }, instruction);
    state.CheckInvariants();
  }
}

TEST(PercpuTcmallocTest, FuzzPercpuTcmallocRegression) {
  FuzzPercpuTcmalloc(18, 0, {Pop{.size_class = 127}});

  FuzzPercpuTcmalloc(
      18, 0,
      {Grow{.size_class = 2147483647, .len = 185}, Push{.size_class = 0},
       PushBatch{.size_class = 0, .count = 56}});

  FuzzPercpuTcmalloc(
      18, 0,
      {GrowOtherClass{.cpu_index = 0, .size_class = 1, .len = 5},
       ShrinkOtherCache{.cpu_index = 0, .size_class = 1, .len = 2},
       Drain{.cpu_index = 0}, ResizeSlabs{.shift_index = 1, .slabs_offset = 0},
       UpdateMaxCapacities{
           .size_class = 1, .new_max_capacity = 8, .slabs_offset = 0},
       UncacheCpuSlab{}, CacheCpuSlab{}, SwitchCpu{.cpu_index = 1},
       StopCpu{.cpu_index = 1}, StartCpu{.cpu_index = 1}});
}

// Slabs at the largest shift span several hugepages on hosts with more than
// four CPUs.  Populate CPUs in different hugepages, drain some of them, and
// check that metadata release of a leading hugepage, resizing to a
// sub-hugepage slab and back with an unaligned start, release of an interior
// hugepage, and a failing madvise all agree with the model.  The instructions
// are robust to any CPU count because cpu_index wraps.
TEST(PercpuTcmallocTest, MultiPageMetadataRegression) {
  FuzzPercpuTcmalloc(
      kMaxShift, 0,
      {Grow{.size_class = 1, .len = 4},
       Push{.size_class = 1},
       Push{.size_class = 1},
       SwitchCpu{.cpu_index = 1},
       Grow{.size_class = 2, .len = 3},
       Push{.size_class = 2},
       SwitchCpu{.cpu_index = 5},
       Grow{.size_class = 1, .len = 4},
       Push{.size_class = 1},
       SwitchCpu{.cpu_index = 7},
       GrowOtherClass{.cpu_index = 6, .size_class = 3, .len = 2},
       Drain{.cpu_index = 0},
       Drain{.cpu_index = 1},
       Drain{.cpu_index = 2},
       Drain{.cpu_index = 3},
       ReleasePerCPUSlabMetadata{.madvise_fail = false},
       Push{.size_class = 1},
       ReleasePerCPUSlabMetadata{.madvise_fail = false},
       ResizeSlabs{.shift_index = 2, .slabs_offset = 3},
       Push{.size_class = 2},
       ResizeSlabs{.shift_index = 7, .slabs_offset = 1},
       Drain{.cpu_index = 5},
       Drain{.cpu_index = 6},
       ReleasePerCPUSlabMetadata{.madvise_fail = false},
       SwitchCpu{.cpu_index = 4},
       Grow{.size_class = 1, .len = 2},
       Push{.size_class = 1},
       Drain{.cpu_index = 4},
       ReleasePerCPUSlabMetadata{.madvise_fail = true},
       UpdateMaxCapacities{
           .size_class = 2, .new_max_capacity = 5, .slabs_offset = 2},
       Push{.size_class = 2},
       ReleasePerCPUSlabMetadata{.madvise_fail = false},
       ResizeSlabs{.shift_index = 0, .slabs_offset = 0},
       ReleasePerCPUSlabMetadata{.madvise_fail = false}});
}

TEST(PercpuTcmallocTest, ShrinkOtherCacheStringify) {
  EXPECT_EQ(
      absl::StrFormat(
          "%v", ShrinkOtherCache{.cpu_index = 1, .size_class = 2, .len = 3}),
      "ShrinkOtherCache{.cpu_index=1, .size_class=2, .len=3}");
  EXPECT_EQ(absl::StrFormat("%v", UpdateMaxCapacities{.size_class = 1,
                                                      .new_max_capacity = 8,
                                                      .slabs_offset = 2}),
            "UpdateMaxCapacities{.size_class=1, .new_max_capacity=8, "
            ".slabs_offset=2}");
}

FUZZ_TEST(PercpuTcmallocTest, FuzzPercpuTcmalloc)
    .WithDomains(fuzztest::InRange<uint8_t>(kMinShift, kMaxShift),
                 fuzztest::Arbitrary<uint8_t>(),
                 fuzztest::Arbitrary<std::vector<Instruction>>());

}  // namespace
}  // namespace tcmalloc::tcmalloc_internal::subtle::percpu
GOOGLE_MALLOC_SECTION_END
