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

#ifndef TCMALLOC_INTERNAL_BITS_H_
#define TCMALLOC_INTERNAL_BITS_H_

#include "tcmalloc/internal/logging.h"

namespace tcmalloc {
namespace tcmalloc_internal {

class Bits {
 public:
  static constexpr int Log2Floor(uint32_t n) {
#if defined(__GNUC__)
    return n == 0 ? -1 : 31 ^ __builtin_clz(n);
#else
    if (n == 0) return -1;
    int log = 0;
    uint32_t value = n;
    for (int i = 4; i >= 0; --i) {
      int shift = (1 << i);
      uint32_t x = value >> shift;
      if (x != 0) {
        value = x;
        log += shift;
      }
    }
    ASSERT(value == 1);
    return log;
#endif
  }

  static constexpr int Log2Ceiling(uint32_t n) {
    int floor = Log2Floor(n);
    if ((n & (n - 1)) == 0)  // zero or a power of two
      return floor;
    else
      return floor + 1;
  }
};

}  // namespace tcmalloc_internal
}  // namespace tcmalloc

#endif  // TCMALLOC_INTERNAL_BITS_H_
