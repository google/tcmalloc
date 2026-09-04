// Copyright 2021 The TCMalloc Authors
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

#include "tcmalloc/arena.h"

#include <stdint.h>

#include <cstddef>
#include <new>

#include "gtest/gtest.h"
#include "absl/base/internal/spinlock.h"
#include "tcmalloc/common.h"

namespace tcmalloc {
namespace tcmalloc_internal {
namespace {

std::align_val_t Align(int align) {
  return static_cast<std::align_val_t>(align);
}

TEST(Arena, AlignedAlloc) {
  Arena arena;
  EXPECT_EQ(reinterpret_cast<uintptr_t>(arena.Alloc(64, Align(64))) % 64, 0);
  EXPECT_EQ(reinterpret_cast<uintptr_t>(arena.Alloc(7)) % 8, 0);
  EXPECT_EQ(reinterpret_cast<uintptr_t>(arena.Alloc(128, Align(64))) % 64, 0);
  for (int alignment = 1; alignment < 100; ++alignment) {
    EXPECT_EQ(reinterpret_cast<uintptr_t>(arena.Alloc(7, Align(alignment))) %
                  alignment,
              0);
  }
}
TEST(Arena, AlignedLargeAlloc) {
  for (int align = 1; align < 100000000; align *= 2) {
    Arena arena;
    for (int i = 0; i < 5; ++i) {
      EXPECT_EQ(
          reinterpret_cast<uintptr_t>(arena.Alloc(123, Align(align))) % align,
          0);
    }
  }
}

TEST(Arena, Stats) {
  Arena arena;

  ArenaStats stats = arena.stats();
  EXPECT_EQ(stats.bytes_allocated, 0);
  EXPECT_EQ(stats.bytes_unallocated, 0);
  EXPECT_EQ(stats.bytes_unavailable, 0);
  EXPECT_EQ(stats.bytes_nonresident, 0);
  EXPECT_EQ(stats.blocks, 0);

  // Trigger an allocation and grab new stats.
  void* ptr = arena.Alloc(1, Align(1));
  ArenaStats stats_after_alloc = arena.stats();

  EXPECT_NE(ptr, nullptr);

  EXPECT_EQ(stats_after_alloc.bytes_allocated, 1);
  EXPECT_GE(stats_after_alloc.bytes_unallocated, 0);
  EXPECT_EQ(stats_after_alloc.bytes_unavailable, 0);
  EXPECT_EQ(stats_after_alloc.bytes_nonresident, 0);
  EXPECT_EQ(stats_after_alloc.blocks, 1);
  EXPECT_EQ(stats_after_alloc.freelist_blocks, 0);

  // Trigger an allocation that is larger than the remaining free bytes.
  //
  // b/201694482: The remaining bytes from the first block are freelisted rather
  // than being wasted, with only 7 bytes lost to alignment padding for Block.
  ptr = arena.Alloc(stats_after_alloc.bytes_unallocated + 1, Align(1));
  ArenaStats stats_after_alloc2 = arena.stats();
  EXPECT_NE(ptr, nullptr);

  EXPECT_EQ(stats_after_alloc2.bytes_allocated,
            stats_after_alloc.bytes_unallocated + 2);
  EXPECT_GE(stats_after_alloc2.bytes_unallocated, 0);
  EXPECT_EQ(stats_after_alloc2.bytes_unavailable, 7);
  EXPECT_EQ(stats_after_alloc.bytes_nonresident, 0);
  EXPECT_EQ(stats_after_alloc2.blocks, 2);
  EXPECT_EQ(stats_after_alloc2.freelist_blocks, 1);

  EXPECT_EQ(stats_after_alloc2.bytes_allocated, arena.allocated());
}

TEST(Arena, ReportUnmapped) {
  Arena arena;
  void* ptr = arena.Alloc(10, Align(1));
  ArenaStats stats_after_alloc = arena.stats();
  EXPECT_NE(ptr, nullptr);

  EXPECT_EQ(stats_after_alloc.bytes_allocated, 10);
  EXPECT_EQ(stats_after_alloc.bytes_nonresident, 0);

  arena.UpdateAllocatedAndNonresident(-5, 5);
  stats_after_alloc = arena.stats();

  EXPECT_EQ(stats_after_alloc.bytes_allocated, 5);
  EXPECT_EQ(stats_after_alloc.bytes_nonresident, 5);

  arena.UpdateAllocatedAndNonresident(3, -3);
  stats_after_alloc = arena.stats();

  EXPECT_EQ(stats_after_alloc.bytes_allocated, 8);
  EXPECT_EQ(stats_after_alloc.bytes_nonresident, 2);
}

TEST(Arena, BytesImpending) {
  Arena arena;

  ArenaStats stats = arena.stats();
  EXPECT_EQ(stats.bytes_allocated, 0);

  arena.UpdateAllocatedAndNonresident(100, 0);
  stats = arena.stats();

  EXPECT_EQ(stats.bytes_allocated, 100);

  arena.UpdateAllocatedAndNonresident(-100, 0);
  void* ptr = arena.Alloc(100, Align(1));
  stats = arena.stats();

  EXPECT_NE(ptr, nullptr);
  EXPECT_EQ(stats.bytes_allocated, 100);
}

TEST(Arena, FreelistReuse) {
  Arena arena;
  // Step 1: Allocate from initial arena block and record remaining capacity.
  void* p1 = arena.Alloc(104, Align(8));
  EXPECT_NE(p1, nullptr);
  ArenaStats s1 = arena.stats();
  EXPECT_EQ(s1.blocks, 1);
  EXPECT_EQ(s1.bytes_allocated, 104);
  size_t b1_remaining = s1.bytes_unallocated;

  // Step 2: Request more bytes than remain in block 1.
  // This pushes block 1's remainder onto freelist_ and allocates block 2 from
  // the OS.
  void* p2 = arena.Alloc(b1_remaining + 104, Align(8));
  EXPECT_NE(p2, nullptr);
  ArenaStats s2 = arena.stats();
  EXPECT_EQ(s2.blocks, 2);
  EXPECT_EQ(s2.bytes_unavailable, 0);

  // Step 3: Drain block 2 down to 100 bytes so it cannot satisfy a 1000-byte
  // request. The if check protects against underflow in case block 2 is already
  // <= 100 bytes.
  size_t b2_remaining = s2.bytes_unallocated > b1_remaining
                            ? s2.bytes_unallocated - b1_remaining
                            : 0;
  if (b2_remaining > 100) {
    void* p_drain = arena.Alloc(b2_remaining - 100, Align(8));
    EXPECT_NE(p_drain, nullptr);
  }

  // Step 4: Request 1000 bytes. Block 2 cannot satisfy this (only 100 bytes
  // left), but block 1 on freelist_ can. Arena should reuse block 1 without
  // allocating block 3.
  void* p3 = arena.Alloc(1000, Align(8));
  EXPECT_NE(p3, nullptr);
  ArenaStats s3 = arena.stats();
  EXPECT_EQ(s3.blocks, 2);
  EXPECT_EQ(s3.bytes_unavailable, 0);
}

TEST(Arena, FreelistMultipleBlocks) {
  Arena arena;
  // Step 1: Allocate from block 1, then drain it down to 5000 bytes remaining.
  void* p1 = arena.Alloc(128, Align(8));
  EXPECT_NE(p1, nullptr);
  ArenaStats s1 = arena.stats();
  size_t to_drain =
      s1.bytes_unallocated > 5000 ? s1.bytes_unallocated - 5000 : 0;
  if (to_drain > 0) {
    void* p_drain1 = arena.Alloc(to_drain, Align(8));
    EXPECT_NE(p_drain1, nullptr);
  }

  // Step 2: Request 10000 bytes. Block 1 (5000 bytes left) is pushed to
  // freelist_, and block 2 is allocated from the OS.
  void* p2 = arena.Alloc(10000, Align(8));
  EXPECT_NE(p2, nullptr);
  ArenaStats s2 = arena.stats();
  EXPECT_EQ(s2.blocks, 2);
  EXPECT_EQ(s2.bytes_unavailable, 0);

  // Step 3: Drain block 2 down to 3000 bytes remaining.
  size_t b2_avail =
      s2.bytes_unallocated > 5000 ? s2.bytes_unallocated - 5000 : 0;
  if (b2_avail > 3000) {
    void* p_drain2 = arena.Alloc(b2_avail - 3000, Align(8));
    EXPECT_NE(p_drain2, nullptr);
  }

  // Step 4: Request 10000 bytes. Neither active block 2 (3000 left) nor
  // freelisted block 1 (5000 left) can satisfy this. Block 2 is pushed to
  // freelist_, and block 3 is allocated from the OS.
  void* p3 = arena.Alloc(10000, Align(8));
  EXPECT_NE(p3, nullptr);
  ArenaStats s3 = arena.stats();
  EXPECT_EQ(s3.blocks, 3);
  EXPECT_EQ(s3.bytes_unavailable, 0);

  // Step 5: Drain active block 3 down to 1000 bytes remaining.
  size_t b3_avail = s3.bytes_unallocated > 5000 + 3000
                        ? s3.bytes_unallocated - 5000 - 3000
                        : 0;
  if (b3_avail > 1000) {
    void* p_drain3 = arena.Alloc(b3_avail - 1000, Align(8));
    EXPECT_NE(p_drain3, nullptr);
  }

  // Step 6: Request 2000 bytes. Active block 3 (1000 left) cannot satisfy
  // this, but block 2 on freelist_ (3000 left) can. It is reused without
  // adding a new OS block.
  void* p4 = arena.Alloc(2000, Align(8));
  EXPECT_NE(p4, nullptr);
  ArenaStats s4 = arena.stats();
  EXPECT_EQ(s4.blocks, 3);
  EXPECT_EQ(s4.bytes_unavailable, 0);

  // Step 7: Request 4000 bytes. Block 1 on freelist_ (5000 left) satisfies
  // this.
  void* p5 = arena.Alloc(4000, Align(8));
  EXPECT_NE(p5, nullptr);
  ArenaStats s5 = arena.stats();
  EXPECT_EQ(s5.blocks, 3);
  EXPECT_EQ(s5.bytes_unavailable, 0);
}

TEST(Arena, SmallRemainderUnavailable) {
  Arena arena;
  // Step 1: Allocate 1 byte to initialize the first OS block and record its
  // capacity.
  void* p1 = arena.Alloc(1, Align(1));
  EXPECT_NE(p1, nullptr);
  ArenaStats s1 = arena.stats();
  size_t rem = s1.bytes_unallocated;
  EXPECT_EQ(s1.freelist_blocks, 0);
  EXPECT_EQ(s1.bytes_unavailable, 0);
  EXPECT_EQ(s1.freelist_blocks, 0);

  // Step 2: Consume all but 4 bytes of the first block.
  void* p2 = arena.Alloc(rem - 4, Align(1));
  EXPECT_NE(p2, nullptr);
  ArenaStats s2 = arena.stats();
  EXPECT_EQ(s2.freelist_blocks, 0);
  EXPECT_EQ(s2.bytes_unallocated, 4);
  EXPECT_EQ(s2.bytes_unavailable, 0);
  EXPECT_EQ(s2.freelist_blocks, 0);

  // Step 3: Request 100 bytes. The remaining 4 bytes cannot satisfy the request
  // and are too small (< sizeof(Block) == 16) to be placed on freelist_. They
  // are marked as bytes_unavailable, and block 2 is allocated from the OS.
  void* p3 = arena.Alloc(100, Align(1));
  ArenaStats s3 = arena.stats();
  EXPECT_EQ(s3.bytes_unavailable, 4);
  EXPECT_EQ(s3.blocks, 2);
  EXPECT_EQ(s3.freelist_blocks, 0);
  EXPECT_NE(p3, nullptr);
}

TEST(Arena, FreelistAlignmentEdgeCase) {
  Arena arena;
  // Step 1: Initialize block 1 and record its remaining capacity.
  void* p1 = arena.Alloc(1, Align(1));
  EXPECT_NE(p1, nullptr);
  ArenaStats s1 = arena.stats();
  size_t rem = s1.bytes_unallocated;
  EXPECT_GT(rem, 200);

  // Step 2: Leave exactly 134 bytes remaining in block 1.
  void* p2 = arena.Alloc(rem - 134, Align(1));
  EXPECT_NE(p2, nullptr);
  ArenaStats s2 = arena.stats();
  EXPECT_EQ(s2.bytes_unallocated, 134);

  // Step 3: Request 2000 bytes. Block 1 (134 bytes left) is placed on
  // freelist_, and block 2 is allocated from the OS.
  void* p3 = arena.Alloc(2000, Align(64));
  EXPECT_NE(p3, nullptr);
  ArenaStats s3 = arena.stats();
  EXPECT_EQ(s3.blocks, 2);

  // Step 4: Drain active block 2 down to 10 bytes remaining.
  size_t b2_rem = s3.bytes_unallocated > 134 ? s3.bytes_unallocated - 134 : 0;
  if (b2_rem > 10) {
    void* p_drain = arena.Alloc(b2_rem - 10, Align(1));
    EXPECT_NE(p_drain, nullptr);
  }

  // Step 5: Request 150 bytes with 64-byte alignment.
  // Although block 1 on freelist_ has 134 bytes, satisfying a 150-byte request
  // exceeds 134 bytes. Thus block 1 cannot be used, and block 3 is allocated
  // from the OS.
  void* p4 = arena.Alloc(150, Align(64));
  EXPECT_NE(p4, nullptr);
  ArenaStats s4 = arena.stats();
  EXPECT_EQ(s4.blocks, 3);

  // Step 6: Request 80 bytes with 4-byte alignment.
  // With smaller size and alignment requirements, block 1 on freelist_ (134
  // bytes) can satisfy the request and is reused without allocating an OS
  // block.
  void* p5 = arena.Alloc(80, Align(4));
  EXPECT_NE(p5, nullptr);
  ArenaStats s5 = arena.stats();
  EXPECT_EQ(s5.blocks, 3);
}

TEST(Arena, FreelistLimit) {
  Arena dummy_arena;
  dummy_arena.Alloc(1, Align(1));
  size_t block_size = dummy_arena.stats().bytes_unallocated + 1;

  // We want to force the freelist to grow beyond 100 blocks.
  // By default, the freelist limit is 100.
  // We allocate 2/3 of a block on each call. Because the remaining 1/3 of the
  // block is less than the requested 2/3, each new allocation will force the
  // remainder of the active block to be stashed on the freelist and a new block
  // to be allocated. Because the stashed blocks (size 1/3) are smaller than the
  // request size (2/3), they can never be reused, causing the freelist to grow
  // monotonically. Only the first 100 blocks should be kept on the freelist.
  // We align X to 8 bytes to avoid alignment padding during stashing.
  size_t X = ((block_size * 2) / 3) & ~7;

  Arena arena;
  for (int i = 0; i < 105; ++i) {
    void* p = arena.Alloc(X, Align(1));
    EXPECT_NE(p, nullptr);
  }

  ArenaStats stats = arena.stats();
  EXPECT_EQ(stats.freelist_blocks, 100);

  size_t expected_unallocated = 101 * (block_size - X);
  EXPECT_EQ(stats.bytes_unallocated, expected_unallocated);

  EXPECT_EQ(stats.bytes_allocated, X * 105);
}

}  // namespace
}  // namespace tcmalloc_internal
}  // namespace tcmalloc
