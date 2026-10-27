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
#include <climits>
#include <cstddef>

#include "absl/base/attributes.h"
#include "absl/base/casts.h"
#include "absl/base/nullability.h"
#include "absl/base/optimization.h"
#include "absl/base/thread_annotations.h"
#include "absl/numeric/bits.h"
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
      : embed_count_(0),
        freelist_(0),
        allocated_(0),
        cache_size_(0),
        nonempty_index_(0),
        level2_bitmap_(0),
        first_page_(first_page.index()),
        raw_bitmap_{} {
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

  enum class BitmapMode : uint8_t {
    kDisabled,     // Legacy bitmap / linked-list
    kInline64,     // <= 64 objects: 1-word L1 in raw_bitmap_[0], no L2
                   // Theoretically we could use kInline256 mode here, but
                   // kInline64 is faster since it does not use L2 bitmaps.
    kInline256,    // 65..256 objects: L1 in raw_bitmap_, L2 in level2_bitmap_
    kOutlineL2_1,  // > 256 objects: outline L1, 1-word L2 in raw_bitmap_
    kOutlineL2_4,  // > 4096 objects: outline L1, 2..4-word L2 in raw_bitmap_
    kOutlineL2_8,  // > 16384 objects: outline L1, 4-word L2 (1 bit = 2 words)
  };

  struct BitmapInfo {
    uint32_t objects_per_span;
    BitmapMode mode;
  };
  [[nodiscard]] static BitmapInfo CalcBitmapInfo(bool use_bitmap_for_small,
                                                 Length pages_per_span,
                                                 size_t object_size);
  // Indicate whether the Span is empty.
  [[nodiscard]] bool FreelistEmpty(uint32_t objects_per_span) const;

  // Pushes ptr onto freelist unless the freelist becomes full, in which case
  // just return false.
  //
  // If the freelist becomes full, we do not push the object onto the freelist.
  [[nodiscard]] bool FreelistPushBatch(absl::Span<ObjIdx> batch, size_t size,
                                       BitmapMode mode) __restrict__;

  // Pops up to N objects from the freelist and returns them in the batch array.
  // Returns number of objects actually popped.
  [[nodiscard]] size_t FreelistPopBatch(absl::Span<void* absl_nonnull> batch,
                                        size_t size, size_t objects_per_span,
                                        BitmapMode mode) __restrict__;

  // Initializes freelist to contain all objects in the span.
  //  - size: the size of each object in the span.
  //  - count: the total number of objects in the span.
  // Populates up to batch.size() objects in the batch array.
  // Returns the number of objects actually placed in batch.
  [[nodiscard]] int BuildFreelist(size_t size, size_t count,
                                  absl::Span<void*> batch,
                                  BitmapMode mode) __restrict__;

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
  [[nodiscard]] static bool UseBitmapForSize(size_t size, BitmapMode mode);

  static void ObjectsToIdx(absl::Span<void* const> batch, Span** spans,
                           size_t size, uint32_t reciprocal, ObjIdx* idx,
                           BitmapMode mode);
  // Test-only helper that converts objects to indexes for a single span.
  static void ObjectsToIdx(absl::Span<void* const> batch, Span* span,
                           size_t size, uint32_t reciprocal, ObjIdx* idx,
                           BitmapMode mode);

  static constexpr size_t kNonemptyIndexBits = 8;

  using BitmapElem = uint64_t;
  constexpr static size_t kRawBitmapSize = 4;
  constexpr static size_t kBitsPerWord = sizeof(BitmapElem) * CHAR_BIT;

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
  static constexpr size_t kCacheSize = 12;

  static constexpr size_t kMaxPageIdBits = kAddressBits - kPageShift;
  // For available objects stored as a compressed linked list, the index of the
  // first object in recorded in freelist_.
  //
  // TODO(b/527641380): Move these into list_.
  struct {
    uint16_t embed_count_;
    uint16_t freelist_;
  };
  uint16_t allocated_;  // Number of non-free objects
  uint8_t cache_size_;
  uint8_t nonempty_index_;  // The nonempty_ list index for this span.

  // The number of bits of the cache space that may be used for bitmap.
  static constexpr size_t kBitmapSize = 8 * sizeof(ObjIdx) * kCacheSize;

  union {
    struct {
      uint64_t level2_bitmap_ : kRawBitmapSize;
      uint64_t first_page_ : kMaxPageIdBits;  // Starting page number.
      static_assert(kRawBitmapSize + kMaxPageIdBits <= 64);
    };
    // Used to pass level2_bitmap_ to DynamicBitmapPushImpl/DynamicBitmapPopWord
    // as a BitmapElem* in kInline256 mode. Only the low kRawBitmapSize bits are
    // ever set or cleared (since popped objects are capped at available),
    // leaving first_page_ in the upper bits intact.
    BitmapElem level2_bitmap_word_;
  };

  struct ListSpanState {
    // Embed cache of free objects.
    ObjIdx cache[Span::kCacheSize];
  };
  union {
    ListSpanState list_;

    // Used for spans with in CentralFreeList with fewer than 192 objects.  Each
    // bit is set to one when the object is available, and zero when the object
    // is used.
    Bitmap<kBitmapSize> bitmap_;
    uint64_t raw_bitmap_[kRawBitmapSize];
  };

  // There is nothing inherently fixed about this size, but it is a useful
  // indicator that we are using the space and not unintentionally regressing
  // it.
  static_assert(sizeof(list_) == 24);
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

  [[nodiscard]] BitmapElem* DynamicBitmap(BitmapMode mode);
  void DynamicBitmapPush(absl::Span<ObjIdx> batch,
                         BitmapMode mode) __restrict__;
  template <BitmapMode kMode>
  GOOGLE_MALLOC_SECTION static void DynamicBitmapPushImpl(
      absl::Span<const ObjIdx> batch, BitmapElem* bm, BitmapElem* bm2);
  [[nodiscard]] size_t DynamicBitmapPop(absl::Span<void* absl_nonnull> batch,
                                        size_t size, size_t objects_per_span,
                                        BitmapMode mode) __restrict__;
  template <BitmapMode kMode>
  GOOGLE_MALLOC_SECTION void DynamicBitmapPopWord(
      char* span_start, void**& batch_pos, void** batch_end, size_t size,
      size_t objects_per_span, BitmapElem* bm, BitmapElem* bm2);
  template <BitmapMode kMode>
  [[nodiscard]] GOOGLE_MALLOC_SECTION BitmapElem DynamicBitmapPopBitmap(
      char* word_start, void**& batch_pos, void** batch_end, size_t size,
      size_t objects_per_span, BitmapElem bitmap);
  [[nodiscard]] int DynamicBitmapBuild(size_t size, size_t count,
                                       absl::Span<void*> batch,
                                       BitmapMode mode) __restrict__;

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

inline void Span::ObjectsToIdx(absl::Span<void* const> batch, Span** spans,
                               size_t size, uint32_t reciprocal, ObjIdx* idx,
                               BitmapMode mode) {
  if (ABSL_PREDICT_TRUE(mode != BitmapMode::kDisabled) ||
      Span::UseBitmapForSize(size, mode)) {
    for (int i = 0; i < batch.size(); ++i) {
      idx[i] = spans[i]->BitmapPtrToIdx(batch[i], size, reciprocal);
      if (mode >= BitmapMode::kOutlineL2_1) {
        PrefetchW(spans[i]->DynamicBitmap(mode) - idx[i] / kBitsPerWord);
      }
    }
  } else {
    for (int i = 0; i < batch.size(); ++i) {
      idx[i] = spans[i]->PtrToIdx(batch[i], size);
    }
  }
}

inline void Span::ObjectsToIdx(absl::Span<void* const> batch, Span* span,
                               size_t size, uint32_t reciprocal, ObjIdx* idx,
                               BitmapMode mode) {
  TC_ASSERT_LE(batch.size(), kMaxObjectsToMove);
  Span* spans[kMaxObjectsToMove];
  std::fill_n(spans, batch.size(), span);
  ObjectsToIdx(batch, spans, size, reciprocal, idx, mode);
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

ABSL_ATTRIBUTE_ALWAYS_INLINE inline bool Span::FreelistPushBatch(
    absl::Span<ObjIdx> batch, size_t size, BitmapMode mode) __restrict__ {
  TC_ASSERT(!batch.empty());
  const uint16_t allocated = allocated_;
  TC_ASSERT_GE(allocated, batch.size());
  if (ABSL_PREDICT_FALSE(allocated == batch.size())) {
    return false;
  }
  allocated_ = allocated - batch.size();
  if (ABSL_PREDICT_TRUE(mode != BitmapMode::kDisabled)) {
    DynamicBitmapPush(batch, mode);
    return true;
  }
  // Bitmaps are used to record object availability when there are no more than
  // kBitmapSize objects in a span.
  if (ABSL_PREDICT_TRUE(UseBitmapForSize(size, mode))) {
    BitmapPushBatch(batch, size);
    return true;
  }
  ListPushBatch(batch, size);
  return true;
}

inline void Span::ListPushBatch(absl::Span<ObjIdx> batch,
                                size_t size) __restrict__ {
  if (cache_size_ < kCacheSize) {
    auto cache_writes = std::min(kCacheSize - cache_size_, batch.size());
    for (int i = 0; i < cache_writes; ++i) {
      // Have empty space in the cache, push there.
      const ObjIdx idx = batch[i];
      list_.cache[cache_size_ + i] = idx;
    }
    cache_size_ += cache_writes;
    batch.remove_prefix(cache_writes);
  }

  if (batch.empty()) {
    return;
  }

  const uintptr_t start = absl::bit_cast<uintptr_t>(start_address());

  ObjIdx freelist = freelist_;
  uint16_t embed_count = embed_count_;

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
  freelist_ = freelist;
  embed_count_ = embed_count;
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
  // Note: we don't have any specific requirements for the use_bitmap_for_small
  // mode.
  if (Span::UseBitmapForSize(size, BitmapMode::kDisabled)) {
    size_t objects = pages.in_bytes() / size;
    return objects <= kBitmapSize;
  } else {
    return pages == Length(1);
  }
}

inline bool Span::UseBitmapForSize(size_t size, BitmapMode mode) {
  TC_ASSERT_EQ(mode, BitmapMode::kDisabled);
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

inline size_t Span::FreelistPopBatch(const absl::Span<void* absl_nonnull> batch,
                                     size_t size, size_t objects_per_span,
                                     BitmapMode mode) __restrict__ {
  if (ABSL_PREDICT_TRUE(mode != BitmapMode::kDisabled)) {
    return DynamicBitmapPop(batch, size, objects_per_span, mode);
  }
  // Handle spans with bitmap.size() or fewer objects using a bitmap. We expect
  // spans to frequently hold smaller objects.
  if (ABSL_PREDICT_TRUE(UseBitmapForSize(size, mode))) {
    return BitmapPopBatch(batch, size);
  }
  return ListPopBatch(batch.data(), batch.size(), size);
}

inline size_t Span::ListPopBatch(void** __restrict batch, size_t N,
                                 size_t size) __restrict__ {
  size_t result = 0;

  // Pop from cache.
  auto csize = cache_size_;
  ASSUME(csize <= kCacheSize);
  auto cache_reads = csize < N ? csize : N;
  const uintptr_t span_start = first_page().start_uintptr();
  for (; result < cache_reads; result++) {
    batch[result] = IdxToPtr(list_.cache[csize - result - 1], size, span_start);
  }

  // Store this->cache_size_ one time.
  cache_size_ = csize - result;

  while (result < N) {
    if (ABSL_PREDICT_FALSE(freelist_ == kListEnd)) {
      break;
    }

    ObjIdx* const host = IdxToPtr(freelist_, size, span_start);
    uint16_t embed_count = embed_count_;

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
      embed_count_ = embed_count;
      break;
    }

    // The first object on the freelist is empty, pop it.
    TC_ASSERT_EQ(embed_count, 0);

    batch[result] = host;
    result++;

    freelist_ = host[0];
    embed_count_ = size / sizeof(ObjIdx) - 1;
  }
  allocated_ += result;
  return result;
}

inline Span::BitmapInfo Span::CalcBitmapInfo(bool use_bitmap_for_small,
                                             Length pages_per_span,
                                             size_t object_size) {
  const size_t span_bytes = pages_per_span.in_bytes();
  uint32_t objects_per_span = span_bytes / object_size;
  if (!use_bitmap_for_small) {
    return {objects_per_span, BitmapMode::kDisabled};
  }
  if (objects_per_span <= kBitsPerWord) {
    return {objects_per_span, BitmapMode::kInline64};
  }
  if (objects_per_span <= kRawBitmapSize * kBitsPerWord) {
    return {objects_per_span, BitmapMode::kInline256};
  }
  // TODO: currently we take a bunch of space from the end of the span
  // for the bitmap (up to 4K), in same cases we waste much more than we need
  // due to rounding up to the object size.
  // Consider allocating bitmaps separately and storing a pointer in the bitmap.
  // Or, alternatively we may consider placing the bitmap in the first free
  // object if it's large enough. Then we both have a large bitmap, and don't
  // take any additional space.
  // Note: we only need/use the bitmap for spans that are tracked in CFL
  // (which is <40% of all spans).
  size_t bitmap_words;
  for (;; --objects_per_span) {
    bitmap_words = (objects_per_span + kBitsPerWord - 1) / kBitsPerWord;
    if (bitmap_words > kRawBitmapSize * kBitsPerWord) {
      // In kOutlineL2_8 mode, each bit in the 2nd-level bitmap represents
      // 2 words in the 1st-level bitmap. Round up to an even number of words
      // so we can always access word pairs without bounds checks.
      bitmap_words = (bitmap_words + 1) & ~size_t{1};
    }
    if (objects_per_span * object_size + bitmap_words * sizeof(BitmapElem) <=
        span_bytes) {
      break;
    }
  }
  BitmapMode mode = BitmapMode::kOutlineL2_1;
  if (bitmap_words > kRawBitmapSize * kBitsPerWord) {
    mode = BitmapMode::kOutlineL2_8;
  } else if (bitmap_words > kBitsPerWord) {
    mode = BitmapMode::kOutlineL2_4;
  }
  // TODO: consider placing the Span itself at the end of the span.
  // Since we take some memory out of the span for the metadata, we can then
  // place the Span there as well. This would allow to locate and prefetch
  // Span w/o touching 3-level page map.

  // kOutline* modes indexe backwards from start_address() + kPageSize, which
  // requires a 1-page span.
  TC_CHECK_EQ(pages_per_span, Length(1));
  return {objects_per_span, mode};
}

inline Span::BitmapElem* Span::DynamicBitmap(BitmapMode mode) {
  TC_ASSERT_NE(mode, BitmapMode::kDisabled);
  switch (mode) {
    case BitmapMode::kInline64:
    case BitmapMode::kInline256:
      return &raw_bitmap_[kRawBitmapSize - 1];
    case BitmapMode::kOutlineL2_1:
    case BitmapMode::kOutlineL2_4:
    case BitmapMode::kOutlineL2_8:
      return reinterpret_cast<BitmapElem*>(static_cast<char*>(start_address()) +
                                           kPageSize) -
             1;
    case BitmapMode::kDisabled:
      ABSL_UNREACHABLE();
  }
}

ABSL_ATTRIBUTE_ALWAYS_INLINE inline void Span::DynamicBitmapPush(
    absl::Span<ObjIdx> batch, BitmapMode mode) __restrict__ {
  TC_ASSERT_NE(mode, BitmapMode::kDisabled);
  TC_ASSERT(!batch.empty());
  switch (mode) {
    case BitmapMode::kInline64:
      DynamicBitmapPushImpl<BitmapMode::kInline64>(batch, DynamicBitmap(mode),
                                                   nullptr);
      break;
    case BitmapMode::kInline256:
      DynamicBitmapPushImpl<BitmapMode::kOutlineL2_1>(
          batch, DynamicBitmap(mode), &level2_bitmap_word_);
      break;
    case BitmapMode::kOutlineL2_1:
      DynamicBitmapPushImpl<BitmapMode::kOutlineL2_1>(
          batch, DynamicBitmap(mode), raw_bitmap_);
      break;
    case BitmapMode::kOutlineL2_4:
      DynamicBitmapPushImpl<BitmapMode::kOutlineL2_4>(
          batch, DynamicBitmap(mode), raw_bitmap_);
      break;
    case BitmapMode::kOutlineL2_8:
      DynamicBitmapPushImpl<BitmapMode::kOutlineL2_8>(
          batch, DynamicBitmap(mode), raw_bitmap_);
      break;
    case BitmapMode::kDisabled:
      ABSL_UNREACHABLE();
  }
}

template <Span::BitmapMode kMode>
ABSL_ATTRIBUTE_ALWAYS_INLINE inline void Span::DynamicBitmapPushImpl(
    absl::Span<const ObjIdx> batch, BitmapElem* bm, BitmapElem* bm2) {
  // Prevent codegen for modes that are only reachable with 256K pages
  // (with <= 32K pages and >= 8B objects, a 1-page span has <= 4096 objects,
  // which fits in <= 64 L1 words and uses at most kOutlineL2_1).
  if constexpr (kPageSize != (256 << 10) &&
                (kMode == BitmapMode::kOutlineL2_4 ||
                 kMode == BitmapMode::kOutlineL2_8)) {
    ABSL_UNREACHABLE();
  }

  // In kOutlineL2_8 mode, each L2 bit covers 2 L1 words.
  constexpr size_t kL2Shift = kMode == BitmapMode::kOutlineL2_8 ? 1 : 0;

  // Accumulate bits belonging to the same L1 word in cur_mask before writing
  // to bm. For kOutlineL2_1, also accumulate L2 bits in level2_mask to update
  // the single L2 word once at the end.
  TC_ASSERT(kMode != BitmapMode::kInline64 || batch[0] < kBitsPerWord);
  size_t cur_word =
      kMode == BitmapMode::kInline64 ? 0 : batch[0] / kBitsPerWord;
  BitmapElem cur_mask = BitmapElem{1} << (batch[0] % kBitsPerWord);
  BitmapElem level2_mask;
  if constexpr (kMode == BitmapMode::kOutlineL2_1) {
    TC_ASSERT_LT(cur_word, kBitsPerWord);
    level2_mask = BitmapElem{1} << cur_word;
  } else if constexpr (kMode != BitmapMode::kInline64) {
    const size_t l2_bit = cur_word >> kL2Shift;
    TC_ASSERT_LT(l2_bit, kRawBitmapSize * kBitsPerWord);
    bm2[l2_bit / kBitsPerWord] |= BitmapElem{1} << (l2_bit % kBitsPerWord);
  }

  for (size_t k = 1; k < batch.size(); ++k) {
    TC_ASSERT(kMode != BitmapMode::kInline64 || batch[k] < kBitsPerWord);
    const size_t w =
        kMode == BitmapMode::kInline64 ? 0 : batch[k] / kBitsPerWord;
    const BitmapElem bit = BitmapElem{1} << (batch[k] % kBitsPerWord);
    if (kMode == BitmapMode::kInline64 || w == cur_word) {
      TC_ASSERT_EQ(cur_mask & bit, 0);
      cur_mask |= bit;
    } else if constexpr (kMode != BitmapMode::kInline64) {
      // Flush accumulated mask for the previous L1 word and start a new word.
      TC_ASSERT_EQ(bm[-static_cast<ptrdiff_t>(cur_word)] & cur_mask, 0);
      bm[-static_cast<ptrdiff_t>(cur_word)] |= cur_mask;
      cur_word = w;
      cur_mask = bit;

      if constexpr (kMode == BitmapMode::kOutlineL2_1) {
        TC_ASSERT_LT(w, kBitsPerWord);
        level2_mask |= BitmapElem{1} << w;
      } else {
        const size_t l2_bit = w >> kL2Shift;
        TC_ASSERT_LT(l2_bit, kRawBitmapSize * kBitsPerWord);
        bm2[l2_bit / kBitsPerWord] |= BitmapElem{1} << (l2_bit % kBitsPerWord);
      }
    }
  }

  // Flush the final L1 word (and L2 word for kOutlineL2_1).
  TC_ASSERT_EQ(bm[-static_cast<ptrdiff_t>(cur_word)] & cur_mask, 0);
  bm[-static_cast<ptrdiff_t>(cur_word)] |= cur_mask;
  if constexpr (kMode == BitmapMode::kOutlineL2_1) {
    bm2[0] |= level2_mask;
  }
}

inline size_t Span::DynamicBitmapPop(absl::Span<void* absl_nonnull> batch,
                                     size_t size, size_t objects_per_span,
                                     BitmapMode mode) __restrict__ {
  const size_t allocated = allocated_;
  TC_ASSERT_LE(allocated, objects_per_span);
  const size_t available = objects_per_span - allocated;
  const size_t want = std::min(batch.size(), available);
  if (ABSL_PREDICT_FALSE(want == 0)) {
    return 0;
  }
  void** batch_pos = batch.data();
  void** const batch_end = batch_pos + want;
  char* const span_start = static_cast<char*>(start_address());
  switch (mode) {
    case BitmapMode::kInline64: {
      BitmapElem& bm = *DynamicBitmap(mode);
      bm = DynamicBitmapPopBitmap<BitmapMode::kInline64>(
          span_start, batch_pos, batch_end, size, objects_per_span, bm);
      break;
    }
    case BitmapMode::kInline256:
      DynamicBitmapPopWord<BitmapMode::kOutlineL2_1>(
          span_start, batch_pos, batch_end, size, objects_per_span,
          &raw_bitmap_[kRawBitmapSize - 1], &level2_bitmap_word_);
      break;
    case BitmapMode::kOutlineL2_1:
      DynamicBitmapPopWord<BitmapMode::kOutlineL2_1>(
          span_start, batch_pos, batch_end, size, objects_per_span,
          DynamicBitmap(mode), raw_bitmap_);
      break;
    case BitmapMode::kOutlineL2_4:
      DynamicBitmapPopWord<BitmapMode::kOutlineL2_4>(
          span_start, batch_pos, batch_end, size, objects_per_span,
          DynamicBitmap(mode), raw_bitmap_);
      break;
    case BitmapMode::kOutlineL2_8:
      DynamicBitmapPopWord<BitmapMode::kOutlineL2_8>(
          span_start, batch_pos, batch_end, size, objects_per_span,
          DynamicBitmap(mode), raw_bitmap_);
      break;
    case BitmapMode::kDisabled:
      ABSL_UNREACHABLE();
  }
  TC_ASSERT_EQ(batch_pos, batch_end);
  allocated_ = allocated + want;
  return want;
}

template <Span::BitmapMode kMode>
ABSL_ATTRIBUTE_ALWAYS_INLINE inline void Span::DynamicBitmapPopWord(
    char* span_start, void**& batch_pos, void** batch_end, size_t size,
    size_t objects_per_span, BitmapElem* bm, BitmapElem* bm2) {
  // Prevent codegen for modes that are only reachable with 256K pages.
  if constexpr (kPageSize != (256 << 10) &&
                (kMode == BitmapMode::kOutlineL2_4 ||
                 kMode == BitmapMode::kOutlineL2_8)) {
    ABSL_UNREACHABLE();
  }

  // DynamicBitmapPop caps the requested batch at available objects and returns
  // early if want == 0. Therefore, batch_pos == batch_end will always terminate
  // the loops before we run out of set bits in the bitmap. In kOutlineL2_1
  // mode (which has a single L2 word), we can elide the l2_word == 0 checks.
  for (size_t l2_idx = 0;; ++l2_idx) {
    TC_ASSERT_LT(l2_idx,
                 kMode == BitmapMode::kOutlineL2_1 ? 1 : kRawBitmapSize);
    BitmapElem l2_word = bm2[l2_idx];
    if (kMode != BitmapMode::kOutlineL2_1 && l2_word == 0) {
      continue;
    }

    do {
      TC_ASSERT_NE(l2_word, 0);
      const int l2_bit = absl::countr_zero(l2_word);
      constexpr size_t kL2Shift = kMode == BitmapMode::kOutlineL2_8 ? 1 : 0;
      const size_t w = (l2_idx * kBitsPerWord + l2_bit) << kL2Shift;
      TC_ASSERT_LT(w, (objects_per_span + kBitsPerWord - 1) / kBitsPerWord);
      const ptrdiff_t bm_idx = -static_cast<ptrdiff_t>(w);

      BitmapElem bitmap = bm[bm_idx];
      if (kMode != BitmapMode::kOutlineL2_8 || bitmap != 0) {
        char* const word_start = span_start + (w * kBitsPerWord) * size;
        bitmap = DynamicBitmapPopBitmap<kMode>(word_start, batch_pos, batch_end,
                                               size, objects_per_span, bitmap);
        bm[bm_idx] = bitmap;
      }

      if constexpr (kMode == BitmapMode::kOutlineL2_8) {
        // Each L2 bit covers a pair of L1 words; check the second word if
        // the first word is now empty.
        if (bitmap == 0) {
          bitmap = bm[bm_idx - 1];
          if (bitmap != 0 && batch_pos < batch_end) {
            char* const word_start =
                span_start + ((w + 1) * kBitsPerWord) * size;
            bitmap =
                DynamicBitmapPopBitmap<kMode>(word_start, batch_pos, batch_end,
                                              size, objects_per_span, bitmap);
            bm[bm_idx - 1] = bitmap;
          }
        }
      }

      if (bitmap == 0) {
        l2_word &= l2_word - 1;
      }
    } while ((kMode == BitmapMode::kOutlineL2_1 || l2_word != 0) &&
             batch_pos < batch_end);

    bm2[l2_idx] = l2_word;
    if (kMode == BitmapMode::kOutlineL2_1 || batch_pos == batch_end) {
      break;
    }
  }
}

template <Span::BitmapMode kMode>
ABSL_ATTRIBUTE_ALWAYS_INLINE inline Span::BitmapElem
Span::DynamicBitmapPopBitmap(char* word_start, void**& batch_pos,
                             void** batch_end, size_t size,
                             size_t objects_per_span, BitmapElem bitmap) {
  TC_ASSERT_NE(bitmap, 0);
  do {
    const int bit = absl::countr_zero(bitmap);
    bitmap &= bitmap - 1;
    char* const ptr = word_start + bit * size;
    TC_ASSERT_GE(ptr, start_address());
    TC_ASSERT_LT(ptr, reinterpret_cast<char*>(start_address()) +
                          size * objects_per_span);
    *batch_pos++ = ptr;
    // In kInline64 mode, the entire span fits in a single bitmap word and
    // batch_end is capped at available objects, so batch_pos == batch_end
    // always fires before or when bitmap becomes 0.
  } while ((kMode == BitmapMode::kInline64 || bitmap != 0) &&
           batch_pos < batch_end);
  return bitmap;
}

}  // namespace tcmalloc_internal
}  // namespace tcmalloc
GOOGLE_MALLOC_SECTION_END

#endif  // TCMALLOC_SPAN_H_
