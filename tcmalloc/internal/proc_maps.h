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

#include <array>
#include <cstddef>
#include <optional>

#include "absl/functional/function_ref.h"
#include "absl/strings/string_view.h"
#include "tcmalloc/internal/config.h"

GOOGLE_MALLOC_SECTION_BEGIN
namespace tcmalloc {
namespace tcmalloc_internal {

// Reads from `read` until EOF (<= 0 return) and invokes `callback(line)` for
// each line (without the trailing newline).
void ReadLines(absl::FunctionRef<ssize_t(char* buf, size_t count)> read,
               absl::FunctionRef<void(absl::string_view line)> callback);

struct NumaMapEntry {
  static constexpr size_t kNumaNodeBuckets = 3;

  uintptr_t start_addr = 0;
  // Resident page counts from /proc/self/numa_maps ("N<node>=<nr_pages>"):
  //   node_pages[0]: NUMA node 0
  //   node_pages[1]: NUMA node 1
  //   node_pages[2]: NUMA nodes > 1
  std::array<uint64_t, kNumaNodeBuckets> node_pages = {};

  bool operator==(const NumaMapEntry& rhs) const {
    return start_addr == rhs.start_addr && node_pages == rhs.node_pages;
  }
};

// Parses a single line from /proc/self/numa_maps and returns the VMA start
// address and per-NUMA-node resident page counts, or std::nullopt if the line
// is malformed.
[[nodiscard]] std::optional<NumaMapEntry> ParseNumaMapsLine(
    absl::string_view line);

// Reads /proc/self/numa_maps and invokes `callback(entry)` for each VMA entry.
void ReadProcNumaMaps(
    absl::FunctionRef<void(const NumaMapEntry& entry)> callback);

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
