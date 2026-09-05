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

#include <string.h>

#include <optional>

#include "absl/base/attributes.h"
#include "absl/strings/string_view.h"
#include "tcmalloc/internal/config.h"

GOOGLE_MALLOC_SECTION_BEGIN
namespace tcmalloc {
namespace tcmalloc_internal {
namespace {

constexpr bool IsAsciiWhitespace(char c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' ||
         c == '\f';
}

constexpr char AsciiToLower(char c) {
  if (c >= 'A' && c <= 'Z') {
    return c + ('a' - 'A');
  }
  return c;
}

bool EqualsIgnoreCase(absl::string_view a, absl::string_view b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i) {
    if (AsciiToLower(a[i]) != AsciiToLower(b[i])) return false;
  }
  return true;
}

}  // namespace

absl::string_view TrimWhitespace(absl::string_view str) {
  while (!str.empty() && IsAsciiWhitespace(str.front())) {
    str.remove_prefix(1);
  }
  while (!str.empty() && IsAsciiWhitespace(str.back())) {
    str.remove_suffix(1);
  }
  return str;
}

std::optional<bool> ParseBool(absl::string_view str) {
  str = TrimWhitespace(str);
  if (EqualsIgnoreCase(str, "1") || EqualsIgnoreCase(str, "true") ||
      EqualsIgnoreCase(str, "yes") || EqualsIgnoreCase(str, "on")) {
    return true;
  }
  if (EqualsIgnoreCase(str, "0") || EqualsIgnoreCase(str, "false") ||
      EqualsIgnoreCase(str, "no") || EqualsIgnoreCase(str, "off")) {
    return false;
  }
  return std::nullopt;
}

#ifdef __linux__
// POSIX provides the **environ array which contains environment variables in a
// linear array, terminated by a NULL string.  This array is only perturbed when
// the environment is changed (which is inherently unsafe) so it's safe to
// return a const pointer into it.
// e.g. { "SHELL=/bin/bash", "MY_ENV_VAR=1", "" }
extern "C" char** environ;
ABSL_ATTRIBUTE_WEAK const char* thread_safe_getenv(const char* env_var) {
  int var_len = strlen(env_var);

  char** envv = environ;
  if (!envv) {
    return nullptr;
  }

  for (; *envv != nullptr; envv++)
    if (strncmp(*envv, env_var, var_len) == 0 && (*envv)[var_len] == '=')
      return *envv + var_len + 1;  // skip over the '='

  return nullptr;
}
#else
const char* thread_safe_getenv(const char* env_var) { return nullptr; }
#endif

}  // namespace tcmalloc_internal
}  // namespace tcmalloc
GOOGLE_MALLOC_SECTION_END
