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
#include <string.h>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <new>
#include <utility>

#include "absl/algorithm/container.h"
#include "absl/base/attributes.h"
#include "absl/base/internal/cycleclock.h"
#include "absl/base/internal/spinlock.h"
#include "absl/base/nullability.h"
#include "absl/base/optimization.h"
#include "absl/base/thread_annotations.h"
#include "absl/numeric/bits.h"
#include "absl/types/span.h"
#include "tcmalloc/common.h"
#include "tcmalloc/internal/atomic_stats_counter.h"
#include "tcmalloc/internal/central_freelist_hooks.h"
#include "tcmalloc/internal/config.h"
#include "tcmalloc/internal/heap.h"
#include "tcmalloc/internal/hook_list.h"
#include "tcmalloc/internal/logging.h"
#include "tcmalloc/internal/optimization.h"
#include "tcmalloc/internal/prefetch.h"
#include "tcmalloc/internal/two_level_array.h"
#include "tcmalloc/pages.h"
#include "tcmalloc/span.h"
#include "tcmalloc/span_stats.h"

GOOGLE_MALLOC_SECTION_BEGIN
namespace tcmalloc {
namespace tcmalloc_internal {

// TODO(b/29448043): Remove latency injection.
// TODO(b/296824599): AllocationGuardSpinLockHolder adds an AllocationGuard
// which is not yet compatible with the CentralFreeListTest code.
class ABSL_SCOPED_LOCKABLE CentralFreeListLockHolder {
 public:
  explicit CentralFreeListLockHolder(absl::base_internal::SpinLock& lock)
      ABSL_EXCLUSIVE_LOCK_FUNCTION(lock)
      : lock_(lock) {
    lock_.lock();
#ifdef TCMALLOC_INTERNAL_LATENCY_INJECTION
    ScopedDelay delay(ScopedDelay::central_freelist_delay);
#endif
  }
  ~CentralFreeListLockHolder() ABSL_UNLOCK_FUNCTION() { lock_.unlock(); }

 private:
  absl::base_internal::SpinLock& lock_;
};

namespace central_freelist_internal {

// StaticForwarder provides access to the PageMap and page heap.
//
// This is a class, rather than namespaced globals, so that it can be mocked for
// testing.
using InsertRangeHook = void (*)(size_t size_class, absl::Span<void*> batch);
using RemoveRangeHook = void (*)(size_t size_class, absl::Span<void*> batch);

class StaticForwarder {
 public:
  static void InvokeInsertRangeHook(size_t size_class,
                                    absl::Span<void*> batch) {
    if (ABSL_PREDICT_TRUE(central_freelist_insert_range_hooks.empty())) {
      return;
    }
    InvokeInsertRangeHookSlow(size_class, batch);
  }

  static void InvokeRemoveRangeHook(size_t size_class,
                                    absl::Span<void*> batch) {
    if (ABSL_PREDICT_TRUE(central_freelist_remove_range_hooks.empty())) {
      return;
    }
    InvokeRemoveRangeHookSlow(size_class, batch);
  }

  static uint64_t clock_now() { return absl::base_internal::CycleClock::Now(); }
  static double clock_frequency() {
    return absl::base_internal::CycleClock::Frequency();
  }

  static size_t class_to_size(int size_class);
  static Length class_to_pages(int size_class);
  [[nodiscard]] static void* absl_nullable AllocateSpan(int size_class,
                                                        size_t objects_per_span,
                                                        Length pages_per_span)
      ABSL_LOCKS_EXCLUDED(pageheap_lock);
  static void DeallocateSpans(size_t objects_per_span, Length pages_per_span,
                              absl::Span<void*> free_spans,
                              absl::Span<std::atomic<SpanMeta>*> pmetas)
      ABSL_LOCKS_EXCLUDED(pageheap_lock);

  static void MapObjectsToMeta(absl::Span<void*> batch,
                               std::atomic<SpanMeta>** metas);
  static std::atomic<SpanMeta>* GetSpanMeta(PageId p);
  static void* ArenaAlloc(size_t bytes, std::align_val_t alignment);

 private:
  static void InvokeInsertRangeHookSlow(size_t size_class,
                                        absl::Span<void*> batch);
  static void InvokeRemoveRangeHookSlow(size_t size_class,
                                        absl::Span<void*> batch);
};

// Specifies number of nonempty_ lists that keep track of non-empty spans.
static constexpr size_t kNumLists = 8;
// Specifies the threshold for number of objects per span. The threshold is
// used to consider a span sparsely- vs. densely-accessed.
static constexpr size_t kFewObjectsAllocMaxLimit = 16;
// Specifies the number of buckets in the histogram that tracks the number of
// spans used to fill a batch.
static constexpr size_t kSpansUsedStatBuckets =
    absl::bit_width(kMaxObjectsToMove);

enum class CflSubbucketPrioritization : bool {
  kDisabled = false,
  kEnabled = true
};

using SpanObjectIdx = uint16_t;

// Span descriptor stored in the CFL nonempty list (only relevant for spans
// that have a mix of allocated and free objects).
// This is the intended layout for main platforms with kAddressBits <= 48.
// The size of the struct is 32 bytes. There are some spare bits left,
// but unless we can squeeze the struct to 24/16 bytes, we leave then unused
// for nicer/faster layout:
//  - kPageShift low bits of span
//  - 2 low bits of pmeta
//  - high bit of span/pmeta if kAddressBits == 47
//  - high bit of allocated
//  - 4 high bits of priority
//  - 4 high bits of freelist_count
template <bool SmallAddr>
struct SpanHeaderImpl {
  // Pointer to the beginning of the span memory.
  uint64_t span : 48;
  // Number of allocated objects in the span.
  uint64_t allocated : 16;
  // Pointer to the SpanMeta object in the page map.
  // We can find it in the page map using the span pointer,
  // but with the 3-level page map, it's faster to have it here.
  uint64_t pmeta : 48;
  // Priority of the span in the nonempty heap for RemoveRange.
  // Span with the highest priority at the tail of the heap is the first
  // candidate for removal.
  uint64_t priority : 8;
  uint64_t freelist_count : 8;
  union {
    // Bitmap of free objects, used if all object indexes fit here.
    uint64_t bitmap[2];
    // Otherwise this space is used as a small complementary freelist of free
    // object indexes. This is still more beneficial than using only the bitmap
    // at the end of the span (which is totally possible) b/c (1) the bitmap
    // may be very sparse and (2) this avoids touching the span memory.
    // Small size classes with lots of objects per span may have lots of spans
    // with very few free objects per span. For these we can use only freelist.
    SpanObjectIdx freelist[8];
  };
};

// Fallback layout for platforms with kAddressBits > 48.
// It has reduced bitmap/freelist capacity.
template <>
struct SpanHeaderImpl<false> {
  uint64_t span;
  uint64_t pmeta;
  uint64_t allocated : 16;
  uint64_t priority : 8;
  uint64_t freelist_count : 8;
  uint64_t unused : 32;
  union {
    uint64_t bitmap[1];
    SpanObjectIdx freelist[4];
  };
};

struct SpanHeader : SpanHeaderImpl<(kAddressBits <= 48)> {
  using PriorityType = uint8_t;

  ABSL_ATTRIBUTE_ALWAYS_INLINE PriorityType Priority() const {
    return priority;
  }

  static ABSL_ATTRIBUTE_ALWAYS_INLINE bool Compare(PriorityType a,
                                                   PriorityType b) {
    return a < b;
  }

  ABSL_ATTRIBUTE_ALWAYS_INLINE void SetIndex(size_t index) {
    std::atomic<SpanMeta>* p = MetaPtr();
    SpanMeta meta = p->load(std::memory_order_relaxed);
    TC_ASSERT(meta.is_in_cfl);
    meta.heap_index_or_page_offset = index;
    p->store(meta, std::memory_order_relaxed);
  }

  ABSL_ATTRIBUTE_ALWAYS_INLINE void* SpanPtr() const {
    return reinterpret_cast<void*>(span);
  }

  ABSL_ATTRIBUTE_ALWAYS_INLINE void SetSpanPtr(void* p) {
    span = reinterpret_cast<uintptr_t>(p);
    TC_ASSERT_EQ(SpanPtr(), p);
  }

  ABSL_ATTRIBUTE_ALWAYS_INLINE std::atomic<SpanMeta>* MetaPtr() const {
    return reinterpret_cast<std::atomic<SpanMeta>*>(pmeta);
  }

  ABSL_ATTRIBUTE_ALWAYS_INLINE void SetMetaPtr(std::atomic<SpanMeta>* p) {
    pmeta = reinterpret_cast<uintptr_t>(p);
    TC_ASSERT_EQ(MetaPtr(), p);
  }
};

static_assert(sizeof(SpanHeader) == 32);

// Data kept per size-class in central cache.
template <typename ForwarderT>
class CentralFreeList {
 public:
  using Forwarder = ForwarderT;
  using PriorityType = typename SpanHeader::PriorityType;

  static constexpr size_t kSameSpanBucketCapacity =
      absl::bit_width(kMaxObjectsToMove);
  // num_same_spans_ is indexed by absl::bit_width(same_span) for same_span in
  // [0, kMaxObjectsToMove - 1], which fits only when kMaxObjectsToMove is a
  // power of two.
  static_assert(absl::has_single_bit(kMaxObjectsToMove));

  constexpr CentralFreeList()
      : lock_(absl::base_internal::SCHEDULE_KERNEL_ONLY),
        size_class_(0),
        object_size_(0),
        objects_per_span_(0),
        pages_per_span_(0),
        nonempty_(),
        use_all_buckets_for_few_object_spans_(false),
        cfl_subbucket_prioritization_(CflSubbucketPrioritization::kDisabled) {
  }

  CentralFreeList(const CentralFreeList&) = delete;
  CentralFreeList& operator=(const CentralFreeList&) = delete;

  void Init(size_t size_class,
            CflSubbucketPrioritization cfl_subbucket_prioritization)
      ABSL_LOCKS_EXCLUDED(lock_);

  // These methods all do internal locking.

  // Insert batch into the central freelist.
  // REQUIRES: batch.size() > 0 && batch.size() <= kMaxObjectsToMove.
  void InsertRange(absl::Span<void* absl_nonnull> batch)
      ABSL_LOCKS_EXCLUDED(lock_);

  // Fill a prefix of batch[0..N-1] with up to N elements removed from central
  // freelist.  Return the number of elements removed.
  [[nodiscard]] int RemoveRange(absl::Span<void*> batch)
      ABSL_LOCKS_EXCLUDED(lock_);

  // Returns the number of free objects in cache.
  size_t length() const { return static_cast<size_t>(counter_.value()); }

  // Returns the memory overhead (internal fragmentation) attributable
  // to the freelist.  This is memory lost when the size of elements
  // in a freelist doesn't exactly divide the page-size (an 8192-byte
  // page full of 5-byte objects would have 2 bytes memory overhead).
  size_t OverheadBytes() const;

  // Returns number of live spans currently in the nonempty_[n] list.
  // REQUIRES: n >= 0 && n < kNumLists.
  size_t NumSpansInList(int n) ABSL_LOCKS_EXCLUDED(lock_);
  SpanStats GetSpanStats() const;

  // Reports span utilization, and number of spans
  // used to fill a batch.
  void PrintSpanUtilStats(Printer& out);
  void PrintNumSpansUsed(Printer& out);
  void PrintSameSpanStats(Printer& out);
  void PrintSpanUtilStatsInPbtxt(PbtxtRegion& region);
  void PrintSameSpanStatsInPbtxt(PbtxtRegion& region);
  void PrintNumSpansUsedInPbtxt(PbtxtRegion& region);

  // Get number of spans in the histogram bucket. We record spans in the
  // histogram indexed by absl::bit_width(allocated). So, instead of using the
  // absolute number of allocated objects, it uses absl::bit_width(allocated),
  // passed as <bitwidth>, to index and return the number of spans in the
  // histogram.
  size_t NumSpansWith(uint16_t bitwidth) const;

  Forwarder& forwarder() { return forwarder_; }

  size_t objects_per_span() const { return objects_per_span_; }

 private:
  friend class CentralFreeListTestPeer;

  [[nodiscard]] int RemoveRangeImpl(absl::Span<void*> batch)
      ABSL_LOCKS_EXCLUDED(lock_);

  // Populate cache by fetching from the page heap.
  // May temporarily release lock_.
  // Fill a prefix of batch[0..N-1] with up to N elements removed from central
  // freelist. Returns the number of elements removed.
  size_t Populate(absl::Span<void*> batch) ABSL_EXCLUSIVE_LOCKS_REQUIRED(lock_);

  // Allocate a span from the forwarder.
  void* AllocateSpan();

  // This lock protects all the mutable data members.
  mutable absl::base_internal::SpinLock lock_;

  size_t size_class_;  // My size class (immutable after Init())
  size_t object_size_;
  size_t objects_per_span_;
  // Size reciprocal is used to replace division with multiplication when
  // computing object indices in the Span bitmap.
  uint32_t size_reciprocal_ = 0;
  Length pages_per_span_;
  // If not 0, the free object bitmap is located at the end of the span
  // at the given offset. Otherwise, SpanHeader::bitmap is used as the free
  // object bitmap.
  size_t bitmap_offset_ = 0;
  // Size of the bitmap in uint64_t words.
  size_t bitmap_words_ = 0;

  size_t num_spans() const {
    size_t requested = num_spans_requested_.value();
    size_t returned = num_spans_returned_.value();
    if (requested < returned) return 0;
    return (requested - returned);
  }

  void RecordSpanAllocated(size_t objects_per_span)
      ABSL_EXCLUSIVE_LOCKS_REQUIRED(lock_) {
    counter_.LossyAdd(objects_per_span);
    num_spans_requested_.LossyAdd(1);
  }

  void RecordMultiSpansDeallocated(size_t num_spans_returned,
                                   size_t objects_freed,
                                   size_t objects_per_span)
      ABSL_EXCLUSIVE_LOCKS_REQUIRED(lock_) {
    counter_.LossyAdd(objects_freed - num_spans_returned * objects_per_span);
    num_spans_returned_.LossyAdd(num_spans_returned);
  }

  void UpdateObjectCounts(int num) ABSL_EXCLUSIVE_LOCKS_REQUIRED(lock_) {
    counter_.LossyAdd(num);
  }

  // Tracks the number of spans used to fill a batch in RemoveRange
  StatsCounters<kSpansUsedStatBuckets> span_allocations_tracker_;

  // The followings are kept as a StatsCounter so that they can read without
  // acquiring a lock. Updates to these variables are guarded by lock_
  // so writes are performed using LossyAdd for speed, the lock still
  // guarantees accuracy.

  // Records histogram of how many consecutive objects fell on the same span for
  // batches.
  //
  // Index in this array corresponds to absl::bit_width(same_span), yielding
  // 8 buckets total because same_span has range [0, 127] (assuming
  // kMaxObjectsToMove is 128).
  //
  // TODO(b/527641380): Delete this after wrapping up optimizations.
  StatsCounter num_same_spans_[kSameSpanBucketCapacity];

  // Num free objects in cache entry
  StatsCounter counter_;

  StatsCounter num_spans_requested_;
  StatsCounter num_spans_returned_;

  // Number of buckets in the span utilization histogram.
  static constexpr size_t kSpanUtilBucketCapacity = 16;

  void PopulateSpanUtilStats(absl::Span<size_t> span_util,
                             absl::Span<size_t> spans_in_list) const
      ABSL_LOCKS_EXCLUDED(lock_);

  // Initializes the first allocated bits of the span bitmap to 0,
  // and the remaining bits to 1. Used when span is first allocated.
  void InitSpanBitmap(SpanHeader* hdr, char* span, size_t allocated) const;
  // Helpers for InsertRange/RemoveRange.
  static void InsertIntoSpan(SpanHeader* hdr, char* span,
                             const SpanObjectIdx* idx, size_t count,
                             size_t bitmap_offset);
  static size_t RemoveFromSpan(SpanHeader* hdr, char* span, void**& batch_pos,
                               void** batch_end, size_t prev_allocated,
                               size_t object_size, size_t objects_per_span,
                               size_t bitmap_offset, size_t bitmap_words);

  // Returns nonempty_ list priority for a span based on the number of allocated
  // objects in the span, and temp sift priority that is used only when the span
  // is moved in the heap.
  [[nodiscard]] std::pair<PriorityType, PriorityType> CalcPriority(
      SpanObjectIdx allocated) const;

  struct ForwarderArenaAllocator {
    void* operator()(size_t bytes, std::align_val_t alignment) const {
      return Forwarder::ArenaAlloc(bytes, alignment);
    }
  };

  using NonemptyArray = TwoLevelArray<SpanHeader, ForwarderArenaAllocator>;
  using NonemptyHeap = Heap<SpanHeader, NonemptyArray>;

  NonemptyHeap nonempty_ ABSL_GUARDED_BY(lock_);

  bool use_all_buckets_for_few_object_spans_;

  CflSubbucketPrioritization cfl_subbucket_prioritization_;

  ABSL_ATTRIBUTE_NO_UNIQUE_ADDRESS Forwarder forwarder_;
};

// Like a constructor and hence we disable thread safety analysis.
template <class Forwarder>
inline void CentralFreeList<Forwarder>::Init(
    size_t size_class, CflSubbucketPrioritization cfl_subbucket_prioritization)
    ABSL_NO_THREAD_SAFETY_ANALYSIS {
  TC_CHECK_EQ(size_class_, 0);
  size_class_ = size_class;
  object_size_ = forwarder_.class_to_size(size_class);
  if (object_size_ == 0) {
    return;
  }
  pages_per_span_ = forwarder_.class_to_pages(size_class);
  objects_per_span_ = pages_per_span_.in_bytes() / object_size_;
  size_reciprocal_ = CalcReciprocal(object_size_);
  use_all_buckets_for_few_object_spans_ = objects_per_span_ <= 2 * kNumLists;

  TC_ASSERT_LE(absl::bit_width(objects_per_span_), kSpanUtilBucketCapacity);
  cfl_subbucket_prioritization_ = cfl_subbucket_prioritization;

  bitmap_offset_ = 0;
  bitmap_words_ = (objects_per_span_ + 63) / 64;
  if (objects_per_span_ > sizeof(SpanHeader::bitmap) * 8) {
    // Bitmap does not fit in SpanHeader::bitmap,
    // place it at the end of the span.
    size_t bitmap_bytes = bitmap_words_ * sizeof(uint64_t);
    size_t end_waste = pages_per_span_.in_bytes() % object_size_;
    if (end_waste < bitmap_bytes) {
      // Bitmap does not fit into end of span waste,
      // take some objects from the span.
      size_t needed = bitmap_bytes - end_waste;
      size_t meta_objs = (needed + object_size_ - 1) / object_size_;
      objects_per_span_ -= meta_objs;
      bitmap_words_ = (objects_per_span_ + 63) / 64;
      bitmap_bytes = bitmap_words_ * sizeof(uint64_t);
    }
    bitmap_offset_ = pages_per_span_.in_bytes() - bitmap_bytes;
    TC_ASSERT_GE(bitmap_offset_, objects_per_span_ * object_size_);
  }

  if (objects_per_span_ > 1) {
    // Check that values fit into our bitfields w/o truncation.
    SpanHeader hdr{};
    hdr.allocated = objects_per_span_ - 1;
    TC_CHECK_EQ(hdr.allocated, objects_per_span_ - 1);
    auto [prio, sift_prio] = CalcPriority(objects_per_span_ - 1);
    hdr.priority = prio;
    TC_CHECK_EQ(hdr.priority, prio);
    hdr.priority = sift_prio;
    TC_CHECK_EQ(hdr.priority, sift_prio);
    SpanMeta meta{};
    meta.heap_index_or_page_offset = pages_per_span_.raw_num();
    TC_CHECK_EQ(meta.heap_index_or_page_offset, pages_per_span_.raw_num());

    // Number of buckets that are pre-allocated for nonempty_ array.
    // This initial block is slightly faster to access, and has better locality
    // (single contiguous allocation). Number of elements is N^2-1,
    // e.g. for N = 10 and sizeof(SpanHeader) = 32, size of the block is 32 KiB.
    // These numbers are chosen looking at 50/90 percentiles of CFL lists,
    // but are not well tuned.
    const size_t nonempty_init_buckets = object_size_ <= 24     ? 7
                                         : object_size_ <= 512  ? 10
                                         : object_size_ <= 1024 ? 7
                                                                : 5;
    nonempty_.Init(nonempty_init_buckets);
  }
}

template <class Forwarder>
inline size_t CentralFreeList<Forwarder>::NumSpansInList(int n) {
  ASSUME(n >= 0);
  ASSUME(n < kNumLists);
  size_t span_util[kSpanUtilBucketCapacity] = {};
  size_t spans_in_list[kNumLists] = {};
  PopulateSpanUtilStats(span_util, spans_in_list);
  return spans_in_list[n];
}

template <class Forwarder>
inline void CentralFreeList<Forwarder>::InsertRange(absl::Span<void*> batch) {
  TC_ASSERT(!batch.empty());
  TC_ASSERT_LE(batch.size(), kMaxObjectsToMove);

  std::atomic<SpanMeta>* pmetas[kMaxObjectsToMove];
  forwarder_.MapObjectsToMeta(batch, pmetas);
  const size_t size_class = size_class_;
  forwarder_.InvokeInsertRangeHook(size_class, batch);

  const uint32_t objects_per_span = objects_per_span_;
  const size_t batch_size = batch.size();
  const Length pages_per_span = pages_per_span_;
  if (ABSL_PREDICT_FALSE(objects_per_span == 1)) {
    // If there is only 1 object per span, skip CentralFreeList entirely.
    forwarder_.DeallocateSpans(objects_per_span, pages_per_span, batch,
                               absl::MakeSpan(pmetas, batch_size));
    return;
  }

  // Use local copy of variables to ensure that they are not reloaded.
  const uint32_t size_reciprocal = size_reciprocal_;
  const size_t bitmap_offset = bitmap_offset_;

  char* spans[kMaxObjectsToMove];
  SpanObjectIdx idx[kMaxObjectsToMove];
  for (size_t i = 0; i < batch_size; ++i) {
    SpanMeta meta = pmetas[i]->load(std::memory_order_relaxed);
    TC_ASSERT(meta.is_small);
    TC_ASSERT_EQ(meta.size_class, size_class);
    PageId p = PageIdContaining(batch[i]);
    if (ABSL_PREDICT_FALSE(pages_per_span > Length(1))) {
      // If the span is more than 1 page, find start of the span.
      // The offset is stored in heap_index_or_page_offset.
      if (size_t offset = meta.heap_index_or_page_offset;
          offset != 0 && !meta.is_in_cfl) {
        p -= Length(offset);
        pmetas[i] -= offset;
        SpanMeta meta = pmetas[i]->load(std::memory_order_relaxed);
        TC_ASSERT(meta.is_small);
        TC_ASSERT_EQ(meta.size_class, size_class);
      }
    }
    char* span = static_cast<char*>(p.start_addr());
    spans[i] = span;
    idx[i] = OffsetToIdx(static_cast<char*>(batch[i]) - span, size_reciprocal);
    if (bitmap_offset != 0) {
      PrefetchW(span + bitmap_offset + idx[i] / 8);
    }
  }

  // Then, release all individual objects into spans under our mutex
  // and collect spans that become completely free.
  // Safe to store free spans into freed up space in span array.
  char** free_spans = spans;
  std::atomic<SpanMeta>** free_metas = pmetas;
  size_t free_count = 0;
  size_t runs = 0;

  {
    CentralFreeListLockHolder h(lock_);
    for (size_t i = 0; i < batch_size;) {
      char* const cur_span = spans[i];
      size_t j = i + 1;
      while (j < batch_size && spans[j] == cur_span) {
        ++j;
      }
      const SpanObjectIdx* cur_idx = &idx[i];
      std::atomic<SpanMeta>* pmeta = pmetas[i];
      size_t step = j - i;
      i += step;
      ++runs;

      SpanMeta meta = pmeta->load(std::memory_order_relaxed);
      TC_ASSERT(meta.is_small);
      TC_ASSERT_EQ(meta.size_class, size_class);
      SpanHeader* hdr = nullptr;
      SpanObjectIdx prev_allocated = objects_per_span;
      size_t heap_index = 0;
      if (ABSL_PREDICT_TRUE(meta.is_in_cfl)) {
        // Span is already in CFL, find the header.
        heap_index = meta.heap_index_or_page_offset;
        hdr = &nonempty_[heap_index];
        if (ABSL_PREDICT_FALSE(hdr->SpanPtr() != cur_span)) {
          // This is possible in the unlikely case the heap index was truncated
          // (huge head with high fragmentation). In this case, we just do a
          // linear search in the list to find the right header. This case
          // is not expected to happen any often in sane production use cases.
          constexpr size_t kStep = size_t{1} << SpanMeta::kHeapIndexBits;
          do {
            heap_index += kStep;
            hdr = &nonempty_[heap_index];
          } while (hdr->SpanPtr() != cur_span);
        }
        prev_allocated = hdr->allocated;
      }
      const SpanObjectIdx cur_allocated = prev_allocated - step;
      if (ABSL_PREDICT_FALSE(cur_allocated == 0)) {
        // Span is now completely free.
        if (ABSL_PREDICT_TRUE(meta.is_in_cfl)) {
          nonempty_.Remove(heap_index);
        }
        free_spans[free_count] = cur_span;
        free_metas[free_count] = pmeta;
        free_count++;
        continue;
      }

      const auto [cur_priority, sift_priority] = CalcPriority(cur_allocated);
      if (ABSL_PREDICT_TRUE(meta.is_in_cfl)) {
        // Span is in CFL, adjust span priority.
        hdr->allocated = cur_allocated;
        const PriorityType prev_priority = hdr->priority;
        TC_ASSERT_LE(cur_priority, prev_priority);
        if (ABSL_PREDICT_FALSE(cur_priority != prev_priority)) {
          hdr->priority = cur_priority;
          auto [new_hdr, new_idx] = nonempty_.SiftUp(heap_index, sift_priority);
          hdr = new_hdr;
          meta.heap_index_or_page_offset = new_idx;
          pmeta->store(meta, std::memory_order_relaxed);
        }
      } else {
        // Span enters CFL.
        SpanHeader new_hdr{};
        new_hdr.SetSpanPtr(cur_span);
        new_hdr.SetMetaPtr(pmeta);
        new_hdr.allocated = cur_allocated;
        new_hdr.priority = cur_priority;
        auto [inserted_hdr, inserted_idx] =
            nonempty_.Push(new_hdr, sift_priority);
        hdr = inserted_hdr;
        meta.is_in_cfl = 1;
        meta.heap_index_or_page_offset = inserted_idx;
        pmeta->store(meta, std::memory_order_relaxed);
      }

      InsertIntoSpan(hdr, cur_span, cur_idx, step, bitmap_offset);
    }

    const int same_span = batch_size - runs;
    TC_ASSERT_GE(same_span, 0);
    num_same_spans_[absl::bit_width(static_cast<unsigned int>(same_span))]
        .LossyAdd(1);

    RecordMultiSpansDeallocated(free_count, batch_size, objects_per_span);
  }

  // Then, release all free spans into page heap under its mutex.
  if (ABSL_PREDICT_FALSE(free_count)) {
    forwarder_.DeallocateSpans(
        objects_per_span, pages_per_span,
        absl::MakeSpan(reinterpret_cast<void**>(free_spans), free_count),
        absl::MakeSpan(free_metas, free_count));
  }
}

template <class Forwarder>
ABSL_ATTRIBUTE_ALWAYS_INLINE void CentralFreeList<Forwarder>::InsertIntoSpan(
    SpanHeader* hdr, char* span, const SpanObjectIdx* idx, size_t count,
    size_t bitmap_offset) {
  if (bitmap_offset != 0) {
    // See if there is any available space in the freelist.
    size_t freelist_count = hdr->freelist_count;
    size_t available = std::min(
        sizeof(SpanHeader::freelist) / sizeof(SpanHeader::freelist[0]) -
            freelist_count,
        count);
    if (available > 0) {
      for (size_t k = 0; k < available; ++k, ++freelist_count) {
        hdr->freelist[freelist_count] = idx[k];
      }
      hdr->freelist_count = freelist_count;
      if (available == count) {
        return;
      }
      idx += available;
      count -= available;
    }
  }

  // Free the rest into the bitmap.
  uint64_t* bm = ABSL_PREDICT_TRUE(bitmap_offset != 0)
                     ? reinterpret_cast<uint64_t*>(span + bitmap_offset)
                     : hdr->bitmap;
  size_t cur_word = idx[0] / 64;
  uint64_t cur_mask = uint64_t{1} << (idx[0] % 64);
  for (size_t k = 1; k < count; ++k) {
    size_t w = idx[k] / 64;
    uint64_t bit = uint64_t{1} << (idx[k] % 64);
    if (w == cur_word) {
      cur_mask |= bit;
    } else {
      bm[cur_word] |= cur_mask;
      cur_word = w;
      cur_mask = bit;
    }
  }
  bm[cur_word] |= cur_mask;
}

template <class Forwarder>
inline int CentralFreeList<Forwarder>::RemoveRange(absl::Span<void*> batch) {
  TC_ASSERT(!batch.empty());
  TC_ASSERT_LE(batch.size(), kMaxObjectsToMove);

  int result = RemoveRangeImpl(batch);

  size_t size = batch.size();
  ASSUME(result <= size);
  forwarder_.InvokeRemoveRangeHook(size_class_, batch.subspan(0, result));
  return result;
}

template <class Forwarder>
int CentralFreeList<Forwarder>::RemoveRangeImpl(absl::Span<void*> batch) {
  const size_t objects_per_span = objects_per_span_;
  if (ABSL_PREDICT_FALSE(objects_per_span == 1)) {
    // If there is only 1 object per span, skip CentralFreeList entirely.
    void* span = AllocateSpan();
    if (ABSL_PREDICT_FALSE(span == nullptr)) {
      return 0;
    }
    batch[0] = span;
    return 1;
  }

  // Use local copy of variable to ensure that it is not reloaded.
  const size_t object_size = object_size_;
  const size_t bitmap_offset = bitmap_offset_;
  const size_t bitmap_words = bitmap_words_;
  void** batch_pos = batch.data();
  void** batch_end = batch.end();
  int num_spans = 0;

  CentralFreeListLockHolder h(lock_);

  do {
    num_spans++;
    if (ABSL_PREDICT_FALSE(nonempty_.empty())) {
      batch_pos += Populate(batch.subspan(batch_pos - batch.data()));
      break;
    }
    SpanHeader* const hdr = &nonempty_.Back();
    const SpanObjectIdx prev_allocated = hdr->allocated;
    ASSUME(prev_allocated > 0);
    char* span = static_cast<char*>(hdr->SpanPtr());
    const size_t here = RemoveFromSpan(
        hdr, span, batch_pos, batch_end, prev_allocated, object_size,
        objects_per_span, bitmap_offset, bitmap_words);
    const SpanObjectIdx cur_allocated = prev_allocated + here;
    if (ABSL_PREDICT_FALSE(cur_allocated == objects_per_span)) {
      // The span is fully allocated, remove it from the CFL.
      std::atomic<SpanMeta>* pmeta = hdr->MetaPtr();
      SpanMeta meta = pmeta->load(std::memory_order_relaxed);
      meta.is_in_cfl = 0;
      meta.heap_index_or_page_offset = 0;
      pmeta->store(meta, std::memory_order_relaxed);
      nonempty_.PopBack();
    } else {
      // Adjust priority, don't need to sift down (it was last and priority
      // can only increase).
      const auto [cur_priority, _] = CalcPriority(cur_allocated);
      TC_ASSERT_GE(cur_priority, hdr->priority);
      hdr->allocated = cur_allocated;
      hdr->priority = cur_priority;
    }
  } while (batch_pos < batch_end);

  TC_ASSERT_GT(num_spans, 0);
  TC_ASSERT_LE(num_spans, kMaxObjectsToMove);
  span_allocations_tracker_
      [absl::bit_width(static_cast<unsigned int>(num_spans)) - 1]
          .LossyAdd(1);
  size_t result = batch_pos - batch.data();
  UpdateObjectCounts(-result);
  return result;
}

template <class Forwarder>
ABSL_ATTRIBUTE_ALWAYS_INLINE size_t CentralFreeList<Forwarder>::RemoveFromSpan(
    SpanHeader* hdr, char* span, void**& batch_pos, void** batch_end,
    size_t prev_allocated, size_t object_size, size_t objects_per_span,
    size_t bitmap_offset, size_t bitmap_words) {
  void** batch_pos0 = batch_pos;
  size_t freelist_count = hdr->freelist_count;
  const ssize_t available_in_bitmap =
      objects_per_span - prev_allocated - freelist_count;
  // See if we can grab something from the freelist first.
  if (freelist_count != 0) {
    while (freelist_count != 0 && batch_pos < batch_end) {
      --freelist_count;
      *batch_pos++ = span + hdr->freelist[freelist_count] * object_size;
    }
    hdr->freelist_count = freelist_count;
    if (available_in_bitmap <= 0) {
      return batch_pos - batch_pos0;
    }
  }
  TC_ASSERT_GT(available_in_bitmap, 0);
  batch_end = std::min(batch_end, batch_pos + available_in_bitmap);
  uint64_t* bm = ABSL_PREDICT_TRUE(bitmap_offset != 0)
                     ? reinterpret_cast<uint64_t*>(span + bitmap_offset)
                     : hdr->bitmap;
  uint64_t* bm_end = bm + bitmap_words;
  char* word_start = span;
  for (; batch_pos < batch_end; bm++, word_start += 64 * object_size) {
    TC_ASSERT_LT(bm, bm_end);
    uint64_t bitmap = *bm;
    if (bitmap == 0) {
      continue;
    }
    do {
      int bit = absl::countr_zero(bitmap);
      bitmap &= (bitmap - 1);
      *batch_pos++ = word_start + bit * object_size;
    } while (bitmap != 0 && batch_pos < batch_end);
    *bm = bitmap;
  }
  return batch_pos - batch_pos0;
}

// Fetch memory from the system and add to the central cache freelist.
template <class Forwarder>
inline size_t CentralFreeList<Forwarder>::Populate(absl::Span<void*> batch) {
  // Release central list lock while operating on pageheap
  lock_.unlock();

  char* span = static_cast<char*>(AllocateSpan());
  if (ABSL_PREDICT_FALSE(span == nullptr)) {
    lock_.lock();
    return 0;
  }

  const size_t objects_per_span = objects_per_span_;
  const size_t object_size = object_size_;
  const size_t allocated = std::min(batch.size(), objects_per_span);
  char* ptr = span;
  for (size_t i = 0; i < allocated; ++i, ptr += object_size) {
    batch[i] = ptr;
  }
  SpanHeader hdr{};
  InitSpanBitmap(&hdr, span, allocated);
  if (allocated != objects_per_span) {
    // The span has free objects, add it to the nonempty list.
    hdr.SetSpanPtr(span);
    auto* pmeta = forwarder_.GetSpanMeta(PageIdContaining(span));
    hdr.SetMetaPtr(pmeta);
    hdr.allocated = allocated;
    const auto [priority, sift_priority] = CalcPriority(allocated);
    hdr.priority = priority;

    lock_.lock();

    auto [inserted_hdr, inserted_idx] = nonempty_.Push(hdr, sift_priority);
    SpanMeta meta = pmeta->load(std::memory_order_relaxed);
    meta.is_in_cfl = 1;
    meta.heap_index_or_page_offset = inserted_idx;
    pmeta->store(meta, std::memory_order_relaxed);
  } else {
    lock_.lock();
  }
  RecordSpanAllocated(objects_per_span);
  return allocated;
}

template <class Forwarder>
void CentralFreeList<Forwarder>::InitSpanBitmap(SpanHeader* hdr, char* span,
                                                size_t allocated) const {
  uint64_t* bm = ABSL_PREDICT_TRUE(bitmap_offset_ != 0)
                     ? reinterpret_cast<uint64_t*>(span + bitmap_offset_)
                     : hdr->bitmap;
  memset(bm, 0, bitmap_words_ * sizeof(uint64_t));
  if (allocated == objects_per_span_) {
    return;
  }
  const size_t start_word = allocated / 64;
  const size_t start_bit = allocated % 64;
  const size_t end_word = objects_per_span_ / 64;
  const size_t end_bit = objects_per_span_ % 64;
  for (size_t w = 0; w < start_word; ++w) {
    bm[w] = 0;
  }
  if (start_word == end_word) {
    bm[start_word] =
        (~uint64_t{0} << start_bit) & ((uint64_t{1} << end_bit) - 1);
    return;
  }
  bm[start_word] = ~uint64_t{0} << start_bit;
  for (size_t w = start_word + 1; w < end_word; ++w) {
    bm[w] = ~uint64_t{0};
  }
  if (end_bit != 0) {
    bm[end_word] = (uint64_t{1} << end_bit) - 1;
  }
}

template <class Forwarder>
ABSL_ATTRIBUTE_ALWAYS_INLINE
    std::pair<typename CentralFreeList<Forwarder>::PriorityType,
              typename CentralFreeList<Forwarder>::PriorityType>
    CentralFreeList<Forwarder>::CalcPriority(SpanObjectIdx allocated) const {
  TC_ASSERT_GT(allocated, 0);
  if (ABSL_PREDICT_TRUE(!use_all_buckets_for_few_object_spans_)) {
    allocated = absl::bit_width(allocated);
  }
  // Shift the priority by 1 to make space for the temp sift priority.
  const PriorityType priority = std::min<PriorityType>(allocated, kNumLists)
                                << 1;
  TC_ASSERT_GT(priority, 0);
  // By default, we prepend (AddFront) to the nonempty_ list. When the
  // CflSubbucketPrioritization feature is enabled, we append (AddBack).
  const PriorityType sift_priority =
      cfl_subbucket_prioritization_ == CflSubbucketPrioritization::kDisabled
          ? priority + 1
          : priority - 1;
  return {priority, sift_priority};
}

template <class Forwarder>
void* CentralFreeList<Forwarder>::AllocateSpan() {
  void* span =
      forwarder_.AllocateSpan(size_class_, objects_per_span_, pages_per_span_);
  if (ABSL_PREDICT_FALSE(span == nullptr)) {
    TC_LOG("tcmalloc: allocation failed %v", pages_per_span_);
  }
  return span;
}

template <class Forwarder>
inline size_t CentralFreeList<Forwarder>::OverheadBytes() const {
  if (ABSL_PREDICT_FALSE(object_size_ == 0)) {
    return 0;
  }
  const size_t overhead_per_span =
      pages_per_span_.in_bytes() - objects_per_span_ * object_size_;
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
inline size_t CentralFreeList<Forwarder>::NumSpansWith(
    uint16_t bitwidth) const {
  TC_ASSERT_GT(bitwidth, 0);
  size_t span_util[kSpanUtilBucketCapacity] = {};
  size_t spans_in_list[kNumLists] = {};
  PopulateSpanUtilStats(span_util, spans_in_list);
  return span_util[bitwidth - 1];
}

template <class Forwarder>
inline void CentralFreeList<Forwarder>::PrintSameSpanStats(Printer& out) {
  out.printf("class %3d [ %8zu bytes ] :", size_class_, object_size_);
  for (int i = 0; i < kSameSpanBucketCapacity; ++i) {
    out.printf(" %6zu", num_same_spans_[i].value());
  }
  out.printf("\n");
}

template <class Forwarder>
inline void CentralFreeList<Forwarder>::PrintSameSpanStatsInPbtxt(
    PbtxtRegion& region) {
  for (int i = 0; i < kSameSpanBucketCapacity; ++i) {
    auto value = num_same_spans_[i].value();
    if (value == 0) {
      continue;
    }
    PbtxtRegion histogram = region.CreateSubRegion("same_span_stats");
    int lower_bound = i == 0 ? 0 : (1 << (i - 1));
    int upper_bound = i == 0 ? 0 : ((1 << i) - 1);
    histogram.PrintI64("lower_bound", lower_bound);
    histogram.PrintI64("upper_bound", upper_bound);
    histogram.PrintI64("value", value);
  }
}

template <class Forwarder>
inline void CentralFreeList<Forwarder>::PrintSpanUtilStats(Printer& out) {
  size_t span_util[kSpanUtilBucketCapacity] = {0};
  size_t spans_in_list[kNumLists] = {0};
  PopulateSpanUtilStats(span_util, spans_in_list);

  out.printf("class %3d [ %8zu bytes ] : ", size_class_, object_size_);
  for (size_t i = 1; i <= kSpanUtilBucketCapacity; ++i) {
    out.printf("%6zu < %zu", span_util[i - 1], size_t{1} << i);
    if (i < kSpanUtilBucketCapacity) {
      out.printf(",");
    }
  }
  out.printf("\n");
  out.printf("class %3d [ %8zu bytes ] : ", size_class_, object_size_);
  for (size_t i = 0; i < kNumLists; ++i) {
    out.printf("%6zu: %zu", i, spans_in_list[i]);
    if (i < kNumLists - 1) {
      out.printf(",");
    }
  }
  out.printf("\n");
}

template <class Forwarder>
inline void CentralFreeList<Forwarder>::PrintNumSpansUsed(Printer& out) {
  out.printf("class %3d [ %8zu bytes ] : ", size_class_, object_size_);
  size_t lower_bound = 1;
  for (size_t i = 0; i < kSpansUsedStatBuckets; ++i) {
    out.printf("%zu : %8zu", lower_bound, span_allocations_tracker_[i].value());
    if (i < kSpansUsedStatBuckets - 1) {
      out.printf(", ");
    }
    lower_bound *= 2;
  }
  out.printf("\n");
}

template <class Forwarder>
inline void CentralFreeList<Forwarder>::PrintSpanUtilStatsInPbtxt(
    PbtxtRegion& region) {
  size_t span_util[kSpanUtilBucketCapacity] = {0};
  size_t spans_in_list[kNumLists] = {0};
  PopulateSpanUtilStats(span_util, spans_in_list);

  for (size_t i = 1; i <= kSpanUtilBucketCapacity; ++i) {
    PbtxtRegion histogram = region.CreateSubRegion("span_util_histogram");
    histogram.PrintI64("lower_bound", 1 << (i - 1));
    histogram.PrintI64("upper_bound", 1 << i);
    histogram.PrintI64("value", span_util[i - 1]);
  }

  for (size_t i = 0; i < kNumLists; ++i) {
    PbtxtRegion occupancy =
        region.CreateSubRegion("prioritization_list_occupancy");
    occupancy.PrintI64("list_index", i);
    occupancy.PrintI64("value", spans_in_list[i]);
  }
}

template <class Forwarder>
inline void CentralFreeList<Forwarder>::PrintNumSpansUsedInPbtxt(
    PbtxtRegion& region) {
  size_t lower_bound = 1;
  for (size_t i = 0; i < kSpansUsedStatBuckets; ++i) {
    PbtxtRegion span_allocations_tracker =
        region.CreateSubRegion("span_allocations_histogram");
    size_t upper_bound = lower_bound * 2;
    span_allocations_tracker.PrintI64("lower_bound", lower_bound);
    span_allocations_tracker.PrintI64("upper_bound", upper_bound);
    span_allocations_tracker.PrintI64("value",
                                      span_allocations_tracker_[i].value());
    lower_bound = upper_bound;
  }
}

template <class Forwarder>
inline void CentralFreeList<Forwarder>::PopulateSpanUtilStats(
    absl::Span<size_t> span_util, absl::Span<size_t> spans_in_list) const {
  TC_ASSERT_EQ(span_util.size(), kSpanUtilBucketCapacity);
  TC_ASSERT_EQ(spans_in_list.size(), kNumLists);
  CentralFreeListLockHolder h(lock_);
  nonempty_.ForEach([&](const SpanHeader& hdr) {
    uint8_t bw = absl::bit_width(hdr.allocated);
    TC_ASSERT(bw > 0 && bw <= kSpanUtilBucketCapacity);
    span_util[bw - 1]++;
    spans_in_list[kNumLists - (hdr.priority >> 1)]++;
  });
  const uint8_t full_bitwidth = absl::bit_width(objects_per_span_);
  if (full_bitwidth > 0 && full_bitwidth <= kSpanUtilBucketCapacity) {
    const size_t live_spans =
        num_spans_requested_.value() - num_spans_returned_.value();
    if (live_spans > nonempty_.size()) {
      span_util[full_bitwidth - 1] += live_spans - nonempty_.size();
    }
  }
}

}  // namespace central_freelist_internal

using CentralFreeList = central_freelist_internal::CentralFreeList<
    central_freelist_internal::StaticForwarder>;

}  // namespace tcmalloc_internal
}  // namespace tcmalloc
GOOGLE_MALLOC_SECTION_END

#endif  // TCMALLOC_CENTRAL_FREELIST_H_
