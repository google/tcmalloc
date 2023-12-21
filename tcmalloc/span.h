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
//
// A Span is a contiguous run of pages.

#ifndef TCMALLOC_SPAN_H_
#define TCMALLOC_SPAN_H_

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <atomic>
#include <cassert>
#include <cstddef>

#include "absl/base/attributes.h"
#include "absl/base/optimization.h"
#include "absl/base/thread_annotations.h"
#include "tcmalloc/common.h"
#include "tcmalloc/internal/config.h"
#include "tcmalloc/internal/linked_list.h"
#include "tcmalloc/internal/logging.h"
#include "tcmalloc/internal/optimization.h"
#include "tcmalloc/internal/prefetch.h"
#include "tcmalloc/internal/range_tracker.h"
#include "tcmalloc/internal/sampled_allocation.h"
#include "tcmalloc/pages.h"
#include "tcmalloc/sizemap.h"

GOOGLE_MALLOC_SECTION_BEGIN
namespace tcmalloc {
namespace tcmalloc_internal {

// Denominator for bitmap scaling factor. The idea is that instead of dividing
// by N we multiply by M = kBitmapScalingDenominator / N and round the resulting
// value.
inline constexpr size_t kBitmapScalingDenominator = 65536;

enum AccessDensityPrediction {
  // Predict that the span would be sparsely-accessed.
  kSparse = 0,
  // Predict that the span would be densely-accessed.
  kDense = 1,
  kPredictionCounts
};

struct SpanAllocInfo {
  size_t objects_per_span;
  AccessDensityPrediction density;
};

// Information kept for a span (a contiguous run of pages).
//
// Spans can be in different states. The current state determines set of methods
// that can be called on the span (and the active member in the union below).
// States are:
//  - SMALL_OBJECT: the span holds multiple small objects.
//    The span is owned by CentralFreeList and is generally on
//    CentralFreeList::nonempty_ list (unless has no free objects).
//    location_ == IN_USE.
//  - LARGE_OBJECT: the span holds a single large object.
//    The span can be considered to be owner by user until the object is freed.
//    location_ == IN_USE.
//  - SAMPLED: the span holds a single sampled object.
//    The span can be considered to be owner by user until the object is freed.
//    location_ == IN_USE && sampled_ == 1.
//  - ON_NORMAL_FREELIST: the span has no allocated objects, owned by PageHeap
//    and is on normal PageHeap list.
//    location_ == ON_NORMAL_FREELIST.
//  - ON_RETURNED_FREELIST: the span has no allocated objects, owned by PageHeap
//    and is on returned PageHeap list.
//    location_ == ON_RETURNED_FREELIST.
class Span;
typedef TList<Span> SpanList;

class Span : public SpanList::Elem {
 public:
  // Allocator/deallocator for spans. Note that these functions are defined
  // in static_vars.h, which is weird: see there for why.
  static Span* New(PageId p, Length len)
      ABSL_EXCLUSIVE_LOCKS_REQUIRED(pageheap_lock);
  static void Delete(Span* span) ABSL_EXCLUSIVE_LOCKS_REQUIRED(pageheap_lock);

  // locations used to track what list a span resides on.
  enum Location {
    IN_USE,                // not on PageHeap lists
    ON_NORMAL_FREELIST,    // on normal PageHeap list
    ON_RETURNED_FREELIST,  // on returned PageHeap list
  };
  Location location() const;
  void set_location(Location loc);

  // ---------------------------------------------------------------------------
  // Support for sampled allocations.
  // There is one-to-one correspondence between a sampled allocation and a span.
  // ---------------------------------------------------------------------------

  // Mark this span in the "SAMPLED" state. It will store the corresponding
  // sampled allocation and update some global counters on the total size of
  // sampled allocations.
  void Sample(SampledAllocation* sampled_allocation);

  // Unmark this span from its "SAMPLED" state. It will return the sampled
  // allocation previously passed to Span::Sample() or nullptr if this is a
  // non-sampling span. It will also update the global counters on the total
  // size of sampled allocations.
  // REQUIRES: this is a SAMPLED span.
  SampledAllocation* Unsample();

  // Returns the sampled allocation of the span.
  // pageheap_lock is not required, but caller either needs to hold the lock or
  // ensure by some other means that the sampling state can't be changed
  // concurrently.
  // REQUIRES: this is a SAMPLED span.
  SampledAllocation* sampled_allocation() const;

  // Is it a sampling span?
  // For debug checks. pageheap_lock is not required, but caller needs to ensure
  // that sampling state can't be changed concurrently.
  bool sampled() const;

  bool donated() const { return is_donated_; }
  void set_donated(bool value) { is_donated_ = value; }
  // ---------------------------------------------------------------------------
  // Span memory range.
  // ---------------------------------------------------------------------------

  // Returns first page of the span.
  PageId first_page() const;

  // Returns the last page in the span.
  PageId last_page() const;

  // Sets span first page.
  void set_first_page(PageId p);

  // Returns start address of the span.
  ABSL_ATTRIBUTE_RETURNS_NONNULL void* start_address() const;

  // Returns number of pages in the span.
  Length num_pages() const;

  // Sets number of pages in the span.
  void set_num_pages(Length len);

  // Total memory bytes in the span.
  size_t bytes_in_span() const;

  // Returns internal fragmentation of the span.
  // REQUIRES: this is a SMALL_OBJECT span.
  double Fragmentation(size_t object_size) const;

  // Returns number of objects allocated in the span.
  uint16_t Allocated() const {
    return allocated_.load(std::memory_order_relaxed);
  }

  // Returns index of the non-empty list to which this span belongs to.
  uint8_t nonempty_index() const { return nonempty_index_; }
  // Records an index of the non-empty list associated with this span.
  void set_nonempty_index(uint8_t index) { nonempty_index_ = index; }

  // ---------------------------------------------------------------------------
  // Freelist management.
  // Used for spans in CentralFreelist to manage free objects.
  // These methods REQUIRE a SMALL_OBJECT span.
  // ---------------------------------------------------------------------------

  // Indicates whether the object is considered large or small based on
  // size > SizeMap::kMultiPageSize.
  enum class Align { SMALL, LARGE };

  // Indicate whether the Span is empty. Size is used to determine whether
  // the span is using a compressed linked list of objects, or a bitmap
  // to hold available objects.
  bool FreelistEmpty(size_t size) const;

  // Pushes ptr onto freelist unless the freelist becomes full, in which case
  // just return false.
  //
  // If the freelist becomes full, we do not push the object onto the freelist.
  bool FreelistPush(void* ptr, size_t size) {
    const auto allocated = allocated_.load(std::memory_order_relaxed);
    ASSERT(allocated > 0);
    if (ABSL_PREDICT_FALSE(allocated == 1)) {
      return false;
    }
    allocated_.store(allocated - 1, std::memory_order_relaxed);
    // Bitmaps are used to record object availability when there are fewer than
    // 64 objects in a span.
    if (ABSL_PREDICT_FALSE(UseBitmapForSize(size))) {
      if (ABSL_PREDICT_TRUE(size <= SizeMap::kMultiPageSize)) {
        return BitmapPush<Align::SMALL>(ptr, size);
      } else {
        return BitmapPush<Align::LARGE>(ptr, size);
      }
    }
    return ListPush(ptr, size);
  }

  // Pops up to N objects from the freelist and returns them in the batch array.
  // Returns number of objects actually popped.
  size_t FreelistPopBatch(void** batch, size_t N, size_t size);

  // Reset a Span object to track the range [p, p + n).
  void Init(PageId p, Length n);

  // Initialize freelist to contain all objects in the span.
  // Pops up to N objects from the freelist and returns them in the batch array.
  // Returns number of objects actually popped.
  int BuildFreelist(size_t size, size_t count, void** batch, int N);

  // Prefetch cacheline containing most important span information.
  void Prefetch();

  // DoesNotFitInBitmap returns true for a <size> class when object size is
  // larger than the minimum bitmap object size, but can accommodate greater
  // than BitmapSize <objects_per_span>.
  static bool DoesNotFitInBitmap(size_t size, size_t objects_per_span);

  // Returns true if Span does not touch objects and the <size> is suitable
  // for cold size classes.
  static bool IsNonIntrusive(size_t size);

 private:
  // See the comment on freelist organization in cc file.
  typedef uint16_t ObjIdx;
  static constexpr ObjIdx kListEnd = -1;

  // Use uint16_t or uint8_t for 16 bit and 8 bit fields instead of bitfields.
  // LLVM will generate widen load/store and bit masking operations to access
  // bitfields and this hurts performance. Although compiler flag
  // -ffine-grained-bitfield-accesses can help the performance if bitfields
  // are used here, but the flag could potentially hurt performance in other
  // cases so it is not enabled by default. For more information, please
  // look at b/35680381 and cl/199502226.
  std::atomic<uint16_t> allocated_;  // Number of non-free objects
  uint16_t embed_count_;
  // For available objects stored as a compressed linked list, the index of
  // the first object in recorded in freelist_. When a bitmap is used to
  // represent available objects, the reciprocal of the object size is
  // stored to enable conversion from the offset of an object within a
  // span to the index of the object.
  union {
    uint16_t freelist_;
    uint16_t reciprocal_;
  };
  uint8_t cache_size_;
  uint8_t nonempty_index_ : 4;  // The nonempty_ list index for this span.
  uint8_t location_ : 2;  // Is the span on a freelist, and if so, which?
  uint8_t sampled_ : 1;   // Sampled object?
  // Has this span allocation resulted in a donation to the filler in the page
  // heap? This is used by page heap to compute abandoned pages.
  uint8_t is_donated_ : 1;

  static constexpr size_t kCacheSize = 4;
  static constexpr size_t kBitmapSize = 8 * sizeof(ObjIdx) * kCacheSize;

  union {
    // Used only for spans in CentralFreeList (SMALL_OBJECT state).
    // Embed cache of free objects.
    ObjIdx cache_[kCacheSize];

    // Used for spans with in CentralFreeList with fewer than 64 objects.
    // Each bit is set to one when the object is available, and zero
    // when the object is used.
    Bitmap<kBitmapSize> bitmap_{};

    // Used only for sampled spans (SAMPLED state).
    SampledAllocation* sampled_allocation_;
  };

  PageId first_page_;  // Starting page number.
  Length num_pages_;   // Number of pages in span.

  // Returns true if Span will use bitmap for objects of size <size>.
  static bool UseBitmapForSize(size_t size);

  // Convert object pointer <-> freelist index.
  ObjIdx PtrToIdx(void* ptr, size_t size) const;
  ObjIdx* IdxToPtr(ObjIdx idx, size_t size, uintptr_t start) const;

  // For bitmap'd spans conversion from an offset to an index is performed
  // by multiplying by the scaled reciprocal of the object size.
  static uint16_t CalcReciprocal(size_t size);

  // Convert object pointer <-> freelist index for bitmap managed objects.
  template <Align align>
  ObjIdx BitmapPtrToIdx(void* ptr, size_t size) const;
  ObjIdx* BitmapIdxToPtr(ObjIdx idx, size_t size) const;

  // Helper function for converting a pointer to an index.
  template <Align align>
  static ObjIdx OffsetToIdx(uintptr_t offset, size_t size, uint16_t reciprocal);
  // Helper function for testing round trips between pointers and indexes.
  static ObjIdx TestOffsetToIdx(uintptr_t ptr, size_t size,
                                uint16_t reciprocal) {
    if (size <= SizeMap::kMultiPageSize) {
      return OffsetToIdx<Align::SMALL>(ptr, size, reciprocal);
    } else {
      return OffsetToIdx<Align::LARGE>(ptr, size, reciprocal);
    }
  }

  size_t ListPopBatch(void** __restrict batch, size_t N, size_t size);

  bool ListPush(void* ptr, size_t size);

  // For spans containing 64 or fewer objects, indicate that the object at the
  // index has been returned. Always returns true.
  template <Align align>
  bool BitmapPush(void* ptr, size_t size);

  // A bitmap is used to indicate object availability for spans containing
  // 64 or fewer objects.
  void BuildBitmap(size_t size, size_t count);

  // For spans with 64 or fewer objects populate batch with up to N objects.
  // Returns number of objects actually popped.
  size_t BitmapPopBatch(void** batch, size_t N, size_t size);

  // Friend class to enable more indepth testing of bitmap code.
  friend class SpanTestPeer;
};

inline Span::ObjIdx* Span::IdxToPtr(ObjIdx idx, size_t size,
                                    uintptr_t start) const {
  ASSERT(num_pages_ == Length(1));
  ASSERT(start == first_page_.start_uintptr());
  ASSERT(idx != kListEnd);
  uintptr_t off = start + (static_cast<uintptr_t>(idx) << kAlignmentShift);
  ObjIdx* ptr = reinterpret_cast<ObjIdx*>(off);
  ASSERT(PtrToIdx(ptr, size) == idx);
  return ptr;
}

inline Span::ObjIdx Span::PtrToIdx(void* ptr, size_t size) const {
  // Object index is an offset from span start divided by kAlignment.
  uintptr_t p = reinterpret_cast<uintptr_t>(ptr);
  // Classes that use freelist must also use 1 page per span,
  // so don't load first_page_ (may be on a different cache line).
  ASSERT(num_pages_ == Length(1));
  ASSERT(PageIdContaining(ptr) == first_page_);
  uintptr_t off = (p & (kPageSize - 1)) >> kAlignmentShift;
  ObjIdx idx = static_cast<ObjIdx>(off);
  ASSERT(idx != kListEnd);
  ASSERT(idx == off);
  ASSERT(p == first_page_.start_uintptr() +
                  (static_cast<uintptr_t>(idx) << kAlignmentShift));
  return idx;
}

inline size_t Span::ListPopBatch(void** __restrict batch, size_t N,
                                 size_t size) {
  size_t result = 0;

  // Pop from cache.
  auto csize = cache_size_;
  ASSUME(csize <= kCacheSize);
  auto cache_reads = csize < N ? csize : N;
  const uintptr_t span_start = first_page_.start_uintptr();
  for (; result < cache_reads; result++) {
    batch[result] = IdxToPtr(cache_[csize - result - 1], size, span_start);
  }

  // Store this->cache_size_ one time.
  cache_size_ = csize - result;

  while (result < N) {
    if (freelist_ == kListEnd) {
      break;
    }

    ObjIdx* const host = IdxToPtr(freelist_, size, span_start);
    uint16_t embed_count = embed_count_;
    ObjIdx current = host[embed_count];

    size_t iter = embed_count;
    if (result + embed_count > N) {
      iter = N - result;
    }
    for (size_t i = 0; i < iter; i++) {
      // Pop from the first object on freelist.
      batch[result + i] = IdxToPtr(host[embed_count - i], size, span_start);
    }
    embed_count -= iter;
    result += iter;

    // Update current for next cycle.
    current = host[embed_count];

    if (result == N) {
      embed_count_ = embed_count;
      break;
    }

    // The first object on the freelist is empty, pop it.
    ASSERT(embed_count == 0);

    batch[result] = host;
    result++;

    freelist_ = current;
    embed_count_ = size / sizeof(ObjIdx) - 1;
  }
  allocated_.store(allocated_.load(std::memory_order_relaxed) + result,
                   std::memory_order_relaxed);
  return result;
}

inline bool Span::ListPush(void* ptr, size_t size) {
  ObjIdx idx = PtrToIdx(ptr, size);
  if (cache_size_ != kCacheSize) {
    // Have empty space in the cache, push there.
    cache_[cache_size_] = idx;
    cache_size_++;
  } else if (ABSL_PREDICT_TRUE(freelist_ != kListEnd) &&
             // -1 because the first slot is used by freelist link.
             ABSL_PREDICT_TRUE(embed_count_ != size / sizeof(ObjIdx) - 1)) {
    // Push onto the first object on freelist.
    // Avoid loading first_page_, we we can infer it from the pointer;
    uintptr_t start = reinterpret_cast<uintptr_t>(ptr) & ~(kPageSize - 1);
    ObjIdx* host = IdxToPtr(freelist_, size, start);
    embed_count_++;
    host[embed_count_] = idx;
  } else {
    // Push onto freelist.
    *reinterpret_cast<ObjIdx*>(ptr) = freelist_;
    freelist_ = idx;
    embed_count_ = 0;
  }
  return true;
}

template <Span::Align align>
Span::ObjIdx Span::OffsetToIdx(uintptr_t offset, size_t size,
                               uint16_t reciprocal) {
  if (align == Align::SMALL) {
    return static_cast<ObjIdx>(
        // Add kBitmapScalingDenominator / 2 to round to nearest integer.
        ((offset >> kAlignmentShift) * reciprocal +
         kBitmapScalingDenominator / 2) /
        kBitmapScalingDenominator);
  } else {
    return static_cast<ObjIdx>(
        ((offset >> SizeMap::kMultiPageAlignmentShift) * reciprocal +
         kBitmapScalingDenominator / 2) /
        kBitmapScalingDenominator);
  }
}

template <Span::Align align>
Span::ObjIdx Span::BitmapPtrToIdx(void* ptr, size_t size) const {
  uintptr_t p = reinterpret_cast<uintptr_t>(ptr);
  uintptr_t off = static_cast<uint32_t>(p - first_page_.start_uintptr());
  ObjIdx idx = OffsetToIdx<align>(off, size, reciprocal_);
  ASSERT(BitmapIdxToPtr(idx, size) == ptr);
  return idx;
}

template <Span::Align align>
bool Span::BitmapPush(void* ptr, size_t size) {
#ifndef NDEBUG
  size_t before = bitmap_.CountBits(0, bitmap_.size());
#endif
  // TODO(djgove) Conversions to offsets can be computed outside of lock.
  ObjIdx idx = BitmapPtrToIdx<align>(ptr, size);
  // Check that the object is not already returned.
  ASSERT(bitmap_.GetBit(idx) == 0);
  // Set the bit indicating where the object was returned.
  bitmap_.SetBit(idx);
#ifndef NDEBUG
  size_t after = bitmap_.CountBits(0, bitmap_.size());
  ASSERT(before + 1 == after);
  ASSERT(allocated_.load(std::memory_order_relaxed) == embed_count_ - after);
#endif
  return true;
}

inline Span::Location Span::location() const {
  return static_cast<Location>(location_);
}

inline void Span::set_location(Location loc) {
  location_ = static_cast<uint64_t>(loc);
}

inline SampledAllocation* Span::sampled_allocation() const {
  ASSERT(sampled_);
  return sampled_allocation_;
}

inline bool Span::sampled() const { return sampled_; }

inline PageId Span::first_page() const { return first_page_; }

inline PageId Span::last_page() const {
  return first_page_ + num_pages_ - Length(1);
}

inline void Span::set_first_page(PageId p) {
  ASSERT(p > PageId{0});
  first_page_ = p;
}

inline void* Span::start_address() const {
  ASSERT(first_page_ > PageId{0});
  return first_page_.start_addr();
}

inline Length Span::num_pages() const { return num_pages_; }

inline void Span::set_num_pages(Length len) { num_pages_ = len; }

inline size_t Span::bytes_in_span() const { return num_pages_.in_bytes(); }

inline bool Span::FreelistEmpty(size_t size) const {
  if (UseBitmapForSize(size)) {
    return bitmap_.IsZero();
  } else {
    return cache_size_ == 0 && freelist_ == kListEnd;
  }
}

inline void Span::Prefetch() {
  // TODO(b/304135905): Will revisit this.
  // The first 16 bytes of a Span are the next and previous pointers
  // for when it is stored in a linked list. Since the sizeof(Span) is
  // 48 bytes, spans fit into 2 cache lines 50% of the time, with either
  // the first 16-bytes or the last 16-bytes in a different cache line.
  // Prefetch the cacheline that contains the most frequestly accessed
  // data by offseting into the middle of the Span.
  static_assert(sizeof(Span) <= 64, "Update span prefetch offset");
  PrefetchT0(&this->allocated_);
}

inline void Span::Init(PageId p, Length n) {
  ASSERT(p > PageId{0});
#ifndef NDEBUG
  // In debug mode we have additional checking of our list ops; these must be
  // initialized.
  new (this) Span();
#endif
  first_page_ = p;
  num_pages_ = n;
  location_ = IN_USE;
  sampled_ = 0;
  nonempty_index_ = 0;
  is_donated_ = 0;
}


inline bool Span::DoesNotFitInBitmap(size_t size,
                                     const size_t objects_per_span) {
  return Span::UseBitmapForSize(size) && objects_per_span > kBitmapSize;
}

inline bool Span::UseBitmapForSize(size_t size) {
  // Can fit kBitmapSize objects into a bitmap, so determine what the minimum
  // object size needs to be in order for that to work. This makes the
  // assumption that we don't increase the number of pages at a point where the
  // object count ends up exceeding kBitmapSize.
  static constexpr size_t kBitmapMinObjectSize = kPageSize / kBitmapSize;
  return size >= kBitmapMinObjectSize;
}

// This is equivalent to UseBitmapForSize, but instrusive-ness is the property
// callers care about, while use of bitmap is an implementation detail.
inline bool Span::IsNonIntrusive(size_t size) { return UseBitmapForSize(size); }

}  // namespace tcmalloc_internal
}  // namespace tcmalloc
GOOGLE_MALLOC_SECTION_END

#endif  // TCMALLOC_SPAN_H_
