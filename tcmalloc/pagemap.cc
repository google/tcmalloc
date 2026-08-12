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

#include "tcmalloc/pagemap.h"

#include <sys/mman.h>

#include <cstddef>
#include <cstdint>

#include "absl/base/thread_annotations.h"
#include "tcmalloc/common.h"
#include "tcmalloc/internal/config.h"
#include "tcmalloc/internal/logging.h"
#include "tcmalloc/internal/util.h"
#include "tcmalloc/pages.h"
#include "tcmalloc/span.h"
#include "tcmalloc/static_vars.h"

GOOGLE_MALLOC_SECTION_BEGIN
namespace tcmalloc {
namespace tcmalloc_internal {

void PageMap::RegisterSizeClass(Span* span, size_t sc) {
  const PageId first = span->first_page();
  const PageId last = span->last_page();
  TC_ASSERT_EQ(GetDescriptor(first), span);
  for (PageId p = first; p <= last; ++p) {
    map_.set_with_sizeclass(p.index(), span, sc);
  }
}

void PageMap::UnregisterSizeClass(Span* span) {
  const PageId first = span->first_page();
  const PageId last = span->last_page();
  TC_ASSERT_EQ(GetDescriptor(first), span);
  for (PageId p = first; p <= last; ++p) {
    map_.clear_sizeclass(p.index());
  }
}

void* MetaDataAlloc(size_t bytes) { return tc_globals.arena().Alloc(bytes); }

}  // namespace tcmalloc_internal
}  // namespace tcmalloc
GOOGLE_MALLOC_SECTION_END
