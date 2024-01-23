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

#include <string.h>

#include <algorithm>
#include <optional>

#include "absl/base/attributes.h"
#include "absl/base/call_once.h"
#include "absl/functional/function_ref.h"
#include "absl/strings/match.h"
#include "absl/strings/string_view.h"
#include "absl/types/optional.h"
#include "tcmalloc/experiment_config.h"
#include "tcmalloc/internal/config.h"
#include "tcmalloc/internal/environment.h"
#include "tcmalloc/internal/logging.h"

GOOGLE_MALLOC_SECTION_BEGIN
namespace tcmalloc {
namespace tcmalloc_internal {
namespace {

const char kDelimiter = ',';
const char kExperiments[] = "BORG_EXPERIMENTS";
const char kDisableExperiments[] = "BORG_DISABLE_EXPERIMENTS";
constexpr absl::string_view kEnableAll = "enable-all-known-experiments";
constexpr absl::string_view kDisableAll = "all";

bool IsCompilerExperiment(Experiment exp) {
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

  absl::base_internal::LowLevelCallOnce(&flag, [&]() {
    const char* active_experiments = thread_safe_getenv(kExperiments);
    const char* disabled_experiments = thread_safe_getenv(kDisableExperiments);
    SelectExperiments(by_id, active_experiments ? active_experiments : "",
                      disabled_experiments ? disabled_experiments : "");
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

const bool* SelectExperiments(bool* buffer, absl::string_view active,
                              absl::string_view disabled) {
  memset(buffer, 0, sizeof(*buffer) * kNumExperiments);

  if (active == kEnableAll) {
    std::fill(buffer, buffer + kNumExperiments, true);
  }

  ParseExperiments(active, [buffer](absl::string_view token) {
    Experiment id;
    if (LookupExperimentID(token, &id)) {
      buffer[static_cast<int>(id)] = true;
    }
  });

  // The compiler experiments should be env variable independent.
#ifdef NPX_COMPILER_ENABLED_EXPERIMENT
  if (!absl::StrContains(active, NPX_COMPILER_ENABLED_EXPERIMENT)) {
    Experiment id;
    if (LookupExperimentID(NPX_COMPILER_ENABLED_EXPERIMENT, &id)) {
      buffer[static_cast<int>(id)] = true;
    }
  }
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

  return buffer;
}

}  // namespace tcmalloc_internal

bool IsExperimentActive(Experiment exp) {
  ASSERT(static_cast<int>(exp) >= 0);
  ASSERT(exp < Experiment::kMaxExperimentID);

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

}  // namespace tcmalloc
GOOGLE_MALLOC_SECTION_END
