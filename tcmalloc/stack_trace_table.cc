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

#include "tcmalloc/stack_trace_table.h"

#include <stddef.h>
#include <string.h>

#include <limits>

#include "absl/base/internal/spinlock.h"
#include "absl/hash/hash.h"
#include "tcmalloc/common.h"
#include "tcmalloc/internal/logging.h"
#include "tcmalloc/internal/mincore.h"
#include "tcmalloc/page_heap_allocator.h"
#include "tcmalloc/sampler.h"
#include "tcmalloc/static_vars.h"

GOOGLE_MALLOC_SECTION_BEGIN
namespace tcmalloc {
namespace tcmalloc_internal {

namespace {
// TODO(patrickx): Use this function in caller instead whenever we get rid of
// StackTraceTable.
void MaybeUpdateResidencyInBucket(StackTraceTable::Bucket& bucket,
                                  Residency* residency, void* span_start,
                                  double count, size_t allocated_size) {
  if (residency == nullptr) return;

  auto residency_info = residency->Get(span_start, allocated_size);
  if (!residency_info.has_value()) {
    bucket.residency_errors_encountered = true;
    return;
  }
  bucket.total_resident += count * residency_info->bytes_resident;
  bucket.total_swapped += count * residency_info->bytes_swapped;
}
}  // namespace

bool StackTraceTable::Bucket::KeyEqual(uintptr_t h, const StackTrace& t) const {
  return this->hash == h && this->trace == t;
}

StackTraceTable::StackTraceTable(ProfileType type, int64_t period, bool merge,
                                 bool unsample)
    : type_(type),
      period_(period),
      bucket_mask_(merge ? (1 << 14) - 1 : 0),
      depth_total_(0),
      table_(new Bucket*[num_buckets()]()),
      bucket_total_(0),
      merge_(merge),
      error_(false),
      unsample_(unsample) {
  memset(table_, 0, num_buckets() * sizeof(Bucket*));
}

StackTraceTable::~StackTraceTable() {
  for (int i = 0; i < num_buckets(); ++i) {
    Bucket* b = table_[i];
    while (b != nullptr) {
      Bucket* next = b->next;
      {
        absl::base_internal::SpinLockHolder h(&pageheap_lock);
        Static::bucket_allocator().Delete(b);
      }
      b = next;
    }
  }
  delete[] table_;
}

void StackTraceTable::AddTrace(double count, const StackTrace& t) {
  return AddTrace(count, t, nullptr);
}

void StackTraceTable::AddTrace(double count, const StackTrace& t,
                               Residency* residency) {
  if (error_) {
    return;
  }

  uintptr_t h = absl::Hash<StackTrace>()(t);

  const int idx = h & bucket_mask_;

  Bucket* b = merge_ ? table_[idx] : nullptr;
  while (b != nullptr && !b->KeyEqual(h, t)) {
    b = b->next;
  }
  if (b != nullptr) {
    b->count += count;
    b->total_weight += count * t.weight;
    b->trace.weight = b->total_weight / b->count + 0.5;
    MaybeUpdateResidencyInBucket(*b, residency, t.span_start_address, count,
                                 t.allocated_size);
  } else {
    depth_total_ += t.depth;
    bucket_total_++;
    {
      // TODO(b/239458966): Use heap allocation for bucket after we remove the
      // need to use StackTraceTable while allocating (e.g. allocationz/).
      absl::base_internal::SpinLockHolder h(&pageheap_lock);
      b = Static::bucket_allocator().New();
    }
    b->hash = h;
    b->trace = t;
    b->count = count;
    b->total_weight = t.weight * count;
    b->next = table_[idx];
    b->total_resident = 0;
    b->total_swapped = 0;
    MaybeUpdateResidencyInBucket(*b, residency, t.span_start_address, count,
                                 t.allocated_size);
    table_[idx] = b;
  }
}

void StackTraceTable::Iterate(
    absl::FunctionRef<void(const Profile::Sample&)> func) const {
  if (error_) {
    return;
  }

  for (int i = 0; i < num_buckets(); ++i) {
    Bucket* b = table_[i];
    while (b != nullptr) {
      // Report total bytes that are a multiple of the object size.
      size_t allocated_size = b->trace.allocated_size;
      size_t requested_size = b->trace.requested_size;

      uintptr_t bytes = b->count * AllocatedBytes(b->trace, unsample_) + 0.5;

      Profile::Sample e;
      // We want sum to be a multiple of allocated_size; pick the nearest
      // multiple rather than always rounding up or down.
      //
      // TODO(b/215362992): Revisit this assertion when GWP-ASan guards
      // zero-byte allocations.
      ASSERT(allocated_size > 0);
      e.count = (bytes + allocated_size / 2) / allocated_size;
      e.sum = e.count * allocated_size;
      e.requested_size = requested_size;
      e.requested_alignment = b->trace.requested_alignment;
      e.allocated_size = allocated_size;
      e.access_hint = static_cast<hot_cold_t>(b->trace.access_hint);
      e.access_allocated = b->trace.cold_allocated
                               ? Profile::Sample::Access::Cold
                               : Profile::Sample::Access::Hot;

      e.depth = b->trace.depth;
      // TODO(b/235916219): This is all changing. Do the refactor as mentioned
      // in the bug and get rid of e.sampled_resident_size. Note "sampled" is
      // currently a misnomer.
      e.sampled_resident_size = b->total_resident;
      if (b->residency_errors_encountered) {
        e.sampled_resident_size = std::numeric_limits<size_t>::max();
      }
      static_assert(kMaxStackDepth <= Profile::Sample::kMaxStackDepth,
                    "Profile stack size smaller than internal stack sizes");
      memcpy(e.stack, b->trace.stack, sizeof(e.stack[0]) * e.depth);
      func(e);

      b = b->next;
    }
  }
}

}  // namespace tcmalloc_internal
}  // namespace tcmalloc
GOOGLE_MALLOC_SECTION_END
