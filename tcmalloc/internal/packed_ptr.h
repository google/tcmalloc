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

#ifndef TCMALLOC_INTERNAL_PACKED_PTR_H_
#define TCMALLOC_INTERNAL_PACKED_PTR_H_

#include <cstddef>
#include <cstdint>

#include "tcmalloc/internal/config.h"
#include "tcmalloc/internal/logging.h"

GOOGLE_MALLOC_SECTION_BEGIN
namespace tcmalloc {
namespace tcmalloc_internal {

template <typename T, size_t kBits = 1>
class PackedPtr {
 public:
  static_assert(kBits > 0, "PackedPtr requires at least one bit.");
  static_assert(kBits < sizeof(uintptr_t) * 8,
                "PackedPtr requires fewer bits than uintptr_t width.");

  constexpr PackedPtr() : rep_(0) {}

  explicit PackedPtr(T* ptr) : rep_(reinterpret_cast<uintptr_t>(ptr)) {
    static_assert(
        alignof(T) >= (size_t{1} << kBits),
        "PackedPtr requires alignof(T) >= (1 << kBits) to store low bits.");
    TC_ASSERT_EQ(rep_ & kBitsMask, uintptr_t{0});
  }

  explicit PackedPtr(T* ptr, uintptr_t bits)
      : rep_(reinterpret_cast<uintptr_t>(ptr) | bits) {
    static_assert(
        alignof(T) >= (size_t{1} << kBits),
        "PackedPtr requires alignof(T) >= (1 << kBits) to store low bits.");
    TC_ASSERT_EQ(reinterpret_cast<uintptr_t>(ptr) & kBitsMask, uintptr_t{0});
    TC_ASSERT_EQ(bits & ~kBitsMask, uintptr_t{0});
  }

  explicit PackedPtr(void* raw) : rep_(reinterpret_cast<uintptr_t>(raw)) {
    static_assert(
        alignof(T) >= (size_t{1} << kBits),
        "PackedPtr requires alignof(T) >= (1 << kBits) to store low bits.");
  }

  [[nodiscard]] T* Get() const {
    return reinterpret_cast<T*>(rep_ & ~kBitsMask);
  }

  [[nodiscard]] uintptr_t Bits() const { return rep_ & kBitsMask; }

  [[nodiscard]] void* Raw() const { return reinterpret_cast<void*>(rep_); }

 private:
  static constexpr uintptr_t kBitsMask = (uintptr_t{1} << kBits) - 1;
  uintptr_t rep_;
};

}  // namespace tcmalloc_internal
}  // namespace tcmalloc
GOOGLE_MALLOC_SECTION_END

#endif  // TCMALLOC_INTERNAL_PACKED_PTR_H_
