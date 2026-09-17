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
constexpr Shift kShift{18};

using SlabsType = TcmallocSlab<kNumClasses>;

void* Malloc(size_t size, std::align_val_t alignment) {
  void* ptr = ::operator new(size, alignment);
  memset(ptr, 0, size);
  return ptr;
}

struct State {
  const size_t num_cpus;
  int current_cpu = 0;
  ScopedFakeCpuId active_cpu;
  SlabsType slab;
  std::vector<bool> cpu_initialized;
  std::vector<bool> cpu_stopped;
  std::array<size_t, kNumClasses> max_capacity;

  absl::flat_hash_set<void*> allocated_objects[kNumClasses];
  std::vector<void*> available_objects[kNumClasses];

  State()
      : num_cpus(NumCPUs()),
        active_cpu(current_cpu),
        cpu_initialized(num_cpus, false),
        cpu_stopped(num_cpus, false) {
    for (size_t sc = 0; sc < kNumClasses; ++sc) {
      max_capacity[sc] = (sc == 0) ? 0 : kMaxCapacity;
    }
    const Shift shift = kShift;
    const size_t slabs_size = GetSlabsAllocSize(shift, num_cpus);
    void* slabs_mem = Malloc(slabs_size, SlabAlignment(shift));
    slab.Init(
        Malloc, slabs_mem, [this](size_t sc) { return MaxCapacity(sc); },
        shift);
    EnsureCpuInitialized(current_cpu);
  }

  ~State();

  size_t MaxCapacity(size_t size_class) const {
    if (size_class == 0 || size_class >= kNumClasses) return 0;
    return max_capacity[size_class];
  }

  void EnsureCpuInitialized(int cpu);
  void CheckInvariants();
  void CheckValidObject(void* obj, size_t sc) const;

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

void State::CheckInvariants() {
  for (size_t sc = 1; sc < kNumClasses; ++sc) {
    size_t total_in_slabs = 0;
    const size_t max_cap = MaxCapacity(sc);
    for (int cpu = 0; cpu < num_cpus; ++cpu) {
      const size_t len = slab.Length(cpu, sc);
      const size_t cap = slab.Capacity(cpu, sc);
      TC_CHECK_LE(len, cap);
      TC_CHECK_LE(cap, max_cap);
      total_in_slabs += len;
    }
    TC_CHECK_EQ(available_objects[sc].size() + total_in_slabs,
                allocated_objects[sc].size());
  }
}

void State::CheckValidObject(void* obj, size_t sc) const {
  TC_CHECK_NE(obj, nullptr);
  TC_CHECK_EQ(reinterpret_cast<uintptr_t>(obj) & 1, 0);
  TC_CHECK(allocated_objects[sc].contains(obj));
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
      TC_CHECK_LT(size_class, kNumClasses);
      for (size_t i = 0; i < size; ++i) {
        CheckValidObject(batch[i], size_class);
        available_objects[size_class].push_back(batch[i]);
      }
    });
  }

  for (int cpu = 0; cpu < num_cpus; ++cpu) {
    for (size_t sc = 1; sc < kNumClasses; ++sc) {
      TC_CHECK_EQ(slab.Length(cpu, sc), 0);
      TC_CHECK_EQ(slab.Capacity(cpu, sc), 0);
    }
  }

  slab.Destroy(sized_aligned_delete);

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
    const int cpu = state.current_cpu;
    size_t len_before = state.slab.Length(cpu, sc);
    size_t cap_before = state.slab.Capacity(cpu, sc);
    bool exact = true;
    bool pushed = state.slab.Push(sc, item);
    if (!pushed) {
      auto [got_cpu, cached] = state.slab.CacheCpuSlab();
      if (cached && got_cpu >= 0 && !state.cpu_stopped[got_cpu]) {
        TC_CHECK_EQ(got_cpu, cpu);
        state.EnsureCpuInitialized(got_cpu);
        len_before = state.slab.Length(cpu, sc);
        cap_before = state.slab.Capacity(cpu, sc);
        pushed = state.slab.Push(sc, item);
      } else if (got_cpu < 0) {
        exact = false;
      }
    }
    // A cached slab on a running CPU accepts the item iff it has room.
    if (exact) {
      TC_CHECK_EQ(pushed, len_before < cap_before);
    }
    TC_CHECK_EQ(state.slab.Length(cpu, sc), len_before + pushed);
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
    const int cpu = state.current_cpu;
    size_t len_before = state.slab.Length(cpu, sc);
    bool exact = true;
    void* item = state.slab.Pop(sc);
    if (item == nullptr) {
      auto [got_cpu, cached] = state.slab.CacheCpuSlab();
      if (cached && got_cpu >= 0 && !state.cpu_stopped[got_cpu]) {
        TC_CHECK_EQ(got_cpu, cpu);
        state.EnsureCpuInitialized(got_cpu);
        len_before = state.slab.Length(cpu, sc);
        item = state.slab.Pop(sc);
      } else if (got_cpu < 0) {
        exact = false;
      }
    }
    // A cached slab on a running CPU yields an item iff it holds one.
    if (exact) {
      TC_CHECK_EQ(item != nullptr, len_before > 0);
    }
    TC_CHECK_EQ(state.slab.Length(cpu, sc),
                len_before - (item != nullptr ? 1 : 0));
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
    const int cpu = state.current_cpu;
    size_t len_before = state.slab.Length(cpu, sc);
    size_t cap_before = state.slab.Capacity(cpu, sc);
    bool exact = true;
    size_t pushed = state.slab.PushBatch(sc, batch, count);
    TC_CHECK_LE(pushed, count);
    if (pushed == 0) {
      auto [got_cpu, cached] = state.slab.CacheCpuSlab();
      if (cached && got_cpu >= 0 && !state.cpu_stopped[got_cpu]) {
        TC_CHECK_EQ(got_cpu, cpu);
        state.EnsureCpuInitialized(got_cpu);
        len_before = state.slab.Length(cpu, sc);
        cap_before = state.slab.Capacity(cpu, sc);
        pushed = state.slab.PushBatch(sc, batch, count);
        TC_CHECK_LE(pushed, count);
      } else if (got_cpu < 0) {
        exact = false;
      }
    }
    // A cached slab on a running CPU takes exactly what fits.
    if (exact) {
      const size_t expected = std::min(count, cap_before - len_before);
      TC_CHECK_EQ(pushed, expected);
    }
    TC_CHECK_EQ(state.slab.Length(cpu, sc), len_before + pushed);
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
    const int cpu = state.current_cpu;
    size_t len_before = state.slab.Length(cpu, sc);
    bool exact = true;
    size_t popped = state.slab.PopBatch(sc, batch, count);
    TC_CHECK_LE(popped, count);
    if (popped == 0) {
      auto [got_cpu, cached] = state.slab.CacheCpuSlab();
      if (cached && got_cpu >= 0 && !state.cpu_stopped[got_cpu]) {
        TC_CHECK_EQ(got_cpu, cpu);
        state.EnsureCpuInitialized(got_cpu);
        len_before = state.slab.Length(cpu, sc);
        popped = state.slab.PopBatch(sc, batch, count);
        TC_CHECK_LE(popped, count);
      } else if (got_cpu < 0) {
        exact = false;
      }
    }
    // A cached slab on a running CPU yields exactly what it holds.
    if (exact) {
      const size_t expected = std::min(count, len_before);
      TC_CHECK_EQ(popped, expected);
    }
    TC_CHECK_EQ(state.slab.Length(cpu, sc), len_before - popped);
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
    const size_t cap_before = state.slab.Capacity(state.current_cpu, sc);
    size_t grew = state.slab.Grow(
        state.current_cpu, sc, len,
        [&state, sc](uint8_t) { return state.MaxCapacity(sc); });
    TC_CHECK_LE(grew, len);
    // Grow either aborts (uncached slab) or grows by exactly what fits under
    // the max capacity.
    const size_t expected = std::min(len, state.MaxCapacity(sc) - cap_before);
    TC_CHECK(grew == 0 || grew == expected);
    TC_CHECK_EQ(state.slab.Capacity(state.current_cpu, sc), cap_before + grew);
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
    const size_t cap_before = state.slab.Capacity(target_cpu, sc);
    size_t grew = state.slab.GrowOtherCache(
        target_cpu, sc, len,
        [&state, sc](uint8_t) { return state.MaxCapacity(sc); });
    TC_CHECK_LE(grew, len);
    // A stopped CPU always grows by exactly what fits under the max capacity.
    const size_t expected = std::min(len, state.MaxCapacity(sc) - cap_before);
    TC_CHECK_EQ(grew, expected);
    TC_CHECK_EQ(state.slab.Capacity(target_cpu, sc), cap_before + grew);
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
    const size_t len_before = state.slab.Length(target_cpu, sc);
    const size_t cap_before = state.slab.Capacity(target_cpu, sc);
    size_t returned = 0;
    size_t shrunk = state.slab.ShrinkOtherCache(
        target_cpu, sc, len, [&](size_t size_class, void** batch, size_t size) {
          TC_CHECK_EQ(size_class, sc);
          returned += size;
          for (size_t i = 0; i < size; ++i) {
            state.CheckValidObject(batch[i], size_class);
            state.available_objects[size_class].push_back(batch[i]);
          }
        });
    TC_CHECK_LE(shrunk, len);
    // Capacity drops by exactly min(len, capacity); unused capacity goes
    // first and only the shortfall is popped from the slab.
    const size_t expected_shrunk = std::min(len, cap_before);
    TC_CHECK_EQ(shrunk, expected_shrunk);
    const size_t unused = cap_before - len_before;
    const size_t expected_returned = shrunk > unused ? shrunk - unused : 0;
    TC_CHECK_EQ(returned, expected_returned);
    TC_CHECK_EQ(state.slab.Capacity(target_cpu, sc), cap_before - shrunk);
    TC_CHECK_EQ(state.slab.Length(target_cpu, sc), len_before - returned);
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
    std::array<size_t, kNumClasses> len_before, cap_before;
    for (size_t sc = 1; sc < kNumClasses; ++sc) {
      len_before[sc] = state.slab.Length(target_cpu, sc);
      cap_before[sc] = state.slab.Capacity(target_cpu, sc);
    }
    state.slab.Drain(target_cpu, [&](int cpu, size_t size_class, void** batch,
                                     size_t size, size_t cap) {
      TC_CHECK_EQ(cpu, target_cpu);
      TC_CHECK_LT(size_class, kNumClasses);
      // The handler receives every cached object and the prior capacity.
      TC_CHECK_EQ(size, len_before[size_class]);
      TC_CHECK_EQ(cap, cap_before[size_class]);
      for (size_t i = 0; i < size; ++i) {
        state.CheckValidObject(batch[i], size_class);
        state.available_objects[size_class].push_back(batch[i]);
      }
    });

    // Draining empties the slab and resets every capacity to zero.
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

    state.slab.ReleaseSlabMetadataForDrainedCpus(
        [&state](int cpu) { return state.cpu_initialized[cpu]; },
        [&state](int cpu) {
          state.cpu_initialized[cpu] = false;
          for (size_t size_class = 1; size_class < kNumClasses; ++size_class) {
            TC_CHECK_EQ(state.slab.Length(cpu, size_class), 0);
            TC_CHECK_EQ(state.slab.Capacity(cpu, size_class), 0);
          }
        },
        [&](void* slab_addr, size_t slab_size) {
          if (madvise_fail) {
            // Simulate that the madvise failed.
            return -1;
          } else {
            madvise(slab_addr, slab_size, MADV_NOHUGEPAGE);
            return madvise(slab_addr, slab_size, MADV_DONTNEED);
          }
        });
  }
};

struct ResizeSlabs {
  uint8_t shift_index;

  template <typename Sink>
  friend void AbslStringify(Sink& sink, const ResizeSlabs& r) {
    absl::Format(&sink, "ResizeSlabs{.shift_index=%v}", r.shift_index);
  }

  void Perform(State& state) const {
    if (absl::c_any_of(state.cpu_stopped, [](bool s) { return s; })) {
      return;
    }
    const uint8_t current_shift = state.slab.GetShift();
    constexpr uint8_t kMinShift = 14;
    constexpr uint8_t kMaxShift = 18;
    uint8_t target_shift =
        kMinShift + (shift_index % (kMaxShift - kMinShift + 1));
    if (target_shift == current_shift) {
      target_shift =
          (current_shift == kMaxShift) ? kMinShift : current_shift + 1;
    }
    const Shift new_shift = ToShiftType(target_shift);
    const size_t new_slabs_size = GetSlabsAllocSize(new_shift, state.num_cpus);
    void* new_slabs = Malloc(new_slabs_size, SlabAlignment(new_shift));
    const auto [old_slabs, old_slabs_size] = state.slab.ResizeSlabs(
        new_shift, new_slabs,
        [&state](size_t sc) { return state.MaxCapacity(sc); },
        [&state](size_t cpu) { return state.cpu_initialized[cpu]; },
        [&state](int cpu, size_t size_class, void** batch, size_t size,
                 size_t cap) {
          TC_CHECK_LT(size_class, kNumClasses);
          for (size_t i = 0; i < size; ++i) {
            state.CheckValidObject(batch[i], size_class);
            state.available_objects[size_class].push_back(batch[i]);
          }
        });
    const Shift old_shift = ToShiftType(current_shift);
    sized_aligned_delete(old_slabs, old_slabs_size, SlabAlignment(old_shift));
    auto [got_cpu, cached] = state.slab.CacheCpuSlab();
    if (cached && got_cpu >= 0 && !state.cpu_stopped[got_cpu]) {
      state.EnsureCpuInitialized(got_cpu);
    }
  }
};

struct UpdateMaxCapacities {
  unsigned size_class;
  uint8_t new_max_capacity;

  template <typename Sink>
  friend void AbslStringify(Sink& sink, const UpdateMaxCapacities& u) {
    absl::Format(&sink,
                 "UpdateMaxCapacities{.size_class=%v, .new_max_capacity=%v}",
                 u.size_class, u.new_max_capacity);
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
    const size_t slabs_size = GetSlabsAllocSize(shift, state.num_cpus);
    void* new_slabs = Malloc(slabs_size, SlabAlignment(shift));
    const auto [old_slabs, old_slabs_size] = state.slab.UpdateMaxCapacities(
        new_slabs,
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
          TC_CHECK_LT(size_class, kNumClasses);
          for (size_t i = 0; i < size; ++i) {
            state.CheckValidObject(batch[i], size_class);
            state.available_objects[size_class].push_back(batch[i]);
          }
        },
        new_caps, 1);
    sized_aligned_delete(old_slabs, old_slabs_size, SlabAlignment(shift));
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

void FuzzPercpuTcmalloc(const std::vector<Instruction>& instructions) {
  if (MallocExtension::PerCpuCachesActive()) {
    return;
  }
  if (!subtle::percpu::IsFast()) {
    // TODO(b/271282540): Run self-test to ensure this matches with our
    // expectations for whether rseq is available rather than skipping.
    return;
  }

  State state;

  for (const auto& instruction : instructions) {
    std::visit([&](auto&& arg) { arg.Perform(state); }, instruction);
    state.CheckInvariants();
  }
}

TEST(PercpuTcmallocTest, FuzzPercpuTcmallocRegression) {
  FuzzPercpuTcmalloc({Pop{.size_class = 127}});

  FuzzPercpuTcmalloc({Grow{.size_class = 2147483647, .len = 185},
                      Push{.size_class = 0},
                      PushBatch{.size_class = 0, .count = 56}});

  FuzzPercpuTcmalloc(
      {GrowOtherClass{.cpu_index = 0, .size_class = 1, .len = 5},
       ShrinkOtherCache{.cpu_index = 0, .size_class = 1, .len = 2},
       Drain{.cpu_index = 0}, ResizeSlabs{.shift_index = 1},
       UpdateMaxCapacities{.size_class = 1, .new_max_capacity = 8},
       UncacheCpuSlab{}, CacheCpuSlab{}, SwitchCpu{.cpu_index = 1},
       StopCpu{.cpu_index = 1}, StartCpu{.cpu_index = 1}});
}

TEST(PercpuTcmallocTest, ShrinkOtherCacheStringify) {
  EXPECT_EQ(
      absl::StrFormat(
          "%v", ShrinkOtherCache{.cpu_index = 1, .size_class = 2, .len = 3}),
      "ShrinkOtherCache{.cpu_index=1, .size_class=2, .len=3}");
}

FUZZ_TEST(PercpuTcmallocTest, FuzzPercpuTcmalloc)
    .WithDomains(fuzztest::Arbitrary<std::vector<Instruction>>());

}  // namespace
}  // namespace tcmalloc::tcmalloc_internal::subtle::percpu
GOOGLE_MALLOC_SECTION_END
