// Copyright 2022 The TCMalloc Authors
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

#ifndef TCMALLOC_INTERNAL_STACKTRACE_FILTER_H_
#define TCMALLOC_INTERNAL_STACKTRACE_FILTER_H_

#include <atomic>

#include "absl/hash/hash.h"
#include "absl/types/span.h"
#include "tcmalloc/internal/logging.h"

namespace tcmalloc {
namespace tcmalloc_internal {

// This class maintains a small collection of StackTrace hashes which are used
// to inform the selection of allocations to be guarded. It provides two
// functions:
//    - Evaluate: returns a double with a value between 0.0 and 1.0.
//      0.0 indicates no guards have been placed on this StackTrace.
//      1.0 indicates this StackTrace is guarded more than any others.
//      The closer the value is to 0.0, then less frequently the StackTrace has
//      been guarded.
//    - Add: which adds the provided StackTrace to the filter, for use when
//      responding to subsequent Evaluate calls.
// Based on the collection size (kSize), it uses the lower bits (kMask) of the
// StackTrace hash as an index into stack_hashes_with_count_.  It stores a count
// of the number of times a hash has been 'Add'-ed in the lower bits (kMask).
//
// To enhance selecting less frequent StackTrace hashes (stack traces), the most
// frequently encountered StackTrace hash count is recorded as a high-water
// mark. It is used within Evaluate to discourage guarding this high frequency
// StackTrace hash.
class StackTraceFilter {
 public:
  constexpr StackTraceFilter() = default;

  double Evaluate(const StackTrace& stacktrace) const;
  void Add(const StackTrace& stacktrace);

 private:
  constexpr static size_t kMask = 0xFF;
  constexpr static size_t kHashCountLimit = kMask;
  constexpr static int kSize = kMask + 1;
  std::atomic<size_t> stack_hashes_with_count_[kSize]{0};
  std::atomic<size_t> most_frequent_hash_count_{1};

  inline size_t HashOfStackTrace(const StackTrace& stacktrace) const {
    return absl::HashOf(
        absl::Span<void* const>(stacktrace.stack, stacktrace.depth));
  }

  friend class StackTraceFilterTest;
  friend class StackTraceFilterThreadedTest;
};

inline double StackTraceFilter::Evaluate(const StackTrace& stacktrace) const {
  size_t stack_hash = HashOfStackTrace(stacktrace);
  size_t existing_stack_hash_with_count =
      stack_hashes_with_count_[stack_hash % kSize].load(
          std::memory_order_relaxed);
  //  New stack trace
  if (existing_stack_hash_with_count == 0) {
    return 0.0;
  }
  // Different stack trace, treat as new
  if ((stack_hash & ~kMask) != (existing_stack_hash_with_count & ~kMask)) {
    return 0.0;
  }
  // Return a value based on the count of the most frequently guarded stack.
  size_t most_frequent_hash_count =
      most_frequent_hash_count_.load(std::memory_order_relaxed);
  ASSERT(most_frequent_hash_count > 0);
  return static_cast<double>(existing_stack_hash_with_count & kMask) /
         most_frequent_hash_count;
}

inline void StackTraceFilter::Add(const StackTrace& stacktrace) {
  size_t stack_hash = HashOfStackTrace(stacktrace);
  size_t existing_stack_hash_with_count =
      stack_hashes_with_count_[stack_hash % kSize].load(
          std::memory_order_relaxed);
  size_t count = 1;
  if ((existing_stack_hash_with_count & ~kMask) == (stack_hash & ~kMask)) {
    // matching entry, increment count
    count = (existing_stack_hash_with_count & kMask) + 1;
    // max count has been reached, skip storing incremented count
    if (count > kHashCountLimit) {
      return;
    }
    stack_hashes_with_count_[stack_hash % kSize].store(
        (stack_hash & ~kMask) | count, std::memory_order_relaxed);
    // if more frequent, then raise limit
    if (count > most_frequent_hash_count_.load(std::memory_order_relaxed)) {
      most_frequent_hash_count_.store(count, std::memory_order_relaxed);
    }
  } else {
    // New stack_hash being placed in (unoccupied entry || existing entry)
    stack_hashes_with_count_[stack_hash % kSize].store(
        (stack_hash & ~kMask) | 1, std::memory_order_relaxed);
    // For the case of replacing an existing entry, if it was the most
    // frequently encountered stack_hash, find the remaining
    // most frequent stack_hash count and store it.
    if ((existing_stack_hash_with_count & kMask) >=
        most_frequent_hash_count_.load(std::memory_order_relaxed)) {
      // Search for highest value and use that.
      size_t new_most_frequent_hash_count = 1;
      std::atomic<size_t>* stack_hashes_with_count_ptr =
          stack_hashes_with_count_;
      for (int index = 0; index < kSize;
           ++index, ++stack_hashes_with_count_ptr) {
        count = stack_hashes_with_count_ptr->load(std::memory_order_relaxed) &
                kMask;
        if (count > new_most_frequent_hash_count) {
          new_most_frequent_hash_count = count;
        }
      }
      most_frequent_hash_count_.store(new_most_frequent_hash_count,
                                      std::memory_order_relaxed);
    }
  }
}

}  // namespace tcmalloc_internal
}  // namespace tcmalloc

#endif  // TCMALLOC_INTERNAL_STACKTRACE_FILTER_H_
