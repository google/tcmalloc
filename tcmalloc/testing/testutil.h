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

#ifndef TCMALLOC_TESTING_TESTUTIL_H_
#define TCMALLOC_TESTING_TESTUTIL_H_

#include "tcmalloc/malloc_extension.h"

// Run a function in a thread of its own and wait for it to finish.
// The function you pass in must have the signature
//    void MyFunction();
extern "C" void RunThread(void (*fn)());

// Run a function X times, in X threads, and wait for them all to finish.
// The function you pass in must have the signature
//    void MyFunction();
extern "C" void RunManyThreads(void (*fn)(), int count);

// The 'advanced' version: run a function X times, in X threads, and
// wait for them all to finish.  Give them all the specified stack-size.
// (If you're curious why this takes a stacksize and the others don't,
// it's because the one client of this fn wanted to specify stacksize. :-) )
// The function you pass in must have the signature
//    void MyFunction(int idx);
// where idx is the index of the thread (which of the X threads this is).
extern "C" void RunManyThreadsWithId(void (*fn)(int), int count, int stacksize);

// When compiled 64-bit and run on systems with swap several unittests will end
// up trying to consume all of RAM+swap, and that can take quite some time.  By
// limiting the address-space size we get sufficient coverage without blowing
// out job limits.
void SetTestResourceLimit();

namespace tcmalloc {

// Get the TCMalloc stats in textproto format.
std::string GetStatsInPbTxt();
extern "C" ABSL_ATTRIBUTE_WEAK int MallocExtension_Internal_GetStatsInPbtxt(
    char *buffer, int buffer_length);

class ScopedProfileSamplingRate {
 public:
  explicit ScopedProfileSamplingRate(int64_t temporary_value)
      : previous_(MallocExtension::GetProfileSamplingRate()) {
    MallocExtension::SetProfileSamplingRate(temporary_value);
    // Reset the per-thread sampler.  It may have a very large gap if sampling
    // had been disabled.
    ::operator delete(::operator new(256 * 1024 * 1024));
  }

  ~ScopedProfileSamplingRate() {
    MallocExtension::SetProfileSamplingRate(previous_);
    ::operator delete(::operator new(256 * 1024 * 1024));
  }

 private:
  int64_t previous_;
};

class ScopedGuardedSamplingRate {
 public:
  explicit ScopedGuardedSamplingRate(int64_t temporary_value)
      : previous_(MallocExtension::GetGuardedSamplingRate()) {
    MallocExtension::SetGuardedSamplingRate(temporary_value);
  }

  ~ScopedGuardedSamplingRate() {
    MallocExtension::SetGuardedSamplingRate(previous_);
  }

 private:
  int64_t previous_;
};

}  // namespace tcmalloc

#endif  // TCMALLOC_TESTING_TESTUTIL_H_
