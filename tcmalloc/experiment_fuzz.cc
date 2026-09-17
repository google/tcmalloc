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

#include <stddef.h>
#include <stdint.h>

#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "fuzztest/fuzztest.h"
#include "absl/strings/string_view.h"
#include "tcmalloc/experiment.h"
#include "tcmalloc/experiment_config.h"

namespace tcmalloc::tcmalloc_internal {
namespace {

void FuzzSelectExperiments(absl::string_view test_target,
                           absl::string_view active, absl::string_view disabled,
                           bool unset, absl::string_view hostname) {
  if (unset && !test_target.empty() && (!active.empty() || !disabled.empty())) {
    return;
  }

  bool buffer[tcmalloc::tcmalloc_internal::kNumExperiments];
  bool buffer2[tcmalloc::tcmalloc_internal::kNumExperiments];

  SelectExperiments(buffer, test_target, active, disabled, unset, hostname,
                    experiments);
  SelectExperiments(buffer2, test_target, active, disabled, unset, hostname,
                    experiments);

  auto IsCompilerExperiment = [](Experiment exp) {
#ifdef NPX_COMPILER_ENABLED_EXPERIMENT
    return exp == Experiment::NPX_COMPILER_EXPERIMENT;
#else
    return false;
#endif
  };

  for (const auto& config : experiments) {
    const int id = static_cast<int>(config.id);
    if (!(unset && !test_target.empty())) {
      EXPECT_EQ(buffer[id], buffer2[id]);
    }

    if (config.force_disable) {
      EXPECT_FALSE(buffer[id]);
    }

    if (disabled == "all" && !IsCompilerExperiment(config.id)) {
      EXPECT_FALSE(buffer[id]);
    }

    if (active == "enable-all-known-experiments" && disabled.empty() &&
        !config.force_disable && !config.brittle) {
      EXPECT_TRUE(buffer[id]);
    }
  }
}

FUZZ_TEST(ExperimentTest, FuzzSelectExperiments);

TEST(ExperimentTest, FuzzSelectExperiments_b395212979) {
  FuzzSelectExperiments(
      "t_fuenchmark&"
      "vvvvvvvvvvvvvvvvvvVvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvv",
      "", "", true, "some_hostname");
}

void FuzzRolloutEnabled(const ExperimentConfig& config,
                        absl::string_view hostname) {
  const bool enabled = IsExperimentRolloutEnabled(config, hostname);
  EXPECT_EQ(enabled, IsExperimentRolloutEnabled(config, hostname));

  if (hostname.empty()) {
    EXPECT_EQ(enabled, config.rollout_inverted);
    return;
  }

  if (!config.rollout_inverted) {
    if (config.rollout_lower_bound >= config.rollout_upper_bound ||
        config.rollout_upper_bound <= 0.0 ||
        config.rollout_lower_bound >= 1.0) {
      EXPECT_FALSE(enabled);
    } else if (config.rollout_lower_bound <= 0.0 &&
               config.rollout_upper_bound >= 1.0) {
      EXPECT_TRUE(enabled);
    }
  } else {
    const double ablation_lower =
        1.0 - (config.rollout_upper_bound - config.rollout_lower_bound);
    if (ablation_lower <= 0.0) {
      EXPECT_FALSE(enabled);
    } else if (ablation_lower >= 1.0) {
      EXPECT_TRUE(enabled);
    }
  }
}

FUZZ_TEST(ExperimentTest, FuzzRolloutEnabled);

TEST(ExperimentTest, FuzzRolloutEnabledRegression) {
  FuzzRolloutEnabled(
      tcmalloc::ExperimentConfig{tcmalloc::Experiment{12}, "", true, false, -1.,
                                 1.7976931348623157e+308, ""},
      "\344\327\344");
}

}  // namespace
}  // namespace tcmalloc::tcmalloc_internal
