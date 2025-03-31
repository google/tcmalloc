// Copyright 2025 The TCMalloc Authors
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

#include "tcmalloc/error_reporting.h"

#include <cstddef>
#include <cstdint>
#include <new>
#include <optional>

#include "absl/base/attributes.h"
#include "absl/base/casts.h"
#include "absl/debugging/stacktrace.h"
#include "absl/numeric/bits.h"
#include "absl/types/span.h"
#include "tcmalloc/internal/config.h"
#include "tcmalloc/internal/logging.h"
#include "tcmalloc/internal/sampled_allocation.h"
#include "tcmalloc/pagemap.h"
#include "tcmalloc/pages.h"
#include "tcmalloc/static_vars.h"

GOOGLE_MALLOC_SECTION_BEGIN
namespace tcmalloc::tcmalloc_internal {

[[noreturn]]
ABSL_ATTRIBUTE_NOINLINE void ReportMismatchedDelete(
    Static& state, const SampledAllocation& alloc, size_t size,
    size_t requested_size, std::optional<size_t> allocated_size) {
  TC_LOG("*** GWP-ASan (https://google.github.io/tcmalloc/gwp-asan.html) has detected a memory error ***");
  TC_LOG("Error originates from memory allocated at:");
  PrintStackTrace(alloc.sampled_stack.stack, alloc.sampled_stack.depth);

  size_t maximum_size;
  if (allocated_size.value_or(requested_size) != requested_size) {
    TC_LOG(
        "Mismatched-size-delete "
        "(https://github.com/google/tcmalloc/tree/master/docs/mismatched-sized-delete.md) "
        "of %v bytes (expected %v - %v bytes) at:",
        size, requested_size, *allocated_size);

    maximum_size = *allocated_size;
  } else {
    TC_LOG(
        "Mismatched-size-delete "
        "(https://github.com/google/tcmalloc/tree/master/docs/mismatched-sized-delete.md) "
        "of %v bytes (expected %v bytes) at:",
        size, requested_size);

    maximum_size = requested_size;
  }
  static void* stack[kMaxStackDepth];
  const size_t depth = absl::GetStackTrace(stack, kMaxStackDepth, 1);
  PrintStackTrace(stack, depth);

  RecordCrash("GWP-ASan", "mismatched-size-delete");
  state.gwp_asan_state().RecordMismatch(
      size, size, requested_size, maximum_size,
      absl::MakeSpan(alloc.sampled_stack.stack, alloc.sampled_stack.depth),
      absl::MakeSpan(stack, depth));
  abort();
}

[[noreturn]]
ABSL_ATTRIBUTE_NOINLINE void ReportMismatchedDelete(Static& state, void* ptr,
                                                    size_t size,
                                                    size_t minimum_size,
                                                    size_t maximum_size) {
  // Try to refine the maximum possible size.
  const PageId p = PageIdContainingTagged(ptr);
  size_t size_class = state.pagemap().sizeclass(p);
  if (size_class != 0) {
    maximum_size = state.sizemap().class_to_size(size_class);
    if (maximum_size < minimum_size) {
      // Our size class refinement may have made the bounds inconsistent.
      // Consult the size map to find the correct bounds.
      minimum_size = state.sizemap().class_to_size_range(size_class).first;
    }
  }

  TC_LOG("*** GWP-ASan (https://google.github.io/tcmalloc/gwp-asan.html) has detected a memory error ***");

  TC_LOG(
      "Mismatched-size-delete "
      "(https://github.com/google/tcmalloc/tree/master/docs/mismatched-sized-delete.md) "
      "of %v bytes (expected between [%v, %v] bytes) for %p at:",
      size, minimum_size, maximum_size, ptr);

  static void* stack[kMaxStackDepth];
  const size_t depth = absl::GetStackTrace(stack, kMaxStackDepth, 1);
  PrintStackTrace(stack, depth);

  RecordCrash("GWP-ASan", "mismatched-size-delete");
  state.gwp_asan_state().RecordMismatch(/*provided_min=*/size,
                                        /*provided_max=*/size, minimum_size,
                                        maximum_size, std::nullopt,
                                        absl::MakeSpan(stack, depth));
  abort();
}

[[noreturn]]
ABSL_ATTRIBUTE_NOINLINE void ReportMismatchedSizeClass(Static& state,
                                                       void* object,
                                                       int page_size_class,
                                                       int object_size_class) {
  auto [object_min_size, object_max_size] =
      state.sizemap().class_to_size_range(object_size_class);
  auto [page_min_size, page_max_size] =
      state.sizemap().class_to_size_range(page_size_class);

  TC_LOG("*** GWP-ASan (https://google.github.io/tcmalloc/gwp-asan.html) has detected a memory error ***");
  TC_LOG(
      "Mismatched-size-class "
      "(https://github.com/google/tcmalloc/tree/master/docs/mismatched-sized-delete.md) "
      "discovered for pointer %p: this pointer was recently freed "
      "with a size argument in the range [%v, %v], but the "
      "associated span of allocated memory is for allocations with sizes "
      "[%v, %v]. This is not a bug in tcmalloc, but rather is indicative "
      "of an application bug such as buffer overrun/underrun, use-after-free "
      "or double-free.",
      object, object_min_size, object_max_size, page_min_size, page_max_size);
  TC_LOG(
      "NOTE: The blamed stack trace that is about to crash is not likely the "
      "root cause of the issue. We are detecting the invalid deletion at a "
      "later point in time and different code location.");
  RecordCrash("GWP-ASan", "mismatched-size-class");

  state.gwp_asan_state().RecordMismatch(object_min_size, object_max_size,
                                        page_min_size, page_max_size,
                                        std::nullopt, std::nullopt);
  abort();
}

[[noreturn]]
ABSL_ATTRIBUTE_NOINLINE void ReportDoubleFree(Static& state, void* ptr) {
  static void* stack[kMaxStackDepth];
  const size_t depth = absl::GetStackTrace(stack, kMaxStackDepth, 1);

  RecordCrash("GWP-ASan", "double-free");
  state.gwp_asan_state().RecordDoubleFree(absl::MakeSpan(stack, depth));

  TC_BUG("Possible double free detected of %p", ptr);
}

[[noreturn]]
ABSL_ATTRIBUTE_NOINLINE void ReportCorruptedFree(
    Static& state, std::align_val_t expected_alignment, void* ptr) {
  static void* stack[kMaxStackDepth];
  const size_t depth = absl::GetStackTrace(stack, kMaxStackDepth, 1);

  RecordCrash("GWP-ASan", "invalid-free");
  state.gwp_asan_state().RecordInvalidFree(
      static_cast<std::align_val_t>(
          1u << absl::countr_zero(absl::bit_cast<uintptr_t>(ptr))),
      expected_alignment, absl::MakeSpan(stack, depth));

  TC_BUG("Attempted to free corrupted pointer %p", ptr);
}

}  // namespace tcmalloc::tcmalloc_internal
GOOGLE_MALLOC_SECTION_END
