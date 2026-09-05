// Copyright 2026 The TCMalloc Authors
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

#include "tcmalloc/static_forwarder.h"

#include <cstddef>
#include <cstdint>
#include <new>
#include <optional>

#include "absl/base/attributes.h"
#include "absl/base/optimization.h"
#include "absl/strings/string_view.h"
#include "absl/types/span.h"
#include "tcmalloc/arena.h"
#include "tcmalloc/common.h"
#include "tcmalloc/error_reporting.h"
#include "tcmalloc/internal/central_freelist_hooks.h"
#include "tcmalloc/internal/config.h"
#include "tcmalloc/internal/logging.h"
#include "tcmalloc/internal/memory_tag.h"
#include "tcmalloc/internal/numa.h"
#include "tcmalloc/internal/prefetch.h"
#include "tcmalloc/internal/system_allocator.h"
#include "tcmalloc/page_allocator_interface.h"
#include "tcmalloc/pagemap.h"
#include "tcmalloc/pages.h"
#include "tcmalloc/sampler.h"
#include "tcmalloc/span.h"
#include "tcmalloc/static_vars.h"
#include "tcmalloc/transfer_cache.h"

GOOGLE_MALLOC_SECTION_BEGIN
namespace tcmalloc {
namespace tcmalloc_internal {

void* Forwarder::Alloc(size_t size, std::align_val_t alignment) {
  return tc_globals.arena().Alloc(size, alignment);
}

void* Forwarder::AllocReportedImpending(size_t size,
                                        std::align_val_t alignment) {
  TC_ASSERT(tc_globals.IsInited());
  // TODO(b/373944374): Arena is thread-safe, but we take the pageheap_lock to
  // present a consistent view of memory usage.
  PageHeapSpinLockHolder l;
  // Negate previous update to allocated that accounted for this allocation.
  tc_globals.arena().UpdateAllocatedAndNonresident(-static_cast<int64_t>(size),
                                                   0);
  return tc_globals.arena().Alloc(size, alignment);
}

void Forwarder::SetAnonVmaName(void* ptr, size_t size,
                               std::optional<absl::string_view> name) const {
  TC_ASSERT_EQ(reinterpret_cast<uintptr_t>(ptr) % kHugePageSize, 0);
  TC_ASSERT_EQ(size % kHugePageSize, 0);
  tc_globals.system_allocator().SetAnonVmaName(ptr, size, name);
}

void Forwarder::ArenaUpdateAllocatedAndNonresident(int64_t allocated,
                                                   int64_t nonresident) {
  TC_ASSERT(tc_globals.IsInited());
  // TODO(b/373944374): Arena is thread-safe, but we take the pageheap_lock to
  // present a consistent view of memory usage.
  PageHeapSpinLockHolder l;
  if (allocated > 0) {
    tc_globals.page_allocator().ShrinkToUsageLimit(
        BytesToLengthCeil(allocated),
        /*may_have_grown=*/allocated > nonresident);
  }
  tc_globals.arena().UpdateAllocatedAndNonresident(allocated, nonresident);
}

bool Forwarder::reuse_size_classes() const {
  return tc_globals.size_class_configuration() ==
         SizeClassConfiguration::kReuseRelaxedBelow64;
}

size_t Forwarder::class_to_size(int size_class) {
  return tc_globals.sizemap().class_to_size(size_class);
}

size_t Forwarder::num_objects_to_move(int size_class) {
  return tc_globals.sizemap().num_objects_to_move(size_class);
}

Length Forwarder::class_to_pages(int size_class) {
  return Length(tc_globals.sizemap().class_to_pages(size_class));
}

const NumaTopology<kNumaPartitions, kNumBaseClasses>& Forwarder::numa_topology()
    const {
  return tc_globals.numa_topology();
}

bool Forwarder::HaveHooks() const { return tc_globals.HaveHooks(); }

size_t Forwarder::active_partitions() const {
  return tc_globals.active_partitions();
}

bool Forwarder::multiple_non_numa_partitions() const {
  return tc_globals.multiple_non_numa_partitions();
}

#ifndef TCMALLOC_INTERNAL_SMALL_BUT_SLOW
void BackingTransferCache::InsertRange(absl::Span<void*> batch) const {
  tc_globals.transfer_cache().InsertRange(size_class_, batch);
}

int BackingTransferCache::RemoveRange(const absl::Span<void*> batch) const {
  return tc_globals.transfer_cache().RemoveRange(size_class_, batch);
}
#endif

namespace central_freelist_internal {

static MemoryTag MemoryTagFromSizeClass(size_t size_class) {
  if (IsColdSizeClass(size_class)) {
    return MemoryTag::kCold;
  }
  if (tc_globals.active_partitions() == 1) {
    return MemoryTag::kNormal;
  }
  return MultiNormalTag(size_class / kNumBaseClasses);
}

static AccessDensityPrediction AccessDensity(int objects_per_span) {
  return objects_per_span > kFewObjectsAllocMaxLimit
             ? AccessDensityPrediction::kDense
             : AccessDensityPrediction::kSparse;
}

[[noreturn]] ABSL_ATTRIBUTE_NOINLINE static void HandleDetectedUB(
    void* ptr, Span* span, int page_size_class, int expected_size_class) {
  if (span == nullptr) {
    ReportCorruptedFree(tc_globals, ptr);
  } else if (span == &tc_globals.invalid_span()) {
    ReportDoubleFree(tc_globals, ptr);
  }
  ReportMismatchedSizeClass(tc_globals, ptr, page_size_class,
                            expected_size_class);
}

#ifdef TCMALLOC_INTERNAL_LEGACY_LOCKING
static void ReturnSpansToPageHeap(MemoryTag tag, absl::Span<Span*> free_spans,
                                  size_t objects_per_span)
    ABSL_LOCKS_EXCLUDED(pageheap_lock) {
  PageHeapSpinLockHolder l;
  for (Span* const free_span : free_spans) {
    TC_ASSERT_EQ(tag, GetMemoryTag(free_span->start_address()));
    tc_globals.page_allocator().Delete(free_span, tag,
                                       {.objects_per_span = objects_per_span});
  }
}
#else
static void ReturnAllocsToPageHeap(
    MemoryTag tag,
    absl::Span<PageAllocatorInterface::AllocationState> free_allocs,
    SpanAllocInfo span_alloc_info) ABSL_LOCKS_EXCLUDED(pageheap_lock) {
  PageHeapSpinLockHolder l;
  for (const auto& alloc : free_allocs) {
    tc_globals.page_allocator().Delete(alloc, tag, span_alloc_info);
  }
}
#endif

}  // namespace central_freelist_internal

void Forwarder::MapObjectsToSpans(absl::Span<void*> batch,
                                  Span** absl_nonnull spans,
                                  int expected_size_class) {
  for (int i = 0; i < batch.size(); ++i) {
    void* ptr = batch[i];
    const PageId p = PageIdContaining(ptr);
    auto [span, page_size_class] =
        tc_globals.pagemap().GetDescriptorAndSizeClass(p);
    if (ABSL_PREDICT_FALSE(page_size_class != expected_size_class)) {
      central_freelist_internal::HandleDetectedUB(ptr, span, page_size_class,
                                                  expected_size_class);
    }
    span->Prefetch();
    spans[i] = span;
  }
}

Span* Forwarder::AllocateSpan(int size_class, size_t objects_per_span,
                              Length pages_per_span) {
  const MemoryTag tag =
      central_freelist_internal::MemoryTagFromSizeClass(size_class);
  const AccessDensityPrediction density =
      central_freelist_internal::AccessDensity(objects_per_span);

  SpanAllocInfo span_alloc_info = {.objects_per_span = objects_per_span,
                                   .density = density};
  TC_ASSERT(density == AccessDensityPrediction::kSparse ||
            (density == AccessDensityPrediction::kDense &&
             pages_per_span == Length(1)));
  Span* span =
      tc_globals.page_allocator().New(pages_per_span, span_alloc_info, tag);
  if (ABSL_PREDICT_FALSE(span == nullptr)) {
    return nullptr;
  }
  TC_ASSERT_EQ(tag, GetMemoryTag(span->start_address()));
  TC_ASSERT_EQ(span->num_pages(), pages_per_span);

  tc_globals.pagemap().RegisterSizeClass(span, size_class);
  return span;
}

void Forwarder::DeallocateSpans(size_t objects_per_span,
                                absl::Span<Span*> free_spans) {
  TC_ASSERT_NE(free_spans.size(), 0);
  TC_ASSERT_LE(free_spans.size(), kMaxObjectsToMove);
  const MemoryTag tag = GetMemoryTag(free_spans[0]->start_address());
  for (Span* const free_span : free_spans) {
    TC_ASSERT_EQ(GetMemoryTag(free_span->start_address()), tag);
    TC_ASSERT(!IsSampledMemory(free_span->start_address()));
    tc_globals.pagemap().UnregisterSizeClass(free_span);

    const PageId p = free_span->first_page();
    void* pt = tc_globals.pagemap().GetHugepage(p);
    PrefetchW(pt);
    PrefetchW(reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(pt) +
                                      ABSL_CACHELINE_SIZE));
  }

#ifdef TCMALLOC_INTERNAL_LEGACY_LOCKING
  central_freelist_internal::ReturnSpansToPageHeap(tag, free_spans,
                                                   objects_per_span);
#else
  PageAllocatorInterface::AllocationState allocs[kMaxObjectsToMove];
  for (int i = 0, n = free_spans.size(); i < n; ++i) {
    Span* s = free_spans[i];
    TC_ASSERT_EQ(tag, GetMemoryTag(s->start_address()));
    allocs[i].r = Range(s->first_page(), s->num_pages());
    allocs[i].donated = s->donated();
    Span::Delete(s);
  }
  const AccessDensityPrediction density =
      central_freelist_internal::AccessDensity(objects_per_span);
  SpanAllocInfo span_alloc_info = {.objects_per_span = objects_per_span,
                                   .density = density};
  central_freelist_internal::ReturnAllocsToPageHeap(
      tag, absl::MakeSpan(allocs, free_spans.size()), span_alloc_info);
#endif
}

Arena& Forwarder::arena() { return tc_globals.arena(); }

void Forwarder::ShrinkToUsageLimit(Length n, bool may_have_grown) {
  tc_globals.page_allocator().ShrinkToUsageLimit(n, may_have_grown);
}

void* Forwarder::GetHugepage(HugePage p) {
  return tc_globals.pagemap().GetHugepage(p.first_page());
}

bool Forwarder::Ensure(Range r) { return tc_globals.pagemap().Ensure(r); }

void Forwarder::ClearSpan(PageId page) {
  tc_globals.pagemap().Set(page, const_cast<Span*>(&tc_globals.invalid_span()));
}

void Forwarder::SetSpan(PageId page, Span* absl_nonnull span) {
  tc_globals.pagemap().Set(page, span);
}

void Forwarder::SetHugepage(HugePage p, void* pt) {
  tc_globals.pagemap().SetHugepage(p.first_page(), pt);
}

Span* Forwarder::NewSpan(Range r) { return Span::New(r); }

void Forwarder::DeleteSpan(Span* span) { Span::Delete(span); }

void Forwarder::ReportDoubleFree(void* ptr) {
  tcmalloc::tcmalloc_internal::ReportDoubleFree(tc_globals, ptr);
}

AddressRange Forwarder::AllocatePages(size_t bytes, size_t align,
                                      MemoryTag tag) {
  return tc_globals.system_allocator().Allocate(bytes, align, tag);
}

void Forwarder::Back(Range r) {
  tc_globals.system_allocator().Back(r.start_addr(), r.in_bytes());
}

MemoryModifyStatus Forwarder::ReleasePages(Range r) {
  return tc_globals.system_allocator().Release(r.start_addr(), r.in_bytes());
}

MemoryModifyStatus Forwarder::CollapsePages(Range r) {
  return tc_globals.system_allocator().Collapse(r.start_addr(), r.in_bytes());
}

void Forwarder::SetAnonVmaName(Range r, std::optional<absl::string_view> name) {
  tc_globals.system_allocator().SetAnonVmaName(r.start_addr(), r.in_bytes(),
                                               name);
}

void Forwarder::InvokeInsertRangeHookSlow(size_t size_class,
                                          absl::Span<void*> batch) {
  central_freelist_insert_range_hooks.Invoke(size_class, batch);
}

void Forwarder::InvokeRemoveRangeHookSlow(size_t size_class,
                                          absl::Span<void*> batch) {
  central_freelist_remove_range_hooks.Invoke(size_class, batch);
}

}  // namespace tcmalloc_internal
}  // namespace tcmalloc
GOOGLE_MALLOC_SECTION_END
