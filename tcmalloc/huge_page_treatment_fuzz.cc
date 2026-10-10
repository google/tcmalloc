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

#ifdef __linux__
#include <linux/prctl.h>
#endif

#include <cstddef>
#include <cstring>

#include "gtest/gtest.h"
#include "fuzztest/fuzztest.h"
#include "absl/strings/match.h"
#include "absl/strings/string_view.h"
#include "absl/types/span.h"
#include "tcmalloc/common.h"
#include "tcmalloc/huge_page_treatment.h"
#include "tcmalloc/internal/config.h"
#include "tcmalloc/internal/memory_tag.h"
#include "tcmalloc/pages.h"

namespace tcmalloc::tcmalloc_internal {
namespace {

#ifdef ANON_VMA_NAME_MAX_LEN
static_assert(kMaxAnonVmaNameSize == ANON_VMA_NAME_MAX_LEN);
#endif

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
                                            bool has_dense_spans,
                                            bool released) {
  char buffer[kMaxAnonVmaNameSize];
  const size_t written = FormatSampledTrackerVmaName(
      absl::MakeSpan(buffer), tag, page_size, Length(lfr), nallocs, nobjects,
      has_dense_spans, released);
  EXPECT_LT(written, kMaxAnonVmaNameSize);
  const absl::string_view vma_name(buffer);
  EXPECT_EQ(vma_name.size(), written);
  EXPECT_TRUE(absl::StartsWith(vma_name, "tcmalloc_region_")) << vma_name;
  EXPECT_TRUE(absl::EndsWith(vma_name, "_r_0") ||
              absl::EndsWith(vma_name, "_r_1"))
      << vma_name;
}

FUZZ_TEST(HugePageTreatmentTest, ValidArchitecturalVmaNameIsUntruncated)
    .WithDomains(MemoryTagDomain(),
                 fuzztest::ElementOf<size_t>({4096, 8192, 32768, 65536,
                                              262144}),
                 fuzztest::InRange<size_t>(0, kMaxPagesPerHugePage),
                 fuzztest::InRange<size_t>(0, kMaxPagesPerHugePage),
                 fuzztest::InRange<size_t>(0, kMaxObjectsPerHugePage),
                 fuzztest::Arbitrary<bool>(), fuzztest::Arbitrary<bool>());

TEST(HugePageTreatmentTest, WorstCaseValidVmaNameFitsWithoutTruncation) {
  char buffer[kMaxAnonVmaNameSize];
  const size_t written = FormatSampledTrackerVmaName(
      absl::MakeSpan(buffer), MemoryTag::kSampledOrColdP1,
      /*page_size=*/262144, /*lfr=*/Length(kMaxPagesPerHugePage),
      /*nallocs=*/kMaxPagesPerHugePage, /*nobjects=*/kMaxObjectsPerHugePage,
      /*has_dense_spans=*/true, /*released=*/true);
  EXPECT_LT(written, kMaxAnonVmaNameSize);
  EXPECT_STREQ(
      buffer,
      "tcmalloc_region_SAMPLED_OR_COLD_P1_pg_262144_lfr_512_na_512_no_262144_"
      "d_1_r_1");

  const size_t zero_written = FormatSampledTrackerVmaName(
      absl::MakeSpan(buffer), MemoryTag::kNormal,
      /*page_size=*/8192, /*lfr=*/Length(0), /*nallocs=*/0,
      /*nobjects=*/0, /*has_dense_spans=*/false, /*released=*/false);
  EXPECT_LT(zero_written, kMaxAnonVmaNameSize);
  EXPECT_STREQ(buffer,
               "tcmalloc_region_NORMAL_pg_8192_lfr_0_na_0_no_0_d_0_r_0");
}

}  // namespace
}  // namespace tcmalloc::tcmalloc_internal
