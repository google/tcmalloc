// Copyright 2020 The TCMalloc Authors
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
#include <array>
#include <memory>
#include <new>
#include <string>
#include <variant>
#include <vector>

#include "fuzztest/fuzztest.h"
#include "absl/log/check.h"
#include "absl/strings/str_format.h"
#include "absl/types/span.h"
#include "tcmalloc/central_freelist.h"
#include "tcmalloc/common.h"
#include "tcmalloc/internal/atomic_stats_counter.h"
#include "tcmalloc/internal/config.h"
#include "tcmalloc/internal/logging.h"
#include "tcmalloc/internal/percpu.h"
#include "tcmalloc/mock_central_freelist.h"
#include "tcmalloc/mock_transfer_cache.h"
#include "tcmalloc/transfer_cache.h"
#include "tcmalloc/transfer_cache_internals.h"
#include "tcmalloc/transfer_cache_stats.h"

GOOGLE_MALLOC_SECTION_BEGIN
namespace tcmalloc::tcmalloc_internal {
namespace {

// Objects moved by a single Insert/Remove instruction: two full batches, so
// that the chunking loops around InsertRange/RemoveRange iterate more than
// once and zero-length requests are exercised.
constexpr int kMaxObjectsPerOp = 2 * kMaxObjectsToMove;

auto ObjectCountDomain() { return fuzztest::InRange(0, kMaxObjectsPerOp); }
auto BatchDomain() { return fuzztest::InRange<int>(1, kMaxObjectsToMove); }

namespace single_size_class {

using TransferCache =
    internal_transfer_cache::TransferCache<MockCentralFreeList,
                                           FakeTransferCacheManager>;
using TransferCacheEnv = FakeTransferCacheEnvironment<TransferCache>;

constexpr int kNumObjectsToMove =
    TransferCache::Forwarder::num_objects_to_move(1);

struct State {
  TransferCacheEnv env;

  State() = default;

  ~State() {
    CheckInvariants();
    Drain();
    CheckInvariants();
    CHECK_EQ(env.transfer_cache().tc_length(), 0);
  }

  void CheckInvariants() {
    const TransferCacheStats stats = env.transfer_cache().GetStats();
    const auto slot_info = env.transfer_cache().GetSlotInfo();
    CHECK_GE(stats.used, 0);
    CHECK_LE(stats.used, stats.capacity);
    CHECK_LE(stats.capacity, stats.max_capacity);
    CHECK_EQ(stats.used, env.transfer_cache().tc_length());
    CHECK_EQ(stats.max_capacity, env.transfer_cache().max_capacity());
    CHECK_EQ(slot_info.used, stats.used);
    CHECK_EQ(slot_info.capacity, stats.capacity);
    CHECK_EQ(env.transfer_cache().HasSpareCapacity(kSizeClass),
             stats.capacity - stats.used >= kNumObjectsToMove);
    CHECK_EQ(env.transfer_cache().CanIncreaseCapacity(kSizeClass),
             stats.capacity + kNumObjectsToMove <= stats.max_capacity);
  }

  void Drain() { env.Drain(); }
};

struct Grow {
  template <typename Sink>
  friend void AbslStringify(Sink& sink, const Grow& g) {
    absl::Format(&sink, "Grow{}");
  }

  void Perform(State& state) const {
    const TransferCacheStats stats = state.env.transfer_cache().GetStats();
    // Confirm that we are always able to grow the cache provided we
    // have sufficient capacity to grow.
    const bool expected =
        stats.capacity + kNumObjectsToMove <= stats.max_capacity;
    CHECK_EQ(state.env.transfer_cache().CanIncreaseCapacity(kSizeClass),
             expected);
    CHECK_EQ(state.env.Grow(), expected);
    CHECK_EQ(state.env.transfer_cache().GetSlotInfo().capacity,
             expected ? stats.capacity + kNumObjectsToMove : stats.capacity);
  }
};

struct Shrink {
  template <typename Sink>
  friend void AbslStringify(Sink& sink, const Shrink& s) {
    absl::Format(&sink, "Shrink{}");
  }

  void Perform(State& state) const {
    const TransferCacheStats stats = state.env.transfer_cache().GetStats();
    // Confirm that we are always able to shrink the cache provided we
    // have sufficient capacity to shrink.
    const bool expected = stats.capacity > kNumObjectsToMove;
    CHECK_EQ(state.env.Shrink(), expected);
    CHECK_EQ(state.env.transfer_cache().GetSlotInfo().capacity,
             expected ? stats.capacity - kNumObjectsToMove : stats.capacity);
  }
};

struct TryPlunder {
  template <typename Sink>
  friend void AbslStringify(Sink& sink, const TryPlunder& t) {
    absl::Format(&sink, "TryPlunder{}");
  }

  void Perform(State& state) const { state.env.TryPlunder(); }
};

struct GetStats {
  template <typename Sink>
  friend void AbslStringify(Sink& sink, const GetStats& g) {
    absl::Format(&sink, "GetStats{}");
  }

  void Perform(State& state) const {
    const TransferCacheStats stats = state.env.transfer_cache().GetStats();
    CHECK_GE(stats.used, 0);
    CHECK_LE(stats.used, stats.capacity);
    CHECK_LE(stats.capacity, stats.max_capacity);
  }
};

struct Insert {
  int batch;

  template <typename Sink>
  friend void AbslStringify(Sink& sink, const Insert& i) {
    absl::Format(&sink, "Insert{.batch=%d}", i.batch);
  }

  void Perform(State& state) const { state.env.Insert(batch); }
};

struct Remove {
  int batch;

  template <typename Sink>
  friend void AbslStringify(Sink& sink, const Remove& r) {
    absl::Format(&sink, "Remove{.batch=%d}", r.batch);
  }

  void Perform(State& state) const { state.env.Remove(batch); }
};

using Instruction =
    std::variant<Grow, Shrink, TryPlunder, GetStats, Insert, Remove>;

void FuzzTransferCache(const std::vector<Instruction>& instructions) {
  State state;
  for (const auto& instruction : instructions) {
    std::visit([&](const auto& instr) { instr.Perform(state); }, instruction);
    state.CheckInvariants();
  }
}

auto GetInstructionDomain() {
  return fuzztest::OneOf(
      fuzztest::Map([](Grow g) { return Instruction{g}; },
                    fuzztest::Arbitrary<Grow>()),
      fuzztest::Map([](Shrink s) { return Instruction{s}; },
                    fuzztest::Arbitrary<Shrink>()),
      fuzztest::Map([](TryPlunder t) { return Instruction{t}; },
                    fuzztest::Arbitrary<TryPlunder>()),
      fuzztest::Map([](GetStats g) { return Instruction{g}; },
                    fuzztest::Arbitrary<GetStats>()),
      fuzztest::Map([](int batch) { return Instruction{Insert{batch}}; },
                    ObjectCountDomain()),
      fuzztest::Map([](int batch) { return Instruction{Remove{batch}}; },
                    ObjectCountDomain()));
}

FUZZ_TEST(TransferCacheTest, FuzzTransferCache)
    .WithDomains(fuzztest::VectorOf(GetInstructionDomain()));

}  // namespace single_size_class

// Size classes exercised by the multi-size-class and sharded fuzzers.  Class 0
// is invalid throughout TCMalloc.  The last entry sits in the cold partition
// when the build has one and is otherwise the highest normal class.
constexpr int kColdSizeClass =
    kHasColdClasses ? kColdClassesStart + 1 : kColdClassesStart - 1;
constexpr std::array<int, 4> kFuzzSizeClasses = {1, 2, 3, kColdSizeClass};
constexpr int kNumFuzzClasses = kFuzzSizeClasses.size();

auto ClassIndexDomain() { return fuzztest::InRange(0, kNumFuzzClasses - 1); }

struct ClassConfig {
  // class_to_size().  Zero leaves the class without a cache.
  size_t size;
  int num_objects_to_move;
};
using ClassConfigs = std::array<ClassConfig, kNumFuzzClasses>;

auto ClassConfigsDomain() {
  return fuzztest::ArrayOf<kNumFuzzClasses>(fuzztest::StructOf<ClassConfig>(
      // Straddle the 4 KiB threshold below which the traditional sharded
      // transfer cache ignores a size class.
      fuzztest::OneOf(fuzztest::Just<size_t>(0),
                      fuzztest::InRange<size_t>(1, 4095),
                      fuzztest::InRange<size_t>(4096, 1 << 20)),
      fuzztest::InRange<int>(kMinObjectsToMove, kMaxObjectsToMove)));
}

// Forwarder whose size-class table and sharded-cache switches are set per
// fuzz iteration.  Every TransferCache embeds its own Forwarder, so the state
// is static and shared.  Alloc() is backed by operator new and everything it
// hands out is released by ReleaseAll() once the caches are gone.
class FuzzForwarder {
 public:
  static void Configure(const ClassConfigs& configs, bool use_generic_cache,
                        bool large_classes_only) {
    std::fill(std::begin(sizes_), std::end(sizes_), 0);
    std::fill(std::begin(num_objects_to_move_), std::end(num_objects_to_move_),
              0);
    for (int i = 0; i < kNumFuzzClasses; ++i) {
      sizes_[kFuzzSizeClasses[i]] = configs[i].size;
      num_objects_to_move_[kFuzzSizeClasses[i]] =
          configs[i].num_objects_to_move;
    }
    use_generic_cache_ = use_generic_cache;
    large_classes_only_ = large_classes_only;
  }

  static size_t class_to_size(int size_class) { return sizes_[size_class]; }
  static size_t num_objects_to_move(int size_class) {
    return num_objects_to_move_[size_class];
  }

  static void Init() {}
  static bool UseGenericCache() { return use_generic_cache_; }
  static bool EnableCacheForLargeClassesOnly() { return large_classes_only_; }

  static void* Alloc(size_t size, std::align_val_t alignment = kAlignment) {
    void* ptr = ::operator new(size, alignment);
    Allocations().push_back({ptr, size, alignment});
    return ptr;
  }

  // REQUIRES: no cache built on this forwarder is alive.
  static void ReleaseAll() {
    for (const Allocation& a : Allocations()) {
      ::operator delete(a.ptr, a.size, a.alignment);
    }
    Allocations().clear();
  }

 private:
  struct Allocation {
    void* ptr;
    size_t size;
    std::align_val_t alignment;
  };

  static std::vector<Allocation>& Allocations() {
    static std::vector<Allocation> allocations;
    return allocations;
  }

  inline static size_t sizes_[kNumClasses] = {};
  inline static size_t num_objects_to_move_[kNumClasses] = {};
  inline static bool use_generic_cache_ = false;
  inline static bool large_classes_only_ = false;
};

// Malloc-backed freelist that remembers its size class, which
// ShardedTransferCacheManagerBase::Plunder() reads back.
class FuzzFreeList : public FakeCentralFreeList {
 public:
  void Init(size_t size_class,
            central_freelist_internal::CflSubbucketPrioritization) {
    size_class_ = size_class;
  }
  int size_class() const { return size_class_; }

 private:
  int size_class_ = -1;
};

using FuzzTransferCache =
    internal_transfer_cache::TransferCache<FuzzFreeList, FuzzForwarder>;

// Single-threaded reference model of one TransferCache: what GetStats() must
// report after each operation, including the low-water mark that TryPlunder()
// consumes.
class CacheModel {
 public:
  CacheModel() = default;
  CacheModel(int batch, size_t capacity, size_t max_capacity) : batch_(batch) {
    stats_.capacity = capacity;
    stats_.max_capacity = max_capacity;
  }

  const TransferCacheStats& stats() const { return stats_; }
  size_t batch() const { return batch_; }
  size_t used() const { return stats_.used; }

  // Returns how many of the n inserted objects the cache keeps; the rest
  // overflow to the freelist.
  size_t Insert(size_t n) {
    const size_t got = std::min(n, stats_.capacity - stats_.used);
    stats_.used += got;
    if (got > 0) ++stats_.insert_hits;
    if (got < n) {
      ++stats_.insert_misses;
      stats_.insert_object_misses += n - got;
    }
    return got;
  }

  // Returns how many objects RemoveRange(n) yields: whatever the cache holds,
  // or a full batch from the freelist once it is empty.
  size_t Remove(size_t n) {
    if (stats_.used == 0) {
      ++stats_.remove_misses;
      stats_.remove_object_misses += n;
      return n;
    }
    const size_t got = std::min(n, stats_.used);
    stats_.used -= got;
    ++stats_.remove_hits;
    stats_.remove_object_hits += got;
    low_water_mark_ = std::min(low_water_mark_, stats_.used);
    return got;
  }

  bool Grow() {
    if (stats_.capacity + batch_ > stats_.max_capacity) return false;
    stats_.capacity += batch_;
    return true;
  }

  bool Shrink() {
    if (stats_.capacity <= batch_) return false;
    const size_t unused = stats_.capacity - stats_.used;
    stats_.capacity -= batch_;
    if (batch_ <= unused) return true;
    stats_.used -= batch_ - unused;
    low_water_mark_ = std::min(low_water_mark_, stats_.used);
    return true;
  }

  void Plunder() {
    if (stats_.max_capacity == 0) return;
    stats_.used -= low_water_mark_;
    low_water_mark_ = stats_.used;
  }

 private:
  size_t batch_ = 0;
  size_t low_water_mark_ = 0;
  TransferCacheStats stats_ = {};
};

void CheckStats(const TransferCacheStats& actual,
                const TransferCacheStats& expected) {
  CHECK_EQ(actual.insert_hits, expected.insert_hits);
  CHECK_EQ(actual.insert_misses, expected.insert_misses);
  CHECK_EQ(actual.insert_object_misses, expected.insert_object_misses);
  CHECK_EQ(actual.remove_hits, expected.remove_hits);
  CHECK_EQ(actual.remove_object_hits, expected.remove_object_hits);
  CHECK_EQ(actual.remove_misses, expected.remove_misses);
  CHECK_EQ(actual.remove_object_misses, expected.remove_object_misses);
  CHECK_EQ(actual.used, expected.used);
  CHECK_EQ(actual.capacity, expected.capacity);
  CHECK_EQ(actual.max_capacity, expected.max_capacity);
}

void Accumulate(TransferCacheStats& into, const TransferCacheStats& from) {
  into.insert_hits += from.insert_hits;
  into.insert_misses += from.insert_misses;
  into.insert_object_misses += from.insert_object_misses;
  into.remove_hits += from.remove_hits;
  into.remove_object_hits += from.remove_object_hits;
  into.remove_misses += from.remove_misses;
  into.remove_object_misses += from.remove_object_misses;
  into.used += from.used;
  into.capacity += from.capacity;
  into.max_capacity += from.max_capacity;
}

namespace multi_size_class {

class State {
 public:
  explicit State(const ClassConfigs& configs) {
    FuzzForwarder::Configure(configs, /*use_generic_cache=*/false,
                             /*large_classes_only=*/false);
    for (int i = 0; i < kNumFuzzClasses; ++i) {
      const int size_class = kFuzzSizeClasses[i];
      const ClassConfig& config = configs[i];
      const size_t batch = config.num_objects_to_move;
      caches_[i] = std::make_unique<FuzzTransferCache>(size_class);
      const TransferCacheStats stats = caches_[i]->GetStats();
      models_[i] = CacheModel(batch, stats.capacity, stats.max_capacity);

      // CapacityNeeded(): whole batches, at most kMaxCapacityInBatches of
      // them, and at most 1 MiB of objects unless a single batch is larger.
      CHECK_EQ(stats.used, 0);
      CHECK_EQ(stats.max_capacity % batch, 0);
      CHECK_LE(stats.max_capacity,
               internal_transfer_cache::kMaxCapacityInBatches * batch);
      CHECK_EQ(
          stats.capacity,
          std::min(internal_transfer_cache::kInitialCapacityInBatches * batch,
                   stats.max_capacity));
      if (config.size == 0) {
        CHECK_EQ(stats.max_capacity, 0);
        continue;
      }
      CHECK_GE(stats.max_capacity, batch);
      CHECK_LE(stats.max_capacity * config.size,
               std::max<size_t>(1 << 20, batch * config.size));
    }
  }

  ~State() {
    CheckInvariants();
    Drain();
    CheckInvariants();
    for (std::unique_ptr<FuzzTransferCache>& cache : caches_) {
      CHECK_EQ(cache->tc_length(), 0);
      cache.reset();
    }
    FuzzForwarder::ReleaseAll();
  }

  void Insert(int index, int n, int batch) {
    const int size_class = kFuzzSizeClasses[index];
    void* bufs[kMaxObjectsToMove];
    while (n > 0) {
      const int b = std::min(n, batch);
      caches_[index]->freelist().AllocateBatch(absl::MakeSpan(bufs, b));
      caches_[index]->InsertRange(size_class, absl::MakeSpan(bufs, b));
      models_[index].Insert(b);
      n -= b;
    }
  }

  void Remove(int index, int n, int batch) {
    const int size_class = kFuzzSizeClasses[index];
    void* bufs[kMaxObjectsToMove];
    while (n > 0) {
      const int b = std::min(n, batch);
      const int removed =
          caches_[index]->RemoveRange(size_class, absl::MakeSpan(bufs, b));
      CHECK_EQ(removed, models_[index].Remove(b));
      CHECK_GT(removed, 0);
      caches_[index]->freelist().FreeBatch(absl::MakeSpan(bufs, removed));
      n -= removed;
    }
  }

  void Grow(int index) {
    CHECK_EQ(caches_[index]->IncreaseCacheCapacity(kFuzzSizeClasses[index]),
             models_[index].Grow());
  }

  void Shrink(int index) {
    CHECK_EQ(caches_[index]->ShrinkCache(kFuzzSizeClasses[index]),
             models_[index].Shrink());
  }

  void TryPlunder(int index) {
    caches_[index]->TryPlunder(kFuzzSizeClasses[index]);
    models_[index].Plunder();
  }

  void CheckInvariants() const {
    for (int i = 0; i < kNumFuzzClasses; ++i) {
      const int size_class = kFuzzSizeClasses[i];
      const FuzzTransferCache& cache = *caches_[i];
      const CacheModel& model = models_[i];
      const TransferCacheStats stats = cache.GetStats();
      CheckStats(stats, model.stats());
      CHECK_EQ(cache.tc_length(), stats.used);
      CHECK_EQ(cache.max_capacity(), stats.max_capacity);
      const auto slot_info = cache.GetSlotInfo();
      CHECK_EQ(slot_info.used, stats.used);
      CHECK_EQ(slot_info.capacity, stats.capacity);
      CHECK_EQ(cache.HasSpareCapacity(size_class),
               stats.capacity - stats.used >= model.batch());
      CHECK_EQ(cache.CanIncreaseCapacity(size_class),
               stats.max_capacity - stats.capacity >= model.batch());
    }
  }

 private:
  void Drain() {
    for (int i = 0; i < kNumFuzzClasses; ++i) {
      Remove(i, models_[i].used(), kMaxObjectsToMove);
    }
  }

  std::array<std::unique_ptr<FuzzTransferCache>, kNumFuzzClasses> caches_;
  std::array<CacheModel, kNumFuzzClasses> models_;
};

struct Insert {
  int index;
  int n;
  int batch;

  template <typename Sink>
  friend void AbslStringify(Sink& sink, const Insert& i) {
    absl::Format(&sink, "Insert{.index=%d, .n=%d, .batch=%d}", i.index, i.n,
                 i.batch);
  }

  void Perform(State& state) const { state.Insert(index, n, batch); }
};

struct Remove {
  int index;
  int n;
  int batch;

  template <typename Sink>
  friend void AbslStringify(Sink& sink, const Remove& r) {
    absl::Format(&sink, "Remove{.index=%d, .n=%d, .batch=%d}", r.index, r.n,
                 r.batch);
  }

  void Perform(State& state) const { state.Remove(index, n, batch); }
};

struct Grow {
  int index;

  template <typename Sink>
  friend void AbslStringify(Sink& sink, const Grow& g) {
    absl::Format(&sink, "Grow{.index=%d}", g.index);
  }

  void Perform(State& state) const { state.Grow(index); }
};

struct Shrink {
  int index;

  template <typename Sink>
  friend void AbslStringify(Sink& sink, const Shrink& s) {
    absl::Format(&sink, "Shrink{.index=%d}", s.index);
  }

  void Perform(State& state) const { state.Shrink(index); }
};

struct TryPlunder {
  int index;

  template <typename Sink>
  friend void AbslStringify(Sink& sink, const TryPlunder& t) {
    absl::Format(&sink, "TryPlunder{.index=%d}", t.index);
  }

  void Perform(State& state) const { state.TryPlunder(index); }
};

using Instruction = std::variant<Insert, Remove, Grow, Shrink, TryPlunder>;

void FuzzMultiSizeClass(const ClassConfigs& configs,
                        const std::vector<Instruction>& instructions) {
  State state(configs);
  for (const auto& instruction : instructions) {
    std::visit([&](const auto& instr) { instr.Perform(state); }, instruction);
    state.CheckInvariants();
  }
}

auto GetInstructionDomain() {
  return fuzztest::OneOf(
      fuzztest::Map(
          [](int index, int n, int batch) {
            return Instruction{Insert{index, n, batch}};
          },
          ClassIndexDomain(), ObjectCountDomain(), BatchDomain()),
      fuzztest::Map(
          [](int index, int n, int batch) {
            return Instruction{Remove{index, n, batch}};
          },
          ClassIndexDomain(), ObjectCountDomain(), BatchDomain()),
      fuzztest::Map([](int index) { return Instruction{Grow{index}}; },
                    ClassIndexDomain()),
      fuzztest::Map([](int index) { return Instruction{Shrink{index}}; },
                    ClassIndexDomain()),
      fuzztest::Map([](int index) { return Instruction{TryPlunder{index}}; },
                    ClassIndexDomain()));
}

FUZZ_TEST(TransferCacheTest, FuzzMultiSizeClass)
    .WithDomains(ClassConfigsDomain(),
                 fuzztest::VectorOf(GetInstructionDomain()));

TEST(TransferCacheTest, FuzzMultiSizeClassSmoke) {
  FuzzMultiSizeClass(
      {{{8, 32}, {16 << 10, 2}, {0, 4}, {4096, 8}}},
      {Insert{0, 40, 32}, Insert{1, 3, 2}, Insert{2, 5, 5}, Insert{3, 300, 128},
       Shrink{0}, Grow{1}, TryPlunder{0}, Remove{0, 10, 8}, Remove{3, 200, 64},
       TryPlunder{0}, TryPlunder{3}, Shrink{2}, Grow{2}, Remove{2, 1, 1}});
}

}  // namespace multi_size_class

namespace sharded {

using ShardedManager =
    ShardedTransferCacheManagerBase<FuzzForwarder, FakeCpuLayout, FuzzFreeList>;

constexpr int kMaxShards =
    FakeCpuLayout::kNumCpus / FakeCpuLayout::kCpusPerShard;

struct Config {
  int num_shards;
  bool use_generic_cache;
  bool large_classes_only;
  ClassConfigs classes;
};

class State {
 public:
  explicit State(const Config& config)
      : num_shards_(config.num_shards), percpu_fast_(subtle::percpu::IsFast()) {
    FuzzForwarder::Configure(config.classes, config.use_generic_cache,
                             config.large_classes_only);
    cpu_layout_.Init(num_shards_);
    manager_ = std::make_unique<ShardedManager>(&cpu_layout_);
    manager_->Init();

    // Mirror Init() and InitShard(): which classes get a sharded cache and
    // its geometry, identical across shards.
    const bool use_sharded_cache =
        config.large_classes_only ||
        (config.use_generic_cache &&
         num_shards_ >= ShardedManager::kMinShardsAllowed);
    const size_t min_size = config.use_generic_cache ? 0 : 4096;
    for (int i = 0; i < kNumFuzzClasses; ++i) {
      const int size_class = kFuzzSizeClasses[i];
      const ClassConfig& c = config.classes[i];
      const bool should_use = use_sharded_cache && c.size >= min_size;
      CHECK_EQ(manager_->should_use(size_class), should_use);
      size_t capacity = 0;
      size_t max_capacity = 0;
      if (should_use && config.use_generic_cache) {
        const FuzzTransferCache::Capacity needed =
            FuzzTransferCache::CapacityNeeded(size_class);
        capacity = needed.capacity;
        max_capacity = needed.max_capacity;
      } else if (should_use) {
        // The traditional sharded cache holds up to 12 MiB per size class.
        capacity = max_capacity = (12 << 20) / c.size;
      }
      for (int shard = 0; shard < num_shards_; ++shard) {
        models_[shard][i] =
            CacheModel(c.num_objects_to_move, capacity, max_capacity);
      }
    }
  }

  ~State() {
    CheckInvariants();
    Drain();
    CheckInvariants();
    CHECK_EQ(manager_->TotalBytes(), 0);
    manager_.reset();
    FuzzForwarder::ReleaseAll();
  }

  void SwitchCpu(int cpu) { cpu_layout_.SetCurrentCpu(cpu); }

  void Insert(int index, int n, int batch) {
    // Nothing reaches the cache, so the shard stays uninitialized.
    if (n == 0) return;
    const int size_class = kFuzzSizeClasses[index];
    CacheModel& model = Touch(index);
    void* bufs[kMaxObjectsToMove];
    while (n > 0) {
      const int b = std::min(n, batch);
      freelist_.AllocateBatch(absl::MakeSpan(bufs, b));
      manager_->InsertRange(size_class, absl::MakeSpan(bufs, b));
      model.Insert(b);
      n -= b;
    }
  }

  void Remove(int index, int n, int batch) {
    if (n == 0) return;
    const int size_class = kFuzzSizeClasses[index];
    CacheModel& model = Touch(index);
    void* bufs[kMaxObjectsToMove];
    while (n > 0) {
      const int b = std::min(n, batch);
      const int removed =
          manager_->RemoveRange(size_class, absl::MakeSpan(bufs, b));
      CHECK_EQ(removed, model.Remove(b));
      CHECK_GT(removed, 0);
      freelist_.FreeBatch(absl::MakeSpan(bufs, removed));
      n -= removed;
    }
  }

  // Push()/Pop() assert a registered rseq; fall back to the range API with a
  // single object where per-CPU mode is unavailable.
  void Push(int index) {
    const int size_class = kFuzzSizeClasses[index];
    CacheModel& model = Touch(index);
    void* ptr;
    freelist_.AllocateBatch(absl::MakeSpan(&ptr, 1));
    if (percpu_fast_) {
      manager_->Push(size_class, ptr);
    } else {
      manager_->InsertRange(size_class, absl::MakeSpan(&ptr, 1));
    }
    model.Insert(1);
  }

  void Pop(int index) {
    const int size_class = kFuzzSizeClasses[index];
    CacheModel& model = Touch(index);
    void* ptr;
    if (percpu_fast_) {
      ptr = manager_->Pop(size_class);
    } else {
      CHECK_EQ(manager_->RemoveRange(size_class, absl::MakeSpan(&ptr, 1)), 1);
    }
    CHECK_NE(ptr, nullptr);
    CHECK_EQ(model.Remove(1), 1);
    freelist_.FreeBatch(absl::MakeSpan(&ptr, 1));
  }

  void Plunder() {
    manager_->Plunder();
    for (int shard = 0; shard < num_shards_; ++shard) {
      if (!initialized_[shard]) continue;
      for (CacheModel& model : models_[shard]) {
        model.Plunder();
      }
    }
  }

  void Print() {
    StatsCounters<kNumClasses> counts;
    for (int i = 0; i < kNumFuzzClasses; ++i) {
      counts[kFuzzSizeClasses[i]].Add(1 + i);
    }
    std::string buffer(1 << 18, '\0');
    Printer printer(buffer.data(), buffer.size());
    manager_->Print(counts, printer);
    PbtxtRegion region(printer, kTop);
    manager_->PrintInPbtxt(counts, region);
    CHECK_LE(printer.SpaceRequired(), buffer.size());
  }

  void CheckInvariants() {
    int active_shards = 0;
    for (int shard = 0; shard < num_shards_; ++shard) {
      CHECK_EQ(manager_->shard_initialized(shard), initialized_[shard]);
      active_shards += initialized_[shard];
    }
    CHECK_EQ(manager_->NumActiveShards(), active_shards);

    size_t total_bytes = 0;
    for (int i = 0; i < kNumFuzzClasses; ++i) {
      const int size_class = kFuzzSizeClasses[i];
      TransferCacheStats expected = {};
      for (int shard = 0; shard < num_shards_; ++shard) {
        if (!initialized_[shard]) continue;
        Accumulate(expected, models_[shard][i].stats());
      }
      CheckStats(manager_->GetStats(size_class), expected);
      CHECK_EQ(manager_->TotalObjectsOfClass(size_class), expected.used);
      total_bytes += expected.used * FuzzForwarder::class_to_size(size_class);
      for (int cpu = 0; cpu < FakeCpuLayout::kNumCpus; ++cpu) {
        const int shard = cpu_layout_.CpuShard(cpu);
        const size_t used = initialized_[shard] ? models_[shard][i].used() : 0;
        CHECK_EQ(manager_->tc_length(cpu, size_class), used);
      }
    }
    CHECK_EQ(manager_->TotalBytes(), total_bytes);
  }

 private:
  int CurrentShard() { return cpu_layout_.CpuShard(cpu_layout_.CurrentCpu()); }

  // Any cache access initializes the current CPU's shard.
  CacheModel& Touch(int index) {
    const int shard = CurrentShard();
    initialized_[shard] = true;
    return models_[shard][index];
  }

  void Drain() {
    for (int shard = 0; shard < num_shards_; ++shard) {
      if (!initialized_[shard]) continue;
      SwitchCpu(shard * FakeCpuLayout::kCpusPerShard);
      CHECK_EQ(CurrentShard(), shard);
      for (int i = 0; i < kNumFuzzClasses; ++i) {
        Remove(i, models_[shard][i].used(), kMaxObjectsToMove);
      }
    }
  }

  const int num_shards_;
  const bool percpu_fast_;
  FakeCentralFreeList freelist_;
  FakeCpuLayout cpu_layout_;
  std::unique_ptr<ShardedManager> manager_;
  std::array<std::array<CacheModel, kNumFuzzClasses>, kMaxShards> models_;
  std::array<bool, kMaxShards> initialized_ = {};
};

struct SwitchCpu {
  int cpu;

  template <typename Sink>
  friend void AbslStringify(Sink& sink, const SwitchCpu& s) {
    absl::Format(&sink, "SwitchCpu{.cpu=%d}", s.cpu);
  }

  void Perform(State& state) const { state.SwitchCpu(cpu); }
};

struct Insert {
  int index;
  int n;
  int batch;

  template <typename Sink>
  friend void AbslStringify(Sink& sink, const Insert& i) {
    absl::Format(&sink, "Insert{.index=%d, .n=%d, .batch=%d}", i.index, i.n,
                 i.batch);
  }

  void Perform(State& state) const { state.Insert(index, n, batch); }
};

struct Remove {
  int index;
  int n;
  int batch;

  template <typename Sink>
  friend void AbslStringify(Sink& sink, const Remove& r) {
    absl::Format(&sink, "Remove{.index=%d, .n=%d, .batch=%d}", r.index, r.n,
                 r.batch);
  }

  void Perform(State& state) const { state.Remove(index, n, batch); }
};

struct Push {
  int index;

  template <typename Sink>
  friend void AbslStringify(Sink& sink, const Push& p) {
    absl::Format(&sink, "Push{.index=%d}", p.index);
  }

  void Perform(State& state) const { state.Push(index); }
};

struct Pop {
  int index;

  template <typename Sink>
  friend void AbslStringify(Sink& sink, const Pop& p) {
    absl::Format(&sink, "Pop{.index=%d}", p.index);
  }

  void Perform(State& state) const { state.Pop(index); }
};

struct Plunder {
  template <typename Sink>
  friend void AbslStringify(Sink& sink, const Plunder&) {
    absl::Format(&sink, "Plunder{}");
  }

  void Perform(State& state) const { state.Plunder(); }
};

struct Print {
  template <typename Sink>
  friend void AbslStringify(Sink& sink, const Print&) {
    absl::Format(&sink, "Print{}");
  }

  void Perform(State& state) const { state.Print(); }
};

using Instruction =
    std::variant<SwitchCpu, Insert, Remove, Push, Pop, Plunder, Print>;

void FuzzSharded(const Config& config,
                 const std::vector<Instruction>& instructions) {
  State state(config);
  for (const auto& instruction : instructions) {
    std::visit([&](const auto& instr) { instr.Perform(state); }, instruction);
    state.CheckInvariants();
  }
}

auto GetConfigDomain() {
  return fuzztest::StructOf<Config>(
      fuzztest::InRange(1, kMaxShards), fuzztest::Arbitrary<bool>(),
      fuzztest::Arbitrary<bool>(), ClassConfigsDomain());
}

auto GetInstructionDomain() {
  return fuzztest::OneOf(
      fuzztest::Map([](int cpu) { return Instruction{SwitchCpu{cpu}}; },
                    fuzztest::InRange(0, FakeCpuLayout::kNumCpus - 1)),
      fuzztest::Map(
          [](int index, int n, int batch) {
            return Instruction{Insert{index, n, batch}};
          },
          ClassIndexDomain(), ObjectCountDomain(), BatchDomain()),
      fuzztest::Map(
          [](int index, int n, int batch) {
            return Instruction{Remove{index, n, batch}};
          },
          ClassIndexDomain(), ObjectCountDomain(), BatchDomain()),
      fuzztest::Map([](int index) { return Instruction{Push{index}}; },
                    ClassIndexDomain()),
      fuzztest::Map([](int index) { return Instruction{Pop{index}}; },
                    ClassIndexDomain()),
      fuzztest::Map([](Plunder p) { return Instruction{p}; },
                    fuzztest::Arbitrary<Plunder>()),
      fuzztest::Map([](Print p) { return Instruction{p}; },
                    fuzztest::Arbitrary<Print>()));
}

FUZZ_TEST(TransferCacheTest, FuzzSharded)
    .WithDomains(GetConfigDomain(), fuzztest::VectorOf(GetInstructionDomain()));

TEST(TransferCacheTest, FuzzShardedGenericSmoke) {
  FuzzSharded(Config{.num_shards = 3,
                     .use_generic_cache = true,
                     .large_classes_only = false,
                     .classes = {{{8, 32}, {4096, 4}, {0, 2}, {64 << 10, 2}}}},
              {SwitchCpu{0}, Insert{0, 40, 32}, Push{1}, SwitchCpu{5}, Push{1},
               Pop{1}, Pop{3}, Insert{2, 3, 3}, Plunder{}, Print{},
               SwitchCpu{1}, Remove{0, 10, 8}, Plunder{}, Plunder{}, Print{}});
}

TEST(TransferCacheTest, FuzzShardedLargeClassesOnlySmoke) {
  FuzzSharded(
      Config{.num_shards = 2,
             .use_generic_cache = false,
             .large_classes_only = true,
             .classes = {{{8, 32}, {4095, 4}, {4096, 2}, {1 << 20, 2}}}},
      {SwitchCpu{3}, Insert{0, 5, 5}, Insert{1, 5, 5}, Insert{2, 256, 128},
       Insert{3, 20, 3}, Print{}, Plunder{}, Remove{2, 100, 128}, Plunder{},
       SwitchCpu{0}, Pop{2}, Push{3}, Plunder{}});
}

TEST(TransferCacheTest, FuzzShardedInactiveSmoke) {
  // Fewer than kMinShardsAllowed cache domains disable the generic cache.
  FuzzSharded(Config{.num_shards = 2,
                     .use_generic_cache = true,
                     .large_classes_only = false,
                     .classes = {{{8, 32}, {4096, 4}, {0, 2}, {64 << 10, 2}}}},
              {Insert{1, 0, 55}, Remove{0, 0, 1}, Insert{0, 40, 32}, Push{1},
               Pop{1}, Pop{3}, Plunder{}, Print{}});
}

}  // namespace sharded
}  // namespace
}  // namespace tcmalloc::tcmalloc_internal
GOOGLE_MALLOC_SECTION_END
