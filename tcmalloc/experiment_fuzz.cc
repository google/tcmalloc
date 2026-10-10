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

#include <set>
#include <vector>

#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "fuzztest/fuzztest.h"
#include "absl/algorithm/container.h"
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

  SelectExperiments(buffer, test_target, active, disabled, unset, hostname,
                    experiments);

  auto IsCompilerExperiment = [](Experiment exp) {
#ifdef NPX_COMPILER_ENABLED_EXPERIMENT
    return exp == Experiment::NPX_COMPILER_EXPERIMENT;
#else
    return false;
#endif
  };

  for (const auto& config : experiments) {
    if (config.force_disable) {
      EXPECT_FALSE(buffer[static_cast<int>(config.id)]);
    }

    if (disabled == "all" && !IsCompilerExperiment(config.id)) {
      EXPECT_FALSE(buffer[static_cast<int>(config.id)]);
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
  (void)IsExperimentRolloutEnabled(config, hostname,
                                   absl::MakeConstSpan(&config, 1));
}

FUZZ_TEST(ExperimentTest, FuzzRolloutEnabled);

TEST(ExperimentTest, FuzzRolloutEnabledRegression) {
  FuzzRolloutEnabled(
      tcmalloc::ExperimentConfig{tcmalloc::Experiment{12}, "", true, false, -1.,
                                 1.7976931348623157e+308, ""},
      "\344\327\344");
}

// Property-based test for asserting that for all hostname/salt pairs,
// experiments are mutually exclusive in the presence of an inverted experiment.
void FuzzInvertedNonoverlap(absl::string_view hostname, absl::string_view salt,
                            std::set<absl::string_view> experiments) {
  if (experiments.empty()) {
    return;
  }
  // If salt is empty, the experiments use their own names, so concurrent
  // experiments are entirely permissible.
  if (salt.empty()) {
    return;
  }

  // Convert inputs to experiment config with fake rollout ranges, leaving space
  // for the implicit control.
  std::vector<ExperimentConfig> arms;
  int exp_id = 0;
  const double stride = 1. / (2 * experiments.size());
  for (absl::string_view exp : experiments) {
    arms.emplace_back(ExperimentConfig{
        static_cast<Experiment>(exp_id),
        exp,
        /*brittle=*/false,
        /*force_disable=*/false,
        /*rollout_lower_bound=*/stride * exp_id,
        /*rollout_upper_bound=*/stride * (exp_id + 1),
        /*rollout_salt=*/salt,
        /*rollout_inverted=*/false,
    });
    exp_id++;

    if (static_cast<Experiment>(exp_id) == Experiment::kMaxExperimentID) {
      break;
    }
  }

  bool buffer[kNumExperiments] = {false};
  SelectExperiments(buffer, "", "", "", /*unset=*/false, hostname, arms);
  // At most 1 experiment is selected.
  EXPECT_LE(absl::c_count(buffer, true), 1);

  // Reset and invert one experiment.  At most 1 experiment should continue to
  // be selected.
  absl::c_fill(buffer, false);
  arms[0].rollout_inverted = true;
  SelectExperiments(buffer, "", "", "", /*unset=*/false, hostname, arms);
  EXPECT_LE(absl::c_count(buffer, true), 1);
}

FUZZ_TEST(ExperimentTest, FuzzInvertedNonoverlap);

TEST(ExperimentTest, FuzzInvertedNonoverlapRegression) {
  FuzzInvertedNonoverlap("", "\370", {});
}

TEST(ExperimentTest, FuzzInvertedNonoverlapEmptySaltRegression) {
  FuzzInvertedNonoverlap("p}}", "", {"\346", "\210"});
}

}  // namespace
}  // namespace tcmalloc::tcmalloc_internal
