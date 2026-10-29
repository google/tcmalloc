// Copyright 2019 The TCMalloc Authors
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

#ifndef TCMALLOC_INTERNAL_MIN_MAX_TRACKER_H_
#define TCMALLOC_INTERNAL_MIN_MAX_TRACKER_H_

#include <stddef.h>
#include <stdint.h>

#include <algorithm>

#include "absl/strings/string_view.h"
#include "absl/time/time.h"
#include "tcmalloc/internal/clock.h"
#include "tcmalloc/internal/config.h"
#include "tcmalloc/internal/logging.h"
#include "tcmalloc/internal/timeseries_tracker.h"

GOOGLE_MALLOC_SECTION_BEGIN
namespace tcmalloc {
namespace tcmalloc_internal {

// Track the extreme values of a value of type T over the past
// kWindow (time ranges approximate.)
template <typename T, size_t kSlots = 16>
class MinMaxTracker {
 public:
  explicit constexpr MinMaxTracker(Clock clock, absl::Duration w)
      : kEpochLength(w / kSlots), timeseries_(clock, kEpochLength) {}

  void Report(T val);
  void Print(Printer& out, absl::string_view prefix,
             absl::string_view label) const;
  void PrintInPbtxt(PbtxtRegion& hpaa, absl::string_view region_name) const;

  // If t < kEpochLength, these functions return statistics for last epoch. The
  // granularity is kEpochLength (rounded up).
  [[nodiscard]] T MaxOverTime(absl::Duration t) const;
  [[nodiscard]] T MinOverTime(absl::Duration t) const;

 private:
  const absl::Duration kEpochLength;

  static constexpr T kMaxVal = T::max();
  struct Extrema {
    T min, max;

    [[nodiscard]] static Extrema Nil() {
      Extrema e;
      e.max = T::min();
      e.min = kMaxVal;
      return e;
    }

    void Report(T n) {
      max = std::max(max, n);
      min = std::min(min, n);
    }

    [[nodiscard]] bool empty() const { return (*this == Nil()); }

    [[nodiscard]] bool operator==(const Extrema& other) const;
  };

  TimeSeriesTracker<Extrema, T, kSlots> timeseries_;
};

template <typename T, size_t kSlots>
void MinMaxTracker<T, kSlots>::Report(T val) {
  timeseries_.Report(val);
}

template <typename T, size_t kSlots>
T MinMaxTracker<T, kSlots>::MaxOverTime(absl::Duration t) const {
  T m = T::min();
  timeseries_.IterBackwards([&](size_t offset, size_t epoch_delta,
                                const Extrema& e) { m = std::max(m, e.max); },
                            t);
  return m;
}

template <typename T, size_t kSlots>
T MinMaxTracker<T, kSlots>::MinOverTime(absl::Duration t) const {
  T m = kMaxVal;
  timeseries_.IterBackwards([&](size_t offset, size_t epoch_delta,
                                const Extrema& e) { m = std::min(m, e.min); },
                            t);
  if (m == kMaxVal) m = T::min();
  return m;
}

template <typename T, size_t kSlots>
void MinMaxTracker<T, kSlots>::Print(Printer& out, absl::string_view prefix,
                                     absl::string_view label) const {
  // Prints record_number:time_offset_ms:min_pages:max_pages for each records,
  // from the earliest to the latest.
  const int64_t millis = absl::ToInt64Milliseconds(kEpochLength);
  out.printf("\n%s: epoch length %lldms, number of records %zu", prefix, millis,
             kSlots);
  int written = 0;
  int64_t accumulated_delta_ms = 0;
  timeseries_.Iter([&](size_t offset, size_t epoch_delta, const Extrema& e) {
    if ((written++) % 100 == 0) {
      out.printf("\n%s: %s timeseries ", prefix, label);
    }
    accumulated_delta_ms +=
        absl::ToInt64Milliseconds(epoch_delta * kEpochLength);
    out.printf("%zu:%zu:%v:%v,", offset, accumulated_delta_ms, e.min, e.max);
  });
  out.printf("\n");
}

template <typename T, size_t kSlots>
void MinMaxTracker<T, kSlots>::PrintInPbtxt(
    PbtxtRegion& hpaa, absl::string_view region_name) const {
  // Prints content of each non-empty epoch, from oldest to most recent data
  auto history = hpaa.CreateSubRegion(region_name);
  history.PrintI64("window_ms", absl::ToInt64Milliseconds(kEpochLength));
  history.PrintI64("epochs", kSlots);
  int64_t accumulated_delta_ms = 0;
  timeseries_.Iter([&](size_t offset, size_t epoch_delta, const Extrema& e) {
    accumulated_delta_ms +=
        absl::ToInt64Milliseconds(epoch_delta * kEpochLength);
    auto m = history.CreateSubRegion("measurements");
    m.PrintI64("epoch", offset);
    m.PrintI64("time_offset_ms", accumulated_delta_ms);
    m.PrintI64("min_bytes", e.min.in_bytes());
    m.PrintI64("max_bytes", e.max.in_bytes());
  });
}

template <typename T, size_t kSlots>
bool MinMaxTracker<T, kSlots>::Extrema::operator==(const Extrema& other) const {
  return (other.max == max) && (other.min == min);
}

}  // namespace tcmalloc_internal
}  // namespace tcmalloc
GOOGLE_MALLOC_SECTION_END

#endif  // TCMALLOC_INTERNAL_MIN_MAX_TRACKER_H_
