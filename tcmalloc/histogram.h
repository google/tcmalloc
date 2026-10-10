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

#ifndef TCMALLOC_HISTOGRAM_H_
#define TCMALLOC_HISTOGRAM_H_

#include <algorithm>
#include <cstddef>
#include <cstdint>

#include "absl/strings/str_format.h"
#include "absl/strings/string_view.h"
#include "absl/time/time.h"
#include "tcmalloc/huge_pages.h"
#include "tcmalloc/internal/config.h"
#include "tcmalloc/internal/hardware_pages.h"
#include "tcmalloc/internal/logging.h"
#include "tcmalloc/internal/page_size.h"
#include "tcmalloc/internal/system_allocator.h"
#include "tcmalloc/pages.h"

GOOGLE_MALLOC_SECTION_BEGIN
namespace tcmalloc {
namespace tcmalloc_internal {

// Computes bucket bounds for page-count histograms. Because nearly empty/full
// huge pages are much more interesting, we allocate step-1 buckets at each end
// and divide the middle into kBucketsInBetween buckets.
struct HistogramBounds {
  static constexpr size_t kBucketsAtBounds = 8;
  static constexpr size_t kBucketsInBetween = 16;
  static constexpr size_t kBucketCapacity =
      kBucketsAtBounds + kBucketsInBetween + kBucketsAtBounds;

  size_t bounds[kBucketCapacity] = {};
  size_t size = 0;

  [[nodiscard]] size_t BucketNum(size_t value) const {
    const size_t* it = std::upper_bound(bounds, bounds + size, value);
    TC_CHECK_NE(it, bounds);
    return static_cast<size_t>(it - bounds - 1);
  }
};

[[nodiscard]] constexpr HistogramBounds MakePageHistogramBounds() {
  HistogramBounds result;
  size_t i = 0;
  for (i = 0; i <= HistogramBounds::kBucketsAtBounds &&
              i < kPagesPerHugePage.raw_num();
       ++i) {
    result.bounds[result.size++] = i;
  }
  static_assert(kPagesPerHugePage.raw_num() >=
                HistogramBounds::kBucketsAtBounds);
  if (i < kPagesPerHugePage.raw_num() - HistogramBounds::kBucketsAtBounds) {
    constexpr int step =
        kPagesPerHugePage.raw_num() / HistogramBounds::kBucketsInBetween;
    // Align middle buckets to a power-of-two step boundary.
    i = ((i - 1) | (step - 1)) + 1;
    for (; i < kPagesPerHugePage.raw_num() - HistogramBounds::kBucketsAtBounds;
         i += step) {
      result.bounds[result.size++] = i;
    }
    i = kPagesPerHugePage.raw_num() - HistogramBounds::kBucketsAtBounds;
  }
  for (; i < kPagesPerHugePage.raw_num(); ++i) {
    result.bounds[result.size++] = i;
  }
  return result;
}

[[nodiscard]] inline HistogramBounds MakeHardwarePageHistogramBounds() {
  HistogramBounds result;
  const size_t hardware_pages = kHugePageSize / GetPageSize();
  const size_t step = hardware_pages / HistogramBounds::kBucketsInBetween;
  TC_ASSERT_GE(hardware_pages, HistogramBounds::kBucketsAtBounds);
  for (size_t i = 0;
       i <= HistogramBounds::kBucketsAtBounds && result.size < hardware_pages;
       ++i) {
    result.bounds[result.size++] = i;
  }
  for (size_t i = 0; i < hardware_pages - HistogramBounds::kBucketsAtBounds;
       ++i) {
    const size_t bound = result.bounds[result.size - 1] + step;
    if (bound >= hardware_pages - HistogramBounds::kBucketsAtBounds) {
      break;
    }
    result.bounds[result.size++] = bound;
  }
  for (size_t i = 0; i < HistogramBounds::kBucketsAtBounds; ++i) {
    const size_t end_bound =
        hardware_pages - HistogramBounds::kBucketsAtBounds + i;
    if (result.bounds[result.size - 1] >= end_bound) {
      continue;
    }
    result.bounds[result.size++] = end_bound;
  }
  return result;
}

// Common storage and text / pbtxt formatting helpers shared by histogram types.
template <typename CountType, size_t kCapacity>
class HistogramBase {
 protected:
  constexpr HistogramBase() = default;

  void IncrementBucket(size_t bucket) {
    TC_ASSERT_LT(bucket, kCapacity);
    ++counts_[bucket];
  }

  [[nodiscard]] CountType GetCount(size_t bucket, size_t num_buckets) const {
    TC_ASSERT_LT(bucket, num_buckets);
    return counts_[bucket];
  }

  void PrintFormatted(Printer& out, absl::string_view blurb,
                      const absl::FormatSpec<size_t, size_t>& bucket_fmt,
                      const size_t* bounds, size_t num_buckets,
                      size_t offset) const {
    out.printf("\nHugePageFiller: # of %s", blurb);
    PrintBuckets(out, bucket_fmt, bounds, num_buckets, offset);
  }

  void PrintFormatted(Printer& out, absl::string_view prefix,
                      absl::string_view blurb,
                      const absl::FormatSpec<size_t, size_t>& bucket_fmt,
                      const size_t* bounds, size_t num_buckets,
                      size_t offset) const {
    out.printf("\nHugePageFiller: # of %s %s", prefix, blurb);
    PrintBuckets(out, bucket_fmt, bounds, num_buckets, offset);
  }

  void PrintBucketsInPbtxt(PbtxtRegion& region, absl::string_view key,
                           const size_t* bounds, size_t num_buckets,
                           size_t offset, size_t upper_bound_delta) const {
    for (size_t i = 0; i < num_buckets; ++i) {
      if (counts_[i] == 0) continue;
      PbtxtRegion hist = region.CreateSubRegion(key);
      hist.PrintI64("lower_bound", bounds[i] + offset);
      hist.PrintI64("upper_bound",
                    (i == num_buckets - 1 ? bounds[i]
                                          : bounds[i + 1] - upper_bound_delta) +
                        offset);
      hist.PrintI64("value", counts_[i]);
    }
  }

 private:
  void PrintBuckets(Printer& out,
                    const absl::FormatSpec<size_t, size_t>& bucket_fmt,
                    const size_t* bounds, size_t num_buckets,
                    size_t offset) const {
    for (size_t i = 0; i < num_buckets; ++i) {
      if (i % 6 == 0) {
        out.printf("\nHugePageFiller:");
      }
      out.printf(bucket_fmt, bounds[i] + offset,
                 static_cast<size_t>(counts_[i]));
    }
    out.printf("\n");
  }

  CountType counts_[kCapacity] = {};
};

template <typename CountType, size_t kOffset>
class PageHistogram
    : public HistogramBase<CountType, HistogramBounds::kBucketCapacity> {
 public:
  static constexpr HistogramBounds kBounds = MakePageHistogramBounds();
  static_assert(kBounds.size <= HistogramBounds::kBucketCapacity);

  constexpr PageHistogram() = default;

  void Record(size_t value) {
    TC_ASSERT_GE(value, kOffset);
    this->IncrementBucket(kBounds.BucketNum(value - kOffset));
  }

  [[nodiscard]] CountType count(size_t bucket) const {
    return this->GetCount(bucket, kBounds.size);
  }

  [[nodiscard]] size_t size() const { return kBounds.size; }

  void Print(Printer& out, absl::string_view blurb) const {
    this->PrintFormatted(out, blurb, " <%3zu<=%6zu", kBounds.bounds,
                         kBounds.size, kOffset);
  }

  void Print(Printer& out, absl::string_view prefix,
             absl::string_view blurb) const {
    this->PrintFormatted(out, prefix, blurb, " <%3zu<=%6zu", kBounds.bounds,
                         kBounds.size, kOffset);
  }

  void PrintInPbtxt(PbtxtRegion& region, absl::string_view key) const {
    this->PrintBucketsInPbtxt(region, key, kBounds.bounds, kBounds.size,
                              kOffset, /*upper_bound_delta=*/1);
  }
};

class HardwarePageHistogram
    : public HistogramBase<uint32_t, HistogramBounds::kBucketCapacity> {
 public:
  HardwarePageHistogram() : bounds_(MakeHardwarePageHistogramBounds()) {}

  void Record(HardwareLength pages) {
    this->IncrementBucket(bounds_.BucketNum(pages.raw_num()));
  }

  [[nodiscard]] uint32_t count(size_t bucket) const {
    return this->GetCount(bucket, bounds_.size);
  }

  void Print(Printer& out, absl::string_view prefix,
             absl::string_view blurb) const {
    this->PrintFormatted(out, prefix, blurb, " <%3zu<=%6zu", bounds_.bounds,
                         bounds_.size, /*offset=*/0);
  }

  void PrintInPbtxt(PbtxtRegion& region, absl::string_view key) const {
    this->PrintBucketsInPbtxt(region, key, bounds_.bounds, bounds_.size,
                              /*offset=*/0, /*upper_bound_delta=*/1);
  }

 private:
  HistogramBounds bounds_;
};

class LifetimeHistogram : public HistogramBase<uint32_t, 8> {
 public:
  static constexpr size_t kBuckets = 8;
  static constexpr size_t kBounds[kBuckets + 1] = {
      0, 1, 10, 100, 1000, 10000, 100000, 1000000, 10000000};

  constexpr LifetimeHistogram() = default;

  void Record(absl::Duration duration) {
    const int64_t duration_ms = absl::ToInt64Milliseconds(duration);
    const size_t clamped = static_cast<size_t>(std::clamp<int64_t>(
        duration_ms, 0, static_cast<int64_t>(kBounds[kBuckets - 1])));
    const size_t* it = std::upper_bound(kBounds, kBounds + kBuckets, clamped);
    TC_CHECK_NE(it, kBounds);
    this->IncrementBucket(static_cast<size_t>(it - kBounds - 1));
  }

  [[nodiscard]] uint32_t count(size_t bucket) const {
    return this->GetCount(bucket, kBuckets);
  }

  void Print(Printer& out, absl::string_view prefix,
             absl::string_view blurb) const {
    this->PrintFormatted(out, prefix, blurb, " < %3zu ms <= %6zu", kBounds,
                         kBuckets, /*offset=*/0);
  }

  void PrintInPbtxt(PbtxtRegion& region, absl::string_view key) const {
    this->PrintBucketsInPbtxt(region, key, kBounds, kBuckets, /*offset=*/0,
                              /*upper_bound_delta=*/0);
  }
};

}  // namespace tcmalloc_internal
}  // namespace tcmalloc
GOOGLE_MALLOC_SECTION_END

#endif  // TCMALLOC_HISTOGRAM_H_
