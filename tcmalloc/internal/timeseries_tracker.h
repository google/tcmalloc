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
#ifndef TCMALLOC_TIMESERIES_TRACKER_H_
#define TCMALLOC_TIMESERIES_TRACKER_H_

#include <stddef.h>
#include <stdint.h>

#include <algorithm>
#include <limits>

#include "absl/functional/function_ref.h"
#include "absl/time/time.h"
#include "tcmalloc/internal/logging.h"

namespace tcmalloc {

// Assumed to tick in nanoseconds.
typedef int64_t (*ClockFunc)();

// Aggregates a series of reported values of type S in a set of entries of type
// T, one entry per epoch. This class factors out common functionality of
// different time series trackers. S can be any type, T needs to implement:
// Nil(), Report(S val), empty()
template <typename T, typename S, size_t kEpochs = 16>
class TimeSeriesTracker {
 public:
  enum SkipEntriesSetting { kSkipEmptyEntries, kDoNotSkipEmptyEntries };

  explicit constexpr TimeSeriesTracker(ClockFunc clock, absl::Duration w)
      : window_(w), epoch_length_(window_ / kEpochs), clock_(clock) {}

  void Report(S val);

  // Iterates over the time series, starting from the oldest entry. The callback
  // receives the offset of the entry, its timestamp according to the clock and
  // the entry itself. Offsets are relative to the beginning of the buffer.
  void Iter(absl::FunctionRef<void(size_t, int64_t, const T&)> f,
            SkipEntriesSetting skip_entries) const;

  // Iterates over the last num_epochs data points (if -1, iterate to the
  // oldest entry). Offsets are relative to the end of the buffer.
  void IterBackwards(absl::FunctionRef<void(size_t, int64_t, const T&)> f,
                     size_t num_epochs = -1) const;

  // Updates the time base to the current time. This is useful to report the
  // most recent time window rather than the last time window that had any
  // reported values.
  void UpdateTimeBase();

 private:
  // Returns true if the tracker moved to a different epoch.
  bool UpdateClock();

  const absl::Duration window_;
  const absl::Duration epoch_length_;

  T entries_[kEpochs]{};
  size_t last_epoch_{0};
  size_t current_epoch_{0};
  ClockFunc clock_;
};

// Erases values from the window that are out of date; sets the current epoch
// to the current location in the ringbuffer.
template <class T, class S, size_t kEpochs>
bool TimeSeriesTracker<T, S, kEpochs>::UpdateClock() {
  const size_t epoch = clock_() / ToInt64Nanoseconds(epoch_length_);
  // How many time steps did we take?  (Since we only record kEpochs
  // time steps, we can pretend it was at most that.)
  size_t delta = epoch - last_epoch_;
  delta = std::min(delta, kEpochs);
  last_epoch_ = epoch;

  if (delta == 0) {
    return false;
  }

  // At each tick, we move our current location by one, to a new location
  // that contains too-old data (which must be zeroed.)
  for (size_t offset = 0; offset < delta; ++offset) {
    current_epoch_++;
    if (current_epoch_ == kEpochs) current_epoch_ = 0;
    entries_[current_epoch_] = T::Nil();
  }
  return true;
}

template <class T, class S, size_t kEpochs>
void TimeSeriesTracker<T, S, kEpochs>::UpdateTimeBase() {
  UpdateClock();
}

template <class T, class S, size_t kEpochs>
void TimeSeriesTracker<T, S, kEpochs>::Iter(
    absl::FunctionRef<void(size_t, int64_t, const T&)> f,
    SkipEntriesSetting skip_entries) const {
  size_t j = current_epoch_ + 1;
  if (j == kEpochs) j = 0;
  int64_t timestamp = (last_epoch_ - kEpochs) * ToInt64Nanoseconds(epoch_length_);
  for (int offset = 0; offset < kEpochs; offset++) {
    timestamp += ToInt64Nanoseconds(epoch_length_);
    if (skip_entries == kDoNotSkipEmptyEntries || !entries_[j].empty()) {
      f(offset, timestamp, entries_[j]);
    }
    j++;
    if (j == kEpochs) j = 0;
  }
}

template <class T, class S, size_t kEpochs>
void TimeSeriesTracker<T, S, kEpochs>::IterBackwards(
    absl::FunctionRef<void(size_t, int64_t, const T&)> f,
    size_t num_epochs) const {
  // -1 means that we are outputting all epochs.
  num_epochs = (num_epochs == -1) ? kEpochs : num_epochs;
  size_t j = current_epoch_;
  ASSERT(num_epochs <= kEpochs);
  int64_t timestamp = last_epoch_ * ToInt64Nanoseconds(epoch_length_);
  for (size_t offset = 0; offset < num_epochs; ++offset) {
    // This is deliberately int64_t and not a time unit, since clock_ is not
    // guaranteed to be a real time base.
    f(offset, timestamp, entries_[j]);
    timestamp -= ToInt64Nanoseconds(epoch_length_);
    if (j == 0) j = kEpochs;
    --j;
  }
}

template <class T, class S, size_t kEpochs>
void TimeSeriesTracker<T, S, kEpochs>::Report(S val) {
  UpdateClock();
  entries_[current_epoch_].Report(val);
}

}  // namespace tcmalloc

#endif  // TCMALLOC_TIMESERIES_TRACKER_H_
