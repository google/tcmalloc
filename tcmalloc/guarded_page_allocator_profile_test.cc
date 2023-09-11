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

#include <stddef.h>

#include <cmath>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "benchmark/benchmark.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "absl/container/flat_hash_set.h"
#include "absl/functional/function_ref.h"
#include "tcmalloc/malloc_extension.h"
#include "tcmalloc/static_vars.h"
#include "tcmalloc/testing/testutil.h"

namespace tcmalloc {
namespace tcmalloc_internal {
namespace {

class GuardedPageAllocatorProfileTest : public testing::Test {
 public:
  struct NextSteps {
    bool stop = true;  // stop allocating
    bool free = true;  // free allocation
  };

  void SetUp() override { MallocExtension::ActivateGuardedSampling(); }

  // Return the number of allocations
  int AllocateUntil(size_t size,
                    absl::FunctionRef<NextSteps(void*)> evaluate_alloc) {
    int alloc_count = 0;
    while (true) {
      void* alloc = ::operator new(size);
      ++alloc_count;
      benchmark::DoNotOptimize(alloc);
      auto result = evaluate_alloc(alloc);
      // evaluate_alloc takes responsibility for delete/free if result.free is
      // set to false.
      if (result.free) {
        ::operator delete(alloc);
      }
      if (result.stop) {
        break;
      }
    }
    return alloc_count;
  }

  // Allocate until sample is guarded
  // Called to reduce the internal counter to -1, which will trigger resetting
  // the counter to the configured rate.
  void AllocateUntilGuarded() {
    AllocateUntil(968, [&](void* alloc) -> NextSteps {
      return {IsSampledMemory(alloc) &&
                  Static::guardedpage_allocator().PointerIsMine(alloc),
              true};
    });
  }

  void ExamineSamples(
      Profile& profile, Profile::Sample::GuardedStatus sought_status,
      absl::flat_hash_set<Profile::Sample::GuardedStatus> allowable_statuses,
      absl::FunctionRef<void(const Profile::Sample& s)> verify =
          [](const Profile::Sample& s) { /* do nothing */ }) {
    absl::flat_hash_set<Profile::Sample::GuardedStatus> found_statuses;
    int samples = 0;
    profile.Iterate([&](const Profile::Sample& s) {
      ++samples;
      found_statuses.insert(s.guarded_status);
      verify(s);
    });
    EXPECT_THAT(found_statuses, ::testing::Contains(sought_status));
    found_statuses.erase(sought_status);
    EXPECT_THAT(found_statuses, ::testing::IsSubsetOf(allowable_statuses));
  }
};

class ParameterizedGuardedPageAllocatorProfileTest
    : public GuardedPageAllocatorProfileTest,
      public testing::WithParamInterface<bool> {};

TEST_P(ParameterizedGuardedPageAllocatorProfileTest, Guarded) {
  ScopedAlwaysSample always_sample;
  ScopedImprovedGuardedSampling improved_guarded_sampling(
      /*is_enabled=*/GetParam());
  AllocateUntilGuarded();
  auto token = MallocExtension::StartAllocationProfiling();

  AllocateUntil(1051, [&](void* alloc) -> NextSteps { return {true, true}; });

  auto profile = std::move(token).Stop();
  ExamineSamples(profile, Profile::Sample::GuardedStatus::Guarded, {});
}

TEST_P(ParameterizedGuardedPageAllocatorProfileTest, NotAttempted) {
  ScopedProfileSamplingRate profile_sampling_rate(4096);
  ScopedImprovedGuardedSampling improved_guarded_sampling(
      /*is_enabled=*/GetParam());
  auto token = MallocExtension::StartAllocationProfiling();

  constexpr size_t alloc_size = 2 * 1024 * 1024;
  AllocateUntil(alloc_size, [&](void* alloc) -> NextSteps {
    return {true, true};
  });

  auto profile = std::move(token).Stop();
  ExamineSamples(profile, Profile::Sample::GuardedStatus::NotAttempted,
                 {Profile::Sample::GuardedStatus::Guarded},
                 [&](const Profile::Sample& s) {
                   switch (s.guarded_status) {
                     case Profile::Sample::GuardedStatus::Guarded:
                       EXPECT_NE(alloc_size, s.requested_size);
                       break;
                     default:
                       break;
                   }
                 });
}

TEST_P(ParameterizedGuardedPageAllocatorProfileTest, LargerThanOnePage) {
  ScopedAlwaysSample always_sample;
  ScopedImprovedGuardedSampling improved_guarded_sampling(
      /*is_enabled=*/GetParam());
  AllocateUntilGuarded();
  auto token = MallocExtension::StartAllocationProfiling();

  constexpr size_t alloc_size = kPageSize + 1;
  AllocateUntil(alloc_size, [&](void* alloc) -> NextSteps {
    return {true, true};
  });

  auto profile = std::move(token).Stop();
  ExamineSamples(profile, Profile::Sample::GuardedStatus::LargerThanOnePage,
                 {Profile::Sample::GuardedStatus::Guarded},
                 [&](const Profile::Sample& s) {
                   switch (s.guarded_status) {
                     case Profile::Sample::GuardedStatus::Guarded:
                       EXPECT_NE(alloc_size, s.requested_size);
                       break;
                     default:
                       break;
                   }
                 });
}

TEST_P(ParameterizedGuardedPageAllocatorProfileTest, Disabled) {
  ScopedGuardedSamplingRate guarded_sampling_rate(-1);
  ScopedProfileSamplingRate profile_sampling_rate(1);
  ScopedImprovedGuardedSampling improved_guarded_sampling(
      /*is_enabled=*/GetParam());
  auto token = MallocExtension::StartAllocationProfiling();

  AllocateUntil(1024, [&](void* alloc) -> NextSteps { return {true, true}; });

  auto profile = std::move(token).Stop();
  ExamineSamples(profile, Profile::Sample::GuardedStatus::Disabled, {});
}

TEST_F(GuardedPageAllocatorProfileTest, RateLimited) {
  ScopedGuardedSamplingRate guarded_sampling_rate(1);
  ScopedProfileSamplingRate profile_sampling_rate(1);
  auto token = MallocExtension::StartAllocationProfiling();

  // Keep allocating until something is sampled
  constexpr size_t alloc_size = 1033;
  bool guarded_found = false;
  bool unguarded_found = false;
  AllocateUntil(alloc_size, [&](void* alloc) -> NextSteps {
    if (IsSampledMemory(alloc)) {
      if (Static::guardedpage_allocator().PointerIsMine(alloc)) {
        guarded_found = true;
      } else {
        unguarded_found = true;
      }
      return {guarded_found && unguarded_found, true};
    }
    return {false, true};
  });

  // Ensure Guarded and RateLimited both occur for the alloc_size
  bool success_found = false;
  bool ratelimited_found = false;
  auto profile = std::move(token).Stop();
  ExamineSamples(profile, Profile::Sample::GuardedStatus::RateLimited,
                 {Profile::Sample::GuardedStatus::Guarded},
                 [&](const Profile::Sample& s) {
                   if (s.requested_size != alloc_size) {
                     return;
                   }
                   switch (s.guarded_status) {
                     case Profile::Sample::GuardedStatus::Guarded:
                       success_found = true;
                       break;
                     case Profile::Sample::GuardedStatus::RateLimited:
                       ratelimited_found = true;
                       break;
                     default:
                       break;
                   }
                 });
  EXPECT_TRUE(success_found);
  EXPECT_TRUE(ratelimited_found);
}

TEST_P(ParameterizedGuardedPageAllocatorProfileTest, TooSmall) {
  ScopedAlwaysSample always_sample;
  AllocateUntilGuarded();
  auto token = MallocExtension::StartAllocationProfiling();

  // Next sampled allocation should be too small
  constexpr size_t alloc_size = 0;
  AllocateUntil(alloc_size, [&](void* alloc) -> NextSteps {
    return {true, true};
  });

  auto profile = std::move(token).Stop();
  ExamineSamples(profile, Profile::Sample::GuardedStatus::TooSmall,
                 {Profile::Sample::GuardedStatus::RateLimited,
                  Profile::Sample::GuardedStatus::Guarded},
                 [&](const Profile::Sample& s) {
                   switch (s.guarded_status) {
                     case Profile::Sample::GuardedStatus::Guarded:
                       EXPECT_NE(alloc_size, s.requested_size);
                       break;
                     case Profile::Sample::GuardedStatus::TooSmall:
                       EXPECT_EQ(alloc_size, s.requested_size);
                       break;
                     default:
                       break;
                   }
                 });
}

TEST_F(GuardedPageAllocatorProfileTest, NoAvailableSlots) {
  ScopedAlwaysSample always_sample;
  AllocateUntilGuarded();

  std::vector<std::unique_ptr<void, void (*)(void*)>> allocs;
  // Guard until there are no slots available.
  AllocateUntil(1039, [&](void* alloc) -> NextSteps {
    if (Static::guardedpage_allocator().PointerIsMine(alloc)) {
      allocs.emplace_back(alloc,
                          static_cast<void (*)(void*)>(::operator delete));
      return {Static::guardedpage_allocator().GetNumAvailablePages() == 0,
              false};
    }
    return {false, true};
  });

  auto token = MallocExtension::StartAllocationProfiling();
  // This should  fail for lack of slots
  constexpr size_t alloc_size = 1055;
  AllocateUntil(alloc_size, [&](void* alloc) -> NextSteps {
    return {true, true};
  });

  auto profile = std::move(token).Stop();
  ExamineSamples(profile, Profile::Sample::GuardedStatus::NoAvailableSlots, {});
}

TEST_P(ParameterizedGuardedPageAllocatorProfileTest, NeverSample) {
  ScopedProfileSamplingRate profile_sampling_rate(0);
  ScopedImprovedGuardedSampling improved_guarded_sampling(
      /*is_enabled=*/GetParam());
  auto token = MallocExtension::StartAllocationProfiling();

  int alloc_count = AllocateUntil(1025, [&](void* alloc) -> NextSteps {
    return {true, true};
  });
  ASSERT_EQ(alloc_count, 1);

  auto profile = std::move(token).Stop();
  int samples = 0;
  profile.Iterate([&](const Profile::Sample& s) { ++samples; });
  EXPECT_EQ(samples, 0);
}

INSTANTIATE_TEST_SUITE_P(x, ParameterizedGuardedPageAllocatorProfileTest,
                         testing::Bool());

TEST_F(GuardedPageAllocatorProfileTest, Filtered) {
  // Enable improved sampling, as filtered is only returned when improved
  // sampling is enabled.
  tcmalloc::ScopedImprovedGuardedSampling improved_guarded_sampling(
      /*is_enabled=*/true);

  // Have to have a rate that is less than every single one.
  ScopedGuardedSamplingRate scoped_guarded_sampling_rate(
      2 * tcmalloc::tcmalloc_internal::Parameters::profile_sampling_rate());
  AllocateUntilGuarded();

  size_t target_sampled_to_guarded_ratio = std::ceil(
      tcmalloc::tcmalloc_internal::Parameters::guarded_sampling_rate() /
      tcmalloc::tcmalloc_internal::Parameters::profile_sampling_rate());

  // Allocate until guarding begins in earnest.
  // In other words, do a lot of allocations which are sampled which drives down
  // the rate until the logic internally forces guarding to the point where a
  // stack trace is filtered for repetition.
  AllocateUntil(10969, [&](void*) -> NextSteps {
    size_t current_sampled_to_guarded_ratio =
        tc_globals.total_sampled_count_.value() /
        tc_globals.guardedpage_allocator().SuccessfulAllocations();
    return {current_sampled_to_guarded_ratio >=
                (2 * target_sampled_to_guarded_ratio),
            true};
  });

  auto token = MallocExtension::StartAllocationProfiling();
  // Obtain a few sample guarding canidates, which will eventualy yield at least
  // one that is filtered.
  bool guarded_found = false;
  bool unguarded_found = false;
  int sampled_count = 0;
  AllocateUntil(1057, [&](void* alloc) -> NextSteps {
    if (IsSampledMemory(alloc)) {
      if (Static::guardedpage_allocator().PointerIsMine(alloc)) {
        guarded_found = true;
      } else {
        unguarded_found = true;
      }
      ++sampled_count;
      if (sampled_count > 1000) {
        return {true, true};
      }
    }
    return {guarded_found && unguarded_found, true};
  });
  auto profile = std::move(token).Stop();

  ExamineSamples(profile, Profile::Sample::GuardedStatus::Filtered,
                 {Profile::Sample::GuardedStatus::Guarded});
}

TEST_F(GuardedPageAllocatorProfileTest, FilteredBrackets) {
  // Enable improved sampling, as filtered is only returned when improved
  // sampling is enabled.
  tcmalloc::ScopedImprovedGuardedSampling improved_guarded_sampling(
      /*is_enabled=*/true);
  // Attempt to guard every sample.
  ScopedGuardedSamplingRate scoped_guarded_sampling_rate(0);

  // Allocate until 2 guards placed, it should not exceed 5 attempts
  // (1st Guard: 100% (1), 2nd: 25% (4))
  int sampled_count = 0;
  int guarded_count = 0;
  AllocateUntil(1058, [&](void* alloc) -> NextSteps {
    if (IsSampledMemory(alloc)) {
      ++sampled_count;
    }
    if (Static::guardedpage_allocator().PointerIsMine(alloc)) {
      ++guarded_count;
    }
    return {guarded_count > 1, true};
  });
  EXPECT_LE(sampled_count, 6);

  // Allocate until 3 guards placed, it should not exceed 13 attempts
  // (1st Guard: 100% (1), 2nd: 25% (4), 3rd: 12.5% (8))
  sampled_count = 0;
  guarded_count = 0;
  AllocateUntil(1059, [&](void* alloc) -> NextSteps {
    if (IsSampledMemory(alloc)) {
      ++sampled_count;
    }
    if (Static::guardedpage_allocator().PointerIsMine(alloc)) {
      ++guarded_count;
    }
    return {guarded_count > 2, true};
  });
  EXPECT_LE(sampled_count, 13);

  // Allocate until 4 guards placed, it should not exceed 141 attempts
  // (1st Guard: 100% (1), 2nd: 25% (4), 3rd: 12.5% (8), 4th: ~1% (128))
  sampled_count = 0;
  guarded_count = 0;
  AllocateUntil(1060, [&](void* alloc) -> NextSteps {
    if (IsSampledMemory(alloc)) {
      ++sampled_count;
    }
    if (Static::guardedpage_allocator().PointerIsMine(alloc)) {
      ++guarded_count;
    }
    return {guarded_count > 3, true};
  });
  EXPECT_LE(sampled_count, 141);
}

}  // namespace
}  // namespace tcmalloc_internal
}  // namespace tcmalloc
