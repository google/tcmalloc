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

#include "tcmalloc/common.h"

#include "tcmalloc/experiment.h"
#include "tcmalloc/runtime_size_classes.h"
#include "tcmalloc/sampler.h"

namespace tcmalloc {

// Load sizes classes from environment variable if present
// and valid, then returns True. If not found or valid, returns
// False.
bool SizeMap::MaybeRunTimeSizeClasses() {
  SizeClassInfo parsed[kNumClasses];
  int num_classes = MaybeSizeClassesFromEnv(kMaxSize, kNumClasses, parsed);
  if (!ValidSizeClasses(num_classes, parsed)) {
    return false;
  }

  if (num_classes != kNumClasses) {
    // TODO(b/122839049) - Add tests for num_classes < kNumClasses before
    // allowing that case.
    Log(kLog, __FILE__, __LINE__, "Can't change the number of size classes",
        num_classes, kNumClasses);
    return false;
  }

  SetSizeClasses(num_classes, parsed);
  Log(kLog, __FILE__, __LINE__, "Loaded valid Runtime Size classes");
  return true;
}

void SizeMap::SetSizeClasses(int num_classes, const SizeClassInfo* parsed) {
  class_to_size_[0] = 0;
  class_to_pages_[0] = 0;
  num_objects_to_move_[0] = 0;

  for (int c = 1; c < num_classes; c++) {
    class_to_size_[c] = parsed[c].size;
    class_to_pages_[c] = parsed[c].pages;
    num_objects_to_move_[c] = parsed[c].num_to_move;
  }

  // Fill any unspecified size classes with the largest size
  // from the static definitions.
  for (int x = num_classes; x < kNumClasses; x++) {
    class_to_size_[x] = kSizeClasses[kNumClasses - 1].size;
    class_to_pages_[x] = kSizeClasses[kNumClasses - 1].pages;
    auto num_to_move = kSizeClasses[kNumClasses - 1].num_to_move;
    if (IsExperimentActive(Experiment::TCMALLOC_LARGE_NUM_TO_MOVE)) {
      num_to_move = std::min(kMaxObjectsToMove, 4 * num_to_move);
    }
    num_objects_to_move_[x] = num_to_move;
  }
}

// Return true if all size classes meet the requirements for alignment
// ordering and min and max values.
bool SizeMap::ValidSizeClasses(int num_classes, const SizeClassInfo* parsed) {
  if (num_classes <= 0) {
    return false;
  }
  for (int c = 1; c < num_classes; c++) {
    size_t class_size = parsed[c].size;
    size_t pages = parsed[c].pages;
    size_t num_objects_to_move = parsed[c].num_to_move;
    // Each size class must be larger than the previous size class.
    if (class_size <= parsed[c - 1].size) {
      Log(kLog, __FILE__, __LINE__, "Non-increasing size class", c,
          parsed[c - 1].size, class_size);
      return false;
    }
    if (class_size > kMaxSize) {
      Log(kLog, __FILE__, __LINE__, "size class too big", c, class_size,
          kMaxSize);
      return false;
    }
    // Check required alignment
    size_t alignment = 128;
    if (class_size <= kMultiPageSize) {
      alignment = kAlignment;
    } else if (class_size <= SizeMap::kMaxSmallSize) {
      alignment = kMultiPageAlignment;
    }
    if ((class_size & (alignment - 1)) != 0) {
      Log(kLog, __FILE__, __LINE__, "Not aligned properly", c, class_size,
          alignment);
      return false;
    }
    if (class_size <= kMultiPageSize && pages != 1) {
      Log(kLog, __FILE__, __LINE__, "Multiple pages not allowed", class_size,
          pages, kMultiPageSize);
      return false;
    }
    if (pages >= 256) {
      Log(kLog, __FILE__, __LINE__, "pages limited to 255", pages);
      return false;
    }
    if (num_objects_to_move > kMaxObjectsToMove) {
      Log(kLog, __FILE__, __LINE__, "num objects to move too large",
          num_objects_to_move, kMaxObjectsToMove);
      return false;
    }
  }
  // Last size class must be able to hold kMaxSize.
  if (parsed[num_classes - 1].size < kMaxSize) {
    Log(kLog, __FILE__, __LINE__, "last class doesn't cover kMaxSize",
        num_classes - 1, parsed[num_classes - 1].size, kMaxSize);
    return false;
  }
  return true;
}

// Initialize the mapping arrays
void SizeMap::Init() {
  // Do some sanity checking on add_amount[]/shift_amount[]/class_array[]
  if (ClassIndex(0) != 0) {
    Log(kCrash, __FILE__, __LINE__,
        "Invalid class index for size 0", ClassIndex(0));
  }
  if (ClassIndex(kMaxSize) >= sizeof(class_array_)) {
    Log(kCrash, __FILE__, __LINE__,
        "Invalid class index for kMaxSize", ClassIndex(kMaxSize));
  }

  static_assert(kAlignment <= 16, "kAlignment is too large");

  if (IsExperimentActive(Experiment::TCMALLOC_SANS_56_SIZECLASS)) {
    SetSizeClasses(kNumClasses, kExperimentalSizeClasses);
  } else if (IsExperimentActive(Experiment::TCMALLOC_4K_SIZE_CLASS)) {
    SetSizeClasses(kNumClasses, kExperimental4kSizeClasses);
  } else {
    SetSizeClasses(kNumClasses, kSizeClasses);
  }
  MaybeRunTimeSizeClasses();

  int next_size = 0;
  for (int c = 1; c < kNumClasses; c++) {
    const int max_size_in_class = class_to_size_[c];

    for (int s = next_size; s <= max_size_in_class; s += kAlignment) {
      class_array_[ClassIndex(s)] = c;
    }
    next_size = max_size_in_class + kAlignment;
    if (next_size > kMaxSize) {
      break;
    }
  }
}

}  // namespace tcmalloc
