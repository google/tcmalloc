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

#include <sys/mman.h>
#include <unistd.h>

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <new>
#include <random>
#include <vector>

#include "gtest/gtest.h"
#include "absl/status/status_matchers.h"
#include "absl/strings/string_view.h"
#include "absl/types/span.h"
#include "tcmalloc/internal/page_size.h"
#include "tcmalloc/internal/range_tracker.h"
#include "tcmalloc/internal/residency.h"

namespace tcmalloc {
namespace tcmalloc_internal {
namespace {

struct PageAlignedBuffer {
  explicit PageAlignedBuffer(size_t num_pages)
      : size(num_pages * GetPageSize()),
        alignment(GetPageSize()),
        ptr(static_cast<char*>(
            ::operator new(size, std::align_val_t(alignment)))) {}

  ~PageAlignedBuffer() { ::operator delete(ptr, std::align_val_t(alignment)); }

  PageAlignedBuffer(const PageAlignedBuffer&) = delete;
  PageAlignedBuffer& operator=(const PageAlignedBuffer&) = delete;

  size_t size;
  size_t alignment;
  char* ptr;
};

// Builds Residency::Info for `[addr, addr + size)` using a character code
// pattern across the hardware pages spanned by the region:
//   'R': Resident
//   'S': Swapped
//   'U': Unbacked (not present)
// If `pattern` is a single character, it is repeated for all spanned pages.
Residency::Info MakeResidencyInfo(const void* addr, size_t size,
                                  absl::string_view pattern) {
  const size_t page_size = GetPageSize();
  const uintptr_t uaddr = reinterpret_cast<uintptr_t>(addr);
  const size_t start_page = uaddr / page_size;
  const size_t end_page = (uaddr + size + page_size - 1) / page_size;
  const size_t num_pages = end_page - start_page;

  assert(pattern.size() == 1 || pattern.size() == num_pages);

  Residency::Info info;
  for (size_t i = 0; i < num_pages; ++i) {
    const char code = (pattern.size() == 1) ? pattern[0] : pattern[i];
    const uintptr_t page_start_addr = (start_page + i) * page_size;
    const uintptr_t page_end_addr = page_start_addr + page_size;
    const uintptr_t chunk_start = std::max(uaddr, page_start_addr);
    const uintptr_t chunk_end = std::min(uaddr + size, page_end_addr);
    const size_t chunk_bytes =
        (chunk_end > chunk_start) ? (chunk_end - chunk_start) : 0;

    switch (code) {
      case 'R':
        info.bytes_resident += chunk_bytes;
        if (i < kMaxResidencyBits) {
          info.page_is_resident.SetBit(i);
        }
        break;
      case 'S':
        info.bytes_swapped += chunk_bytes;
        break;
      case 'U':
        break;
      default:
        assert(false && "Unknown residency pattern code");
    }
  }
  return info;
}

TEST(CompressibilityTest, AllZeroes) {
  CompressionAnalyzer analyzer;
  std::vector<char> buf(8199, 0);
  auto res = analyzer.Analyze(absl::MakeConstSpan(buf),
                              MakeResidencyInfo(buf.data(), buf.size(), "R"));
  ABSL_ASSERT_OK(res);
  EXPECT_EQ(res->zero_bytes, 8199);
}

TEST(CompressibilityTest, PartialZeroes) {
  CompressionAnalyzer analyzer;
  std::vector<char> buf(8199, 0);
  std::memset(buf.data() + 100, 0x42, 100);
  auto res = analyzer.Analyze(absl::MakeConstSpan(buf),
                              MakeResidencyInfo(buf.data(), buf.size(), "R"));
  ABSL_ASSERT_OK(res);
  EXPECT_EQ(res->zero_bytes, 8199 - 100);
}

TEST(CompressibilityTest, UniformNonZero) {
  CompressionAnalyzer analyzer;
  std::vector<char> buf(8199, 0x42);
  auto res = analyzer.Analyze(absl::MakeConstSpan(buf),
                              MakeResidencyInfo(buf.data(), buf.size(), "R"));
  ABSL_ASSERT_OK(res);
  EXPECT_EQ(res->zero_bytes, 0);
}

TEST(CompressibilityTest, PseudoRandomUncompressible) {
  CompressionAnalyzer analyzer;
  std::vector<char> buf(8199);
  std::minstd_rand rng(12345);
  std::generate(buf.begin(), buf.end(),
                [&]() { return static_cast<char>(rng() | 1); });
  auto res = analyzer.Analyze(absl::MakeConstSpan(buf),
                              MakeResidencyInfo(buf.data(), buf.size(), "R"));
  ABSL_ASSERT_OK(res);
  EXPECT_LE(res->zero_bytes, 100);
}

TEST(CompressibilityTest, MultiChunkScanning) {
  CompressionAnalyzer analyzer;
  // 5MB allocation: first 2MB is sampled and extrapolated.
  const size_t page_size = GetPageSize();
  const size_t num_pages = (5 * 1024 * 1024) / page_size;
  PageAlignedBuffer buf(num_pages);
  std::memset(buf.ptr, 0, buf.size);
  // First 1MB is non-zero, next 1MB is zero (50% zero ratio in sample).
  std::memset(buf.ptr, 0x42, 1 * 1024 * 1024);

  auto res = analyzer.Analyze(absl::MakeConstSpan(buf.ptr, buf.size),
                              MakeResidencyInfo(buf.ptr, buf.size, "R"));
  ABSL_ASSERT_OK(res);
  EXPECT_EQ(res->zero_bytes, buf.size / 2);
}

#if defined(__linux__)
TEST(CompressibilityTest, SafeMemoryCopyFailure) {
  CompressionAnalyzer analyzer;
  size_t page_size = sysconf(_SC_PAGESIZE);
  void* prot_none =
      mmap(nullptr, page_size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (prot_none != MAP_FAILED) {
    auto res = analyzer.Analyze(
        absl::MakeConstSpan(static_cast<const char*>(prot_none), page_size),
        MakeResidencyInfo(prot_none, page_size, "R"));
    EXPECT_FALSE(res.ok());
    munmap(prot_none, page_size);
  }
}
#endif  // defined(__linux__)

TEST(CompressibilityTest, ZeroTailWithResidency) {
  const size_t page_size = GetPageSize();
  PageAlignedBuffer buffer(/*num_pages=*/3);
  std::memset(buffer.ptr, 0x42, page_size);  // Page 0 has non-zero data
  std::memset(buffer.ptr + page_size, 0,
              2 * page_size);  // Pages 1 & 2 unbacked

  CompressionAnalyzer analyzer(100);
  auto res =
      analyzer.Analyze(absl::MakeConstSpan(buffer.ptr, buffer.size),
                       MakeResidencyInfo(buffer.ptr, buffer.size, "RUU"));
  ABSL_ASSERT_OK(res);
  EXPECT_EQ(res->zero_bytes, 0);
}

TEST(CompressibilityTest, SwappedPagesSkipped) {
  CompressionAnalyzer analyzer(100);
  std::vector<char> buf(100, 0x42);  // Non-zero data, but "swapped" out

  auto res = analyzer.Analyze(absl::MakeConstSpan(buf),
                              MakeResidencyInfo(buf.data(), buf.size(), "S"));
  ABSL_ASSERT_OK(res);
  // Swapped pages are backed memory (not unbacked zeroes), so zero_bytes is 0
  // and compressed_size is 100.
  EXPECT_EQ(res->zero_bytes, 0);
}

TEST(CompressibilityTest, MisalignedAllocation) {
  const size_t page_size = GetPageSize();
  PageAlignedBuffer buffer(/*num_pages=*/4);
  std::memset(buffer.ptr, 0, buffer.size);

  // Offset 500 bytes into page 0, spanning through page 1 and partway into
  // page 2.
  const size_t offset_in_page = 500;
  char* start_ptr = buffer.ptr + offset_in_page;
  const size_t alloc_size = 2 * page_size + 300;
  std::memset(start_ptr, 0x42, 100);  // 100 non-zero bytes

  CompressionAnalyzer analyzer;
  auto res = analyzer.Analyze(absl::MakeConstSpan(start_ptr, alloc_size),
                              MakeResidencyInfo(start_ptr, alloc_size, "R"));
  ABSL_ASSERT_OK(res);
  EXPECT_EQ(res->zero_bytes, alloc_size - 100);
}

TEST(CompressibilityTest, ResidencyGap) {
  const size_t page_size = GetPageSize();
  PageAlignedBuffer buffer(/*num_pages=*/3);
  // Page 0: non-zero data (0x42)
  // Page 1: unbacked gap (0xAA)
  // Page 2: zeroes
  std::memset(buffer.ptr, 0x42, page_size);
  std::memset(buffer.ptr + page_size, 0xAA, page_size);
  std::memset(buffer.ptr + 2 * page_size, 0, page_size);

  CompressionAnalyzer analyzer;
  auto res =
      analyzer.Analyze(absl::MakeConstSpan(buffer.ptr, buffer.size),
                       MakeResidencyInfo(buffer.ptr, buffer.size, "RUR"));
  ABSL_ASSERT_OK(res);
  // Page 1 is unbacked.
  // Pages 0 & 2 are resident (page 0 has 0 zeroes, page 2 has 1 page of zeroes
  // -> 50% ratio). Total backed is 2 pages. Backed zeroes = 50% of 2 pages = 1
  // page of zeroes.
  EXPECT_EQ(res->zero_bytes, page_size);
}

TEST(CompressibilityTest, MisalignedAllocationWithResidencyGap) {
  const size_t page_size = GetPageSize();
  PageAlignedBuffer buffer(/*num_pages=*/4);
  std::memset(buffer.ptr, 0x42, buffer.size);

  // Start 500 bytes into page 0:
  // - Page 0 (partial): bytes [500, page_size) = page_size - 500 bytes
  // (non-zero)
  // - Page 1 (full gap): bytes [page_size, 2 * page_size) = page_size bytes
  // (unbacked)
  // - Page 2 (partial): bytes [2 * page_size, 2 * page_size + 1300) = 1300
  // bytes (zeroes) Total alloc_size = (page_size - 500) + page_size + 1300 = 2
  // * page_size + 800.
  const size_t offset_in_page = 500;
  char* start_ptr = buffer.ptr + offset_in_page;
  const size_t alloc_size = 2 * page_size + 800;

  // Zero out the resident Page 2.
  std::memset(buffer.ptr + 2 * page_size, 0, page_size);

  CompressionAnalyzer analyzer;
  auto res = analyzer.Analyze(absl::MakeConstSpan(start_ptr, alloc_size),
                              MakeResidencyInfo(start_ptr, alloc_size, "RUR"));
  ABSL_ASSERT_OK(res);

  // Backed bytes: Page 0 partial (page_size - 500) + Page 2 partial (1300).
  // Resident sample: Page 0 has 0 zeroes, Page 2 has 1300 zeroes.
  // Total backed zeroes = 1300.
  EXPECT_EQ(res->zero_bytes, 1300);
}

TEST(CompressibilityTest, BackedZeroBytesDoesNotExceedTotalBacked) {
  const size_t page_size = GetPageSize();
  PageAlignedBuffer buffer(/*num_pages=*/4);
  // Page 0: resident with non-zero data.
  // Pages 1, 2, 3: unbacked.
  std::memset(buffer.ptr, 0x42, page_size);
  std::memset(buffer.ptr + page_size, 0, 3 * page_size);

  CompressionAnalyzer analyzer;
  Residency::Info residency_info =
      MakeResidencyInfo(buffer.ptr, buffer.size, "RUUU");
  auto res = analyzer.Analyze(absl::MakeConstSpan(buffer.ptr, buffer.size),
                              residency_info);
  ABSL_ASSERT_OK(res);
  const size_t total_backed =
      residency_info.bytes_resident + residency_info.bytes_swapped;
  EXPECT_LE(res->zero_bytes, total_backed);
  EXPECT_EQ(res->zero_bytes, 0);
}

TEST(CompressibilityTest, CompletelyUnbackedAllocation) {
  PageAlignedBuffer buffer(/*num_pages=*/3);
  std::memset(buffer.ptr, 0, buffer.size);

  CompressionAnalyzer analyzer;
  auto res =
      analyzer.Analyze(absl::MakeConstSpan(buffer.ptr, buffer.size),
                       MakeResidencyInfo(buffer.ptr, buffer.size, "UUU"));
  ABSL_ASSERT_OK(res);
  EXPECT_EQ(res->zero_bytes, 0);
}

}  // namespace
}  // namespace tcmalloc_internal
}  // namespace tcmalloc
