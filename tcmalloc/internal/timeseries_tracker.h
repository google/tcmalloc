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
#ifndef TCMALLOC_INTERNAL_TIMESERIES_TRACKER_H_
#define TCMALLOC_INTERNAL_TIMESERIES_TRACKER_H_

#include <stddef.h>
#include <stdint.h>

#include <algorithm>
#include <cmath>

#include "absl/base/optimization.h"
#include "absl/functional/function_ref.h"
#include "absl/numeric/bits.h"
#include "absl/numeric/int128.h"
#include "absl/time/time.h"
#include "tcmalloc/internal/clock.h"
#include "tcmalloc/internal/logging.h"

GOOGLE_MALLOC_SECTION_BEGIN
namespace tcmalloc {
namespace tcmalloc_internal {

// Aggregates a series of reported values of type S in a set of entries of type
// T in a kSlots-array. This class factors out common functionality of
// different time series trackers. S can be any type, T needs to implement:
// Nil(), Report(S val), empty(). The series has kSlots and each slot contains
// value aggregated during an epoch_length_ via T.Report(S val). When the clock
// advances, the series always advances current_slot_ by one no matter how much
// time has passed (e.g., either 1 epoch or 10 epochs were passed). The tracker
// achieves this by logging the amount of time (in epoch) had passed since the
// previous record was taken. As a result, it can hold data reported in
// longer than K-epochs period (i.e, time length >= kSlots * epoch_length).
template <typename T, typename S, size_t kSlots = 16>
class TimeSeriesTracker {
 public:
  struct RecordView {
    size_t epoch_taken;  // When the record was taken, relative to now.
    T data;
  };

  explicit constexpr TimeSeriesTracker(Clock clock, absl::Duration epoch_length)
      : epoch_length_(epoch_length), clock_(clock) {
    TC_ASSERT_GT(epoch_length, absl::ZeroDuration());
    // See comment in GetCurrentEpoch().
    auto d = static_cast<uint64_t>(absl::ToDoubleSeconds(epoch_length_) *
                                   clock.freq());
    div_precision_ = 63 + absl::bit_width(d);
    epoch_ticks_m_ =
        static_cast<uint64_t>(
            (static_cast<absl::uint128>(1) << div_precision_) / d) +
        1;
    InitTracker();
  }

  bool Report(const S& val);

  // Iterates over the time series, starting from the oldest entry. The callback
  // receives the sequence number of the entry, the epoch_delta (i.e., number of
  // epochs passed since the previous entry), and the entry itself. The sequence
  // number ranges between [0, kSlots), and is relative to the beginning of the
  // buffer.
  void Iter(absl::FunctionRef<void(size_t, size_t, const T&)> f) const;

  // Iterates backwards over the data points recorded in the window.
  // It iterates all entries if w is infinite. The sequence number is relative
  // to the end of the buffer.
  void IterBackwards(absl::FunctionRef<void(size_t, size_t, const T&)> f,
                     absl::Duration interval = absl::InfiniteDuration()) const;

  // Retrieves the most recent record and when the record was taken (relative to
  // now). For example, epoch_taken = 1 means it was taken in the last epoch,
  // and epoch_taken = 5 means that it was taken at 5 * epoch_length ago. The
  // default record value will be returned if there is no valid record.
  struct RecordView GetMostRecentRecord() const;

  // Updates the time base to the current time. This is useful to report the
  // most recent time window rather than the last time window that had any
  // reported values.
  void UpdateTimeBase() { UpdateClock(); }

 private:
  // Returns true if the tracker moved to a different epoch.
  bool UpdateClock();

  // Returns the current epoch number based on the clock.
  int64_t GetCurrentEpoch() {
    // This is equivalent to
    // `clock_.now() / (absl::ToDoubleSeconds(epoch_length_) * clock_.freq())`.
    // We basically follow the technique from
    // https://ridiculousfish.com/blog/posts/labor-of-division-episode-i.html,
    // except that we use one fewer bit of precision than necessary to always
    // get the correct answer if the numerator were a 64-bit unsigned number. In
    // this case, because clock_.now() returns a signed 64-bit number (i.e. max
    // is <2^63), it shouldn't cause a problem. This way, we don't need to
    // handle overflow so it's simpler. See also:
    // https://lemire.me/blog/2019/02/20/more-fun-with-fast-remainders-when-the-divisor-is-a-constant/.
    return static_cast<int64_t>(static_cast<absl::uint128>(epoch_ticks_m_) *
                                    clock_.now() >>
                                div_precision_);
  }
  void InitTracker() {
    // Inits the tracker by "create" an record for "now" on slot 0. The record
    // serves as the first valid record in the tracker, with epoch coverage
    // (delta) 1 and an empty payload. In this way, we would know when was the
    // first real data point taken, as it would be the time diff between "now"
    // and "then". If the first data point is taken immediately, this record
    // will be updated with real data.
    size_t delta = 1;
    last_epoch_ = GetCurrentEpoch();
    entries_[current_slot_] = TimeSeriesContent(delta);
    covered_epochs_ = delta;
  }

  struct TimeSeriesContent {
    size_t epoch_delta;  // Time gap (in epoch) between this slot and
                         // the previous slot.
    T payload;           // Aggregated data.

    TimeSeriesContent() : epoch_delta(0), payload(T::Nil()) {}
    explicit TimeSeriesContent(size_t delta)
        : epoch_delta(delta), payload(T::Nil()) {}
  };

  const absl::Duration epoch_length_;

  TimeSeriesContent entries_[kSlots]{};
  size_t last_epoch_{0};
  size_t current_slot_{0};
  // How many epochs did it take to collect all valid data points?
  size_t covered_epochs_{0};
  // This is the magic constant from
  // https://ridiculousfish.com/blog/posts/labor-of-division-episode-i.html.
  uint64_t epoch_ticks_m_;
  uint8_t div_precision_;

  Clock clock_;
};

// Advances the current slot if the clock had advanced >= 1 epoch; sets the
// epoch_delta for how many epoch had passed sice the previous clock update.
template <class T, class S, size_t kSlots>
bool TimeSeriesTracker<T, S, kSlots>::UpdateClock() {
  const size_t epoch = GetCurrentEpoch();
  // How much time had passed?
  size_t delta = epoch - last_epoch_;
  if (delta == 0) {
    return false;
  }
  // Epoch value is very unlikely overflow if uses real clocks but can be in
  // tests (using fake clocks).
  TC_ASSERT_LT(last_epoch_, epoch);
  last_epoch_ = epoch;
  // We update the time series in a constant manner: advances the current epoch
  // by 1 and records the delta as how many epochs have passed.
  current_slot_++;
  if (current_slot_ == kSlots) current_slot_ = 0;
  // Before erasing the data, removes its "covered period" from the tracker.
  TC_ASSERT_GE(covered_epochs_, entries_[current_slot_].epoch_delta);
  covered_epochs_ -= entries_[current_slot_].epoch_delta;
  entries_[current_slot_] = TimeSeriesContent(delta);
  covered_epochs_ += delta;
  return true;
}

template <class T, class S, size_t kSlots>
void TimeSeriesTracker<T, S, kSlots>::Iter(
    absl::FunctionRef<void(size_t, size_t, const T&)> f) const {
  size_t j = current_slot_ + 1;
  if (j == kSlots) j = 0;
  for (int sequenc_num = 0; sequenc_num < kSlots; sequenc_num++) {
    // We would have no empty entries between valid data points hence there is
    // no reason to perform action on them. We think the slot would be all
    // filled shortly after the job started.
    if (ABSL_PREDICT_TRUE(!entries_[j].payload.empty())) {
      f(sequenc_num, entries_[j].epoch_delta, entries_[j].payload);
    }
    j++;
    if (j == kSlots) j = 0;
  }
}

template <class T, class S, size_t kSlots>
void TimeSeriesTracker<T, S, kSlots>::IterBackwards(
    absl::FunctionRef<void(size_t, size_t, const T&)> f,
    absl::Duration interval) const {
  if (interval == absl::ZeroDuration()) return;
  size_t epochs_to_traverse;
  if (interval == absl::InfiniteDuration()) {
    // InfiniteDuration() means that we are outputting all records.
    epochs_to_traverse = covered_epochs_;
  } else {
    epochs_to_traverse = static_cast<size_t>(
        std::min(ceil(absl::FDivDuration(interval, epoch_length_)),
                 static_cast<double>(covered_epochs_)));
  }
  // We would be returned already if nothing to traverse, plus the covered
  // epochs should always be higher than zero (see UpdateClock() for details).
  TC_ASSERT_GT(epochs_to_traverse, 0);
  size_t j = current_slot_, epochs_traversed = 0;
  // Traverses as many slots as needed to cover the epochs but stop earlier if
  // we have covered them all.
  size_t max_slots = std::min(kSlots, epochs_to_traverse);
  for (size_t sequence_num = 0; sequence_num < max_slots; ++sequence_num) {
    f(sequence_num, entries_[j].epoch_delta, entries_[j].payload);
    // How many epochs that we have covered so far?
    epochs_traversed += entries_[j].epoch_delta;
    if (epochs_traversed >= epochs_to_traverse) break;
    if (j == 0) j = kSlots;
    --j;
  }
}

template <class T, class S, size_t kSlots>
typename TimeSeriesTracker<T, S, kSlots>::RecordView
TimeSeriesTracker<T, S, kSlots>::GetMostRecentRecord() const {
  // The most recent record stored in current_slot - 1.
  size_t slot_num = (current_slot_ + kSlots - 1) % kSlots;
  // "when the record was taken" is stored in current_slot_ as the delta
  // between "current" and the "most recent".
  return RecordView{.epoch_taken = entries_[current_slot_].epoch_delta,
                    .data = entries_[slot_num].payload};
}

template <class T, class S, size_t kSlots>
bool TimeSeriesTracker<T, S, kSlots>::Report(const S& val) {
  bool updated_clock = UpdateClock();
  entries_[current_slot_].payload.Report(val);
  return updated_clock;
}

}  // namespace tcmalloc_internal
}  // namespace tcmalloc
GOOGLE_MALLOC_SECTION_END

#endif  // TCMALLOC_INTERNAL_TIMESERIES_TRACKER_H_
