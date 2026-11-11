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

#ifndef TCMALLOC_INTERNAL_COMPRESSIBILITY_H_
#define TCMALLOC_INTERNAL_COMPRESSIBILITY_H_

#include <cstddef>

#include "absl/container/fixed_array.h"
#include "absl/random/random.h"
#include "absl/status/statusor.h"
#include "absl/types/span.h"
#include "tcmalloc/internal/residency.h"

namespace tcmalloc {
namespace tcmalloc_internal {

// Estimates how compressible sampled heap allocations are.
//
// Compressibility analysis across a heap profile operates in three bounded
// stages so collection work stays bounded on multi-GiB heaps:
//
// 1. Per-span window (`kDefaultMaxLocalCopySize`): For an individual span,
//    `CompressionAnalyzer::Analyze` copies at most 64 KiB of resident pages
//    into a local buffer (selecting a random contiguous window of resident
//    pages when the span has more than 64 KiB resident) and scales the
//    measured ZSTD compression and zero-byte ratios to the span's backed
//    (resident + swapped) bytes.
// 2. Per-group reservoir (`kMaxCompressibilityBytesPerGroup`,
//    `kMaxCompressibilitySamplesPerGroup`): During the first pass of profile
//    merging, raw samples are grouped by callstack and each group retains a
//    uniform random reservoir (Algorithm R) of resident candidate spans,
//    capped at 2 MiB of resident sample budget per group (at most 32 spans of
//    64 KiB each for large allocations) and at most 512 spans for small size
//    classes.
// 3. Per-profile budget (`kMaxCompressibilityBytesPerProfile`): During the
//    second pass, candidate spans are analyzed in round-robin order across all
//    merged callstack groups until the total resident bytes copied across the
//    profile reaches 256 MiB. Each group's measured compressed and backed-zero
//    bytes are then scaled from its analyzed backed bytes
//    (`analyzed_compressibility_space`) to the group's total backed bytes,
//    falling back to incompressible if no backed bytes in the group were
//    analyzed.
//
// A `CompressionAnalyzer` instance owns scratch buffers (and a compression
// context) that are reused across calls to `Analyze()`, so it is not safe to
// call `Analyze()` concurrently on the same instance. Callers use one instance
// per (serial) profile walk.
class CompressionAnalyzer {
 public:
  // Maximum resident bytes copied and compressed from a single span.
  static constexpr size_t kDefaultMaxLocalCopySize = 64 * 1024;

  // Target maximum resident bytes copied and compressed for a single merged
  // callstack group (at most 32 spans of 64 KiB each for large allocations).
  static constexpr size_t kMaxCompressibilityBytesPerGroup = 2 * 1024 * 1024;

  // Maximum number of sampled spans retained in a merged callstack group's
  // reservoir for small size classes.
  static constexpr size_t kMaxCompressibilitySamplesPerGroup = 512;

  // Maximum total resident bytes copied and compressed by `CompressionAnalyzer`
  // across an entire heap profile conversion.
  static constexpr size_t kMaxCompressibilityBytesPerProfile =
      256 * 1024 * 1024;

  explicit CompressionAnalyzer(
      size_t max_local_copy_size = kDefaultMaxLocalCopySize);
  ~CompressionAnalyzer();

  CompressionAnalyzer(const CompressionAnalyzer&) = delete;
  CompressionAnalyzer& operator=(const CompressionAnalyzer&) = delete;

  struct Results {
    size_t compressed_size = 0;
    size_t zero_bytes = 0;
  };

  absl::StatusOr<Results> Analyze(absl::Span<const char> data,
                                  const Residency::Info& residency_info);

 private:
  absl::FixedArray<char> local_copy_;
  absl::BitGen rng_;
};

}  // namespace tcmalloc_internal
}  // namespace tcmalloc

#endif  // TCMALLOC_INTERNAL_COMPRESSIBILITY_H_
