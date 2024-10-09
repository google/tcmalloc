// Copyright 2020 The TCMalloc Authors
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

#include "tcmalloc/mock_central_freelist.h"

#include "absl/base/internal/spinlock.h"
#include "absl/types/span.h"
#include "tcmalloc/internal/logging.h"

namespace tcmalloc {
namespace tcmalloc_internal {

void RealCentralFreeListForTesting::AllocateBatch(absl::Span<void*> batch) {
  int total = 0;

  while (total < batch.size()) {
    const int to_remove = batch.size() - total;
    const int removed = RemoveRange(batch.subspan(total));
    ASSERT_GT(removed, 0);
    ASSERT_LE(removed, to_remove);
    total += removed;
  }
}

void RealCentralFreeListForTesting::FreeBatch(absl::Span<void*> batch) {
  InsertRange(batch);
}

void MinimalFakeCentralFreeList::AllocateBatch(absl::Span<void*> batch) {
  for (void*& v : batch) v = &v;
}

void MinimalFakeCentralFreeList::FreeBatch(absl::Span<void*> batch) {
  for (void* x : batch) TC_CHECK_NE(x, nullptr);
}

void MinimalFakeCentralFreeList::InsertRange(absl::Span<void*> batch) {
  absl::base_internal::SpinLockHolder h(&lock_);
  FreeBatch(batch);
}

int MinimalFakeCentralFreeList::RemoveRange(absl::Span<void*> batch) {
  absl::base_internal::SpinLockHolder h(&lock_);
  AllocateBatch(batch);
  return batch.size();
}

void FakeCentralFreeList::AllocateBatch(absl::Span<void*> batch) {
  for (int i = 0; i < batch.size(); ++i) {
    batch[i] = ::operator new(4);
  }
}

void FakeCentralFreeList::FreeBatch(absl::Span<void*> batch) {
  for (void* x : batch) {
    ::operator delete(x);
  }
}

void FakeCentralFreeList::InsertRange(absl::Span<void*> batch) {
  FreeBatch(batch);
}

int FakeCentralFreeList::RemoveRange(absl::Span<void*> batch) {
  AllocateBatch(batch);
  return batch.size();
}

}  // namespace tcmalloc_internal
}  // namespace tcmalloc
