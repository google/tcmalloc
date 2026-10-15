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

#include <sys/mman.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <new>
#include <variant>
#include <vector>

#include "gtest/gtest.h"
#include "fuzztest/fuzztest.h"
#include "absl/numeric/bits.h"
#include "absl/strings/str_format.h"
#include "tcmalloc/common.h"
#include "tcmalloc/guarded_page_allocator.h"
#include "tcmalloc/internal/logging.h"
#include "tcmalloc/internal/page_size.h"
#include "tcmalloc/malloc_extension.h"
#include "tcmalloc/static_vars.h"

namespace tcmalloc::tcmalloc_internal {
namespace {

constexpr size_t kMaxGpaPages = GuardedPageAllocator::kGpaMaxPages;

// Size of the pages handed out by GuardedPageAllocator.
size_t PageSize() {
  static const size_t page_size =
      std::max(kPageSize, static_cast<size_t>(GetPageSize()));
  return page_size;
}

StackTrace GetStackTrace() {
self:
  StackTrace s;
  s.stack[0] = reinterpret_cast<void*>(&&self);
  s.depth = 1;
  return s;
}

// The allocator is shared by every input.  Init() takes its slot metadata from
// TCMalloc's arena, which never returns memory, so building a fresh allocator
// per input would grow without bound.  Carrying state over also lets a slot be
// quarantined by one mechanism and released by another, which is the
// interesting part of the state space.
GuardedPageAllocator& Allocator() {
  static GuardedPageAllocator* allocator = [] {
    auto* gpa = new GuardedPageAllocator();
    PageHeapSpinLockHolder l;
    gpa->Init(kMaxGpaPages, kMaxGpaPages);
    gpa->AllowAllocations();
    return gpa;
  }();
  return *allocator;
}

void* PageBase(void* ptr) {
  return reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(ptr) &
                                 ~(PageSize() - 1));
}

struct Allocation {
  char* ptr;
  size_t size;
  uint8_t pattern;
};

class State {
 public:
  ~State() {
    for (const Allocation& allocation : allocs_) {
      Verify(allocation);
      Allocator().Deallocate(allocation.ptr);
    }
    for (void* page : locked_) {
      TC_CHECK_EQ(munlock(page, PageSize()), 0);
    }
    if (locked_all_) {
      TC_CHECK_EQ(munlockall(), 0);
    }
  }

  std::vector<Allocation>& allocs() { return allocs_; }

  // Records that page is locked, so that it is released even if the input
  // never unlocks it.
  void MarkLocked(void* page) {
    if (std::find(locked_.begin(), locked_.end(), page) == locked_.end()) {
      locked_.push_back(page);
    }
  }

  void MarkUnlocked(void* page) {
    locked_.erase(std::remove(locked_.begin(), locked_.end(), page),
                  locked_.end());
  }

  void set_locked_all(bool locked_all) { locked_all_ = locked_all; }

  uint8_t NextPattern() { return ++pattern_; }

  // A live allocation must read back exactly what was written to it.
  // Quarantining and releasing pages must not disturb their contents.
  static void Verify(const Allocation& allocation) {
    for (size_t i = 0; i < allocation.size; ++i) {
      TC_CHECK_EQ(static_cast<uint8_t>(allocation.ptr[i]), allocation.pattern);
    }
  }

 private:
  std::vector<Allocation> allocs_;
  std::vector<void*> locked_;
  bool locked_all_ = false;
  uint8_t pattern_ = 0;
};

struct Allocate {
  uint16_t size;
  uint8_t alignment_shift;

  template <typename Sink>
  friend void AbslStringify(Sink& sink, const Allocate& a) {
    absl::Format(&sink, "Allocate{.size=%v, .alignment_shift=%v}", a.size,
                 a.alignment_shift);
  }

  void Perform(State& state) const {
    const size_t page_size = PageSize();
    const size_t alloc_size = size % (page_size + 1);
    const size_t alignment =
        alignment_shift == 0
            ? 0
            : size_t{1} << (alignment_shift % absl::bit_width(page_size));

    const GuardedAllocWithStatus result = Allocator().Allocate(
        alloc_size, static_cast<std::align_val_t>(alignment), GetStackTrace());
    if (result.status != Profile::Sample::GuardedStatus::Guarded) {
      TC_CHECK_EQ(result.alloc, nullptr);
      return;
    }

    char* ptr = static_cast<char*>(result.alloc);
    TC_CHECK_NE(ptr, nullptr);
    TC_CHECK(Allocator().PointerIsMine(ptr));
    TC_CHECK_EQ(Allocator().GetRequestedSize(ptr), alloc_size);
    if (alignment != 0) {
      TC_CHECK_EQ(reinterpret_cast<uintptr_t>(ptr) % alignment, 0);
    }

    // Slots are handed out at most once at a time.
    for (const Allocation& live : state.allocs()) {
      TC_CHECK_NE(PageBase(live.ptr), PageBase(ptr));
    }

    // Zero-byte allocations leave the page quarantined, so they are never
    // written to.
    const uint8_t pattern = alloc_size == 0 ? 0 : state.NextPattern();
    memset(ptr, pattern, alloc_size);
    state.allocs().push_back({ptr, alloc_size, pattern});
  }
};

struct Deallocate {
  uint8_t index;

  template <typename Sink>
  friend void AbslStringify(Sink& sink, const Deallocate& d) {
    absl::Format(&sink, "Deallocate{.index=%v}", d.index);
  }

  void Perform(State& state) const {
    if (state.allocs().empty()) return;
    const size_t i = index % state.allocs().size();
    const Allocation allocation = state.allocs()[i];
    State::Verify(allocation);
    state.allocs().erase(state.allocs().begin() + i);
    // The page may be locked.  Quarantining it has to work anyway.
    Allocator().Deallocate(allocation.ptr);
  }
};

struct Touch {
  uint8_t index;

  template <typename Sink>
  friend void AbslStringify(Sink& sink, const Touch& t) {
    absl::Format(&sink, "Touch{.index=%v}", t.index);
  }

  void Perform(State& state) const {
    if (state.allocs().empty()) return;
    Allocation& allocation = state.allocs()[index % state.allocs().size()];
    if (allocation.size == 0) return;
    State::Verify(allocation);
    allocation.pattern = state.NextPattern();
    memset(allocation.ptr, allocation.pattern, allocation.size);
  }
};

struct Lock {
  uint8_t index;

  template <typename Sink>
  friend void AbslStringify(Sink& sink, const Lock& l) {
    absl::Format(&sink, "Lock{.index=%v}", l.index);
  }

  void Perform(State& state) const {
    if (state.allocs().empty()) return;
    void* page = PageBase(state.allocs()[index % state.allocs().size()].ptr);
    // RLIMIT_MEMLOCK may not permit this.
    if (mlock(page, PageSize()) != 0) return;
    state.MarkLocked(page);
  }
};

struct Unlock {
  uint8_t index;

  template <typename Sink>
  friend void AbslStringify(Sink& sink, const Unlock& u) {
    absl::Format(&sink, "Unlock{.index=%v}", u.index);
  }

  void Perform(State& state) const {
    if (state.allocs().empty()) return;
    void* page = PageBase(state.allocs()[index % state.allocs().size()].ptr);
    if (munlock(page, PageSize()) != 0) return;
    state.MarkUnlocked(page);
  }
};

struct LockAll {
  template <typename Sink>
  friend void AbslStringify(Sink& sink, const LockAll&) {
    absl::Format(&sink, "LockAll{}");
  }

  void Perform(State& state) const {
    // RLIMIT_MEMLOCK rarely permits locking the entire address space, but when
    // it does every page of the pool is covered at once.
    if (mlockall(MCL_CURRENT) != 0) return;
    state.set_locked_all(true);
  }
};

struct UnlockAll {
  template <typename Sink>
  friend void AbslStringify(Sink& sink, const UnlockAll&) {
    absl::Format(&sink, "UnlockAll{}");
  }

  void Perform(State& state) const {
    TC_CHECK_EQ(munlockall(), 0);
    state.set_locked_all(false);
    // munlockall() drops VM_LOCKED from every VMA, including the ones
    // individual Lock instructions established.
    for (Allocation& allocation : state.allocs()) {
      state.MarkUnlocked(PageBase(allocation.ptr));
    }
  }
};

using Instruction =
    std::variant<Allocate, Deallocate, Touch, Lock, Unlock, LockAll, UnlockAll>;

void FuzzGuardedPageAllocator(const std::vector<Instruction>& instructions) {
  State state;
  for (const Instruction& instruction : instructions) {
    std::visit([&](const auto& arg) { arg.Perform(state); }, instruction);
  }
}

FUZZ_TEST(GuardedPageAllocatorTest, FuzzGuardedPageAllocator)
    .WithDomains(fuzztest::Arbitrary<std::vector<Instruction>>());

// Deallocating a page that the application has locked must quarantine it and
// leave the rest of the pool usable.
TEST(GuardedPageAllocatorTest, FuzzMlockedDeallocation) {
  FuzzGuardedPageAllocator({
      Allocate{.size = 128, .alignment_shift = 0},
      Lock{.index = 0},
      Deallocate{.index = 0},
      Allocate{.size = 128, .alignment_shift = 0},
      Touch{.index = 0},
  });
}

// A page released while its mapping is locked comes back through a different
// mechanism than the one that quarantined it.
TEST(GuardedPageAllocatorTest, FuzzMlockedReuse) {
  FuzzGuardedPageAllocator({
      Allocate{.size = 4096, .alignment_shift = 0},
      Lock{.index = 0},
      Deallocate{.index = 0},
      Allocate{.size = 4096, .alignment_shift = 0},
      Unlock{.index = 0},
      Touch{.index = 0},
      Deallocate{.index = 0},
      Allocate{.size = 4096, .alignment_shift = 0},
      Touch{.index = 0},
  });
}

}  // namespace
}  // namespace tcmalloc::tcmalloc_internal
