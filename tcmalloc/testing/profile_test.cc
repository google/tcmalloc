// Copyright 2021 The TCMalloc Authors
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
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "tcmalloc/internal/profile.pb.h"
#include "benchmark/benchmark.h"
#include "gtest/gtest.h"
#include "absl/base/optimization.h"
#include "absl/container/btree_set.h"
#include "absl/container/flat_hash_map.h"
#include "absl/container/flat_hash_set.h"
#include "absl/status/statusor.h"
#include "absl/strings/match.h"
#include "absl/strings/string_view.h"
#include "absl/synchronization/blocking_counter.h"
#include "absl/time/clock.h"
#include "absl/time/time.h"
#include "google/protobuf/io/coded_stream.h"
#include "google/protobuf/io/gzip_stream.h"
#include "google/protobuf/io/zero_copy_stream_impl_lite.h"
#include "tcmalloc/internal/linked_list.h"
#include "tcmalloc/malloc_extension.h"
#include "tcmalloc/profile_marshaler.h"
#include "tcmalloc/testing/testutil.h"

namespace tcmalloc::tcmalloc_internal {
namespace {

TEST(AllocationSampleTest, TokenAbuse) {
  auto token = MallocExtension::StartAllocationProfiling();
  void* ptr = ::operator new(512 * 1024 * 1024);
  // TODO(b/183453911): Remove workaround for GCC 10.x deleting operator new,
  // https://gcc.gnu.org/bugzilla/show_bug.cgi?id=94295.
  benchmark::DoNotOptimize(ptr);
  ::operator delete(ptr);
  // Repeated Claims should happily return null.
  auto profile = std::move(token).Stop();
  int count = 0;
  profile.Iterate([&](const Profile::Sample&) { count++; });

  if (!kSanitizerPresent) {
    EXPECT_EQ(count, 1);
  }

  auto profile2 = std::move(token).Stop();  // NOLINT: use-after-move intended
  int count2 = 0;
  profile2.Iterate([&](const Profile::Sample&) { count2++; });
  EXPECT_EQ(count2, 0);

  // Delete (on the scope ending) without Claim should also be OK.
  {
    (void)MallocExtension::StartAllocationProfiling();
  }
}

// Verify that profiling sessions concurrent with allocations do not crash due
// to mutating pointers accessed by the sampling code (b/143623146).
TEST(AllocationSampleTest, RaceToClaim) {
  MallocExtension::SetProfileSamplingInterval(1 << 14);

  absl::BlockingCounter counter(2);
  std::atomic<bool> stop{false};

  std::thread t1([&]() {
    counter.DecrementCount();

    while (!stop) {
      auto token = MallocExtension::StartAllocationProfiling();
      absl::SleepFor(absl::Microseconds(1));
      auto profile = std::move(token).Stop();
    }
  });

  std::thread t2([&]() {
    counter.DecrementCount();

    const int kNum = 1000000;
    std::vector<void*> ptrs;
    while (!stop) {
      for (int i = 0; i < kNum; i++) {
        ptrs.push_back(::operator new(1));
      }
      for (void* p : ptrs) {
        sized_delete(p, 1);
      }
      ptrs.clear();
    }
  });

  // Verify the threads are up and running before we start the clock.
  counter.Wait();

  absl::SleepFor(absl::Milliseconds(50));

  stop.store(true);

  t1.join();
  t2.join();
}

// Similar to the AllocationSampleTest but for DeallocationSample which uses a
// similar API susceptible to the same race condition. It should be possible to
// combine these to into a single test if the deallocation profiler is
// refactored to use a template-d interface shared with the allocation profiler.
TEST(DeallocationSampleTest, RaceToClaim) {
  MallocExtension::SetProfileSamplingInterval(1 << 14);

  absl::BlockingCounter counter(2);
  std::atomic<bool> stop{false};

  std::thread t1([&]() {
    counter.DecrementCount();

    while (!stop) {
      auto token = MallocExtension::StartLifetimeProfiling();
      absl::SleepFor(absl::Microseconds(1));
      auto profile = std::move(token).Stop();
    }
  });

  std::thread t2([&]() {
    counter.DecrementCount();

    const int kNum = 1000000;
    std::vector<void*> ptrs;
    while (!stop) {
      for (int i = 0; i < kNum; i++) {
        ptrs.push_back(::operator new(1));
      }
      for (void* p : ptrs) {
        sized_delete(p, 1);
      }
      ptrs.clear();
    }
  });

  // Verify the threads are up and running before we start the clock.
  counter.Wait();

  absl::SleepFor(absl::Milliseconds(50));

  stop.store(true);

  t1.join();
  t2.join();
}

TEST(AllocationSampleTest, SampleAccuracy) {
  // Disable GWP-ASan, since it allocates different sizes than normal samples.
  MallocExtension::SetGuardedSamplingInterval(-1);

  // Allocate about 512 MiB each of various sizes. For _some_ but not all
  // sizes, delete it as we go--it shouldn't matter for the sample count.
  static const size_t kTotalPerSize = 512 * 1024 * 1024;

  // (object size, object alignment, keep objects)
  struct Requests {
    size_t size;
    std::optional<std::align_val_t> alignment;
    std::optional<tcmalloc::hot_cold_t> hot_cold;
    bool expected_hot;
    bool keep;
    // objects we don't delete as we go
    void* list = nullptr;
  };
  std::vector<Requests> sizes = {
      {8, std::nullopt, std::nullopt, true, false},
      {16, std::align_val_t{16}, std::nullopt, true, true},
      {1024, std::nullopt, std::nullopt, true, false},
      {64 * 1024, std::align_val_t{64}, std::nullopt, true, false},
      {512 * 1024, std::nullopt, std::nullopt, true, true},
      {1024 * 1024, std::align_val_t{128}, std::nullopt, true, true},
      {32, std::nullopt, tcmalloc::hot_cold_t{0}, false, true},
      {64, std::nullopt, tcmalloc::hot_cold_t{255}, true, true},
      {8192, std::nullopt, tcmalloc::hot_cold_t{0}, false, true},
  };
  absl::btree_set<size_t> sizes_expected;
  for (auto s : sizes) {
    sizes_expected.insert(s.size);
  }
  auto token = MallocExtension::StartAllocationProfiling();

  // We use new/delete to allocate memory, as malloc returns objects aligned to
  // std::max_align_t.
  for (auto& s : sizes) {
    for (size_t bytes = 0; bytes < kTotalPerSize; bytes += s.size) {
      void* obj;
      if (s.alignment.has_value()) {
        obj = operator new(s.size, *s.alignment);
      } else if (s.hot_cold.has_value()) {
        obj = operator new(s.size, *s.hot_cold);
      } else {
        obj = operator new(s.size);
      }
      if (s.keep) {
        tcmalloc_internal::SLL_Push(&s.list, obj);
      } else if (s.alignment.has_value()) {
        operator delete(obj, *s.alignment);
      } else {
        sized_delete(obj, s.size);
      }
    }
  }
  auto profile = std::move(token).Stop();

  // size -> bytes seen
  absl::flat_hash_map<size_t, size_t> m;

  // size -> alignment request
  absl::flat_hash_map<size_t, std::optional<std::align_val_t>> alignment;

  // size -> access_hint
  absl::flat_hash_map<size_t, hot_cold_t> access_hint;

  // size -> access_allocated
  absl::flat_hash_map<size_t, Profile::Sample::Access> access_allocated;

  for (auto s : sizes) {
    alignment[s.size] = s.alignment;
    access_hint[s.size] = s.hot_cold.value_or(hot_cold_t{255});
    access_allocated[s.size] = s.expected_hot ? Profile::Sample::Access::Hot
                                              : Profile::Sample::Access::Cold;
  }

  profile.Iterate([&](const tcmalloc::Profile::Sample& e) {
    // Skip unexpected sizes.  They may have been triggered by a background
    // thread.
    if (sizes_expected.find(e.requested_size) == sizes_expected.end()) {
      return;
    }
    // Also skip if the access hint doesn't match our expectation, which means
    // it's a background thread allocation that happened to match our size.
    if (e.access_hint != access_hint[e.requested_size]) {
      return;
    }

    SCOPED_TRACE(e.requested_size);

    // Don't check stack traces until we have evidence that's broken, it's
    // tedious and done fairly well elsewhere.
    m[e.allocated_size] += e.sum;
    EXPECT_EQ(alignment[e.requested_size], e.requested_alignment);
    EXPECT_EQ(access_hint[e.requested_size], e.access_hint);
    if (access_allocated[e.requested_size] == Profile::Sample::Access::Cold &&
        e.access_allocated == Profile::Sample::Access::Hot) {
      // The allocator may place cold hints into the Hot partition under some
      // circumstances (e.g. heap partitioning disabled or unsupported).
    } else {
      EXPECT_EQ(access_allocated[e.requested_size], e.access_allocated);
    }
  });

  if (!kSanitizerPresent) {
    size_t max_bytes = 0, min_bytes = std::numeric_limits<size_t>::max();
    EXPECT_EQ(m.size(), sizes_expected.size());
    for (auto seen : m) {
      size_t bytes = seen.second;
      min_bytes = std::min(min_bytes, bytes);
      max_bytes = std::max(max_bytes, bytes);
    }
    // Hopefully we're in a fairly small range, that contains our actual
    // allocation.
    EXPECT_GE((min_bytes * 3) / 2, max_bytes);
    EXPECT_LE((min_bytes * 3) / 4, kTotalPerSize);
    EXPECT_LE(kTotalPerSize, (max_bytes * 4) / 3);
  }

  // Remove the objects we left alive
  for (auto& s : sizes) {
    while (s.list != nullptr) {
      void* obj = tcmalloc_internal::SLL_Pop(&s.list);
      if (s.alignment.has_value()) {
        operator delete(obj, *s.alignment);
      } else {
        operator delete(obj);
      }
    }
  }
}

TEST(FragmentationzTest, Accuracy) {
  // Increase sampling rate to decrease flakiness.
  ScopedProfileSamplingInterval ps(512 * 1024);
  // Disable GWP-ASan, since it allocates different sizes than normal samples.
  ScopedGuardedSamplingInterval gs(-1);

  // a fairly odd allocation size - will be rounded to 128.  This lets
  // us find our record in the table.
  static const size_t kItemSize = 115;
  // allocate about 3.5 GiB:
  static const size_t kNumItems = 32 * 1024 * 1024;

  std::vector<std::unique_ptr<char[]>> keep;
  std::vector<std::unique_ptr<char[]>> drop;
  // hint expected sizes:
  drop.reserve(kNumItems * 8 / 10);
  keep.reserve(kNumItems * 2 / 10);

  // We allocate many items, then free 80% of them "randomly". (To
  // decrease noise and speed up, we just keep every 5th one exactly.)
  for (int i = 0; i < kNumItems; ++i) {
    // Ideally we should use a malloc() here, for consistency; but unique_ptr
    // doesn't come with a have a "free()" deleter; use ::operator new instead.
    (i % 5 == 0 ? keep : drop)
        .push_back(std::unique_ptr<char[]>(
            static_cast<char*>(::operator new[](kItemSize))));
  }
  drop.resize(0);

  // there are at least 64 items per span here. (8/10)^64 = 6.2e-7 ~= 0
  // probability we actually managed to free a page; every page is fragmented.
  // We still have 20% or so of it allocated, so we should see 80% of it
  // charged to these allocations as fragmentations.
  auto profile = MallocExtension::SnapshotCurrent(ProfileType::kFragmentation);

  // Pull out the fragmentationz entry corresponding to this
  size_t requested_size = 0;
  size_t allocated_size = 0;
  size_t sum = 0;
  size_t count = 0;
  profile.Iterate([&](const Profile::Sample& e) {
    if (e.requested_size != kItemSize) return;

    if (requested_size == 0) {
      allocated_size = e.allocated_size;
      requested_size = e.requested_size;
    } else {
      // we will usually have single entry in
      // profile, but in builds without optimization
      // our fast-path code causes same call-site to
      // have two different stack traces. Thus we
      // expect and deal with second entry for same
      // allocation.
      EXPECT_EQ(requested_size, e.requested_size);
      EXPECT_EQ(allocated_size, e.allocated_size);
    }
    sum += e.sum;
    count += e.count;
  });

  double frag_bytes = sum;
  double real_frag_bytes =
      static_cast<double>(allocated_size * kNumItems) * 0.8;
  // We should be pretty close with this much data.
  EXPECT_NEAR(real_frag_bytes, frag_bytes, real_frag_bytes * 0.15)
      << " sum = " << sum << " allocated = " << allocated_size
      << " requested = " << requested_size << " count = " << count;
}

extern "C" {
void* __alloc_token_0__ZnwmRKSt9nothrow_t(size_t size, std::nothrow_t);
void* __alloc_token_1__ZnwmRKSt9nothrow_t(size_t size, std::nothrow_t);
}

TEST(ProfileTest, HeapProfile) {
#if ABSL_HAVE_ADDRESS_SANITIZER || ABSL_HAVE_HWADDRESS_SANITIZER || \
    ABSL_HAVE_MEMORY_SANITIZER || ABSL_HAVE_THREAD_SANITIZER ||     \
    defined(__SANITIZE_ALLOC_TOKEN__)
  GTEST_SKIP() << "Skipping heap profile test under sanitizers.";
#endif

  constexpr int64_t kSamplingInterval = 1024 * 1024;
  ScopedProfileSamplingInterval s(kSamplingInterval);

  // Tweak alloc_size to make it more likely we can distinguish it from others.
  constexpr int kAllocs = 32;
  const size_t alloc_size = 64 * kSamplingInterval + 123;

  auto deleter = [](void* ptr) { ::operator delete(ptr); };
  std::vector<std::unique_ptr<void, decltype(deleter)>> allocs;
  // Sometimes the compiler duplicates the tcmalloc call depending on if the
  // fast path of the first `emplace_back` is taken. We reserve enough space for
  // all insertions so that all `emplace_back` calls go through the fast path
  // and there is only one stack trace for tcmalloc.
  allocs.reserve(5 * kAllocs);
  for (int i = 0; i < kAllocs; i++) {
    allocs.emplace_back(::operator new(alloc_size), deleter);
    allocs.emplace_back(::operator new(alloc_size, std::nothrow), deleter);
    allocs.emplace_back(
        __alloc_token_0__ZnwmRKSt9nothrow_t(alloc_size, std::nothrow), deleter);
    allocs.emplace_back(
        __alloc_token_1__ZnwmRKSt9nothrow_t(alloc_size, std::nothrow), deleter);
    allocs.emplace_back(__size_returning_new(alloc_size).p, deleter);
  }

  auto malloc_deleter = [](void* ptr) { free(ptr); };
  std::vector<std::unique_ptr<void, decltype(malloc_deleter)>> mallocs;
  mallocs.reserve(2 * kAllocs);
  for (int i = 0; i < kAllocs; i++) {
    mallocs.emplace_back(malloc(alloc_size), malloc_deleter);
    mallocs.emplace_back(aligned_alloc(ABSL_CACHELINE_SIZE, alloc_size),
                         malloc_deleter);
  }

  // Grab profile, encode, then decode to look for the allocations.
  Profile profile = MallocExtension::SnapshotCurrent(ProfileType::kHeap);
  absl::StatusOr<std::string> encoded_or = Marshal(profile);
  ASSERT_TRUE(encoded_or.ok());

  const absl::string_view encoded = *encoded_or;

  google::protobuf::io::ArrayInputStream stream(encoded.data(), encoded.size());
  google::protobuf::io::GzipInputStream gzip_stream(&stream);
  google::protobuf::io::CodedInputStream coded(&gzip_stream);

  perftools::profiles::Profile converted;
  ASSERT_TRUE(converted.ParseFromCodedStream(&coded));

  // Look for "request", "size_returning", "allocation_type", "new", "malloc",
  // "aligned_malloc" strings in string table.
  std::optional<int> request_id, size_returning_id, allocation_type_id, new_id,
      malloc_id, aligned_malloc_id, token_id;
  for (int i = 0, n = converted.string_table().size(); i < n; ++i) {
    if (converted.string_table(i) == "request") {
      request_id = i;
    } else if (converted.string_table(i) == "size_returning") {
      size_returning_id = i;
    } else if (converted.string_table(i) == "allocation type") {
      allocation_type_id = i;
    } else if (converted.string_table(i) == "new") {
      new_id = i;
    } else if (converted.string_table(i) == "malloc") {
      malloc_id = i;
    } else if (converted.string_table(i) == "aligned malloc") {
      aligned_malloc_id = i;
    } else if (converted.string_table(i) == "token_id") {
      token_id = i;
    }
  }

  EXPECT_TRUE(request_id.has_value());
  EXPECT_TRUE(size_returning_id.has_value());
  EXPECT_TRUE(allocation_type_id.has_value());
  EXPECT_TRUE(new_id.has_value());
  EXPECT_TRUE(malloc_id.has_value());
  EXPECT_TRUE(aligned_malloc_id.has_value());
  EXPECT_TRUE(token_id.has_value());

  absl::flat_hash_map<int, int> token_count;
  size_t count = 0, bytes = 0, samples = 0, size_returning_samples = 0,
         new_samples = 0, malloc_samples = 0, aligned_malloc_samples = 0;
  for (const auto& sample : converted.sample()) {
    count += sample.value(0);
    bytes += sample.value(1);

    // Count the number of times we saw an alloc_size-sized allocation.
    bool alloc_sized = false;
    for (const auto& label : sample.label()) {
      if (label.key() == request_id && label.num() == alloc_size) {
        alloc_sized = true;
        samples++;
      }
    }

    if (alloc_sized) {
      for (const auto& label : sample.label()) {
        if (label.key() == size_returning_id && label.num() > 0) {
          size_returning_samples++;
        }
      }

      // Count new versus malloc'd allocations.
      bool type_seen = false;
      for (const auto& label : sample.label()) {
        if (label.key() == allocation_type_id) {
          EXPECT_FALSE(type_seen);
          type_seen = true;

          if (label.str() == new_id) {
            new_samples++;
          } else if (label.str() == malloc_id) {
            malloc_samples++;
          } else if (label.str() == aligned_malloc_id) {
            aligned_malloc_samples++;
          } else {
            GTEST_FAIL() << "Unexpected string key: "
                         << converted.string_table(label.str()) << " ("
                         << label.str() << ")";
          }
        } else if (label.key() == token_id) {
          token_count[label.num()]++;
        }
      }
    }
  }

  EXPECT_GT(count, 0);
  EXPECT_GE(bytes, 2 * alloc_size * kAllocs);
  // To minimize the size of profiles, we expect to coalesce similar allocations
  // (same call stack, size, alignment, etc.) during generation of the
  // profile.proto.  Since all of the calls to operator new(alloc_size) are
  // similar in these dimensions, we expect to see only 2 samples, one for
  // ::operator new and one for __size_returning_new.
  EXPECT_EQ(samples, 7);
  EXPECT_EQ(size_returning_samples, 1);
  EXPECT_EQ(new_samples, 5);
  EXPECT_EQ(malloc_samples, 1);
  EXPECT_EQ(aligned_malloc_samples, 1);

  EXPECT_EQ(token_count[static_cast<int>(TokenId::kAllocToken0)], 1);
  EXPECT_EQ(token_count[static_cast<int>(TokenId::kAllocToken1)], 1);
  EXPECT_EQ(token_count[static_cast<int>(TokenId::kNoAllocToken)], 5);

  // Dump the profile in case of failures so that it's possible to debug.
  // Since SCOPED_TRACE attaches output to every failure, we use ASSERTs below.
  SCOPED_TRACE(converted.DebugString());

  absl::flat_hash_map<int, const perftools::profiles::Mapping*> mappings;
  mappings.reserve(converted.mapping().size());
  for (const auto& mapping : converted.mapping()) {
    ASSERT_NE(mapping.id(), 0);
    ASSERT_TRUE(mappings.insert({mapping.id(), &mapping}).second);
  }

  absl::flat_hash_map<int, const perftools::profiles::Location*> locations;
  for (const auto& location : converted.location()) {
    ASSERT_NE(location.id(), 0);
    ASSERT_TRUE(locations.insert({location.id(), &location}).second);
  }

  // We can't unwind past optimized libstdc++.so, and as the result have some
  // bogus frames (random numbers), which don't have a mapping.
  absl::flat_hash_set<int> unreliable_locations;
  for (const auto& sample : converted.sample()) {
    bool unreliable = false;
    for (auto loc_id : sample.location_id()) {
      if (unreliable) {
        unreliable_locations.insert(loc_id);
        continue;
      }
      const auto* loc = locations[loc_id];
      ASSERT_NE(loc, nullptr);
      const auto* mapping = mappings[loc->mapping_id()];
      ASSERT_NE(mapping, nullptr);
      ASSERT_LT(mapping->filename(), converted.string_table().size());
      const auto& file = converted.string_table()[mapping->filename()];
      unreliable = absl::StrContains(file, "libstdc++.so");
    }
  }

  // Every reliable location should have a mapping.
  for (const auto& location : converted.location()) {
    if (unreliable_locations.contains(location.id())) {
      continue;
    }
    const int mapping_id = location.mapping_id();
    ASSERT_TRUE(mappings.contains(mapping_id)) << mapping_id;
  }
}

}  // namespace
}  // namespace tcmalloc::tcmalloc_internal
