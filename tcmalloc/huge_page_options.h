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

#ifndef TCMALLOC_HUGE_PAGE_OPTIONS_H_
#define TCMALLOC_HUGE_PAGE_OPTIONS_H_

#include <cstdint>

#include "tcmalloc/internal/config.h"

GOOGLE_MALLOC_SECTION_BEGIN
namespace tcmalloc::tcmalloc_internal {

enum class HugePageTreatmentType : uint8_t {
  kSampled = 1 << 0,
  kCollapse = 1 << 1,
};

enum class EnableCollapse : uint8_t {
  kDisabled = false,
  kEnabled = true,
};

template <typename Sink>
inline void AbslStringify(Sink& sink, EnableCollapse v) {
  switch (v) {
    case EnableCollapse::kDisabled:
      sink.Append("EnableCollapse::kDisabled");
      return;
    case EnableCollapse::kEnabled:
      sink.Append("EnableCollapse::kEnabled");
      return;
  }
}

enum class EnableUnfilteredCollapse : bool {
  kDisabled = false,
  kEnabled = true,
};

enum class CollapseReleasedHugePages : bool {
  kDisabled = false,
  kEnabled = true,
};

enum class ReleaseStalePages : bool {
  kDisabled = false,
  kEnabled = true,
};

enum class MadviseRegionsNoHugepage : bool {
  kDisabled = false,
  kEnabled = true,
};

enum class TrackBrokenHugepages : bool {
  kDisabled = false,
  kEnabled = true,
};

enum class PreserveDonatedOnCollapse : bool {
  kDisabled = false,
  kEnabled = true,
};

enum class PreferBackedPages : bool {
  kDisabled = false,
  kEnabled = true,
};

template <typename Sink>
inline void AbslStringify(Sink& sink, PreferBackedPages v) {
  switch (v) {
    case PreferBackedPages::kDisabled:
      sink.Append("false");
      return;
    case PreferBackedPages::kEnabled:
      sink.Append("true");
      return;
  }
}

// Controls where HugePageFiller::Put re-lists a hugepage after pages are freed
// into it.  When disabled, the hugepage is prepended to its tracker list and
// becomes the next allocation target among hugepages in that list.  When
// enabled, it is appended instead: a hugepage that is draining is likely to
// keep draining, so we would rather fill one that has not recently had pages
// freed and let this one empty out so it can be returned whole.
enum class FillerAppendOnFree : bool {
  kDisabled = false,
  kEnabled = true,
};

template <typename Sink>
inline void AbslStringify(Sink& sink, FillerAppendOnFree v) {
  switch (v) {
    case FillerAppendOnFree::kDisabled:
      sink.Append("false");
      return;
    case FillerAppendOnFree::kEnabled:
      sink.Append("true");
      return;
  }
}

enum class HugeRegionUsageOption : bool {
  // This is a default behavior. We use slack to determine when to use
  // HugeRegion. When slack is greater than 64MB (to ignore small binaries), and
  // greater than the number of small allocations, we allocate large allocations
  // from HugeRegion.
  kDefault,
  // When the experiment TEST_ONLY_TCMALLOC_USE_HUGE_REGIONS_MORE_OFTEN is
  // enabled, we use number of abandoned pages in addition to slack to make a
  // decision. If the size of abandoned pages plus slack exceeds 64MB (to ignore
  // small binaries), we use HugeRegion for large allocations.
  kUseForAllLargeAllocs
};

enum class SubreleaseUnbackedMode : bool {
  kDisabled = false,
  kEnabled = true,
};

enum class ReleasePartialAllocPages : bool {
  kDisabled = false,
  kEnabled = true,
};

enum class HugeRegionAdaptiveRelease : bool {
  kDisabled = false,
  kEnabled = true,
};

}  // namespace tcmalloc::tcmalloc_internal
GOOGLE_MALLOC_SECTION_END

#endif  // TCMALLOC_HUGE_PAGE_OPTIONS_H_
