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

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>

#include "gtest/gtest.h"
#include "fuzztest/fuzztest.h"
#include "absl/types/span.h"
#include "tcmalloc/internal/compressibility.h"
#include "tcmalloc/internal/page_size.h"
#include "tcmalloc/internal/residency.h"

namespace tcmalloc {
namespace tcmalloc_internal {
namespace {

void FuzzAnalyze(std::string data, size_t max_local_copy_size,
                 std::vector<bool> page_resident_bits, size_t bytes_swapped) {
  CompressionAnalyzer analyzer(max_local_copy_size);
  const size_t page_size = GetPageSize();
  const uintptr_t uaddr = reinterpret_cast<uintptr_t>(data.data());
  const size_t start_page = uaddr / page_size;
  const size_t end_page = (uaddr + data.size() + page_size - 1) / page_size;
  const size_t num_pages = end_page - start_page;

  if (page_resident_bits.size() < num_pages) {
    page_resident_bits.resize(num_pages, false);
  }

  size_t bytes_resident = 0;
  for (size_t i = 0; i < num_pages; ++i) {
    if (!page_resident_bits[i]) continue;
    const uintptr_t page_start_addr = (start_page + i) * page_size;
    const uintptr_t page_end_addr = page_start_addr + page_size;
    const uintptr_t chunk_start = std::max(uaddr, page_start_addr);
    const uintptr_t chunk_end = std::min(uaddr + data.size(), page_end_addr);
    if (chunk_end > chunk_start) {
      bytes_resident += (chunk_end - chunk_start);
    }
  }

  Residency::Info info;
  info.bytes_resident = bytes_resident;
  info.bytes_swapped = std::min(data.size() - bytes_resident, bytes_swapped);
  for (size_t i = 0; i < std::min(num_pages, kMaxResidencyBits); ++i) {
    if (page_resident_bits[i]) {
      info.page_is_resident.SetBit(i);
    }
  }

  auto res = analyzer.Analyze(absl::MakeConstSpan(data), info);
  if (res.ok()) {
    EXPECT_LE(res->zero_bytes, data.size());
  }
}

FUZZ_TEST(CompressibilityFuzzTest, FuzzAnalyze)
    .WithDomains(fuzztest::String(), fuzztest::InRange<size_t>(1, 1024),
                 fuzztest::Arbitrary<std::vector<bool>>(),
                 fuzztest::Arbitrary<size_t>());

}  // namespace
}  // namespace tcmalloc_internal
}  // namespace tcmalloc
