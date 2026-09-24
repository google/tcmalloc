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
#include "tcmalloc/malloc_tracing_extension.h"
#include "tcmalloc/pages.h"
#include "tcmalloc/span.h"

GOOGLE_MALLOC_SECTION_BEGIN
namespace tcmalloc {
namespace tcmalloc_internal {

typedef void* (*PagemapAllocator)(size_t);
void* MetaDataAlloc(size_t bytes);

// Convenience wrapper around a uintptr that packs a Span pointer and its
// size class into a single word.
class PackedSpanAndSizeclass {
 public:
  void set(Span* absl_nullable span, CompactSizeClass sizeclass) {
    uintptr_t expected =
        (static_cast<uintptr_t>(sizeclass) << kSizeclassShift) |
        reinterpret_cast<uintptr_t>(span);
    packed_value_.store(expected, std::memory_order_relaxed);
  }

  Span* absl_nullable span() const {
    return reinterpret_cast<Span*>(
        packed_value_.load(std::memory_order_relaxed) & kSpanMask);
  }
  CompactSizeClass sizeclass() const {
    return static_cast<CompactSizeClass>(
        packed_value_.load(std::memory_order_relaxed) >> kSizeclassShift);
  }

 private:
  std::atomic<uintptr_t> packed_value_;
  static_assert(sizeof(CompactSizeClass) <= 2);
  static constexpr uintptr_t kSizeclassShift = 48;
  static constexpr uintptr_t kSpanMask = (uintptr_t{1} << kSizeclassShift) - 1;
};

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
    // Span pointers, with the top two most significant bytes used to also
    // store a redundant copy of the sizeclass. This allows us to avoid two
    // separate memory loads when fetching both the span and the sizeclass.
    PackedSpanAndSizeclass span_and_sizeclass[kLeafLength];
    void* hugepage[kLeafHugepages];

    Span* absl_nullable span(int i) const {
      return span_and_sizeclass[i].span();
    }
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

  // Return the descriptor for the specified page.  Returns NULL if
  // this PageId was not allocated previously.
  // No locks required.  See SYNCHRONIZATION explanation at top of tcmalloc.cc.
  [[nodiscard]] Span* absl_nullable GetDescriptor(PageId p) const
      ABSL_NO_THREAD_SAFETY_ANALYSIS {
    auto [leaf, i3] = MaybeIndex(p);
    if (ABSL_PREDICT_FALSE(leaf == nullptr)) {
      return nullptr;
    }
    return leaf->span(i3);
  }

  // Return the descriptor for the specified page.
  // PageId must have been previously allocated.
  // No locks required.  See SYNCHRONIZATION explanation at top of tcmalloc.cc.
  [[nodiscard]] Span* absl_nullable GetExistingDescriptor(PageId p) const
      ABSL_NO_THREAD_SAFETY_ANALYSIS {
    auto [leaf, i3] = MustIndex(p);
    return leaf->span(i3);
  }

  // Return the descriptor and sizeclass for the specified page.
  // No locks required.  See SYNCHRONIZATION explanation at top of tcmalloc.cc.
  //
  // ABSL_ATTRIBUTE_NO_SANITIZE_UNDEFINED is to disable array-bounds sanitizer.
  // This function is hot, and we can manually prove the array accesses.
  //
  // TODO(b/406313446): Remove ABSL_ATTRIBUTE_NO_SANITIZE_UNDEFINED once clang
  // optimizes out the array bounds check.
  [[nodiscard]] std::pair<Span* absl_nullable, CompactSizeClass>
  GetDescriptorAndSizeClass(PageId p) const ABSL_NO_THREAD_SAFETY_ANALYSIS
#ifdef __clang__
      ABSL_ATTRIBUTE_NO_SANITIZE_UNDEFINED
#endif  // __clang__
  {
    auto [leaf, i3] = MaybeIndex(p);
    if (ABSL_PREDICT_FALSE(leaf == nullptr)) {
      return std::make_pair(nullptr, 0);
    }
    PackedSpanAndSizeclass span_and_sizeclass = leaf->span_and_sizeclass[i3];
    return std::make_pair(span_and_sizeclass.span(),
                          span_and_sizeclass.sizeclass());
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
    return leaf->span_and_sizeclass[i3].sizeclass();
  }

  void Set(PageId p, Span* span) {
    auto [leaf, i3] = MustIndex(p);
    // This function should be used just after allocating a new Span;
    // in that case, the sizeclass should have been left at zero when the
    // old span was deallocated/unregistered (or it would have been zero
    // at initialization time.)
    TC_ASSERT_EQ(leaf->span_and_sizeclass[i3].sizeclass(), 0);
    leaf->span_and_sizeclass[i3].set(span, 0);
  }

  void Set(PageId p, Span* span, CompactSizeClass sc) {
    auto [leaf, i3] = MustIndex(p);
    leaf->span_and_sizeclass[i3].set(span, sc);
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
          if (root_[i1]->leafs[i2]->span(i3) != nullptr)
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

  constexpr size_t RootSize() const { return sizeof(root_); }

  // Mark an allocated span as being used for small objects of the
  // specified size-class.
  // REQUIRES: span was returned by an earlier call to PageAllocator::New()
  //           and has not yet been deleted.
  // Concurrent calls to this method are safe unless they mark the same span.
  void RegisterSizeClass(Span* span, size_t sc) {
    const PageId first = span->first_page();
    const PageId last = span->last_page();
    TC_ASSERT_EQ(GetDescriptor(first), span);
    for (PageId p = first; p <= last; ++p) {
      Set(p, span, sc);
    }
  }

  // Mark an allocated span as being not used for any size-class.
  // REQUIRES: span was returned by an earlier call to PageAllocator::New()
  //           and has not yet been deleted.
  // Concurrent calls to this method are safe unless they mark the same span.
  void UnregisterSizeClass(Span* span) {
    const PageId first = span->first_page();
    const PageId last = span->last_page();
    TC_ASSERT_EQ(GetDescriptor(first), span);
    for (PageId p = first; p <= last; ++p) {
      auto [leaf, i3] = MustIndex(p);
      leaf->span_and_sizeclass[i3].set(leaf->span_and_sizeclass[i3].span(), 0);
    }
  }

  // Returns the count of the currently allocated Spans and also adds details
  // of such Spans in the provided allocated_spans vector. This routine avoids
  // allocation events since we hold the pageheap_lock, so no more elements will
  // be added to allocated_spans after it reaches its already reserved capacity.
  GOOGLE_MALLOC_SECTION int GetAllocatedSpans(
      std::vector<tcmalloc::malloc_tracing_extension::AllocatedAddressRanges::
                      SpanDetails>& allocated_spans);
};

using ProdPageMap = PageMap<kAddressBits - kPageShift, MetaDataAlloc>;

}  // namespace tcmalloc_internal
}  // namespace tcmalloc
GOOGLE_MALLOC_SECTION_END

#endif  // TCMALLOC_PAGEMAP_H_
