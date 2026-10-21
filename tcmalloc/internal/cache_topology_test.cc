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

#include "tcmalloc/internal/cache_topology.h"

#include <errno.h>
#include <string.h>
#include <unistd.h>

#include <cstddef>
#include <optional>

#include "gtest/gtest.h"
#include "tcmalloc/internal/sysinfo.h"

namespace tcmalloc::tcmalloc_internal {
namespace {

int CreateCpuListFd(const char* content) {
  int pipefd[2];
  EXPECT_EQ(pipe(pipefd), 0);
  const size_t len = strlen(content);
  EXPECT_EQ(write(pipefd[1], content, len), len);
  EXPECT_EQ(close(pipefd[1]), 0);
  return pipefd[0];
}

TEST(CacheTopology, ComputesSomethingReasonable) {
  // This test verifies that each L3 cache serves the same number of CPU. This
  // is not a strict requirement for the correct operation of this code, but a
  // sign of sanity.
  CacheTopology topology;
  topology.Init();
  EXPECT_EQ(NumCPUs() % topology.l3_count(), 0);
  ASSERT_GT(topology.l3_count(), 0);
  static const int kMaxNodes = 256 / 8;
  int count_per_node[kMaxNodes] = {0};
  for (int i = 0, n = NumCPUs(); i < n; ++i) {
    count_per_node[topology.GetL3FromCpuId(i)]++;
  }
  for (int i = 0; i < topology.l3_count(); ++i) {
    EXPECT_EQ(count_per_node[i], NumCPUs() / topology.l3_count());
  }
}

TEST(CacheTopology, MissingSysfsCacheIndex3FallsBackToSingleL3) {
  CacheTopology topology;
  topology.InitForTest(
      [](size_t /*cpu*/) {
        errno = ENOENT;
        return -1;
      },
      /*maybe_numcpus=*/4);
  EXPECT_EQ(topology.l3_count(), 1);
  EXPECT_EQ(topology.GetL3FromCpuId(0), 0);
  EXPECT_EQ(topology.GetL3FromCpuId(1), 0);
  EXPECT_EQ(topology.GetL3FromCpuId(2), 0);
  EXPECT_EQ(topology.GetL3FromCpuId(3), 0);
}

TEST(CacheTopology, OfflineMidRangeCpuDoesNotTruncateTopology) {
  CacheTopology topology;
  topology.InitForTest(
      [](size_t cpu) {
        if (cpu == 0 || cpu == 1) {
          return CreateCpuListFd("0-1\n");
        }
        if (cpu == 2 || cpu == 3) {
          errno = ENOENT;
          return -1;
        }
        if (cpu == 4 || cpu == 5) {
          return CreateCpuListFd("4-5\n");
        }
        errno = ENOENT;
        return -1;
      },
      /*maybe_numcpus=*/6);
  EXPECT_EQ(topology.l3_count(), 2);
  EXPECT_EQ(topology.GetL3FromCpuId(0), 0);
  EXPECT_EQ(topology.GetL3FromCpuId(1), 0);
  EXPECT_EQ(topology.GetL3FromCpuId(2), 0);
  EXPECT_EQ(topology.GetL3FromCpuId(3), 0);
  EXPECT_EQ(topology.GetL3FromCpuId(4), 1);
  EXPECT_EQ(topology.GetL3FromCpuId(5), 1);
}

TEST(CacheTopology, MissingNumCpusFallsBackToSingleL3) {
  CacheTopology topology;
  topology.InitForTest(
      [](size_t /*cpu*/) {
        errno = ENOENT;
        return -1;
      },
      /*maybe_numcpus=*/std::nullopt);
  EXPECT_EQ(topology.l3_count(), 1);
  EXPECT_EQ(topology.GetL3FromCpuId(0), 0);
  EXPECT_EQ(topology.GetL3FromCpuId(NumCPUs() - 1), 0);
  EXPECT_EQ(topology.GetL3FromCpuId(2047), 0);
}

}  // namespace
}  // namespace tcmalloc::tcmalloc_internal
