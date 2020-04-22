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

#include "tcmalloc/internal/logging.h"

#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syscall.h>
#include <unistd.h>

#include <algorithm>

#include "absl/base/attributes.h"
#include "absl/base/internal/spinlock.h"
#include "absl/base/macros.h"
#include "absl/debugging/stacktrace.h"
#include "tcmalloc/internal/parameter_accessors.h"
#include "tcmalloc/malloc_extension.h"

// Variables for storing crash output.  Allocated statically since we
// may not be able to heap-allocate while crashing.
static absl::base_internal::SpinLock crash_lock(
    absl::base_internal::kLinkerInitialized);
static bool crashed = false;

static const size_t kStatsBufferSize = 16 << 10;
static char stats_buffer[kStatsBufferSize] = { 0 };

namespace tcmalloc {

static void WriteMessage(const char* msg, int length) {
  syscall(SYS_write, STDERR_FILENO, msg, length);
}

void (*log_message_writer)(const char* msg, int length) = WriteMessage;


class Logger {
 public:
  bool Add(const LogItem& item);
  bool AddStr(const char* str, int n);
  bool AddNum(uint64_t num, int base);  // base must be 10 or 16.

  static constexpr int kBufSize = 200;
  char* p_;
  char* end_;
  char buf_[kBufSize];
};

ABSL_ATTRIBUTE_NOINLINE
void Log(LogMode mode, const char* filename, int line,
         LogItem a, LogItem b, LogItem c, LogItem d) {
  Logger state;
  state.p_ = state.buf_;
  state.end_ = state.buf_ + sizeof(state.buf_);
  state.AddStr(filename, strlen(filename))
      && state.AddStr(":", 1)
      && state.AddNum(line, 10)
      && state.AddStr("]", 1)
      && state.Add(a)
      && state.Add(b)
      && state.Add(c)
      && state.Add(d);

  const bool crash = (mode == kCrash || mode == kCrashWithStats);
  StackTrace t;
  if (crash || mode == kLogWithStack) {
    t.depth = absl::GetStackTrace(t.stack, tcmalloc::kMaxStackDepth, 1);
    state.Add(LogItem("@"));
    for (int i = 0; i < t.depth; i++) {
      state.Add(LogItem(t.stack[i]));
    }
  }

  // Teminate with newline
  if (state.p_ >= state.end_) {
    state.p_ = state.end_ - 1;
  }
  *state.p_ = '\n';
  state.p_++;

  int msglen = state.p_ - state.buf_;
  if (!crash) {
    (*log_message_writer)(state.buf_, msglen);
    return;
  }

  // FailureSignalHandler mallocs for various logging attempts.
  // We might be crashing holding tcmalloc locks.
  // We're substantially less likely to try to take those locks
  // (and thus deadlock until the alarm timer fires) if we disable sampling.
  if (TCMalloc_Internal_SetProfileSamplingRate != nullptr) {
    TCMalloc_Internal_SetProfileSamplingRate(0);
  }

  bool first_crash = false;
  {
    absl::base_internal::SpinLockHolder l(&crash_lock);
    if (!crashed) {
      crashed = true;
      first_crash = true;
    }
  }

  (*log_message_writer)(state.buf_, msglen);
  if (first_crash && mode == kCrashWithStats) {
    if (&TCMalloc_Internal_GetStats != nullptr) {
      size_t n = TCMalloc_Internal_GetStats(stats_buffer, kStatsBufferSize);
      (*log_message_writer)(stats_buffer, std::min(n, kStatsBufferSize));
    }
  }

  abort();
}

bool Logger::Add(const LogItem& item) {
  // Separate real items with spaces
  if (item.tag_ != LogItem::kEnd && p_ < end_) {
    *p_ = ' ';
    p_++;
  }

  switch (item.tag_) {
    case LogItem::kStr:
      return AddStr(item.u_.str, strlen(item.u_.str));
    case LogItem::kUnsigned:
      return AddNum(item.u_.unum, 10);
    case LogItem::kSigned:
      if (item.u_.snum < 0) {
        // The cast to uint64_t is intentionally before the negation
        // so that we do not attempt to negate -2^63.
        return AddStr("-", 1)
            && AddNum(- static_cast<uint64_t>(item.u_.snum), 10);
      } else {
        return AddNum(static_cast<uint64_t>(item.u_.snum), 10);
      }
    case LogItem::kPtr:
      return AddStr("0x", 2)
          && AddNum(reinterpret_cast<uintptr_t>(item.u_.ptr), 16);
    default:
      return false;
  }
}

bool Logger::AddStr(const char* str, int n) {
  if (end_ - p_ < n) {
    return false;
  } else {
    memcpy(p_, str, n);
    p_ += n;
    return true;
  }
}

bool Logger::AddNum(uint64_t num, int base) {
  static const char kDigits[] = "0123456789abcdef";
  char space[22];  // more than enough for 2^64 in smallest supported base (10)
  char* end = space + sizeof(space);
  char* pos = end;
  do {
    pos--;
    *pos = kDigits[num % base];
    num /= base;
  } while (num > 0 && pos > space);
  return AddStr(pos, end - pos);
}

}  // namespace tcmalloc

void TCMalloc_Printer::printf(const char* format, ...) {
  ASSERT(left_ >= 0);
  va_list ap;
  va_start(ap, format);
  const int r = vsnprintf(buf_, left_, format, ap);
  va_end(ap);
  if (r < 0) {
    // Perhaps an old glibc that returns -1 on truncation?  We can't draw
    // conclusions on how this affects the required buffer space.
    left_ = 0;
    return;
  }

  required_ += r;

  if (r > left_) {
    // Truncation
    left_ = 0;
  } else {
    left_ -= r;
    buf_ += r;
  }
}

PbtxtRegion::PbtxtRegion(TCMalloc_Printer* out, PbtxtRegionType type,
                         int indent)
    : out_(out), type_(type), indent_(indent) {
  switch (type_) {
    case kTop:
      break;
    case kNested:
      out_->printf("{");
      break;
  }
  ++indent_;
}

PbtxtRegion::~PbtxtRegion() {
  --indent_;
  out_->printf("\n");
  for (int i = 0; i < indent_; i++) {
    out_->printf("  ");
  }
  switch (type_) {
    case kTop:
      break;
    case kNested:
      out_->printf("}");
      break;
  }
}

void PbtxtRegion::NewLineAndIndent() {
  out_->printf("\n");
  for (int i = 0; i < indent_; i++) {
    out_->printf("  ");
  }
}

void PbtxtRegion::PrintU64(absl::string_view key, uint64_t value) {
  NewLineAndIndent();
  out_->printf("%s: %" PRIu64, key.data(), value);
}

void PbtxtRegion::PrintI64(absl::string_view key, int64_t value) {
  NewLineAndIndent();
  out_->printf("%s: %" PRIi64, key.data(), value);
}

void PbtxtRegion::PrintDouble(absl::string_view key, double value) {
  NewLineAndIndent();
  out_->printf("%s: %.3g", key.data(), value);
}

void PbtxtRegion::PrintBool(absl::string_view key, bool value) {
  NewLineAndIndent();
  out_->printf("%s: %s", key.data(), value ? "true" : "false");
}

void PbtxtRegion::PrintRaw(absl::string_view key, absl::string_view value) {
  NewLineAndIndent();
  out_->printf("%s: %s", key.data(), value.data());
}

PbtxtRegion PbtxtRegion::CreateSubRegion(absl::string_view key) {
  NewLineAndIndent();
  out_->printf("%s ", key.data());
  PbtxtRegion sub(out_, kNested, indent_);
  return sub;
}
