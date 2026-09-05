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

#include "tcmalloc/experiment.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <map>
#include <optional>
#include <string>
#include <string_view>

#include "absl/base/attributes.h"
#include "absl/base/call_once.h"
#include "absl/base/internal/cycleclock.h"
#include "absl/functional/function_ref.h"
#include "absl/hash/hash.h"
#include "absl/strings/match.h"
#include "absl/strings/string_view.h"
#include "absl/strings/strip.h"
#include "tcmalloc/experiment_config.h"
#include "tcmalloc/internal/config.h"
#include "tcmalloc/internal/environment.h"
#include "tcmalloc/malloc_extension.h"

GOOGLE_MALLOC_SECTION_BEGIN
namespace tcmalloc {
namespace tcmalloc_internal {
namespace {

const char kDelimiter = ',';
const char kExperiments[] = "BORG_EXPERIMENTS";
const char kDisableExperiments[] = "BORG_DISABLE_EXPERIMENTS";
constexpr absl::string_view kEnableAll = "enable-all-known-experiments";
constexpr absl::string_view kDisableAll = "all";

bool IsCompilerExperiment(Experiment exp ABSL_ATTRIBUTE_UNUSED) {
#ifdef NPX_COMPILER_ENABLED_EXPERIMENT
  return exp == Experiment::NPX_COMPILER_EXPERIMENT;
#else
  return false;
#endif
}

bool LookupExperimentID(absl::string_view label, Experiment* exp) {
  for (auto config : experiments) {
    if (config.name == label) {
      *exp = config.id;
      return true;
    }
  }

  return false;
}

const bool* GetSelectedExperiments() {
  ABSL_CONST_INIT static bool by_id[kNumExperiments];
  ABSL_CONST_INIT static absl::once_flag flag;

  absl::base_internal::LowLevelCallOnce(&flag, [&]() GOOGLE_MALLOC_SECTION {
    const char* test_target = thread_safe_getenv("TEST_TARGET");
    const char* active_experiments = thread_safe_getenv(kExperiments);
    const char* disabled_experiments = thread_safe_getenv(kDisableExperiments);
    absl::string_view hostname = LookupHostname();

    SelectExperiments(
        by_id, test_target ? test_target : "",
        active_experiments ? active_experiments : "",
        disabled_experiments ? disabled_experiments : "",
        active_experiments == nullptr && disabled_experiments == nullptr,
        hostname);
  });
  return by_id;
}

template <typename F>
void ParseExperiments(absl::string_view labels, F f) {
  absl::string_view::size_type pos = 0;
  do {
    absl::string_view token;
    auto end = labels.find(kDelimiter, pos);
    if (end == absl::string_view::npos) {
      token = labels.substr(pos);
      pos = end;
    } else {
      token = labels.substr(pos, end - pos);
      pos = end + 1;
    }

    f(token);
  } while (pos != absl::string_view::npos);
}

}  // namespace

absl::string_view LookupHostname() {
  return {};
}

std::optional<uint64_t> CalculateRolloutBucket(absl::string_view hostname,
                                               absl::string_view salt) {
  // Ensure no experiments are selected.
  return std::nullopt;
}

bool IsExperimentRolloutEnabled(const ExperimentConfig& config,
                                absl::string_view hostname) {
  if (hostname.empty()) {
    return false;
  }

  // Ensure experiments are i.i.d. from one another by using their own names as
  // a salt, unless explicitly requested.
  absl::string_view salt =
      config.rollout_salt.empty() ? config.name : config.rollout_salt;
  const std::optional<uint64_t> val = CalculateRolloutBucket(hostname, salt);
  if (!val.has_value()) {
    return false;
  }

  constexpr int digits = std::numeric_limits<double>::digits;
  // Scale val onto [0, 1.0).
  const double target = std::ldexp(*val >> (64 - digits), -digits);
  return target >= config.rollout_lower_bound &&
         target < config.rollout_upper_bound;
}

void SelectExperiments(bool* buffer, absl::string_view test_target,
                       absl::string_view active, absl::string_view disabled,
                       bool unset, absl::string_view hostname) {
  memset(buffer, 0, sizeof(*buffer) * kNumExperiments);

  if (active == kEnableAll) {
    std::fill(buffer, buffer + kNumExperiments, true);
  }

  for (const auto& config : experiments) {
    if (config.rollout_upper_bound > 0) {
      if (IsExperimentRolloutEnabled(config, hostname)) {
        buffer[static_cast<int>(config.id)] = true;
      }
    }
  }

  ParseExperiments(active, [buffer](absl::string_view token) {
    Experiment id;
    if (LookupExperimentID(token, &id)) {
      buffer[static_cast<int>(id)] = true;
    }
  });

  // The compiler experiments should be env variable independent.
#ifdef NPX_COMPILER_ENABLED_EXPERIMENT
#define Q(x) STR(x)
#define STR(x) #x
  if (!absl::StrContains(active, Q(NPX_COMPILER_ENABLED_EXPERIMENT))) {
    Experiment id;
    if (LookupExperimentID(Q(NPX_COMPILER_ENABLED_EXPERIMENT), &id)) {
      buffer[static_cast<int>(id)] = true;
    }
  }
#undef STR
#undef Q
#endif

  if (disabled == kDisableAll) {
    for (auto config : experiments) {
      // Exclude compile-time experiments
      if (!IsCompilerExperiment(config.id)) {
        buffer[static_cast<int>(config.id)] = false;
      }
    }
  }

  // disable non-compiler experiments
  ParseExperiments(disabled, [buffer](absl::string_view token) {
    Experiment id;
    if (LookupExperimentID(token, &id) && !IsCompilerExperiment(id)) {
      buffer[static_cast<int>(id)] = false;
    }
  });

  // Enable some random combination of experiments for tests that don't
  // explicitly set any of the experiment env vars. This allows to get better
  // test coverage of experiments before production.
  // Tests can opt out by exporting BORG_EXPERIMENTS="".
  // Enabled experiments are selected based on the stable test target name hash,
  // this allows get a wide range of experiment permutations on a large test
  // base, but at the same time avoids flaky test failures (if a particular
  // test fails only with a particular experiment combination).
  // It would be nice to print what experiments we enable, but printing even
  // to stderr breaks some tests that capture subprocess output.
  if (unset && !test_target.empty()) {
    // TODO: b/454666418 - Replace with TC_CHECK when the synchronization
    // experimentation is finished.
    assert(active.empty() && disabled.empty());
    uint64_t seed =
        static_cast<uint64_t>(absl::base_internal::CycleClock::Now());
    const size_t target_hash = absl::HashOf(test_target, seed);
    constexpr size_t kVanillaOneOf = 11;
    constexpr size_t kEnableOneOf = 3;
    if ((target_hash % kVanillaOneOf) != 0) {
      int num_enabled_experiments = 0;
      Experiment experiment_id = Experiment::kMaxExperimentID;
      for (auto config : experiments) {
        if (IsCompilerExperiment(config.id) || config.brittle) {
          continue;
        }
        experiment_id = config.id;

        // Enabling is specifically based on the experiment name so that it's
        // stable when experiments are added/removed.
        bool enabled =
            ((target_hash ^ absl::HashOf(config.name)) % kEnableOneOf) == 0;
        buffer[static_cast<int>(config.id)] |= enabled;
        num_enabled_experiments += enabled;
      }
      // In case the hash-based selection above did not work out, select the
      // last experiment.
      if (num_enabled_experiments == 0 &&
          experiment_id != Experiment::kMaxExperimentID) {
        // TODO: b/454666418 - Replace with TC_CHECK when the synchronization
        // experimentation is finished.
        assert(!buffer[static_cast<int>(experiment_id)]);
        buffer[static_cast<int>(experiment_id)] = true;
      }
    }
  }

  // Ensure unsafe experiments are disabled.
  for (const auto& config : experiments) {
    if (config.force_disable) {
      buffer[static_cast<int>(config.id)] = false;
    }
  }
}

static_assert(
    [] {
      for (const auto& e : experiments) {
        if (e.rollout_lower_bound < 0.0) return false;
        if (e.rollout_upper_bound > 1.0) return false;
      }
      return true;
    }(),
    "rollout bounds must be in [0, 1]");

}  // namespace tcmalloc_internal

bool IsExperimentActive(Experiment exp) {
  // TODO: b/454666418 - Replace with TC_CHECK when the synchronization
  // experimentation is finished.
  assert(static_cast<int>(exp) >= 0);
  assert(exp < Experiment::kMaxExperimentID);

  return tcmalloc_internal::GetSelectedExperiments()[static_cast<int>(exp)];
}

std::optional<Experiment> FindExperimentByName(absl::string_view name) {
  for (const auto& config : experiments) {
    if (name == config.name) {
      return config.id;
    }
  }

  return std::nullopt;
}

void WalkExperiments(
    absl::FunctionRef<void(absl::string_view name, bool active)> callback) {
  for (const auto& config : experiments) {
    callback(config.name, IsExperimentActive(config.id));
  }
}

extern "C" void MallocExtension_Internal_GetExperiments(
    tcmalloc::MallocExtension::PropertyMap* result) {
  WalkExperiments([&](absl::string_view name, bool active) {
    (*result)[absl::StrCat("tcmalloc.experiment.", name)].value = active;
  });
}

}  // namespace tcmalloc
GOOGLE_MALLOC_SECTION_END
