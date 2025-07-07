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

#include "tcmalloc/testing/malloc_hook_recorder.h"

#include <cstddef>

#include "absl/base/optimization.h"

#define _GNU_SOURCE 1  // for mremap (in sys/mman.h)
#include <stdint.h>
#include <sys/mman.h>

#include <ostream>
#include <vector>

#include "absl/base/attributes.h"
#include "absl/base/macros.h"
#include "absl/debugging/stacktrace.h"
#include "tcmalloc/internal/logging.h"

ABSL_DEFINE_ATTRIBUTE_SECTION_VARS(google_malloc);
ABSL_DECLARE_ATTRIBUTE_SECTION_VARS(google_malloc);

#define ADDR_IN_ATTRIBUTE_SECTION(addr, name)                         \
  (reinterpret_cast<uintptr_t>(ABSL_ATTRIBUTE_SECTION_START(name)) <= \
       reinterpret_cast<uintptr_t>(addr) &&                           \
   reinterpret_cast<uintptr_t>(addr) <                                \
       reinterpret_cast<uintptr_t>(ABSL_ATTRIBUTE_SECTION_STOP(name)))

namespace tcmalloc {
namespace tcmalloc_testing {

static bool CheckSectionVars() {
#if defined(__clang__)
  static bool ok = [] {
    ABSL_INIT_ATTRIBUTE_SECTION_VARS(google_malloc);
    if (ABSL_ATTRIBUTE_SECTION_START(google_malloc) ==
        ABSL_ATTRIBUTE_SECTION_STOP(google_malloc)) {
      TC_LOG(
          "google_malloc section is missing, no caller information available");
      return false;
    }
    return true;
  }();
  return ok;
#else
  // GOOGLE_MALLOC_SECTION_BEGIN/GOOGLE_MALLOC_SECTION_END are no-ops on GCC.
  return false;
#endif
}

MallocHookRecorder::MallocHookRecorder(int log_size, bool overflow)
    : overflow_(overflow) {
  TC_CHECK_GE(log_size, 0);
  log_.reserve(log_size);
  TC_CHECK(!tls_hook_);
  TC_CHECK(
      tcmalloc::MallocHook::AddNewHook(&MallocHookRecorder::GlobalNewHook));
  TC_CHECK(tcmalloc::MallocHook::AddDeleteHook(
      &MallocHookRecorder::GlobalDeleteHook));
  tls_hook_ = this;
}

MallocHookRecorder::~MallocHookRecorder() {
  TC_CHECK_EQ(tls_hook_, this);
  tls_hook_ = nullptr;
  TC_CHECK(
      tcmalloc::MallocHook::RemoveNewHook(&MallocHookRecorder::GlobalNewHook));
  TC_CHECK(MallocHook::RemoveDeleteHook(&MallocHookRecorder::GlobalDeleteHook));
}

void MallocHookRecorder::Restart() {
  enabled_ = false;
  log_.clear();
  head_ = 0;
  enabled_ = true;
}

void MallocHookRecorder::Stop() { enabled_ = false; }

static bool IsMMap(MallocHookRecorder::Type t) {
  switch (t) {
    case MallocHookRecorder::kNew:
    case MallocHookRecorder::kDelete:
      return false;
    case MallocHookRecorder::kMmap:
    case MallocHookRecorder::kMremap:
    case MallocHookRecorder::kMunmap:
    case MallocHookRecorder::kSbrk:
      return true;
  }

  __builtin_unreachable();
  return false;
}

std::vector<MallocHookRecorder::CallEntry> MallocHookRecorder::ConsumeLog(
    bool include_mmap) {
  Stop();
  std::vector<CallEntry> log;
  log.reserve(log_.size());
  for (int i = head_; i < log_.size(); i++) {
    if (include_mmap || !IsMMap(log_[i].type)) {
      log.push_back(log_[i]);
    }
  }
  for (int i = 0; i < head_; i++) {
    if (include_mmap || !IsMMap(log_[i].type)) {
      log.push_back(log_[i]);
    }
  }
  log_.clear();
  head_ = 0;
  return log;
}

void MallocHookRecorder::AddLog(CallEntry entry) {
  if (CheckSectionVars()) {
    entry.caller = kOther;
    void* stack[32];
    int depth = absl::GetStackTrace(stack, ABSL_ARRAYSIZE(stack), 3);
    for (int i = depth - 1; i >= 0; --i) {
      if (ADDR_IN_ATTRIBUTE_SECTION(stack[i], google_malloc)) {
        entry.caller = kTCMalloc;
        break;
      }
    }
  } else {
    // GCC doesn't support
    // GOOGLE_MALLOC_SECTION_BEGIN/GOOGLE_MALLOC_SECTION_END.  Presume calls
    // came from TCMalloc.
#if defined(__clang__)
    entry.caller = kError;
#else
    entry.caller = kTCMalloc;
#endif
  }

  if (log_.size() < log_.capacity()) {
    log_.push_back(entry);
  } else {
    TC_CHECK(overflow_);
    log_[head_] = entry;
    if (++head_ == log_.size()) head_ = 0;
  }
}

void MallocHookRecorder::GlobalNewHook(const MallocHook::NewInfo& info) {
  if (tls_hook_ && tls_hook_->enabled_) {
    tls_hook_->AddLog({kNew, info.ptr, info.requested_size, info.allocated_size,
                       info.is_mutable});
  }
}

void MallocHookRecorder::GlobalDeleteHook(const MallocHook::DeleteInfo& info) {
  if (tls_hook_ && tls_hook_->enabled_) {
    tls_hook_->AddLog({kDelete, info.ptr, info.deallocated_size,
                       info.allocated_size, info.is_mutable});
  }
}

thread_local MallocHookRecorder* MallocHookRecorder::tls_hook_ = nullptr;

bool operator==(const MallocHookRecorder::CallEntry& lhs,
                const MallocHookRecorder::CallEntry& rhs) {
  if (lhs.type != rhs.type) return false;
  if (lhs.ptr != rhs.ptr || lhs.requested_size != rhs.requested_size ||
      lhs.allocated_size != rhs.allocated_size ||
      lhs.is_mutable != rhs.is_mutable) {
    return false;
  }
  if (lhs.caller != MallocHookRecorder::kAny &&
      rhs.caller != MallocHookRecorder::kAny && lhs.caller != rhs.caller) {
    return false;
  }
  if (lhs.type == MallocHookRecorder::kMmap) {
    if (lhs.address != rhs.address || lhs.protection != rhs.protection ||
        lhs.flags != rhs.flags || lhs.fd != rhs.fd ||
        lhs.offset != rhs.offset) {
      return false;
    }
  }
  if (lhs.type == MallocHookRecorder::kMremap) {
    if (lhs.address != rhs.address || lhs.flags != rhs.flags ||
        lhs.old_size != rhs.old_size || lhs.new_address != rhs.new_address) {
      return false;
    }
  }
  return true;
}

static const char* ToString(MallocHookRecorder::Caller caller) {
  switch (caller) {
    case MallocHookRecorder::kError:
      return "<error>";
    case MallocHookRecorder::kOther:
      return "other";
    case MallocHookRecorder::kAny:
      return "";
    case MallocHookRecorder::kTCMalloc:
      return "tcmalloc";
  }
  ABSL_UNREACHABLE();
}

static const char* ToString(HookMemoryMutable is_mutable) {
  switch (is_mutable) {
    case HookMemoryMutable::kMutable:
      return "Mutable";
    case HookMemoryMutable::kImmutable:
      return "Immutable";
  }
  ABSL_UNREACHABLE();
}

std::ostream& operator<<(std::ostream& stream,
                         const MallocHookRecorder::CallEntry& lhs) {
  switch (lhs.type) {
    case MallocHookRecorder::kNew:
      return stream << ToString(lhs.caller) << "::New(" << lhs.ptr << ", "
                    << *lhs.requested_size << ", " << lhs.allocated_size << ", "
                    << ToString(lhs.is_mutable) << ")";
    case MallocHookRecorder::kDelete:
      stream << ToString(lhs.caller) << "::Delete(" << lhs.ptr << ", ";

      if (lhs.requested_size.has_value()) {
        stream << "size = " << *lhs.requested_size << ", ";
      } else {
        stream << "(unsized deallocation), ";
      }
      return stream << lhs.allocated_size << ", " << ToString(lhs.is_mutable)
                    << ")";
    case MallocHookRecorder::kMmap:
      return stream << ToString(lhs.caller) << "::Mmap(" << lhs.ptr << ", "
                    << lhs.address << ", " << *lhs.requested_size << ", "
                    << lhs.protection << ", " << lhs.flags << ", " << lhs.fd
                    << ", " << lhs.offset << ")";
    case MallocHookRecorder::kMremap:
      return stream << ToString(lhs.caller) << "::Mremap(" << lhs.ptr << ", "
                    << lhs.address << ", " << lhs.old_size << ", "
                    << *lhs.requested_size << ", " << lhs.flags << ", "
                    << lhs.new_address << ")";
    case MallocHookRecorder::kMunmap:
      return stream << ToString(lhs.caller) << "::Munmap(" << lhs.ptr << ", "
                    << *lhs.requested_size << ")";
    case MallocHookRecorder::kSbrk:
      return stream << ToString(lhs.caller) << "::Sbrk(" << lhs.ptr << ", "
                    << static_cast<ptrdiff_t>(*lhs.requested_size) << ")";
  }
  ABSL_UNREACHABLE();
}

#undef ADDR_IN_ATTRIBUTE_SECTION

}  // namespace tcmalloc_testing
}  // namespace tcmalloc
