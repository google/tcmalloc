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

#include "tcmalloc/internal/compressibility.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/types/span.h"
#include "tcmalloc/internal/util.h"

namespace tcmalloc {
namespace tcmalloc_internal {

CompressionAnalyzer::CompressionAnalyzer(size_t max_local_copy_size)
    : local_copy_(max_local_copy_size)
{}

absl::StatusOr<CompressionAnalyzer::Results> CompressionAnalyzer::Analyze(
    absl::Span<const char> data) {
  Results results;
  bool still_in_trailing_zeroes = true;

  // Walk backwards from the end of the data, copying chunks.
  int64_t end_offset = data.size();
  while (end_offset > 0) {
    int64_t chunk_size =
        std::min(end_offset, static_cast<int64_t>(local_copy_.size()));
    if (!SafeCopyMemory(/*src=*/data.data() + end_offset - chunk_size,
                        /*dst=*/local_copy_.data(), /*size=*/chunk_size)) {
      return absl::InternalError("SafeCopyMemory failed");
    }
    auto chunk = absl::MakeConstSpan(local_copy_.data(), chunk_size);

    // Count zero bytes and trailing zero bytes in chunk.
    for (size_t i = chunk.size(); i > 0; --i) {
      char c = chunk[i - 1];
      if (c == 0) {
        results.zero_bytes++;
        if (still_in_trailing_zeroes) {
          results.trailing_zero_bytes++;
        }
      } else {
        still_in_trailing_zeroes = false;
      }
    }

    end_offset -= chunk_size;
  }

  return results;
}

}  // namespace tcmalloc_internal
}  // namespace tcmalloc
