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

#include "tcmalloc/internal/environment.h"

#include <stdlib.h>
#include <string.h>

#include "gtest/gtest.h"

namespace tcmalloc {
namespace tcmalloc_internal {
namespace {

TEST(EnvironmentTest, thread_safe_getenv) {
  // Should never be defined at test start
  const char *result, *undefined_env_var = "UTIL_TEST_UNDEFINED_ENV_VAR";

  // Check that we handle an undefined variable and then set it
  ASSERT_TRUE(getenv(undefined_env_var) == nullptr);
  ASSERT_TRUE(thread_safe_getenv(undefined_env_var) == nullptr);
  ASSERT_EQ(setenv(undefined_env_var, "1234567890", 0), 0);
  ASSERT_TRUE(getenv(undefined_env_var) != nullptr);

  // Make sure we can find the new variable
  result = thread_safe_getenv(undefined_env_var);
  ASSERT_TRUE(result != nullptr);
  // ... and that it matches what was set
  EXPECT_EQ(strcmp(result, getenv(undefined_env_var)), 0);
}

TEST(EnvironmentTest, TrimWhitespace) {
  EXPECT_EQ(TrimWhitespace(""), "");
  EXPECT_EQ(TrimWhitespace("   "), "");
  EXPECT_EQ(TrimWhitespace("\t\r\n\v\f"), "");
  EXPECT_EQ(TrimWhitespace("hello"), "hello");
  EXPECT_EQ(TrimWhitespace("  hello"), "hello");
  EXPECT_EQ(TrimWhitespace("hello  "), "hello");
  EXPECT_EQ(TrimWhitespace(" \t\r\n hello world \v\f "), "hello world");
}

TEST(EnvironmentTest, ParseBool) {
  // True tokens
  EXPECT_EQ(ParseBool("1"), true);
  EXPECT_EQ(ParseBool("true"), true);
  EXPECT_EQ(ParseBool("True"), true);
  EXPECT_EQ(ParseBool("TRUE"), true);
  EXPECT_EQ(ParseBool("yes"), true);
  EXPECT_EQ(ParseBool("Yes"), true);
  EXPECT_EQ(ParseBool("YES"), true);
  EXPECT_EQ(ParseBool("on"), true);
  EXPECT_EQ(ParseBool("On"), true);
  EXPECT_EQ(ParseBool("ON"), true);
  EXPECT_EQ(ParseBool("  true  "), true);
  EXPECT_EQ(ParseBool("\t1\n"), true);

  // False tokens
  EXPECT_EQ(ParseBool("0"), false);
  EXPECT_EQ(ParseBool("false"), false);
  EXPECT_EQ(ParseBool("False"), false);
  EXPECT_EQ(ParseBool("FALSE"), false);
  EXPECT_EQ(ParseBool("no"), false);
  EXPECT_EQ(ParseBool("No"), false);
  EXPECT_EQ(ParseBool("NO"), false);
  EXPECT_EQ(ParseBool("off"), false);
  EXPECT_EQ(ParseBool("Off"), false);
  EXPECT_EQ(ParseBool("OFF"), false);
  EXPECT_EQ(ParseBool("  false  "), false);
  EXPECT_EQ(ParseBool("\t0\n"), false);

  // Invalid tokens
  EXPECT_EQ(ParseBool(""), std::nullopt);
  EXPECT_EQ(ParseBool("   "), std::nullopt);
  EXPECT_EQ(ParseBool("2"), std::nullopt);
  EXPECT_EQ(ParseBool("-1"), std::nullopt);
  EXPECT_EQ(ParseBool("random"), std::nullopt);
  EXPECT_EQ(ParseBool("tru"), std::nullopt);
  EXPECT_EQ(ParseBool("truee"), std::nullopt);
}

}  // namespace
}  // namespace tcmalloc_internal
}  // namespace tcmalloc
