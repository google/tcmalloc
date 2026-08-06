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

enum class EnableCollapse : bool {
  kDisabled = false,
  kEnabled = true,
};

enum class EnableUnfilteredCollapse : bool {
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

}  // namespace tcmalloc::tcmalloc_internal
GOOGLE_MALLOC_SECTION_END

#endif  // TCMALLOC_HUGE_PAGE_OPTIONS_H_
