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

#ifndef TCMALLOC_TESTING_SMAPS_H_
#define TCMALLOC_TESTING_SMAPS_H_

#include <cstddef>
#include <cstdint>

#include "absl/functional/function_ref.h"
#include "absl/strings/string_view.h"

namespace tcmalloc {

struct SmapEntry {
  uintptr_t start = 0;
  uintptr_t end = 0;
  size_t rss_bytes = 0;
  char vm_flags[128] = {};
  size_t vm_flags_len = 0;

  [[nodiscard]] bool Overlaps(const void* addr, size_t size) const {
    const uintptr_t target_start = reinterpret_cast<uintptr_t>(addr);
    const uintptr_t target_end = target_start + size;
    if (target_end <= target_start) {
      return false;
    }
    return start < target_end && end > target_start;
  }

  [[nodiscard]] bool HasVmFlag(absl::string_view flag) const;
};

// Parses smaps content (in the format of /proc/self/smaps) and invokes
// `callback` for each VMA entry.
void ParseSmaps(absl::string_view content,
                absl::FunctionRef<void(const SmapEntry&)> callback);

// Reads /proc/self/smaps and invokes `callback` for each VMA entry. Returns
// true on success.
[[nodiscard]] bool ForEachSmapEntry(
    absl::FunctionRef<void(const SmapEntry&)> callback);

}  // namespace tcmalloc

#endif  // TCMALLOC_TESTING_SMAPS_H_
