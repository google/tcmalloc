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

#include "tcmalloc/internal/logging.h"

#include <string.h>

#include <string>

#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "absl/flags/flag.h"
#include "absl/hash/hash_testing.h"

namespace tcmalloc {
namespace tcmalloc_internal {
namespace {

TEST(StackTraceTest, Hash) {
  StackTrace s1, s2, s3, s4;

  s1.requested_size = 1;
  s1.requested_alignment = 2;
  s1.allocated_size = 4;
  s1.access_hint = 8;
  s1.cold_allocated = false;
  s1.depth = 2;
  s1.stack[0] = absl::bit_cast<void*>(uintptr_t{0xAB});
  s1.stack[1] = absl::bit_cast<void*>(uintptr_t{0xBA});

  s2.requested_size = 8;
  s2.requested_alignment = 4;
  s2.allocated_size = 2;
  s2.access_hint = 1;
  s2.cold_allocated = true;
  s2.depth = 2;
  s2.stack[0] = absl::bit_cast<void*>(uintptr_t{0xBA});
  s2.stack[1] = absl::bit_cast<void*>(uintptr_t{0xAB});

  s3.requested_size = 8;
  s3.requested_alignment = 4;
  s3.allocated_size = 2;
  s3.access_hint = 1;
  s3.cold_allocated = true;
  s3.depth = 3;
  s3.stack[0] = absl::bit_cast<void*>(uintptr_t{0xBA});
  s3.stack[1] = absl::bit_cast<void*>(uintptr_t{0xAB});
  s3.stack[2] = absl::bit_cast<void*>(uintptr_t{0xCD});

  s4.requested_size = 1;
  s4.requested_alignment = 2;
  s4.allocated_size = 4;
  s4.access_hint = 16;
  s4.cold_allocated = true;
  s4.depth = 2;
  s4.stack[0] = absl::bit_cast<void*>(uintptr_t{0xAB});
  s4.stack[1] = absl::bit_cast<void*>(uintptr_t{0xBA});

  EXPECT_TRUE(absl::VerifyTypeImplementsAbslHashCorrectly(
      {StackTrace(), s1, s2, s3, s4}));
}

static std::string* log_buffer;

static void RecordLogMessage(const char* msg, int length) {
  // Make tests less brittle by trimming trailing whitespace
  while (length > 0 && (msg[length - 1] == ' ' || msg[length - 1] == '\n')) {
    length--;
  }
  log_buffer->assign(msg, length);
}

TEST(InternalLogging, MessageFormatting) {
  std::string long_string;
  for (int i = 0; i < 100; i++) {
    long_string += "the quick brown fox jumped over the lazy dog";
  }

  // Arrange to intercept Log() output
  log_buffer = new std::string();
  void (*old_writer)(const char*, int) = log_message_writer;
  log_message_writer = RecordLogMessage;

  Log(kLog, "foo.cc", 100, "Hello");
  EXPECT_EQ("foo.cc:100] Hello", *log_buffer);

  Log(kLog, "foo.cc", 100, 123u, -456, 0);
  EXPECT_EQ("foo.cc:100] 123 -456 0", *log_buffer);

  Log(kLog, "foo.cc", 100, 123u, std::numeric_limits<int64_t>::min());
  EXPECT_EQ("foo.cc:100] 123 -9223372036854775808", *log_buffer);

  Log(kLog, "foo.cc", 2,
      reinterpret_cast<const void*>(static_cast<uintptr_t>(1025)));
  EXPECT_EQ("foo.cc:2] 0x401", *log_buffer);

  Log(kLog, "foo.cc", 10, "hello", long_string.c_str());
  EXPECT_THAT(*log_buffer,
              testing::StartsWith(
                  "foo.cc:10] hello the quick brown fox jumped over the lazy "
                  "dogthe quick brown fox jumped over the lazy dog"));

  Log(kLogWithStack, "foo.cc", 10, "stk");
  EXPECT_TRUE(strstr(log_buffer->c_str(), "stk @ 0x") != nullptr)
      << *log_buffer;

  log_message_writer = old_writer;
  delete log_buffer;
}

TEST(InternalLogging, Assert) {
  CHECK_CONDITION((2 + 2) == 4);

  if (false)
    CHECK_CONDITION(false);
  else
    CHECK_CONDITION(true);

  ASSERT_DEATH(CHECK_CONDITION((2 + 2) == 5),
               ".*tcmalloc\\/internal/logging_test\\.cc:[0-9]+\\] "
               "\\(2 \\+ 2\\) == 5 @( 0x[0-9a-f]+)+");
}

TEST(Printer, RequiredSpace) {
  const char kChunk[] = "0123456789";
  std::string expected;

  for (int i = 0; i < 10; i++) {
    int length = strlen(kChunk) * i + 1;
    std::unique_ptr<char[]> buf(new char[length]);
    Printer printer(buf.get(), length);

    for (int j = 0; j < i; j++) {
      printer.printf("%s", kChunk);
    }
    EXPECT_EQ(buf.get(), expected);
    EXPECT_EQ(length - 1, printer.SpaceRequired());

    // Go past the end of the buffer.  This should not overrun or affect the
    // existing contents of buf, but we should see SpaceRequired tick up.
    printer.printf("%s", kChunk);
    EXPECT_EQ(buf.get(), expected);
    EXPECT_EQ(length - 1 + strlen(kChunk), printer.SpaceRequired());

    expected.append(kChunk);
  }
}

}  // namespace
}  // namespace tcmalloc_internal
}  // namespace tcmalloc
