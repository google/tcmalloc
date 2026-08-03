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
#include <cstddef>
#include <cstring>
#include <random>
#include <vector>

#include "gtest/gtest.h"
#include "absl/status/status_matchers.h"
#include "absl/types/span.h"

namespace tcmalloc {
namespace tcmalloc_internal {
namespace {

TEST(CompressibilityTest, AllZeroes) {
  CompressionAnalyzer analyzer;
  std::vector<char> buf(8199, 0);
  auto res = analyzer.Analyze(absl::MakeConstSpan(buf));
  ABSL_ASSERT_OK(res);
  EXPECT_EQ(res->zero_bytes, 8199);
  EXPECT_EQ(res->trailing_zero_bytes, 8199);
}

TEST(CompressibilityTest, PartialZeroes) {
  CompressionAnalyzer analyzer;
  std::vector<char> buf(8199, 0);
  std::memset(buf.data() + 100, 0x42, 100);
  auto res = analyzer.Analyze(absl::MakeConstSpan(buf));
  ABSL_ASSERT_OK(res);
  EXPECT_EQ(res->zero_bytes, 8199 - 100);
  EXPECT_EQ(res->trailing_zero_bytes, 8199 - 200);
}

TEST(CompressibilityTest, UniformNonZero) {
  CompressionAnalyzer analyzer;
  std::vector<char> buf(8199, 0x42);
  auto res = analyzer.Analyze(absl::MakeConstSpan(buf));
  ABSL_ASSERT_OK(res);
  EXPECT_EQ(res->zero_bytes, 0);
  EXPECT_EQ(res->trailing_zero_bytes, 0);
}

TEST(CompressibilityTest, PseudoRandomUncompressible) {
  CompressionAnalyzer analyzer;
  std::vector<char> buf(8199);
  std::minstd_rand rng(12345);
  std::generate(buf.begin(), buf.end(),
                [&]() { return static_cast<char>(rng() | 1); });
  auto res = analyzer.Analyze(absl::MakeConstSpan(buf));
  ABSL_ASSERT_OK(res);
  EXPECT_EQ(res->zero_bytes, 0);
  EXPECT_EQ(res->trailing_zero_bytes, 0);
}

TEST(CompressibilityTest, MultiChunkScanning) {
  CompressionAnalyzer analyzer;
  // 5MB allocation to test multi-chunk scanning (>2MB)
  size_t huge_size = 5 * 1024 * 1024 + 123;
  std::vector<char> buf(huge_size);
  std::memset(buf.data(), 0x42, 4 * 1024 * 1024);
  std::memset(buf.data() + 4 * 1024 * 1024, 0, 1 * 1024 * 1024 + 123);

  auto res = analyzer.Analyze(absl::MakeConstSpan(buf));
  ABSL_ASSERT_OK(res);
  EXPECT_EQ(res->zero_bytes, 1 * 1024 * 1024 + 123);
  EXPECT_EQ(res->trailing_zero_bytes, 1 * 1024 * 1024 + 123);
}

TEST(CompressibilityTest, TrailingZeroesSkippedInCompression) {
  CompressionAnalyzer analyzer;
  size_t huge_size = 5 * 1024 * 1024 + 123;
  std::vector<char> buf(huge_size);
  std::minstd_rand rng(12345);
  std::generate(buf.begin(), buf.begin() + 4 * 1024 * 1024,
                [&]() { return static_cast<char>(rng() | 1); });
  std::memset(buf.data() + 4 * 1024 * 1024, 0, 1 * 1024 * 1024 + 123);

  auto res = analyzer.Analyze(absl::MakeConstSpan(buf));
  ABSL_ASSERT_OK(res);
  EXPECT_EQ(res->zero_bytes, 1 * 1024 * 1024 + 123);
  EXPECT_EQ(res->trailing_zero_bytes, 1 * 1024 * 1024 + 123);
}

#if defined(__linux__)
TEST(CompressibilityTest, SafeMemoryCopyFailure) {
  CompressionAnalyzer analyzer;
  size_t page_size = sysconf(_SC_PAGESIZE);
  void* prot_none =
      mmap(nullptr, page_size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (prot_none != MAP_FAILED) {
    auto res = analyzer.Analyze(
        absl::MakeConstSpan(static_cast<const char*>(prot_none), page_size));
    EXPECT_FALSE(res.ok());
    munmap(prot_none, page_size);
  }
}
#endif  // defined(__linux__)

}  // namespace
}  // namespace tcmalloc_internal
}  // namespace tcmalloc
