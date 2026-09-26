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

#ifndef TCMALLOC_STATIC_FORWARDER_H_
#define TCMALLOC_STATIC_FORWARDER_H_

#include <stddef.h>

#include <new>

#include "absl/base/nullability.h"
#include "tcmalloc/arena.h"
#include "tcmalloc/internal/config.h"

GOOGLE_MALLOC_SECTION_BEGIN
namespace tcmalloc {
namespace tcmalloc_internal {

class Static;
extern Static tc_globals;

// StaticForwarder provides the caches and page allocator with access to the
// global allocator state, `state`.
//
// This is a class, rather than namespaced globals, so that it can be mocked for
// testing.  It is templated on `state` so that its members can be defined
// inline here, despite Static containing the caches that use it:  member
// definitions are only instantiated where they are used, by which point Static
// is complete.
template <typename State, State& state>
class StaticForwarder {
 public:
  static size_t class_to_size(int size_class) {
    return state.sizemap().class_to_size(size_class);
  }

  static size_t num_objects_to_move(int size_class) {
    return state.sizemap().num_objects_to_move(size_class);
  }

  // Allocates metadata from the arena, accounted to `tag`.  This does not take
  // pageheap_lock, so it may be called with pageheap_lock held.
  [[nodiscard]] static void* absl_nonnull Alloc(ArenaAlloc tag, size_t size,
                                                std::align_val_t alignment) {
    return state.arena().Alloc(tag, size, alignment);
  }
};

}  // namespace tcmalloc_internal
}  // namespace tcmalloc
GOOGLE_MALLOC_SECTION_END

#endif  // TCMALLOC_STATIC_FORWARDER_H_
