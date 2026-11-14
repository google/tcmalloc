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

#include "tcmalloc/internal/residency.h"

#include <fcntl.h>
#include <stddef.h>
#if defined(__linux__)
#include <sys/syscall.h>
#endif
#include <unistd.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <optional>

#include "absl/algorithm/container.h"
#include "absl/functional/function_ref.h"
#include "absl/status/status.h"
#include "tcmalloc/internal/config.h"
#include "tcmalloc/internal/logging.h"
#include "tcmalloc/internal/page_size.h"
#include "tcmalloc/internal/range_tracker.h"
#include "tcmalloc/internal/util.h"

GOOGLE_MALLOC_SECTION_BEGIN
namespace tcmalloc {
namespace tcmalloc_internal {

// TODO(b/585745921): Move to internal/numa.h once :numa no longer pulls in
// :percpu's initial-exec TLS globals.
int GetCurrentNumaNode() {
#if defined(__linux__) && defined(SYS_getcpu)
  unsigned int node = 0;
  if (syscall(SYS_getcpu, nullptr, &node, nullptr) == 0) {
    return node;
  }
#endif
  return 0;
}

bool ForEachRemoteNumaPage(
    const uintptr_t base_page, const size_t num_pages, const int local_node,
    const absl::FunctionRef<void(size_t page_index)> on_remote_page) {
  const size_t kHardwarePageSize = GetPageSize();
  if (local_node < 0 || (base_page % kHardwarePageSize) != 0) {
    return false;
  }
  if (num_pages == 0) {
    return true;
  }
#if defined(__linux__) && defined(SYS_move_pages)
  constexpr size_t kBatchSize = 64;
  const void* pages[kBatchSize];
  int status[kBatchSize];

  for (size_t page_idx = 0; page_idx < num_pages; page_idx += kBatchSize) {
    const size_t batch = std::min(kBatchSize, num_pages - page_idx);
    for (size_t i = 0; i < batch; ++i) {
      pages[i] = reinterpret_cast<const void*>(
          base_page + (page_idx + i) * kHardwarePageSize);
    }
    absl::c_fill(status, -1);
    if (syscall(SYS_move_pages, 0, batch, pages, nullptr, status, 0) != 0) {
      return false;
    }
    for (size_t i = 0; i < batch; ++i) {
      if (status[i] >= 0 && status[i] != local_node) {
        on_remote_page(page_idx + i);
      }
    }
  }
  return true;
#else
  return false;
#endif
}

namespace {

// From fs/proc/task_mmu.c:
#define PM_MMAP_EXCLUSIVE (1ULL << 56)
#define PM_GUARD_REGION (1ULL << 58)
#define PM_FILE (1ULL << 61)
#define PM_SWAP (1ULL << 62)
#define PM_PRESENT (1ULL << 63)

constexpr bool PageSwapped(uint64_t flags) {
  return (flags & (PM_SWAP | PM_GUARD_REGION)) == PM_SWAP;
}

constexpr bool PagePresent(uint64_t flags) {
  constexpr uint64_t kPresentPage = PM_PRESENT;
  return (flags & kPresentPage) == kPresentPage;
}

// Small helper to interpret /proc/pid/pagemap. Bit 62 represents if the page is
// swapped, and bit 63 represents if the page is present.
void Update(const uint64_t input, const size_t size, const size_t page_index,
            Residency::Info& info) {
  // From fs/proc/task_mmu.c:
  // Zero-page PTEs are PM_PRESENT without PM_MMAP_EXCLUSIVE or PM_FILE and are
  // not counted in Rss.
  bool present = PagePresent(input) && (input & (PM_MMAP_EXCLUSIVE | PM_FILE));
  bool swapped = PageSwapped(input);
  if (present) {
    info.bytes_resident += size;
    if (page_index < kMaxResidencyBits) {
      info.page_is_resident.SetBit(page_index);
    }
  }
  if (swapped) {
    info.bytes_swapped += size;
  }
}

}  // namespace

ResidencyPageMap::ResidencyPageMap()
    : fd_(signal_safe_open("/proc/self/pagemap", O_RDONLY)) {
  TC_CHECK_GE(sizeof(buf_), kSizeOfHugepageInPagemap,
              "Buffer size is not large enough to hold the pagemap entries");
  TC_CHECK_LE(kHardwarePagesInHugePage, kMaxResidencyBits,
              "Actual number of native pages in a hugepage is larger than the "
              "total capacity of residency bitmaps");
}

ResidencyPageMap::ResidencyPageMap(const char* const alternate_filename)
    : fd_(signal_safe_open(alternate_filename, O_RDONLY)) {
  TC_CHECK_GE(sizeof(buf_), kSizeOfHugepageInPagemap,
              "Buffer size is not large enough to hold the pagemap entries");
  TC_CHECK_LE(kHardwarePagesInHugePage, kMaxResidencyBits,
              "Actual number of native pages in a hugepage is larger than the "
              "total capacity of residency bitmaps");
}

ResidencyPageMap::~ResidencyPageMap() {
  if (fd_ >= 0) {
    signal_safe_close(fd_);
  }
}

absl::StatusCode ResidencyPageMap::Seek(const uintptr_t vaddr) {
  size_t offset = vaddr / kHardwarePageSize * kPagemapEntrySize;
  // Note: lseek can't be interrupted.
  off_t status = ::lseek(fd_, offset, SEEK_SET);
  if (status != offset) {
    return absl::StatusCode::kUnavailable;
  }
  return absl::StatusCode::kOk;
}

std::optional<uint64_t> ResidencyPageMap::ReadOne() {
  static_assert(sizeof(buf_) >= kPagemapEntrySize);
  // /proc/pid/pagemap is a sequence of 64-bit values in machine endianness, one
  // per page. The style guide really does not want me to do this "unsafe
  // conversion", but the conversion is done in reverse by the kernel and we
  // never persist it anywhere, so we actually do want this.
  auto status = signal_safe_read(fd_, reinterpret_cast<char*>(buf_),
                                 kPagemapEntrySize, nullptr);
  if (status != kPagemapEntrySize) {
    return std::nullopt;
  }
  return buf_[0];
}

absl::StatusCode ResidencyPageMap::ReadMany(int64_t num_pages,
                                            size_t page_index,
                                            Residency::Info& info) {
  while (num_pages > 0) {
    const size_t batch_size = std::min<int64_t>(kEntriesInBuf, num_pages);
    const size_t to_read = kPagemapEntrySize * batch_size;

    // We read continuously. For the first read, this starts at wherever the
    // first ReadOne ended. See above note for the reinterpret_cast.
    auto status =
        signal_safe_read(fd_, reinterpret_cast<char*>(buf_), to_read, nullptr);
    if (status != to_read) {
      return absl::StatusCode::kUnavailable;
    }
    for (size_t i = 0; i < batch_size; ++i) {
      Update(buf_[i], kHardwarePageSize, page_index++, info);
    }
    num_pages -= batch_size;
  }
  return absl::StatusCode::kOk;
}

std::optional<Residency::Info> ResidencyPageMap::Get(const void* const addr,
                                                     const size_t size) {
  if (fd_ < 0) {
    return std::nullopt;
  }

  Residency::Info info;
  if (size == 0) return info;

  uintptr_t uaddr = reinterpret_cast<uintptr_t>(addr);
  // Round address down to get the start of the page containing the data.
  uintptr_t basePage = uaddr & ~(kHardwarePageSize - 1);
  // Round end address up to get the end of the page containing the data.
  // The data is in [basePage, endPage).
  uintptr_t endPage =
      (uaddr + size + kHardwarePageSize - 1) & ~(kHardwarePageSize - 1);

  int64_t remainingPages = (endPage - basePage) / kHardwarePageSize;
  size_t page_index = 0;

  if (auto res = Seek(basePage); res != absl::StatusCode::kOk) {
    return std::nullopt;
  }

  if (remainingPages == 1) {
    auto res = ReadOne();
    if (!res.has_value()) return std::nullopt;
    Update(res.value(), size, page_index++, info);
    return info;
  }

  // Since the input address might not be page-aligned (it can possibly point to
  // an arbitrary object), we read the information about the first page
  // separately with ReadOne, then read the complete pages with ReadMany, and
  // then read the last page with ReadOne again if needed.
  auto res = ReadOne();
  if (!res.has_value()) return std::nullopt;

  // Handle the first page.
  size_t firstPageSize = kHardwarePageSize - (uaddr - basePage);
  Update(res.value(), firstPageSize, page_index++, info);
  remainingPages--;

  // Handle all pages but the last page.
  if (auto res = ReadMany(remainingPages - 1, page_index, info);
      res != absl::StatusCode::kOk) {
    return std::nullopt;
  }
  page_index += remainingPages - 1;
  remainingPages = 1;

  // Check final page
  size_t lastPageSize = kHardwarePageSize - (endPage - uaddr - size);
  res = ReadOne();
  if (!res.has_value()) return std::nullopt;
  Update(res.value(), lastPageSize, page_index++, info);

  return info;
}

std::optional<size_t> ResidencyPageMap::GetRemoteNumaBytes(
    const void* const addr, const size_t size, const int local_node) {
  if (fd_ < 0 || addr == nullptr || local_node < 0) {
    return std::nullopt;
  }
  if (size == 0) {
    return 0;
  }
  const uintptr_t uaddr = reinterpret_cast<uintptr_t>(addr);
  const uintptr_t base_page = uaddr & ~(kHardwarePageSize - 1);
  const uintptr_t end_page =
      (uaddr + size + kHardwarePageSize - 1) & ~(kHardwarePageSize - 1);
  const size_t total_pages = (end_page - base_page) / kHardwarePageSize;

  size_t remote_bytes = 0;
  if (!ForEachRemoteNumaPage(
          base_page, total_pages, local_node, [&](size_t page_index) {
            const uintptr_t page_start =
                base_page + page_index * kHardwarePageSize;
            const uintptr_t span_start = std::max(uaddr, page_start);
            const uintptr_t span_end =
                std::min(uaddr + size, page_start + kHardwarePageSize);
            remote_bytes += span_end - span_start;
          })) {
    return std::nullopt;
  }
  return remote_bytes;
}

Residency::SinglePageBitmaps ResidencyPageMap::GetUnbackedAndSwappedBitmaps(
    const void* const addr) {
  Bitmap<kMaxResidencyBits> page_unbacked;
  Bitmap<kMaxResidencyBits> page_swapped;
  uintptr_t currPage = reinterpret_cast<uintptr_t>(addr);
  if ((currPage & kHugePageMask) != currPage) {
    TC_LOG("Address is not hugepage aligned");
    return SinglePageBitmaps{page_unbacked, page_swapped,
                             absl::StatusCode::kFailedPrecondition};
  }
  auto res = Seek(currPage);
  if (res != absl::StatusCode::kOk) {
    return SinglePageBitmaps{page_unbacked, page_swapped,
                             absl::StatusCode::kUnavailable};
  }
  auto status = signal_safe_read(fd_, reinterpret_cast<char*>(buf_),
                                 kSizeOfHugepageInPagemap, nullptr);
  if (status != kSizeOfHugepageInPagemap) {
    TC_LOG(
        "Could not read from pagemap file due to unexpected number of bytes "
        "read. Expected %d bytes, got %d bytes",
        kSizeOfHugepageInPagemap, status);
    return SinglePageBitmaps{page_unbacked, page_swapped,
                             absl::StatusCode::kUnavailable};
  }

  for (int native_page_idx = 0; native_page_idx < kHardwarePagesInHugePage;
       ++native_page_idx) {
    uint64_t page_map = buf_[native_page_idx];
    if (!PagePresent(page_map)) {
      if (PageSwapped(page_map)) {
        page_swapped.SetBit(native_page_idx);
      } else {
        page_unbacked.SetBit(native_page_idx);
      }
    }
  }
  return SinglePageBitmaps{page_unbacked, page_swapped, absl::StatusCode::kOk};
}

}  // namespace tcmalloc_internal
}  // namespace tcmalloc
GOOGLE_MALLOC_SECTION_END
