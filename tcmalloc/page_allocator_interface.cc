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

#include "tcmalloc/page_allocator_interface.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>

#include "tcmalloc/internal/logging.h"
#include "tcmalloc/internal/util.h"
#include "tcmalloc/static_vars.h"

namespace tcmalloc {

using tcmalloc::tcmalloc_internal::signal_safe_open;
using tcmalloc::tcmalloc_internal::thread_safe_getenv;

static int OpenLog(bool tagged) {
  const char *fname = tagged
                          ? thread_safe_getenv("TCMALLOC_TAGGED_PAGE_LOG_FILE")
                          : thread_safe_getenv("TCMALLOC_PAGE_LOG_FILE");
  if (!fname) return -1;

  if (getuid() != geteuid() || getgid() != getegid()) {
    Log(kLog, __FILE__, __LINE__, "Cannot take a pagetrace from setuid binary");
    return -1;
  }
  char buf[PATH_MAX];
  // Tag file with PID - handles forking children much better.
  int pid = getpid();
  // Blaze tests can output here for recovery of the output file
  const char *test_dir = thread_safe_getenv("TEST_UNDECLARED_OUTPUTS_DIR");
  if (test_dir) {
    snprintf(buf, sizeof(buf), "%s/%s.%d", test_dir, fname, pid);
  } else {
    snprintf(buf, sizeof(buf), "%s.%d", fname, pid);
  }
  int fd =
      signal_safe_open(buf, O_WRONLY | O_CREAT | O_TRUNC, S_IRUSR | S_IWUSR);

  if (fd < 0) {
    Log(kCrash, __FILE__, __LINE__, fd, errno, fname);
  }

  return fd;
}

PageAllocatorInterface::PageAllocatorInterface(const char *label, bool tagged)
    : PageAllocatorInterface(label, Static::pagemap(), tagged) {}

PageAllocatorInterface::PageAllocatorInterface(const char *label, PageMap *map,
                                               bool tagged)
    : info_(label, OpenLog(tagged)), pagemap_(map), tagged_(tagged) {}

PageAllocatorInterface::~PageAllocatorInterface() {
  // This is part of tcmalloc statics - they must be immortal.
  Log(kCrash, __FILE__, __LINE__, "should never destroy this");
}

}  // namespace tcmalloc
