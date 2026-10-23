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

// `CpuCache` layers capacity accounting, size-class resizing, slab resizing,
// shuffling, and draining on top of `TcmallocSlab`.  Each of these paths is
// exercised in isolation by cpu_cache_test, but their interleavings are not.
// This harness uses FuzzTest to drive a sequence of "instructions" against a
// single-threaded `CpuCache<TestStaticForwarder>` from a faked CPU and checks
// the accounting invariants after every step.

#include <sys/mman.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "gtest/gtest.h"
#include "fuzztest/fuzztest.h"
#include "absl/base/optimization.h"
#include "absl/numeric/bits.h"
#include "absl/strings/str_format.h"
#include "absl/types/span.h"
#include "tcmalloc/arena.h"
#include "tcmalloc/common.h"
#include "tcmalloc/cpu_cache.h"
#include "tcmalloc/internal/config.h"
#include "tcmalloc/internal/logging.h"
#include "tcmalloc/internal/percpu.h"
#include "tcmalloc/internal/percpu_tcmalloc.h"
#include "tcmalloc/internal/sysinfo.h"
#include "tcmalloc/mock_central_freelist.h"
#include "tcmalloc/mock_cpu_cache.h"
#include "tcmalloc/mock_transfer_cache.h"
#include "tcmalloc/sizemap.h"
#include "tcmalloc/testing/testutil.h"
#include "tcmalloc/transfer_cache.h"

GOOGLE_MALLOC_SECTION_BEGIN
namespace tcmalloc::tcmalloc_internal {
namespace {

// ShardedTransferCacheManagerBase never frees the shards and slots it
// allocates, so initializing it afresh for every fuzz iteration through
// ArenaBasedFakeTransferCacheForwarder would leak.  This forwarder carves them
// out of a fixed arena that is rewound once the previous manager has been
// drained.
class ArenaShardedForwarder : public FakeShardedTransferCacheForwarder {
 public:
  [[nodiscard]] static void* Alloc(ArenaAlloc tag, size_t size,
                                   std::align_val_t alignment) {
    TC_CHECK(tag == ArenaAlloc::kTransferCache);
    const size_t align = static_cast<size_t>(alignment);
    TC_CHECK(absl::has_single_bit(align));
    const uintptr_t base = reinterpret_cast<uintptr_t>(arena_);
    const uintptr_t start = (base + offset_ + align - 1) & ~(align - 1);
    TC_CHECK_LE(start + size, base + sizeof(arena_));
    offset_ = start + size - base;
    return reinterpret_cast<void*>(start);
  }

  static void Reset() { offset_ = 0; }

 private:
  alignas(ABSL_CACHELINE_SIZE) inline static char arena_[4 << 20];
  inline static size_t offset_ = 0;
};

// CpuCache keeps every slab it has ever allocated in slabs_by_shift_ for reuse
// and only returns the current one in Deactivate().  Across thousands of fuzz
// iterations the remainder would exhaust vm.max_map_count, so record every
// mapping handed out and unmap the survivors when the harness tears down.
//
// AllocReportedImpending runs under an AllocationGuard, so the registry is
// fixed-size storage rather than a container.
//
// The sharded transfer cache is owned here rather than taken from
// TestStaticForwarder so that it is backed by FakeCentralFreeList, whose
// objects can be dereferenced and leak-checked, instead of
// MinimalFakeCentralFreeList, which hands out pointers into its own stack
// frame.
class FuzzForwarder : public TestStaticForwarder {
 public:
  using ShardedManager =
      ShardedTransferCacheManagerBase<ArenaShardedForwarder, FakeCpuLayout,
                                      FakeCentralFreeList>;

  static void* AllocCpuCache(size_t size, std::align_val_t alignment) {
    void* ptr = TestStaticForwarder::AllocCpuCache(size, alignment);
    Track(ptr, size);
    return ptr;
  }

  void* AllocReportedImpending(size_t size, std::align_val_t alignment) {
    void* ptr = TestStaticForwarder::AllocReportedImpending(size, alignment);
    Track(ptr, size);
    return ptr;
  }

  static void Dealloc(void* ptr, size_t size, std::align_val_t alignment) {
    Untrack(ptr, size);
    TestStaticForwarder::Dealloc(ptr, size, alignment);
  }

  static void ReleaseLeakedMappings() {
    for (size_t i = 0; i < num_mappings_; ++i) {
      munmap(mappings_[i].ptr, mappings_[i].size);
    }
    num_mappings_ = 0;
  }

  ShardedManager& sharded_transfer_cache() { return sharded_manager_; }
  const ShardedManager& sharded_transfer_cache() const {
    return sharded_manager_;
  }

  void SetGenericShardedCache(bool value) {
    ArenaShardedForwarder::SetGenericShardedCache(value);
  }
  void SetShardedCacheForLargeClassesOnly(bool value) {
    ArenaShardedForwarder::SetShardedCacheForLargeClassesOnly(value);
  }

  // Init() latches the two settings above and allocates fresh shards, so the
  // previous ones must have been drained with DrainShardedCaches() first.
  void InitializeShardedManager(int num_shards) {
    ArenaShardedForwarder::Reset();
    cpu_layout_.Init(num_shards);
    sharded_manager_.Init();
  }

  // Spreads the fuzzed CPUs over every shard.
  void SetCurrentCpu(int cpu) {
    cpu_layout_.SetCurrentCpu((cpu * FakeCpuLayout::kCpusPerShard) %
                              FakeCpuLayout::kNumCpus);
  }

  // Returns everything the sharded caches hold to FakeCentralFreeList.
  void DrainShardedCaches() {
    const int saved_cpu = cpu_layout_.CurrentCpu();
    const int num_shards = cpu_layout_.NumShards();
    FakeCentralFreeList central_freelist;
    for (int shard = 0; shard < num_shards; ++shard) {
      const int cpu = shard * FakeCpuLayout::kCpusPerShard;
      cpu_layout_.SetCurrentCpu(cpu);
      for (int size_class = 1; size_class < kNumClasses; ++size_class) {
        void* batch[kMaxObjectsToMove];
        for (size_t remaining = sharded_manager_.tc_length(cpu, size_class);
             remaining > 0;) {
          const size_t n = std::min(remaining, kMaxObjectsToMove);
          const int removed = sharded_manager_.RemoveRange(
              size_class, absl::MakeSpan(batch, n));
          TC_CHECK_GT(removed, 0);
          TC_CHECK_LE(removed, n);
          central_freelist.FreeBatch(absl::MakeSpan(batch, removed));
          remaining -= removed;
        }
      }
    }
    cpu_layout_.SetCurrentCpu(saved_cpu);
  }

 private:
  struct Mapping {
    void* ptr;
    size_t size;
  };

  // resize_, the initial slab, and every (shift, resize copy) pair.
  static constexpr size_t kMaxMappings = 2 + kTotalPossibleSlabs + 16;

  static void Track(void* ptr, size_t size) {
    if (ptr == MAP_FAILED) return;
    TC_CHECK_LT(num_mappings_, kMaxMappings);
    mappings_[num_mappings_++] = {ptr, size};
  }

  static void Untrack(void* ptr, size_t size) {
    for (size_t i = 0; i < num_mappings_; ++i) {
      if (mappings_[i].ptr != ptr) continue;
      TC_CHECK_EQ(mappings_[i].size, size);
      mappings_[i] = mappings_[--num_mappings_];
      return;
    }
    TC_BUG("Dealloc of untracked mapping %p", ptr);
  }

  inline static std::array<Mapping, kMaxMappings> mappings_ = {};
  inline static size_t num_mappings_ = 0;

  FakeCpuLayout cpu_layout_;
  ShardedManager sharded_manager_{&cpu_layout_};
};

using CpuCache = CpuCache<FuzzForwarder>;
using MissCount = CpuCache::MissCount;
using PerClassMissType = CpuCache::PerClassMissType;

// TestStaticForwarder's transfer cache only backs these size classes.
constexpr std::array<size_t, 3> kSizeClasses = {
    1, 2, ThreeSizeClassForwarder::kColdSizeClass};

// Cap the number of distinct fake CPUs so that each one sees enough traffic to
// populate, resize, and be stolen from.  NumCPUs() is not overridable through
// the forwarder, so accounting still spans every CPU.
constexpr int kMaxFuzzCpus = 4;

// Per-CPU cache limits, chosen so that StealFromOtherCache (available <
// kMaxSize) and ResizeCpuSizeClasses (available < kMaxCpuCacheSize) are
// reachable as well as the default.
constexpr std::array<uint64_t, 6> kCacheLimits = {
    1 << 10, 4 << 10, 64 << 10, 256 << 10, kMaxCpuCacheSize, 128 << 20};

// Which experiment, if any, routes objects through the sharded transfer cache.
// ShardedTransferCacheManagerBase::Init() latches the choice when the
// allocator starts up and nothing changes it afterwards, so it is part of the
// configuration rather than an instruction.
enum class ShardedBacking : uint8_t {
  kNone,
  // Size classes of at least 4 KiB bypass the per-CPU cache altogether.
  kLargeClassesOnly,
  // The sharded cache backs every size class's per-CPU cache in place of the
  // regular transfer cache.
  kGeneric,
};

template <typename Sink>
void AbslStringify(Sink& sink, ShardedBacking backing) {
  switch (backing) {
    case ShardedBacking::kNone:
      sink.Append("ShardedBacking::kNone");
      return;
    case ShardedBacking::kLargeClassesOnly:
      sink.Append("ShardedBacking::kLargeClassesOnly");
      return;
    case ShardedBacking::kGeneric:
      sink.Append("ShardedBacking::kGeneric");
      return;
  }
  absl::Format(&sink, "static_cast<ShardedBacking>(%v)",
               static_cast<int>(backing));
}

struct Config {
  bool dynamic_slab_enabled;
  bool release_drained_slab_metadata;
  bool use_real_size_map;
  ShardedBacking sharded_backing;
  uint8_t cache_limit_index;

  template <typename Sink>
  friend void AbslStringify(Sink& sink, const Config& c) {
    absl::Format(&sink,
                 "Config{.dynamic_slab_enabled=%v, "
                 ".release_drained_slab_metadata=%v, .use_real_size_map=%v, "
                 ".sharded_backing=%v, .cache_limit_index=%v}",
                 c.dynamic_slab_enabled, c.release_drained_slab_metadata,
                 c.use_real_size_map, c.sharded_backing, c.cache_limit_index);
  }
};

class State {
 public:
  explicit State(const Config& config)
      : num_cpus_(NumCPUs()),
        num_fuzz_cpus_(std::min(num_cpus_, kMaxFuzzCpus)) {
    cache_.Init();
    FuzzForwarder& forwarder = cache_.forwarder();
    forwarder.dynamic_slab_enabled_ = config.dynamic_slab_enabled;
    forwarder.release_drained_slab_metadata_ =
        config.release_drained_slab_metadata;
    if (config.use_real_size_map) {
      SizeMap size_map;
      TC_CHECK(size_map.Init(size_map.CurrentClasses().classes));
      forwarder.size_map_ = size_map;
    }
    const bool generic = config.sharded_backing == ShardedBacking::kGeneric;
    const bool large_classes_only =
        config.sharded_backing == ShardedBacking::kLargeClassesOnly;
    forwarder.SetGenericShardedCache(generic);
    forwarder.SetShardedCacheForLargeClassesOnly(large_classes_only);
    forwarder.InitializeShardedManager(
        FuzzForwarder::ShardedManager::kMinShardsAllowed);
    const FuzzForwarder::ShardedManager& sharded =
        forwarder.sharded_transfer_cache();
    TC_CHECK_EQ(sharded.UseGenericCache(), generic);
    TC_CHECK_EQ(sharded.UseCacheForLargeClassesOnly(), large_classes_only);
    TC_CHECK_EQ(sharded.should_use(1), generic || large_classes_only);
    TC_CHECK_EQ(sharded.should_use(2), generic);
    // Activate() copies CacheLimit() into every CPU's capacity.
    cache_.SetCacheLimit(
        kCacheLimits[config.cache_limit_index % kCacheLimits.size()]);
    cache_.Activate();
    SwitchCpu(0);
  }

  ~State();

  CpuCache& cache() { return cache_; }
  FuzzForwarder& forwarder() { return cache_.forwarder(); }
  int num_cpus() const { return num_cpus_; }
  int current_cpu() const { return current_cpu_; }

  int FuzzCpu(uint8_t cpu_index) const { return cpu_index % num_fuzz_cpus_; }
  static size_t SizeClass(uint8_t size_class_index) {
    return kSizeClasses[size_class_index % kSizeClasses.size()];
  }
  std::vector<void*>& held(size_t size_class) {
    return held_[std::find(kSizeClasses.begin(), kSizeClasses.end(),
                           size_class) -
                 kSizeClasses.begin()];
  }

  // Emulates the fence that follows every remote slab operation (Drain,
  // Shrink, ResizeSlabs, ReleaseSlabMetadataForDrainedCpus, ...).  In
  // production, StopCpu/StopAllCpus issue a membarrier whose rseq
  // notify-resume makes the kernel rewrite __rseq_abi.cpu_id_start, whose
  // bytes overlap the cached-slab bit in tcmalloc_slabs, so every thread
  // re-caches its slab (re-populating an unpopulated CPU) on its next slow
  // path.  ScopedFakeCpuId leaves rseq unregistered and the kernel never does
  // so; re-registering rseq (destroying and recreating the ScopedFakeCpuId)
  // has the same effect.  The kernel can produce the same state at any time
  // through preemption, so this never hides a bug.
  void Fence() {
    fake_cpu_.reset();
    fake_cpu_.emplace(current_cpu_);
#if TCMALLOC_INTERNAL_PERCPU_USE_RSEQ
    TC_CHECK_EQ(subtle::percpu::tcmalloc_slabs & TCMALLOC_CACHED_SLABS_MASK, 0);
#endif
  }

  // Fakes migrating to <cpu>.  ScopedFakeCpuId::SwitchTo leaves the slab
  // pointer cached in tcmalloc_slabs, so the fast path would keep operating on
  // the previous CPU's slab; migration is a fence as well.
  void SwitchCpu(int cpu) {
    if (fake_cpu_.has_value() && current_cpu_ == cpu) return;
    current_cpu_ = cpu;
    forwarder().SetCurrentCpu(cpu);
    Fence();
  }

  void Allocate(size_t size_class, size_t count);
  void Deallocate(size_t size_class, size_t count);

  void CheckInvariants();

 private:
  const int num_cpus_;
  const int num_fuzz_cpus_;
  CpuCache cache_;
  std::optional<ScopedFakeCpuId> fake_cpu_;
  int current_cpu_ = -1;
  std::array<std::vector<void*>, kSizeClasses.size()> held_;
};

void State::Allocate(size_t size_class, size_t count) {
  std::vector<void*>& objects = held(size_class);
  for (size_t i = 0; i < count; ++i) {
    void* ptr = cache_.Allocate(size_class);
    TC_CHECK_NE(ptr, nullptr);
    // FakeCentralFreeList hands out 4-byte objects from the system allocator;
    // touching the first byte lets ASan flag an object that was freed while
    // still cached.
    *static_cast<char*>(ptr) = 1;
    objects.push_back(ptr);
  }
}

void State::Deallocate(size_t size_class, size_t count) {
  std::vector<void*>& objects = held(size_class);
  count = std::min(count, objects.size());
  for (size_t i = 0; i < count; ++i) {
    void* ptr = objects.back();
    objects.pop_back();
    *static_cast<char*>(ptr) = 2;
    cache_.Deallocate(ptr, size_class);
  }
}

void State::CheckInvariants() {
  const uint8_t shift = CpuCachePeer::GetSlabShift(cache_);
  const SlabShiftBounds bounds = cache_.GetPerCpuSlabShiftBounds();
  TC_CHECK_GE(shift, bounds.initial_shift);
  TC_CHECK_LE(shift, bounds.max_shift);

  uint64_t total_capacity = 0;
  uint64_t total_used = 0;
  for (int cpu = 0; cpu < num_cpus_; ++cpu) {
    const uint64_t allocated = cache_.Allocated(cpu);
    const uint64_t unallocated = cache_.Unallocated(cpu);
    const uint64_t capacity = cache_.Capacity(cpu);
    TC_CHECK_EQ(allocated + unallocated, capacity, "cpu=%v", cpu);
    total_capacity += capacity;
    const uint64_t used = cache_.UsedBytes(cpu);
    TC_CHECK_LE(used, allocated, "cpu=%v", cpu);
    total_used += used;
    if (!cache_.HasPopulated(cpu)) {
      TC_CHECK_EQ(allocated, 0, "cpu=%v", cpu);
    }
  }
  // Shuffling moves capacity between CPUs but never creates or destroys it,
  // and SetCacheLimit changes every CPU by the same amount.
  TC_CHECK_EQ(total_capacity, cache_.CacheLimit() * num_cpus_);
  TC_CHECK_EQ(cache_.TotalUsedBytes(), total_used);

  uint64_t used_by_class = 0;
  for (size_t size_class : kSizeClasses) {
    used_by_class += cache_.TotalObjectsOfClass(size_class) *
                     forwarder().class_to_size(size_class);
    const CpuCache::SizeClassCapacityStats stats =
        cache_.GetSizeClassCapacityStats(size_class);
    TC_CHECK_LE(static_cast<double>(stats.min_capacity), stats.avg_capacity);
    TC_CHECK_LE(stats.avg_capacity, static_cast<double>(stats.max_capacity));
    TC_CHECK_LE(stats.max_capacity, cache_.GetMaxCapacity(size_class));
  }
  TC_CHECK_EQ(used_by_class, total_used);
}

State::~State() {
  for (size_t size_class : kSizeClasses) {
    Deallocate(size_class, held(size_class).size());
  }
  for (int cpu = 0; cpu < num_cpus_; ++cpu) {
    cache_.Drain(cpu);
  }
  TC_CHECK_EQ(cache_.TotalUsedBytes(), 0);
  cache_.Deactivate();

  // Return everything the CPU caches pushed into the transfer caches to
  // FakeCentralFreeList so that nothing leaks.
  FakeCentralFreeList central_freelist;
  auto& transfer_cache = forwarder().transfer_cache();
  for (size_t size_class : kSizeClasses) {
    const size_t batch_size = transfer_cache.num_objects_to_move(size_class);
    std::vector<void*> batch(batch_size);
    for (size_t remaining = transfer_cache.tc_length(size_class);
         remaining > 0;) {
      const size_t n = std::min(remaining, batch_size);
      const int removed = transfer_cache.RemoveRange(
          size_class, absl::MakeSpan(batch).first(n));
      TC_CHECK_GT(removed, 0);
      TC_CHECK_LE(removed, n);
      central_freelist.FreeBatch(absl::MakeSpan(batch).first(removed));
      remaining -= removed;
    }
  }
  forwarder().DrainShardedCaches();
  forwarder().SetGenericShardedCache(false);
  forwarder().SetShardedCacheForLargeClassesOnly(false);

  fake_cpu_.reset();
  FuzzForwarder::ReleaseLeakedMappings();
}

// Instructions.  Each carries the fuzzer-chosen operands, a Perform method that
// applies it to a State, and an AbslStringify that prints valid C++ so that a
// failing input can be pasted into a regression test.

struct Allocate {
  uint8_t cpu_index;
  uint8_t size_class_index;
  uint8_t count;

  template <typename Sink>
  friend void AbslStringify(Sink& sink, const Allocate& a) {
    absl::Format(&sink,
                 "Allocate{.cpu_index=%v, .size_class_index=%v, .count=%v}",
                 a.cpu_index, a.size_class_index, a.count);
  }

  void Perform(State& state) const {
    state.SwitchCpu(state.FuzzCpu(cpu_index));
    state.Allocate(State::SizeClass(size_class_index), count);
  }
};

struct Deallocate {
  uint8_t cpu_index;
  uint8_t size_class_index;
  uint8_t count;

  template <typename Sink>
  friend void AbslStringify(Sink& sink, const Deallocate& d) {
    absl::Format(&sink,
                 "Deallocate{.cpu_index=%v, .size_class_index=%v, .count=%v}",
                 d.cpu_index, d.size_class_index, d.count);
  }

  void Perform(State& state) const {
    state.SwitchCpu(state.FuzzCpu(cpu_index));
    state.Deallocate(State::SizeClass(size_class_index), count);
  }
};

// Allocates and then frees whole batches on one CPU.  Repeated rounds drive a
// size class to its maximum capacity, which is the only way to record the
// max-capacity misses that GetUpdatedMaxCapacities acts on.
struct Churn {
  uint8_t cpu_index;
  uint8_t size_class_index;
  uint8_t rounds;

  template <typename Sink>
  friend void AbslStringify(Sink& sink, const Churn& c) {
    absl::Format(&sink,
                 "Churn{.cpu_index=%v, .size_class_index=%v, .rounds=%v}",
                 c.cpu_index, c.size_class_index, c.rounds);
  }

  void Perform(State& state) const {
    state.SwitchCpu(state.FuzzCpu(cpu_index));
    const size_t size_class = State::SizeClass(size_class_index);
    const size_t batch_size = state.forwarder().num_objects_to_move(size_class);
    for (int round = 1; round <= rounds % 32; ++round) {
      state.Allocate(size_class, round * batch_size);
      state.Deallocate(size_class, round * batch_size);
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
    const int cpu = state.FuzzCpu(cpu_index);
    CpuCache& cache = state.cache();
    const bool populated = cache.HasPopulated(cpu);
    const uint64_t drains_before = cache.GetNumDrains(cpu);
    const uint64_t used_before = cache.UsedBytes(cpu);
    const uint64_t drained = cache.Drain(cpu);
    state.Fence();
    TC_CHECK_EQ(drained, used_before);
    TC_CHECK_EQ(cache.UsedBytes(cpu), 0);
    TC_CHECK_EQ(cache.GetNumDrains(cpu), drains_before + (populated ? 1 : 0));
  }
};

// Drains <cpu> and marks it unpopulated, as TryDrainingCaches does when it
// releases slab metadata, without depending on hugepage alignment.  The next
// allocation on the CPU must take the Populate path again.
struct Reclaim {
  uint8_t cpu_index;

  template <typename Sink>
  friend void AbslStringify(Sink& sink, const Reclaim& r) {
    absl::Format(&sink, "Reclaim{.cpu_index=%v}", r.cpu_index);
  }

  void Perform(State& state) const {
    const int cpu = state.FuzzCpu(cpu_index);
    CpuCache& cache = state.cache();
    const bool populated = cache.HasPopulated(cpu);
    const uint64_t used_before = cache.UsedBytes(cpu);
    const uint64_t unpopulates_before = cache.GetNumUnpopulates();
    const uint64_t reclaimed = CpuCachePeer::Reclaim(cache, cpu);
    state.Fence();
    TC_CHECK_EQ(reclaimed, used_before);
    TC_CHECK(!cache.HasPopulated(cpu));
    TC_CHECK_EQ(cache.UsedBytes(cpu), 0);
    TC_CHECK_EQ(cache.Allocated(cpu), 0);
    TC_CHECK_EQ(cache.GetNumUnpopulates(),
                unpopulates_before + (populated ? 1 : 0));
  }
};

// Releases the slab metadata of every drained CPU.  Only whole hugepages whose
// CPUs are all drained are released, so which drained CPUs become unpopulated
// depends on the slab's alignment; the oracle checks what must hold either
// way.
struct ReleaseDrainedMetadata {
  template <typename Sink>
  friend void AbslStringify(Sink& sink, const ReleaseDrainedMetadata&) {
    absl::Format(&sink, "ReleaseDrainedMetadata{}");
  }

  void Perform(State& state) const {
    CpuCache& cache = state.cache();
    std::vector<bool> populated_before(state.num_cpus());
    std::vector<bool> drained_before(state.num_cpus());
    for (int cpu = 0; cpu < state.num_cpus(); ++cpu) {
      populated_before[cpu] = cache.HasPopulated(cpu);
      drained_before[cpu] = cache.Allocated(cpu) == 0;
    }
    const uint64_t unpopulates_before = cache.GetNumUnpopulates();
    CpuCachePeer::ReleaseDrainedSlabMetadata(cache);
    state.Fence();
    uint64_t unpopulated = 0;
    for (int cpu = 0; cpu < state.num_cpus(); ++cpu) {
      if (cache.HasPopulated(cpu)) {
        TC_CHECK(populated_before[cpu], "cpu=%v", cpu);
        continue;
      }
      if (!populated_before[cpu]) continue;
      TC_CHECK(drained_before[cpu], "cpu=%v", cpu);
      ++unpopulated;
    }
    TC_CHECK_EQ(cache.GetNumUnpopulates(), unpopulates_before + unpopulated);
  }
};

// TryDrainingCaches drains a CPU only if its usage and miss counts were
// unchanged since the previous call, so a back-to-back pair with nothing in
// between must leave every cache empty.
struct TryDrainingCaches {
  template <typename Sink>
  friend void AbslStringify(Sink& sink, const TryDrainingCaches&) {
    absl::Format(&sink, "TryDrainingCaches{}");
  }

  void Perform(State& state) const {
    CpuCache& cache = state.cache();
    const uint64_t drains_before = cache.GetNumDrains();
    cache.TryDrainingCaches();
    cache.TryDrainingCaches();
    state.Fence();
    TC_CHECK_EQ(cache.TotalUsedBytes(), 0);
    TC_CHECK_GE(cache.GetNumDrains(), drains_before);
  }
};

struct ShuffleCpuCaches {
  template <typename Sink>
  friend void AbslStringify(Sink& sink, const ShuffleCpuCaches&) {
    absl::Format(&sink, "ShuffleCpuCaches{}");
  }

  void Perform(State& state) const {
    state.cache().ShuffleCpuCaches();
    state.Fence();
  }
};

struct ResizeSizeClasses {
  template <typename Sink>
  friend void AbslStringify(Sink& sink, const ResizeSizeClasses&) {
    absl::Format(&sink, "ResizeSizeClasses{}");
  }

  void Perform(State& state) const {
    CpuCache& cache = state.cache();
    const uint64_t resizes_before = cache.GetNumResizes();
    cache.ResizeSizeClasses();
    state.Fence();
    TC_CHECK_GE(cache.GetNumResizes(), resizes_before);
  }
};

// Raising one size class's maximum capacity lowers others by the same slab
// bytes, so the sum over all size classes at the maximum shift is conserved.
struct ResizeSizeClassMaxCapacities {
  template <typename Sink>
  friend void AbslStringify(Sink& sink, const ResizeSizeClassMaxCapacities&) {
    absl::Format(&sink, "ResizeSizeClassMaxCapacities{}");
  }

  static uint64_t SumMaxCapacities(const CpuCache& cache) {
    uint64_t total = 0;
    for (int size_class = 1; size_class < kNumClasses; ++size_class) {
      total += cache.GetMaxCapacity(size_class);
    }
    return total;
  }

  void Perform(State& state) const {
    CpuCache& cache = state.cache();
    const uint64_t before = SumMaxCapacities(cache);
    cache.ResizeSizeClassMaxCapacities();
    state.Fence();
    TC_CHECK_EQ(SumMaxCapacities(cache), before);
  }
};

// TestStaticForwarder maps DynamicSlab::kGrow to an always-satisfied grow
// threshold and DynamicSlab::kShrink to shrinking whenever the interval saw
// any underflow, so the resulting shift is fully determined here.
struct ResizeSlab {
  uint8_t mode;

  template <typename Sink>
  friend void AbslStringify(Sink& sink, const ResizeSlab& r) {
    absl::Format(&sink, "ResizeSlab{.mode=%v}", r.mode);
  }

  void Perform(State& state) const {
    CpuCache& cache = state.cache();
    constexpr std::array<DynamicSlab, 3> kModes = {
        DynamicSlab::kNoop, DynamicSlab::kGrow, DynamicSlab::kShrink};
    const DynamicSlab dynamic_slab = kModes[mode % kModes.size()];
    state.forwarder().dynamic_slab_ = dynamic_slab;

    size_t interval_underflows = 0;
    for (int cpu = 0; cpu < state.num_cpus(); ++cpu) {
      interval_underflows +=
          cache.GetIntervalCacheMissStats(cpu, MissCount::kSlabResize)
              .underflows;
    }
    const SlabShiftBounds bounds = cache.GetPerCpuSlabShiftBounds();
    const uint8_t shift_before = CpuCachePeer::GetSlabShift(cache);
    uint8_t expected = shift_before;
    if (dynamic_slab == DynamicSlab::kGrow && shift_before < bounds.max_shift) {
      ++expected;
    } else if (dynamic_slab == DynamicSlab::kShrink &&
               interval_underflows > 0 && shift_before > bounds.initial_shift) {
      --expected;
    }

    const size_t impending_calls_before =
        state.forwarder().shrink_to_usage_limit_calls_;
    cache.ResizeSlabIfNeeded();
    state.Fence();
    TC_CHECK_EQ(CpuCachePeer::GetSlabShift(cache), expected);
    TC_CHECK_EQ(state.forwarder().shrink_to_usage_limit_calls_,
                impending_calls_before + (expected != shift_before ? 1 : 0));
    TC_CHECK_EQ(state.forwarder().arena_reported_impending_bytes_, 0);
  }
};

struct SetCacheLimit {
  uint8_t limit_index;

  template <typename Sink>
  friend void AbslStringify(Sink& sink, const SetCacheLimit& s) {
    absl::Format(&sink, "SetCacheLimit{.limit_index=%v}", s.limit_index);
  }

  void Perform(State& state) const {
    CpuCache& cache = state.cache();
    const uint64_t limit = kCacheLimits[limit_index % kCacheLimits.size()];
    const uint64_t prev = cache.CacheLimit();
    std::vector<uint64_t> capacities(state.num_cpus());
    for (int cpu = 0; cpu < state.num_cpus(); ++cpu) {
      capacities[cpu] = cache.Capacity(cpu);
    }

    cache.SetCacheLimit(limit);
    TC_CHECK_EQ(cache.CacheLimit(), limit);
    // Growing keeps the redistribution made by ShuffleCpuCaches and adds the
    // difference to every CPU; shrinking resets every CPU to the new limit.
    for (int cpu = 0; cpu < state.num_cpus(); ++cpu) {
      uint64_t expected = capacities[cpu];
      if (limit > prev) {
        expected += limit - prev;
      } else if (limit < prev) {
        expected = limit;
      }
      TC_CHECK_EQ(cache.Capacity(cpu), expected, "cpu=%v prev=%v", cpu, prev);
    }
  }
};

size_t PrintBufferSize(const State& state) {
  return state.num_cpus() * 512 + kNumClasses * 1024 + 8192;
}

struct Print {
  template <typename Sink>
  friend void AbslStringify(Sink& sink, const Print&) {
    absl::Format(&sink, "Print{}");
  }

  void Perform(State& state) const {
    const std::string out = PrintToString(
        PrintBufferSize(state), [&](Printer& p) { state.cache().Print(p); });
    TC_CHECK(!out.empty());
  }
};

struct PrintInPbtxt {
  template <typename Sink>
  friend void AbslStringify(Sink& sink, const PrintInPbtxt&) {
    absl::Format(&sink, "PrintInPbtxt{}");
  }

  void Perform(State& state) const {
    CpuCache& cache = state.cache();
    const std::string out =
        PrintToString(PrintBufferSize(state),
                      [&](PbtxtRegion& region) { cache.PrintInPbtxt(region); });
    TC_CHECK(!out.empty());
  }
};

struct ReadStats {
  uint8_t cpu_index;
  uint8_t size_class_index;

  template <typename Sink>
  friend void AbslStringify(Sink& sink, const ReadStats& r) {
    absl::Format(&sink, "ReadStats{.cpu_index=%v, .size_class_index=%v}",
                 r.cpu_index, r.size_class_index);
  }

  void Perform(State& state) const {
    CpuCache& cache = state.cache();
    const int cpu = state.FuzzCpu(cpu_index);
    const size_t size_class = State::SizeClass(size_class_index);

    const CpuCache::CpuCacheMissStats total = cache.GetTotalCacheMissStats(cpu);
    const CpuCache::CpuCacheMissStats interval =
        cache.GetAndUpdateIntervalCacheMissStats(cpu, MissCount::kDrain);
    TC_CHECK_LE(interval.underflows, total.underflows);
    TC_CHECK_LE(interval.overflows, total.overflows);
    const CpuCache::CpuCacheMissStats after_update =
        cache.GetIntervalCacheMissStats(cpu, MissCount::kDrain);
    TC_CHECK_EQ(after_update.underflows, 0);
    TC_CHECK_EQ(after_update.overflows, 0);

    // Interval reads are pure and must be stable between calls.
    for (const auto [total_type, interval_type] :
         {std::pair{PerClassMissType::kCapacityTotal,
                    PerClassMissType::kCapacityResize},
          std::pair{PerClassMissType::kMaxCapacityTotal,
                    PerClassMissType::kMaxCapacityResize}}) {
      const size_t misses = cache.GetIntervalSizeClassMisses(
          cpu, size_class, total_type, interval_type);
      TC_CHECK_EQ(misses, cache.GetIntervalSizeClassMisses(
                              cpu, size_class, total_type, interval_type));
    }
    TC_CHECK_LE(cache.GetCapacityOfSizeClass(cpu, size_class),
                cache.GetMaxCapacity(size_class));

    const PerCPUMetadataState metadata = cache.MetadataMemoryUsage();
    TC_CHECK_LE(metadata.resident_size, metadata.virtual_size);
    TC_CHECK_GE(cache.MaxTouchedCpu(), cache.CountTouchedCpus() - 1);
  }
};

using Instruction =
    std::variant<Allocate, Deallocate, Churn, Drain, Reclaim,
                 ReleaseDrainedMetadata, TryDrainingCaches, ShuffleCpuCaches,
                 ResizeSizeClasses, ResizeSizeClassMaxCapacities, ResizeSlab,
                 SetCacheLimit, Print, PrintInPbtxt, ReadStats>;

template <typename Sink>
void AbslStringify(Sink& sink, const Instruction& i) {
  std::visit([&](auto&& arg) { absl::Format(&sink, "%v", arg); }, i);
}

// Unlike percpu_tcmalloc_fuzz, this binary links cpu_cache.cc, whose module
// initializer activates the global CpuCache even under the system allocator,
// so MallocExtension::PerCpuCachesActive() is not a usable guard here.  As in
// cpu_cache_test, the global cache is never used once the fuzzed one exists.
//
// Skipping is safe: the RestartableSequences.HasWorkingRseq self-test linked in
// from //tcmalloc/testing:rseq_util fails if IsFast() disagrees
// with whether this environment is expected to have working rseq.
bool CanFuzz() { return subtle::percpu::IsFast(); }

void FuzzCpuCache(const Config& config,
                  const std::vector<Instruction>& instructions) {
  if (!CanFuzz()) {
    return;
  }

  // State embeds a SizeMap and is too large for the stack.
  auto state = std::make_unique<State>(config);
  state->CheckInvariants();
  for (const Instruction& instruction : instructions) {
    std::visit([&](auto&& arg) { arg.Perform(*state); }, instruction);
    state->CheckInvariants();
  }
}

// Every instruction at least once, under a small cache limit so that stealing
// and per-CPU size class resizing engage.
TEST(CpuCacheFuzzTest, Smoke) {
  if (!CanFuzz()) {
    GTEST_SKIP() << "rseq unavailable";
  }
  for (const bool dynamic_slab_enabled : {false, true}) {
    FuzzCpuCache(
        Config{.dynamic_slab_enabled = dynamic_slab_enabled,
               .release_drained_slab_metadata = true,
               .use_real_size_map = false,
               .sharded_backing = ShardedBacking::kNone,
               .cache_limit_index = 0},
        {Allocate{.cpu_index = 0, .size_class_index = 0, .count = 100},
         Allocate{.cpu_index = 1, .size_class_index = 1, .count = 8},
         Allocate{.cpu_index = 2, .size_class_index = 2, .count = 5},
         Deallocate{.cpu_index = 0, .size_class_index = 0, .count = 100},
         Churn{.cpu_index = 0, .size_class_index = 0, .rounds = 8},
         Churn{.cpu_index = 1, .size_class_index = 1, .rounds = 4},
         ReadStats{.cpu_index = 0, .size_class_index = 0},
         ShuffleCpuCaches{},
         ResizeSizeClasses{},
         ResizeSizeClassMaxCapacities{},
         Print{},
         PrintInPbtxt{},
         ResizeSlab{.mode = 1},
         ResizeSlab{.mode = 2},
         ResizeSlab{.mode = 0},
         Drain{.cpu_index = 1},
         SetCacheLimit{.limit_index = 4},
         ShuffleCpuCaches{},
         TryDrainingCaches{},
         Allocate{.cpu_index = 3, .size_class_index = 0, .count = 40},
         TryDrainingCaches{},
         Reclaim{.cpu_index = 0},
         Reclaim{.cpu_index = 3},
         Allocate{.cpu_index = 0, .size_class_index = 0, .count = 10},
         Deallocate{.cpu_index = 0, .size_class_index = 0, .count = 10},
         Drain{.cpu_index = 0},
         ReleaseDrainedMetadata{},
         PrintInPbtxt{},
         Churn{.cpu_index = 1, .size_class_index = 0, .rounds = 4},
         Allocate{.cpu_index = 2, .size_class_index = 1, .count = 8},
         Drain{.cpu_index = 1},
         Allocate{.cpu_index = 3, .size_class_index = 0, .count = 20},
         Deallocate{.cpu_index = 3, .size_class_index = 0, .count = 20},
         Deallocate{.cpu_index = 1, .size_class_index = 1, .count = 16},
         Deallocate{.cpu_index = 2, .size_class_index = 2, .count = 5},
         Deallocate{.cpu_index = 3, .size_class_index = 0, .count = 40},
         PrintInPbtxt{}});
  }
}

TEST(CpuCacheFuzzTest, RealSizeMap) {
  if (!CanFuzz()) {
    GTEST_SKIP() << "rseq unavailable";
  }
  FuzzCpuCache(Config{.dynamic_slab_enabled = true,
                      .release_drained_slab_metadata = false,
                      .use_real_size_map = true,
                      .sharded_backing = ShardedBacking::kNone,
                      .cache_limit_index = 4},
               {Churn{.cpu_index = 0, .size_class_index = 0, .rounds = 16},
                Churn{.cpu_index = 1, .size_class_index = 1, .rounds = 16},
                ResizeSizeClassMaxCapacities{}, ResizeSlab{.mode = 1},
                ResizeSlab{.mode = 2}, Print{}, PrintInPbtxt{}});
}

// Objects cross between the per-CPU slabs and the sharded transfer cache in
// both of its configurations, including while slabs are resized underneath.
TEST(CpuCacheFuzzTest, ShardedBacking) {
  if (!CanFuzz()) {
    GTEST_SKIP() << "rseq unavailable";
  }
  for (const ShardedBacking sharded_backing :
       {ShardedBacking::kLargeClassesOnly, ShardedBacking::kGeneric}) {
    FuzzCpuCache(
        Config{.dynamic_slab_enabled = true,
               .release_drained_slab_metadata = true,
               .use_real_size_map = false,
               .sharded_backing = sharded_backing,
               .cache_limit_index = 2},
        {Allocate{.cpu_index = 0, .size_class_index = 0, .count = 200},
         Churn{.cpu_index = 1, .size_class_index = 0, .rounds = 12},
         Churn{.cpu_index = 2, .size_class_index = 1, .rounds = 6},
         Churn{.cpu_index = 3, .size_class_index = 2, .rounds = 6},
         ResizeSlab{.mode = 1},
         Deallocate{.cpu_index = 2, .size_class_index = 0, .count = 100},
         ShuffleCpuCaches{}, TryDrainingCaches{},
         Churn{.cpu_index = 0, .size_class_index = 0, .rounds = 4},
         Deallocate{.cpu_index = 1, .size_class_index = 0, .count = 50},
         Reclaim{.cpu_index = 1},
         Allocate{.cpu_index = 3, .size_class_index = 0, .count = 100},
         Churn{.cpu_index = 3, .size_class_index = 1, .rounds = 3},
         ResizeSlab{.mode = 2},
         Deallocate{.cpu_index = 0, .size_class_index = 0, .count = 150},
         Churn{.cpu_index = 2, .size_class_index = 0, .rounds = 4},
         ReleaseDrainedMetadata{}, Print{}, PrintInPbtxt{}});
  }
}

TEST(CpuCacheFuzzTest, AllocateStringify) {
  EXPECT_EQ(
      absl::StrFormat(
          "%v", Allocate{.cpu_index = 1, .size_class_index = 2, .count = 3}),
      "Allocate{.cpu_index=1, .size_class_index=2, .count=3}");
}

FUZZ_TEST(CpuCacheFuzzTest, FuzzCpuCache)
    .WithDomains(fuzztest::StructOf<Config>(
                     fuzztest::Arbitrary<bool>(), fuzztest::Arbitrary<bool>(),
                     fuzztest::Arbitrary<bool>(),
                     fuzztest::ElementOf({ShardedBacking::kNone,
                                          ShardedBacking::kLargeClassesOnly,
                                          ShardedBacking::kGeneric}),
                     fuzztest::InRange<uint8_t>(0, kCacheLimits.size() - 1)),
                 fuzztest::Arbitrary<std::vector<Instruction>>());

}  // namespace
}  // namespace tcmalloc::tcmalloc_internal
GOOGLE_MALLOC_SECTION_END
