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
// metadata about that page using a three-level radix tree.
//
// The BITS parameter should be the number of bits required to hold
// a page number.  E.g., with 48-bit virtual address space and 8K pages
// (i.e., page offset fits in lower 13 bits), BITS == 35 (48-13).
//
// A PageMap requires external synchronization, except for the sizeclass/meta
// methods (see explanation at top of tcmalloc.cc).

#ifndef TCMALLOC_PAGEMAP_H_
#define TCMALLOC_PAGEMAP_H_

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <atomic>
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

struct SampledAllocation;

typedef void* (*PagemapAllocator)(size_t);

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
  static_assert(std::atomic<SpanMeta>::is_always_lock_free);
  struct Leaf {
    std::atomic<SpanMeta> meta[kLeafLength];
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

  // Return the size class for p, or 0 if it is not known to tcmalloc
  // or is a page containing large objects.
  // No locks required.  See SYNCHRONIZATION explanation at top of tcmalloc.cc.
  [[nodiscard]] CompactSizeClass sizeclass(PageId p) const
      ABSL_NO_THREAD_SAFETY_ANALYSIS {
    auto [leaf, i3] = MaybeIndex(p);
    if (ABSL_PREDICT_FALSE(leaf == nullptr)) {
      return 0;
    }
    SpanMeta meta = leaf->meta[i3].load(std::memory_order_relaxed);
    return meta.is_small ? meta.size_class : 0;
  }

  [[nodiscard]] std::atomic<SpanMeta>* GetSpanMeta(PageId p) const
      ABSL_NO_THREAD_SAFETY_ANALYSIS {
    auto [leaf, i3] = MustIndex(p);
    return &leaf->meta[i3];
  }

  [[nodiscard]] std::optional<SpanMeta> GetSpanMetaNullable(PageId p) const
      ABSL_NO_THREAD_SAFETY_ANALYSIS {
    auto [leaf, i3] = MaybeIndex(p);
    if (ABSL_PREDICT_FALSE(leaf == nullptr)) return std::nullopt;
    SpanMeta meta = leaf->meta[i3].load(std::memory_order_relaxed);
    if (ABSL_PREDICT_FALSE(!meta.IsValid())) return std::nullopt;
    return meta;
  }

  [[nodiscard]] bool IsFreed(PageId p) const ABSL_NO_THREAD_SAFETY_ANALYSIS {
    auto [leaf, i3] = MaybeIndex(p);
    if (ABSL_PREDICT_FALSE(leaf == nullptr)) return false;
    return leaf->meta[i3].load(std::memory_order_relaxed).IsFreed();
  }

  void RegisterSmallSpan(PageId p, Length n, CompactSizeClass sc,
                         bool donated) {
    TC_ASSERT_GT(n, Length(0));
    SpanMeta meta{};
    meta.is_small = 1;
    meta.size_class = sc;
    meta.is_donated = donated;
    meta.is_in_cfl = 0;
    for (Length offset = Length(0); offset < n; ++offset) {
      meta.heap_index_or_page_offset = offset.raw_num();
      auto [leaf, i3] = MustIndex(p + offset);
      leaf->meta[i3].store(meta, std::memory_order_relaxed);
    }
  }

  void UnregisterSmallSpan(PageId p, Length n) {
    const SpanMeta freed = SpanMeta::Freed();
    for (Length offset = Length(0); offset < n; ++offset) {
      auto [leaf, i3] = MustIndex(p + offset);
      leaf->meta[i3].store(freed, std::memory_order_relaxed);
    }
  }

  void RegisterLargeSpan(PageId p, Length n, bool donated, bool is_sampled) {
    auto [leaf, i3] = MustIndex(p);
    SpanMeta meta{};
    meta.is_small1 = 0;
    meta.is_donated1 = donated;
    meta.is_sampled = is_sampled;
    if (ABSL_PREDICT_TRUE(n < Length(SpanMeta::kMaxNumPages))) {
      meta.num_pages_or_sampled_index = n.raw_num();
    } else {
      meta.num_pages_or_sampled_index = 0;
      SetHugepage(p,
                  reinterpret_cast<void*>(static_cast<uintptr_t>(n.raw_num())));
    }
    leaf->meta[i3].store(meta, std::memory_order_relaxed);
  }

  void UnregisterLargeSpan(PageId p, Length n) {
    auto [leaf, i3] = MustIndex(p);
    if (ABSL_PREDICT_FALSE(n >= Length(SpanMeta::kMaxNumPages))) {
      SetHugepage(p, nullptr);
    }
    leaf->meta[i3].store(SpanMeta::Freed(), std::memory_order_relaxed);
  }

  GOOGLE_MALLOC_SECTION void RegisterSampledSpan(PageId p, SampledAllocation* s)
      ABSL_NO_THREAD_SAFETY_ANALYSIS;

  GOOGLE_MALLOC_SECTION SampledAllocation* UnregisterSampledSpan(PageId p)
      ABSL_NO_THREAD_SAFETY_ANALYSIS;

  GOOGLE_MALLOC_SECTION [[nodiscard]] size_t GetLargeSize(
      PageId p, SpanMeta meta) const ABSL_NO_THREAD_SAFETY_ANALYSIS;

  GOOGLE_MALLOC_SECTION [[nodiscard]] size_t GetLargeSize(PageId p) const
      ABSL_NO_THREAD_SAFETY_ANALYSIS;

  [[nodiscard]] void* GetHugepage(PageId p) const {
    auto [leaf, i3] = MustIndex(p);
    return leaf->hugepage[i3 >> (kLeafBits - kLeafHugeBits)];
  }

  void SetHugepage(PageId p, void* v) {
    auto [leaf, i3] = MustIndex(p);
    const Number i4 = i3 >> (kLeafBits - kLeafHugeBits);
    void*& slot = leaf->hugepage[i4];
    TC_ASSERT(!v || !slot, "slot=%p", slot);
    slot = v;
  }

  [[nodiscard]] bool HasLeaf(PageId p) const {
    auto [leaf, i3] = MaybeIndex(p);
    return leaf != nullptr;
  }

  // No locks required.  See SYNCHRONIZATION explanation at top of tcmalloc.cc.
  std::optional<PageId> get_next_set_page(PageId p) const {
    auto [i1, i2, i3] = Index(p + Length(1));
    for (; i1 < kRootLength; ++i1, i2 = 0, i3 = 0) {
      if (root_[i1] == nullptr) continue;
      for (; i2 < kMidLength; ++i2, i3 = 0) {
        if (root_[i1]->leafs[i2] == nullptr) continue;
        return PageId((i1 << (kLeafBits + kMidBits)) | (i2 << kLeafBits) | i3);
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

  // Returns the count of the currently allocated Spans and also adds details
  // of such Spans in the provided allocated_spans vector. This routine avoids
  // allocation events since we hold the pageheap_lock, so no more elements will
  // be added to allocated_spans after it reaches its already reserved capacity.
  GOOGLE_MALLOC_SECTION int GetAllocatedSpans(
      std::vector<tcmalloc::malloc_tracing_extension::AllocatedAddressRanges::
                      SpanDetails>& allocated_spans);
};

void* PageMapMetaDataAlloc(size_t bytes);
using ProdPageMap = PageMap<kAddressBits - kPageShift, PageMapMetaDataAlloc>;

}  // namespace tcmalloc_internal
}  // namespace tcmalloc
GOOGLE_MALLOC_SECTION_END

#endif  // TCMALLOC_PAGEMAP_H_
