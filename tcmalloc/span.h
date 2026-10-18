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

#include <algorithm>
#include <cassert>
#include <cstddef>

#include "absl/base/attributes.h"
#include "absl/base/casts.h"
#include "absl/base/nullability.h"
#include "absl/base/optimization.h"
#include "absl/base/thread_annotations.h"
#include "absl/types/span.h"
#include "tcmalloc/common.h"
#include "tcmalloc/internal/config.h"
#include "tcmalloc/internal/linked_list.h"
#include "tcmalloc/internal/logging.h"
#include "tcmalloc/internal/optimization.h"
#include "tcmalloc/internal/prefetch.h"
#include "tcmalloc/internal/range_tracker.h"
#include "tcmalloc/pages.h"

GOOGLE_MALLOC_SECTION_BEGIN
namespace tcmalloc {
namespace tcmalloc_internal {

// Denominator for bitmap scaling factor. The idea is that instead of dividing
// by N we multiply by M = kBitmapScalingDenominator / N and round the resulting
// value.
inline constexpr size_t kBitmapScalingDenominator = 1 << 30;

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

[[nodiscard]] inline uint32_t CalcReciprocal(size_t size) {
  // Calculate scaling factor. We want to avoid dividing by the size of the
  // object. Instead we'll multiply by a scaled version of the reciprocal.
  // We divide kBitmapScalingDenominator by the object size, so later we can
  // multiply by this reciprocal, and then divide this scaling factor out.
  TC_ASSERT_GT(size, 0);
  return kBitmapScalingDenominator / size;
}

[[nodiscard]] ABSL_ATTRIBUTE_ALWAYS_INLINE inline uint32_t OffsetToIdx(
    uintptr_t offset, uint32_t reciprocal) {
  // Add kBitmapScalingDenominator / 2 to round to nearest integer.
  return static_cast<uint32_t>(
      (offset * reciprocal + kBitmapScalingDenominator / 2) /
      kBitmapScalingDenominator);
}

// Information kept for a span (a contiguous run of pages).
//
// A span holds multiple small objects. The span is owned by CentralFreeList
// and is generally on CentralFreeList::nonempty_ list (unless it has no free
// objects).
class Span;
typedef TList<Span> SpanList;

class ABSL_CACHELINE_ALIGNED Span final : public SpanList::Elem {
 public:
  explicit Span(PageId first_page)
      : allocated_(0),
        nonempty_index_(0),
        first_page_(first_page.index()),
        bitmap_{} {
    TC_ASSERT_GT(first_page, PageId{0});
    TC_CHECK_LT(first_page.index(), static_cast<uint64_t>(1) << kMaxPageIdBits);
  }

  Span(const Span&) = delete;
  Span& operator=(const Span&) = delete;

  // Allocator/deallocator for spans. Note that these functions are defined
  // in static_vars.h, which is weird: see there for why.
  [[nodiscard]] static Span* absl_nonnull New(PageId first_page)
      ABSL_LOCKS_EXCLUDED(pageheap_lock);
  static void Delete(Span* absl_nonnull span);
  static void Delete(absl::Span<Span* absl_nonnull> spans);

  // Returns first page of the span.
  [[nodiscard]] PageId first_page() const;

  // Returns start address of the span.
  [[nodiscard]] ABSL_ATTRIBUTE_RETURNS_NONNULL void* start_address() const;

  // Returns number of objects allocated in the span.
  [[nodiscard]] uint16_t Allocated() const { return allocated_; }

  // Returns index of the non-empty list to which this span belongs to.
  [[nodiscard]] uint8_t nonempty_index() const { return nonempty_index_; }
  // Records an index of the non-empty list associated with this span.
  void set_nonempty_index(uint8_t index) {
    nonempty_index_ = index;
    TC_ASSERT_EQ(nonempty_index_, index);
  }

  // ---------------------------------------------------------------------------
  // Freelist management.
  // Used for spans in CentralFreelist to manage free objects.
  // These methods REQUIRE a SMALL_OBJECT span.
  // ---------------------------------------------------------------------------

  using ObjIdx = uint16_t;

  // Indicate whether the Span is empty.
  [[nodiscard]] bool FreelistEmpty(uint32_t objects_per_span) const;

  // Pushes ptr onto freelist unless the freelist becomes full, in which case
  // just return false.
  //
  // If the freelist becomes full, we do not push the object onto the freelist.
  [[nodiscard]] bool FreelistPushBatch(absl::Span<ObjIdx> batch,
                                       size_t size) __restrict__;

  // Pops up to N objects from the freelist and returns them in the batch array.
  // Returns number of objects actually popped.
  [[nodiscard]] size_t FreelistPopBatch(absl::Span<void* absl_nonnull> batch,
                                        size_t size) __restrict__;

  // Initializes freelist to contain all objects in the span.
  //  - size: the size of each object in the span.
  //  - count: the total number of objects in the span.
  // Populates up to batch.size() objects in the batch array.
  // Returns the number of objects actually placed in batch.
  [[nodiscard]] int BuildFreelist(size_t size, size_t count,
                                  absl::Span<void*> batch) __restrict__;

  // Prefetch cacheline containing most important span information.
  void Prefetch();

  // IsValidSizeClass verifies size class parameters from the Span perspective.
  [[nodiscard]] static bool IsValidSizeClass(size_t size, Length pages);

  // For bitmap'd spans conversion from an offset to an index is performed
  // by multiplying by the scaled reciprocal of the object size.
  [[nodiscard]] static uint32_t CalcReciprocal(size_t size) {
    return tcmalloc_internal::CalcReciprocal(size);
  }

  // Returns true if Span will use bitmap for objects of size <size>.
  [[nodiscard]] static bool UseBitmapForSize(size_t size);

  static void ObjectsToIdx(absl::Span<void* const> batch,
                           const Span* const* spans, size_t size,
                           uint32_t reciprocal, ObjIdx* idx);
  // Test-only helper that converts objects to indexes for a single span.
  static void ObjectsToIdx(absl::Span<void* const> batch, const Span* span,
                           size_t size, uint32_t reciprocal, ObjIdx* idx);

  static constexpr size_t kNonemptyIndexBits = 8;

 private:
  // See the comment on freelist organization in cc file.
  static constexpr ObjIdx kListEnd = -1;

  // Actual number of objects that we may cache. This is lower than the total
  // cache array size. Some cache entries are reserved or are used for other
  // purposes.
  //
  // TODO(b/527641380): With the enlarged bitmap, microbenchmarks on a single,
  // cache-resident span regress.  Investigate this.  We use a larger cache
  // anyways because the multi-span benchmarks that put more pressure on the
  // memory subsystem improve anyways.
#ifdef TCMALLOC_INTERNAL_LEGACY_LOCKING2
  static constexpr size_t kCacheSize = 12;
#else
  static constexpr size_t kCacheSize = 13;
#endif

  static constexpr size_t kMaxPageIdBits = kAddressBits - kPageShift;
#ifdef TCMALLOC_INTERNAL_LEGACY_LOCKING2
  // For available objects stored as a compressed linked list, the index of the
  // first object in recorded in freelist_.
  //
  // TODO(b/527641380): Move these into list_.
  struct {
    uint16_t embed_count_ = 0;
    uint16_t freelist_ = 0;
  };
  uint16_t allocated_;  // Number of non-free objects
  uint8_t cache_size_ = 0;
  uint8_t nonempty_index_;  // The nonempty_ list index for this span.

  // The number of bits of the cache space that may be used for bitmap.
  static constexpr size_t kBitmapSize = 8 * sizeof(ObjIdx) * kCacheSize;

  uint64_t first_page_ : kMaxPageIdBits;  // Starting page number.

  struct ListSpanState {
    // Embed cache of free objects.
    ObjIdx cache[Span::kCacheSize];
  };
#else
  uint16_t allocated_;      // Number of non-free objects
  uint8_t nonempty_index_;  // The nonempty_ list index for this span.

  uint64_t first_page_;  // Starting page number.

  struct ListSpanState {
    uint16_t embed_count = 0;
    uint16_t freelist = 0;
    uint8_t cache_size = 0;
    // Embed cache of free objects.
    ObjIdx cache[Span::kCacheSize];
  };

  // The number of bits of the cache space that may be used for bitmap.
  static constexpr size_t kBitmapSize = 8 * sizeof(ListSpanState);
#endif
  union {
    ListSpanState list_;

    // Used for spans with in CentralFreeList with fewer than 192 objects.  Each
    // bit is set to one when the object is available, and zero when the object
    // is used.
    Bitmap<kBitmapSize> bitmap_;
  };

#ifdef TCMALLOC_INTERNAL_LEGACY_LOCKING2
  [[nodiscard]] ABSL_ATTRIBUTE_ALWAYS_INLINE uint16_t embed_count() const {
    return embed_count_;
  }
  ABSL_ATTRIBUTE_ALWAYS_INLINE void set_embed_count(uint16_t v) {
    embed_count_ = v;
  }
  [[nodiscard]] ABSL_ATTRIBUTE_ALWAYS_INLINE uint16_t freelist() const {
    return freelist_;
  }
  ABSL_ATTRIBUTE_ALWAYS_INLINE void set_freelist(uint16_t v) { freelist_ = v; }
  [[nodiscard]] ABSL_ATTRIBUTE_ALWAYS_INLINE uint8_t cache_size() const {
    return cache_size_;
  }
  ABSL_ATTRIBUTE_ALWAYS_INLINE void set_cache_size(uint8_t v) {
    cache_size_ = v;
  }
  static_assert(sizeof(list_) == 24);
#else
  [[nodiscard]] ABSL_ATTRIBUTE_ALWAYS_INLINE uint16_t embed_count() const {
    return list_.embed_count;
  }
  ABSL_ATTRIBUTE_ALWAYS_INLINE void set_embed_count(uint16_t v) {
    list_.embed_count = v;
  }
  [[nodiscard]] ABSL_ATTRIBUTE_ALWAYS_INLINE uint16_t freelist() const {
    return list_.freelist;
  }
  ABSL_ATTRIBUTE_ALWAYS_INLINE void set_freelist(uint16_t v) {
    list_.freelist = v;
  }
  [[nodiscard]] ABSL_ATTRIBUTE_ALWAYS_INLINE uint8_t cache_size() const {
    return list_.cache_size;
  }
  ABSL_ATTRIBUTE_ALWAYS_INLINE void set_cache_size(uint8_t v) {
    list_.cache_size = v;
  }
  static_assert(sizeof(list_) == 32);
#endif
  // There is nothing inherently fixed about this size, but it is a useful
  // indicator that we are using the space and not unintentionally regressing
  // it.
  static_assert(sizeof(bitmap_) == sizeof(list_),
                "Bitmap and List representations should be equivalent to "
                "maximize byte efficiency");

  [[nodiscard]] size_t ListPopBatch(void** __restrict batch, size_t N,
                                    size_t size) __restrict__;

  void ListPushBatch(absl::Span<ObjIdx> batch, size_t size) __restrict__;
  void BitmapPushBatch(absl::Span<ObjIdx> batch, size_t size) __restrict__;

  // A bitmap is used to indicate object availability for spans containing
  // 64 or fewer objects.
  void BuildBitmap(size_t size, size_t count) __restrict__;

  // For spans with kBitmapSize or fewer objects populate batch with up to N
  // objects.  Returns number of objects actually popped.
  [[nodiscard]] size_t BitmapPopBatch(absl::Span<void*> batch,
                                      size_t size) __restrict__;

  // Convert object pointer <-> freelist index.
  [[nodiscard]] ObjIdx PtrToIdx(void* ptr, size_t size) const;
  [[nodiscard]] ObjIdx* IdxToPtr(ObjIdx idx, size_t size,
                                 uintptr_t start) const;

  // Convert object pointer <-> freelist index for bitmap managed objects.
  [[nodiscard]] ObjIdx BitmapPtrToIdx(void* ptr, size_t size,
                                      uint32_t reciprocal) const;
  [[nodiscard]] void* BitmapIdxToPtr(ObjIdx idx, size_t size) const;
  [[nodiscard]] void* BitmapIdxToPtr(ObjIdx idx, size_t size,
                                     uintptr_t start) const;
};

inline void Span::ObjectsToIdx(absl::Span<void* const> batch,
                               const Span* const* spans, size_t size,
                               uint32_t reciprocal, ObjIdx* idx) {
  if (Span::UseBitmapForSize(size)) {
    for (int i = 0; i < batch.size(); ++i) {
      idx[i] = spans[i]->BitmapPtrToIdx(batch[i], size, reciprocal);
    }
  } else {
    for (int i = 0; i < batch.size(); ++i) {
      idx[i] = spans[i]->PtrToIdx(batch[i], size);
    }
  }
}

inline void Span::ObjectsToIdx(absl::Span<void* const> batch, const Span* span,
                               size_t size, uint32_t reciprocal, ObjIdx* idx) {
  TC_ASSERT_LE(batch.size(), kMaxObjectsToMove);
  const Span* spans[kMaxObjectsToMove];
  std::fill_n(spans, batch.size(), span);
  ObjectsToIdx(batch, spans, size, reciprocal, idx);
}

inline Span::ObjIdx* Span::IdxToPtr(ObjIdx idx, size_t size,
                                    uintptr_t start) const {
  TC_ASSERT_EQ(start, first_page().start_uintptr());
  TC_ASSERT_NE(idx, kListEnd);
  uintptr_t off = start + (static_cast<uintptr_t>(idx) << kAlignmentShift);
  ObjIdx* ptr = reinterpret_cast<ObjIdx*>(off);
  TC_ASSERT_EQ(PtrToIdx(ptr, size), idx);
  return ptr;
}

inline Span::ObjIdx Span::PtrToIdx(void* ptr, size_t size) const {
  // Object index is an offset from span start divided by kAlignment.
  uintptr_t p = reinterpret_cast<uintptr_t>(ptr);
  // Classes that use freelist must also use 1 page per span,
  // so don't load first_page_ (may be on a different cache line).
  TC_ASSERT_EQ(PageIdContaining(ptr), first_page());
  uintptr_t off = (p & (kPageSize - 1)) >> kAlignmentShift;
  ObjIdx idx = static_cast<ObjIdx>(off);
  TC_ASSERT_NE(idx, kListEnd);
  TC_ASSERT_EQ(idx, off);
  TC_ASSERT_EQ(p, first_page().start_uintptr() +
                      (static_cast<uintptr_t>(idx) << kAlignmentShift));
  return idx;
}

inline bool Span::FreelistPushBatch(absl::Span<ObjIdx> batch,
                                    size_t size) __restrict__ {
  const uint16_t allocated = allocated_;
  TC_ASSERT_GE(allocated, batch.size());
  if (ABSL_PREDICT_FALSE(allocated == batch.size())) {
    return false;
  }
  allocated_ = allocated - batch.size();
  // Bitmaps are used to record object availability when there are no more than
  // kBitmapSize objects in a span.
  if (ABSL_PREDICT_TRUE(UseBitmapForSize(size))) {
    BitmapPushBatch(batch, size);
    return true;
  }
  ListPushBatch(batch, size);
  return true;
}

inline void Span::ListPushBatch(absl::Span<ObjIdx> batch,
                                size_t size) __restrict__ {
#ifdef TCMALLOC_INTERNAL_LEGACY_LOCKING2
  if (this->cache_size() < kCacheSize) {
    auto cache_writes = std::min(kCacheSize - this->cache_size(), batch.size());
    for (int i = 0; i < cache_writes; ++i) {
      // Have empty space in the cache, push there.
      const ObjIdx idx = batch[i];
      list_.cache[this->cache_size() + i] = idx;
    }
    this->set_cache_size(this->cache_size() + cache_writes);
    batch.remove_prefix(cache_writes);
  }
#else
  const size_t csize = this->cache_size();
  ASSUME(csize <= kCacheSize);
  if (csize < kCacheSize) {
    const size_t cache_writes = std::min(kCacheSize - csize, batch.size());
    for (size_t i = 0; i < cache_writes; ++i) {
      // Have empty space in the cache, push there.
      const ObjIdx idx = batch[i];
      list_.cache[csize + i] = idx;
    }
    this->set_cache_size(csize + cache_writes);
    batch.remove_prefix(cache_writes);
  }
#endif

  if (batch.empty()) {
    return;
  }

  const uintptr_t start = absl::bit_cast<uintptr_t>(start_address());

  ObjIdx freelist = this->freelist();
  uint16_t embed_count = this->embed_count();

  ObjIdx* __restrict host;
  if (ABSL_PREDICT_TRUE(freelist != kListEnd)) {
    host = IdxToPtr(freelist, size, start);
  } else {
    ObjIdx idx = batch[0];
    batch.remove_prefix(1);

    host = IdxToPtr(idx, size, start);
    *host = kListEnd;
    freelist = idx;
    embed_count = 0;
  }

  TC_ASSERT_NE(freelist, kListEnd);

  // -1 because the first slot is used by freelist link.
  const size_t limit = size / sizeof(ObjIdx) - 1;

  for (const ObjIdx idx : batch) {
    if (ABSL_PREDICT_TRUE(embed_count != limit)) {
      // Push onto the first object on freelist.
      embed_count++;
      host[embed_count] = idx;
    } else {
      // Push onto freelist.
      ObjIdx* __restrict new_host = IdxToPtr(idx, size, start);
      *new_host = freelist;
      freelist = idx;
      embed_count = 0;
      host = new_host;
    }
  }
  this->set_freelist(freelist);
  this->set_embed_count(embed_count);
}

inline void* Span::BitmapIdxToPtr(ObjIdx idx, size_t size,
                                  uintptr_t start) const {
  TC_ASSERT_EQ(start, first_page().start_uintptr());
  uintptr_t off = start + idx * size;
  return reinterpret_cast<void*>(off);
}

inline void* Span::BitmapIdxToPtr(ObjIdx idx, size_t size) const {
  return BitmapIdxToPtr(idx, size, first_page().start_uintptr());
}

inline Span::ObjIdx Span::BitmapPtrToIdx(void* ptr, size_t size,
                                         uint32_t reciprocal) const {
  uintptr_t p = reinterpret_cast<uintptr_t>(ptr);
  uintptr_t off = static_cast<uint32_t>(p - first_page().start_uintptr());
  ObjIdx idx = static_cast<ObjIdx>(OffsetToIdx(off, reciprocal));
  TC_ASSERT_EQ(BitmapIdxToPtr(idx, size), ptr);
  return idx;
}

inline void Span::BitmapPushBatch(absl::Span<ObjIdx> batch,
                                  size_t size) __restrict__ {
  size_t before = bitmap_.CountBits();
  for (const ObjIdx idx : batch) {
    // Check that the object is not already returned.
    TC_ASSERT_EQ(bitmap_.GetBit(idx), 0);
    // Set the bit indicating where the object was returned.
    bitmap_.SetBit(idx);
  }
  TC_ASSERT_EQ(before + batch.size(), bitmap_.CountBits());
}

inline PageId Span::first_page() const { return PageId(first_page_); }

inline void* Span::start_address() const {
  TC_ASSERT_GT(first_page(), PageId{0});
  return first_page().start_addr();
}

inline bool Span::FreelistEmpty(uint32_t objects_per_span) const {
  return allocated_ == objects_per_span;
}

inline void Span::Prefetch() { PrefetchW(this); }

inline bool Span::IsValidSizeClass(size_t size, Length pages) {
  if (Span::UseBitmapForSize(size)) {
    size_t objects = pages.in_bytes() / size;
    return objects <= kBitmapSize;
  } else {
    return pages == Length(1);
  }
}

inline bool Span::UseBitmapForSize(size_t size) {
  // Can fit kBitmapSize objects into a bitmap, so determine what the minimum
  // object size needs to be in order for that to work. This makes the
  // assumption that we don't increase the number of pages at a point where the
  // object count ends up exceeding kBitmapSize.
  static constexpr size_t kBitmapMinObjectSize = kPageSize / kBitmapSize;
  return size >= kBitmapMinObjectSize;
}

inline size_t Span::BitmapPopBatch(absl::Span<void*> batch,
                                   size_t size) __restrict__ {
  void** ptrs = batch.data();
  const uintptr_t span_start = first_page().start_uintptr();
  size_t popped = bitmap_.PopBatch(
      [&](size_t offset) {
        *ptrs++ = BitmapIdxToPtr(static_cast<ObjIdx>(offset), size, span_start);
      },
      batch.size());
  allocated_ += popped;
  return popped;
}

ABSL_ATTRIBUTE_ALWAYS_INLINE inline size_t Span::FreelistPopBatch(
    const absl::Span<void* absl_nonnull> batch, size_t size) __restrict__ {
  // Handle spans with bitmap.size() or fewer objects using a bitmap. We expect
  // spans to frequently hold smaller objects.
  if (ABSL_PREDICT_TRUE(UseBitmapForSize(size))) {
    return BitmapPopBatch(batch, size);
  }
  return ListPopBatch(batch.data(), batch.size(), size);
}

inline size_t Span::ListPopBatch(void** __restrict batch, size_t N,
                                 size_t size) __restrict__ {
  size_t result = 0;

  // Pop from cache.
  auto csize = this->cache_size();
  ASSUME(csize <= kCacheSize);
  auto cache_reads = csize < N ? csize : N;
  const uintptr_t span_start = first_page().start_uintptr();
  for (; result < cache_reads; result++) {
    batch[result] = IdxToPtr(list_.cache[csize - result - 1], size, span_start);
  }

  // Store this->cache_size_ one time.
  this->set_cache_size(csize - result);

#ifdef TCMALLOC_INTERNAL_LEGACY_LOCKING2
  while (result < N) {
    if (ABSL_PREDICT_FALSE(this->freelist() == kListEnd)) {
      break;
    }

    ObjIdx* const host = IdxToPtr(this->freelist(), size, span_start);
    uint16_t embed_count = this->embed_count();

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

    if (result == N) {
      this->set_embed_count(embed_count);
      break;
    }

    // The first object on the freelist is empty, pop it.
    TC_ASSERT_EQ(embed_count, 0);

    batch[result] = host;
    result++;

    this->set_freelist(host[0]);
    this->set_embed_count(size / sizeof(ObjIdx) - 1);
  }
#else
  if (result < N) {
    ObjIdx freelist = this->freelist();
    uint16_t embed_count = this->embed_count();
    const uint16_t max_embed = size / sizeof(ObjIdx) - 1;
    while (result < N && ABSL_PREDICT_TRUE(freelist != kListEnd)) {
      ObjIdx* const __restrict host = IdxToPtr(freelist, size, span_start);
      const size_t iter = std::min<size_t>(embed_count, N - result);
      for (size_t i = 0; i < iter; i++) {
        // Pop from the first object on freelist.
        batch[result + i] = IdxToPtr(host[embed_count - i], size, span_start);
      }
      embed_count -= iter;
      result += iter;

      if (result == N) {
        break;
      }

      // The first object on the freelist is empty, pop it.
      TC_ASSERT_EQ(embed_count, 0);

      batch[result] = host;
      result++;

      freelist = host[0];
      embed_count = max_embed;
    }
    this->set_freelist(freelist);
    this->set_embed_count(embed_count);
  }
#endif
  allocated_ += result;
  return result;
}

}  // namespace tcmalloc_internal
}  // namespace tcmalloc
GOOGLE_MALLOC_SECTION_END

#endif  // TCMALLOC_SPAN_H_
