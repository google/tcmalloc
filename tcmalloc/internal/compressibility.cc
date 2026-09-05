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
#include <vector>

#include "absl/algorithm/container.h"
#include "absl/status/status.h"
#include "absl/status/status_macros.h"
#include "absl/status/statusor.h"
#include "absl/strings/string_view.h"
#include "absl/types/span.h"
#include "tcmalloc/internal/page_size.h"
#include "tcmalloc/internal/residency.h"
#include "tcmalloc/internal/util.h"

namespace tcmalloc {
namespace tcmalloc_internal {

namespace {

constexpr uintptr_t AlignDown(uintptr_t addr, size_t alignment) {
  return addr & ~(alignment - 1);
}

constexpr uintptr_t AlignUp(uintptr_t addr, size_t alignment) {
  return (addr + alignment - 1) & ~(alignment - 1);
}

// Copies up to `dst.size()` resident bytes from `data` into `dst`, skipping any
// unbacked or swapped pages and concatenating the resident pages.
// Returns the number of bytes copied.
absl::StatusOr<size_t> CopyResidentPages(absl::Span<const char> data,
                                         const Residency::Info& residency_info,
                                         absl::Span<char> dst) {
  const size_t hardware_page_size = GetPageSize();

  const uintptr_t uaddr = reinterpret_cast<uintptr_t>(data.data());
  const uintptr_t start_page_addr = AlignDown(uaddr, hardware_page_size);
  const uintptr_t end_page_addr =
      AlignUp(uaddr + data.size(), hardware_page_size);
  const size_t num_pages =
      (end_page_addr - start_page_addr) / hardware_page_size;
  const size_t pages_to_scan =
      std::min(num_pages, residency_info.page_is_resident.size());

  std::vector<absl::string_view> remote_chunks;
  size_t total_resident_bytes = 0;
  size_t page_index = 0;

  do {
    // Find the next range of resident pages.
    size_t start_page_index =
        residency_info.page_is_resident.FindSet(page_index);
    if (start_page_index >= pages_to_scan) {
      break;
    }
    size_t end_page_index =
        residency_info.page_is_resident.FindClear(start_page_index);
    end_page_index = std::min(end_page_index, pages_to_scan);

    const uintptr_t chunk_start = std::max(
        uaddr, start_page_addr + start_page_index * hardware_page_size);
    const uintptr_t chunk_end =
        std::min(uaddr + data.size(),
                 start_page_addr + end_page_index * hardware_page_size);
    const size_t chunk_bytes = chunk_end - chunk_start;
    const size_t to_copy =
        std::min(chunk_bytes, dst.size() - total_resident_bytes);
    remote_chunks.push_back(
        absl::string_view(reinterpret_cast<const char*>(chunk_start), to_copy));
    total_resident_bytes += to_copy;

    page_index = end_page_index;
  } while (page_index < pages_to_scan && total_resident_bytes < dst.size());

  if (total_resident_bytes > 0) {
    if (!SafeCopyMemory(remote_chunks, /*dst=*/dst.data())) {
      return absl::InternalError("SafeCopyMemory failed");
    }
  }

  return total_resident_bytes;
}

// Extrapolates zero bytes in backed memory by scaling zeroes measured within
// the resident sample to the allocation's total backed (resident + swapped)
// size.
size_t EstimateZeroBytes(const Residency::Info& residency_info,
                         absl::Span<const char> resident_sample) {
  const size_t total_backed =
      residency_info.bytes_resident + residency_info.bytes_swapped;
  if (resident_sample.empty()) {
    return 0;
  }

  const size_t sample_zeroes = absl::c_count(resident_sample, '\0');
  const double zero_ratio =
      static_cast<double>(sample_zeroes) / resident_sample.size();
  const size_t estimated_backed_zeroes =
      static_cast<size_t>(zero_ratio * total_backed);

  return std::min(total_backed, estimated_backed_zeroes);
}

}  // namespace

CompressionAnalyzer::CompressionAnalyzer(size_t max_local_copy_size)
    : local_copy_(max_local_copy_size)
{}

absl::StatusOr<CompressionAnalyzer::Results> CompressionAnalyzer::Analyze(
    absl::Span<const char> data, const Residency::Info& residency_info) {
  // Sample resident pages up to local buffer capacity (2MB).
  ABSL_ASSIGN_OR_RETURN(
      const size_t copied_bytes,
      CopyResidentPages(data, residency_info, absl::MakeSpan(local_copy_)));

  const absl::Span<const char> resident_sample =
      absl::MakeConstSpan(local_copy_).first(copied_bytes);

  Results results;
  results.zero_bytes = EstimateZeroBytes(residency_info, resident_sample);

  return results;
}

}  // namespace tcmalloc_internal
}  // namespace tcmalloc
