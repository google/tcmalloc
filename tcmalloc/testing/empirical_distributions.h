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

#ifndef TCMALLOC_TESTING_EMPIRICAL_DISTRIBUTIONS_H_
#define TCMALLOC_TESTING_EMPIRICAL_DISTRIBUTIONS_H_

#include "absl/types/span.h"
#include "tcmalloc/testing/empirical.h"

namespace tcmalloc {
namespace empirical_distributions {

absl::Span<const EmpiricalData::Entry> Beta();
absl::Span<const EmpiricalData::Entry> Bravo();
absl::Span<const EmpiricalData::Entry> Charlie();
absl::Span<const EmpiricalData::Entry> Echo();
absl::Span<const EmpiricalData::Entry> Merced();
absl::Span<const EmpiricalData::Entry> Sierra();
absl::Span<const EmpiricalData::Entry> Sigma();
absl::Span<const EmpiricalData::Entry> Uniform();

}  // namespace empirical_distributions
}  // namespace tcmalloc

#endif  // TCMALLOC_TESTING_EMPIRICAL_DISTRIBUTIONS_H_
