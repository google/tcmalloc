// Copyright 2023 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef TCMALLOC_MEMFS_MALLOC_H_
#define TCMALLOC_MEMFS_MALLOC_H_

#include <string>

#include "absl/flags/declare.h"

ABSL_DECLARE_FLAG(std::string, memfs_malloc_path);

#ifdef __linux__

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#include "absl/base/internal/spinlock.h"
#include "absl/base/thread_annotations.h"
#include "absl/types/span.h"
#include "tcmalloc/internal/config.h"
#include "tcmalloc/malloc_extension.h"

GOOGLE_MALLOC_SECTION_BEGIN
namespace tcmalloc {
namespace tcmalloc_internal {

class HugetlbRegion;

// Hugetlbfs based allocator for tcmalloc.
class HugetlbRegionFactory final : public AddressRegionFactory {
 public:
  explicit HugetlbRegionFactory(AddressRegionFactory* fallback);
  HugetlbRegionFactory(AddressRegionFactory* fallback, int hugetlb_fd,
                       size_t min_page_size);
  ~HugetlbRegionFactory() override;

  AddressRegion* Create(void* start, size_t size, UsageHint hint) override;
  size_t GetStats(absl::Span<char> buffer) override;
  void NotifyRelease(size_t length) override ABSL_LOCKS_EXCLUDED(stats_mu_);
  void NotifyRelease(void* start, size_t length) override
      ABSL_LOCKS_EXCLUDED(stats_mu_);
  void NotifyBack(size_t length) override ABSL_LOCKS_EXCLUDED(stats_mu_);
  void NotifyBack(void* start, size_t length) override
      ABSL_LOCKS_EXCLUDED(stats_mu_);

  bool Initialize();

  int fd() const { return hugetlb_fd_; }
  size_t min_page_size() const { return min_page_size_; }
  AddressRegionFactory* fallback() const { return fallback_; }

 private:
  friend class HugetlbRegion;

  struct MappedRange {
    uintptr_t va_start = 0;
    uintptr_t va_end = 0;
    size_t file_offset = 0;
  };
  static constexpr size_t kMaxMappedRanges = 512;
  static constexpr size_t kClusterBitmapWords = 1024;
  static constexpr size_t kMaxTrackedClusters = kClusterBitmapWords * 64;

  // Mmaps size bytes of memory from hugetlb_fd_ at start.  Returns true on
  // success, false on failure.
  bool MapFromMemfs(void* start, size_t size, off_t offset)
      ABSL_LOCKS_EXCLUDED(stats_mu_);

  void LogFailure() ABSL_LOCKS_EXCLUDED(stats_mu_);

  void AddToAllocatedBytes(void* start, size_t size, off_t file_offset)
      ABSL_LOCKS_EXCLUDED(stats_mu_);

  bool failed_;  // Whether failed to allocate memory.

  // Const after initialization:
  size_t min_page_size_;        // Min page size alignment returned by OS
  size_t preferred_page_size_;  // Preferred page size for allocation
  int hugetlb_fd_;              // file descriptor for hugetlb

  // protects hugetlb_base_ and allocated_bytes_ which are read
  // in both GetStats() and Create()/Alloc().
  absl::base_internal::SpinLock stats_mu_;
  AddressRegionFactory* fallback_;  // Default system allocator to fall back to.

  off_t hugetlb_base_ ABSL_GUARDED_BY(stats_mu_);
  // cumulative bytes returned from Alloc():
  int64_t allocated_bytes_ ABSL_GUARDED_BY(stats_mu_);
  int64_t released_bytes_ ABSL_GUARDED_BY(stats_mu_);
  size_t num_mapped_ranges_ ABSL_GUARDED_BY(stats_mu_) = 0;
  MappedRange mapped_ranges_[kMaxMappedRanges] ABSL_GUARDED_BY(stats_mu_) = {};
  uint64_t released_cluster_bitmap_[kClusterBitmapWords] ABSL_GUARDED_BY(
      stats_mu_) = {};
};

}  // namespace tcmalloc_internal
}  // namespace tcmalloc
GOOGLE_MALLOC_SECTION_END

#endif  // __linux__

#endif  // TCMALLOC_MEMFS_MALLOC_H_
