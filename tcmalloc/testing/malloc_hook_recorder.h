// Copyright 2025 The TCMalloc Authors
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

#ifndef TCMALLOC_TESTING_MALLOC_HOOK_RECORDER_H_
#define TCMALLOC_TESTING_MALLOC_HOOK_RECORDER_H_

#include <stddef.h>
#include <sys/types.h>

#include <iosfwd>
#include <vector>

#include "tcmalloc/malloc_hook.h"

namespace tcmalloc {
namespace tcmalloc_testing {

class MallocHookRecorder {
 public:
  explicit MallocHookRecorder(int log_size = 100, bool overflow = false);
  ~MallocHookRecorder();

  enum Type { kNew, kDelete, kMmap, kMremap, kMunmap, kSbrk };
  enum Caller { kError = -3, kOther = -2, kAny = -1, kTCMalloc = 1 };

  struct CallEntry {
    Type type;
    const void* ptr;
    // requested size for allocations, provided size for deallocations (possibly
    // nullopt)
    std::optional<size_t> requested_size;
    size_t allocated_size;
    HookMemoryMutable is_mutable;
    Caller caller;

    // mmap
    const void* address;
    int protection;
    int flags;
    int fd;
    off_t offset;

    // mremap
    size_t old_size;
    const void* new_address;
  };

  void Restart();
  void Stop();

  std::vector<CallEntry> ConsumeLog(bool include_mmap);

 private:
  void AddLog(CallEntry entry);
  static void GlobalNewHook(const MallocHook::NewInfo& info);
  static void GlobalDeleteHook(const MallocHook::DeleteInfo& info);

  const bool overflow_;
  bool enabled_ = true;
  size_t head_ = 0;
  std::vector<CallEntry> log_;
  static thread_local MallocHookRecorder* tls_hook_;
};

bool operator==(const MallocHookRecorder::CallEntry& lhs,
                const MallocHookRecorder::CallEntry& rhs);

std::ostream& operator<<(std::ostream& stream,
                         const MallocHookRecorder::CallEntry& lhs);

}  // namespace tcmalloc_testing
}  // namespace tcmalloc

#endif  // TCMALLOC_TESTING_MALLOC_HOOK_RECORDER_H_
