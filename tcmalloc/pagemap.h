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
// A data structure used by the caching malloc.  It maps from page# to
// a pointer that contains info about that page using a three-level radix
// tree.
//
// The BITS parameter should be the number of bits required to hold
// a page number.  E.g., with 48-bit virtual address space and 8K pages
// (i.e., page offset fits in lower 13 bits), BITS == 35 (48-13).
//
// A PageMap requires external synchronization, except for the get/sizeclass
// methods (see explanation at top of tcmalloc.cc).

#ifndef TCMALLOC_PAGEMAP_H_
#define TCMALLOC_PAGEMAP_H_

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <algorithm>
#include <optional>
#include <tuple>
#include <utility>
#include <vector>

#include "absl/base/attributes.h"
#include "absl/base/nullability.h"
#include "absl/base/optimization.h"
#include "absl/base/thread_annotations.h"
#include "tcmalloc/common.h"
#include "tcmalloc/internal/config.h"
#include "tcmalloc/internal/logging.h"
#include "tcmalloc/internal/sampled_allocation.h"
#include "tcmalloc/malloc_tracing_extension.h"
#include "tcmalloc/pages.h"
#include "tcmalloc/span.h"

GOOGLE_MALLOC_SECTION_BEGIN
namespace tcmalloc {
namespace tcmalloc_internal {

typedef void* (*PagemapAllocator)(size_t);
[[nodiscard]] void* MetaDataAlloc(size_t bytes);

// Per-page metadata kept in the PageMap.
class PageMeta {
 public:
  constexpr PageMeta()
      : sizeclass_(0),
        sampled_(0),
        donated_(0),
        freed_(0),
        unused_(0),
        span_or_size_(0) {}

  void set_small(Span* absl_nonnull span, CompactSizeClass sizeclass,
                 bool donated) {
    TC_ASSERT_NE(span, nullptr);
    TC_ASSERT_NE(sizeclass, 0);
    sizeclass_ = static_cast<uint64_t>(sizeclass);
    sampled_ = 0;
    donated_ = donated;
    freed_ = 0;
    unused_ = 0;
    span_or_size_ = reinterpret_cast<uint64_t>(span);
    TC_ASSERT_EQ(this->span(), span);
  }

  void set_large(Length size, bool donated) {
    TC_ASSERT_GT(size, Length(0));
    sizeclass_ = 0;
    sampled_ = 0;
    donated_ = donated;
    freed_ = 0;
    unused_ = 0;
    span_or_size_ = size.raw_num();
    TC_ASSERT_EQ(span_or_size_, size.raw_num());
  }

  void set_sampled(SampledAllocation* absl_nonnull sampled, bool donated) {
    TC_ASSERT_NE(sampled, nullptr);
    sizeclass_ = 0;
    sampled_ = 1;
    donated_ = donated;
    freed_ = 0;
    unused_ = 0;
    span_or_size_ = reinterpret_cast<uint64_t>(sampled);
  }

  void set_freed() {
    sizeclass_ = 0;
    sampled_ = 0;
    donated_ = 0;
    freed_ = 1;
    unused_ = 0;
    span_or_size_ = 0;
  }

  [[nodiscard]] bool valid() const { return span_or_size_ != 0; }
  [[nodiscard]] CompactSizeClass sizeclass() const { return sizeclass_; }
  [[nodiscard]] bool sampled() const { return sampled_; }
  [[nodiscard]] bool donated() const { return donated_; }
  [[nodiscard]] bool freed() const { return freed_; }

  [[nodiscard]] Span* absl_nonnull span() const {
    TC_ASSERT_NE(sizeclass_, 0);
    TC_ASSERT_NE(span_or_size_, 0);
    return reinterpret_cast<Span*>(span_or_size_);
  }

  [[nodiscard]] Length size() const {
    TC_ASSERT_EQ(sizeclass_, 0);
    TC_ASSERT_NE(span_or_size_, 0);
    if (sampled_) {
      return std::max<Length>(
          BytesToLengthCeil(sampled_allocation()->sampled_stack.allocated_size),
          Length(1));
    }
    return Length(span_or_size_);
  }

  [[nodiscard]] SampledAllocation* absl_nonnull sampled_allocation() const {
    TC_ASSERT_EQ(sizeclass_, 0);
    TC_ASSERT(sampled_);
    TC_ASSERT_NE(span_or_size_, 0);
    return reinterpret_cast<SampledAllocation*>(span_or_size_);
  }

 private:
  // Place sizeclass_ in the low bits and span_or_size_ in the high bits with
  // flags and explicit padding in between, so that sizeclass_ can be loaded
  // without shifting, span_or_size_ can be extracted with a single shift, and
  // the whole 64-bit word can be written at once without preserving unmentioned
  // padding bits.
  uint64_t sizeclass_ : sizeof(CompactSizeClass) * 8;
  uint64_t sampled_ : 1;
  uint64_t donated_ : 1;
  uint64_t freed_ : 1;
  uint64_t unused_ : 64 - kAddressBits - sizeof(CompactSizeClass) * 8 - 3;
  uint64_t span_or_size_ : kAddressBits;
};

static_assert(sizeof(PageMeta) == 8);

// Three-level radix tree
template <int BITS, PagemapAllocator Allocator>
class PageMap {
 private:
  // For x86 we currently have 48 usable bits, for POWER we have 46. With
  // 4KiB page sizes (12 bits) we end up with 36 bits for x86 and 34 bits
  // for POWER. So leaf covers 4KiB * 1 << 12 = 16MiB - which is huge page
  // size for POWER.
  static constexpr int kLeafBits = (BITS + 2) / 3;  // Round up
  static constexpr int kLeafLength = 1 << kLeafBits;
  static constexpr int kMidBits = (BITS + 2) / 3;  // Round up
  static constexpr int kMidLength = 1 << kMidBits;
  static constexpr int kRootBits = BITS - kLeafBits - kMidBits;
  static_assert(kRootBits > 0, "Too many bits assigned to leaf and mid");
  // (1<<kRootBits) must not overflow an "int"
  static_assert(kRootBits < sizeof(int) * 8 - 1, "Root bits too large");
  static constexpr int kRootLength = 1 << kRootBits;

  static constexpr size_t kLeafCoveredBytes = size_t{1}
                                              << (kLeafBits + kPageShift);
  static_assert(kLeafCoveredBytes >= kHugePageSize, "leaf too small");
  static constexpr size_t kLeafHugeBits =
      (kLeafBits + kPageShift - kHugePageShift);
  static constexpr size_t kLeafHugepages = kLeafCoveredBytes / kHugePageSize;
  static_assert(kLeafHugepages == 1 << kLeafHugeBits, "sanity");
  struct Leaf {
    // Per-page metadata (span pointer, large allocation size, or sampled
    // allocation pointer, along with flags and sizeclass). This allows us to
    // avoid two separate memory loads when fetching both the descriptor and the
    // sizeclass.
    PageMeta page[kLeafLength];
    void* hugepage[kLeafHugepages];
  };

  struct Node {
    // Mid-level structure that holds pointers to leafs
    Leaf* absl_nullable leafs[kMidLength];
  };

  typedef uintptr_t Number;

  Node* absl_nullable root_[kRootLength];  // Top-level node

  [[nodiscard]] ABSL_ATTRIBUTE_ALWAYS_INLINE std::tuple<Number, Number, Number>
  Index(PageId p) const {
    const Number k = p.index();
    const Number i1 = k >> (kLeafBits + kMidBits);
    const Number i2 = (k >> kLeafBits) & (kMidLength - 1);
    const Number i3 = k & (kLeafLength - 1);
    return {i1, i2, i3};
  }

  [[nodiscard]] ABSL_ATTRIBUTE_ALWAYS_INLINE
      std::pair<Leaf* absl_nonnull, Number>
      MustIndex(PageId p) const {
    TC_ASSERT_EQ(p.index() >> BITS, 0);
    auto [i1, i2, i3] = Index(p);
    TC_ASSERT_NE(root_[i1], nullptr);
    Leaf* leaf = root_[i1]->leafs[i2];
    TC_ASSERT_NE(leaf, nullptr);
    return {leaf, i3};
  }

  [[nodiscard]] ABSL_ATTRIBUTE_ALWAYS_INLINE
      std::pair<Leaf* absl_nullable, Number>
      MaybeIndex(PageId p) const {
    const Number k = p.index();
    if (ABSL_PREDICT_FALSE((k >> BITS) > 0)) {
      return {nullptr, 0};
    }
    auto [i1, i2, i3] = Index(p);
    const Node* node = root_[i1];
    if (ABSL_PREDICT_FALSE(node == nullptr)) {
      return {nullptr, 0};
    }
    return {node->leafs[i2], i3};
  }

 public:
  constexpr PageMap() : root_{} {}

  // Return the descriptor for the specified page.  Returns an empty PageMeta if
  // this PageId was not allocated previously.
  // No locks required.  See SYNCHRONIZATION explanation at top of tcmalloc.cc.
  //
  // ABSL_ATTRIBUTE_NO_SANITIZE_UNDEFINED is to disable array-bounds sanitizer.
  // This function is hot, and we can manually prove the array accesses.
  //
  // TODO(b/406313446): Remove ABSL_ATTRIBUTE_NO_SANITIZE_UNDEFINED once clang
  // optimizes out the array bounds check.
  [[nodiscard]] PageMeta GetDescriptor(PageId p) const
      ABSL_NO_THREAD_SAFETY_ANALYSIS
#ifdef __clang__
      ABSL_ATTRIBUTE_NO_SANITIZE_UNDEFINED
#endif  // __clang__
  {
    auto [leaf, i3] = MaybeIndex(p);
    if (ABSL_PREDICT_FALSE(leaf == nullptr)) {
      return PageMeta{};
    }
    return leaf->page[i3];
  }

  // Return the descriptor for the specified page.
  // PageId must have been previously allocated.
  // No locks required.  See SYNCHRONIZATION explanation at top of tcmalloc.cc.
  [[nodiscard]] PageMeta GetExistingDescriptor(PageId p) const
      ABSL_NO_THREAD_SAFETY_ANALYSIS {
    auto [leaf, i3] = MustIndex(p);
    return leaf->page[i3];
  }

  // Return the size class for p, or 0 if it is not known to tcmalloc
  // or is a page containing large objects.
  // No locks required.  See SYNCHRONIZATION explanation at top of tcmalloc.cc.
  //
  // TODO(b/193887621): Convert to atomics to permit the PageMap to run cleanly
  // under TSan.
  [[nodiscard]] CompactSizeClass sizeclass(PageId p) const
      ABSL_NO_THREAD_SAFETY_ANALYSIS {
    auto [leaf, i3] = MaybeIndex(p);
    if (ABSL_PREDICT_FALSE(leaf == nullptr)) {
      return 0;
    }
    return leaf->page[i3].sizeclass();
  }

  void SetSmall(PageId p, Span* absl_nonnull span, CompactSizeClass sc,
                bool donated) {
    auto [leaf, i3] = MustIndex(p);
    leaf->page[i3].set_small(span, sc, donated);
  }

  void SetLarge(PageId p, Length size, bool donated) {
    auto [leaf, i3] = MustIndex(p);
    TC_ASSERT_EQ(leaf->page[i3].sizeclass(), 0);
    leaf->page[i3].set_large(size, donated);
  }

  void SetSampled(PageId p, SampledAllocation* absl_nonnull sampled,
                  bool donated) {
    auto [leaf, i3] = MustIndex(p);
    TC_ASSERT_EQ(leaf->page[i3].sizeclass(), 0);
    leaf->page[i3].set_sampled(sampled, donated);
  }

  void SetFreed(PageId p) {
    auto [leaf, i3] = MustIndex(p);
    TC_ASSERT_EQ(leaf->page[i3].sizeclass(), 0);
    leaf->page[i3].set_freed();
  }

  [[nodiscard]] void* GetHugepage(PageId p) const {
    auto [leaf, i3] = MustIndex(p);
    return leaf->hugepage[i3 >> (kLeafBits - kLeafHugeBits)];
  }

  void SetHugepage(PageId p, void* v) {
    auto [leaf, i3] = MustIndex(p);
    leaf->hugepage[i3 >> (kLeafBits - kLeafHugeBits)] = v;
  }

  [[nodiscard]] bool HasLeaf(PageId p) const {
    auto [leaf, i3] = MaybeIndex(p);
    return leaf != nullptr;
  }

  // No locks required.  See SYNCHRONIZATION explanation at top of tcmalloc.cc.
  [[nodiscard]] std::optional<PageId> get_next_set_page(PageId p) const {
    auto [i1, i2, i3] = Index(p + Length(1));
    for (; i1 < kRootLength; ++i1, i2 = 0, i3 = 0) {
      if (root_[i1] == nullptr) continue;
      for (; i2 < kMidLength; ++i2, i3 = 0) {
        if (root_[i1]->leafs[i2] == nullptr) continue;
        for (; i3 < kLeafLength; ++i3) {
          if (root_[i1]->leafs[i2]->page[i3].valid())
            return PageId((i1 << (kLeafBits + kMidBits)) | (i2 << kLeafBits) |
                          i3);
        }
      }
    }
    return std::nullopt;
  }

  [[nodiscard]] bool Ensure(Range r) {
    if (r.n == Length(0)) return true;
    const PageId last = r.p + r.n - Length(1);
    for (PageId p = r.p; p <= last;) {
      const auto [i1, i2, i3] = Index(p);
      (void)i3;

      // Check within root
      if (i1 >= kRootLength) return false;

      // Allocate Node if necessary
      if (root_[i1] == nullptr) {
        Node* node = reinterpret_cast<Node*>(Allocator(sizeof(Node)));
        if (node == nullptr) return false;
        memset(node, 0, sizeof(*node));
        root_[i1] = node;
      }

      // Allocate Leaf if necessary
      if (root_[i1]->leafs[i2] == nullptr) {
        Leaf* leaf = reinterpret_cast<Leaf*>(Allocator(sizeof(Leaf)));
        if (leaf == nullptr) return false;
        memset(leaf, 0, sizeof(*leaf));
        root_[i1]->leafs[i2] = leaf;
      }

      // Advance p past whatever is covered by this leaf node
      Number key = ((p.index() >> kLeafBits) + 1) << kLeafBits;
      if (key == 0) {
        return false;
      }
      p = PageId(key);
    }
    return true;
  }

  [[nodiscard]] constexpr size_t RootSize() const { return sizeof(root_); }

  // Mark an allocated span as being used for small objects of the
  // specified size-class.
  // REQUIRES: span was returned by an earlier call to PageAllocator::New()
  //           and has not yet been deleted.
  // Concurrent calls to this method are safe unless they mark the same span.
  void RegisterSizeClass(Span* span, Length num_pages, size_t sc,
                         bool donated) {
    const PageId first = span->first_page();
    const PageId last = first + num_pages - Length(1);
    for (PageId p = first; p <= last; ++p) {
      SetSmall(p, span, sc, donated);
    }
  }

  // Mark an allocated span as being not used for any size-class.
  // Returns whether the span was donated.
  // REQUIRES: span was returned by an earlier call to PageAllocator::New()
  //           and has not yet been deleted.
  // Concurrent calls to this method are safe unless they mark the same span.
  [[nodiscard]] bool UnregisterSizeClass(Span* span, Length num_pages) {
    const PageId first = span->first_page();
    const PageId last = first + num_pages - Length(1);
    const PageMeta first_meta = GetExistingDescriptor(first);
    TC_ASSERT_EQ(first_meta.span(), span);
    const bool donated = first_meta.donated();
    for (PageId p = first; p <= last; ++p) {
      auto [leaf, i3] = MustIndex(p);
      leaf->page[i3].set_freed();
    }
    return donated;
  }

  // Returns the count of the currently allocated Spans and also adds details
  // of such Spans in the provided allocated_spans vector. This routine avoids
  // allocation events since we hold the pageheap_lock, so no more elements will
  // be added to allocated_spans after it reaches its already reserved capacity.
  [[nodiscard]] GOOGLE_MALLOC_SECTION int GetAllocatedSpans(
      std::vector<tcmalloc::malloc_tracing_extension::AllocatedAddressRanges::
                      SpanDetails>& allocated_spans);
};

using ProdPageMap = PageMap<kAddressBits - kPageShift, MetaDataAlloc>;

}  // namespace tcmalloc_internal
}  // namespace tcmalloc
GOOGLE_MALLOC_SECTION_END

#endif  // TCMALLOC_PAGEMAP_H_
