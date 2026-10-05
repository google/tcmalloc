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

#include "tcmalloc/testing/smaps.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "absl/functional/function_ref.h"
#include "absl/strings/ascii.h"
#include "absl/strings/numbers.h"
#include "absl/strings/str_split.h"
#include "absl/strings/string_view.h"
#include "absl/strings/strip.h"

namespace tcmalloc {
namespace {

bool ParseRangeHeader(absl::string_view line, uintptr_t& start,
                      uintptr_t& end) {
  line = absl::StripLeadingAsciiWhitespace(line);
  const size_t space = line.find_first_of(" \t");
  if (space == absl::string_view::npos) {
    return false;
  }
  absl::string_view range = line.substr(0, space);
  const size_t dash = range.find('-');
  if (dash == absl::string_view::npos) {
    return false;
  }
  return absl::SimpleHexAtoi(range.substr(0, dash), &start) &&
         absl::SimpleHexAtoi(range.substr(dash + 1), &end);
}

class SmapsParser {
 public:
  explicit SmapsParser(absl::FunctionRef<void(const SmapEntry&)> callback)
      : callback_(callback) {}

  ~SmapsParser() {
    if (have_current_) {
      callback_(current_);
    }
  }

  SmapsParser(const SmapsParser&) = delete;
  SmapsParser& operator=(const SmapsParser&) = delete;

  void ProcessLine(absl::string_view line) {
    uintptr_t start;
    uintptr_t end;
    if (ParseRangeHeader(line, start, end)) {
      if (have_current_) {
        callback_(current_);
      }
      current_ = SmapEntry{};
      current_.start = start;
      current_.end = end;
      have_current_ = true;
      return;
    }
    if (absl::ConsumePrefix(&line, "Rss:")) {
      line = absl::StripAsciiWhitespace(line);
      if (!absl::ConsumeSuffix(&line, "kB")) {
        return;
      }
      line = absl::StripTrailingAsciiWhitespace(line);
      size_t rss_kib;
      if (absl::SimpleAtoi(line, &rss_kib)) {
        current_.rss_bytes = rss_kib * 1024;
      }
      return;
    }
    if (absl::ConsumePrefix(&line, "VmFlags:")) {
      line = absl::StripAsciiWhitespace(line);
      const size_t len = std::min(line.size(), sizeof(current_.vm_flags));
      std::memcpy(current_.vm_flags, line.data(), len);
      current_.vm_flags_len = len;
    }
  }

 private:
  absl::FunctionRef<void(const SmapEntry&)> callback_;
  SmapEntry current_;
  bool have_current_ = false;
};

}  // namespace

bool SmapEntry::HasVmFlag(absl::string_view flag) const {
  for (absl::string_view token :
       absl::StrSplit(absl::string_view(vm_flags, vm_flags_len),
                      absl::ByAnyChar(" \t\r\n"), absl::SkipEmpty())) {
    if (token == flag) {
      return true;
    }
  }
  return false;
}

void ParseSmaps(absl::string_view content,
                absl::FunctionRef<void(const SmapEntry&)> callback) {
  SmapsParser parser(callback);
  for (absl::string_view line : absl::StrSplit(content, '\n')) {
    parser.ProcessLine(line);
  }
}

bool ForEachSmapEntry(absl::FunctionRef<void(const SmapEntry&)> callback) {
  FILE* f = fopen("/proc/self/smaps", "r");
  if (f == nullptr) {
    return false;
  }

  {
    SmapsParser parser(callback);
    char buf[BUFSIZ];
    while (fgets(buf, sizeof(buf), f) != nullptr) {
      parser.ProcessLine(buf);
    }
  }
  const bool ok = feof(f) != 0 && ferror(f) == 0;
  fclose(f);
  return ok;
}

}  // namespace tcmalloc
