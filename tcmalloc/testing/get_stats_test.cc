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

#include <ctype.h>
#include <pthread.h>
#include <stddef.h>
#include <string.h>

#include <algorithm>
#include <cstdlib>
#include <limits>
#include <memory>
#include <optional>
#include <string>

#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "absl/base/config.h"
#include "absl/strings/match.h"
#include "absl/strings/str_cat.h"
#include "absl/time/time.h"
#include "absl/types/optional.h"
#include "tcmalloc/common.h"
#include "tcmalloc/experiment.h"
#include "tcmalloc/experiment_config.h"
#include "tcmalloc/internal/logging.h"
#include "tcmalloc/malloc_extension.h"
#include "tcmalloc/parameters.h"
#include "tcmalloc/testing/testutil.h"

namespace tcmalloc {
namespace {

using tcmalloc_internal::Parameters;
using ::testing::AnyOf;
using ::testing::ContainsRegex;
using ::testing::HasSubstr;

class GetStatsTest : public ::testing::Test {};

TEST_F(GetStatsTest, Pbtxt) {
  // Trigger a sampled allocation.
  constexpr size_t kSize = 123456;
  void* alloc;
  {
    ScopedAlwaysSample s;
    alloc = ::operator new(kSize);
  }

  const std::string buf = GetStatsInPbTxt();

  std::optional<size_t> fragmentation = MallocExtension::GetNumericProperty(
      "tcmalloc.sampled_internal_fragmentation");
  ASSERT_THAT(fragmentation, testing::Ne(std::nullopt));
  EXPECT_GT(*fragmentation, 0);

  // Expect `buf` to be in pbtxt format.
  EXPECT_THAT(buf, ContainsRegex(R"(in_use_by_app: [0-9]+)"));
  EXPECT_THAT(buf, ContainsRegex(R"(page_heap_freelist: [0-9]+)"));
  EXPECT_THAT(buf, ContainsRegex(R"(tcmalloc_huge_page_size: [0-9]+)"));
#if defined(GTEST_USES_PCRE)
  EXPECT_THAT(buf,
              ContainsRegex(absl::StrCat(
                  R"(freelist\s{\s*sizeclass:\s\d+\s*bytes:\s\d+\s*)",
                  R"(num_spans_requested:\s\d+\s*num_spans_returned:\s\d+\s*)",
                  R"(obj_capacity:\s\d+\s*)")));
#endif  // defined(GTEST_USES_PCRE)
  EXPECT_THAT(buf, AnyOf(HasSubstr(R"(page_heap {)"),
                         HasSubstr(R"(huge_page_aware {)")));
  EXPECT_THAT(buf, HasSubstr(R"(gwp_asan {)"));

  EXPECT_THAT(buf, ContainsRegex(R"(mmap_sys_allocator: [0-9]*)"));
  EXPECT_THAT(buf, HasSubstr("memory_release_failures: 0"));

  if (MallocExtension::PerCpuCachesActive()) {
    EXPECT_THAT(buf, ContainsRegex(R"(per_cpu_cache_freelist: [1-9][0-9]*)"));
    EXPECT_THAT(buf, ContainsRegex(R"(percpu_slab_size: [1-9][0-9]*)"));
    EXPECT_THAT(buf, ContainsRegex(R"(percpu_slab_residence: [1-9][0-9]*)"));
  } else {
    EXPECT_THAT(buf, HasSubstr("per_cpu_cache_freelist: 0"));
    EXPECT_THAT(buf, HasSubstr("percpu_slab_size: 0"));
    EXPECT_THAT(buf, HasSubstr("percpu_slab_residence: 0"));
  }
  EXPECT_THAT(buf, ContainsRegex("(cpus_allowed: [1-9][0-9]*)"));

  EXPECT_THAT(buf, HasSubstr("desired_usage_limit_bytes: -1"));
  EXPECT_THAT(buf,
              HasSubstr(absl::StrCat("profile_sampling_interval: ",
                                     Parameters::profile_sampling_interval())));
  EXPECT_THAT(buf, HasSubstr("limit_hits: 0"));
#ifdef TCMALLOC_INTERNAL_SMALL_BUT_SLOW
  EXPECT_THAT(buf, HasSubstr("tcmalloc_skip_subrelease_short_interval_ns: 0"));
  EXPECT_THAT(buf, HasSubstr("tcmalloc_skip_subrelease_long_interval_ns: 0"));
#else
  EXPECT_THAT(
      buf,
      HasSubstr("tcmalloc_skip_subrelease_short_interval_ns: 60000000000"));
  EXPECT_THAT(
      buf,
      HasSubstr("tcmalloc_skip_subrelease_long_interval_ns: 300000000000"));
#endif
#ifdef TCMALLOC_INTERNAL_SMALL_BUT_SLOW
  EXPECT_THAT(buf,
              HasSubstr("tcmalloc_cache_demand_release_short_interval_ns: 0"));
  EXPECT_THAT(buf,
              HasSubstr("tcmalloc_cache_demand_release_long_interval_ns: 0"));
#else
  EXPECT_THAT(
      buf, HasSubstr(
               "tcmalloc_cache_demand_release_short_interval_ns: 10000000000"));
  EXPECT_THAT(
      buf,
      HasSubstr("tcmalloc_cache_demand_release_long_interval_ns: 30000000000"));
#endif
  EXPECT_THAT(buf, HasSubstr("tcmalloc_release_partial_alloc_pages: true"));
  EXPECT_THAT(buf,
              HasSubstr("tcmalloc_huge_region_demand_based_release: false"));
  EXPECT_THAT(buf,
              HasSubstr("tcmalloc_huge_cache_demand_based_release: false"));
  EXPECT_THAT(buf, HasSubstr("tcmalloc_release_pages_from_huge_region: true"));
  if (!IsExperimentActive(Experiment::TCMALLOC_MIN_HOT_ACCESS_HINT_ABLATION)) {
    EXPECT_THAT(buf, HasSubstr("min_hot_access_hint: 2"));
  } else {
    EXPECT_THAT(buf, HasSubstr("min_hot_access_hint: 1"));
  }

  sized_delete(alloc, kSize);
}

TEST_F(GetStatsTest, Parameters) {
  Parameters::set_hpaa_subrelease(false);
  Parameters::set_guarded_sampling_interval(-1);
#ifdef TCMALLOC_DEPRECATED_PERTHREAD
  Parameters::set_per_cpu_caches(false);
#endif  // TCMALLOC_DEPRECATED_PERTHREAD
  Parameters::set_max_per_cpu_cache_size(-1);
  Parameters::set_max_total_thread_cache_bytes(-1);
  Parameters::set_filler_skip_subrelease_short_interval(absl::Seconds(2));
  Parameters::set_filler_skip_subrelease_long_interval(absl::Seconds(3));
  Parameters::set_cache_demand_release_short_interval(absl::Seconds(4));
  Parameters::set_cache_demand_release_long_interval(absl::Seconds(5));

  auto using_hpaa = [](absl::string_view sv) {
    return absl::StrContains(sv, "HugePageAwareAllocator");
  };

  {
    const std::string buf = MallocExtension::GetStats();
    const std::string pbtxt = GetStatsInPbTxt();

    if (using_hpaa(buf)) {
      EXPECT_THAT(buf, HasSubstr(R"(PARAMETER hpaa_subrelease 1)"));
    }
    EXPECT_THAT(buf,
                HasSubstr(R"(PARAMETER tcmalloc_guarded_sample_parameter -1)"));
#ifdef TCMALLOC_DEPRECATED_PERTHREAD
    EXPECT_THAT(buf, HasSubstr(R"(PARAMETER tcmalloc_per_cpu_caches 0)"));
#endif  // TCMALLOC_DEPRECATED_PERTHREAD
    EXPECT_THAT(buf,
                HasSubstr(R"(PARAMETER tcmalloc_max_per_cpu_cache_size -1)"));
    EXPECT_THAT(
        buf,
        HasSubstr(R"(PARAMETER tcmalloc_max_total_thread_cache_bytes -1)"));
    EXPECT_THAT(
        buf,
        HasSubstr(R"(PARAMETER tcmalloc_skip_subrelease_short_interval 2s)"));
    EXPECT_THAT(
        buf,
        HasSubstr(R"(PARAMETER tcmalloc_skip_subrelease_long_interval 3s)"));

    EXPECT_THAT(
        buf, HasSubstr(R"(PARAMETER tcmalloc_release_partial_alloc_pages 1)"));
    EXPECT_THAT(
        buf,
        HasSubstr(R"(PARAMETER tcmalloc_huge_cache_demand_based_release 0)"));
    EXPECT_THAT(
        buf,
        HasSubstr(R"(PARAMETER tcmalloc_huge_region_demand_based_release 0)"));
    EXPECT_THAT(
        buf,
        HasSubstr(R"(PARAMETER tcmalloc_release_pages_from_huge_region 1)"));
    if (using_hpaa(buf)) {
      EXPECT_THAT(buf, HasSubstr(R"(using_hpaa_subrelease: false)"));
    }

    EXPECT_THAT(pbtxt, HasSubstr(R"(guarded_sample_parameter: -1)"));
#ifdef TCMALLOC_DEPRECATED_PERTHREAD
    EXPECT_THAT(pbtxt, HasSubstr(R"(tcmalloc_per_cpu_caches: false)"));
#endif  // TCMALLOC_DEPRECATED_PERTHREAD
    EXPECT_THAT(pbtxt, HasSubstr(R"(tcmalloc_max_per_cpu_cache_size: -1)"));
    EXPECT_THAT(pbtxt,
                HasSubstr(R"(tcmalloc_max_total_thread_cache_bytes: -1)"));
    EXPECT_THAT(
        pbtxt,
        HasSubstr(R"(tcmalloc_skip_subrelease_short_interval_ns: 2000000000)"));
    EXPECT_THAT(
        pbtxt,
        HasSubstr(R"(tcmalloc_skip_subrelease_long_interval_ns: 3000000000)"));
    EXPECT_THAT(
        pbtxt,
        HasSubstr(
            R"(tcmalloc_cache_demand_release_short_interval_ns: 4000000000)"));
    EXPECT_THAT(
        pbtxt,
        HasSubstr(
            R"(tcmalloc_cache_demand_release_long_interval_ns: 5000000000)"));
    if (!IsExperimentActive(
            Experiment::TCMALLOC_MIN_HOT_ACCESS_HINT_ABLATION)) {
      EXPECT_THAT(pbtxt, HasSubstr(R"(min_hot_access_hint: 2)"));
    } else {
      EXPECT_THAT(pbtxt, HasSubstr(R"(min_hot_access_hint: 1)"));
    }
  }

  Parameters::set_hpaa_subrelease(true);
  Parameters::set_guarded_sampling_interval(
      50 * Parameters::profile_sampling_interval());
  Parameters::set_per_cpu_caches(true);
  Parameters::set_max_per_cpu_cache_size(3 << 20);
  Parameters::set_max_total_thread_cache_bytes(4 << 20);
  Parameters::set_filler_skip_subrelease_short_interval(
      absl::Milliseconds(120250));
  Parameters::set_filler_skip_subrelease_long_interval(
      absl::Milliseconds(180375));
  Parameters::set_cache_demand_release_short_interval(
      absl::Milliseconds(180250));
  Parameters::set_cache_demand_release_long_interval(
      absl::Milliseconds(240375));
  Parameters::set_huge_cache_demand_based_release(true);
  Parameters::set_min_hot_access_hint(hot_cold_t{3});

  {
    const std::string buf = MallocExtension::GetStats();
    const std::string pbtxt = GetStatsInPbTxt();

    if (using_hpaa(buf)) {
      EXPECT_THAT(buf, HasSubstr(R"(PARAMETER hpaa_subrelease 1)"));
    }
    EXPECT_THAT(
        buf,
        HasSubstr(R"(PARAMETER tcmalloc_huge_cache_demand_based_release 1)"));
    EXPECT_THAT(buf,
                HasSubstr(R"(PARAMETER tcmalloc_guarded_sample_parameter 50)"));
    EXPECT_THAT(
        buf,
        HasSubstr(
            R"(PARAMETER desired_usage_limit_bytes 18446744073709551615)"));
    EXPECT_THAT(buf, HasSubstr(R"(PARAMETER tcmalloc_per_cpu_caches 1)"));
    EXPECT_THAT(
        buf, HasSubstr(R"(PARAMETER tcmalloc_max_per_cpu_cache_size 3145728)"));
    EXPECT_THAT(
        buf, HasSubstr(
                 R"(PARAMETER tcmalloc_max_total_thread_cache_bytes 4194304)"));
    EXPECT_THAT(
        buf,
        HasSubstr(
            R"(PARAMETER tcmalloc_skip_subrelease_short_interval 2m0.25s)"));
    EXPECT_THAT(
        buf,
        HasSubstr(
            R"(PARAMETER tcmalloc_skip_subrelease_long_interval 3m0.375s)"));
    EXPECT_THAT(
        buf,
        HasSubstr(
            R"(PARAMETER tcmalloc_cache_demand_release_short_interval 3m0.25s)"));
    EXPECT_THAT(
        buf,
        HasSubstr(
            R"(PARAMETER tcmalloc_cache_demand_release_long_interval 4m0.375s)"));
    if (using_hpaa(buf)) {
      EXPECT_THAT(pbtxt, HasSubstr(R"(using_hpaa_subrelease: true)"));
    }
    EXPECT_THAT(pbtxt, HasSubstr(R"(guarded_sample_parameter: 50)"));
    EXPECT_THAT(pbtxt, HasSubstr(R"(desired_usage_limit_bytes: -1)"));
    EXPECT_THAT(pbtxt, HasSubstr(R"(hard_usage_limit_bytes: -1)"));
    EXPECT_THAT(pbtxt, HasSubstr(R"(tcmalloc_per_cpu_caches: true)"));
    EXPECT_THAT(pbtxt,
                HasSubstr(R"(tcmalloc_max_per_cpu_cache_size: 3145728)"));
    EXPECT_THAT(pbtxt,
                HasSubstr(R"(tcmalloc_max_total_thread_cache_bytes: 4194304)"));
    EXPECT_THAT(
        pbtxt,
        HasSubstr(
            R"(tcmalloc_skip_subrelease_short_interval_ns: 120250000000)"));
    EXPECT_THAT(
        pbtxt,
        HasSubstr(
            R"(tcmalloc_skip_subrelease_long_interval_ns: 180375000000)"));
    EXPECT_THAT(
        pbtxt,
        HasSubstr(
            R"(tcmalloc_cache_demand_release_short_interval_ns: 180250000000)"));
    EXPECT_THAT(
        pbtxt,
        HasSubstr(
            R"(tcmalloc_cache_demand_release_long_interval_ns: 240375000000)"));
    EXPECT_THAT(pbtxt, HasSubstr(R"(min_hot_access_hint: 3)"));
  }
}

TEST_F(GetStatsTest, StackDepth) {
  // We run a thread with a limited stack size to confirm that we do not use too
  // much stack space gathering statistics.
  //
  // Running out of stack space will manifest as a segmentation fault.
  const size_t max_stack_depth = std::max<size_t>(60 * 1024, PTHREAD_STACK_MIN);

  struct Args {
    bool plaintext;
    std::string output;
  };

  Args args;

  // We use raw pthreads to have control of the stack size.
  pthread_t stats_thread;
  pthread_attr_t thread_attributes;

  ASSERT_EQ(pthread_attr_init(&thread_attributes), 0);
  ASSERT_EQ(pthread_attr_setstacksize(&thread_attributes, max_stack_depth), 0);

  auto get_stats = +[](void* arg) {
    Args* args = static_cast<Args*>(arg);

    if (args->plaintext) {
      args->output = MallocExtension::GetStats();
    } else {
      args->output = GetStatsInPbTxt();
    }
    return static_cast<void*>(nullptr);
  };

  for (auto plaintext : {false, true}) {
    SCOPED_TRACE(absl::StrCat("plaintext: ", plaintext));

    args.plaintext = plaintext;
    args.output.clear();
    ASSERT_EQ(
        pthread_create(&stats_thread, &thread_attributes, get_stats, &args), 0);
    ASSERT_EQ(pthread_join(stats_thread, nullptr), 0);
#if !(defined(ABSL_HAVE_ADDRESS_SANITIZER) || \
      defined(ABSL_HAVE_LEAK_SANITIZER) ||    \
      defined(ABSL_HAVE_MEMORY_SANITIZER) ||  \
      defined(ABSL_HAVE_THREAD_SANITIZER))
    // We won't have data if we are running under a sanitizer, but everything
    // should run cleanly.
    EXPECT_FALSE(args.output.empty());
#endif
  }

  ASSERT_EQ(pthread_attr_destroy(&thread_attributes), 0);
}

TEST_F(GetStatsTest, SelSan) {
  std::string buf = MallocExtension::GetStats();
  std::string pbtxt = GetStatsInPbTxt();
#ifndef TCMALLOC_INTERNAL_SELSAN
  std::transform(buf.begin(), buf.end(), buf.begin(), tolower);
  std::transform(pbtxt.begin(), pbtxt.end(), pbtxt.begin(), tolower);
  EXPECT_THAT(buf, Not(HasSubstr("selsan")));
  EXPECT_THAT(pbtxt, Not(HasSubstr("selsan")));
#else   // #ifndef TCMALLOC_INTERNAL_SELSAN
  EXPECT_THAT(buf, ContainsRegex(R"(
SelSan Status
------------------------------------------------
Enabled: (0|1)
)"));
  EXPECT_THAT(pbtxt,
              ContainsRegex(R"(selsan { status: SELSAN_(ENABLED|DISABLED)})"));
#endif  // #ifndef TCMALLOC_INTERNAL_SELSAN
}

TEST_F(GetStatsTest, RequiredBufferSizes) {
  if (&MallocExtension_Internal_GetStatsInPbtxt == nullptr) {
    GTEST_SKIP() << "Not linked against malloc";
  }

  std::string stats;
  const int kLargeBufferSize = 3 << 20;
  const int kSmallBufferSize = 16 << 10;
  std::string buf;
  buf.resize(kLargeBufferSize);

  const int actual_large =
      MallocExtension_Internal_GetStatsInPbtxt(&buf[0], kLargeBufferSize);
  // This should be enough space.
  EXPECT_LE(actual_large, kLargeBufferSize);

  const int actual_small =
      MallocExtension_Internal_GetStatsInPbtxt(&buf[0], kSmallBufferSize);
  // But the small buffer will probably overflow.  Relax test if we don't have
  // per-CPU caches or HPAA.
  if (MallocExtension::PerCpuCachesActive() ||
      MallocExtension::GetNumericProperty("tcmalloc.page_algorithm")
              .value_or(0) == 1) {
    EXPECT_GT(actual_small, kSmallBufferSize);
  }

  // The required bytes should be similar.
  EXPECT_LE(std::abs(actual_large - actual_small), 1000);
}

}  // namespace
}  // namespace tcmalloc
