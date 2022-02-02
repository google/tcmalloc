// Copyright 2018 The Abseil Authors.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "tcmalloc/sampled_allocation_recorder.h"

#include <algorithm>
#include <atomic>
#include <random>
#include <vector>

#include "gmock/gmock.h"
#include "absl/synchronization/notification.h"
#include "absl/time/time.h"
#include "tcmalloc/explicitly_constructed.h"
#include "tcmalloc/testing/thread_manager.h"

namespace tcmalloc {
namespace tcmalloc_internal {
namespace {
using ::testing::IsEmpty;
using ::testing::UnorderedElementsAre;

struct Info : public Sample<Info> {
 public:
  Info() { PrepareForSampling(); }
  void PrepareForSampling() ABSL_EXCLUSIVE_LOCKS_REQUIRED(lock) {
    initialized = true;
  }
  std::atomic<size_t> size;
  absl::Time create_time;
  bool initialized;
};

class TestAllocator {
 public:
  static Info* New() { return new Info; }
  static void Delete(Info* info) { delete info; }
};

class SampleRecorderTest : public ::testing::Test {
 public:
  SampleRecorderTest() : sample_recorder_(&allocator_) {
    sample_recorder_.Init();
  }

  std::vector<size_t> GetSizes() {
    std::vector<size_t> res;
    sample_recorder_.Iterate([&](const Info& info) {
      res.push_back(info.size.load(std::memory_order_acquire));
    });
    return res;
  }

  Info* Register(size_t size) {
    auto* info = sample_recorder_.Register();
    assert(info != nullptr);
    info->size.store(size);
    return info;
  }

  TestAllocator allocator_;
  SampleRecorder<Info, TestAllocator> sample_recorder_;
};

// In static_vars.cc, we use tcmalloc/explicitly_constructed.h to
// set up the sample recorder. Have a test here to verify that it is properly
// initialized and functional through this approach.
TEST_F(SampleRecorderTest, ExplicitlyConstructed) {
  ExplicitlyConstructed<SampleRecorder<Info, TestAllocator>>
      sample_recorder_helper;
  sample_recorder_helper.Construct(&allocator_);
  SampleRecorder<Info, TestAllocator>& sample_recorder =
      sample_recorder_helper.get_mutable();
  sample_recorder.Init();

  Info* info = sample_recorder.Register();
  assert(info != nullptr);
  sample_recorder.Unregister(info);
}

// Check that the state modified by PrepareForSampling() is properly set.
TEST_F(SampleRecorderTest, PrepareForSampling) {
  Info* info1 = Register(1);
  // PrepareForSampling() is invoked in the constructor.
  EXPECT_TRUE(info1->initialized);
  info1->initialized = false;
  sample_recorder_.Unregister(info1);

  Info* info2 = Register(2);
  // We are reusing the sample, PrepareForSampling() is invoked in PopDead();
  EXPECT_TRUE(info2->initialized);
}

TEST_F(SampleRecorderTest, Registration) {
  auto* info1 = Register(1);
  EXPECT_THAT(GetSizes(), UnorderedElementsAre(1));

  auto* info2 = Register(2);
  EXPECT_THAT(GetSizes(), UnorderedElementsAre(1, 2));
  info1->size.store(3);
  EXPECT_THAT(GetSizes(), UnorderedElementsAre(3, 2));

  sample_recorder_.Unregister(info1);
  sample_recorder_.Unregister(info2);
}

TEST_F(SampleRecorderTest, Unregistration) {
  std::vector<Info*> infos;
  for (size_t i = 0; i < 3; ++i) {
    infos.push_back(Register(i));
  }
  EXPECT_THAT(GetSizes(), UnorderedElementsAre(0, 1, 2));

  sample_recorder_.Unregister(infos[1]);
  EXPECT_THAT(GetSizes(), UnorderedElementsAre(0, 2));

  infos.push_back(Register(3));
  infos.push_back(Register(4));
  EXPECT_THAT(GetSizes(), UnorderedElementsAre(0, 2, 3, 4));
  sample_recorder_.Unregister(infos[3]);
  EXPECT_THAT(GetSizes(), UnorderedElementsAre(0, 2, 4));

  sample_recorder_.Unregister(infos[0]);
  sample_recorder_.Unregister(infos[2]);
  sample_recorder_.Unregister(infos[4]);
  EXPECT_THAT(GetSizes(), IsEmpty());
}

TEST_F(SampleRecorderTest, MultiThreaded) {
  absl::Notification stop;
  ThreadManager threads;
  threads.Start(10, [&](int) {
    std::random_device rd;
    std::mt19937 gen(rd());

    std::vector<Info*> infoz;
    while (!stop.HasBeenNotified()) {
      if (infoz.empty()) {
        infoz.push_back(sample_recorder_.Register());
      }
      switch (std::uniform_int_distribution<>(0, 2)(gen)) {
        case 0: {
          infoz.push_back(sample_recorder_.Register());
          break;
        }
        case 1: {
          size_t p = std::uniform_int_distribution<>(0, infoz.size() - 1)(gen);
          Info* info = infoz[p];
          infoz[p] = infoz.back();
          infoz.pop_back();
          sample_recorder_.Unregister(info);
          break;
        }
        case 2: {
          absl::Duration oldest = absl::ZeroDuration();
          sample_recorder_.Iterate([&](const Info& info) {
            oldest = std::max(oldest, absl::Now() - info.create_time);
            ASSERT_TRUE(info.initialized);
          });
          ASSERT_GE(oldest, absl::ZeroDuration());
          break;
        }
      }
    }
  });
  // The threads will hammer away.  Give it a little bit of time for tsan to
  // spot errors.
  absl::SleepFor(absl::Seconds(3));
  stop.Notify();
  threads.Stop();
}

TEST_F(SampleRecorderTest, Callback) {
  auto* info1 = Register(1);
  auto* info2 = Register(2);

  static const Info* expected;

  auto callback = [](const Info& info) {
    // We can't use `info` outside of this callback because the object will be
    // disposed as soon as we return from here.
    EXPECT_EQ(&info, expected);
  };

  // Set the callback.
  EXPECT_EQ(sample_recorder_.SetDisposeCallback(callback), nullptr);
  expected = info1;
  sample_recorder_.Unregister(info1);

  // Unset the callback.
  EXPECT_EQ(callback, sample_recorder_.SetDisposeCallback(nullptr));
  expected = nullptr;  // no more calls.
  sample_recorder_.Unregister(info2);
}

}  // namespace
}  // namespace tcmalloc_internal
}  // namespace tcmalloc
