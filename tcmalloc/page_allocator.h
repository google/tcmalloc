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

#ifndef TCMALLOC_PAGE_ALLOCATOR_H_
#define TCMALLOC_PAGE_ALLOCATOR_H_

#include <inttypes.h>
#include <stddef.h>

#include <utility>

#include "absl/base/thread_annotations.h"
#include "tcmalloc/common.h"
#include "tcmalloc/huge_page_aware_allocator.h"
#include "tcmalloc/internal/logging.h"
#include "tcmalloc/page_allocator_interface.h"
#include "tcmalloc/page_heap.h"
#include "tcmalloc/span.h"
#include "tcmalloc/stats.h"

namespace tcmalloc {

class PageAllocator {
 public:
  PageAllocator();
  ~PageAllocator() = delete;
  // Allocate a run of "n" pages.  Returns zero if out of memory.
  // Caller should not pass "n == 0" -- instead, n should have
  // been rounded up already.
  // Any address in the returned Span is guaranteed to satisfy
  // IsTaggedMemory(addr) == "tagged".
  Span* New(Length n, bool tagged) ABSL_LOCKS_EXCLUDED(pageheap_lock);

  // As New, but the returned span is aligned to a <align>-page boundary.
  // <align> must be a power of two.
  Span* NewAligned(Length n, Length align, bool tagged)
      ABSL_LOCKS_EXCLUDED(pageheap_lock);

  // Delete the span "[p, p+n-1]".
  // REQUIRES: span was returned by earlier call to New() with the same value of
  //           "tagged" and has not yet been deleted.
  void Delete(Span* span, bool tagged)
      ABSL_EXCLUSIVE_LOCKS_REQUIRED(pageheap_lock);

  BackingStats stats() const ABSL_EXCLUSIVE_LOCKS_REQUIRED(pageheap_lock);

  void GetSmallSpanStats(SmallSpanStats* result)
      ABSL_EXCLUSIVE_LOCKS_REQUIRED(pageheap_lock);

  void GetLargeSpanStats(LargeSpanStats* result)
      ABSL_EXCLUSIVE_LOCKS_REQUIRED(pageheap_lock);

  // Try to release at least num_pages for reuse by the OS.  Returns
  // the actual number of pages released, which may be less than
  // num_pages if there weren't enough pages to release. The result
  // may also be larger than num_pages since page_heap might decide to
  // release one large range instead of fragmenting it into two
  // smaller released and unreleased ranges.
  Length ReleaseAtLeastNPages(Length num_pages)
      ABSL_EXCLUSIVE_LOCKS_REQUIRED(pageheap_lock);

  // Prints stats about the page heap to *out.
  void Print(TCMalloc_Printer* out, bool tagged)
      ABSL_LOCKS_EXCLUDED(pageheap_lock);
  void PrintInPbtxt(PbtxtRegion* region, bool tagged)
      ABSL_LOCKS_EXCLUDED(pageheap_lock);

  void set_limit(size_t limit, bool is_hard) ABSL_LOCKS_EXCLUDED(pageheap_lock);
  std::pair<size_t, bool> limit() const ABSL_LOCKS_EXCLUDED(pageheap_lock);
  int64_t limit_hits() const ABSL_LOCKS_EXCLUDED(pageheap_lock);

  // If we have a usage limit set, ensure we're not violating it from our latest
  // allocation.
  void ShrinkToUsageLimit() ABSL_EXCLUSIVE_LOCKS_REQUIRED(pageheap_lock);

  const PageAllocInfo& info(bool tagged) const
      ABSL_EXCLUSIVE_LOCKS_REQUIRED(pageheap_lock);

  enum Algorithm {
    PAGE_HEAP = 0,
    HPAA = 1,
  };

  Algorithm algorithm() const { return alg_; }

 private:
  bool ShrinkHardBy(Length pages) ABSL_EXCLUSIVE_LOCKS_REQUIRED(pageheap_lock);

  PageAllocatorInterface* impl(bool tagged) const;

  union Choices {
    Choices() : dummy(0) {}
    ~Choices() {}
    int dummy;
    PageHeap ph;
    HugePageAwareAllocator hpaa;
  } choices_[2];
  PageAllocatorInterface* untagged_impl_;
  PageAllocatorInterface* tagged_impl_;
  Algorithm alg_;

  bool limit_is_hard_{false};
  // Max size of backed spans we will attempt to maintain.
  size_t limit_{std::numeric_limits<size_t>::max()};
  // The number of times the limit has been hit.
  int64_t limit_hits_{0};
};

inline PageAllocatorInterface* PageAllocator::impl(bool tagged) const {
  return tagged ? tagged_impl_ : untagged_impl_;
}

inline Span* PageAllocator::New(Length n, bool tagged) {
  return impl(tagged)->New(n);
}

inline Span* PageAllocator::NewAligned(Length n, Length align, bool tagged) {
  return impl(tagged)->NewAligned(n, align);
}

inline void PageAllocator::Delete(Span* span, bool tagged) {
  impl(tagged)->Delete(span);
}

inline BackingStats PageAllocator::stats() const {
  return untagged_impl_->stats() + tagged_impl_->stats();
}

inline void PageAllocator::GetSmallSpanStats(SmallSpanStats* result) {
  SmallSpanStats untagged, tagged;
  untagged_impl_->GetSmallSpanStats(&untagged);
  tagged_impl_->GetSmallSpanStats(&tagged);
  *result = untagged + tagged;
}

inline void PageAllocator::GetLargeSpanStats(LargeSpanStats* result) {
  LargeSpanStats untagged, tagged;
  untagged_impl_->GetLargeSpanStats(&untagged);
  tagged_impl_->GetLargeSpanStats(&tagged);
  *result = untagged + tagged;
}

inline Length PageAllocator::ReleaseAtLeastNPages(Length num_pages) {
  Length released = untagged_impl_->ReleaseAtLeastNPages(num_pages);
  if (released < num_pages) {
    released += tagged_impl_->ReleaseAtLeastNPages(num_pages - released);
  }
  return released;
}

inline void PageAllocator::Print(TCMalloc_Printer* out, bool tagged) {
  if (tagged) {
    out->printf("\n>>>>>>> Begin tagged page allocator <<<<<<<\n");
  }
  impl(tagged)->Print(out);
  if (tagged) {
    out->printf(">>>>>>> End tagged page allocator <<<<<<<\n");
  }
}

inline void PageAllocator::PrintInPbtxt(PbtxtRegion* region, bool tagged) {
  PbtxtRegion pa = region->CreateSubRegion("page_allocator");
  pa.PrintBool("tagged", tagged);
  impl(tagged)->PrintInPbtxt(&pa);
}

inline void PageAllocator::set_limit(size_t limit, bool is_hard) {
  absl::base_internal::SpinLockHolder h(&pageheap_lock);
  limit_ = limit;
  limit_is_hard_ = is_hard;
}

inline std::pair<size_t, bool> PageAllocator::limit() const {
  absl::base_internal::SpinLockHolder h(&pageheap_lock);
  return {limit_, limit_is_hard_};
}

inline int64_t PageAllocator::limit_hits() const {
  absl::base_internal::SpinLockHolder h(&pageheap_lock);
  return limit_hits_;
}

inline const PageAllocInfo& PageAllocator::info(bool tagged) const {
  return impl(tagged)->info();
}

}  // namespace tcmalloc

#endif  // TCMALLOC_PAGE_ALLOCATOR_H_
