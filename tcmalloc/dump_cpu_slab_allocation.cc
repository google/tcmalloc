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

#include <sys/mman.h>
#include <sys/prctl.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <new>

#include "absl/strings/string_view.h"
#include "tcmalloc/common.h"
#include "tcmalloc/cpu_cache.h"
#include "tcmalloc/internal/numa.h"
#include "tcmalloc/internal/percpu_tcmalloc.h"
#include "tcmalloc/mock_central_freelist.h"
#include "tcmalloc/mock_transfer_cache.h"
#include "tcmalloc/transfer_cache.h"

namespace tcmalloc {
namespace tcmalloc_internal {

// Only the bare minimum to get Init() and Activate() to work.
class DumpForwarder {
 public:
  DumpForwarder() { numa_topology_.Init(); }

  static void* Alloc(size_t size, std::align_val_t alignment) {
    return new char[size];
  }
  const NumaTopology<kNumaPartitions, kNumBaseClasses>& numa_topology() const {
    return numa_topology_;
  }
  using ShardedManager =
      ShardedTransferCacheManagerBase<FakeShardedTransferCacheManager,
                                      FakeCpuLayout,
                                      MinimalFakeCentralFreeList>;
  ShardedManager& sharded_transfer_cache() const { abort(); }
  size_t active_partitions() const { return 1; }
  size_t num_objects_to_move(size_t size_class) const { return 1; }
  size_t class_to_size(size_t size_class) const { return size_class * 8; }
  bool IsActive(size_t size_class) { return true; }
  size_t UseShardedCacheForLargeClassesOnly() const { return false; }
  bool per_cpu_caches_dynamic_slab_enabled() const { return false; }
  bool multiple_non_numa_partitions() const { return false; }
  void SetAnonVmaName(void* addr, size_t size, absl::string_view name) {}
  size_t UseWiderSlabs() const { return false; }
  bool reuse_size_classes() const { return true; }

 private:
  NumaTopology<kNumaPartitions, kNumBaseClasses> numa_topology_;
};

void DumpMaxCapacityForTest() {
  if (!subtle::percpu::IsFast()) {
    printf("Cannot run CpuCache test without rcache support.\n");
    return;
  }

  CpuCache<DumpForwarder> cache;
  cache.Init();
  cache.Activate();

  for (uint8_t shift = kInitialBasePerCpuShift; shift <= kMaxBasePerCpuShift;
       ++shift) {
    std::array<std::atomic<uint16_t>, kNumClasses> max_capacity;
    cache.CalculateMaxCapacityForAllClasses(shift, max_capacity.data());
    MaxCapacityFunctor get_capacity{max_capacity.data()};

    // Add space used for Header.
    printf(
        "Initial max capacity each size class for %u kB pages, %u kB slabs:\n",
        (1 << TCMALLOC_PAGE_SHIFT) / 1024, (1 << shift) / 1024);
    for (size_t size_class = 0; size_class < kNumClasses; ++size_class) {
      printf(" - %3zu: %5zu element(s)\n", size_class,
             get_capacity(size_class));
    }

    // Note that bytes_used includes Header metadata and guard pointers,
    // so it will be larger than just adding together all max_capacity[]
    // and multiplying by sizeof(void*).
    int bytes_used = EstimateSlabBytes(
        get_capacity,
        subtle::percpu::TcmallocSlab<kNumClasses>::GetTotalClassHeaderSize());
    printf("Total: %d bytes used, %d bytes unused.\n", bytes_used,
           (1 << shift) - bytes_used);
    printf("\n");
  }
}

}  // namespace tcmalloc_internal
}  // namespace tcmalloc

int main(int argc, char** argv) {
  tcmalloc::tcmalloc_internal::DumpMaxCapacityForTest();
}
