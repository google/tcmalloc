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

#include <cstddef>
#include <string>

#include "gtest/gtest.h"
#include "fuzztest/fuzztest.h"
#include "absl/types/span.h"
#include "tcmalloc/internal/compressibility.h"

namespace tcmalloc {
namespace tcmalloc_internal {
namespace {

void FuzzAnalyze(std::string data, size_t max_local_copy_size) {
  CompressionAnalyzer analyzer(max_local_copy_size);
  auto res = analyzer.Analyze(absl::MakeConstSpan(data));
  if (res.ok()) {
    EXPECT_LE(res->zero_bytes, data.size());
    EXPECT_LE(res->trailing_zero_bytes, data.size());
    EXPECT_LE(res->trailing_zero_bytes, res->zero_bytes);
  }
}

FUZZ_TEST(CompressibilityFuzzTest, FuzzAnalyze)
    .WithDomains(fuzztest::String(), fuzztest::InRange<size_t>(1, 1024));

}  // namespace
}  // namespace tcmalloc_internal
}  // namespace tcmalloc
