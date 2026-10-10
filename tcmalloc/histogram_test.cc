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

#include "tcmalloc/histogram.h"

#include <cstdint>
#include <string>

#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "absl/time/time.h"
#include "tcmalloc/internal/logging.h"
#include "tcmalloc/pages.h"
#include "tcmalloc/testing/testutil.h"

namespace tcmalloc {
namespace tcmalloc_internal {
namespace {

TEST(HistogramTest, PageHistogramZeroOffset) {
  PageHistogram<uint32_t, /*kOffset=*/0> hist;
  hist.Record(0);
  hist.Record(8);
  hist.Record(9);
  hist.Record(kPagesPerHugePage.raw_num() - 1);

  EXPECT_EQ(hist.count(0), 1);
  EXPECT_EQ(hist.count(8), 2);
  EXPECT_EQ(hist.count(hist.size() - 1), 1);

  std::string text = PrintToString(
      4096, [&](Printer& out) { hist.Print(out, "pages in test"); });
  EXPECT_THAT(text, testing::HasSubstr("HugePageFiller: # of pages in test"));
  EXPECT_THAT(text, testing::HasSubstr("<  0<=     1"));
  EXPECT_THAT(text, testing::HasSubstr("<  8<=     2"));

  std::string prefixed_text = PrintToString(
      4096, [&](Printer& out) { hist.Print(out, "sparse", "pages in test"); });
  EXPECT_THAT(prefixed_text,
              testing::HasSubstr("HugePageFiller: # of sparse pages in test"));

  std::string pbtxt = PrintToString(4096, [&](PbtxtRegion& region) {
    hist.PrintInPbtxt(region, "page_histo");
  });
  EXPECT_THAT(pbtxt,
              testing::HasSubstr(
                  "page_histo { lower_bound: 0 upper_bound: 0 value: 1}"));
  if (kPagesPerHugePage == Length(256)) {
    EXPECT_THAT(pbtxt,
                testing::HasSubstr(
                    "page_histo { lower_bound: 8 upper_bound: 15 value: 2}"));
    EXPECT_THAT(
        pbtxt, testing::HasSubstr(
                   "page_histo { lower_bound: 255 upper_bound: 255 value: 1}"));
  }
}

TEST(HistogramTest, PageHistogramOneOffset) {
  PageHistogram<uint64_t, /*kOffset=*/1> hist;
  hist.Record(1);
  hist.Record(9);
  hist.Record(16);
  hist.Record(kPagesPerHugePage.raw_num());

  EXPECT_EQ(hist.count(0), 1);
  EXPECT_EQ(hist.count(8), 2);
  EXPECT_EQ(hist.count(hist.size() - 1), 1);

  std::string pbtxt = PrintToString(4096, [&](PbtxtRegion& region) {
    hist.PrintInPbtxt(region, "one_based_histo");
  });
  EXPECT_THAT(pbtxt,
              testing::HasSubstr(
                  "one_based_histo { lower_bound: 1 upper_bound: 1 value: 1}"));
  if (kPagesPerHugePage == Length(256)) {
    EXPECT_THAT(
        pbtxt,
        testing::HasSubstr(
            "one_based_histo { lower_bound: 9 upper_bound: 16 value: 2}"));
    EXPECT_THAT(
        pbtxt,
        testing::HasSubstr(
            "one_based_histo { lower_bound: 256 upper_bound: 256 value: 1}"));
  }
}

TEST(HistogramTest, HardwareAndLifetimeHistograms) {
  HardwarePageHistogram hw_hist;
  hw_hist.Record(HardwareLength(0));
  hw_hist.Record(HardwareLength(8));
  EXPECT_EQ(hw_hist.count(0), 1);
  EXPECT_EQ(hw_hist.count(8), 1);

  std::string hw_text = PrintToString(4096, [&](Printer& out) {
    hw_hist.Print(out, "sparse", "unbacked pages");
  });
  EXPECT_THAT(hw_text,
              testing::HasSubstr("HugePageFiller: # of sparse unbacked pages"));
  EXPECT_THAT(hw_text, testing::HasSubstr("<  0<=     1"));

  std::string hw_pbtxt = PrintToString(4096, [&](PbtxtRegion& region) {
    hw_hist.PrintInPbtxt(region, "unbacked_histo");
  });
  EXPECT_THAT(hw_pbtxt,
              testing::HasSubstr(
                  "unbacked_histo { lower_bound: 0 upper_bound: 0 value: 1}"));

  LifetimeHistogram lifetime_hist;
  lifetime_hist.Record(absl::ZeroDuration());
  lifetime_hist.Record(absl::Milliseconds(15));
  lifetime_hist.Record(absl::Hours(10));
  EXPECT_EQ(lifetime_hist.count(0), 1);
  EXPECT_EQ(lifetime_hist.count(2), 1);
  EXPECT_EQ(lifetime_hist.count(LifetimeHistogram::kBuckets - 1), 1);

  std::string lifetime_text = PrintToString(4096, [&](Printer& out) {
    lifetime_hist.Print(out, "dense", "lifetime");
  });
  EXPECT_THAT(lifetime_text,
              testing::HasSubstr("HugePageFiller: # of dense lifetime"));
  EXPECT_THAT(lifetime_text, testing::HasSubstr("<   0 ms <=      1"));

  std::string lifetime_pbtxt = PrintToString(4096, [&](PbtxtRegion& region) {
    lifetime_hist.PrintInPbtxt(region, "lifetime_histo");
  });
  EXPECT_THAT(lifetime_pbtxt,
              testing::HasSubstr(
                  "lifetime_histo { lower_bound: 0 upper_bound: 1 value: 1}"));
  EXPECT_THAT(lifetime_pbtxt,
              testing::HasSubstr("lifetime_histo { lower_bound: 1000000 "
                                 "upper_bound: 1000000 value: 1}"));
}

}  // namespace
}  // namespace tcmalloc_internal
}  // namespace tcmalloc
