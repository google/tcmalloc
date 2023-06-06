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

#include "tcmalloc/huge_page_aware_allocator.h"

#include <stdint.h>
#include <string.h>

#include <new>

#include "absl/base/internal/cycleclock.h"
#include "absl/base/internal/spinlock.h"
#include "absl/base/thread_annotations.h"
#include "absl/time/time.h"
#include "tcmalloc/common.h"
#include "tcmalloc/experiment.h"
#include "tcmalloc/experiment_config.h"
#include "tcmalloc/huge_allocator.h"
#include "tcmalloc/huge_page_filler.h"
#include "tcmalloc/huge_pages.h"
#include "tcmalloc/internal/environment.h"
#include "tcmalloc/internal/logging.h"
#include "tcmalloc/internal/optimization.h"
#include "tcmalloc/internal/prefetch.h"
#include "tcmalloc/pagemap.h"
#include "tcmalloc/parameters.h"
#include "tcmalloc/span.h"
#include "tcmalloc/static_vars.h"
#include "tcmalloc/stats.h"
#include "tcmalloc/system-alloc.h"

GOOGLE_MALLOC_SECTION_BEGIN
namespace tcmalloc {
namespace tcmalloc_internal {

bool decide_want_hpaa();
ABSL_ATTRIBUTE_WEAK int default_want_hpaa();
ABSL_ATTRIBUTE_WEAK int default_subrelease();

namespace huge_page_allocator_internal {

bool decide_subrelease() {
  if (!decide_want_hpaa()) {
    // Subrelease is off if HPAA is off.
    return false;
  }

  const char* e = thread_safe_getenv("TCMALLOC_HPAA_CONTROL");
  if (e) {
    switch (e[0]) {
      case '0':
        if (default_want_hpaa != nullptr) {
          int default_hpaa = default_want_hpaa();
          if (default_hpaa < 0) {
            return false;
          }
        }

        Log(kLog, __FILE__, __LINE__,
            "Runtime opt-out from HPAA requires building with "
            "//tcmalloc:want_no_hpaa."
        );
        break;
      case '1':
        return false;
      case '2':
        return true;
      default:
        Crash(kCrash, __FILE__, __LINE__, "bad env var", e);
        return false;
    }
  }

  if (default_subrelease != nullptr) {
    const int decision = default_subrelease();
    if (decision != 0) {
      return decision > 0;
    }
  }

  return true;
}

extern "C" ABSL_ATTRIBUTE_WEAK bool
default_want_disable_huge_region_more_often();

bool use_huge_region_more_often() {
  // Disable huge regions more often feature if built against an opt-out.
  if (default_want_disable_huge_region_more_often != nullptr) {
    return false;
  }

  const char* e =
      thread_safe_getenv("TCMALLOC_USE_HUGE_REGION_MORE_OFTEN_DISABLE");
  if (e) {
    switch (e[0]) {
      case '0':
        return true;
      case '1':
        return false;
      default:
        Crash(kCrash, __FILE__, __LINE__, "bad env var", e);
        return false;
    }
  }

  return true;
}

HugeRegionUsageOption huge_region_option() {
  // By default, we use slack to determine when to use HugeRegion. When slack is
  // greater than 64MB (to ignore small binaries), and greater than the number
  // of small allocations, we allocate large allocations from HugeRegion.
  //
  // When huge-region-more-often feature is enabled, we use number of abandoned
  // pages in addition to slack to make a decision. If the size of abandoned
  // pages plus slack exceeds 64MB (to ignore small binaries), we use HugeRegion
  // for large allocations. This results in using HugeRegions for all the large
  // allocations once the size exceeds 64MB.
  return use_huge_region_more_often()
             ? HugeRegionUsageOption::kUseForAllLargeAllocs
             : HugeRegionUsageOption::kDefault;
}

Arena& StaticForwarder::arena() { return tc_globals.arena(); }

void* StaticForwarder::GetHugepage(HugePage p) {
  return tc_globals.pagemap().GetHugepage(p.first_page());
}

bool StaticForwarder::Ensure(PageId page, Length length) {
  return tc_globals.pagemap().Ensure(page, length);
}

void StaticForwarder::Set(PageId page, Span* span) {
  tc_globals.pagemap().Set(page, span);
}

void StaticForwarder::SetHugepage(HugePage p, void* pt) {
  tc_globals.pagemap().SetHugepage(p.first_page(), pt);
}

void StaticForwarder::ShrinkToUsageLimit(Length n) {
  tc_globals.page_allocator().ShrinkToUsageLimit(n);
}

Span* StaticForwarder::NewSpan(PageId page, Length length) {
  // TODO(b/134687001):  Delete this when span_allocator moves.
  return Span::New(page, length);
}

void StaticForwarder::DeleteSpan(Span* span) { Span::Delete(span); }

}  // namespace huge_page_allocator_internal

}  // namespace tcmalloc_internal
}  // namespace tcmalloc
GOOGLE_MALLOC_SECTION_END
