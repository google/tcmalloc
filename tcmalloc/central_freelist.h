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

#ifndef TCMALLOC_CENTRAL_FREELIST_H_
#define TCMALLOC_CENTRAL_FREELIST_H_

#include <stddef.h>

#include <cstddef>

#include "absl/base/attributes.h"
#include "absl/base/const_init.h"
#include "absl/base/internal/spinlock.h"
#include "absl/base/macros.h"
#include "absl/base/thread_annotations.h"
#include "tcmalloc/common.h"
#include "tcmalloc/internal/atomic_stats_counter.h"
#include "tcmalloc/internal/optimization.h"
#include "tcmalloc/pages.h"
#include "tcmalloc/span.h"
#include "tcmalloc/span_stats.h"

GOOGLE_MALLOC_SECTION_BEGIN
namespace tcmalloc {
namespace tcmalloc_internal {

namespace central_freelist_internal {

// StaticForwarder provides access to the PageMap and page heap.
//
// This is a class, rather than namespaced globals, so that it can be mocked for
// testing.
class StaticForwarder {
 public:
  static size_t class_to_size(int size_class);
  static Length class_to_pages(int size_class);

  static Span* MapObjectToSpan(const void* object);
  static Span* AllocateSpan(int size_class, Length pages_per_span)
      ABSL_LOCKS_EXCLUDED(pageheap_lock);
  static void DeallocateSpans(int size_class, absl::Span<Span*> free_spans)
      ABSL_LOCKS_EXCLUDED(pageheap_lock);
};

// Records histogram of span utilization. Span utilization represents
// the number of objects allocated from the span, with maximum number of objects
// that can be allocated from a span is less than objects_per_span_.
// Buckets in the histogram correspond to power-of-two number of objects. That
// is, bucket N tracks number of spans with allocated objects < 2^N.
static constexpr size_t kSpanUtilBucketCapacity = 17;
struct SpanUtilHistogram {
  size_t value[kSpanUtilBucketCapacity] = {0};
};

// Data kept per size-class in central cache.
template <typename ForwarderT>
class CentralFreeList {
 public:
  using Forwarder = ForwarderT;

  constexpr CentralFreeList()
      : lock_(absl::kConstInit, absl::base_internal::SCHEDULE_KERNEL_ONLY),
        size_class_(0),
        object_size_(0),
        objects_per_span_(0),
        pages_per_span_(0),
        nonempty_() {}

  CentralFreeList(const CentralFreeList&) = delete;
  CentralFreeList& operator=(const CentralFreeList&) = delete;

  void Init(size_t cl) ABSL_LOCKS_EXCLUDED(lock_);

  // These methods all do internal locking.

  // Insert batch into the central freelist.
  // REQUIRES: batch.size() > 0 && batch.size() <= kMaxObjectsToMove.
  void InsertRange(absl::Span<void*> batch) ABSL_LOCKS_EXCLUDED(lock_);

  // Fill a prefix of batch[0..N-1] with up to N elements removed from central
  // freelist.  Return the number of elements removed.
  ABSL_MUST_USE_RESULT int RemoveRange(void** batch, int N)
      ABSL_LOCKS_EXCLUDED(lock_);

  // Returns the number of free objects in cache.
  size_t length() const { return static_cast<size_t>(counter_.value()); }

  // Returns the memory overhead (internal fragmentation) attributable
  // to the freelist.  This is memory lost when the size of elements
  // in a freelist doesn't exactly divide the page-size (an 8192-byte
  // page full of 5-byte objects would have 2 bytes memory overhead).
  size_t OverheadBytes() const;

  SpanStats GetSpanStats() const;

  // Reports span utilization histogram stats.
  void PrintSpanUtilStats(Printer* out) const;
  void PrintSpanUtilStatsInPbtxt(PbtxtRegion* region) const;

  // Get histogram of span utilization.
  // Histogram consists of kSpanUtilBucketCapacity number of buckets; bucket N
  // records number of spans with allocated objects < 2^N.
  SpanUtilHistogram GetSpanUtilHistogram() const;

  const Forwarder& forwarder() const { return forwarder_; }

  Forwarder& forwarder() { return forwarder_; }

 private:
  // Release an object to spans.
  // Returns object's span if it become completely free.
  Span* ReleaseToSpans(void* object, Span* span, size_t object_size)
      ABSL_EXCLUSIVE_LOCKS_REQUIRED(lock_);

  // Populate cache by fetching from the page heap.
  // May temporarily release lock_.
  // Fill a prefix of batch[0..N-1] with up to N elements removed from central
  // freelist. Returns the number of elements removed.
  int Populate(void** batch, int N) ABSL_EXCLUSIVE_LOCKS_REQUIRED(lock_);

  // This lock protects all the mutable data members.
  absl::base_internal::SpinLock lock_;

  size_t size_class_;  // My size class (immutable after Init())
  size_t object_size_;
  size_t objects_per_span_;
  Length pages_per_span_;

  size_t num_spans() const {
    size_t requested = num_spans_requested_.value();
    size_t returned = num_spans_returned_.value();
    if (requested < returned) return 0;
    return (requested - returned);
  }

  void RecordSpanAllocated() ABSL_EXCLUSIVE_LOCKS_REQUIRED(lock_) {
    counter_.LossyAdd(objects_per_span_);
    num_spans_requested_.LossyAdd(1);
  }

  void RecordMultiSpansDeallocated(size_t num_spans_returned)
      ABSL_EXCLUSIVE_LOCKS_REQUIRED(lock_) {
    counter_.LossyAdd(-num_spans_returned * objects_per_span_);
    num_spans_returned_.LossyAdd(num_spans_returned);
  }

  void UpdateObjectCounts(int num) ABSL_EXCLUSIVE_LOCKS_REQUIRED(lock_) {
    counter_.LossyAdd(num);
  }

  // The followings are kept as a StatsCounter so that they can read without
  // acquiring a lock. Updates to these variables are guarded by lock_
  // so writes are performed using LossyAdd for speed, the lock still
  // guarantees accuracy.

  // Num free objects in cache entry
  StatsCounter counter_;

  StatsCounter num_spans_requested_;
  StatsCounter num_spans_returned_;

  // Records current number of spans with corresponding number of allocated
  // objects. Instead of using the absolute value of number of
  // allocated objects, we use absl::bit_width(free_objects) to index this
  // map. As the actual value of objects_per_span_ is not known at compile
  // time, we use maximum bit_width that objects_per_span_ can have to
  // construct this map.
  StatsCounter objects_to_spans_
      [std::numeric_limits<decltype(objects_per_span_)>::digits];

  // Records <span> in objects_to_span_ map.
  // If increase is set to true, includes the span by incrementing the count
  // in the map. Otherwise, removes the span by decrementing the count in
  // the map.
  //
  // Updates to objects_to_span_ are guarded by lock_, so writes may be
  // performed using LossyAdd.
  void RecordSpanUtil(Span* span, bool increase)
      ABSL_EXCLUSIVE_LOCKS_REQUIRED(lock_) {
    const size_t allocated = span->Allocated();
    ASSUME(allocated > 0);
    objects_to_spans_[absl::bit_width(allocated)].LossyAdd(increase ? 1 : -1);
  }

  // Dummy header for non-empty spans
  SpanList nonempty_ ABSL_GUARDED_BY(lock_);

  TCMALLOC_NO_UNIQUE_ADDRESS Forwarder forwarder_;
};

// Like a constructor and hence we disable thread safety analysis.
template <class Forwarder>
inline void CentralFreeList<Forwarder>::Init(size_t cl)
    ABSL_NO_THREAD_SAFETY_ANALYSIS {
  size_class_ = cl;
  object_size_ = Forwarder::class_to_size(cl);
  pages_per_span_ = Forwarder::class_to_pages(cl);
  objects_per_span_ =
      pages_per_span_.in_bytes() / (object_size_ ? object_size_ : 1);
  ASSERT(absl::bit_width(objects_per_span_) < kSpanUtilBucketCapacity);
}

template <class Forwarder>
inline Span* CentralFreeList<Forwarder>::ReleaseToSpans(void* object,
                                                        Span* span,
                                                        size_t object_size) {
  if (ABSL_PREDICT_FALSE(span->FreelistEmpty(object_size))) {
    nonempty_.prepend(span);
  }

  // As the objects are being added to the span, its utilization might change.
  // We remove the stale utilization from the histogram and add the new
  // utilization to the histogram after we release objects to the span.
  RecordSpanUtil(span, /*increase=*/false);
  if (ABSL_PREDICT_TRUE(span->FreelistPush(object, object_size))) {
    RecordSpanUtil(span, /*increase=*/true);
    return nullptr;
  }
  span->RemoveFromList();  // from nonempty_
  return span;
}

template <class Forwarder>
inline void CentralFreeList<Forwarder>::InsertRange(absl::Span<void*> batch) {
  CHECK_CONDITION(!batch.empty() && batch.size() <= kMaxObjectsToMove);
  Span* spans[kMaxObjectsToMove];
  // Safe to store free spans into freed up space in span array.
  Span** free_spans = spans;
  int free_count = 0;

  // Prefetch Span objects to reduce cache misses.
  for (int i = 0; i < batch.size(); ++i) {
    Span* span = forwarder_.MapObjectToSpan(batch[i]);
    ASSERT(span != nullptr);
    span->Prefetch();
    spans[i] = span;
  }

  // First, release all individual objects into spans under our mutex
  // and collect spans that become completely free.
  {
    // Use local copy of variable to ensure that it is not reloaded.
    size_t object_size = object_size_;
    absl::base_internal::SpinLockHolder h(&lock_);
    for (int i = 0; i < batch.size(); ++i) {
      Span* span = ReleaseToSpans(batch[i], spans[i], object_size);
      if (ABSL_PREDICT_FALSE(span)) {
        free_spans[free_count] = span;
        free_count++;
      }
    }

    RecordMultiSpansDeallocated(free_count);
    UpdateObjectCounts(batch.size());
  }

  // Then, release all free spans into page heap under its mutex.
  if (ABSL_PREDICT_FALSE(free_count)) {
    forwarder_.DeallocateSpans(size_class_,
                               absl::MakeSpan(free_spans, free_count));
  }
}

template <class Forwarder>
inline int CentralFreeList<Forwarder>::RemoveRange(void** batch, int N) {
  ASSUME(N > 0);
  // Use local copy of variable to ensure that it is not reloaded.
  size_t object_size = object_size_;
  int result = 0;
  absl::base_internal::SpinLockHolder h(&lock_);
  if (ABSL_PREDICT_FALSE(nonempty_.empty())) {
    result = Populate(batch, N);
  } else {
    do {
      Span* span = nonempty_.first();
      // As the objects are being popped from the span, its utilization might
      // change. So, we remove the stale utilization from the histogram here and
      // add it again once we pop the objects.
      RecordSpanUtil(span, /*increase=*/false);
      int here =
          span->FreelistPopBatch(batch + result, N - result, object_size);
      RecordSpanUtil(span, /*increase=*/true);
      ASSERT(here > 0);
      if (span->FreelistEmpty(object_size)) {
        span->RemoveFromList();  // from nonempty_
      }
      result += here;
    } while (result < N && !nonempty_.empty());
  }
  UpdateObjectCounts(-result);
  return result;
}

// Fetch memory from the system and add to the central cache freelist.
template <class Forwarder>
inline int CentralFreeList<Forwarder>::Populate(void** batch, int N)
    ABSL_NO_THREAD_SAFETY_ANALYSIS {
  // Release central list lock while operating on pageheap
  // Note, this could result in multiple calls to populate each allocating
  // a new span and the pushing those partially full spans onto nonempty.
  lock_.Unlock();

  Span* span = forwarder_.AllocateSpan(size_class_, pages_per_span_);
  if (ABSL_PREDICT_FALSE(span == nullptr)) {
    Log(kLog, __FILE__, __LINE__, "tcmalloc: allocation failed",
        pages_per_span_.in_bytes());

    lock_.Lock();
    return 0;
  }

  size_t objects_per_span = objects_per_span_;
  int result = span->BuildFreelist(object_size_, objects_per_span, batch, N);
  ASSERT(result > 0);
  // This is a cheaper check than using FreelistEmpty().
  bool span_empty = result == objects_per_span;

  lock_.Lock();

  // Update the histogram once we populate the span.
  RecordSpanUtil(span, /*increase=*/true);
  if (!span_empty) {
    nonempty_.prepend(span);
  }
  RecordSpanAllocated();
  return result;
}

template <class Forwarder>
inline size_t CentralFreeList<Forwarder>::OverheadBytes() const {
  if (ABSL_PREDICT_FALSE(object_size_ == 0)) {
    return 0;
  }
  const size_t overhead_per_span = pages_per_span_.in_bytes() % object_size_;
  return num_spans() * overhead_per_span;
}

template <class Forwarder>
inline SpanStats CentralFreeList<Forwarder>::GetSpanStats() const {
  SpanStats stats;
  if (ABSL_PREDICT_FALSE(objects_per_span_ == 0)) {
    return stats;
  }
  stats.num_spans_requested = static_cast<size_t>(num_spans_requested_.value());
  stats.num_spans_returned = static_cast<size_t>(num_spans_returned_.value());
  stats.obj_capacity = stats.num_live_spans() * objects_per_span_;
  return stats;
}

template <class Forwarder>
inline SpanUtilHistogram CentralFreeList<Forwarder>::GetSpanUtilHistogram()
    const {
  SpanUtilHistogram histogram;
  for (int i = 0; i <= absl::bit_width(objects_per_span_); ++i) {
    histogram.value[i] = objects_to_spans_[i].value();
  }
  return histogram;
}

template <class Forwarder>
inline void CentralFreeList<Forwarder>::PrintSpanUtilStats(Printer* out) const {
  const SpanUtilHistogram util_histogram = GetSpanUtilHistogram();

  out->printf("class %3d [ %8zu bytes ] : ", size_class_, object_size_);
  for (size_t i = 0; i < kSpanUtilBucketCapacity; ++i) {
    out->printf("%6zu < %zu", util_histogram.value[i], 1 << i);
    if (i < kSpanUtilBucketCapacity - 1) {
      out->printf(",");
    }
  }
  out->printf("\n");
}

template <class Forwarder>
inline void CentralFreeList<Forwarder>::PrintSpanUtilStatsInPbtxt(
    PbtxtRegion* region) const {
  const SpanUtilHistogram util_histogram = GetSpanUtilHistogram();

  for (size_t i = 0; i < kSpanUtilBucketCapacity; ++i) {
    PbtxtRegion histogram = region->CreateSubRegion("span_util_histogram");
    size_t lower_bound = i > 0 ? 1 << (i - 1) : 0;
    histogram.PrintI64("lower_bound", lower_bound);
    histogram.PrintI64("upper_bound", 1 << i);
    histogram.PrintI64("value", util_histogram.value[i]);
  }
}

}  // namespace central_freelist_internal

using CentralFreeList = central_freelist_internal::CentralFreeList<
    central_freelist_internal::StaticForwarder>;

}  // namespace tcmalloc_internal
}  // namespace tcmalloc
GOOGLE_MALLOC_SECTION_END

#endif  // TCMALLOC_CENTRAL_FREELIST_H_
