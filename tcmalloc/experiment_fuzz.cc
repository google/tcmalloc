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
#include <stdint.h>
#include <string.h>

#include "fuzztest/fuzztest.h"
#include "absl/strings/string_view.h"
#include "tcmalloc/experiment.h"

namespace tcmalloc::tcmalloc_internal {
namespace {

void FuzzSelectExperiments(absl::string_view test_target,
                           absl::string_view active, absl::string_view disabled,
                           bool unset) {
  if (unset && !test_target.empty() && (!active.empty() || !disabled.empty())) {
    return;
  }

  bool buffer[tcmalloc::tcmalloc_internal::kNumExperiments];

  SelectExperiments(buffer, test_target, active, disabled, unset);
}

FUZZ_TEST(ExperimentTest, FuzzSelectExperiments);

}  // namespace
}  // namespace tcmalloc::tcmalloc_internal
