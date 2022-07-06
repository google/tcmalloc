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

#include "tcmalloc/internal/residency.h"

#include <errno.h>
#include <stddef.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include <string>
#include <utility>

#include "benchmark/benchmark.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "absl/status/status.h"
#include "absl/strings/str_format.h"

namespace tcmalloc {
namespace tcmalloc_internal {

class ResidencySpouse {
 public:
  explicit ResidencySpouse(const char* const filename) : r_(filename) {}

  template <typename... Args>
  decltype(auto) Get(Args&&... args) {
    return r_.Get(std::forward<Args>(args)...);
  }

 private:
  Residency r_;
};

namespace {

using ::testing::FieldsAre;
using ::testing::HasSubstr;

#ifdef GTEST_HAS_STATUS_MATCHERS
using ::testing::status::IsOkAndHolds;
using ::testing::status::StatusIs;
#else
class StatusIsMatcher {
 public:
  StatusIsMatcher(const absl::StatusCode& status_code,
                  const testing::Matcher<const std::string&>& message_matcher)
      : status_code_(status_code), message_matcher_(message_matcher) {}

  void DescribeTo(std::ostream* os) const {
    *os << status_code_ << " status code where the message ";
    message_matcher_.DescribeTo(os);
  }

  void DescribeNegationTo(std::ostream* os) const {
    *os << "not (";
    DescribeTo(os);
    *os << ")";
  }

  template <typename StatusType>
  bool MatchAndExplain(const StatusType& actual,
                       testing::MatchResultListener* listener) const {
    const absl::Status& actual_status = GetStatus(actual);
    return actual_status.code() == status_code_ &&
           message_matcher_.MatchAndExplain(
               std::string{actual_status.message()}, listener);
  }

 private:
  static const ::absl::Status& GetStatus(const ::absl::Status& status) {
    return status;
  }
  template <typename T>
  static const ::absl::Status& GetStatus(const ::absl::StatusOr<T>& status) {
    return status.status();
  }
  const absl::StatusCode status_code_;
  const testing::Matcher<const std::string&> message_matcher_;
};

template <typename MessageMatcher>
testing::PolymorphicMatcher<StatusIsMatcher> StatusIs(
    const absl::StatusCode& code, const MessageMatcher& message) {
  return testing::MakePolymorphicMatcher(
      StatusIsMatcher(code, testing::MatcherCast<const std::string&>(message)));
}

MATCHER_P(IsOkAndHolds, value, "") {
  if (!arg.ok()) {
    return false;
  }

  *result_listener << "with value: " << testing::PrintToString(*arg);
  auto matcher = testing::MatcherCast<
      typename std::remove_reference<decltype(arg)>::type::value_type>(value);
  return ExplainMatchResult(matcher, arg.value(), result_listener);
}
#endif

TEST(ResidenceTest, ThisProcess) {
  const size_t kPageSize = getpagesize();
  const int kNumPages = 16;

  // Minimize the chance of a race between munmap and a new mmap.
  void* const mmap_hint = reinterpret_cast<void*>(0x00007BADDEAD0000);

  int base = 0;
#ifdef ABSL_HAVE_THREAD_SANITIZER
  // TSAN completely ignores hints unless you ask really nicely.
  base = MAP_FIXED;
#endif
  // Try both private and shared mappings to make sure we have the bit order of
  // /proc/pid/pageflags correct.
  for (const int flags : {base | MAP_ANONYMOUS | MAP_SHARED,
                          base | MAP_ANONYMOUS | MAP_PRIVATE}) {
    Residency r;
    // Overallocate kNumPages of memory, so we can munmap the page before and
    // after it.
    void* p = mmap(mmap_hint, (kNumPages + 2) * kPageSize,
                   PROT_READ | PROT_WRITE, flags, -1, 0);
    ASSERT_NE(p, MAP_FAILED) << errno;
    EXPECT_THAT(r.Get(p, (kNumPages + 2) * kPageSize),
                IsOkAndHolds(FieldsAre(0, 0)));
    if (p != mmap_hint) {
      absl::FPrintF(stderr,
                    "failed to move test mapping out of the way; we might fail "
                    "due to race\n");
    }
    ASSERT_EQ(munmap(p, kPageSize), 0);
    void* q = reinterpret_cast<char*>(p) + kPageSize;
    void* last = reinterpret_cast<char*>(p) + (kNumPages + 1) * kPageSize;
    ASSERT_EQ(munmap(last, kPageSize), 0);

    memset(q, 0, kNumPages * kPageSize);
    ::benchmark::DoNotOptimize(q);

    EXPECT_THAT(r.Get(q, kPageSize), IsOkAndHolds(FieldsAre(kPageSize, 0)));

    EXPECT_THAT(r.Get(p, (kNumPages + 2) * kPageSize),
                IsOkAndHolds(FieldsAre(kPageSize * kNumPages, 0)));

    EXPECT_THAT(r.Get(reinterpret_cast<char*>(q) + 7, 3 * kPageSize),
                IsOkAndHolds(FieldsAre(kPageSize * 3, 0)));

    EXPECT_THAT(
        r.Get(reinterpret_cast<char*>(q) + 7, (kNumPages + 1) * kPageSize),
        IsOkAndHolds(FieldsAre(kPageSize * kNumPages - 7, 0)));

    ASSERT_EQ(munmap(q, kNumPages * kPageSize), 0);
  }
}

TEST(ResidenceTest, CannotOpen) {
  ResidencySpouse r("/tmp/a667ba48-18ba-4523-a8a7-b49ece3a6c2b");
  EXPECT_THAT(r.Get(nullptr, 1),
              StatusIs(absl::StatusCode::kUnavailable, HasSubstr("open")));
}

TEST(ResidenceTest, CannotRead) {
  ResidencySpouse r("/dev/null");
  EXPECT_THAT(r.Get(nullptr, 1),
              StatusIs(absl::StatusCode::kUnavailable, HasSubstr("read")));
}

TEST(ResidenceTest, CannotSeek) {
  ResidencySpouse r("/dev/null");
  EXPECT_THAT(r.Get(&r, 1),
              StatusIs(absl::StatusCode::kUnavailable, HasSubstr("seek")));
}

}  // namespace
}  // namespace tcmalloc_internal
}  // namespace tcmalloc
