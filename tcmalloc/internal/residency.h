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

#ifndef TCMALLOC_INTERNAL_RESIDENCY_H_
#define TCMALLOC_INTERNAL_RESIDENCY_H_

#include <stddef.h>
#include <stdint.h>
#include <unistd.h>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "tcmalloc/internal/config.h"

GOOGLE_MALLOC_SECTION_BEGIN
namespace tcmalloc {
namespace tcmalloc_internal {

// Residency offers information about memory residency: whether or not specific
// spans of memory are resident in core ("m in core"), swapped, or not present.
// Originally, this was implemented via the mincore syscall, but has since been
// abstracted to provide more information.
class Residency {
 public:
  // This class keeps an open file handle to procfs. Destroy the object to
  // reclaim it.
  Residency();
  ~Residency();

  // Query a span of memory starting from `addr` for `size` bytes.
  // This is NOT thread-safe. Do not use multiple copies of this class across
  // threads.
  struct Info {
    size_t bytes_resident = 0;
    size_t bytes_swapped = 0;
  };
  absl::StatusOr<Info> Get(const void* addr, size_t size);

 private:
  // This helper seeks the internal file to the correct location for the given
  // virtual address.
  absl::Status Seek(uintptr_t vaddr);
  // This helper reads information for a single page. This is useful for the
  // boundaries. It continues the read from the last Seek() or last Read
  // operation.
  absl::StatusOr<uint64_t> ReadOne();
  // This helper reads information for `num_pages` worth of _full_ pages and
  // puts the results into `info`. It continues the read from the last Seek() or
  // last Read operation.
  absl::Status ReadMany(int64_t num_pages, Info& info);

  // For testing.
  friend class ResidencySpouse;
  explicit Residency(const char* alternate_filename);

  // Size of the buffer used to gather results.
  static constexpr int kBufferLength = 4096;
  static constexpr int kPagemapEntrySize = 8;
  static constexpr int kEntriesInBuf = kBufferLength / kPagemapEntrySize;

  const size_t kPageSize = getpagesize();
  uint64_t buf_[kEntriesInBuf];
  const int fd_;
};

}  // namespace tcmalloc_internal
}  // namespace tcmalloc
GOOGLE_MALLOC_SECTION_END

#endif  // TCMALLOC_INTERNAL_RESIDENCY_H_
