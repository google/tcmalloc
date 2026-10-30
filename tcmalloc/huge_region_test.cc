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

#include "tcmalloc/huge_region.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <functional>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "absl/base/thread_annotations.h"
#include "absl/container/fixed_array.h"
#include "absl/random/random.h"
#include "absl/strings/string_view.h"
#include "absl/synchronization/mutex.h"
#include "absl/time/clock.h"
#include "absl/time/time.h"
#include "absl/types/span.h"
#include "tcmalloc/common.h"
#include "tcmalloc/huge_cache.h"
#include "tcmalloc/huge_pages.h"
#include "tcmalloc/internal/clock.h"
#include "tcmalloc/internal/logging.h"
#include "tcmalloc/internal/memory_tag.h"
#include "tcmalloc/internal/system_allocator.h"
#include "tcmalloc/pages.h"
#include "tcmalloc/stats.h"
#include "tcmalloc/testing/thread_manager.h"

namespace tcmalloc {
namespace tcmalloc_internal {
namespace {

using testing::NiceMock;
using testing::Return;

class MockMemoryTagFunction : public MemoryTagFunction {
 public:
  MOCK_METHOD(void, Call, (Range r, std::optional<absl::string_view> name));
  void operator()(Range r, std::optional<absl::string_view> name) override {
    Call(r, name);
  }
};

void* MakeTaggedAddress(MemoryTag tag) {
  return reinterpret_cast<void*>(uintptr_t{static_cast<uint8_t>(tag)}
                                 << kTagShift);
}

class HugeRegionTest : public ::testing::Test {
 protected:
  HugeRegionTest()
      : mock_(std::make_unique<NiceMock<MockBackingInterface>>()),
        // an unlikely magic page
        p_(HugePageContaining(MakeTaggedAddress(MemoryTag::kNormal))),
        region_({p_, region_.size()}, *mock_, mock_set_anon_vma_name_) {
    // we usually don't care about backing calls, unless testing that
    // specifically.
  }

  ~HugeRegionTest() override { mock_.reset(nullptr); }

  class MockBackingInterface : public MemoryModifyFunction {
   public:
    MOCK_METHOD(MemoryModifyStatus, Unback, (PageId p, Length len), ());

    MemoryModifyStatus operator()(Range r) override { return Unback(r.p, r.n); }
  };

  std::unique_ptr<MockBackingInterface> mock_;
  MockMemoryTagFunction mock_set_anon_vma_name_;

  void CheckMock() { testing::Mock::VerifyAndClearExpectations(mock_.get()); }

  void ExpectUnback(HugeRange r, bool success = true) {
    EXPECT_CALL(*mock_, Unback(r.start().first_page(), r.len().in_pages()))
        .WillOnce(
            Return(MemoryModifyStatus{.success = success, .error_number = 0}));
  }

  struct Alloc {
    PageId p;
    Length n;
    size_t mark;
  };

  HugePage p_;
  typedef HugeRegion Region;
  Region region_;
  size_t next_mark_{0};
  size_t marks_[Region::size().in_pages().raw_num()];

  void Mark(Alloc a) {
    EXPECT_LE(p_.first_page(), a.p);
    size_t index = (a.p - p_.first_page()).raw_num();
    size_t end = index + a.n.raw_num();
    EXPECT_LE(end, region_.size().in_pages().raw_num());
    for (; index < end; ++index) {
      marks_[index] = a.mark;
    }
  }

  void Check(Alloc a) {
    EXPECT_LE(p_.first_page(), a.p);
    size_t index = (a.p - p_.first_page()).raw_num();
    size_t end = index + a.n.raw_num();
    EXPECT_LE(end, region_.size().in_pages().raw_num());
    for (; index < end; ++index) {
      EXPECT_EQ(a.mark, marks_[index]);
    }
  }

  Alloc Allocate(Length n) {
    bool from_released;
    return Allocate(n, &from_released);
  }

  Alloc Allocate(Length n, bool* from_released) {
    Alloc ret;
    TC_CHECK(region_.MaybeGet(n, &ret.p, from_released));
    ret.n = n;
    ret.mark = ++next_mark_;
    Mark(ret);
    return ret;
  }

  void Delete(Alloc a) {
    Check(a);
    region_.Put(Range(a.p, a.n), false, [] {});
  }

  void DeleteUnback(Alloc a) {
    Check(a);
    region_.Put(Range(a.p, a.n), true, [] {});
  }

  // Rebuilds region_ over the same range, unbacking through `unback`.
  void ResetRegion(MemoryModifyFunction& unback) {
    region_.~HugeRegion();
    new (&region_)
        HugeRegion({p_, region_.size()}, unback, mock_set_anon_vma_name_);
  }

  // Release as HugePageAwareAllocator calls it: with pageheap_lock held, so
  // that an UnbackWithoutLock unbacker has a lock to drop.
  HugeLength ReleaseUnderLock(Length n, HugeRegionAdaptiveRelease adaptive) {
    PageHeapSpinLockHolder l;
    return region_.Release(n, adaptive, [] {});
  }
};

TEST_F(HugeRegionTest, Basic) {
  Length total;
  std::vector<Alloc> allocs;
  for (Length n(1); total + n < region_.size().in_pages(); ++n) {
    allocs.push_back(Allocate(n));
    total += n;
    EXPECT_EQ(total, region_.used_pages());
  }

  // Free every other alloc
  std::vector<Length> lengths;
  std::vector<Alloc> new_allocs;
  for (int j = 0; j < allocs.size(); ++j) {
    if (j % 2 == 0) {
      new_allocs.push_back(allocs[j]);
      continue;
    }
    Length n = allocs[j].n;
    Delete(allocs[j]);
    total -= n;
    EXPECT_EQ(total, region_.used_pages());
    lengths.push_back(n);
  }
  allocs.swap(new_allocs);
  // and reallocate them in a random order:
  std::shuffle(lengths.begin(), lengths.end(), absl::BitGen());
  // This should fit, since thge allocator is best-fit
  // and we have unique gaps of each size.
  for (auto n : lengths) {
    allocs.push_back(Allocate(n));
    total += n;
    EXPECT_EQ(total, region_.used_pages());
  }

  for (auto a : allocs) {
    Delete(a);
  }
}

TEST_F(HugeRegionTest, ReqsBacking) {
  const Length n = kPagesPerHugePage;
  std::vector<Alloc> allocs;
  // should back the first page
  bool from_released;
  allocs.push_back(Allocate(n - Length(1), &from_released));
  EXPECT_TRUE(from_released);
  // nothing
  allocs.push_back(Allocate(Length(1), &from_released));
  EXPECT_FALSE(from_released);
  // second page
  allocs.push_back(Allocate(Length(1), &from_released));
  EXPECT_TRUE(from_released);
  // third, fourth, fifth
  allocs.push_back(Allocate(3 * n, &from_released));
  EXPECT_TRUE(from_released);

  for (auto a : allocs) {
    Delete(a);
  }
}

TEST_F(HugeRegionTest, ReleaseFrac) {
  const Length n = kPagesPerHugePage;
  bool from_released;
  auto a = Allocate(n * 20, &from_released);
  EXPECT_TRUE(from_released);

  Delete(a);
  ExpectUnback({p_ + NHugePages(0), NHugePages(2)});
  EXPECT_EQ(NHugePages(2),
            region_.Release(NHugePages(2).in_pages(),
                            HugeRegionAdaptiveRelease::kDisabled, [] {}));
  CheckMock();

  ExpectUnback({p_ + NHugePages(2), NHugePages(1)});
  EXPECT_EQ(NHugePages(1),
            region_.Release(NHugePages(1).in_pages(),
                            HugeRegionAdaptiveRelease::kDisabled, [] {}));
  CheckMock();

  ExpectUnback({p_ + NHugePages(3), NHugePages(8)});
  EXPECT_EQ(NHugePages(8),
            region_.Release(NHugePages(8).in_pages(),
                            HugeRegionAdaptiveRelease::kDisabled, [] {}));
  CheckMock();

  ExpectUnback({p_ + NHugePages(11), NHugePages(9)});
  EXPECT_EQ(NHugePages(9),
            region_.Release(NHugePages(9).in_pages(),
                            HugeRegionAdaptiveRelease::kDisabled, [] {}));
  CheckMock();
}

TEST_F(HugeRegionTest, Release) {
  const Length n = kPagesPerHugePage;
  bool from_released;
  auto a = Allocate(n * 4 - Length(1), &from_released);
  EXPECT_TRUE(from_released);

  auto b = Allocate(n * 3, &from_released);
  EXPECT_TRUE(from_released);

  auto c = Allocate(n * 5 + Length(1), &from_released);
  EXPECT_TRUE(from_released);

  auto d = Allocate(n * 2, &from_released);
  EXPECT_TRUE(from_released);

  auto e = Allocate(n / 2, &from_released);
  EXPECT_TRUE(from_released);
  auto f = Allocate(n / 2, &from_released);
  EXPECT_FALSE(from_released);

  // Don't unback the first or last hugepage this touches -- since they
  // overlap with others.
  Delete(b);
  ExpectUnback({p_ + NHugePages(4), NHugePages(2)});
  EXPECT_EQ(NHugePages(2),
            region_.Release(NHugePages(2).in_pages(),
                            HugeRegionAdaptiveRelease::kDisabled, [] {}));
  CheckMock();

  // Now we're on exact boundaries so we should unback the whole range.
  Delete(d);
  ExpectUnback({p_ + NHugePages(12), NHugePages(2)});
  EXPECT_EQ(NHugePages(2),
            region_.Release(NHugePages(2).in_pages(),
                            HugeRegionAdaptiveRelease::kDisabled, [] {}));
  CheckMock();

  Delete(a);
  ExpectUnback({p_ + NHugePages(0), NHugePages(4)});
  EXPECT_EQ(NHugePages(4),
            region_.Release(NHugePages(4).in_pages(),
                            HugeRegionAdaptiveRelease::kDisabled, [] {}));
  CheckMock();

  // Should work just as well with aggressive Put():
  ExpectUnback({p_ + NHugePages(6), NHugePages(6)});
  DeleteUnback(c);
  CheckMock();

  // And this _shouldn't_ do anything (page still in use)
  DeleteUnback(e);
  // But this should:
  ExpectUnback({p_ + NHugePages(14), NHugePages(1)});
  DeleteUnback(f);
  CheckMock();
}

TEST_F(HugeRegionTest, ReleaseAdaptive) {
  const Length n = kPagesPerHugePage;
  bool from_released;
  auto a = Allocate(n * 4, &from_released);
  EXPECT_TRUE(from_released);

  Delete(a);
  // Now we have 4 backed, free hugepages: 0, 1, 2, 3 relative to p_.

  // Release 2 hugepages with adaptive_release = true.
  // It should release 3 and 2.
  ExpectUnback({p_ + NHugePages(2), NHugePages(2)});
  EXPECT_EQ(NHugePages(2),
            region_.Release(NHugePages(2).in_pages(),
                            HugeRegionAdaptiveRelease::kEnabled, [] {}));
  CheckMock();
}

// Verify that adaptive release (reverse order) correctly traverses across
// fragmented, disjoint ranges of free backed hugepages separated by active
// allocations, and correctly computes the sub-range offset when only a partial
// range is needed.
TEST_F(HugeRegionTest, ReleaseAdaptiveFragmented) {
  const Length n = kPagesPerHugePage;
  bool from_released;
  std::optional<Alloc> allocs[8];
  for (int i = 0; i < 8; ++i) {
    allocs[i] = Allocate(n, &from_released);
    EXPECT_TRUE(from_released);
  }

  // Delete hugepages: 0, 1, 3, 4, 6, 7 while keeping 2 and 5 allocated.
  Delete(*allocs[0]);
  Delete(*allocs[1]);
  Delete(*allocs[3]);
  Delete(*allocs[4]);
  Delete(*allocs[6]);
  Delete(*allocs[7]);

  // Free backed hugepages are: [0, 1], [3, 4], [6, 7].
  // Reverse order should release: 7, 6 from the last range, then 4 from the
  // middle range.
  ExpectUnback({p_ + NHugePages(4), NHugePages(1)});
  ExpectUnback({p_ + NHugePages(6), NHugePages(2)});
  EXPECT_EQ(NHugePages(3),
            region_.Release(NHugePages(3).in_pages(),
                            HugeRegionAdaptiveRelease::kEnabled, [] {}));
  CheckMock();

  Delete(*allocs[2]);
  Delete(*allocs[5]);
}

// Verify that non-adaptive release (forward order) correctly traverses across
// fragmented, disjoint ranges of free backed hugepages separated by active
// allocations.
TEST_F(HugeRegionTest, ReleaseNonAdaptiveFragmented) {
  const Length n = kPagesPerHugePage;
  bool from_released;
  std::optional<Alloc> allocs[8];
  for (int i = 0; i < 8; ++i) {
    allocs[i] = Allocate(n, &from_released);
    EXPECT_TRUE(from_released);
  }

  // Delete hugepages: 0, 1, 3, 4, 6, 7 while keeping 2 and 5 allocated.
  Delete(*allocs[0]);
  Delete(*allocs[1]);
  Delete(*allocs[3]);
  Delete(*allocs[4]);
  Delete(*allocs[6]);
  Delete(*allocs[7]);

  // Free backed hugepages are: [0, 1], [3, 4], [6, 7].
  // Forward order should release: 0, 1 from the first range, then 3 from the
  // middle range.
  ExpectUnback({p_, NHugePages(2)});
  ExpectUnback({p_ + NHugePages(3), NHugePages(1)});
  EXPECT_EQ(NHugePages(3),
            region_.Release(NHugePages(3).in_pages(),
                            HugeRegionAdaptiveRelease::kDisabled, [] {}));
  CheckMock();

  Delete(*allocs[2]);
  Delete(*allocs[5]);
}

TEST_F(HugeRegionTest, ReleaseFailure) {
  const Length n = kPagesPerHugePage;
  bool from_released;
  auto a = Allocate(n * 4 - Length(1), &from_released);
  EXPECT_TRUE(from_released);
  EXPECT_EQ(NHugePages(4), region_.backed());

  // Don't unback the first or last hugepage this touches -- since they
  // overlap with others.
  Delete(a);
  ExpectUnback({p_, NHugePages(4)}, false);
  EXPECT_EQ(NHugePages(0),
            region_.Release(NHugePages(4).in_pages(),
                            HugeRegionAdaptiveRelease::kDisabled, [] {}));
  EXPECT_EQ(NHugePages(4), region_.backed());
  CheckMock();

  // Reallocate.
  a = Allocate(n * 4 - Length(1), &from_released);
  EXPECT_FALSE(from_released);
  Delete(a);

  EXPECT_EQ(NHugePages(4), region_.backed());
}

TEST_F(HugeRegionTest, Reback) {
  const Length n = kPagesPerHugePage / 4;
  bool from_released;
  // Even in back/unback cycles we should still call the functions
  // on every transition.
  for (int i = 0; i < 20; ++i) {
    std::vector<Alloc> allocs;
    allocs.push_back(Allocate(n, &from_released));
    EXPECT_TRUE(from_released);
    allocs.push_back(Allocate(n, &from_released));
    EXPECT_FALSE(from_released);
    allocs.push_back(Allocate(n, &from_released));
    EXPECT_FALSE(from_released);
    allocs.push_back(Allocate(n, &from_released));
    EXPECT_FALSE(from_released);

    std::shuffle(allocs.begin(), allocs.end(), absl::BitGen());
    DeleteUnback(allocs[0]);
    DeleteUnback(allocs[1]);
    DeleteUnback(allocs[2]);

    ExpectUnback({p_, NHugePages(1)});
    DeleteUnback(allocs[3]);
    CheckMock();
  }
}

class MemorySimulation final : public MemoryModifyFunction {
 public:
  MemorySimulation(absl::Mutex& mu, PageId base,
                   absl::Span<std::atomic<int8_t>> bytes)
      : mu_(mu), base_(base), bytes_(bytes) {}

  MemoryModifyStatus operator()(Range r) override
      ABSL_NO_THREAD_SAFETY_ANALYSIS {
    mu_.AssertHeld();
    // Simulate UnbackHugepages dropping pageheap_lock around the unback
    // syscall (b/73749855).  Mark each page kUnbacking while unlocked: if
    // another thread allocates or unbacks an overlapping range before we
    // finish, its exchange or ours will see a non-zero value and fail.
    mu_.Unlock();

    const size_t index = (r.p - base_).raw_num();
    const size_t n = r.n.raw_num();
    for (size_t i = 0; i < n; ++i) {
      const int old =
          bytes_[index + i].exchange(kUnbacking, std::memory_order_acq_rel);
      TC_CHECK_EQ(old, 0);
    }
    absl::SleepFor(absl::Microseconds(10));
    for (size_t i = 0; i < n; ++i) {
      const int old = bytes_[index + i].exchange(0, std::memory_order_acq_rel);
      TC_CHECK_EQ(old, static_cast<int>(kUnbacking));
    }

    mu_.Lock();
    return {.success = success_.load(std::memory_order_relaxed),
            .error_number = 0};
  }

  // Whether subsequent unbacks report success.  May be changed concurrently
  // with an unback in flight.
  void set_success(bool success) {
    success_.store(success, std::memory_order_relaxed);
  }

 private:
  static constexpr int8_t kUnbacking = -1;

  absl::Mutex& mu_;
  PageId base_;
  absl::Span<std::atomic<int8_t>> bytes_;
  std::atomic<bool> success_{true};
};

TEST_F(HugeRegionTest, ReleaseFuzz) {
  absl::Mutex mu;
  absl::FixedArray<std::atomic<int8_t>> bytes(
      region_.size().in_pages().raw_num());
  for (auto& b : bytes) {
    b.store(0, std::memory_order_relaxed);
  }

  MemorySimulation simulation(mu, p_.first_page(), absl::MakeSpan(bytes));

  region_.~HugeRegion();
  new (&region_)
      HugeRegion({p_, region_.size()}, simulation, mock_set_anon_vma_name_);

  const int kThreads = 10;
  std::vector<absl::BitGen> rngs(kThreads);

  absl::Mutex state_mu;
  struct FuzzAlloc {
    int owner;
    Range r;
  };
  std::vector<FuzzAlloc> allocs;

  ThreadManager threads;
  threads.Start(kThreads, [&](int tid) {
    // 0 means free and -1 means kUnbacking, so thread IDs are shifted by 1.
    const int owner = tid + 1;
    switch (absl::Uniform(rngs[tid], 0, 5)) {
      case 0: {
        const size_t n =
            absl::Uniform(rngs[tid], 1u, region_.size().in_pages().raw_num());

        FuzzAlloc f;
        f.owner = owner;
        f.r.n = Length(n);
        bool from_released;
        {
          absl::MutexLock l(mu);
          if (!region_.MaybeGet(f.r.n, &f.r.p, &from_released)) {
            break;
          }
        }

        const size_t base = (f.r.p - p_.first_page()).raw_num();
        for (size_t i = 0; i < n; ++i) {
          const int old_val = bytes[base + i].exchange(
              static_cast<int8_t>(owner), std::memory_order_acq_rel);
          TC_CHECK_EQ(old_val, 0);
        }

        {
          absl::MutexLock l(state_mu);
          allocs.push_back(f);
        }
        break;
      }
      case 1: {
        FuzzAlloc f;

        {
          absl::MutexLock l(state_mu);
          if (allocs.empty()) {
            break;
          }

          const size_t index = absl::Uniform(rngs[tid], 0u, allocs.size());
          f = allocs[index];
          std::swap(allocs[index], allocs.back());
          allocs.resize(allocs.size() - 1);
        }

        const size_t base = (f.r.p - p_.first_page()).raw_num();
        for (size_t i = 0; i < f.r.n.raw_num(); ++i) {
          const int old_val =
              bytes[base + i].exchange(0, std::memory_order_acq_rel);
          TC_CHECK_EQ(old_val, f.owner);
        }

        const bool release = absl::Bernoulli(rngs[tid], 0.5);
        absl::MutexLock l(mu);
        region_.Put(f.r, release, [] {});
        break;
      }
      case 2: {
        absl::MutexLock l(state_mu);
        if (allocs.empty()) {
          break;
        }

        const size_t index = absl::Uniform(rngs[tid], 0u, allocs.size());
        FuzzAlloc f = allocs[index];

        const size_t base = (f.r.p - p_.first_page()).raw_num();
        for (size_t i = 0; i < f.r.n.raw_num(); ++i) {
          const int val = bytes[base + i].load(std::memory_order_acquire);
          TC_CHECK_EQ(val, f.owner);
        }

        break;
      }
      case 3: {
        const Length to_release = Length(
            absl::Uniform(rngs[tid], 0u, region_.size().in_pages().raw_num()));
        const HugeRegionAdaptiveRelease adaptive =
            absl::Bernoulli(rngs[tid], 0.5)
                ? HugeRegionAdaptiveRelease::kEnabled
                : HugeRegionAdaptiveRelease::kDisabled;

        absl::MutexLock l(mu);
        (void)region_.Release(to_release, adaptive, [] {});

        break;
      }
      case 4: {
        // Fail some unbacks, so the rollback path also races with other
        // threads.
        simulation.set_success(absl::Bernoulli(rngs[tid], 0.75));
        break;
      }
    }
  });

  absl::SleepFor(absl::Milliseconds(100));

  threads.Stop();
  simulation.set_success(true);

  for (const FuzzAlloc& f : allocs) {
    const size_t base = (f.r.p - p_.first_page()).raw_num();
    for (size_t i = 0; i < f.r.n.raw_num(); ++i) {
      const int old_val =
          bytes[base + i].exchange(0, std::memory_order_acq_rel);
      TC_CHECK_EQ(old_val, f.owner);
    }
    absl::MutexLock l(mu);
    region_.Put(f.r, /*release=*/false, [] {});
  }
  while (region_.free_backed() > NHugePages(0)) {
    absl::MutexLock l(mu);
    (void)region_.Release(region_.size().in_pages(),
                          HugeRegionAdaptiveRelease::kDisabled, [] {});
  }
  EXPECT_EQ(region_.used_pages(), Length(0));
  EXPECT_EQ(region_.free_backed(), NHugePages(0));
  EXPECT_EQ(region_.backed(), NHugePages(0));
  for (const auto& b : bytes) {
    EXPECT_EQ(b.load(std::memory_order_relaxed), 0);
  }
}

class CallbackRelease final : public MemoryModifyFunction {
 public:
  explicit CallbackRelease(std::function<void()> callback)
      : callback_(callback) {}

  [[nodiscard]] MemoryModifyStatus operator()(Range r) override {
    callback_();
    regions_.emplace_back(r);
    return {true, 0};
  }

  std::vector<Range> regions() const { return regions_; }

 private:
  std::function<void()> callback_;
  std::vector<Range> regions_;
};

TEST_F(HugeRegionTest, ReleaseDuringPut) {
  HugeLength released;
  int callback_count = 0;
  CallbackRelease blocking([&]() {
    // Simulate a Release from another thread while we are unbacking from put.
    callback_count++;
    if (callback_count != 1) {
      return;
    }

    released = region_.Release(region_.size().in_pages(),
                               HugeRegionAdaptiveRelease::kDisabled, [] {});
  });
  region_.~HugeRegion();
  new (&region_)
      HugeRegion({p_, region_.size()}, blocking, mock_set_anon_vma_name_);

  auto a1 = Allocate(5 * kPagesPerHugePage / 2);
  EXPECT_EQ(region_.backed(), NHugePages(3));
  EXPECT_EQ(region_.free_backed(), NHugePages(0));

  auto a2 = Allocate(5 * kPagesPerHugePage / 2);
  EXPECT_EQ(region_.backed(), NHugePages(5));
  EXPECT_EQ(region_.free_backed(), NHugePages(0));

  auto a3 = Allocate(3 * kPagesPerHugePage);
  EXPECT_EQ(region_.backed(), NHugePages(8));
  EXPECT_EQ(region_.free_backed(), NHugePages(0));

  // Give some free pages for Release() to find.
  Delete(a1);

  EXPECT_EQ(callback_count, 0);
  EXPECT_EQ(region_.backed(), NHugePages(8));
  EXPECT_EQ(region_.free_backed(), NHugePages(2));

  // Delete(a2), but unback it triggering callback.
  Check(a2);
  region_.Put(Range(a2.p, a2.n), true, [] {});

  EXPECT_EQ(region_.backed(), NHugePages(3));
  EXPECT_EQ(region_.free_backed(), NHugePages(0));

  // We expect to have gotten a1 rounded down (2).
  EXPECT_EQ(callback_count, 2);
  EXPECT_EQ(released, NHugePages(2));

  // Check for intersection.
  auto regions = blocking.regions();
  for (const auto& region : regions) {
    EXPECT_TRUE(a3.p + a3.n <= region.p || region.p + region.n <= a3.p);
  }

  Delete(a3);
}

TEST_F(HugeRegionTest, Stats) {
  const Length kLen = region_.size().in_pages();
  const size_t kBytes = kLen.in_bytes();
  struct Helper {
    static void Stat(const Region& region, std::vector<Length>* small_backed,
                     std::vector<Length>* small_unbacked, LargeSpanStats* large,
                     BackingStats* stats) {
      SmallSpanStats small;
      *large = LargeSpanStats();
      region.AddSpanStats(&small, large);
      small_backed->clear();
      small_unbacked->clear();
      for (auto i = Length(0); i < kMaxPages; ++i) {
        for (int j = 0; j < small.normal_length[i.raw_num()]; ++j) {
          small_backed->push_back(i);
        }

        for (int j = 0; j < small.returned_length[i.raw_num()]; ++j) {
          small_unbacked->push_back(i);
        }
      }

      *stats = region.stats();
    }
  };

  LargeSpanStats large;
  std::vector<Length> small_backed, small_unbacked;
  BackingStats stats;

  Helper::Stat(region_, &small_backed, &small_unbacked, &large, &stats);
  EXPECT_THAT(small_backed, testing::ElementsAre());
  EXPECT_THAT(small_unbacked, testing::ElementsAre());
  EXPECT_EQ(1, large.spans);
  EXPECT_EQ(Length(0), large.normal_pages);
  EXPECT_EQ(kLen, large.returned_pages);
  EXPECT_EQ(kBytes, stats.system_bytes);
  EXPECT_EQ(0, stats.free_bytes);
  EXPECT_EQ(kBytes, stats.unmapped_bytes);

  // We don't, in production, use small allocations from the region, but
  // the API supports it, so test it here.
  Alloc a = Allocate(Length(1));
  Allocate(Length(1));
  Alloc b = Allocate(Length(2));
  Alloc barrier = Allocate(Length(1));
  Alloc c = Allocate(Length(3));
  Allocate(Length(1));
  const Length slack = kPagesPerHugePage - Length(9);

  Helper::Stat(region_, &small_backed, &small_unbacked, &large, &stats);
  EXPECT_THAT(small_backed, testing::ElementsAre());
  EXPECT_THAT(small_unbacked, testing::ElementsAre());
  EXPECT_EQ(2, large.spans);
  EXPECT_EQ(slack, large.normal_pages);
  EXPECT_EQ(kLen - kPagesPerHugePage, large.returned_pages);
  EXPECT_EQ(kBytes, stats.system_bytes);
  EXPECT_EQ(slack.in_bytes(), stats.free_bytes);
  EXPECT_EQ((region_.size() - NHugePages(1)).in_bytes(), stats.unmapped_bytes);

  Delete(a);
  Helper::Stat(region_, &small_backed, &small_unbacked, &large, &stats);
  EXPECT_THAT(small_backed, testing::ElementsAre(Length(1)));
  EXPECT_THAT(small_unbacked, testing::ElementsAre());
  EXPECT_EQ(2, large.spans);
  EXPECT_EQ(slack, large.normal_pages);
  EXPECT_EQ(kLen - kPagesPerHugePage, large.returned_pages);
  EXPECT_EQ(kBytes, stats.system_bytes);
  EXPECT_EQ((slack + Length(1)).in_bytes(), stats.free_bytes);
  EXPECT_EQ((region_.size() - NHugePages(1)).in_bytes(), stats.unmapped_bytes);

  Delete(b);
  Helper::Stat(region_, &small_backed, &small_unbacked, &large, &stats);
  EXPECT_THAT(small_backed, testing::ElementsAre(Length(1), Length(2)));
  EXPECT_THAT(small_unbacked, testing::ElementsAre());
  EXPECT_EQ(2, large.spans);
  EXPECT_EQ(slack, large.normal_pages);
  EXPECT_EQ(kLen - kPagesPerHugePage, large.returned_pages);
  EXPECT_EQ(kBytes, stats.system_bytes);
  EXPECT_EQ((slack + Length(3)).in_bytes(), stats.free_bytes);
  EXPECT_EQ((region_.size() - NHugePages(1)).in_bytes(), stats.unmapped_bytes);

  Delete(c);
  Helper::Stat(region_, &small_backed, &small_unbacked, &large, &stats);
  EXPECT_THAT(small_backed,
              testing::ElementsAre(Length(1), Length(2), Length(3)));
  EXPECT_THAT(small_unbacked, testing::ElementsAre());
  EXPECT_EQ(2, large.spans);
  EXPECT_EQ(slack, large.normal_pages);
  EXPECT_EQ(kLen - kPagesPerHugePage, large.returned_pages);
  EXPECT_EQ(kBytes, stats.system_bytes);
  EXPECT_EQ((slack + Length(6)).in_bytes(), stats.free_bytes);
  EXPECT_EQ((region_.size() - NHugePages(1)).in_bytes(), stats.unmapped_bytes);

  Delete(barrier);
  Helper::Stat(region_, &small_backed, &small_unbacked, &large, &stats);
  EXPECT_THAT(small_backed, testing::ElementsAre(Length(1), Length(6)));
  EXPECT_THAT(small_unbacked, testing::ElementsAre());
  EXPECT_EQ(2, large.spans);
  EXPECT_EQ(slack, large.normal_pages);
  EXPECT_EQ(kLen - kPagesPerHugePage, large.returned_pages);
  EXPECT_EQ(kBytes, stats.system_bytes);
  EXPECT_EQ((slack + Length(7)).in_bytes(), stats.free_bytes);
  EXPECT_EQ((region_.size() - NHugePages(1)).in_bytes(), stats.unmapped_bytes);
}

// Test that free regions are broken down properly when they cross
// page boundaries that change the backed/unbacked state.
TEST_F(HugeRegionTest, StatBreakdown) {
  const Length n = kPagesPerHugePage;
  Alloc a = Allocate(n / 4);
  Alloc b = Allocate(n * 3 + n / 3);
  Alloc c = Allocate((n - n / 3 - n / 4) + n * 5 + n / 5);
  Alloc d = Allocate(n - (n / 5) - Length(1));
  // This unbacks the middle 2 hugepages, but not the beginning or
  // trailing region
  ExpectUnback(
      HugeRange::Make(HugePageContaining(b.p) + NHugePages(1), NHugePages(2)));
  DeleteUnback(b);
  Delete(c);
  SmallSpanStats small;
  LargeSpanStats large;
  region_.AddSpanStats(&small, &large);
  // Backed beginning of hugepage 0, unbacked range in middle of b,
  // long backed range from c, unbacked tail of allocation.
  EXPECT_EQ(4, large.spans);
  // Tail end of A's page, B/C combined page + all of C.
  EXPECT_EQ((n - n / 4) + n * 6 + (n / 5), large.normal_pages);
  // The above fill up 10 total pages.
  EXPECT_EQ(2 * n + (Region::size().raw_num() - 10) * n, large.returned_pages);
  EXPECT_EQ(1, small.normal_length[1]);

  EXPECT_EQ(Length(1) + large.normal_pages + large.returned_pages +
                region_.used_pages(),
            Region::size().in_pages());
  Delete(a);
  Delete(d);
}

TEST_F(HugeRegionTest, StatBreakdownReleaseFailure) {
  const Length n = kPagesPerHugePage;
  Alloc a = Allocate(n / 4);
  Alloc b = Allocate(n * 3 + n / 3);
  Alloc c = Allocate((n - n / 3 - n / 4) + n * 5 + n / 5);
  Alloc d = Allocate(n - (n / 5) - Length(1));
  // This tries to unback the middle 2 hugepages, but not the beginning or
  // trailing region, but fails.
  ExpectUnback(
      HugeRange::Make(HugePageContaining(b.p) + NHugePages(1), NHugePages(2)),
      /*success=*/false);
  DeleteUnback(b);
  Delete(c);
  SmallSpanStats small;
  LargeSpanStats large;
  region_.AddSpanStats(&small, &large);
  // Backed beginning of hugepage A/B/C/D and the unbacked tail of allocation.
  EXPECT_EQ(2, large.spans);
  // Tail end of A's page, all of B, all of C.
  EXPECT_EQ((n - n / 4) + n * 8 + (n / 5), large.normal_pages);
  // The above fill up 10 total pages.
  EXPECT_EQ((Region::size().raw_num() - 10) * n, large.returned_pages);
  EXPECT_EQ(1, small.normal_length[1]);

  EXPECT_EQ(Length(1) + large.normal_pages + large.returned_pages +
                region_.used_pages(),
            Region::size().in_pages());
  Delete(a);
  Delete(d);
}

TEST_F(HugeRegionTest, GetPageAllocationStatus) {
  PageBitmap pages;

  // HugePage outside the region returns false.
  HugePage outside_hp = p_ - NHugePages(1);
  EXPECT_FALSE(region_.GetPageAllocationStatus(outside_hp, pages));

  // Before any allocations, first hugepage is unbacked.
  // GetPageAllocationStatus returns true, but pages bitmap is empty (all 0s).
  EXPECT_TRUE(region_.GetPageAllocationStatus(p_, pages));
  EXPECT_EQ(pages.CountBits(0, kPagesPerHugePage.raw_num()), 0);

  // Allocate 10 pages in the first hugepage.
  Alloc a1 = Allocate(Length(10));
  EXPECT_EQ(a1.p, p_.first_page());

  EXPECT_TRUE(region_.GetPageAllocationStatus(p_, pages));
  EXPECT_EQ(pages.CountBits(0, kPagesPerHugePage.raw_num()), 10);
  for (size_t i = 0; i < 10; ++i) {
    EXPECT_TRUE(pages.GetBit(i));
  }

  // Deallocate a1.
  Delete(a1);

  // After deallocation without unbacking, hugepage is backed but all pages are
  // free.
  EXPECT_TRUE(region_.GetPageAllocationStatus(p_, pages));
  EXPECT_EQ(pages.CountBits(0, kPagesPerHugePage.raw_num()), 0);
}

TEST_F(HugeRegionTest, ReleaseDuringRelease) {
  HugeLength released_during_callback;
  int callback_count = 0;

  class LocalMemoryTagFunction final : public MemoryTagFunction {
   public:
    void operator()(Range r, std::optional<absl::string_view> name) override {}
  };
  LocalMemoryTagFunction tag;

  CallbackRelease blocking([&]() {
    callback_count++;
    if (callback_count != 1) {
      return;
    }

    // Simulate a Release from another thread while we are unbacking from the
    // first Release. This concurrent Release will try to release all pages in
    // the region. It should skip HP 0 (since it is currently unbacking and
    // thus marked busy). It should find HP 2 is free and backed, and
    // successfully unback it.
    released_during_callback = region_.Release(
        region_.size().in_pages(), HugeRegionAdaptiveRelease::kDisabled, [] {});
  });

  region_.~HugeRegion();
  new (&region_) HugeRegion({p_, region_.size()}, blocking, tag);

  const Length hp = kPagesPerHugePage;

  // Setup: HP 0, 2 are free; HP 1 is allocated.
  bool from_released;
  auto a0 = Allocate(hp, &from_released);
  auto a1 = Allocate(hp, &from_released);
  auto a2 = Allocate(hp, &from_released);

  // Make HP 0 and 2 free (but backed)
  Delete(a0);
  Delete(a2);

  EXPECT_EQ(callback_count, 0);

  // Start the first Release. It will try to release HP 0 and 2.
  // It finds HP 0 first, starts unbacking it, which triggers the 'blocking'
  // callback.
  HugeLength total_released =
      region_.Release(hp * 2, HugeRegionAdaptiveRelease::kDisabled, [] {});

  // We expect that:
  // 1. The concurrent Release (inside callback) successfully unbacked HP 2
  //    (1 hugepage).
  EXPECT_EQ(released_during_callback, NHugePages(1));
  // 2. The original Release successfully unbacked HP 0 (1 hugepage).
  //    And it safely skipped HP 2 because it was already unbacked by the
  //    callback.
  EXPECT_EQ(total_released, NHugePages(1));

  EXPECT_EQ(callback_count, 2);  // 1 for HP 0 unback, 1 for HP 2 unback

  std::vector<Range> unbacked = blocking.regions();
  ASSERT_EQ(unbacked.size(), 2);
  // The inner release (HP 2) completes and records its range first.
  EXPECT_EQ(unbacked[0].p, p_.first_page() + kPagesPerHugePage * 2);
  EXPECT_EQ(unbacked[0].n, kPagesPerHugePage);
  // The outer release (HP 0) completes and records its range second.
  EXPECT_EQ(unbacked[1].p, p_.first_page());
  EXPECT_EQ(unbacked[1].n, kPagesPerHugePage);

  // Clean up
  Delete(a1);
}

// Counts the unbacks it completes; fails them all if !success_.
class CountingUnback final : public MemoryModifyFunction {
 public:
  [[nodiscard]] MemoryModifyStatus operator()(Range r) override {
    if (success_) {
      ++unbacked_;
    }
    return {.success = success_, .error_number = 0};
  }

  int unbacked_ = 0;
  bool success_ = true;
};

// Drops pageheap_lock around the unback.  Tests may install a hook that runs
// while the lock is dropped, standing in for another thread that takes
// pageheap_lock while UnbackHugepages is mid-unback (b/73749855).
class UnbackWithoutLock final : public MemoryModifyFunction {
 public:
  explicit UnbackWithoutLock(
      MemoryModifyFunction& unback ABSL_ATTRIBUTE_LIFETIME_BOUND)
      : unback_(unback) {}

  [[nodiscard]] MemoryModifyStatus operator()(Range r) override
      ABSL_NO_THREAD_SAFETY_ANALYSIS {
    pageheap_lock.AssertHeld();
    pageheap_lock.unlock();
    if (unlocked_hook_ != nullptr) {
      unlocked_hook_(r);
    }
    MemoryModifyStatus ret = unback_(r);
    pageheap_lock.lock();
    return ret;
  }

  // Runs with pageheap_lock dropped.  The hook takes the lock itself before
  // touching the region, as the thread it stands in for would.  A hook that
  // unbacks through this same object is re-entered.
  std::function<void(Range)> unlocked_hook_;

 private:
  MemoryModifyFunction& unback_;
};

// region_ unbacks through UnbackWithoutLock, as HugeRegion::Release will once
// it drops pageheap_lock around the syscall (b/73749855).
//
// region_ (a base member) refers to the unbackers below, which are destroyed
// first; that is safe because HugeRegion's destructor never unbacks.
class HugeRegionUnbackWithoutLockTest : public HugeRegionTest {
 protected:
  HugeRegionUnbackWithoutLockTest() { Reset(); }

  // Rebuilds region_ and clears the unbackers' state, for tests that loop.
  void Reset() {
    unback_.unbacked_ = 0;
    unback_.success_ = true;
    unback_without_lock_.unlocked_hook_ = nullptr;
    ResetRegion(unback_without_lock_);
  }

  CountingUnback unback_;
  UnbackWithoutLock unback_without_lock_{unback_};
};

// An allocation that runs while UnbackHugepages is unbacking a range must not
// receive any page of that range, even when it is the best-fit free span, and
// the range must look allocated to stats and GetPageAllocationStatus until the
// unback finishes.
TEST_F(HugeRegionUnbackWithoutLockTest, GetDuringUnbackAvoidsRangeInFlight) {
  const Length hp = kPagesPerHugePage;
  Alloc a0 = Allocate(2 * hp);
  Alloc a1 = Allocate(hp);
  // Free hugepages 0-1 (best fit for a 2-hugepage request); hugepages 3+ are
  // free and unbacked.
  Delete(a0);
  ASSERT_EQ(region_.backed(), NHugePages(3));
  ASSERT_EQ(region_.free_backed(), NHugePages(2));

  Alloc concurrent;
  bool concurrent_from_released = false;
  unback_without_lock_.unlocked_hook_ = [&](Range r) {
    EXPECT_EQ(r.p, p_.first_page());
    EXPECT_EQ(r.n, 2 * hp);

    // Taking pageheap_lock here shows the release dropped it: this thread
    // would otherwise deadlock.
    PageHeapSpinLockHolder l;
    EXPECT_EQ(region_.free_backed(), NHugePages(0));
    EXPECT_EQ(region_.used_pages(), 3 * hp);

    for (HugeLength offset; offset < NHugePages(2); ++offset) {
      PageBitmap pages;
      ASSERT_TRUE(region_.GetPageAllocationStatus(p_ + offset, pages));
      EXPECT_EQ(pages.CountBits(0, hp.raw_num()), hp.raw_num());
    }
    SmallSpanStats small;
    LargeSpanStats large;
    region_.AddSpanStats(&small, &large);
    EXPECT_EQ(large.normal_pages, Length(0));

    concurrent = Allocate(2 * hp, &concurrent_from_released);
  };
  EXPECT_EQ(ReleaseUnderLock(2 * hp, HugeRegionAdaptiveRelease::kDisabled),
            NHugePages(2));
  unback_without_lock_.unlocked_hook_ = nullptr;

  // Hugepages 0-1 were in flight, so the concurrent Get landed past a1 on
  // hugepages 3-4.
  EXPECT_EQ(concurrent.p, p_.first_page() + 3 * hp);
  EXPECT_TRUE(concurrent_from_released);
  EXPECT_EQ(region_.backed(), NHugePages(3));
  EXPECT_EQ(region_.free_backed(), NHugePages(0));
  EXPECT_EQ(region_.used_pages(), 3 * hp);

  Delete(concurrent);
  Delete(a1);
}

// Release selects all candidate hugepages before unbacking the first range.
// If a concurrent Get allocates even one page of a later candidate while the
// first unback has dropped the lock, UnbackHugepages must recheck CanUnback
// and skip it.
TEST_F(HugeRegionUnbackWithoutLockTest, GetDuringUnbackTakesPendingCandidate) {
  const Length hp = kPagesPerHugePage;
  for (HugeRegionAdaptiveRelease adaptive :
       {HugeRegionAdaptiveRelease::kDisabled,
        HugeRegionAdaptiveRelease::kEnabled}) {
    SCOPED_TRACE(testing::Message()
                 << "adaptive=" << static_cast<bool>(adaptive));
    Reset();

    // Hugepages 0 and 2 are free and backed; 1 and 3 are allocated so that 0
    // and 2 are separate unback calls and hugepage 2 is the best-fit 1-hugepage
    // gap for a 1-page allocation after hugepage 0 is marked in flight.
    Alloc a0 = Allocate(hp);
    Alloc a1 = Allocate(hp);
    Alloc a2 = Allocate(hp);
    Alloc a3 = Allocate(hp);
    Delete(a0);
    Delete(a2);

    Alloc taken;
    bool from_released = true;
    int hooks = 0;
    unback_without_lock_.unlocked_hook_ = [&](Range r) {
      ++hooks;
      EXPECT_EQ(r.p, p_.first_page());
      EXPECT_EQ(r.n, hp);
      // Take 1 page of hugepage 2, which Release already put in should_unback.
      PageHeapSpinLockHolder l;
      taken = Allocate(Length(1), &from_released);
    };
    const HugeLength released = ReleaseUnderLock(2 * hp, adaptive);
    unback_without_lock_.unlocked_hook_ = nullptr;

    EXPECT_EQ(hooks, 1);
    EXPECT_EQ(released, NHugePages(1));
    EXPECT_EQ(unback_.unbacked_, 1);
    EXPECT_EQ(taken.p, p_.first_page() + 2 * hp);
    EXPECT_FALSE(from_released);
    EXPECT_EQ(region_.backed(), NHugePages(3));
    EXPECT_EQ(region_.free_backed(), NHugePages(0));

    Delete(taken);
    EXPECT_EQ(region_.free_backed(), NHugePages(1));
    Delete(a1);
    Delete(a3);
  }
}

// A Put that runs while Release is unbacking another range updates the freed
// hugepage's state without disturbing the in-flight unback.
TEST_F(HugeRegionUnbackWithoutLockTest, PutDuringUnback) {
  const Length hp = kPagesPerHugePage;
  for (bool release_on_put : {false, true}) {
    SCOPED_TRACE(testing::Message() << "release_on_put=" << release_on_put);
    Reset();

    Alloc a0 = Allocate(hp);
    Alloc a1 = Allocate(hp);
    Alloc a2 = Allocate(hp);
    Delete(a0);

    // With release_on_put, the Put unbacks a2's hugepage through the same
    // unbacker, re-entering the hook; only the outer call does the Put.
    int hooks = 0;
    unback_without_lock_.unlocked_hook_ = [&](Range r) {
      if (hooks++ > 0) {
        return;
      }
      EXPECT_EQ(r.p, p_.first_page());
      PageHeapSpinLockHolder l;
      if (release_on_put) {
        DeleteUnback(a2);
      } else {
        Delete(a2);
      }
      EXPECT_EQ(region_.free_backed(),
                release_on_put ? NHugePages(0) : NHugePages(1));
    };
    EXPECT_EQ(ReleaseUnderLock(hp, HugeRegionAdaptiveRelease::kDisabled),
              NHugePages(1));
    unback_without_lock_.unlocked_hook_ = nullptr;

    EXPECT_EQ(hooks, release_on_put ? 2 : 1);
    EXPECT_EQ(unback_.unbacked_, release_on_put ? 2 : 1);
    EXPECT_EQ(region_.backed(), release_on_put ? NHugePages(1) : NHugePages(2));
    EXPECT_EQ(region_.free_backed(),
              release_on_put ? NHugePages(0) : NHugePages(1));
    EXPECT_EQ(region_.used_pages(), hp);

    Delete(a1);
  }
}

// When an unback fails after another thread allocated from the region during
// the lock drop, the rollback restores the failed range to the free-and-backed
// pool alongside the concurrent changes, and the range can be allocated and
// released again.
TEST_F(HugeRegionUnbackWithoutLockTest, FailedUnbackAfterConcurrentGet) {
  unback_.success_ = false;

  const Length hp = kPagesPerHugePage;
  Alloc a0 = Allocate(2 * hp);
  Alloc a1 = Allocate(hp);
  Alloc a2 = Allocate(hp);
  // Hugepages 0-1 and 3 are free and backed; 2 is allocated.
  Delete(a0);
  Delete(a2);
  ASSERT_EQ(region_.backed(), NHugePages(4));
  ASSERT_EQ(region_.free_backed(), NHugePages(3));

  Alloc concurrent;
  bool from_released = true;
  unback_without_lock_.unlocked_hook_ = [&](Range r) {
    EXPECT_EQ(r.p, p_.first_page());
    EXPECT_EQ(r.n, 2 * hp);
    PageHeapSpinLockHolder l;
    EXPECT_EQ(region_.free_backed(), NHugePages(1));
    // Consumes hugepage 3 while hugepages 0-1 are in flight.
    concurrent = Allocate(hp, &from_released);
    EXPECT_EQ(concurrent.p, p_.first_page() + 3 * hp);
    EXPECT_FALSE(from_released);
    EXPECT_EQ(region_.free_backed(), NHugePages(0));
  };
  EXPECT_EQ(ReleaseUnderLock(2 * hp, HugeRegionAdaptiveRelease::kDisabled),
            NHugePages(0));
  unback_without_lock_.unlocked_hook_ = nullptr;

  // Hugepages 0-1 rolled back to free-and-backed; hugepage 3 stays allocated.
  EXPECT_EQ(region_.backed(), NHugePages(4));
  EXPECT_EQ(region_.free_backed(), NHugePages(2));
  EXPECT_EQ(region_.used_pages(), 2 * hp);

  // The rolled-back range is unmarked in tracker_ and still backed.
  bool reget_from_released = true;
  Alloc reget = Allocate(2 * hp, &reget_from_released);
  EXPECT_EQ(reget.p, p_.first_page());
  EXPECT_FALSE(reget_from_released);
  Delete(reget);

  // And it is still eligible for a subsequent Release.
  unback_.success_ = true;
  EXPECT_EQ(ReleaseUnderLock(2 * hp, HugeRegionAdaptiveRelease::kEnabled),
            NHugePages(2));
  EXPECT_EQ(region_.backed(), NHugePages(2));
  EXPECT_EQ(region_.free_backed(), NHugePages(0));

  Delete(concurrent);
  Delete(a1);
}

class NilUnback final : public MemoryModifyFunction {
 public:
  MemoryModifyStatus operator()(Range r) override {
    return {.success = true, .error_number = 0};
  }
};

class NilMemoryTagFunction final : public MemoryTagFunction {
 public:
  void operator()(Range r, std::optional<absl::string_view> name) override {}
};

class HugeRegionSetTest
    : public ::testing::TestWithParam<HugeRegionUsageOption> {
 protected:
  typedef HugeRegion Region;

  HugeRegionSetTest()
      : set_(/*use_huge_region_more_often=*/GetParam(),
             ResetAndMakeFakeClock()) {
    next_ = HugePageContaining(nullptr);
  }

  static constexpr double kFakeClockFreq = 1000.0;  // 1 tick == 1 ms.
  static int64_t fake_clock_ticks_;
  static int64_t FakeClockNow() { return fake_clock_ticks_; }
  static double FakeClockFreq() { return kFakeClockFreq; }
  static Clock ResetAndMakeFakeClock() {
    fake_clock_ticks_ = 0;
    return Clock{.now = FakeClockNow, .freq = FakeClockFreq};
  }

  void AdvanceClock(absl::Duration d) {
    fake_clock_ticks_ += absl::ToInt64Milliseconds(d);
  }

  std::string PrintToString() const {
    std::string buf(2 << 20, '\0');
    Printer out(&buf[0], buf.size());
    set_.Print(out);
    TC_CHECK_LE(out.SpaceRequired(), buf.size());
    buf.resize(strlen(buf.c_str()));
    return buf;
  }

  std::string PrintInPbtxtToString() const {
    std::string buf(2 << 20, '\0');
    Printer out(&buf[0], buf.size());
    {
      PbtxtRegion region(out, kTop);
      set_.PrintInPbtxt(region);
    }
    TC_CHECK_LE(out.SpaceRequired(), buf.size());
    buf.resize(out.SpaceRequired());
    return buf;
  }

  [[nodiscard]] std::unique_ptr<Region> GetRegion() {
    return GetRegion(nil_unback_);
  }

  [[nodiscard]] std::unique_ptr<Region> GetRegion(
      MemoryModifyFunction& unback) {
    // These regions are backed by "real" memory, but we don't touch it.
    auto r = std::make_unique<Region>(HugeRange{next_, Region::size()}, unback,
                                      nil_set_anon_vma_name_);
    next_ += Region::size();
    return r;
  }

  // Rebuilds set_ and clears the unbackers' state, for tests that loop
  // (HugeRegionSet has no way to remove regions once contributed).
  void Reset() {
    unback_.unbacked_ = 0;
    unback_.success_ = true;
    unback_without_lock_.unlocked_hook_ = nullptr;
    set_.~HugeRegionSet();
    new (&set_) HugeRegionSet<Region>(/*use_huge_region_more_often=*/GetParam(),
                                      ResetAndMakeFakeClock());
  }

  [[nodiscard]] static Range Allocate(Region& region, Length n) {
    PageId p;
    bool from_released;
    TC_CHECK(region.MaybeGet(n, &p, &from_released));
    return Range(p, n);
  }

  // set_'s count should match its regions', plus any in-flight unback it has
  // not yet been told about.
  void ExpectFreeBacked(absl::Span<const Region* const> regions,
                        HugeLength in_flight) const {
    HugeLength sum = in_flight;
    for (const Region* r : regions) {
      sum += r->free_backed();
    }
    EXPECT_EQ(set_.free_backed(), sum);
  }

  // Releases up to `desired` pages from `set_` with hit_limit=true, holding
  // pageheap_lock as HugePageAwareAllocator does.
  [[nodiscard]] Length ReleaseAll(Length desired,
                                  HugeRegionAdaptiveRelease adaptive) {
    PageHeapSpinLockHolder l;
    return set_.ReleasePages(desired, adaptive, /*hit_limit=*/true);
  }

  // MaybePut may unback (unless UseHugeRegionMoreOften), so it too runs with
  // pageheap_lock held.
  void PutUnderLock(absl::Span<const Range> ranges) {
    PageHeapSpinLockHolder l;
    for (Range r : ranges) {
      EXPECT_TRUE(set_.MaybePut(r));
    }
  }

  bool UseHugeRegionMoreOften() const { return set_.UseHugeRegionMoreOften(); }

  NilUnback nil_unback_;
  CountingUnback unback_;
  UnbackWithoutLock unback_without_lock_{unback_};
  NilMemoryTagFunction nil_set_anon_vma_name_;
  HugeRegionSet<Region> set_;
  HugePage next_;

  struct Alloc {
    PageId p;
    Length n;
  };
};

int64_t HugeRegionSetTest::fake_clock_ticks_ = 0;

TEST_P(HugeRegionSetTest, Release) {
  absl::BitGen rng;
  PageId p;
  constexpr Length kSize = kPagesPerHugePage;
  bool from_released;
  ASSERT_FALSE(set_.MaybeGet(Length(1), &p, &from_released));
  auto r1 = GetRegion();
  set_.Contribute(r1.get());

  std::vector<Alloc> allocs;

  while (set_.MaybeGet(kSize, &p, &from_released)) {
    allocs.push_back({p, kSize});
  }
  BackingStats stats = set_.stats();
  EXPECT_EQ(stats.unmapped_bytes, 0);

  for (auto a : allocs) {
    ASSERT_TRUE(set_.MaybePut(Range(a.p, a.n)));
  }

  stats = set_.stats();
  EXPECT_EQ(stats.unmapped_bytes,
            UseHugeRegionMoreOften() ? 0 : stats.system_bytes);
  // All the huge pages in the region would be free, but backed, when
  // huge-region-more-often feature is enabled.
  EXPECT_EQ(r1->free_backed(),
            UseHugeRegionMoreOften() ? Region::size() : NHugePages(0));
  Length released =
      set_.ReleasePages(Length::max(), HugeRegionAdaptiveRelease::kDisabled,
                        /*hit_limit=*/true);
  stats = set_.stats();
  EXPECT_EQ(released.in_bytes(),
            UseHugeRegionMoreOften() ? stats.system_bytes : 0);
  EXPECT_EQ(r1->free_backed().in_bytes(), 0);
  EXPECT_EQ(stats.unmapped_bytes, stats.system_bytes);
}

TEST_P(HugeRegionSetTest, ReleaseAdaptive) {
  if (!UseHugeRegionMoreOften()) {
    return;
  }

  PageId p;
  constexpr Length kSize = kPagesPerHugePage;
  bool from_released;
  auto r1 = GetRegion();
  set_.Contribute(r1.get());

  std::vector<Alloc> allocs;

  // Allocate all space.
  while (set_.MaybeGet(kSize, &p, &from_released)) {
    allocs.push_back({p, kSize});
  }

  // Put all back.
  for (auto a : allocs) {
    ASSERT_TRUE(set_.MaybePut(Range(a.p, a.n)));
  }

  // Adaptive release limit is 0, so ReleasePages should release nothing.
  Length released =
      set_.ReleasePages(Length::max(), HugeRegionAdaptiveRelease::kEnabled,
                        /*hit_limit=*/false);
  EXPECT_EQ(released.in_pages(), Length(0));

  // Adaptive release limit is now reset to free_backed_count (which is
  // the total size of region since we put all back).
  // Let's allocate some.
  size_t to_alloc = allocs.size() / 2;

  std::vector<Alloc> active_allocs;
  for (size_t i = 0; i < to_alloc; ++i) {
    ASSERT_TRUE(set_.MaybeGet(kSize, &p, &from_released));
    active_allocs.push_back({p, kSize});
  }

  // Put them back.
  for (auto a : active_allocs) {
    ASSERT_TRUE(set_.MaybePut(Range(a.p, a.n)));
  }

  // Now free_backed is back to full, but adaptive release limit is full -
  // to_alloc. Verify we release up to the adaptive release limit (low water
  // mark) when use_adaptive is enabled.
  released =
      set_.ReleasePages(Length::max(), HugeRegionAdaptiveRelease::kEnabled,
                        /*hit_limit=*/false);
  const Length expected_released =
      NHugePages(allocs.size() - to_alloc).in_pages();

  ASSERT_GT(expected_released, Length(0));
  EXPECT_EQ(released, expected_released);
  EXPECT_LT(released, r1->size().in_pages());
}

TEST_P(HugeRegionSetTest, ReleaseAdaptiveWithLimit) {
  if (!UseHugeRegionMoreOften()) {
    return;
  }

  PageId p;
  constexpr Length kSize = kPagesPerHugePage;
  bool from_released;
  auto r1 = GetRegion();
  set_.Contribute(r1.get());

  std::vector<Alloc> allocs;

  // Allocate all space.
  while (set_.MaybeGet(kSize, &p, &from_released)) {
    allocs.push_back({p, kSize});
  }

  // Put all back.
  for (auto a : allocs) {
    ASSERT_TRUE(set_.MaybePut(Range(a.p, a.n)));
  }

  // Low water mark is 0, so ReleasePages should release nothing.
  Length released =
      set_.ReleasePages(Length::max(), HugeRegionAdaptiveRelease::kEnabled,
                        /*hit_limit=*/false);
  EXPECT_EQ(released.in_pages(), Length(0));

  // Adaptive release limit is now reset to free_backed_count (which is
  // the total size of region since we put all back).
  // Let's allocate some.
  size_t to_alloc = allocs.size() / 2;
  std::vector<Alloc> active_allocs;
  for (size_t i = 0; i < to_alloc; ++i) {
    ASSERT_TRUE(set_.MaybeGet(kSize, &p, &from_released));
    active_allocs.push_back({p, kSize});
  }

  // Put them back.
  for (auto a : active_allocs) {
    ASSERT_TRUE(set_.MaybePut(Range(a.p, a.n)));
  }

  // Now free_backed is back to full, but adaptive release limit is full -
  // to_alloc.
  const Length expected_released =
      NHugePages(allocs.size() - to_alloc).in_pages();

  // We pass a limit which is less than expected_released.
  // Since use_adaptive is true and hit_limit is false, this limit (desired)
  // should be ignored, and we should release up to the low water mark.
  Length limit = kPagesPerHugePage;
  ASSERT_LT(limit, expected_released);

  released = set_.ReleasePages(limit, HugeRegionAdaptiveRelease::kEnabled,
                               /*hit_limit=*/false);

  EXPECT_EQ(released, expected_released);
}

TEST_P(HugeRegionSetTest, ReleaseAdaptiveWithHitLimit) {
  if (!UseHugeRegionMoreOften()) {
    return;
  }

  PageId p;
  constexpr Length kSize = kPagesPerHugePage;
  bool from_released;
  auto r1 = GetRegion();
  set_.Contribute(r1.get());

  std::vector<Alloc> allocs;

  // Allocate all space.
  while (set_.MaybeGet(kSize, &p, &from_released)) {
    allocs.push_back({p, kSize});
  }

  // Put all back.
  for (auto a : allocs) {
    ASSERT_TRUE(set_.MaybePut(Range(a.p, a.n)));
  }

  // Low water mark is 0. If we call ReleasePages with hit_limit = false,
  // it would release nothing.
  // But with hit_limit = true, it should release everything.
  Length released =
      set_.ReleasePages(Length::max(), HugeRegionAdaptiveRelease::kEnabled,
                        /*hit_limit=*/true);
  EXPECT_EQ(released, r1->size().in_pages());

  // Verify everything is unmapped.
  BackingStats stats = set_.stats();
  EXPECT_EQ(stats.unmapped_bytes, stats.system_bytes);
  EXPECT_EQ(r1->free_backed().in_bytes(), 0);
}

TEST_P(HugeRegionSetTest, ReleaseAdaptiveOrder) {
  if (!UseHugeRegionMoreOften()) {
    return;
  }

  PageId p;
  constexpr Length kSize = kPagesPerHugePage;
  bool from_released;
  auto r1 = GetRegion();
  set_.Contribute(r1.get());

  // Allocate memory in r1 and free it so we have backed pages.
  std::vector<Alloc> r1_allocs;
  while (set_.MaybeGet(kSize, &p, &from_released)) {
    r1_allocs.push_back({p, kSize});
  }
  for (size_t i = 1; i < r1_allocs.size(); ++i) {
    ASSERT_TRUE(set_.MaybePut(Range(r1_allocs[i].p, r1_allocs[i].n)));
  }

  // Do the same for r2.
  auto r2 = GetRegion();
  set_.Contribute(r2.get());

  std::vector<Alloc> r2_allocs;
  while (set_.MaybeGet(kSize, &p, &from_released)) {
    r2_allocs.push_back({p, kSize});
  }

  for (auto a : r2_allocs) {
    ASSERT_TRUE(set_.MaybePut(Range(a.p, a.n)));
  }

  // r1 has 1 active alloc, so r1->longest_free() < r2->longest_free().
  // Thus r1 is first in list_, and r2 is last in list_.
  HugeLength r1_backed_before = r1->free_backed();
  HugeLength r2_backed_before = r2->free_backed();
  ASSERT_EQ(r1_backed_before, r1->size() - NHugePages(1));
  ASSERT_EQ(r2_backed_before, r2->size());

  // Release 1 hugepage with use_adaptive = true.
  // * Adaptive release iterates list_ in reverse, so r2 (the last region)
  //   should be released first.
  // * `hit_limit` ensures we try to release the full desired quantity.
  Length released =
      set_.ReleasePages(kSize, HugeRegionAdaptiveRelease::kEnabled,
                        /*hit_limit=*/true);
  EXPECT_EQ(released, kSize);

  // r2 lost 1 hugepage of free_backed, while r1's free_backed is untouched.
  EXPECT_EQ(r2->free_backed(), r2_backed_before - NHugePages(1));
  EXPECT_EQ(r1->free_backed(), r1_backed_before);

  ASSERT_TRUE(set_.MaybePut(Range(r1_allocs[0].p, r1_allocs[0].n)));
}

TEST_P(HugeRegionSetTest, Set) {
  absl::BitGen rng;
  PageId p;
  constexpr Length kSize = kPagesPerHugePage + Length(1);
  bool from_released;
  ASSERT_FALSE(set_.MaybeGet(Length(1), &p, &from_released));
  auto r1 = GetRegion();
  auto r2 = GetRegion();
  auto r3 = GetRegion();
  auto r4 = GetRegion();
  set_.Contribute(r1.get());
  set_.Contribute(r2.get());
  set_.Contribute(r3.get());
  set_.Contribute(r4.get());

  std::vector<Alloc> allocs;
  std::vector<Alloc> doomed;

  while (set_.MaybeGet(kSize, &p, &from_released)) {
    allocs.push_back({p, kSize});
  }

  // Define a random set by shuffling, then move half of the allocations into
  // doomed.
  std::shuffle(allocs.begin(), allocs.end(), rng);
  doomed.insert(doomed.begin(), allocs.begin() + allocs.size() / 2,
                allocs.end());
  allocs.erase(allocs.begin() + allocs.size() / 2, allocs.end());

  for (auto d : doomed) {
    ASSERT_TRUE(set_.MaybePut(Range(d.p, d.n)));
  }

  for (size_t i = 0; i < 100 * 1000; ++i) {
    const size_t N = allocs.size();
    size_t index = absl::Uniform<int32_t>(rng, 0, N);
    std::swap(allocs[index], allocs[N - 1]);
    auto a = allocs.back();
    ASSERT_TRUE(set_.MaybePut(Range(a.p, a.n)));
    allocs.pop_back();
    ASSERT_TRUE(set_.MaybeGet(kSize, &p, &from_released));
    allocs.push_back({p, kSize});
  }

  // Random traffic should have defragmented our allocations into full
  // and empty regions, and released the empty ones.  Annoyingly, we don't
  // know which region is which, so we have to do a bit of silliness:
  std::vector<Region*> regions = {r1.get(), r2.get(), r3.get(), r4.get()};
  std::sort(regions.begin(), regions.end(),
            [](const Region* a, const Region* b) -> bool {
              return a->used_pages() > b->used_pages();
            });

  for (int i = 0; i < regions.size(); i++) {
    TC_LOG("i=%v used=%v free=%v unmapped=%v", i, regions[i]->used_pages(),
           regions[i]->free_pages(), regions[i]->unmapped_pages());
  }
  // Now first two should be "full" (ish)
  EXPECT_LE(Region::size().in_pages().raw_num() * 0.9,
            regions[0]->used_pages().raw_num());
  EXPECT_LE(Region::size().in_pages().raw_num() * 0.9,
            regions[1]->used_pages().raw_num());
  // and last two "empty" (ish.)
  if (UseHugeRegionMoreOften()) {
    EXPECT_EQ(regions[2]->unmapped_pages(), Length(0));
    EXPECT_EQ(regions[3]->unmapped_pages(), Length(0));
    EXPECT_GT(regions[2]->free_backed().raw_num(),
              Region::size().raw_num() * 0.9);
    EXPECT_GT(regions[3]->free_backed().raw_num(),
              Region::size().raw_num() * 0.9);
  } else {
    EXPECT_LE(Region::size().in_pages().raw_num() * 0.9,
              regions[2]->unmapped_pages().raw_num());
    EXPECT_LE(Region::size().in_pages().raw_num() * 0.9,
              regions[3]->unmapped_pages().raw_num());
    EXPECT_EQ(regions[2]->free_backed(), NHugePages(0));
    EXPECT_EQ(regions[3]->free_backed(), NHugePages(0));
  }

  // Check the stats line up.
  auto stats = set_.stats();
  auto raw = r1->stats();
  raw += r2->stats();
  raw += r3->stats();
  raw += r4->stats();
  EXPECT_EQ(raw.system_bytes, stats.system_bytes);
  EXPECT_EQ(raw.unmapped_bytes, stats.unmapped_bytes);
  EXPECT_EQ(raw.free_bytes, stats.free_bytes);

  // Print out the stats for inspection of formats.
  std::vector<char> buf(64 * 1024);
  Printer out(&buf[0], buf.size());
  set_.Print(out);
  std::cout << out.view() << "\n";
  EXPECT_THAT(out.view(), testing::HasSubstr("0 low water mark free backed"));

  std::vector<char> pbtxt_buf(64 * 1024);
  Printer pbtxt_out(&pbtxt_buf[0], pbtxt_buf.size());
  PbtxtRegion region(pbtxt_out, kNested);
  set_.PrintInPbtxt(region);
  EXPECT_THAT(pbtxt_out.view(),
              testing::HasSubstr("huge_region_low_water_mark_bytes: 0"));
}

TEST_P(HugeRegionSetTest, GetPageAllocationStatus) {
  PageBitmap pages;
  HugePage hp = next_;
  auto r1 = GetRegion();
  set_.Contribute(r1.get());

  HugePage outside_hp = hp + Region::size();
  EXPECT_FALSE(set_.GetPageAllocationStatus(outside_hp, pages));

  // Region is in set_, unbacked initially.
  EXPECT_TRUE(set_.GetPageAllocationStatus(hp, pages));
  EXPECT_EQ(pages.CountBits(0, kPagesPerHugePage.raw_num()), 0);

  PageId p;
  bool from_released;
  ASSERT_TRUE(set_.MaybeGet(Length(7), &p, &from_released));
  EXPECT_EQ(p, hp.first_page());

  EXPECT_TRUE(set_.GetPageAllocationStatus(hp, pages));
  EXPECT_EQ(pages.CountBits(0, kPagesPerHugePage.raw_num()), 7);
  for (size_t i = 0; i < 7; ++i) {
    EXPECT_TRUE(pages.GetBit(i));
  }

  ASSERT_TRUE(set_.MaybePut(Range(p, Length(7))));
}

TEST_P(HugeRegionSetTest, TimeSeriesMinMaxWindow) {
  EXPECT_THAT(PrintToString(),
              testing::HasSubstr(
                  "HugeRegionSet: recent usage range: 0 min - 0 curr - 0 max "
                  "MiB\nHugeRegionSet: recent free backed range: 0 min - 0 "
                  "curr - 0 max MiB"));

  auto r1 = GetRegion();
  set_.Contribute(r1.get());

  const Length kHalfHp = kPagesPerHugePage / 2;
  PageId p1, p2, p3;
  bool from_released = false;

  // Epoch 0 (t = 0ms): allocate 0.5 HP + 4 HP, backing 5 hugepages in use.
  ASSERT_TRUE(set_.MaybeGet(kHalfHp, &p1, &from_released));
  EXPECT_TRUE(from_released);
  ASSERT_TRUE(set_.MaybeGet(NHugePages(4).in_pages(), &p2, &from_released));
  EXPECT_TRUE(from_released);

  // Epoch 1 (t = 1000ms): free the 4 HP allocation, leaving 1 HP in use and
  // 4 HP free-backed when lazy release is enabled (or 0 when eagerly unbacked).
  AdvanceClock(absl::Seconds(1));
  ASSERT_TRUE(set_.MaybePut(Range(p2, NHugePages(4).in_pages())));

  if (!UseHugeRegionMoreOften()) {
    const std::string pbtxt = PrintInPbtxtToString();
    EXPECT_THAT(pbtxt, testing::HasSubstr("huge_region_usage_history {"));
    ASSERT_TRUE(set_.MaybePut(Range(p1, kHalfHp)));
    return;
  }

  // Epoch 2 (t = 2000ms): allocate 2 HP from free-backed pages (3 HP in use,
  // 2 HP free-backed), release the remaining 2 HP free-backed pages, and then
  // free p3 so 1 HP remains in use and 2 HP remain free-backed.
  AdvanceClock(absl::Seconds(1));
  ASSERT_TRUE(set_.MaybeGet(NHugePages(2).in_pages(), &p3, &from_released));
  EXPECT_FALSE(from_released);
  EXPECT_EQ(
      set_.ReleasePages(Length::max(), HugeRegionAdaptiveRelease::kEnabled,
                        /*hit_limit=*/true),
      NHugePages(2).in_pages());
  ASSERT_TRUE(set_.MaybePut(Range(p3, NHugePages(2).in_pages())));

  const std::string text = PrintToString();
  EXPECT_THAT(text, testing::HasSubstr(absl::StrFormat(
                        "HugeRegionSet: recent usage range: %zu min - %zu curr "
                        "- %zu max MiB",
                        NHugePages(1).in_mib(), NHugePages(1).in_mib(),
                        NHugePages(5).in_mib())));
  EXPECT_THAT(
      text,
      testing::HasSubstr(absl::StrFormat(
          "HugeRegionSet: recent free backed range: 0 min - %zu curr - %zu max "
          "MiB",
          NHugePages(2).in_mib(), NHugePages(4).in_mib())));
  EXPECT_THAT(text, testing::HasSubstr("HugeRegionSet: Usage timeseries "));
  EXPECT_THAT(text,
              testing::HasSubstr("HugeRegionSet: Free backed timeseries "));

  // Advance across 605 one-second epochs (> 600 slots) with a 1 HP pulse so
  // the initial 5 HP usage peak and 0 / 4 HP free-backed extrema roll off the
  // 10-minute window.
  for (int epoch = 0; epoch < 605; ++epoch) {
    AdvanceClock(absl::Seconds(1));
    PageId p;
    ASSERT_TRUE(set_.MaybeGet(kPagesPerHugePage, &p, &from_released));
    EXPECT_FALSE(from_released);
    ASSERT_TRUE(set_.MaybePut(Range(p, kPagesPerHugePage)));
  }

  ASSERT_TRUE(set_.MaybePut(Range(p1, kHalfHp)));
}

// While ReleasePages has a region unbacking a range, other threads may Get
// from and Put to the set (b/73749855).  The set learns of the release only
// when ReleasePages returns, so meanwhile its free_backed() exceeds its
// regions' by the range in flight; afterwards the two agree.
TEST_P(HugeRegionSetTest, GetAndPutDuringReleasePages) {
  const Length hp = kPagesPerHugePage;
  for (HugeRegionAdaptiveRelease adaptive :
       {HugeRegionAdaptiveRelease::kDisabled,
        HugeRegionAdaptiveRelease::kEnabled}) {
    for (bool success : {true, false}) {
      SCOPED_TRACE(testing::Message()
                   << "adaptive=" << static_cast<bool>(adaptive)
                   << " success=" << success);
      Reset();
      unback_.success_ = success;
      std::unique_ptr<Region> region = GetRegion(unback_without_lock_);
      // Hugepages 0-3 are backed; 1 and 2 are free, 0 and 3 are live.
      Range hp0 = Allocate(*region, hp);
      Range hp1_2 = Allocate(*region, 2 * hp);
      Range hp3 = Allocate(*region, hp);
      region->Put(hp1_2, /*release=*/false, [] {});
      set_.Contribute(region.get());
      ASSERT_EQ(set_.free_backed(), NHugePages(2));

      // Unless UseHugeRegionMoreOften, the MaybePut below unbacks hugepage 3
      // through the same unbacker, re-entering the hook.
      int hooks = 0;
      unback_without_lock_.unlocked_hook_ = [&](Range r) {
        if (hooks++ > 0) {
          return;
        }
        // ReleasePages unbacks hugepages 1-2 in a single call; region has
        // pre-decremented them from free_backed(), while set_ has not yet.
        EXPECT_EQ(r.p, hp1_2.p);
        EXPECT_EQ(r.n, hp1_2.n);
        PageHeapSpinLockHolder l;
        const HugeLength in_flight = NHugePages(2);
        ExpectFreeBacked({region.get()}, in_flight);

        // MaybePut applies region's before/after diff on hugepage 3 to set_,
        // leaving the 2-hugepage skew from the in-flight unback unchanged.
        ASSERT_TRUE(set_.MaybePut(hp3));
        ExpectFreeBacked({region.get()}, in_flight);

        // Hugepages 1-2 are marked in flight, so MaybeGet lands back on
        // hugepage 3; it was unbacked iff MaybePut tried to release and
        // succeeded.
        bool from_released;
        ASSERT_TRUE(set_.MaybeGet(hp, &hp3.p, &from_released));
        EXPECT_EQ(hp3.p, hp0.p + 3 * hp);
        EXPECT_EQ(from_released, !UseHugeRegionMoreOften() && success);
        ExpectFreeBacked({region.get()}, in_flight);
      };
      const Length released = ReleaseAll(10 * hp, adaptive);
      unback_without_lock_.unlocked_hook_ = nullptr;
      // MaybePut also unbacked hugepage 3 unless UseHugeRegionMoreOften.
      EXPECT_EQ(hooks, UseHugeRegionMoreOften() ? 1 : 2);
      // On success set_ subtracts the 2 unbacked hugepages; on failure region
      // restores them to free_backed() and set_ subtracts 0.
      EXPECT_EQ(released, success ? 2 * hp : Length(0));
      ExpectFreeBacked({region.get()}, NHugePages(0));
      EXPECT_EQ(set_.free_backed(), success ? NHugePages(0) : NHugePages(2));

      const Range live[] = {hp0, hp3};
      PutUnderLock(live);
      ExpectFreeBacked({region.get()}, NHugePages(0));
    }
  }
}

// A ReleasePages that runs while another has a range in flight releases the
// other candidates, which the outer call then skips.  Together they release
// each free hugepage once.
TEST_P(HugeRegionSetTest, ReleasePagesDuringReleasePages) {
  const Length hp = kPagesPerHugePage;
  for (HugeRegionAdaptiveRelease adaptive :
       {HugeRegionAdaptiveRelease::kDisabled,
        HugeRegionAdaptiveRelease::kEnabled}) {
    SCOPED_TRACE(testing::Message()
                 << "adaptive=" << static_cast<bool>(adaptive));
    Reset();
    std::unique_ptr<Region> region = GetRegion(unback_without_lock_);
    // Hugepages 0-4 are backed; 1 and 3 are free, and are unbacked by
    // separate calls.
    Range hp0 = Allocate(*region, hp);
    Range hp1 = Allocate(*region, hp);
    Range hp2 = Allocate(*region, hp);
    Range hp3 = Allocate(*region, hp);
    Range hp4 = Allocate(*region, hp);
    region->Put(hp1, /*release=*/false, [] {});
    region->Put(hp3, /*release=*/false, [] {});
    set_.Contribute(region.get());
    ASSERT_EQ(set_.free_backed(), NHugePages(2));

    // The nested ReleasePages unbacks hugepage 3 through the same unbacker,
    // re-entering the hook; only the outer call runs the nested ReleaseAll.
    int hooks = 0;
    Length inner;
    unback_without_lock_.unlocked_hook_ = [&](Range r) {
      if (hooks++ > 0) {
        return;
      }
      // UnbackHugepages scans should_unback from low index in both modes, so
      // the outer call unbacks hugepage 1 first and hides it while in flight.
      EXPECT_EQ(r.p, hp1.p);
      EXPECT_EQ(r.n, hp1.n);
      ExpectFreeBacked({region.get()}, NHugePages(1));
      // Nested ReleasePages skips in-flight hugepage 1, unbacks hugepage 3,
      // and subtracts 1 hugepage from set_.free_backed() before returning;
      // only the outer call's 1 in-flight hugepage remains skewed.
      inner = ReleaseAll(10 * hp, adaptive);
      EXPECT_EQ(inner, hp);
      ExpectFreeBacked({region.get()}, NHugePages(1));
    };
    const Length outer = ReleaseAll(10 * hp, adaptive);
    unback_without_lock_.unlocked_hook_ = nullptr;
    // Outer re-checks CanUnback(3) after re-acquiring the lock and skips
    // already-unbacked hugepage 3, so each call releases and subtracts 1
    // hugepage and only the 3 live hugepages (0, 2, 4) stay backed.
    EXPECT_EQ(hooks, 2);
    EXPECT_EQ(outer, hp);
    EXPECT_EQ(outer + inner, 2 * hp);
    EXPECT_EQ(unback_.unbacked_, 2);
    EXPECT_EQ(set_.free_backed(), NHugePages(0));
    ExpectFreeBacked({region.get()}, NHugePages(0));
    EXPECT_EQ(region->backed(), NHugePages(3));

    const Range live[] = {hp0, hp2, hp4};
    PutUnderLock(live);
  }
}

// A region contributed while ReleasePages has a range in flight is counted
// once, whether or not that ReleasePages goes on to visit it.
TEST_P(HugeRegionSetTest, ContributeDuringReleasePages) {
  const Length hp = kPagesPerHugePage;
  for (HugeRegionAdaptiveRelease adaptive :
       {HugeRegionAdaptiveRelease::kDisabled,
        HugeRegionAdaptiveRelease::kEnabled}) {
    SCOPED_TRACE(testing::Message()
                 << "adaptive=" << static_cast<bool>(adaptive));
    Reset();
    std::unique_ptr<Region> r1 = GetRegion(unback_without_lock_);
    // r1 has hugepages 0-3 backed, 1 and 2 free; r2 has 0-1 backed, 0 free.
    Range r1_hp0 = Allocate(*r1, hp);
    Range r1_hp1_2 = Allocate(*r1, 2 * hp);
    Range r1_hp3 = Allocate(*r1, hp);
    r1->Put(r1_hp1_2, /*release=*/false, [] {});
    set_.Contribute(r1.get());
    std::unique_ptr<Region> r2 = GetRegion();
    Range r2_hp0 = Allocate(*r2, hp);
    Range r2_hp1 = Allocate(*r2, hp);
    r2->Put(r2_hp0, /*release=*/false, [] {});

    unback_without_lock_.unlocked_hook_ = [&](Range r) {
      PageHeapSpinLockHolder l;
      // r2 adds 1 free-backed hugepage to set_ while r1 still has 2 in flight.
      set_.Contribute(r2.get());
      EXPECT_EQ(set_.ActiveRegions(), 2);
      ExpectFreeBacked({r1.get(), r2.get()}, NHugePages(2));
    };
    const Length released = ReleaseAll(10 * hp, adaptive);
    unback_without_lock_.unlocked_hook_ = nullptr;
    // r2 has a longer free tail than r1 so Contribute places it after r1 in
    // list_: depending on whether the sweep visits r2 after r1, it releases 2
    // or 3 hugepages, and set_.free_backed() drops from 3 by that amount.
    EXPECT_GE(released, 2 * hp);
    EXPECT_LE(released, 3 * hp);
    EXPECT_EQ(set_.free_backed(), NHugePages(3) - HLFromPages(released));
    ExpectFreeBacked({r1.get(), r2.get()}, NHugePages(0));

    const Range live1[] = {r1_hp0, r1_hp3};
    const Range live2[] = {r2_hp1};
    PutUnderLock(live1);
    PutUnderLock(live2);
    ExpectFreeBacked({r1.get(), r2.get()}, NHugePages(0));
  }
}

// Regression test for the visit bound in HugeRegionSet::ReleasePages.
//
// Each region here holds exactly one contiguous free-but-backed range, so one
// region visit performs exactly one unback call. Freeing the tail of the
// region being visited grows its longest free range past every other
// region's, so Fix() moves it to the back of list_ and the reverse iterator
// resumes from the moved node's new neighbours, walking part of the list a
// second time.
//
// With the bound the traversal performs kNumRegions visits; without it, one
// more.
TEST_P(HugeRegionSetTest, ReleasePagesVisitBound) {
  constexpr int kNumRegions = 3;

  std::vector<std::unique_ptr<Region>> regions;
  // The still-allocated tail of each region, indexed like `regions`.
  std::vector<Range> tails;
  for (int i = 0; i < kNumRegions; ++i) {
    std::unique_ptr<Region> region = GetRegion(unback_without_lock_);

    // Back the whole region, then free a prefix. The region is left with a
    // free-but-backed range to release, and with a longest free range that is
    // strictly larger than the previous region's, so that Contribute() orders
    // list_ as R0, R1, R2 and the adaptive traversal visits R2, R1, R0.
    PageId p;
    bool from_released;
    ASSERT_TRUE(
        region->MaybeGet(Region::size().in_pages(), &p, &from_released));
    const Length free_len = NHugePages(4 * (i + 1)).in_pages();
    region->Put(Range(p, free_len), /*release=*/false, [] {});
    tails.push_back(Range(p + free_len, Region::size().in_pages() - free_len));

    set_.Contribute(region.get());
    regions.push_back(std::move(region));
  }

  // Always fail unbacks so `released` never advances: only the visit bound can
  // terminate the traversal.
  unback_.success_ = false;
  int visits = 0;
  bool in_put = false;
  bool relocated = false;
  unback_without_lock_.unlocked_hook_ = [&](Range r) {
    // When !UseHugeRegionMoreOften(), PutUnderLock -> MaybePut eagerly unbacks
    // the freed tail through this same unback_without_lock_, re-entering this
    // hook. Ignore that nested call so `visits` only counts regions visited by
    // ReleasePages.
    if (in_put) return;
    // Relocate while the second region of the traversal (R1) is being visited.
    // Relocating during the first (R2) would be a no-op: R2 is already at the
    // back of the list and cannot move further back.
    if (++visits != 2) return;
    if (regions[1]->contains(r.p)) {
      in_put = true;
      PutUnderLock({&tails[1], 1});
      in_put = false;
      relocated = true;
    }
  };

  EXPECT_EQ(ReleaseAll(Length::max(), HugeRegionAdaptiveRelease::kEnabled),
            Length(0));
  unback_without_lock_.unlocked_hook_ = nullptr;
  unback_.success_ = true;

  EXPECT_TRUE(relocated);
  EXPECT_EQ(visits, kNumRegions);
}

// While ReleasePages unbacks part of a region's free run, that run is marked
// used and the region's longest_free() reads low. Another thread may allocate
// from the set in that window (b/73749855); list_ must stay sorted by live
// longest_free() both during and after the unback so MaybeGet keeps allocating
// from the best fit.
TEST_P(HugeRegionSetTest, ListStaysSortedAcrossReleasePagesUnback) {
  const Length hp = kPagesPerHugePage;

  // r0 has a 4-hugepage unbacked free run; r1 has an 8-hugepage backed free
  // run, so Contribute orders list_ as [r0 (4), r1 (8)] and only r1 has pages
  // to release.
  std::unique_ptr<Region> r0 = GetRegion();
  const Range r0_whole = Allocate(*r0, Region::size().in_pages());
  r0->Put(Range(r0_whole.p, 4 * hp), /*release=*/true, [] {});
  set_.Contribute(r0.get());

  std::unique_ptr<Region> r1 = GetRegion(unback_without_lock_);
  const Range r1_whole = Allocate(*r1, Region::size().in_pages());
  r1->Put(Range(r1_whole.p, 8 * hp), /*release=*/false, [] {});
  set_.Contribute(r1.get());

  Length mid_r_n;
  Length mid_r1_longest_free;
  bool mid_get_ok = false;
  PageId mid_get_page;
  unback_without_lock_.unlocked_hook_ = [&](Range r) {
    // 5 of r1's 8 free hugepages are in flight; 3 remain free, so r1 must move
    // ahead of r0 (4) while unbacking and serve a 1-hugepage Get, leaving r1
    // at 2 free hugepages until Unmark restores the 5 unbacked hugepages.
    mid_r_n = r.n;
    PageHeapSpinLockHolder l;
    mid_r1_longest_free = r1->longest_free();
    bool from_released;
    mid_get_ok = set_.MaybeGet(hp, &mid_get_page, &from_released);
  };
  EXPECT_EQ(ReleaseAll(5 * hp, HugeRegionAdaptiveRelease::kDisabled), 5 * hp);
  unback_without_lock_.unlocked_hook_ = nullptr;
  EXPECT_EQ(mid_r_n, 5 * hp);
  EXPECT_EQ(mid_r1_longest_free, 3 * hp);
  ASSERT_TRUE(mid_get_ok);
  EXPECT_TRUE(r1->contains(mid_get_page));

  // Unmarking the 5 unbacked hugepages grows r1's longest_free() back to 5
  // (above r0's 4), so list_ must return to [r0 (4), r1 (5)] and a 4-hugepage
  // Get must pick r0 over r1.
  PageId post_get_page;
  bool from_released;
  ASSERT_TRUE(set_.MaybeGet(4 * hp, &post_get_page, &from_released));
  EXPECT_TRUE(r0->contains(post_get_page));
}

TEST(HugeRegionNamedVmaTest, NamedVmaNormal) {
  MockMemoryTagFunction mock_set_anon_vma_name;
  NilUnback nil_unback;
  HugePage p = HugePageContaining(MakeTaggedAddress(MemoryTag::kNormal));

  Range expected_range{p.first_page(), HugeRegion::size().in_pages()};
  EXPECT_CALL(
      mock_set_anon_vma_name,
      Call(testing::AllOf(testing::Field(&Range::p, expected_range.p),
                          testing::Field(&Range::n, expected_range.n)),
           std::optional<absl::string_view>("tcmalloc_huge_region_NORMAL")))
      .Times(1);

  HugeRegion region({p, HugeRegion::size()}, nil_unback,
                    mock_set_anon_vma_name);
}

TEST(HugeRegionNamedVmaTest, NamedVmaCold) {
  MockMemoryTagFunction mock_set_anon_vma_name;
  NilUnback nil_unback;
  HugePage p = HugePageContaining(MakeTaggedAddress(MemoryTag::kSampledOrCold));

  Range expected_range{p.first_page(), HugeRegion::size().in_pages()};
  EXPECT_CALL(mock_set_anon_vma_name,
              Call(testing::AllOf(testing::Field(&Range::p, expected_range.p),
                                  testing::Field(&Range::n, expected_range.n)),
                   std::optional<absl::string_view>(
                       "tcmalloc_huge_region_SAMPLED_OR_COLD")))
      .Times(1);

  HugeRegion region({p, HugeRegion::size()}, nil_unback,
                    mock_set_anon_vma_name);
}

INSTANTIATE_TEST_SUITE_P(
    All, HugeRegionSetTest,
    testing::Values(HugeRegionUsageOption::kDefault,
                    HugeRegionUsageOption::kUseForAllLargeAllocs));

}  // namespace
}  // namespace tcmalloc_internal
}  // namespace tcmalloc
