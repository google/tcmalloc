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
//
// Test out of memory handling.  Kept in a separate test since running out
// of memory causes other parts of the runtime to behave improperly.

#include <stddef.h>
#include <stdlib.h>

#include "gtest/gtest.h"
#include "tcmalloc/testing/testutil.h"

namespace tcmalloc {
namespace {

class OutOfMemoryTest : public ::testing::Test {
 public:
  OutOfMemoryTest() { SetTestResourceLimit(); }
};

TEST_F(OutOfMemoryTest, TestUntilFailure) {
  // Check that large allocations fail with NULL instead of crashing.
  static const size_t kIncrement = 100 << 20;
  static const size_t kMaxSize = ~static_cast<size_t>(0);
  for (size_t s = kIncrement; s < kMaxSize - kIncrement; s += kIncrement) {
    SCOPED_TRACE(s);
    void* large_object = malloc(s);
    if (large_object == nullptr) {
      return;
    }
    free(large_object);
  }
  ASSERT_TRUE(false) << "Did not run out of memory";
}

}  // namespace
}  // namespace tcmalloc
