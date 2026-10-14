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

#include "tcmalloc/internal/proc_maps.h"

#include <fcntl.h>
#include <sys/sysmacros.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <cstring>

#include "absl/strings/numbers.h"
#include "absl/strings/str_format.h"
#include "absl/strings/string_view.h"
#include "tcmalloc/internal/config.h"
#include "tcmalloc/internal/logging.h"
#include "tcmalloc/internal/util.h"

GOOGLE_MALLOC_SECTION_BEGIN
namespace tcmalloc {
namespace tcmalloc_internal {

ProcMapsIterator::ProcMapsIterator(Buffer* buffer) {
  ibuf_ = buffer->buf;

  stext_ = etext_ = nextline_ = ibuf_;
  ebuf_ = ibuf_ + Buffer::kBufSize - 1;
  nextline_ = ibuf_;

#if defined(__linux__)
  // /maps exists in several places:
  // * /proc/pid/maps (global view),
  // * /proc/pid/task/tid/maps (local view), and
  // * /proc/thread-self/maps (local view for current thread).
  //
  // The global view attempts to label each VMA which is the stack of a thread,
  // which is expensive and scales quadratically. We prefer the local view.
  constexpr char kPath[] = "/proc/thread-self/maps";

  // No error logging since this can be called from the crash dump
  // handler at awkward moments. Users should call Valid() before
  // using.
  TCMALLOC_RETRY_ON_TEMP_FAILURE(fd_ = open(kPath, O_RDONLY));
#else
  fd_ = -1;  // so Valid() is always false
#endif
}

ProcMapsIterator::~ProcMapsIterator() {
  // As it turns out, Linux guarantees that close() does in fact close a file
  // descriptor even when the return value is EINTR. According to the notes in
  // the manpage for close(2), this is widespread yet not fully portable, which
  // is unfortunate. POSIX explicitly leaves this behavior as unspecified.
  if (fd_ >= 0) close(fd_);
}

bool ProcMapsIterator::Valid() const { return fd_ != -1; }

#if defined __linux__
namespace {

absl::string_view ConsumeToken(absl::string_view& line, char delim) {
  const size_t start = line.find_first_not_of(' ');
  if (start == absl::string_view::npos) return {};
  line.remove_prefix(start);
  const size_t end = line.find(delim);
  if (end == absl::string_view::npos) {
    if (delim != ' ') return {};
    absl::string_view token = line;
    line.remove_prefix(line.size());
    return token;
  }
  absl::string_view token = line.substr(0, end);
  line.remove_prefix(end + 1);
  return token;
}

}  // namespace
#endif

bool ProcMapsIterator::NextExt(uint64_t* start, uint64_t* end, char** flags,
                               uint64_t* offset, int64_t* inode,
                               char** filename, dev_t* dev) {
#if defined __linux__
  do {
    // Advance to the start of the next line
    stext_ = nextline_;

    // See if we have a complete line in the buffer already
    nextline_ = static_cast<char*>(memchr(stext_, '\n', etext_ - stext_));
    if (!nextline_) {
      // Shift/fill the buffer so we do have a line
      int count = etext_ - stext_;

      // Move the current text to the start of the buffer
      memmove(ibuf_, stext_, count);
      stext_ = ibuf_;
      etext_ = ibuf_ + count;

      int nread = 0;  // fill up buffer with text
      while (etext_ < ebuf_) {
        TCMALLOC_RETRY_ON_TEMP_FAILURE(nread =
                                           read(fd_, etext_, ebuf_ - etext_));
        if (nread > 0)
          etext_ += nread;
        else
          break;
      }

      // Zero out remaining characters in buffer at EOF to avoid returning
      // garbage from subsequent calls.
      if (etext_ != ebuf_ && nread == 0) {
        memset(etext_, 0, ebuf_ - etext_);
      }
      *etext_ = '\n';  // sentinel; safe because ibuf extends 1 char beyond ebuf
      nextline_ = static_cast<char*>(memchr(stext_, '\n', etext_ + 1 - stext_));
    }
    absl::string_view line(stext_, nextline_ - stext_);
    *nextline_ = 0;                               // turn newline into nul
    nextline_ += ((nextline_ < etext_) ? 1 : 0);  // skip nul if not end of text
    // stext_ now points at a nul-terminated line
    uint64_t local_start, local_end, local_offset;
    uint32_t major, minor;
    int64_t local_inode;
    if (!absl::SimpleHexAtoi(ConsumeToken(line, '-'), &local_start)) continue;
    if (!absl::SimpleHexAtoi(ConsumeToken(line, ' '), &local_end)) continue;
    absl::string_view flags_tok = ConsumeToken(line, ' ');
    if (flags_tok.empty() || flags_tok.size() > 4) continue;
    memcpy(flags_, flags_tok.data(), flags_tok.size());
    flags_[flags_tok.size()] = '\0';
    if (!absl::SimpleHexAtoi(ConsumeToken(line, ' '), &local_offset)) continue;
    if (!absl::SimpleHexAtoi(ConsumeToken(line, ':'), &major)) continue;
    if (!absl::SimpleHexAtoi(ConsumeToken(line, ' '), &minor)) continue;
    if (!absl::SimpleAtoi(ConsumeToken(line, ' '), &local_inode)) continue;
    const size_t filename_offset = line.find_first_not_of(' ');
    line.remove_prefix(filename_offset == absl::string_view::npos
                           ? line.size()
                           : filename_offset);

    if (start) *start = local_start;
    if (end) *end = local_end;
    if (offset) *offset = local_offset;
    if (inode) *inode = local_inode;
    if (flags) *flags = flags_;
    if (filename) *filename = const_cast<char*>(line.data());
    if (dev) *dev = makedev(static_cast<int>(major), static_cast<int>(minor));

    return true;
  } while (etext_ > ibuf_);
#endif

  // We didn't find anything
  return false;
}

}  // namespace tcmalloc_internal
}  // namespace tcmalloc
GOOGLE_MALLOC_SECTION_END
