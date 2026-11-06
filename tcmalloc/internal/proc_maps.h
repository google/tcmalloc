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

#ifndef TCMALLOC_INTERNAL_PROC_MAPS_H_
#define TCMALLOC_INTERNAL_PROC_MAPS_H_

#include <stdint.h>
#include <sys/types.h>

#include <cstddef>
#include <optional>

#include "absl/functional/function_ref.h"
#include "absl/strings/string_view.h"
#include "tcmalloc/internal/config.h"

GOOGLE_MALLOC_SECTION_BEGIN
namespace tcmalloc {
namespace tcmalloc_internal {

// Parses a single line from /proc/self/numa_maps and returns the NUMA node ID
// with the largest number of resident pages, or std::nullopt if the line is
// malformed or has no resident pages.  If start_addr is non-null, stores the
// VMA start address.
[[nodiscard]] std::optional<int> ParseNumaMapsLine(absl::string_view line,
                                                   uintptr_t* start_addr);

// Reads /proc/self/numa_maps and invokes `callback(start_addr, numa_node)` for
// each VMA with resident pages.
void ReadProcNumaMaps(
    absl::FunctionRef<void(uintptr_t start_addr, int numa_node)> callback);

// A ProcMapsIterator abstracts access to /proc/maps for a given process.
class ProcMapsIterator {
 public:
  struct Buffer {
    static constexpr size_t kBufSize = PATH_MAX + 1024;
    char buf[kBufSize];
  };

  // Create an iterator with specified storage (for use in signal handler).
  explicit ProcMapsIterator(Buffer* buffer);

  // Returns true if the iterator successfully initialized;
  [[nodiscard]] bool Valid() const;

  [[nodiscard]] bool NextExt(uint64_t* start, uint64_t* end, char** flags,
                             uint64_t* offset, int64_t* inode, char** filename,
                             dev_t* dev);

  ~ProcMapsIterator();

 private:
  char* ibuf_;      // input buffer
  char* stext_;     // start of text
  char* etext_;     // end of text
  char* nextline_;  // start of next line
  char* ebuf_;      // end of buffer (1 char for a nul)
  int fd_;          // filehandle on /proc/*/maps
  char flags_[10];
};

}  // namespace tcmalloc_internal
}  // namespace tcmalloc
GOOGLE_MALLOC_SECTION_END

#endif  // TCMALLOC_INTERNAL_PROC_MAPS_H_
