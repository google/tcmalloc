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

size_t MaxCapacity(size_t size_class) {
  if (size_class == 0 || size_class >= kNumClasses) return 0;
  return kMaxCapacity;
}

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

  absl::flat_hash_set<void*> allocated_objects[kNumClasses];
  std::vector<void*> available_objects[kNumClasses];

  State()
      : num_cpus(NumCPUs()),
        active_cpu(current_cpu),
        cpu_initialized(num_cpus, false),
        cpu_stopped(num_cpus, false) {
    const Shift shift = kShift;
    const size_t slabs_size = GetSlabsAllocSize(shift, num_cpus);
    void* slabs_mem = Malloc(slabs_size, SlabAlignment(shift));
    slab.Init(
        Malloc, slabs_mem, [](size_t sc) { return MaxCapacity(sc); }, shift);
    EnsureCpuInitialized(current_cpu);
  }

  ~State();

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
};

void State::FreeObject(void* obj, size_t size_class) {
  auto it = allocated_objects[size_class].find(obj);
  TC_CHECK(it != allocated_objects[size_class].end());
  allocated_objects[size_class].erase(it);

  ::operator delete(obj, FakeSizeForSizeClass(size_class));
}

void State::EnsureCpuInitialized(int cpu) {
  if (cpu >= 0 && cpu < num_cpus && !cpu_initialized[cpu] &&
      !cpu_stopped[cpu]) {
    slab.InitCpu(cpu, [](size_t sc) { return MaxCapacity(sc); });
    cpu_initialized[cpu] = true;
  }
}

void State::CheckInvariants() {
  for (int cpu = 0; cpu < num_cpus; ++cpu) {
    for (size_t sc = 1; sc < kNumClasses; ++sc) {
      const size_t len = slab.Length(cpu, sc);
      const size_t cap = slab.Capacity(cpu, sc);
      const size_t max_cap = MaxCapacity(sc);
      TC_CHECK_LE(len, cap);
      TC_CHECK_LE(cap, max_cap);
    }
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
    if (pushed < count) {
      // Discard any objects we didn't successfully push.
      for (size_t i = 0; i < count - pushed; ++i) {
        state.FreeObject(batch[i], sc);
      }
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
    size_t grew = state.slab.Grow(state.current_cpu, sc, len,
                                  [sc](uint8_t) { return MaxCapacity(sc); });
    TC_CHECK_LE(grew, len);
  }
};

struct ShrinkOtherCache {
  unsigned size_class;
  uint8_t cpu_index;
  uint8_t len;

  template <typename Sink>
  friend void AbslStringify(Sink& sink, const ShrinkOtherCache& s) {
    absl::Format(&sink,
                 "ShrinkOtherCache{.size_class=%v, .cpu_index=%v, .len=%v}",
                 s.cpu_index, s.size_class, s.len);
  }

  void Perform(State& state) const {
    const int target_cpu = cpu_index % state.num_cpus;
    const size_t sc = 1 + (size_class % (kNumClasses - 1));
    const size_t len = 1 + (this->len % kMaxCapacity);
    if (!state.cpu_initialized[target_cpu]) {
      if (!state.cpu_stopped[target_cpu]) {
        state.EnsureCpuInitialized(target_cpu);
      } else {
        return;
      }
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
      TC_CHECK_LT(size_class, kNumClasses);
      for (size_t i = 0; i < size; ++i) {
        state.CheckValidObject(batch[i], size_class);
        state.available_objects[size_class].push_back(batch[i]);
      }
    });

    for (size_t sc = 1; sc < kNumClasses; ++sc) {
      TC_CHECK_EQ(state.slab.Length(target_cpu, sc), 0);
      const size_t cap = state.slab.Capacity(target_cpu, sc);
      const size_t max_cap = MaxCapacity(sc);
      TC_CHECK_LE(0, cap);
      TC_CHECK_LE(cap, max_cap);
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

// TODO(b/271282540): Add additional coverage for
// * GrowOtherClass
// * ResizeSlabs
// * UpdateMaxCapacities

using Instruction =
    std::variant<Push, Pop, PushBatch, PopBatch, Grow, ShrinkOtherCache, Drain,
                 ReleasePerCPUSlabMetadata, SwitchCpu, StopCpu, StartCpu>;

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
}

FUZZ_TEST(PercpuTcmallocTest, FuzzPercpuTcmalloc)
    .WithDomains(fuzztest::Arbitrary<std::vector<Instruction>>());

}  // namespace
}  // namespace tcmalloc::tcmalloc_internal::subtle::percpu
GOOGLE_MALLOC_SECTION_END
