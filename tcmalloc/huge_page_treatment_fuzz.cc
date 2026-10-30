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

#include <cstddef>
#include <cstdint>

#include "gtest/gtest.h"
#include "fuzztest/fuzztest.h"
#include "absl/strings/match.h"
#include "absl/strings/string_view.h"
#include "absl/time/time.h"
#include "absl/types/span.h"
#include "tcmalloc/common.h"
#include "tcmalloc/huge_page_treatment.h"
#include "tcmalloc/internal/config.h"
#include "tcmalloc/internal/memory_tag.h"
#include "tcmalloc/pages.h"

namespace tcmalloc::tcmalloc_internal {
namespace {

constexpr size_t kMinPageSize = 4096;
constexpr size_t kMaxPagesPerHugePage = kHugePageSize / kMinPageSize;
constexpr size_t kMaxObjectsPerHugePage = kHugePageSize / 8;

auto MemoryTagDomain() {
  return fuzztest::ElementOf({
      MemoryTag::kNormal,
      MemoryTag::kNormalP1,
      MemoryTag::kSampledOrCold,
      MemoryTag::kSampledOrColdP1,
      MemoryTag::kGuarded,
      MemoryTag::kMetadata,
  });
}

void ValidArchitecturalVmaNameIsUntruncated(MemoryTag tag, size_t page_size,
                                            size_t lfr, size_t nallocs,
                                            size_t nobjects,
                                            bool has_dense_spans, bool released,
                                            int64_t age_ms) {
  char buffer[kMaxAnonVmaNameSize];
  const size_t written = FormatSampledTrackerVmaName(
      absl::MakeSpan(buffer), tag, page_size, Length(lfr), nallocs, nobjects,
      has_dense_spans, released, absl::Milliseconds(age_ms));
  EXPECT_LT(written, kMaxAnonVmaNameSize);
  const absl::string_view vma_name(buffer);
  EXPECT_EQ(vma_name.size(), written);
  EXPECT_TRUE(absl::StartsWith(vma_name, "tcmalloc_region_")) << vma_name;
  EXPECT_TRUE(absl::StrContains(vma_name, "_r_0_a_") ||
              absl::StrContains(vma_name, "_r_1_a_"))
      << vma_name;
  ASSERT_GE(vma_name.size(), 4);
  EXPECT_EQ(vma_name.substr(vma_name.size() - 4, 3), "_a_") << vma_name;
  EXPECT_GE(vma_name.back(), '0') << vma_name;
  EXPECT_LE(vma_name.back(), '9') << vma_name;
}

FUZZ_TEST(HugePageTreatmentTest, ValidArchitecturalVmaNameIsUntruncated)
    .WithDomains(MemoryTagDomain(),
                 fuzztest::ElementOf<size_t>({4096, 8192, 32768, 65536,
                                              262144}),
                 fuzztest::InRange<size_t>(0, kMaxPagesPerHugePage),
                 fuzztest::InRange<size_t>(0, kMaxPagesPerHugePage),
                 fuzztest::InRange<size_t>(0, kMaxObjectsPerHugePage),
                 fuzztest::Arbitrary<bool>(), fuzztest::Arbitrary<bool>(),
                 fuzztest::InRange<int64_t>(0, 24LL * 3600 * 1000));

TEST(HugePageTreatmentTest, WorstCaseValidVmaNameFitsWithoutTruncation) {
  char buffer[kMaxAnonVmaNameSize];
  const size_t written_4k = FormatSampledTrackerVmaName(
      absl::MakeSpan(buffer), MemoryTag::kSampledOrColdP1,
      /*page_size=*/4096, /*lfr=*/Length(kMaxPagesPerHugePage),
      /*nallocs=*/kMaxPagesPerHugePage, /*nobjects=*/kMaxObjectsPerHugePage,
      /*has_dense_spans=*/true, /*released=*/true,
      /*time_since_last_alloc=*/absl::InfiniteDuration());
  EXPECT_LT(written_4k, kMaxAnonVmaNameSize);
  EXPECT_STREQ(
      buffer,
      "tcmalloc_region_SAMPLED_OR_COLD_P1_pg_4096_lfr_512_na_512_no_262144_"
      "d_1_r_1_a_9");

  const size_t written_256k = FormatSampledTrackerVmaName(
      absl::MakeSpan(buffer), MemoryTag::kSampledOrColdP1,
      /*page_size=*/262144, /*lfr=*/Length(kMaxPagesPerHugePage),
      /*nallocs=*/kMaxPagesPerHugePage, /*nobjects=*/kMaxObjectsPerHugePage,
      /*has_dense_spans=*/true, /*released=*/true,
      /*time_since_last_alloc=*/absl::InfiniteDuration());
  EXPECT_LT(written_256k, kMaxAnonVmaNameSize);
  EXPECT_STREQ(
      buffer,
      "tcmalloc_region_SAMPLED_OR_COLD_P1_pg_262144_lfr_0_na_0_no_262144_"
      "d_1_r_1_a_9");

  const size_t zero_written = FormatSampledTrackerVmaName(
      absl::MakeSpan(buffer), MemoryTag::kNormal,
      /*page_size=*/8192, /*lfr=*/Length(0), /*nallocs=*/0,
      /*nobjects=*/0, /*has_dense_spans=*/false, /*released=*/false,
      /*time_since_last_alloc=*/absl::ZeroDuration());
  EXPECT_LT(zero_written, kMaxAnonVmaNameSize);
  EXPECT_STREQ(buffer,
               "tcmalloc_region_NORMAL_pg_8192_lfr_0_na_0_no_0_d_0_r_0_a_0");
}

TEST(HugePageTreatmentTest, QuantizeSampledTrackerAgeBucketBoundaries) {
  EXPECT_EQ(QuantizeSampledTrackerAgeBucket(absl::ZeroDuration()), 0);
  EXPECT_EQ(QuantizeSampledTrackerAgeBucket(absl::Minutes(10)), 1);
  EXPECT_EQ(QuantizeSampledTrackerAgeBucket(absl::Hours(5)), 2);
  EXPECT_EQ(QuantizeSampledTrackerAgeBucket(absl::InfiniteDuration()), 9);
}

}  // namespace
}  // namespace tcmalloc::tcmalloc_internal
