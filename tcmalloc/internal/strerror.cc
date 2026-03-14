// Copyright 2026 The TCMalloc Authors
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

#include "tcmalloc/internal/strerror.h"

#include <errno.h>

#include "absl/strings/string_view.h"
#include "tcmalloc/internal/config.h"

GOOGLE_MALLOC_SECTION_BEGIN
namespace tcmalloc {
namespace tcmalloc_internal {

absl::string_view StrError(int err) {
  switch (err) {
    case EPERM:
      return "Operation not permitted";
    case ENOENT:
      return "No such file or directory";
    case ESRCH:
      return "No such process";
    case EINTR:
      return "Interrupted system call";
    case EIO:
      return "Input/output error";
    case ENXIO:
      return "No such device or address";
    case E2BIG:
      return "Argument list too long";
    case ENOEXEC:
      return "Exec format error";
    case EBADF:
      return "Bad file descriptor";
    case ECHILD:
      return "No child processes";
    case EAGAIN:
      return "Resource temporarily unavailable";
    case ENOMEM:
      return "Cannot allocate memory";
    case EACCES:
      return "Permission denied";
    case EFAULT:
      return "Bad address";
    case ENOTBLK:
      return "Block device required";
    case EBUSY:
      return "Device or resource busy";
    case EEXIST:
      return "File exists";
    case EXDEV:
      return "Invalid cross-device link";
    case ENODEV:
      return "No such device";
    case ENOTDIR:
      return "Not a directory";
    case EISDIR:
      return "Is a directory";
    case EINVAL:
      return "Invalid argument";
    case ENFILE:
      return "Too many open files in system";
    case EMFILE:
      return "Too many open files";
    case ENOTTY:
      return "Inappropriate ioctl for device";
    case ETXTBSY:
      return "Text file busy";
    case EFBIG:
      return "File too large";
    case ENOSPC:
      return "No space left on device";
    case ESPIPE:
      return "Illegal seek";
    case EROFS:
      return "Read-only file system";
    case EMLINK:
      return "Too many links";
    case EPIPE:
      return "Broken pipe";
    case EDOM:
      return "Numerical argument out of domain";
    case ERANGE:
      return "Numerical result out of range";
    default:
      return "Unknown error";
  }
}

}  // namespace tcmalloc_internal
}  // namespace tcmalloc
GOOGLE_MALLOC_SECTION_END
