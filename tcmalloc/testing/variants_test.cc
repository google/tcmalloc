// Copyright 2023 The TCMalloc Authors
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

#include <array>
#include <cerrno>
#include <cstddef>
#include <cstring>
#include <string>
#include <vector>

#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "absl/base/attributes.h"
#include "absl/base/const_init.h"
#include "absl/base/internal/spinlock.h"
#include "absl/base/thread_annotations.h"
#include "absl/container/flat_hash_set.h"
#include "absl/strings/match.h"
#include "absl/strings/str_split.h"
#include "absl/strings/string_view.h"
#include "tcmalloc/experiment.h"
#include "tcmalloc/internal/environment.h"
#include "tcmalloc/internal/logging.h"
#include "tcmalloc/internal/percpu.h"

namespace tcmalloc {

ABSL_CONST_INIT static absl::base_internal::SpinLock envlock(
    absl::kConstInit, absl::base_internal::SCHEDULE_KERNEL_ONLY);
ABSL_CONST_INIT int envcount ABSL_GUARDED_BY(envlock) = 0;
constexpr size_t kNumEnvNames = 16;
ABSL_CONST_INIT std::array<char[128], kNumEnvNames> envnames
    ABSL_GUARDED_BY(envlock) = {};

namespace tcmalloc_internal {

extern "C" char** environ;

// This is a specialized version of thread_safe_getenv that overrides the weak
// implementation in google3/tcmalloc/internal/environment.cc.  It
// records the requested environment variable names for introspection.
const char* thread_safe_getenv(const char* env_var) {
  int var_len = strlen(env_var);

  char** envv = environ;
  if (!envv) {
    return nullptr;
  }

  {
    absl::base_internal::SpinLockHolder l(&envlock);

    int index;
    for (index = 0; index < envcount; index++) {
      if (strncmp(envnames[index], env_var, var_len) == 0) {
        break;
      }
    }

    if (index == envcount) {
      TC_CHECK_LT(index, envnames.size());
      // Not found, insert.
      strncpy(envnames[index], env_var, sizeof(envnames[0]));
      ++envcount;
    }
  }

  for (; *envv != nullptr; envv++)
    if (strncmp(*envv, env_var, var_len) == 0 && (*envv)[var_len] == '=')
      return *envv + var_len + 1;  // skip over the '='

  return nullptr;
}

}  // namespace tcmalloc_internal

namespace {

TEST(TCMallocVariant, KnownExperimentVariants) {
  // This test confirms that all experiment parameters passed for testing
  // purposes are known to the binary.  If any are unknown, this indicates a
  // test configuration that can be cleaned up.
  auto env = absl::NullSafeStringView(
      tcmalloc_internal::thread_safe_getenv("BORG_EXPERIMENTS"));
  if (env.empty()) {
    return;
  }
  for (auto name : absl::StrSplit(env, ',')) {
    auto exp = FindExperimentByName(name);
    EXPECT_TRUE(exp && IsExperimentActive(exp.value()));
  }
}

// This test verifies that the list of environment variables accessed through
// thread_safe_getenv is the superset of the variables specified during testing.
TEST(TCMallocVariant, KnownEnvironmentVariables) {
  absl::flat_hash_set<absl::string_view> exemptions;

  std::vector<std::string> strings;
  strings.resize(kNumEnvNames);
  for (int i = 0; i < kNumEnvNames; ++i) {
    strings[i].reserve(sizeof(envnames[0]));
  }

  {
    absl::base_internal::SpinLockHolder l(&envlock);

    for (int i = 0; i < envcount; ++i) {
      strings[i].assign(envnames[i]);
    }

    strings.resize(envcount);
  }

  absl::flat_hash_set<absl::string_view> names;
  for (const auto& s : strings) {
    names.insert(s);
  }

  // Iterate over environment.  If any match TCMALLOC..., verify they appear in
  // `names`.
  char** envv = environ;
  for (; *envv != nullptr; envv++) {
    absl::string_view env(*envv);
    auto eq = env.find('=');
    if (eq != absl::string_view::npos) {
      env = env.substr(0, eq);
    }

    if (!absl::StartsWith(env, "TCMALLOC")) {
      continue;
    }

    if (exemptions.contains(env)) {
      continue;
    }

    EXPECT_THAT(names, testing::Contains(env));
  }
}

TEST(TCMallocVariant, GlibcRseq) {
#if !TCMALLOC_INTERNAL_PERCPU_USE_RSEQ
  GTEST_SKIP() << "RSEQ not supported";
#endif

  const char* env = tcmalloc_internal::thread_safe_getenv("GLIBC_TUNABLES");
  if (!env) {
    GTEST_SKIP() << "Not turning off glibc RSEQ";
  }

  if (tcmalloc_internal::subtle::percpu::IsFast()) {
    GTEST_SUCCEED() << "RSEQ working.";
    return;
  }

  // This should fail with ENOSYS, not another error.
  int ret = syscall(__NR_rseq, nullptr, nullptr, 0, 0);
  int err = errno;
  ASSERT_EQ(ret, -1);
  EXPECT_EQ(err, ENOSYS) << "Unexpected errno";
}

}  // namespace
}  // namespace tcmalloc
