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

#ifndef TCMALLOC_INTERNAL_CONFIG_H_
#define TCMALLOC_INTERNAL_CONFIG_H_

#include <stddef.h>

// TCMALLOC_HAVE_SCHED_GETCPU is defined when the system implements
// sched_getcpu(3) as by glibc and it's imitators.
#if defined(__linux__) || defined(__ros__)
#define TCMALLOC_HAVE_SCHED_GETCPU 1
#else
#undef TCMALLOC_HAVE_SCHED_GETCPU
#endif

namespace tcmalloc {

#if defined(__x86_64__)
// x86 has 2 MiB huge pages
static constexpr size_t kHugePageShift = 21;
#elif defined(__PPC64__)
static constexpr size_t kHugePageShift = 24;
#elif defined __aarch64__ && defined __linux__
static constexpr size_t kHugePageShift = 21;
#else
// ...whatever, guess something big-ish
static constexpr size_t kHugePageShift = 21;
#endif

static constexpr size_t kHugePageSize = static_cast<size_t>(1)
                                        << kHugePageShift;

}  // namespace tcmalloc

#endif  // TCMALLOC_INTERNAL_CONFIG_H_
