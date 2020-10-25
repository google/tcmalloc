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
//
// Checks basic properties of the sampler

#include "tcmalloc/sampler.h"

#include <math.h>
#include <stddef.h>
#include <stdlib.h>

#include <algorithm>
#include <cstdint>
#include <new>
#include <string>
#include <type_traits>
#include <vector>

#include "absl/strings/str_cat.h"
#include "gtest/gtest.h"
#include "tcmalloc/malloc_extension.h"
#include "tcmalloc/testing/testutil.h"

// Back-door so we can access Sampler internals.
namespace tcmalloc {

class SamplerTest {
 public:
  static void Init(Sampler* s, uint64_t seed) { s->Init(seed); }
};

namespace {

// Note that these tests are stochastic.
// This mean that the chance of correct code passing the test is,
// in the case of 5 standard deviations:
// kSigmas=5:    ~99.99994267%
// in the case of 4 standard deviations:
// kSigmas=4:    ~99.993666%
static const double kSigmas = 4;
static const size_t kSamplingInterval =
    MallocExtension::GetProfileSamplingRate();
static const size_t kGuardedSamplingInterval = 100 * kSamplingInterval;

// Tests that GetSamplePeriod returns the expected value
// which is 1<<19
TEST(Sampler, TestGetSamplePeriod) {
  tcmalloc::Sampler sampler;
  SamplerTest::Init(&sampler, 1);
  uint64_t sample_period;
  sample_period = sampler.GetSamplePeriod();
  EXPECT_GT(sample_period, 0);
}

// Tests of the quality of the random numbers generated
// This uses the Anderson Darling test for uniformity.
// See "Evaluating the Anderson-Darling Distribution" by Marsaglia
// for details.

// Short cut version of ADinf(z), z>0 (from Marsaglia)
// This returns the p-value for Anderson Darling statistic in
// the limit as n-> infinity. For finite n, apply the error fix below.
double AndersonDarlingInf(double z) {
  if (z < 2) {
    return exp(-1.2337141 / z) / sqrt(z) *
           (2.00012 +
            (0.247105 -
             (0.0649821 - (0.0347962 - (0.011672 - 0.00168691 * z) * z) * z) *
                 z) *
                z);
  }
  return exp(
      -exp(1.0776 -
           (2.30695 -
            (0.43424 - (0.082433 - (0.008056 - 0.0003146 * z) * z) * z) * z) *
               z));
}

// Corrects the approximation error in AndersonDarlingInf for small values of n
// Add this to AndersonDarlingInf to get a better approximation
// (from Marsaglia)
double AndersonDarlingErrFix(int n, double x) {
  if (x > 0.8) {
    return (-130.2137 +
            (745.2337 -
             (1705.091 - (1950.646 - (1116.360 - 255.7844 * x) * x) * x) * x) *
                x) /
           n;
  }
  double cutoff = 0.01265 + 0.1757 / n;
  double t;
  if (x < cutoff) {
    t = x / cutoff;
    t = sqrt(t) * (1 - t) * (49 * t - 102);
    return t * (0.0037 / (n * n) + 0.00078 / n + 0.00006) / n;
  } else {
    t = (x - cutoff) / (0.8 - cutoff);
    t = -0.00022633 +
        (6.54034 - (14.6538 - (14.458 - (8.259 - 1.91864 * t) * t) * t) * t) *
            t;
    return t * (0.04213 + 0.01365 / n) / n;
  }
}

// Returns the AndersonDarling p-value given n and the value of the statistic
double AndersonDarlingPValue(int n, double z) {
  double ad = AndersonDarlingInf(z);
  double errfix = AndersonDarlingErrFix(n, ad);
  return ad + errfix;
}

double AndersonDarlingStatistic(const std::vector<double>& random_sample) {
  int n = random_sample.size();
  double ad_sum = 0;
  for (int i = 0; i < n; i++) {
    ad_sum += (2 * i + 1) *
              std::log(random_sample[i] * (1 - random_sample[n - 1 - i]));
  }
  double ad_statistic = -n - 1 / static_cast<double>(n) * ad_sum;
  return ad_statistic;
}

// Tests if the array of doubles is uniformly distributed.
// Returns the p-value of the Anderson Darling Statistic
// for the given set of sorted random doubles
// See "Evaluating the Anderson-Darling Distribution" by
// Marsaglia and Marsaglia for details.
double AndersonDarlingTest(const std::vector<double>& random_sample) {
  double ad_statistic = AndersonDarlingStatistic(random_sample);
  double p = AndersonDarlingPValue(random_sample.size(), ad_statistic);
  return p;
}

// Testing that NextRandom generates uniform
// random numbers.
// Applies the Anderson-Darling test for uniformity
void TestNextRandom(int n) {
  tcmalloc::Sampler sampler;
  SamplerTest::Init(&sampler, 1);
  uint64_t x = 1;
  // This assumes that the prng returns 48 bit numbers
  uint64_t max_prng_value = static_cast<uint64_t>(1) << 48;
  // Initialize
  for (int i = 1; i <= 20; i++) {  // 20 mimics sampler.Init()
    x = sampler.NextRandom(x);
  }
  std::vector<uint64_t> int_random_sample(n);
  // Collect samples
  for (int i = 0; i < n; i++) {
    int_random_sample[i] = x;
    x = sampler.NextRandom(x);
  }
  // First sort them...
  std::sort(int_random_sample.begin(), int_random_sample.end());
  std::vector<double> random_sample(n);
  // Convert them to uniform randoms (in the range [0,1])
  for (int i = 0; i < n; i++) {
    random_sample[i] =
        static_cast<double>(int_random_sample[i]) / max_prng_value;
  }
  // Now compute the Anderson-Darling statistic
  double ad_pvalue = AndersonDarlingTest(random_sample);
  EXPECT_GT(std::min(ad_pvalue, 1 - ad_pvalue), 0.0001)
      << "prng is not uniform: n = " << n << " p = " << ad_pvalue;
}

TEST(Sampler, TestNextRandom_MultipleValues) {
  TestNextRandom(10);  // Check short-range correlation
  TestNextRandom(100);
  TestNextRandom(1000);
  TestNextRandom(10000);  // Make sure there's no systematic error
}

void TestSampleAndersonDarling(int sample_period,
                               std::vector<uint64_t>* sample) {
  // First sort them...
  std::sort(sample->begin(), sample->end());
  int n = sample->size();
  std::vector<double> random_sample(n);
  // Convert them to uniform random numbers
  // by applying the geometric CDF
  for (int i = 0; i < n; i++) {
    random_sample[i] =
        1 - exp(-static_cast<double>((*sample)[i]) / sample_period);
  }
  // Now compute the Anderson-Darling statistic
  double geom_ad_pvalue = AndersonDarlingTest(random_sample);
  EXPECT_GT(std::min(geom_ad_pvalue, 1 - geom_ad_pvalue), 0.0001)
      << "PickNextSamplingPoint does not produce good "
         "geometric/exponential random numbers "
         "n = "
      << n << " p = " << geom_ad_pvalue;
}

// Tests that PickNextSamplePeriod generates
// geometrically distributed random numbers.
// First converts to uniforms then applied the
// Anderson-Darling test for uniformity.
void TestPickNextSample(int n) {
  tcmalloc::Sampler sampler;
  SamplerTest::Init(&sampler, 1);
  std::vector<uint64_t> int_random_sample(n);
  int sample_period = sampler.GetSamplePeriod();
  int ones_count = 0;
  for (int i = 0; i < n; i++) {
    int_random_sample[i] = sampler.PickNextSamplingPoint();
    EXPECT_GE(int_random_sample[i], 1);
    if (int_random_sample[i] == 1) {
      ones_count += 1;
    }
    EXPECT_LT(ones_count, 4) << " out of " << i << " samples.";
  }
  TestSampleAndersonDarling(sample_period, &int_random_sample);
}

TEST(Sampler, TestPickNextSample_MultipleValues) {
  TestPickNextSample(10);  // Make sure the first few are good (enough)
  TestPickNextSample(100);
  TestPickNextSample(1000);
  TestPickNextSample(10000);  // Make sure there's no systematic error
}

void TestPickNextGuardedSample(int n) {
  tcmalloc::Sampler sampler;
  SamplerTest::Init(&sampler, 1);
  std::vector<uint64_t> int_random_sample(n);
  for (int i = 0; i < n; i++) {
    int_random_sample[i] = 1 + sampler.PickNextGuardedSamplingPoint();
    ASSERT_GE(int_random_sample[i], 1);
  }
  TestSampleAndersonDarling(kGuardedSamplingInterval / kSamplingInterval,
                            &int_random_sample);
}

TEST(Sampler, TestPickNextGuardedSample_MultipleValues) {
  ScopedGuardedSamplingRate s(kGuardedSamplingInterval);

  TestPickNextGuardedSample(10);  // Make sure the first few are good (enough)
  TestPickNextGuardedSample(100);
  TestPickNextGuardedSample(1000);
  TestPickNextGuardedSample(10000);  // Make sure there's no systematic error
}

// Further tests

double StandardDeviationsErrorInSample(int total_samples, int picked_samples,
                                       int alloc_size, int sampling_interval) {
  double p = 1 - exp(-(static_cast<double>(alloc_size) / sampling_interval));
  double expected_samples = total_samples * p;
  double sd = pow(p * (1 - p) * total_samples, 0.5);
  return ((picked_samples - expected_samples) / sd);
}

TEST(Sampler, LargeAndSmallAllocs_CombinedTest) {
  tcmalloc::Sampler sampler;
  SamplerTest::Init(&sampler, 1);
  int counter_big = 0;
  int counter_small = 0;
  int size_big = 129 * 8 * 1024 + 1;
  int size_small = 1024 * 8;
  int num_iters = 128 * 4 * 8;
  // Allocate in mixed chunks
  for (int i = 0; i < num_iters; i++) {
    if (sampler.RecordAllocation(size_big)) {
      counter_big += 1;
    }
    for (int i = 0; i < 129; i++) {
      if (sampler.RecordAllocation(size_small)) {
        counter_small += 1;
      }
    }
  }
  // Now test that there are the right number of each
  double large_allocs_sds = StandardDeviationsErrorInSample(
      num_iters, counter_big, size_big, kSamplingInterval);
  double small_allocs_sds = StandardDeviationsErrorInSample(
      num_iters * 129, counter_small, size_small, kSamplingInterval);
  ASSERT_LE(fabs(large_allocs_sds), kSigmas) << large_allocs_sds;
  ASSERT_LE(fabs(small_allocs_sds), kSigmas) << small_allocs_sds;
}

TEST(Sampler, TestShouldSampleGuardedAllocation) {
  ScopedGuardedSamplingRate s(kGuardedSamplingInterval);

  tcmalloc::Sampler sampler;
  SamplerTest::Init(&sampler, 1);
  int counter = 0;
  int num_iters = 10000;
  for (int i = 0; i < num_iters; i++) {
    if (sampler.ShouldSampleGuardedAllocation()) {
      counter++;
    }
  }
  double sd = StandardDeviationsErrorInSample(
      num_iters, counter, /*alloc_size=*/1,
      kGuardedSamplingInterval / kSamplingInterval);
  EXPECT_LE(fabs(sd), kSigmas);
}

template <typename Body>
void DoCheckMean(size_t mean, int num_samples, Body next_sampling_point) {
  size_t total = 0;
  for (int i = 0; i < num_samples; i++) {
    total += next_sampling_point();
  }
  double empirical_mean = total / static_cast<double>(num_samples);
  double expected_sd = mean / pow(num_samples * 1.0, 0.5);
  EXPECT_LT(fabs(mean - empirical_mean), expected_sd * kSigmas);
}

void CheckMean(size_t mean, int num_samples, bool guarded) {
  tcmalloc::Sampler sampler;
  SamplerTest::Init(&sampler, 1);
  DoCheckMean(mean, num_samples, [guarded, &sampler]() {
    if (guarded) {
      return sampler.PickNextGuardedSamplingPoint();
    } else {
      return sampler.PickNextSamplingPoint();
    }
  });
}

// Tests whether the mean is about right over 1000 samples
TEST(Sampler, IsMeanRight) {
  ScopedGuardedSamplingRate s(kGuardedSamplingInterval);

  CheckMean(kSamplingInterval, 1000, /*guarded=*/false);
  CheckMean(kGuardedSamplingInterval / kSamplingInterval, 1000,
            /*guarded=*/true);
}

// This checks that the stated maximum value for the sampling rate never
// overflows bytes_until_sample_
TEST(Sampler, bytes_until_sample_Overflow_Underflow) {
  tcmalloc::Sampler sampler;
  SamplerTest::Init(&sampler, 1);
  uint64_t one = 1;
  // sample_rate = 0;  // To test the edge case
  uint64_t sample_rate_array[4] = {0, 1, one << 19, one << 58};
  for (int i = 0; i < 4; i++) {
    uint64_t sample_rate = sample_rate_array[i];
    SCOPED_TRACE(sample_rate);

    double sample_scaling = -std::log(2.0) * sample_rate;
    // Take the top 26 bits as the random number
    // (This plus the 1<<26 sampling bound give a max step possible of
    // 1209424308 bytes.)
    const uint64_t prng_mod_power = 48;  // Number of bits in prng

    // First, check the largest_prng value
    uint64_t largest_prng_value = (static_cast<uint64_t>(1) << 48) - 1;
    double q = (largest_prng_value >> (prng_mod_power - 26)) + 1.0;
    uint64_t smallest_sample_step =
        1 + static_cast<uint64_t>((std::log2(q) - 26) * sample_scaling);
    uint64_t cutoff =
        static_cast<uint64_t>(10) * (sample_rate / (one << 24) + 1);
    // This checks that the answer is "small" and positive
    ASSERT_LE(smallest_sample_step, cutoff);

    // Next, check with the smallest prng value
    uint64_t smallest_prng_value = 0;
    q = (smallest_prng_value >> (prng_mod_power - 26)) + 1.0;
    uint64_t largest_sample_step =
        1 + static_cast<uint64_t>((std::log2(q) - 26) * sample_scaling);
    ASSERT_LE(largest_sample_step, one << 63);
    ASSERT_GE(largest_sample_step, smallest_sample_step);
  }
}

// Test that NextRand is in the right range.  Unfortunately, this is a
// stochastic test which could miss problems.
TEST(Sampler, NextRand_range) {
  tcmalloc::Sampler sampler;
  SamplerTest::Init(&sampler, 1);
  uint64_t one = 1;
  // The next number should be (one << 48) - 1
  uint64_t max_value = (one << 48) - 1;
  uint64_t x = (one << 55);
  int n = 22;                            // 27;
  for (int i = 1; i <= (1 << n); i++) {  // 20 mimics sampler.Init()
    x = sampler.NextRandom(x);
    ASSERT_LE(x, max_value);
  }
}

// Tests certain arithmetic operations to make sure they compute what we
// expect them too (for testing across different platforms)
TEST(Sampler, arithmetic_1) {
  tcmalloc::Sampler sampler;
  SamplerTest::Init(&sampler, 1);
  uint64_t rnd;  // our 48 bit random number, which we don't trust
  const uint64_t prng_mod_power = 48;
  uint64_t one = 1;
  rnd = one;
  uint64_t max_value = (one << 48) - 1;
  for (int i = 1; i <= (1 << 27); i++) {  // 20 mimics sampler.Init()
    rnd = sampler.NextRandom(rnd);
    ASSERT_LE(rnd, max_value);
    double q = (rnd >> (prng_mod_power - 26)) + 1.0;
    ASSERT_GE(q, 0) << rnd << "  " << prng_mod_power;
  }
  // Test some potentially out of bounds value for rnd
  for (int i = 1; i < 64; i++) {
    rnd = one << i;
    double q = (rnd >> (prng_mod_power - 26)) + 1.0;
    ASSERT_GE(q, 0) << " rnd=" << rnd << "  i=" << i << " prng_mod_power"
                    << prng_mod_power;
  }
}

// Tests certain arithmetic operations to make sure they compute what we
// expect them too (for testing across different platforms)
// know bad values under with -c dbg --cpu piii for _some_ binaries:
// rnd=227453640600554
// shifted_rnd=54229173
// (hard to reproduce)
TEST(Sampler, arithmetic_2) {
  uint64_t rnd{227453640600554};

  const uint64_t prng_mod_power = 48;  // Number of bits in prng
  uint64_t shifted_rnd = rnd >> (prng_mod_power - 26);
  ASSERT_LT(shifted_rnd, (1 << 26));
  ASSERT_GE(static_cast<double>(static_cast<uint32_t>(shifted_rnd)), 0)
      << " rnd=" << rnd << "  srnd=" << shifted_rnd;
  ASSERT_GE(static_cast<double>(shifted_rnd), 0)
      << " rnd=" << rnd << "  srnd=" << shifted_rnd;
  double q = static_cast<double>(shifted_rnd) + 1.0;
  ASSERT_GT(q, 0);
}

// It's not really a test, but it's good to know
TEST(Sampler, size_of_class) {
  tcmalloc::Sampler sampler;
  SamplerTest::Init(&sampler, 1);
  EXPECT_LE(sizeof(sampler), 48);
}

TEST(Sampler, stirring) {
  // Lets test that we get somewhat random values from sampler even when we're
  // dealing with Samplers that have same addresses, as we see when thread's TLS
  // areas are reused. b/117296263

  absl::aligned_storage_t<sizeof(tcmalloc::Sampler), alignof(tcmalloc::Sampler)>
      place;

  DoCheckMean(kSamplingInterval, 1000, [&place]() {
    tcmalloc::Sampler* sampler = new (&place) tcmalloc::Sampler;
    // Sampler constructor just 0-initializes
    // everything. RecordAllocation really makes sampler initialize
    // itself.
    sampler->RecordAllocation(1);
    // And then we probe sampler's (second) value.
    size_t retval = sampler->PickNextSamplingPoint();
    sampler->tcmalloc::Sampler::~Sampler();
    return retval;
  });
}

// Tests that the weights returned by RecordAllocation match the sampling rate.
TEST(Sampler, weight_distribution) {
  static constexpr size_t sizes[] = {
      0, 1, 8, 198, 1024, 1152, 3712, 1 << 16, 1 << 25, 50 << 20, 1 << 30};

  for (auto size : sizes) {
    SCOPED_TRACE(size);

    tcmalloc::Sampler s;
    SamplerTest::Init(&s, 1);

    static constexpr int kSamples = 10000;
    double expected =
        (size + 1) / (1.0 - exp(-1.0 * (size + 1) / s.GetSamplePeriod()));
    // Since each sample requires ~2MiB / size iterations, using fewer samples
    // for the small sizes makes this test run in ~2s vs. ~90s on Forge in 2019.
    DoCheckMean(expected, size < 256 ? 100 : kSamples, [size, &s]() {
      size_t weight = 0;
      while (!(weight = s.RecordAllocation(size))) {
      }
      return weight;
    });
  }
}

}  // namespace
}  // namespace tcmalloc
