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

// Runs the page heap's release paths (background release, explicit
// ReleaseMemoryToSystem, usage-limit shrinking, and hugepage treatment)
// concurrently with allocation churn on the real allocator.  Unbacking drops
// pageheap_lock, so this exercises interleavings that the HugePageFiller unit
// tests can only choreograph one at a time.

#include <stddef.h>

#include <atomic>
#include <cstdlib>
#include <memory>
#include <thread>  // NOLINT(build/c++11)
#include <utility>
#include <vector>

#include "gtest/gtest.h"
#include "absl/random/bit_gen_ref.h"
#include "absl/random/random.h"
#include "absl/time/clock.h"
#include "absl/time/time.h"
#include "tcmalloc/malloc_extension.h"
#include "tcmalloc/testing/testutil.h"
#include "tcmalloc/testing/thread_manager.h"

namespace tcmalloc {
namespace {

// Object sizes above kMaxSize are served straight from the page heap, so
// churn in this range lands in the HugePageFiller (and HugeRegion for
// multi-hugepage spans) without passing through the per-CPU or transfer
// caches.
constexpr size_t kMinLarge = 256 << 10;
constexpr size_t kMaxLarge = 8 << 20;

// A bounded pool of large objects owned by one thread.  Every page of an
// object is written with a per-object pattern and verified before the object
// is freed: unbacking memory that is still allocated zeroes it, which shows up
// here as corruption.
class LargeObjectPool {
 public:
  explicit LargeObjectPool(size_t budget) : budget_(budget) {}
  ~LargeObjectPool() {
    for (const Object& o : owned_) {
      Check(o);
      sized_delete(o.ptr, o.size);
    }
  }

  void Step(absl::BitGenRef rng) {
    if (used_ < budget_ && (owned_.empty() || absl::Bernoulli(rng, 0.5))) {
      Object o;
      o.size = absl::LogUniform<size_t>(rng, kMinLarge, kMaxLarge);
      o.ptr = static_cast<char*>(::operator new(o.size));
      o.fill = static_cast<char>(absl::Uniform<int>(rng, 1, 256));
      Fill(o);
      owned_.push_back(o);
      used_ += o.size;
      return;
    }
    if (owned_.empty()) return;
    size_t index = absl::Uniform<size_t>(rng, 0, owned_.size());
    std::swap(owned_[index], owned_.back());
    Object o = owned_.back();
    owned_.pop_back();
    Check(o);
    sized_delete(o.ptr, o.size);
    used_ -= o.size;
  }

 private:
  struct Object {
    char* ptr;
    size_t size;
    char fill;
  };

  static constexpr size_t kStride = 4096;

  static void Fill(const Object& o) {
    for (size_t i = 0; i < o.size; i += kStride) o.ptr[i] = o.fill;
    o.ptr[o.size - 1] = o.fill;
  }

  static void Check(const Object& o) {
    for (size_t i = 0; i < o.size; i += kStride) {
      ASSERT_EQ(o.ptr[i], o.fill) << o.ptr << " size " << o.size << " at " << i;
    }
    ASSERT_EQ(o.ptr[o.size - 1], o.fill);
  }

  const size_t budget_;
  size_t used_ = 0;
  std::vector<Object> owned_;
};

size_t PhysicalMemoryUsed() {
  return *MallocExtension::GetNumericProperty("generic.physical_memory_used");
}

// Churns large objects on kThreads threads for kDuration while: the background
// thread releases memory at a high rate and treats hugepage trackers every few
// milliseconds; one thread calls ReleaseMemoryToSystem in a loop; and one
// thread repeatedly sets a soft limit below the current footprint (forcing
// PageAllocator::ShrinkToUsageLimit on the allocating threads) and clears it
// again.  With hard_limit, a hard limit with headroom for the whole working
// set is also in place, so a hard-limit shrink that comes up short because
// another thread is already releasing aborts the process.
void RunChurn(bool hard_limit) {
  constexpr int kThreads = 8;
  constexpr size_t kBudgetPerThread = 32 << 20;
  constexpr absl::Duration kDuration = absl::Seconds(3);

  ScopedBackgroundProcessSleepInterval sleep_interval(absl::Milliseconds(1));
  ScopedBackgroundReleaseRate release_rate(
      MallocExtension::BytesPerSecond{size_t{1} << 30});
  std::thread background([] { MallocExtension::ProcessBackgroundActions(); });

  std::vector<std::unique_ptr<LargeObjectPool>> pools;
  std::vector<absl::BitGen> rngs(kThreads);
  for (int i = 0; i < kThreads; ++i) {
    pools.push_back(std::make_unique<LargeObjectPool>(kBudgetPerThread));
  }

  if (hard_limit) {
    // The pools hold at most kThreads * kBudgetPerThread bytes of live
    // objects.  Leave room beyond that for hugepage rounding of the spans,
    // metadata, and the test's own small allocations, so that a shrink can
    // always succeed by releasing free pages.
    const size_t limit = PhysicalMemoryUsed() + kThreads * kBudgetPerThread +
                         (kThreads * kMaxLarge) + (256 << 20);
    MallocExtension::SetMemoryLimit(limit, MallocExtension::LimitKind::kHard);
  }

  std::atomic<bool> done{false};
  ThreadManager mgr;
  mgr.Start(kThreads, [&](int id) { pools[id]->Step(rngs[id]); });

  std::thread releaser([&] {
    while (!done.load(std::memory_order_acquire)) {
      MallocExtension::ReleaseMemoryToSystem(4 << 20);
    }
  });
  std::thread limiter([&] {
    while (!done.load(std::memory_order_acquire)) {
      const size_t used = PhysicalMemoryUsed();
      MallocExtension::SetMemoryLimit(used * 3 / 4,
                                      MallocExtension::LimitKind::kSoft);
      absl::SleepFor(absl::Milliseconds(2));
      MallocExtension::SetMemoryLimit(0, MallocExtension::LimitKind::kSoft);
      absl::SleepFor(absl::Milliseconds(2));
    }
  });

  absl::SleepFor(kDuration);

  done.store(true, std::memory_order_release);
  limiter.join();
  releaser.join();
  mgr.Stop();
  {
    ScopedBackgroundProcessActionsEnabled background_enabled(/*value=*/false);
    background.join();
  }

  // Frees every remaining object, verifying its contents.
  pools.clear();
  MallocExtension::SetMemoryLimit(0, MallocExtension::LimitKind::kSoft);
  MallocExtension::SetMemoryLimit(0, MallocExtension::LimitKind::kHard);

  // Exit status indicates whether we've failed any of the expectations above.
  exit(testing::Test::HasFailure());
}

// Each case runs in a subprocess: a hard-limit abort (TC_BUG) must fail the
// test rather than kill it, and limits and background settings must not leak
// between cases.
TEST(ConcurrentReleaseTest, ChurnWithReleaseTreatmentAndSoftLimit) {
  EXPECT_EXIT(RunChurn(/*hard_limit=*/false), testing::ExitedWithCode(0), "");
}

TEST(ConcurrentReleaseTest, ChurnWithReleaseTreatmentAndHardLimit) {
  EXPECT_EXIT(RunChurn(/*hard_limit=*/true), testing::ExitedWithCode(0), "");
}

}  // namespace
}  // namespace tcmalloc
