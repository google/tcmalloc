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
#include <unistd.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <optional>

#include "absl/status/status.h"
#include "tcmalloc/internal/config.h"
#include "tcmalloc/internal/logging.h"
#include "tcmalloc/internal/range_tracker.h"
#include "tcmalloc/internal/util.h"

GOOGLE_MALLOC_SECTION_BEGIN
namespace tcmalloc {
namespace tcmalloc_internal {
namespace {

// From fs/proc/task_mmu.c:
#define PM_SWAP 1ULL << 62
#define PM_PRESENT 1ULL << 63

constexpr bool PageSwapped(uint64_t flags) {
  constexpr uint64_t kSwap = PM_SWAP;
  return (flags & kSwap) == kSwap;
}

constexpr bool PagePresent(uint64_t flags) {
  constexpr uint64_t kPresentPage = PM_PRESENT;
  return (flags & kPresentPage) == kPresentPage;
}

// Small helper to interpret /proc/pid/pagemap. Bit 62 represents if the page is
// swapped, and bit 63 represents if the page is present.
void Update(const uint64_t input, const size_t size, Residency::Info& info) {
  // From fs/proc/task_mmu.c:
  if (PagePresent(input)) {
    info.bytes_resident += size;
  }
  if (PageSwapped(input)) {
    info.bytes_swapped += size;
  }
}

}  // namespace

ResidencyPageMap::ResidencyPageMap()
    : fd_(signal_safe_open("/proc/self/pagemap", O_RDONLY)) {
  TC_CHECK_GE(sizeof(buf_), kSizeOfHugepageInPagemap,
              "Buffer size is not large enough to hold the pagemap entries");
  TC_CHECK_LE(kNativePagesInHugePage, kMaxResidencyBits,
              "Actual number of native pages in a hugepage is larger than the "
              "total capacity of residency bitmaps");
}

ResidencyPageMap::ResidencyPageMap(const char* const alternate_filename)
    : fd_(signal_safe_open(alternate_filename, O_RDONLY)) {
  TC_CHECK_GE(sizeof(buf_), kSizeOfHugepageInPagemap,
              "Buffer size is not large enough to hold the pagemap entries");
  TC_CHECK_LE(kNativePagesInHugePage, kMaxResidencyBits,
              "Actual number of native pages in a hugepage is larger than the "
              "total capacity of residency bitmaps");
}

ResidencyPageMap::~ResidencyPageMap() {
  if (fd_ >= 0) {
    signal_safe_close(fd_);
  }
}

absl::StatusCode ResidencyPageMap::Seek(const uintptr_t vaddr) {
  size_t offset = vaddr / kPageSize * kPagemapEntrySize;
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
    for (int i = 0; i < batch_size; ++i) {
      Update(buf_[i], kPageSize, info);
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
  uintptr_t basePage = uaddr & ~(kPageSize - 1);
  // Round end address up to get the end of the page containing the data.
  // The data is in [basePage, endPage).
  uintptr_t endPage = (uaddr + size + kPageSize - 1) & ~(kPageSize - 1);

  int64_t remainingPages = (endPage - basePage) / kPageSize;

  if (auto res = Seek(basePage); res != absl::StatusCode::kOk) {
    return std::nullopt;
  }

  if (remainingPages == 1) {
    auto res = ReadOne();
    if (!res.has_value()) return std::nullopt;
    Update(res.value(), size, info);
    return info;
  }

  // Since the input address might not be page-aligned (it can possibly point to
  // an arbitrary object), we read the information about the first page
  // separately with ReadOne, then read the complete pages with ReadMany, and
  // then read the last page with ReadOne again if needed.
  auto res = ReadOne();
  if (!res.has_value()) return std::nullopt;

  // Handle the first page.
  size_t firstPageSize = kPageSize - (uaddr - basePage);
  Update(res.value(), firstPageSize, info);
  remainingPages--;

  // Handle all pages but the last page.
  if (auto res = ReadMany(remainingPages - 1, info);
      res != absl::StatusCode::kOk) {
    return std::nullopt;
  }

  // Check final page
  size_t lastPageSize = kPageSize - (endPage - uaddr - size);
  res = ReadOne();
  if (!res.has_value()) return std::nullopt;
  Update(res.value(), lastPageSize, info);
  return info;
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

  for (int native_page_idx = 0; native_page_idx < kNativePagesInHugePage;
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
