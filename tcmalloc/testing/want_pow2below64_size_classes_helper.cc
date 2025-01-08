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

#include <cstdio>

#include "absl/types/span.h"
#include "tcmalloc/common.h"
#include "tcmalloc/internal/logging.h"
#include "tcmalloc/sizemap.h"
#include "tcmalloc/static_vars.h"

int main(int argc, char** argv) {
  const size_t kExpectedClasses[] = {0, 8, 16, 32, 64, 72, 80, 88};

  absl::Span<const size_t> classes = absl::MakeSpan(kExpectedClasses);

  TC_CHECK_LE(classes.size(), tcmalloc::tcmalloc_internal::kNumClasses);
  for (int c = 0; c < classes.size(); ++c) {
    if (tcmalloc::tcmalloc_internal::Static::sizemap().class_to_size(c) !=
        classes[c]) {
      printf("Other");
      return 0;
    }
  }
  printf("Pow2Below64");

  return 0;
}
