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

#include "tcmalloc/internal/fake_clock.h"

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace tcmalloc::tcmalloc_internal {

std::atomic<int64_t> FakeClock::clock_{1234};
std::atomic<size_t> FakeClock::now_calls_{0};
std::atomic<size_t> FakeClock::freq_calls_{0};

}  // namespace tcmalloc::tcmalloc_internal
