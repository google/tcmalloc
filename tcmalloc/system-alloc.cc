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

#include "tcmalloc/system-alloc.h"

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/mman.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <new>
#include <tuple>
#include <type_traits>
#include <utility>

#include "absl/base/attributes.h"
#include "absl/base/internal/spinlock.h"
#include "absl/base/macros.h"
#include "absl/base/optimization.h"
#include "tcmalloc/common.h"
#include "tcmalloc/internal/logging.h"
#include "tcmalloc/malloc_extension.h"
#include "tcmalloc/sampler.h"

// On systems (like freebsd) that don't define MAP_ANONYMOUS, use the old
// form of the name instead.
#ifndef MAP_ANONYMOUS
# define MAP_ANONYMOUS MAP_ANON
#endif

// Solaris has a bug where it doesn't declare madvise() for C++.
//    http://www.opensolaris.org/jive/thread.jspa?threadID=21035&tstart=0
#if defined(__sun) && defined(__SVR4)
#include <sys/types.h>
extern "C" int madvise(caddr_t, size_t, int);
#endif

namespace tcmalloc {

namespace {

// Check that no bit is set at position ADDRESS_BITS or higher.
template <int ADDRESS_BITS>
void CheckAddressBits(uintptr_t ptr) {
  ASSERT((ptr >> ADDRESS_BITS) == 0);
}

// Specialize for the bit width of a pointer to avoid undefined shift.
template <>
ABSL_ATTRIBUTE_UNUSED void CheckAddressBits<8 * sizeof(void*)>(uintptr_t ptr) {}

static_assert(kAddressBits <= 8 * sizeof(void*),
              "kAddressBits must be smaller than the pointer size");

// Structure for discovering alignment
union MemoryAligner {
  void*  p;
  double d;
  size_t s;
} ABSL_CACHELINE_ALIGNED;

static_assert(sizeof(MemoryAligner) < kMinSystemAlloc,
              "hugepage alignment too small");

absl::base_internal::SpinLock spinlock(absl::base_internal::kLinkerInitialized);

// Page size is initialized on demand
size_t pagesize = 0;
size_t preferred_alignment = 0;

// The current region factory.
AddressRegionFactory* region_factory = nullptr;

// Rounds size down to a multiple of alignment.
size_t RoundDown(const size_t size, const size_t alignment) {
  // Checks that the alignment has only one bit set.
  ASSERT(alignment != 0 && (alignment & (alignment - 1)) == 0);
  return (size) & ~(alignment - 1);
}

// Rounds size up to a multiple of alignment.
size_t RoundUp(const size_t size, const size_t alignment) {
  return RoundDown(size + alignment - 1, alignment);
}

// Rounds size up to the nearest power of 2.
// Requires: size <= (SIZE_MAX / 2) + 1.
size_t RoundUpPowerOf2(size_t size) {
  for (size_t i = 0; i < sizeof(size_t) * 8; ++i) {
    size_t pow2 = size_t{1} << i;
    if (pow2 >= size) return pow2;
  }
  CHECK_CONDITION(false && "size too big to round up");
  return 0;
}

class MmapRegion : public AddressRegion {
 public:
  MmapRegion(uintptr_t start, size_t size) : start_(start), free_size_(size) {}
  std::pair<void*, size_t> Alloc(size_t size, size_t alignment) override;

 private:
  const uintptr_t start_;
  size_t free_size_;
};

class MmapRegionFactory : public AddressRegionFactory {
 public:
  AddressRegion* Create(void* start, size_t size, UsageHint hint) override;
  size_t GetStats(absl::Span<char> buffer) override;
  size_t GetStatsInPbtxt(absl::Span<char> buffer) override;

 private:
  std::atomic<int64_t> bytes_reserved_{0};
};
std::aligned_storage<sizeof(MmapRegionFactory),
                     alignof(MmapRegionFactory)>::type mmap_space;

class RegionManager {
 public:
  std::pair<void*, size_t> Alloc(size_t size, size_t alignment, bool tagged);

  void DiscardMappedRegions() {
    untagged_region_ = nullptr;
    tagged_region_ = nullptr;
  }

 private:
  // Checks that there is sufficient space available in the reserved region
  // for the next allocation, if not allocate a new region.
  // Then returns a pointer to the new memory.
  std::pair<void*, size_t> Allocate(size_t size, size_t alignment, bool tagged);

  AddressRegion* untagged_region_{nullptr};
  AddressRegion* tagged_region_{nullptr};
};
std::aligned_storage<sizeof(RegionManager), alignof(RegionManager)>::type
    region_manager_space;
RegionManager* region_manager = nullptr;

std::pair<void*, size_t> MmapRegion::Alloc(size_t request_size,
                                           size_t alignment) {
  // Align on kMinSystemAlloc boundaries to reduce external fragmentation for
  // future allocations.
  size_t size = RoundUp(request_size, kMinSystemAlloc);
  if (size < request_size) return {nullptr, 0};
  alignment = std::max(alignment, preferred_alignment);

  // Tries to allocate size bytes from the end of [start_, start_ + free_size_),
  // aligned to alignment.
  uintptr_t end = start_ + free_size_;
  uintptr_t result = end - size;
  if (result > end) return {nullptr, 0};  // Underflow.
  result &= ~(alignment - 1);
  if (result < start_) return {nullptr, 0};  // Out of memory in region.
  size_t actual_size = end - result;

  ASSERT(result % pagesize == 0);
  void* result_ptr = reinterpret_cast<void*>(result);
  if (mprotect(result_ptr, actual_size, PROT_READ | PROT_WRITE) != 0) {
    Log(kLogWithStack, __FILE__, __LINE__,
        "mprotect() region failed (ptr, size, error)", result_ptr, actual_size,
        strerror(errno));
    return {nullptr, 0};
  }
  free_size_ -= actual_size;
  return {result_ptr, actual_size};
}

AddressRegion* MmapRegionFactory::Create(void* start, size_t size,
                                         UsageHint hint) {
  void* region_space = MallocInternal(sizeof(MmapRegion));
  if (!region_space) return nullptr;
  bytes_reserved_.fetch_add(size, std::memory_order_relaxed);
  return new (region_space)
      MmapRegion(reinterpret_cast<uintptr_t>(start), size);
}

size_t MmapRegionFactory::GetStats(absl::Span<char> buffer) {
  TCMalloc_Printer printer(buffer.data(), buffer.size());
  long long allocated = bytes_reserved_.load(std::memory_order_relaxed);
  constexpr double MiB = 1048576.0;
  printer.printf("MmapSysAllocator: %lld bytes (%.1f MiB) reserved\n",
                 allocated, allocated / MiB);

  size_t required = printer.SpaceRequired();
  // SpaceRequired includes the null terminator.
  if (required > 0) {
    required--;
  }

  return required;
}

size_t MmapRegionFactory::GetStatsInPbtxt(absl::Span<char> buffer) {
  TCMalloc_Printer printer(buffer.data(), buffer.size());
  long long allocated = bytes_reserved_.load(std::memory_order_relaxed);
  printer.printf("mmap_sys_allocator: %lld\n", allocated);

  size_t required = printer.SpaceRequired();
  // SpaceRequired includes the null terminator.
  if (required > 0) {
    required--;
  }

  return required;
}

std::pair<void*, size_t> RegionManager::Alloc(size_t request_size,
                                              size_t alignment, bool tagged) {
  // We do not support size or alignment larger than kTagMask.
  // TODO(b/141325493): Handle these large allocations.
  if (request_size > kTagMask || alignment > kTagMask) return {nullptr, 0};

  // If we are dealing with large sizes, or large alignments we do not
  // want to throw away the existing reserved region, so instead we
  // return a new region specifically targeted for the request.
  if (request_size > kMinMmapAlloc || alignment > kMinMmapAlloc) {
    // Align on kMinSystemAlloc boundaries to reduce external fragmentation for
    // future allocations.
    size_t size = RoundUp(request_size, kMinSystemAlloc);
    if (size < request_size) return {nullptr, 0};
    alignment = std::max(alignment, preferred_alignment);
    void* ptr = MmapAligned(size, alignment, tagged);
    if (!ptr) return {nullptr, 0};
    auto region_type = tagged ? AddressRegionFactory::UsageHint::kInfrequent
                              : AddressRegionFactory::UsageHint::kNormal;
    AddressRegion* region = region_factory->Create(ptr, size, region_type);
    if (!region) {
      munmap(ptr, size);
      return {nullptr, 0};
    }
    std::pair<void*, size_t> result = region->Alloc(size, alignment);
    if (result.first != nullptr) {
      ASSERT(result.first == ptr);
      ASSERT(result.second == size);
    } else {
      ASSERT(result.second == 0);
    }
    return result;
  }
  return Allocate(request_size, alignment, tagged);
}

std::pair<void*, size_t> RegionManager::Allocate(size_t size, size_t alignment,
                                                 bool tagged) {
  AddressRegion*& region = tagged ? tagged_region_ : untagged_region_;
  // For sizes that fit in our reserved range first of all check if we can
  // satisfy the request from what we have available.
  if (region) {
    std::pair<void*, size_t> result = region->Alloc(size, alignment);
    if (result.first) return result;
  }

  // Allocation failed so we need to reserve more memory.
  // Reserve new region and try allocation again.
  void* ptr = MmapAligned(kMinMmapAlloc, kMinMmapAlloc, tagged);
  if (!ptr) return {nullptr, 0};
  auto region_type = tagged ? AddressRegionFactory::UsageHint::kInfrequent
                            : AddressRegionFactory::UsageHint::kNormal;
  region = region_factory->Create(ptr, kMinMmapAlloc, region_type);
  if (!region) {
    munmap(ptr, kMinMmapAlloc);
    return {nullptr, 0};
  }
  return region->Alloc(size, alignment);
}

void InitSystemAllocatorIfNecessary() {
  if (region_factory) return;
  pagesize = getpagesize();
  // Sets the preferred alignment to be the largest of either the alignment
  // returned by mmap() or our minimum allocation size. The minimum allocation
  // size is usually a multiple of page size, but this need not be true for
  // SMALL_BUT_SLOW where we do not allocate in units of huge pages.
  preferred_alignment = std::max(pagesize, kMinSystemAlloc);
  region_manager = new (&region_manager_space) RegionManager();
  region_factory = new (&mmap_space) MmapRegionFactory();
}

ABSL_CONST_INIT std::atomic<int> system_release_errors = ATOMIC_VAR_INIT(0);

}  // namespace

void* SystemAlloc(size_t bytes, size_t* actual_bytes, size_t alignment,
                  bool tagged) {
  // If default alignment is set request the minimum alignment provided by
  // the system.
  alignment = std::max(alignment, pagesize);

  // Discard requests that overflow
  if (bytes + alignment < bytes) return nullptr;

  // This may return significantly more memory than "bytes" by default, so
  // require callers to know the true amount allocated.
  ASSERT(actual_bytes != nullptr);

  absl::base_internal::SpinLockHolder lock_holder(&spinlock);

  InitSystemAllocatorIfNecessary();

  void* result = nullptr;
  std::tie(result, *actual_bytes) =
      region_manager->Alloc(bytes, alignment, tagged);

  if (result != nullptr) {
    CheckAddressBits<kAddressBits>(reinterpret_cast<uintptr_t>(result) +
                                   *actual_bytes - 1);
    ASSERT(tcmalloc::IsTaggedMemory(result) == tagged);
  }
  return result;
}

static bool ReleasePages(void* start, size_t length) {
  int ret;
  // Note -- ignoring most return codes, because if this fails it
  // doesn't matter...
  // Moreover, MADV_REMOVE *will* fail (with EINVAL) on anonymous memory,
  // but that's harmless.
#ifdef MADV_REMOVE
  // MADV_REMOVE deletes any backing storage for non-anonymous memory
  // (tmpfs).
  do {
    ret = madvise(start, length, MADV_REMOVE);
  } while (ret == -1 && errno == EAGAIN);

  if (ret == 0) {
    return true;
  }
#endif
#ifdef MADV_DONTNEED
  // MADV_DONTNEED drops page table info and any anonymous pages.
  do {
    ret = madvise(start, length, MADV_DONTNEED);
  } while (ret == -1 && errno == EAGAIN);

  if (ret == 0) {
    return true;
  }
#endif

  return false;
}

int SystemReleaseErrors() {
  return system_release_errors.load(std::memory_order_relaxed);
}

void SystemRelease(void* start, size_t length) {
  int saved_errno = errno;
#if defined(MADV_DONTNEED) || defined(MADV_REMOVE)
  const size_t pagemask = pagesize - 1;

  size_t new_start = reinterpret_cast<size_t>(start);
  size_t end = new_start + length;
  size_t new_end = end;

  // Round up the starting address and round down the ending address
  // to be page aligned:
  new_start = (new_start + pagesize - 1) & ~pagemask;
  new_end = new_end & ~pagemask;

  ASSERT((new_start & pagemask) == 0);
  ASSERT((new_end & pagemask) == 0);
  ASSERT(new_start >= reinterpret_cast<size_t>(start));
  ASSERT(new_end <= end);

  if (new_end > new_start) {
    void* new_ptr = reinterpret_cast<void*>(new_start);
    size_t new_length = new_end - new_start;

    if (!ReleasePages(new_ptr, new_length)) {
      // Try unlocking.
      int ret;
      do {
        ret = munlock(reinterpret_cast<char*>(new_start), new_end - new_start);
      } while (ret == -1 && errno == EAGAIN);

      if (ret != 0 || !ReleasePages(new_ptr, new_length)) {
        // If we fail to munlock *or* fail our second attempt at madvise,
        // increment our failure count.
        system_release_errors.fetch_add(1, std::memory_order_relaxed);
      }
    }
  }
#endif
  errno = saved_errno;
}

void SystemBack(void* start, size_t length) {
  // TODO(b/134694141): use madvise when we have better support for that;
  // taking faults is not free.

  // TODO(b/134694141): enable this, if we can avoid causing trouble for apps
  // that routinely make large mallocs they never touch (sigh).
  return;

  // Strictly speaking, not everything uses 4K pages.  However, we're
  // not asking the OS for anything actually page-related, just taking
  // a fault on every "page".  If the real page size is bigger, we do
  // a few extra reads; this is not worth worrying about.
  static const size_t kHardwarePageSize = 4 * 1024;
  CHECK_CONDITION(reinterpret_cast<intptr_t>(start) % kHardwarePageSize == 0);
  CHECK_CONDITION(length % kHardwarePageSize == 0);
  const size_t num_pages = length / kHardwarePageSize;

  struct PageStruct {
    volatile size_t data[kHardwarePageSize / sizeof(size_t)];
  };
  CHECK_CONDITION(sizeof(PageStruct) == kHardwarePageSize);

  PageStruct* ps = reinterpret_cast<PageStruct*>(start);
  PageStruct* limit = ps + num_pages;
  for (; ps < limit; ++ps) {
    ps->data[0] = 0;
  }
}

AddressRegionFactory* GetRegionFactory() {
  absl::base_internal::SpinLockHolder lock_holder(&spinlock);
  InitSystemAllocatorIfNecessary();
  return region_factory;
}

void SetRegionFactory(AddressRegionFactory* factory) {
  absl::base_internal::SpinLockHolder lock_holder(&spinlock);
  InitSystemAllocatorIfNecessary();
  region_manager->DiscardMappedRegions();
  region_factory = factory;
}

static uintptr_t RandomMmapHint(size_t size, size_t alignment, bool tagged) {
  // Rely on kernel's mmap randomization to seed our RNG.
  static uintptr_t rnd = []() {
    void* seed =
        mmap(nullptr, kPageSize, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (seed == MAP_FAILED) {
      Log(kCrash, __FILE__, __LINE__,
          "Initial mmap() reservation failed (size)", kPageSize);
    }
    munmap(seed, kPageSize);
    return reinterpret_cast<uintptr_t>(seed);
  }();

  // Mask out bits that cannot be used by the hardware, mask out the top
  // "usable" bit since it is reserved for kernel use, and also mask out the
  // next top bit to significantly reduce collisions with mappings that tend to
  // be placed in the upper half of the address space (e.g., stack, executable,
  // kernel-placed mmaps).  See b/139357826.
#if defined(MEMORY_SANITIZER) || defined(THREAD_SANITIZER)
  // MSan and TSan use up all of the lower address space, so we allow use of
  // mid-upper address space when they're active.  This only matters for
  // TCMalloc-internal tests, since sanitizers install their own malloc/free.
  constexpr uintptr_t kAddrMask = (uintptr_t{3} << (kAddressBits - 3)) - 1;
#else
  constexpr uintptr_t kAddrMask = (uintptr_t{1} << (kAddressBits - 2)) - 1;
#endif

  // Ensure alignment >= size so we're guaranteed the full mapping has the same
  // tag.
  alignment = RoundUpPowerOf2(std::max(alignment, size));

  rnd = Sampler::NextRandom(rnd);
  uintptr_t addr = rnd & kAddrMask & ~(alignment - 1) & ~kTagMask;
  if (!tagged) {
    addr |= kTagMask;
  }
  return addr;
}

void* MmapAligned(size_t size, size_t alignment, bool tagged) {
  ASSERT(size <= kTagMask);
  ASSERT(alignment <= kTagMask);

  static uintptr_t next_untagged_addr = 0;
  static uintptr_t next_tagged_addr = 0;

  uintptr_t& next_addr = tagged ? next_tagged_addr : next_untagged_addr;
  if (!next_addr || next_addr & (alignment - 1) ||
      IsTaggedMemory(reinterpret_cast<void*>(next_addr)) != tagged ||
      IsTaggedMemory(reinterpret_cast<void*>(next_addr + size - 1)) != tagged) {
    next_addr = RandomMmapHint(size, alignment, tagged);
  }
  for (int i = 0; i < 1000; ++i) {
    void* hint = reinterpret_cast<void*>(next_addr);
    // TODO(b/140190055): Use MAP_FIXED_NOREPLACE once available.
    void* result =
        mmap(hint, size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (result == hint) {
      // Attempt to keep the next mmap contiguous in the common case.
      next_addr += size;
      CHECK_CONDITION(kAddressBits == std::numeric_limits<uintptr_t>::digits ||
                      next_addr <= uintptr_t{1} << kAddressBits);
      return result;
    }
    if (result == MAP_FAILED) {
      Log(kLogWithStack, __FILE__, __LINE__,
          "mmap() reservation failed (hint, size, error)", hint, size,
          strerror(errno));
      return nullptr;
    }
    if (int err = munmap(result, size)) {
      Log(kLogWithStack, __FILE__, __LINE__, "munmap() failed");
      ASSERT(err == 0);
    }
    next_addr = RandomMmapHint(size, alignment, tagged);
  }

  Log(kLogWithStack, __FILE__, __LINE__,
      "MmapAligned() failed (size, alignment)", size, alignment);
  return nullptr;
}

}  // namespace tcmalloc
