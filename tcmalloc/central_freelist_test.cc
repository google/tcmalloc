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

#include "tcmalloc/central_freelist.h"

#include <math.h>
#include <stddef.h>
#include <stdint.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

#include "benchmark/benchmark.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "absl/algorithm/container.h"
#include "absl/base/internal/spinlock.h"
#include "absl/base/thread_annotations.h"
#include "absl/container/fixed_array.h"
#include "absl/container/flat_hash_map.h"
#include "absl/container/flat_hash_set.h"
#include "absl/memory/memory.h"
#include "absl/numeric/bits.h"
#include "absl/random/random.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_format.h"
#include "absl/strings/str_join.h"
#include "absl/synchronization/mutex.h"
#include "absl/time/clock.h"
#include "absl/time/time.h"
#include "absl/types/span.h"
#include "tcmalloc/common.h"
#include "tcmalloc/internal/logging.h"
#include "tcmalloc/internal/size_class_info.h"
#include "tcmalloc/mock_static_forwarder.h"
#include "tcmalloc/pagemap.h"
#include "tcmalloc/pages.h"
#include "tcmalloc/sizemap.h"
#include "tcmalloc/span.h"
#include "tcmalloc/span_stats.h"
#include "tcmalloc/static_vars.h"
#include "tcmalloc/stats.h"
#include "tcmalloc/testing/testutil.h"
#include "tcmalloc/testing/thread_manager.h"

namespace tcmalloc {
namespace tcmalloc_internal {
namespace central_freelist_internal {

class CentralFreeListTestPeer {
 public:
  template <typename Forwarder>
  using CFL = CentralFreeList<Forwarder>;

  template <typename Forwarder>
  static size_t num_same_spans(const CentralFreeList<Forwarder>& cfl,
                               size_t index) {
#ifndef TCMALLOC_INTERNAL_LEGACY_LOCKING
    return cfl.num_same_spans_[absl::bit_width(index)].value();
#else
    return 0;
#endif
  }

  static void VerifyLegacyLayout() {
#ifdef TCMALLOC_INTERNAL_LEGACY_LOCKING
    using CFLType = CFL<Forwarder>;
    EXPECT_EQ(offsetof(CFLType, lock_), 0);
    EXPECT_EQ(offsetof(CFLType, size_class_), 8);
    EXPECT_EQ(offsetof(CFLType, object_size_), 16);
    EXPECT_EQ(offsetof(CFLType, objects_per_span_), 24);
    EXPECT_EQ(offsetof(CFLType, size_reciprocal_), 32);
    EXPECT_EQ(offsetof(CFLType, first_nonempty_index_), 40);
    EXPECT_EQ(offsetof(CFLType, pages_per_span_), 48);
    EXPECT_EQ(offsetof(CFLType, completed_spans_), 56);
    EXPECT_EQ(offsetof(CFLType, span_allocations_tracker_), 120);
    EXPECT_EQ(offsetof(CFLType, counter_), 184);
    EXPECT_EQ(offsetof(CFLType, num_spans_requested_), 192);
    EXPECT_EQ(offsetof(CFLType, num_spans_returned_), 200);
    EXPECT_EQ(offsetof(CFLType, objects_to_spans_), 208);
    EXPECT_EQ(offsetof(CFLType, nonempty_), 336);
#ifdef NDEBUG
    EXPECT_EQ(sizeof(((CFLType*)0)->nonempty_), 144);
    EXPECT_EQ(offsetof(CFLType, use_all_buckets_for_few_object_spans_), 480);
#else
    EXPECT_EQ(sizeof(((CFLType*)0)->nonempty_), 208);
    EXPECT_EQ(offsetof(CFLType, use_all_buckets_for_few_object_spans_), 544);
#endif
#endif
  }
};

TEST(CentralFreeListLayoutTest, LegacyOffsets) {
#ifdef TCMALLOC_INTERNAL_LEGACY_LOCKING
  CentralFreeListTestPeer::VerifyLegacyLayout();
#else
  GTEST_SKIP() << "Test only applies under TCMALLOC_INTERNAL_LEGACY_LOCKING";
#endif
}

}  // namespace central_freelist_internal

namespace {

using central_freelist_internal::kNumLists;
using TypeParam = FakeCentralFreeListEnvironment<
    central_freelist_internal::CentralFreeList<MockStaticForwarder>>;
using CentralFreeListTest = ::testing::TestWithParam<std::tuple<
    SizeClassInfo, central_freelist_internal::CflSubbucketPrioritization>>;

TEST_P(CentralFreeListTest, IsolatedSmoke) {
#if ABSL_HAVE_HWADDRESS_SANITIZER
  GTEST_SKIP()
      << "Skipping under HWASan, which uses the top bits of the pointer.";
#endif

  TypeParam e(std::get<0>(GetParam()).size, std::get<0>(GetParam()).bytes,
              std::get<0>(GetParam()).num_to_move, std::get<1>(GetParam()));
  EXPECT_CALL(e.forwarder(), AllocateSpan).Times(1);

  absl::FixedArray<void*> batch(e.batch_size());
  int allocated = e.central_freelist().RemoveRange(
      absl::MakeSpan(&batch[0], e.batch_size()));
  ASSERT_GT(allocated, 0);
  EXPECT_LE(allocated, e.batch_size());

  // We should observe span's utilization captured in the histogram. The number
  // of spans in rest of the buckets should be zero.
  const int bitwidth = absl::bit_width(static_cast<unsigned>(allocated));
  for (int i = 1; i <= absl::bit_width(e.objects_per_span()); ++i) {
    // Skip the check for objects_per_span = 1 since such spans skip most of the
    // central freelist's logic.
    if (i == bitwidth && e.objects_per_span() != 1) {
      EXPECT_EQ(e.central_freelist().NumSpansWith(i), 1);
    } else {
      EXPECT_EQ(e.central_freelist().NumSpansWith(i), 0);
    }
  }

  EXPECT_CALL(e.forwarder(), MapObjectsToSpans).Times(1);
  EXPECT_CALL(e.forwarder(), DeallocateSpans).Times(1);

  // Skip the check for objects_per_span = 1 since such spans skip most of the
  // central freelist's logic.
  SpanStats stats = e.central_freelist().GetSpanStats();
  if (e.objects_per_span() != 1) {
    EXPECT_EQ(stats.num_spans_requested, 1);
    EXPECT_EQ(stats.num_spans_returned, 0);
    EXPECT_EQ(stats.obj_capacity, e.objects_per_span());
  }

  e.central_freelist().InsertRange(absl::MakeSpan(&batch[0], allocated));
  // Skip the check for objects_per_span = 1 since such spans skip most of the
  // central freelist's logic.
  if (e.objects_per_span() != 1) {
    SpanStats stats = e.central_freelist().GetSpanStats();
    EXPECT_EQ(stats.num_spans_requested, 1);
    EXPECT_EQ(stats.num_spans_returned, 1);
    EXPECT_EQ(stats.obj_capacity, 0);
  }

  // Span captured in the histogram with the earlier utilization should have
  // been removed.
  for (int i = 1; i <= absl::bit_width(e.objects_per_span()); ++i) {
    EXPECT_EQ(e.central_freelist().NumSpansWith(i), 0);
  }
}

TEST_P(CentralFreeListTest, SameSpanTracking) {
#if ABSL_HAVE_HWADDRESS_SANITIZER
  GTEST_SKIP()
      << "Skipping under HWASan, which uses the top bits of the pointer.";
#endif

  TypeParam e(std::get<0>(GetParam()).size, std::get<0>(GetParam()).bytes,
              std::get<0>(GetParam()).num_to_move, std::get<1>(GetParam()));
  if (e.objects_per_span() <= 1) {
    GTEST_SKIP() << "Single-object spans skip CentralFreeList InsertRange";
  }

  EXPECT_CALL(e.forwarder(), AllocateSpan).Times(1);

  absl::FixedArray<void*> batch(e.batch_size());
  int allocated = e.central_freelist().RemoveRange(
      absl::MakeSpan(&batch[0], e.batch_size()));
  ASSERT_GT(allocated, 0);

  EXPECT_CALL(e.forwarder(), MapObjectsToSpans).Times(1);
  EXPECT_CALL(e.forwarder(), DeallocateSpans).Times(testing::AtLeast(0));

  e.central_freelist().InsertRange(absl::MakeSpan(&batch[0], allocated));

#ifndef TCMALLOC_INTERNAL_LEGACY_LOCKING
  const int expected_same_span = allocated - 1;
  EXPECT_GE(central_freelist_internal::CentralFreeListTestPeer::num_same_spans(
                e.central_freelist(), expected_same_span),
            1);
#endif
}

TEST_P(CentralFreeListTest, SpanUtilizationHistogram) {
#if ABSL_HAVE_HWADDRESS_SANITIZER
  GTEST_SKIP()
      << "Skipping under HWASan, which uses the top bits of the pointer.";
#endif

  TypeParam e(std::get<0>(GetParam()).size, std::get<0>(GetParam()).bytes,
              std::get<0>(GetParam()).num_to_move, std::get<1>(GetParam()));
  constexpr size_t kNumSpans = 10;

  // Request kNumSpans spans.
  void* batch[kMaxObjectsToMove];
  const int num_objects_to_fetch = kNumSpans * e.objects_per_span();
  int total_fetched = 0;
  // Tracks object and corresponding span from which it was allocated.
  std::vector<std::pair<void*, Span*>> object_to_span;
  // Tracks number of objects allocated per span.
  absl::flat_hash_map<Span*, size_t> allocated_per_span;
  int span_idx = 0;

  while (total_fetched < num_objects_to_fetch) {
    size_t n = num_objects_to_fetch - total_fetched;
    int got = e.central_freelist().RemoveRange(
        absl::MakeSpan(batch, std::min(n, e.batch_size())));
    total_fetched += got;

    // Increment span_idx if current objects have been fetched from the new
    // span.
    if (total_fetched > (span_idx + 1) * e.objects_per_span()) {
      ++span_idx;
    }
    // Record fetched object and the associated span.
    for (int i = 0; i < got; ++i) {
      Span* s = e.forwarder().MapObjectToSpan(batch[i]);
      object_to_span.emplace_back(batch[i], s);
      allocated_per_span[s] += 1;
    }
    TC_ASSERT_LT(span_idx, kNumSpans);
  }

  // Make sure that we have fetched exactly from kNumSpans spans.
  EXPECT_EQ(span_idx + 1, kNumSpans);

  // We should have kNumSpans spans in the histogram with number of allocated
  // objects equal to e.objects_per_span() (i.e. in the last bucket).
  // Rest of the buckets should be empty.
  const int expected_bitwidth = absl::bit_width(e.objects_per_span());
  // Skip the check when objects_per_span = 1 as those spans skip most of the
  // central freelist's logic.
  if (e.objects_per_span() != 1) {
    EXPECT_EQ(e.central_freelist().NumSpansWith(expected_bitwidth), kNumSpans);
  }
  for (int i = 1; i < expected_bitwidth; ++i) {
    EXPECT_EQ(e.central_freelist().NumSpansWith(i), 0);
  }

  // Shuffle.
  absl::BitGen rng;
  std::shuffle(object_to_span.begin(), object_to_span.end(), rng);

  // Return objects, a fraction at a time, each time checking that histogram is
  // correct.
  int total_returned = 0;
  const int last_bucket = absl::bit_width(e.objects_per_span()) - 1;
  while (total_returned < num_objects_to_fetch) {
    uint64_t size_to_pop =
        std::min(object_to_span.size() - total_returned, e.batch_size());

    for (int i = 0; i < size_to_pop; ++i) {
      const auto [ptr, span] = object_to_span[i + total_returned];
      batch[i] = ptr;
      --allocated_per_span[span];
    }
    total_returned += size_to_pop;
    e.central_freelist().InsertRange({batch, size_to_pop});

    // Calculate expected histogram.
    std::vector<size_t> expected(absl::bit_width(e.objects_per_span()), 0);
    for (const auto& span_and_count : allocated_per_span) {
      // If span has non-zero allocated objects, include it in the histogram.
      if (span_and_count.second > 0) {
        const size_t bucket = absl::bit_width(span_and_count.second) - 1;
        TC_ASSERT_LE(bucket, last_bucket);
        ++expected[bucket];
      }
    }

    // Fetch number of spans logged in the histogram and compare it with the
    // expected histogram that we calculated using the tracked allocated
    // objects per span.
    for (int i = 1; i <= last_bucket; ++i) {
      EXPECT_EQ(e.central_freelist().NumSpansWith(i), expected[i - 1]);
    }
  }

  // Since no span is live here, histogram must be empty.
  for (int i = 1; i <= last_bucket; ++i) {
    EXPECT_EQ(e.central_freelist().NumSpansWith(i), 0);
  }
}

// Confirms that a call to RemoveRange returns at most kObjectsPerSpan objects
// in cases when there are no non-empty spans in the central freelist. This
// makes sure that we populate, and subsequently allocate from a single span.
// This avoids memory regression due to multiple Populate calls observed in
// b/225880278.
TEST_P(CentralFreeListTest, SinglePopulate) {
#if ABSL_HAVE_HWADDRESS_SANITIZER
  GTEST_SKIP()
      << "Skipping under HWASan, which uses the top bits of the pointer.";
#endif

  // Make sure that we allocate up to kObjectsPerSpan objects in both the span
  // prioritization states.
  TypeParam e(std::get<0>(GetParam()).size, std::get<0>(GetParam()).bytes,
              std::get<0>(GetParam()).num_to_move, std::get<1>(GetParam()));
  // Try to fetch sufficiently large number of objects at startup.
  const int num_objects_to_fetch = kMaxObjectsToMove;
  std::vector<void*> objects(num_objects_to_fetch, nullptr);
  const size_t got = e.central_freelist().RemoveRange(
      absl::MakeSpan(objects.data(), num_objects_to_fetch));
  // Confirm we allocated at most kObjectsPerSpan number of objects.
  EXPECT_GT(got, 0);
  EXPECT_LE(got, e.objects_per_span());
  size_t returned = 0;
  while (returned < got) {
    const size_t to_return = std::min(got - returned, e.batch_size());
    e.central_freelist().InsertRange({&objects[returned], to_return});
    returned += to_return;
  }
}

// Tests whether the index generated by the input indexing function matches the
// index of the span on which allocations and deallocation operations are
// carried out.  The test first allocates objects and deallocates them.  After
// each operation, the actual index is matched against the expected one.
template <typename IndexingFunc>
void TestIndexing(TypeParam& e, IndexingFunc f) {
  TC_ASSERT_GT(kNumLists, 0);
  const int num_objects_to_fetch = e.objects_per_span();
  std::vector<void*> objects(num_objects_to_fetch);
  size_t fetched = 0;
  int expected_idx = kNumLists - 1;

  // Fetch one object at a time from a span and confirm that the span is moved
  // through the nonempty_ lists as we allocate more objects from it.
  while (fetched < num_objects_to_fetch) {
    // Try to fetch one object from the span.
    int got =
        e.central_freelist().RemoveRange(absl::MakeSpan(&objects[fetched], 1));
    fetched += got;
    TC_ASSERT(fetched);
    if (fetched % num_objects_to_fetch == 0) {
      // Span should have been removed from nonempty_ lists because we have
      // allocated all the objects from it.
      EXPECT_EQ(e.central_freelist().NumSpansInList(expected_idx), 0);
    } else {
      expected_idx = f(fetched);
      TC_ASSERT_GE(expected_idx, 0);
      TC_ASSERT_LT(expected_idx, kNumLists);
      // Check that the span exists in the corresponding nonempty_ list.
      EXPECT_EQ(e.central_freelist().NumSpansInList(expected_idx), 1);
    }
  }

  // Similar to our previous test, we now make sure that the span is moved
  // through the nonempty_ lists when we deallocate objects back to it.
  size_t remaining = fetched;
  while (--remaining > 0) {
    // Return objects back to the span one at a time.
    e.central_freelist().InsertRange({&objects[remaining], 1});
    TC_ASSERT(remaining);
    // When allocated objects are more than the threshold, the span is indexed
    // to nonempty_ list 0.
    expected_idx = f(remaining);
    EXPECT_LT(expected_idx, kNumLists);
    EXPECT_EQ(e.central_freelist().NumSpansInList(expected_idx), 1);
  }

  // When the last object is returned, we release the span to the page heap. So,
  // nonempty_[0] should also be empty.
  e.central_freelist().InsertRange({&objects[remaining], 1});
  EXPECT_EQ(e.central_freelist().NumSpansInList(0), 0);
}

TEST_P(CentralFreeListTest, BitwidthIndexedNonEmptyLists) {
#if ABSL_HAVE_HWADDRESS_SANITIZER
  GTEST_SKIP()
      << "Skipping under HWASan, which uses the top bits of the pointer.";
#endif

  TypeParam e(std::get<0>(GetParam()).size, std::get<0>(GetParam()).bytes,
              std::get<0>(GetParam()).num_to_move, std::get<1>(GetParam()));
  if (e.objects_per_span() <= 2 * kNumLists) {
    GTEST_SKIP()
        << "Skipping test as one hot encoding used for few object spans.";
  }
  auto bitwidth_indexing = [](size_t allocated) {
    size_t bitwidth = absl::bit_width(allocated);
    return kNumLists - std::min(bitwidth, kNumLists);
  };
  TestIndexing(e, bitwidth_indexing);
}

TEST_P(CentralFreeListTest, DirectIndexedEncodedNonEmptyLists) {
#if ABSL_HAVE_HWADDRESS_SANITIZER
  GTEST_SKIP()
      << "Skipping under HWASan, which uses the top bits of the pointer.";
#endif

  TypeParam e(std::get<0>(GetParam()).size, std::get<0>(GetParam()).bytes,
              std::get<0>(GetParam()).num_to_move, std::get<1>(GetParam()));
  if (e.objects_per_span() > 2 * kNumLists) {
    GTEST_SKIP() << "Skipping test as one hot encoding not required.";
  }
  auto direct_indexing = [](int allocated) -> size_t {
    if (allocated <= kNumLists) return kNumLists - allocated;
    return 0;
  };
  TestIndexing(e, direct_indexing);
}

// Checks if we are indexing a span in the nonempty_ lists as expected. We also
// check if the spans are correctly being prioritized. That is, we create a
// scenario where we have two live spans, and one span has more allocated
// objects than the other span. On subsequent allocations, we confirm that the
// objects are allocated from the span with a higher number of allocated objects
// as enforced by our prioritization scheme.
TEST_P(CentralFreeListTest, SpanPriority) {
#if ABSL_HAVE_HWADDRESS_SANITIZER
  GTEST_SKIP()
      << "Skipping under HWASan, which uses the top bits of the pointer.";
#endif

  TypeParam e(std::get<0>(GetParam()).size, std::get<0>(GetParam()).bytes,
              std::get<0>(GetParam()).num_to_move, std::get<1>(GetParam()));

  // If the number of objects per span is less than 2, we do not use more than
  // one nonempty_ lists. So, we can not prioritize the spans based on how many
  // objects were allocated from them.
  const int objects_per_span = e.objects_per_span();
  if (objects_per_span < 3 || kNumLists < 2) return;

  constexpr int kNumSpans = 2;
  // Track objects allocated per span.
  absl::FixedArray<std::vector<void*>> objects(kNumSpans);
  void* batch[kMaxObjectsToMove];

  const size_t to_fetch = objects_per_span;
  // Allocate all objects from kNumSpans.
  for (int span = 0; span < kNumSpans; ++span) {
    size_t fetched = 0;
    while (fetched < to_fetch) {
      const size_t n = to_fetch - fetched;
      int got = e.central_freelist().RemoveRange(
          absl::MakeSpan(batch, std::min(n, e.batch_size())));
      for (int i = 0; i < got; ++i) {
        objects[span].push_back(batch[i]);
      }
      fetched += got;
    }
  }

  // Perform deallocations so that each span contains only two objects.
  size_t to_release = to_fetch - 2;
  for (int span = 0; span < kNumSpans; ++span) {
    size_t released = 0;
    while (released < to_release) {
      uint64_t n = std::min(to_release - released, e.batch_size());
      for (int i = 0; i < n; ++i) {
        batch[i] = objects[span][i + released];
      }
      released += n;
      e.central_freelist().InsertRange({batch, n});
    }
    objects[span].erase(objects[span].begin(),
                        objects[span].begin() + released);
  }

  // Make sure we have kNumSpans in the expected second-last nonempty_ list.
  EXPECT_EQ(e.central_freelist().NumSpansInList(kNumLists - 2), kNumSpans);

  // Check intra-bucket ordering: span 0 entered the bucket first, span 1
  // second. Prepend (LIFO) should draw from span 1, while append (FIFO) should
  // draw from span 0.
  {
    int got = e.central_freelist().RemoveRange(absl::MakeSpan(batch, 1));
    EXPECT_EQ(got, 1);
    Span* drawn_span = e.forwarder().MapObjectToSpan(batch[0]);
    Span* span0 = e.forwarder().MapObjectToSpan(objects[0][0]);
    Span* span1 = e.forwarder().MapObjectToSpan(objects[1][0]);

    if (std::get<1>(GetParam()) ==
        central_freelist_internal::CflSubbucketPrioritization::kDisabled) {
      EXPECT_EQ(drawn_span, span1);
    } else {
      EXPECT_EQ(drawn_span, span0);
    }
    // Return the drawn object so both spans remain with two allocated objects.
    e.central_freelist().InsertRange({batch, 1});
  }

  // Release an additional object from all but one spans so that they are
  // deprioritized for subsequent allocations.
  to_release = 1;
  for (int span = 1; span < kNumSpans; ++span) {
    size_t released = 0;
    while (released < to_release) {
      uint64_t n = std::min(to_release - released, e.batch_size());
      for (int i = 0; i < n; ++i) {
        batch[i] = objects[span][i + released];
      }
      released += n;
      e.central_freelist().InsertRange({batch, n});
    }
    objects[span].erase(objects[span].begin(),
                        objects[span].begin() + released);
  }

  // Make sure we have kNumSpans-1 spans in the last nonempty_ list and just one
  // span in the second-last list.
  EXPECT_EQ(e.central_freelist().NumSpansInList(kNumLists - 1), kNumSpans - 1);
  EXPECT_EQ(e.central_freelist().NumSpansInList(kNumLists - 2), 1);

  // Allocate one object to ensure that it is being allocated from the span with
  // the highest number of allocated objects.
  int got = e.central_freelist().RemoveRange(absl::MakeSpan(batch, 1));
  EXPECT_EQ(got, 1);
  // Number of spans in the last nonempty_ list should be unchanged (i.e.
  // kNumSpans-1).
  EXPECT_EQ(e.central_freelist().NumSpansInList(kNumLists - 1), kNumSpans - 1);
  if (e.objects_per_span() == 3) {
    // Since we allocated another object from the span that had two objects
    // allocated from it, so the span would no longer be there in the span list.
    for (int i = kNumLists - 2; i >= 0; --i) {
      EXPECT_EQ(e.central_freelist().NumSpansInList(i), 0);
    }
  } else if (e.objects_per_span() <= 2 * kNumLists) {
    // We should have only one span in the third-last nonempty_ list; this is
    // the span from which we should have allocated the last object.
    EXPECT_EQ(e.central_freelist().NumSpansInList(kNumLists - 3), 1);
  } else {
    // We should have only one span in the second-last nonempty_ list; this is
    // the span from which we should have allocated the last object.
    EXPECT_EQ(e.central_freelist().NumSpansInList(kNumLists - 2), 1);
  }
  // Return previously allocated object.
  e.central_freelist().InsertRange({batch, 1});

  // Return rest of the objects.
  for (int span = 0; span < kNumSpans; ++span) {
    for (size_t i = 0; i < objects[span].size(); ++i) {
      e.central_freelist().InsertRange({&objects[span][i], 1});
    }
  }
}

// Verifies that returning an object to a span via InsertRange does not alter
// its intra-bucket priority if the span remains in the same nonempty_ bucket.
TEST_P(CentralFreeListTest, InsertRangeSameBucketDoesNotChangePriority) {
#if ABSL_HAVE_HWADDRESS_SANITIZER
  GTEST_SKIP()
      << "Skipping under HWASan, which uses the top bits of the pointer.";
#endif

  TypeParam e(std::get<0>(GetParam()).size, std::get<0>(GetParam()).bytes,
              std::get<0>(GetParam()).num_to_move, std::get<1>(GetParam()));

  const int objects_per_span = e.objects_per_span();
  if (objects_per_span < 3 || kNumLists < 2) return;

  auto index_for = [&](size_t allocated) -> size_t {
    if (objects_per_span <= 2 * kNumLists) {
      if (allocated <= kNumLists) return kNumLists - allocated;
      return 0;
    }
    size_t bitwidth = absl::bit_width(allocated);
    return kNumLists - std::min(bitwidth, kNumLists);
  };

  // Find an allocated count where releasing an object keeps the span in the
  // same bucket.
  size_t target_allocated = 0;
  for (size_t a = objects_per_span - 1; a >= 2; --a) {
    if (index_for(a) == index_for(a - 1)) {
      target_allocated = a;
      break;
    }
  }

  if (target_allocated == 0) {
    GTEST_SKIP() << "Skipping test as no two allocated counts share a bucket.";
  }

  constexpr int kNumSpans = 2;
  void* batch[kMaxObjectsToMove];

  // Helper lambda to test that releasing an object to a span in the same bucket
  // preserves intra-bucket priority.
  auto test_release_keeps_priority = [&](bool release_to_front) {
    absl::FixedArray<std::vector<void*>> objects(kNumSpans);

    // Step 1: Allocate all objects from kNumSpans so both spans are empty.
    const size_t to_fetch = objects_per_span;
    for (int span = 0; span < kNumSpans; ++span) {
      size_t fetched = 0;
      while (fetched < to_fetch) {
        const size_t n = to_fetch - fetched;
        int got = e.central_freelist().RemoveRange(
            absl::MakeSpan(batch, std::min(n, e.batch_size())));
        for (int i = 0; i < got; ++i) {
          objects[span].push_back(batch[i]);
        }
        fetched += got;
      }
    }

    // Step 2: Release objects so each span has target_allocated objects
    // allocated, placing both spans into the same nonempty_ bucket.
    const size_t to_release = objects_per_span - target_allocated;
    for (int span = 0; span < kNumSpans; ++span) {
      size_t released = 0;
      while (released < to_release) {
        uint64_t n = std::min(to_release - released, e.batch_size());
        for (int i = 0; i < n; ++i) {
          batch[i] = objects[span][i + released];
        }
        released += n;
        e.central_freelist().InsertRange({batch, n});
      }
      objects[span].erase(objects[span].begin(),
                          objects[span].begin() + released);
    }

    Span* span0 = e.forwarder().MapObjectToSpan(objects[0][0]);
    Span* span1 = e.forwarder().MapObjectToSpan(objects[1][0]);

    // Span 0 entered the bucket first, Span 1 entered second.
    // Under disabled prioritization (prepend/LIFO), Span 1 is at the front.
    // Under enabled prioritization (append/FIFO), Span 0 is at the front.
    const bool prioritization_enabled =
        std::get<1>(GetParam()) ==
        central_freelist_internal::CflSubbucketPrioritization::kEnabled;
    Span* front_span = prioritization_enabled ? span0 : span1;
    std::vector<void*>& target_objects =
        release_to_front ? (prioritization_enabled ? objects[0] : objects[1])
                         : (prioritization_enabled ? objects[1] : objects[0]);

    // Step 3: Release 1 object to the chosen span. Since
    // index_for(target_allocated)
    // == index_for(target_allocated - 1), the span remains in the same bucket.
    batch[0] = target_objects[0];
    e.central_freelist().InsertRange({batch, 1});
    target_objects.erase(target_objects.begin());

    // Step 4: Remove 1 object. The intra-bucket priority must remain unchanged,
    // so the object must be drawn from front_span.
    int got = e.central_freelist().RemoveRange(absl::MakeSpan(batch, 1));
    EXPECT_EQ(got, 1);
    Span* drawn_span = e.forwarder().MapObjectToSpan(batch[0]);
    EXPECT_EQ(drawn_span, front_span);

    // Step 5: Clean up by returning all remaining allocated objects.
    e.central_freelist().InsertRange({batch, 1});
    for (int span = 0; span < kNumSpans; ++span) {
      for (size_t i = 0; i < objects[span].size(); ++i) {
        e.central_freelist().InsertRange({&objects[span][i], 1});
      }
    }
  };

  // Test releasing to the front span (verifies it is not demoted to the back).
  test_release_keeps_priority(/*release_to_front=*/true);

  // Test releasing to the back span (verifies it is not promoted to the front).
  test_release_keeps_priority(/*release_to_front=*/false);
}

struct SpanLifetimes {
  absl::flat_hash_map<size_t, size_t> live;
  absl::flat_hash_map<size_t, size_t> completed;
  void InitializeDefault() {
    live[0] = 0;
    completed[0] = 0;
    for (int i = 1; i <= 1000000; i *= 10) {
      live[i] = 0;
      completed[i] = 0;
    }
  }
};

void CheckLifetimeStats(TypeParam& e, SpanLifetimes span_lifetimes) {
  SpanLifetimes expected_lifetimes;
  expected_lifetimes.InitializeDefault();

  auto& live = expected_lifetimes.live;
  for (const auto& [key, value] : span_lifetimes.live) {
    live[key] = value;
  }

  auto& completed = expected_lifetimes.completed;
  for (const auto& [key, value] : span_lifetimes.completed) {
    completed[key] = value;
  }

  // Check txt stats
  std::string live_spans_txt = absl::StrFormat(
      R"(live spans:   0 ms <      %d,  1 ms <      %d, 10 ms <      %d,100 ms <      %d,1000 ms <      %d,10000 ms <      %d,100000 ms <      %d,1000000 ms <      %d)",
      live[0], live[1], live[10], live[100], live[1000], live[10000],
      live[100000], live[1000000]);

  std::string completed_spans_txt = absl::StrFormat(
      R"(completed spans:   0 ms <      %d,  1 ms <      %d, 10 ms <      %d,100 ms <      %d,1000 ms <      %d,10000 ms <      %d,100000 ms <      %d,1000000 ms <      %d)",
      completed[0], completed[1], completed[10], completed[100],
      completed[1000], completed[10000], completed[100000], completed[1000000]);

  std::string buffer = PrintToString(1024 * 1024, [&](Printer& printer) {
    e.central_freelist().PrintSpanLifetimeStats(printer);
  });

  EXPECT_THAT(buffer, testing::AllOf(testing::HasSubstr(completed_spans_txt),
                                     testing::HasSubstr(live_spans_txt)));

  // Check pbtxt stats
  std::vector<std::pair<size_t, size_t>> bounds = {
      {0, 1},        {1, 10},         {10, 100},         {100, 1000},
      {1000, 10000}, {10000, 100000}, {100000, 1000000}, {1000000, 1000000}};
  std::vector<std::string> live_spans_pbtxt;
  std::vector<std::string> completed_spans_pbtxt;
  for (auto [lower_bound, upper_bound] : bounds) {
    live_spans_pbtxt.push_back(absl::StrFormat(
        "span_lifetime_histogram { lower_bound: %d upper_bound: %d value: %d}",
        lower_bound, upper_bound, live[lower_bound]));
    completed_spans_pbtxt.push_back(
        absl::StrFormat("span_completed_lifetime_histogram { lower_bound: %d "
                        "upper_bound: %d value: %d}",
                        lower_bound, upper_bound, completed[lower_bound]));
  }

  std::string buffer_pbtxt =
      PrintToString(1024 * 1024, [&](PbtxtRegion& region) {
        e.central_freelist().PrintSpanLifetimeStatsInPbtxt(region);
      });
  EXPECT_THAT(
      buffer_pbtxt,
      testing::AllOf(
          testing::HasSubstr(absl::StrJoin(live_spans_pbtxt, " ")),
          testing::HasSubstr(absl::StrJoin(completed_spans_pbtxt, " "))));
}

TEST_P(CentralFreeListTest, HookTracing) {
#if ABSL_HAVE_HWADDRESS_SANITIZER
  GTEST_SKIP()
      << "Skipping under HWASan, which uses the top bits of the pointer.";
#endif

  TypeParam e(std::get<0>(GetParam()).size, std::get<0>(GetParam()).bytes,
              std::get<0>(GetParam()).num_to_move, std::get<1>(GetParam()));

  static int insert_count = 0;
  static int remove_count = 0;
  insert_count = 0;
  remove_count = 0;
  auto insert_hook = [](size_t size_class, absl::Span<void*> batch) {
    insert_count += batch.size();
  };
  auto remove_hook = [](size_t size_class, absl::Span<void*> batch) {
    remove_count += batch.size();
  };

  EXPECT_TRUE(e.forwarder().insert_range_hooks_.Add(insert_hook));
  EXPECT_TRUE(e.forwarder().remove_range_hooks_.Add(remove_hook));

  void* batch[kMaxObjectsToMove];
  int got = e.central_freelist().RemoveRange(
      absl::MakeSpan(batch, std::min(size_t{1}, e.batch_size())));
  EXPECT_GT(got, 0);
  EXPECT_EQ(remove_count, got);

  e.central_freelist().InsertRange({batch, static_cast<size_t>(got)});
  EXPECT_EQ(insert_count, got);

  EXPECT_TRUE(e.forwarder().insert_range_hooks_.Remove(insert_hook));
  EXPECT_TRUE(e.forwarder().remove_range_hooks_.Remove(remove_hook));
}

TEST_P(CentralFreeListTest, SpanLifetime) {
#if ABSL_HAVE_HWADDRESS_SANITIZER
  GTEST_SKIP()
      << "Skipping under HWASan, which uses the top bits of the pointer.";
#endif

  TypeParam e(std::get<0>(GetParam()).size, std::get<0>(GetParam()).bytes,
              std::get<0>(GetParam()).num_to_move, std::get<1>(GetParam()));
  // Skip the check for objects_per_span = 1 since such spans skip most of the
  // central freelist's logic.
  if (e.objects_per_span() == 1) {
    GTEST_SKIP() << "Skipping test for objects_per_span = 1.";
  }

  std::vector<void*> all_objects;
  // Request kNumSpans spans.
  void* batch[kMaxObjectsToMove];
  ASSERT_GT(e.objects_per_span(), 0);
  int got = e.central_freelist().RemoveRange(absl::MakeSpan(batch, 1));
  ASSERT_EQ(got, 1);

  e.forwarder().AdvanceClock(absl::Seconds(1));
  CheckLifetimeStats(e, {.live = {{1000, 1}}});

  e.forwarder().AdvanceClock(absl::Seconds(10));
  CheckLifetimeStats(e, {.live = {{10000, 1}}});

  e.forwarder().AdvanceClock(absl::Seconds(100));
  CheckLifetimeStats(e, {.live = {{100000, 1}}});

  e.forwarder().AdvanceClock(absl::Seconds(1000));
  CheckLifetimeStats(e, {.live = {{1000000, 1}}});

  e.forwarder().AdvanceClock(absl::Seconds(-1000));
  e.central_freelist().InsertRange({batch, 1});
  e.forwarder().AdvanceClock(absl::Seconds(1000));
  CheckLifetimeStats(e, {.completed = {{100000, 1}}});

  // Allocate another span, regress the clock before its allocation time,
  // and ensure deallocation clamps to bucket 0 instead of underflowing.
  got = e.central_freelist().RemoveRange(absl::MakeSpan(batch, 1));
  ASSERT_EQ(got, 1);
  e.forwarder().AdvanceClock(absl::Seconds(-500));
  e.central_freelist().InsertRange({batch, 1});
  CheckLifetimeStats(e, {.completed = {{0, 1}, {100000, 1}}});
}

TEST_P(CentralFreeListTest, SpanAllocationTracker) {
#if ABSL_HAVE_HWADDRESS_SANITIZER
  GTEST_SKIP()
      << "Skipping under HWASan, which uses the top bits of the pointer.";
#endif

  TypeParam e(std::get<0>(GetParam()).size, std::get<0>(GetParam()).bytes,
              std::get<0>(GetParam()).num_to_move, std::get<1>(GetParam()));

  const int objects_per_span = e.objects_per_span();
  if (objects_per_span == 1) return;

  constexpr int kNumSpans = 5;
  // Track objects allocated per span.
  absl::FixedArray<std::vector<void*>> objects(kNumSpans);
  void* batch[kMaxObjectsToMove];

  const size_t to_fetch = objects_per_span;
  // Allocate all objects from kNumSpans.
  for (int span = 0; span < kNumSpans; ++span) {
    size_t fetched = 0;
    while (fetched < to_fetch) {
      const size_t n = to_fetch - fetched;
      int got = e.central_freelist().RemoveRange(
          absl::MakeSpan(batch, std::min(n, e.batch_size())));
      for (int i = 0; i < got; ++i) {
        objects[span].push_back(batch[i]);
      }
      fetched += got;
    }
  }

  // Perform deallocations so that each span contains only one free object.
  for (int span = 0; span < kNumSpans; ++span) {
    batch[0] = objects[span][0];
    e.central_freelist().InsertRange({batch, 1});
    objects[span].erase(objects[span].begin(), objects[span].begin() + 1);
  }

  // Calculate the expected number of allocations served by 1 span. These all
  // come from the case where there are no non-empty spans and a new span has to
  // be allocated.
  int single_spans = objects_per_span / e.batch_size();
  if (objects_per_span % e.batch_size() != 0) {
    single_spans++;
  }
  single_spans *= kNumSpans;

  size_t max_lower_bound = 1 << (absl::bit_width<size_t>(kNumSpans) - 1);
  size_t max_upper_bound = max_lower_bound * 2;

  // Check txt stats
  int got = e.central_freelist().RemoveRange(absl::MakeSpan(batch, kNumSpans));
  EXPECT_EQ(got, kNumSpans);
  std::string buffer = PrintToString(1024 * 1024, [&](Printer& printer) {
    e.central_freelist().PrintNumSpansUsed(printer);
  });
  EXPECT_THAT(
      buffer,
      testing::AllOf(
          testing::HasSubstr(absl::StrFormat("%v :        1", max_lower_bound)),
          testing::ContainsRegex(absl::StrFormat("1 : *%v, ", single_spans))));

  // Check pbtxt stats
  std::string buffer_pbtxt =
      PrintToString(1024 * 1024, [&](PbtxtRegion& region) {
        e.central_freelist().PrintNumSpansUsedInPbtxt(region);
      });
  EXPECT_THAT(buffer_pbtxt,
              testing::AllOf(testing::HasSubstr(absl::StrFormat(
                                 "{ lower_bound: %v upper_bound: %v value: 1}",
                                 max_lower_bound, max_upper_bound)),
                             testing::HasSubstr(absl::StrFormat(
                                 "{ lower_bound: 1 upper_bound: 2 value: %v}",
                                 single_spans))));
}

TEST_P(CentralFreeListTest, SameSpans) {
#ifdef TCMALLOC_INTERNAL_LEGACY_LOCKING
  GTEST_SKIP() << "Stats are non-functional when optimization is not enabled.";
#endif
  const int num_to_move = std::get<0>(GetParam()).num_to_move;
  TypeParam e(std::get<0>(GetParam()).size, std::get<0>(GetParam()).bytes,
              num_to_move, std::get<1>(GetParam()));

  // Roundtrip a batch.
  void* batch[kMaxObjectsToMove];
  const int got =
      e.central_freelist().RemoveRange(absl::MakeSpan(batch, num_to_move));
  ASSERT_GT(got, 0);

  Span* spans[kMaxObjectsToMove];
  e.forwarder().MapObjectsToSpans(absl::MakeSpan(batch, got), spans,
                                  e.kSizeClass);
  absl::flat_hash_set<Span*> pseudo_spans;
  for (int i = 0; i < got; ++i) {
    pseudo_spans.insert(spans[i]);
  }

  e.central_freelist().InsertRange(absl::MakeSpan(batch, got));

  // Check the stats after the first insertion.
  {
    std::string expected_stats =
        absl::StrFormat("class %3d [ %8zu bytes ] :", e.kSizeClass,
                        std::get<0>(GetParam()).size);
    for (int i = 0; i < CentralFreeList::kSameSpanBucketCapacity; ++i) {
      const bool first_batch = e.objects_per_span() > 1 &&
                               i == absl::bit_width(static_cast<unsigned int>(
                                        got - pseudo_spans.size()));
      const int count = first_batch ? 1 : 0;
      absl::StrAppendFormat(&expected_stats, " %6d", count);
    }
    absl::StrAppend(&expected_stats, "\n");

    std::string buffer = PrintToString(1024 * 1024, [&](Printer& printer) {
      e.central_freelist().PrintSameSpanStats(printer);
    });
    EXPECT_EQ(buffer, expected_stats) << got;
  }
  {
    std::string expected_pbtxt = "";
    if (e.objects_per_span() > 1) {
      int same_span_val = got - pseudo_spans.size();
      int bucket = absl::bit_width(static_cast<unsigned int>(same_span_val));
      int lower_bound = bucket == 0 ? 0 : (1 << (bucket - 1));
      int upper_bound = bucket == 0 ? 0 : ((1 << bucket) - 1);
      expected_pbtxt = absl::StrFormat(
          " same_span_stats { lower_bound: %d upper_bound: %d value: 1}",
          lower_bound, upper_bound);
    }

    std::string buffer_pbtxt =
        PrintToString(1024 * 1024, [&](PbtxtRegion& region) {
          e.central_freelist().PrintSameSpanStatsInPbtxt(region);
        });
    EXPECT_EQ(buffer_pbtxt, expected_pbtxt) << got;
  }
}

TEST_P(CentralFreeListTest, MultipleSpans) {
#if ABSL_HAVE_HWADDRESS_SANITIZER
  GTEST_SKIP()
      << "Skipping under HWASan, which uses the top bits of the pointer.";
#endif

  TypeParam e(std::get<0>(GetParam()).size, std::get<0>(GetParam()).bytes,
              std::get<0>(GetParam()).num_to_move, std::get<1>(GetParam()));
  std::vector<void*> all_objects;
  constexpr size_t kNumSpans = 10;

  // Request kNumSpans spans.
  void* batch[kMaxObjectsToMove];
  ASSERT_GT(e.objects_per_span(), 0);
  const int num_objects_to_fetch = kNumSpans * e.objects_per_span();
  int total_fetched = 0;
  while (total_fetched < num_objects_to_fetch) {
    size_t n = num_objects_to_fetch - total_fetched;
    int got = e.central_freelist().RemoveRange(
        absl::MakeSpan(batch, std::min(n, e.batch_size())));
    for (int i = 0; i < got; ++i) {
      all_objects.push_back(batch[i]);
    }
    total_fetched += got;
  }

  // We should have kNumSpans spans in the histogram with number of
  // allocated objects equal to e.objects_per_span() (i.e. in the last
  // bucket). Rest of the buckets should be empty.
  const int expected_bitwidth = absl::bit_width(e.objects_per_span());
  // Skip the check for objects_per_span = 1 since such spans skip most of the
  // central freelist's logic.
  if (e.objects_per_span() != 1) {
    EXPECT_EQ(e.central_freelist().NumSpansWith(expected_bitwidth), kNumSpans);
  }
  for (int i = 1; i < expected_bitwidth; ++i) {
    EXPECT_EQ(e.central_freelist().NumSpansWith(i), 0);
  }

  // Skip the check for objects_per_span = 1 since such spans skip most of the
  // central freelist's logic.
  if (e.objects_per_span() != 1) {
    SpanStats stats = e.central_freelist().GetSpanStats();
    EXPECT_EQ(stats.num_spans_requested, kNumSpans);
    EXPECT_EQ(stats.num_spans_returned, 0);
  }

  EXPECT_EQ(all_objects.size(), num_objects_to_fetch);

  // Shuffle
  absl::BitGen rng;
  std::shuffle(all_objects.begin(), all_objects.end(), rng);

  // Return all
  int total_returned = 0;
  while (total_returned < num_objects_to_fetch) {
    uint64_t size_to_pop =
        std::min(all_objects.size() - total_returned, e.batch_size());
    for (int i = 0; i < size_to_pop; ++i) {
      batch[i] = all_objects[i + total_returned];
    }
    total_returned += size_to_pop;
    e.central_freelist().InsertRange({batch, size_to_pop});
    // sanity check
    if (e.objects_per_span() != 1 && total_returned < num_objects_to_fetch) {
      SpanStats stats = e.central_freelist().GetSpanStats();
      EXPECT_GT(stats.num_spans_requested, stats.num_spans_returned);
      EXPECT_NE(stats.obj_capacity, 0);
      // Total spans recorded in the histogram must be equal to the number of
      // live spans.
      size_t spans_in_histogram = 0;
      for (int i = 1; i <= absl::bit_width(e.objects_per_span()); ++i) {
        spans_in_histogram += e.central_freelist().NumSpansWith(i);
      }
      EXPECT_EQ(spans_in_histogram, stats.num_live_spans());
    }
  }

  SpanStats stats = e.central_freelist().GetSpanStats();
  EXPECT_EQ(stats.num_spans_requested, stats.num_spans_returned);
  // Since no span is live, histogram must be empty.
  for (int i = 1; i <= absl::bit_width(e.objects_per_span()); ++i) {
    EXPECT_EQ(e.central_freelist().NumSpansWith(i), 0);
  }
  EXPECT_EQ(stats.obj_capacity, 0);
}

TEST_P(CentralFreeListTest, PassSpanDensityToPageheap) {
#if ABSL_HAVE_HWADDRESS_SANITIZER
  GTEST_SKIP()
      << "Skipping under HWASan, which uses the top bits of the pointer.";
#endif

  TypeParam e(std::get<0>(GetParam()).size, std::get<0>(GetParam()).bytes,
              std::get<0>(GetParam()).num_to_move, std::get<1>(GetParam()));
  ASSERT_GE(e.objects_per_span(), 1);
  auto test_function = [&](size_t num_objects,
                           AccessDensityPrediction density) {
    std::vector<void*> objects(e.objects_per_span());
    EXPECT_CALL(e.forwarder(), AllocateSpan(testing::_, testing::_, testing::_))
        .Times(1);
    const size_t to_fetch = std::min(e.objects_per_span(), e.batch_size());
    const size_t fetched =
        e.central_freelist().RemoveRange(absl::MakeSpan(&objects[0], to_fetch));
    size_t returned = 0;
    while (returned < fetched) {
      EXPECT_CALL(e.forwarder(), DeallocateSpans(testing::_, testing::_))
          .Times(1);
      const size_t to_return = std::min(fetched - returned, e.batch_size());
      e.central_freelist().InsertRange({&objects[returned], to_return});
      returned += to_return;
    }
  };
  test_function(1, AccessDensityPrediction::kDense);
  test_function(e.objects_per_span(), AccessDensityPrediction::kDense);
}
INSTANTIATE_TEST_SUITE_P(
    CentralFreeList, CentralFreeListTest,
    testing::Combine(
        // We skip the first size class since it is set to 0.
        testing::ValuesIn(kSizeClasses.classes.begin() + 1,
                          kSizeClasses.classes.end()),
        testing::Values(
            central_freelist_internal::CflSubbucketPrioritization::kDisabled,
            central_freelist_internal::CflSubbucketPrioritization::kEnabled)));

}  // namespace
}  // namespace tcmalloc_internal
}  // namespace tcmalloc
