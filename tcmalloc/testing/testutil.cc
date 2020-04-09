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
// A few routines that are useful for multiple tests in this directory.

#include "tcmalloc/testing/testutil.h"

#include <pthread.h>
#include <stdlib.h>
#include <sys/resource.h>

#include <cstring>
#include <limits>

#define SAFE_PTHREAD(fncall)  do { if ((fncall) != 0) abort(); } while (0)

extern "C" {
  struct FunctionAndId {
    void (*ptr_to_function)(int);
    int id;
  };

// This helper function has the signature that pthread_create wants.
  static void* RunFunctionInThread(void *ptr_to_ptr_to_fn) {
    (**static_cast<void (**)()>(ptr_to_ptr_to_fn))();    // runs fn
    return nullptr;
  }

  static void* RunFunctionInThreadWithId(void *ptr_to_fnid) {
    FunctionAndId* fn_and_id = static_cast<FunctionAndId*>(ptr_to_fnid);
    (*fn_and_id->ptr_to_function)(fn_and_id->id);   // runs fn
    return nullptr;
  }

  // Run a function in a thread of its own and wait for it to finish.
  // This is useful for tcmalloc testing, because each thread is
  // handled separately in tcmalloc, so there's interesting stuff to
  // test even if the threads are not running concurrently.
  void RunThread(void (*fn)()) {
    pthread_t thr;
    // Even though fn is on the stack, it's safe to pass a pointer to it,
    // because we pthread_join immediately (ie, before RunInThread exits).
    SAFE_PTHREAD(pthread_create(&thr, nullptr, RunFunctionInThread, &fn));
    SAFE_PTHREAD(pthread_join(thr, nullptr));
  }

  void RunManyThreads(void (*fn)(), int count) {
    pthread_t* thr = new pthread_t[count];
    for (int i = 0; i < count; i++) {
      SAFE_PTHREAD(pthread_create(&thr[i], nullptr, RunFunctionInThread, &fn));
    }
    for (int i = 0; i < count; i++) {
      SAFE_PTHREAD(pthread_join(thr[i], nullptr));
    }
    delete[] thr;
  }

  void RunManyThreadsWithId(void (*fn)(int), int count, int stacksize) {
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, stacksize);

    pthread_t* thr = new pthread_t[count];
    FunctionAndId* fn_and_ids = new FunctionAndId[count];
    for (int i = 0; i < count; i++) {
      fn_and_ids[i].ptr_to_function = fn;
      fn_and_ids[i].id = i;
      SAFE_PTHREAD(pthread_create(&thr[i], &attr,
                                  RunFunctionInThreadWithId, &fn_and_ids[i]));
    }
    for (int i = 0; i < count; i++) {
      SAFE_PTHREAD(pthread_join(thr[i], nullptr));
    }
    delete[] fn_and_ids;
    delete[] thr;

    pthread_attr_destroy(&attr);
  }
}


// When compiled 64-bit and run on systems with swap several unittests will end
// up trying to consume all of RAM+swap, and that can take quite some time.  By
// limiting the address-space size we get sufficient coverage without blowing
// out job limits.
void SetTestResourceLimit() {

  // The actual resource we need to set varies depending on which flavour of
  // unix.  On Linux we need RLIMIT_AS because that covers the use of mmap.
  // Otherwise hopefully RLIMIT_RSS is good enough.  (Unfortunately 64-bit
  // and 32-bit headers disagree on the type of these constants!)
#ifdef RLIMIT_AS
#define USE_RESOURCE RLIMIT_AS
#else
#define USE_RESOURCE RLIMIT_RSS
#endif

  // Restrict the test to 8GiB by default.
  // Be careful we don't overflow rlim - if we would, this is a no-op
  // and we can just do nothing.
  const int64_t lim = static_cast<int64_t>(8) * 1024 * 1024 * 1024;
  if (lim > std::numeric_limits<rlim_t>::max()) return;
  const rlim_t kMaxMem = lim;

  struct rlimit rlim;
  if (getrlimit(USE_RESOURCE, &rlim) == 0) {
    if (rlim.rlim_cur == RLIM_INFINITY || rlim.rlim_cur > kMaxMem) {
      rlim.rlim_cur = kMaxMem;
      setrlimit(USE_RESOURCE, &rlim); // ignore result
    }
  }
}

namespace tcmalloc {

std::string GetStatsInPbTxt() {
  // When huge page telemetry is enabled, the output can become very large.
  const int buffer_length = 3 << 20;
  std::string buf;
  buf.resize(buffer_length);
  int actual_size =
      MallocExtension_Internal_GetStatsInPbtxt(&buf[0], buffer_length);
  buf.resize(actual_size);
  return buf;
}

}  // namespace tcmalloc
