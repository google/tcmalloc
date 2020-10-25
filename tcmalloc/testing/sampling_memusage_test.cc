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

#include <stddef.h>
#include <stdlib.h>
#include <sys/types.h>

#include <string>
#include <vector>

#include "absl/base/internal/sysinfo.h"
#include "absl/strings/string_view.h"
#include "absl/types/optional.h"
#include "benchmark/benchmark.h"
#include "gtest/gtest.h"
#include "tcmalloc/common.h"
#include "tcmalloc/internal/logging.h"
#include "tcmalloc/internal/util.h"
#include "tcmalloc/malloc_extension.h"
#include "tcmalloc/static_vars.h"

namespace tcmalloc {
namespace {

using tcmalloc_internal::AllowedCpus;
using tcmalloc_internal::ScopedAffinityMask;

class SamplingMemoryTest : public ::testing::TestWithParam<size_t> {
 protected:
  SamplingMemoryTest() {
    MallocExtension::SetGuardedSamplingRate(-1);
#ifdef TCMALLOC_256K_PAGES
    // For 256k pages, the sampling overhead is larger. Reduce
    // the sampling period to 1<<24
    MallocExtension::SetProfileSamplingRate(1 << 24);
#endif
  }

  size_t Property(absl::string_view name) {
    absl::optional<size_t> result = MallocExtension::GetNumericProperty(name);
    CHECK_CONDITION(result.has_value());
    return *result;
  }

  void SetSamplingInterval(int64_t val) {
    MallocExtension::SetProfileSamplingRate(val);
    // We do this to reset the per-thread sampler - it may have a
    // very large gap put in here if sampling had been disabled.
    ::operator delete(::operator new(1024 * 1024 * 1024));
  }

  size_t CurrentHeapSize() {
    const size_t result = Property("generic.current_allocated_bytes") +
                          Property("tcmalloc.metadata_bytes");
    return result;
  }

  // Return peak memory usage growth when allocating many "size" byte objects.
  ssize_t HeapGrowth(size_t size) {
    if (size < sizeof(void*)) {
      size = sizeof(void*);  // Must be able to fit a pointer in each object
    }

    // For speed, allocate smaller number of total bytes when size is small
    size_t total = 100 << 20;
    if (size <= 4096) {
      total = 30 << 20;
    }

    constexpr int kMaxTries = 10;

    for (int i = 0; i < kMaxTries; i++) {
      // We are trying to make precise measurements about the overhead of
      // allocations.  Keep harness-related allocations outside of our probe
      // points.
      //
      // We pin to a CPU and trigger an allocation of the target size to ensure
      // that the per-CPU slab has been initialized.
      std::vector<int> cpus = AllowedCpus();
      ScopedAffinityMask mask(cpus[0]);

      ::operator delete(::operator new(size));

      const size_t start_memory = CurrentHeapSize();
      void* list = nullptr;
      for (size_t alloc = 0; alloc < total; alloc += size) {
        void** object = reinterpret_cast<void**>(::operator new(size));
        *object = list;
        list = object;
      }
      const size_t peak_memory = CurrentHeapSize();

      while (list != nullptr) {
        void** object = reinterpret_cast<void**>(list);
        list = *object;
        ::operator delete(object);
      }

      if (mask.Tampered()) {
        continue;
      }

      return peak_memory - start_memory;
    }

    return 0;
  }
};

// Check that percent memory overhead created by sampling under the
// specified allocation pattern is not too large.
TEST_P(SamplingMemoryTest, Overhead) {
  const size_t size = GetParam();
  int64_t original = MallocExtension::GetProfileSamplingRate();
  SetSamplingInterval(0);
  const ssize_t baseline = HeapGrowth(size);

  SetSamplingInterval(original);

  const ssize_t with_sampling = HeapGrowth(size);

  // Allocating many MB's of memory should trigger some growth.
  EXPECT_NE(baseline, 0);
  EXPECT_NE(with_sampling, 0);

  const double percent =
      (static_cast<double>(with_sampling) - static_cast<double>(baseline)) *
      100.0 / static_cast<double>(baseline);

  // some noise is unavoidable
  EXPECT_GE(percent, -1.0) << baseline << " " << with_sampling;
  EXPECT_LE(percent, 10.0) << baseline << " " << with_sampling;
}

std::vector<size_t> InterestingSizes() {
  std::vector<size_t> ret;

  for (size_t cl = 1; cl < kNumClasses; cl++) {
    size_t size = tcmalloc::Static::sizemap().class_to_size(cl);
    ret.push_back(size);
  }
  // Add one size not covered by sizeclasses
  ret.push_back(ret.back() + 1);
  return ret;
}

INSTANTIATE_TEST_SUITE_P(AllSizeClasses, SamplingMemoryTest,
                         testing::ValuesIn(InterestingSizes()),
                         testing::PrintToStringParamName());

}  // namespace
}  // namespace tcmalloc

int main(int argc, char** argv) {
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
