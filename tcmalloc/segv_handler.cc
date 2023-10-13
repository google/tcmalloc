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

#include "tcmalloc/segv_handler.h"

#include <fcntl.h>
#include <unistd.h>

#include "absl/base/internal/sysinfo.h"
#include "absl/debugging/stacktrace.h"
#include "tcmalloc/guarded_allocations.h"
#include "tcmalloc/internal/environment.h"
#include "tcmalloc/internal/logging.h"
#include "tcmalloc/static_vars.h"

GOOGLE_MALLOC_SECTION_BEGIN
namespace tcmalloc {
namespace tcmalloc_internal {


// If this failure occurs during "bazel test", writes a warning for Bazel to
// display.
static void RecordBazelWarning(absl::string_view error) {
  const char* warning_file = thread_safe_getenv("TEST_WARNINGS_OUTPUT_FILE");
  if (!warning_file) return;  // Not a bazel test.

  constexpr char warning[] = "GWP-ASan error detected: ";
  int fd = open(warning_file, O_CREAT | O_WRONLY | O_APPEND, 0644);
  if (fd == -1) return;
  (void)write(fd, warning, sizeof(warning) - 1);
  (void)write(fd, error.data(), error.size());
  (void)write(fd, "\n", 1);
  close(fd);
}

// If this failure occurs during a gUnit test, writes an XML file describing the
// error type.  Note that we cannot use ::testing::Test::RecordProperty()
// because it doesn't write the XML file if a test crashes (which we're about to
// do here).  So we write directly to the XML file instead.
//
static void RecordTestFailure(absl::string_view error) {
  const char* xml_file = thread_safe_getenv("XML_OUTPUT_FILE");
  if (!xml_file) return;  // Not a gUnit test.

  // Record test failure for Sponge.
  constexpr char xml_text_header[] =
      "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
      "<testsuites><testsuite><testcase>"
      "  <properties>"
      "    <property name=\"gwp-asan-report\" value=\"";
  constexpr char xml_text_footer[] =
      "\"/>"
      "  </properties>"
      "  <failure message=\"MemoryError\">"
      "    GWP-ASan detected a memory error.  See the test log for full report."
      "  </failure>"
      "</testcase></testsuite></testsuites>";

  int fd = open(xml_file, O_CREAT | O_WRONLY | O_TRUNC, 0644);
  if (fd == -1) return;
  (void)write(fd, xml_text_header, sizeof(xml_text_header) - 1);
  (void)write(fd, error.data(), error.size());
  (void)write(fd, xml_text_footer, sizeof(xml_text_footer) - 1);
  close(fd);
}
//
// If this crash occurs in a test, records test failure summaries.
//
// error contains the type of error to record.
static void RecordCrash(absl::string_view error) {

  RecordBazelWarning(error);
  RecordTestFailure(error);
}

static void PrintStackTrace(void** stack_frames, size_t depth) {
  for (size_t i = 0; i < depth; ++i) {
    Log(kLog, __FILE__, __LINE__, "  @  ", stack_frames[i]);
  }
}

static void PrintStackTraceFromSignalHandler(void* context) {
  void* stack_frames[kMaxStackDepth];
  size_t depth = absl::GetStackTraceWithContext(stack_frames, kMaxStackDepth,
  1,
                                                context, nullptr);
  PrintStackTrace(stack_frames, depth);
}

constexpr const char* WriteFlagToString(WriteFlag write_flag) {
  switch (write_flag) {
    case WriteFlag::Unknown:
      return "(read or write: indeterminate)";
    case WriteFlag::Read:
      return "(read)";
    case WriteFlag::Write:
      return "(write)";
  }
  ASSUME(false);
}

#if defined(__aarch64__)
struct __esr_context {
  struct _aarch64_ctx head;
  uint64_t esr;
};

static bool Aarch64GetESR(ucontext_t* ucontext, uint64_t* esr) {
  static const uint32_t kEsrMagic = 0x45535201;
  uint8_t* aux = reinterpret_cast<uint8_t*>(ucontext->uc_mcontext.__reserved);
  while (true) {
    _aarch64_ctx* ctx = (_aarch64_ctx*)aux;
    if (ctx->size == 0) break;
    if (ctx->magic == kEsrMagic) {
      *esr = ((__esr_context*)ctx)->esr;
      return true;
    }
    aux += ctx->size;
  }
  return false;
}
#endif

static WriteFlag ExtractWriteFlagFromContext(void* context) {
#if defined(__x86_64__)
  ucontext_t* uc = reinterpret_cast<ucontext_t*>(context);
  uintptr_t value = uc->uc_mcontext.gregs[REG_ERR];
  static const uint64_t PF_WRITE = 1U << 1;
  return value & PF_WRITE ? WriteFlag::Write : WriteFlag::Read;
#elif defined(__aarch64__)
  ucontext_t* uc = reinterpret_cast<ucontext_t*>(context);
  uint64_t esr;
  if (!Aarch64GetESR(uc, &esr)) return WriteFlag::Unknown;
  static const uint64_t ESR_ELx_WNR = 1U << 6;
  return esr & ESR_ELx_WNR ? WriteFlag::Write : WriteFlag::Read;
#else
  // __riscv is NOT (yet) supported
  (void)context;
  return WriteFlag::Unknown;
#endif
}

GuardedAllocationsErrorType RefineErrorTypeBasedOnWriteFlag(
    GuardedAllocationsErrorType error, WriteFlag write_flag) {
  switch (error) {
    case GuardedAllocationsErrorType::kUseAfterFree:
      switch (write_flag) {
        case WriteFlag::Write:
          return GuardedAllocationsErrorType::kUseAfterFreeWrite;
        case WriteFlag::Read:
          return GuardedAllocationsErrorType::kUseAfterFreeRead;
        default:
          break;
      }
      break;
    case GuardedAllocationsErrorType::kBufferUnderflow:
      switch (write_flag) {
        case WriteFlag::Write:
          return GuardedAllocationsErrorType::kBufferUnderflowWrite;
        case WriteFlag::Read:
          return GuardedAllocationsErrorType::kBufferUnderflowRead;
        default:
          break;
      }
      break;
    case GuardedAllocationsErrorType::kBufferOverflow:
      switch (write_flag) {
        case WriteFlag::Write:
          return GuardedAllocationsErrorType::kBufferOverflowWrite;
        case WriteFlag::Read:
          return GuardedAllocationsErrorType::kBufferOverflowRead;
        default:
          break;
      }
      break;
    default:
      break;
  }
  return error;
}

// A SEGV handler that prints stack traces for the allocation and deallocation
// of relevant memory as well as the location of the memory error.
void SegvHandler(int signo, siginfo_t* info, void* context) {
  if (signo != SIGSEGV) return;
  void* fault = info->si_addr;
  if (!tc_globals.guardedpage_allocator().PointerIsMine(fault)) return;

  // Store load/store from context.
  WriteFlag write_flag = ExtractWriteFlagFromContext(context);

  GuardedAllocationsStackTrace *alloc_trace, *dealloc_trace;
  GuardedAllocationsErrorType error =
      tc_globals.guardedpage_allocator().GetStackTraces(fault, &alloc_trace,
                                                        &dealloc_trace);
  if (error == GuardedAllocationsErrorType::kUnknown) return;
  error = RefineErrorTypeBasedOnWriteFlag(error, write_flag);
  pid_t current_thread = absl::base_internal::GetTID();
  off_t offset;
  size_t size;
  std::tie(offset, size) =
      tc_globals.guardedpage_allocator().GetAllocationOffsetAndSize(fault);

  Log(kLog, __FILE__, __LINE__,
      "*** GWP-ASan "
      "(https://google.github.io/tcmalloc/gwp-asan.html)  "
      "has detected a memory error ***");
  Log(kLog, __FILE__, __LINE__, ">>> Access at offset", offset,
      "into buffer of length", size);
  Log(kLog, __FILE__, __LINE__,
      "Error originates from memory allocated in thread", alloc_trace->tid,
      "at:");
  PrintStackTrace(alloc_trace->stack, alloc_trace->depth);

  switch (error) {
    case GuardedAllocationsErrorType::kUseAfterFree:
    case GuardedAllocationsErrorType::kUseAfterFreeRead:
    case GuardedAllocationsErrorType::kUseAfterFreeWrite:
      Log(kLog, __FILE__, __LINE__, "The memory was freed in thread",
          dealloc_trace->tid, "at:");
      PrintStackTrace(dealloc_trace->stack, dealloc_trace->depth);
      Log(kLog, __FILE__, __LINE__, "Use-after-free",
          WriteFlagToString(write_flag), "occurs in thread", current_thread,
          "at:");
      RecordCrash("use-after-free");
      break;
    case GuardedAllocationsErrorType::kBufferUnderflow:
    case GuardedAllocationsErrorType::kBufferUnderflowRead:
    case GuardedAllocationsErrorType::kBufferUnderflowWrite:
      Log(kLog, __FILE__, __LINE__, "Buffer underflow",
          WriteFlagToString(write_flag), "occurs in thread", current_thread,
          "at:");
      RecordCrash("buffer-underflow");
      break;
    case GuardedAllocationsErrorType::kBufferOverflow:
    case GuardedAllocationsErrorType::kBufferOverflowRead:
    case GuardedAllocationsErrorType::kBufferOverflowWrite:
      Log(kLog, __FILE__, __LINE__, "Buffer overflow",
          WriteFlagToString(write_flag), "occurs in thread", current_thread,
          "at:");
      RecordCrash("buffer-overflow");
      break;
    case GuardedAllocationsErrorType::kDoubleFree:
      Log(kLog, __FILE__, __LINE__, "The memory was freed in thread",
          dealloc_trace->tid, "at:");
      PrintStackTrace(dealloc_trace->stack, dealloc_trace->depth);
      Log(kLog, __FILE__, __LINE__, "Double free occurs in thread",
          current_thread, "at:");
      RecordCrash("double-free");
      break;
    case GuardedAllocationsErrorType::kBufferOverflowOnDealloc:
      Log(kLog, __FILE__, __LINE__,
          "Buffer overflow (write) detected in thread", current_thread,
          "at free:");
      RecordCrash("buffer-overflow-detected-at-free");
      break;
    case GuardedAllocationsErrorType::kUnknown:
      Crash(kCrash, __FILE__, __LINE__,
            "Unexpected GuardedAllocationsErrorType::kUnknown");
  }
  PrintStackTraceFromSignalHandler(context);
  if (error == GuardedAllocationsErrorType::kBufferOverflowOnDealloc) {
    Log(kLog, __FILE__, __LINE__,
        "*** Try rerunning with --config=asan to get stack trace of overflow "
        "***");
  }
  Log(kLog, __FILE__, __LINE__,
      "improved_guarded_sampling:", Parameters::improved_guarded_sampling());
}

static struct sigaction old_sa;

static void ForwardSignal(int signo, siginfo_t* info, void* context) {
  if (old_sa.sa_flags & SA_SIGINFO) {
    old_sa.sa_sigaction(signo, info, context);
  } else if (old_sa.sa_handler == SIG_DFL) {
    // No previous handler registered.  Re-raise signal for core dump.
    int err = sigaction(signo, &old_sa, nullptr);
    if (err == -1) {
      Log(kLog, __FILE__, __LINE__, "Couldn't restore previous sigaction!");
    }
    raise(signo);
  } else if (old_sa.sa_handler == SIG_IGN) {
    return;  // Previous sigaction ignored signal, so do the same.
  } else {
    old_sa.sa_handler(signo);
  }
}

static void HandleSegvAndForward(int signo, siginfo_t* info, void* context) {
  SegvHandler(signo, info, context);
  ForwardSignal(signo, info, context);
}

extern "C" void MallocExtension_Internal_ActivateGuardedSampling() {
  static absl::once_flag flag;
  absl::call_once(flag, []() {
    struct sigaction action = {};
    action.sa_sigaction = HandleSegvAndForward;
    sigemptyset(&action.sa_mask);
    action.sa_flags = SA_SIGINFO;
    sigaction(SIGSEGV, &action, &old_sa);
    tc_globals.guardedpage_allocator().AllowAllocations();
  });
}

}  // namespace tcmalloc_internal
}  // namespace tcmalloc
GOOGLE_MALLOC_SECTION_END
