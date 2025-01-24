// Copyright 2022 The TCMalloc Authors
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

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

#include "gtest/gtest.h"
#include "fuzztest/fuzztest.h"
#include "absl/base/attributes.h"
#include "absl/container/flat_hash_set.h"
#include "absl/log/check.h"
#include "tcmalloc/huge_cache.h"
#include "tcmalloc/huge_pages.h"
#include "tcmalloc/huge_region.h"
#include "tcmalloc/internal/logging.h"
#include "tcmalloc/pages.h"
#include "tcmalloc/stats.h"

namespace tcmalloc::tcmalloc_internal {
namespace {

class MockUnback final : public MemoryModifyFunction {
 public:
  [[nodiscard]] bool operator()(Range r) override {
    release_callback_();

    if (!unback_success_) {
      return false;
    }

    PageId end = r.p + r.n;
    for (; r.p != end; ++r.p) {
      released_.insert(r.p);
    }

    return true;
  }

  absl::flat_hash_set<PageId> released_;
  bool unback_success_ = true;
  std::function<void()> release_callback_;
};

void FuzzRegion(const std::string& s) {
  const char* data = s.data();
  size_t size = s.size();
  if (size < 4) {
    return;
  }
  // data[0][0]  - Simulate reentrancy from release.
  // data[1...3] - Reserved
  //
  // TODO(b/271282540): Convert these to strongly typed fuzztest parameters.
  const bool reentrant_release = data[0] & 0x1;

  data += 4;
  size -= 4;

  const HugePage start =
      HugePageContaining(reinterpret_cast<void*>(0x1faced200000));
  MockUnback unback;
  HugeRegion region({start, region.size()}, unback);

  unback.released_.reserve(region.size().in_pages().raw_num());
  for (PageId p = start.first_page(), end = p + region.size().in_pages();
       p != end; ++p) {
    unback.released_.insert(p);
  }

  std::vector<Range> allocs;
  std::vector<std::pair<const char*, size_t>> reentrant;

  std::string output;
  output.resize(1 << 20);

  auto run_dsl = [&](const char* data, size_t size) {
    for (size_t i = 0; i + 9 <= size; i += 9) {
      const uint8_t op = data[i];
      uint64_t value;
      memcpy(&value, &data[i + 1], sizeof(value));

      switch (op & 0x7) {
        case 0: {
          // Allocate.
          //
          // value[0:17] - Length to allocate
          const Length n = Length(std::max<size_t>(value & ((1 << 18) - 1), 1));
          PageId p;
          bool from_released;
          if (!region.MaybeGet(n, &p, &from_released)) {
            continue;
          }

          allocs.emplace_back(p, n);

          if (from_released) {
            bool did_release = false;

            for (PageId q = p, end = p + n; q != end; ++q) {
              auto it = unback.released_.find(q);
              if (it != unback.released_.end()) {
                unback.released_.erase(it);
                did_release = true;
              }
            }

            CHECK(did_release);
          }

          break;
        }
        case 1: {
          // Deallocate.
          //
          // value[0:17] - Index of allocs to remove.
          // value[18] - Release
          if (allocs.empty()) {
            continue;
          }

          int index = value & ((1 << 18) - 1);
          const bool release = (value >> 18) & 0x1;
          index %= allocs.size();

          auto alloc = allocs[index];
          using std::swap;
          swap(allocs[index], allocs.back());
          allocs.resize(allocs.size() - 1);

          region.Put(alloc, release);
          break;
        }
        case 2: {
          // Release
          // value[0:17] - Length to release.
          const Length len = Length(value & ((1 << 18) - 1));
          const HugeLength max_expected =
              std::min(region.free_backed(), HLFromPages(len));

          const HugeLength actual = region.Release(len);
          if (unback.unback_success_) {
            if (max_expected > NHugePages(0) && len > Length(0)) {
              TC_CHECK_GT(actual, NHugePages(0));
            }
            TC_CHECK_LE(actual, max_expected);
          } else {
            TC_CHECK_EQ(actual, NHugePages(0));
          }
          break;
        }
        case 3: {
          // Stats
          region.stats();
          SmallSpanStats small;
          LargeSpanStats large;
          region.AddSpanStats(&small, &large);
          break;
        }
        case 4: {
          // Toggle
          unback.unback_success_ = !unback.unback_success_;
          break;
        }
        case 5: {
          // Not quite a runtime parameter:  Interpret value as a subprogram
          // in our dsl.
          size_t subprogram = std::min(size - i - 9, value);
          if (subprogram < 9) {
            break;
          }
          reentrant.emplace_back(data + i + 9, subprogram);
          i += size;
          break;
        }
        case 6: {
          // Gather stats in pbtxt format.
          //
          // value is unused.
          Printer p(&output[0], output.size());
          {
            PbtxtRegion r(p, kTop);
            region.PrintInPbtxt(r);
          }
          CHECK_LE(p.SpaceRequired(), output.size());
          break;
        }
        case 7: {
          // Print stats.
          //
          // value is unused.
          Printer p(&output[0], output.size());
          region.Print(p);
          break;
        }
      }
    }
  };

  unback.release_callback_ = [&]() {
    if (!reentrant_release) {
      return;
    }

    if (reentrant.empty()) {
      return;
    }

    ABSL_CONST_INIT static int depth = 0;
    if (depth >= 5) {
      return;
    }

    auto [data, size] = reentrant.back();
    reentrant.pop_back();

    depth++;
    run_dsl(data, size);
    depth--;
  };

  run_dsl(data, size);

  // Stop recursing, since region.Put below might cause us to "release"
  // more pages to the system.
  reentrant.clear();

  for (const auto& alloc : allocs) {
    region.Put(alloc, false);
  }
}

FUZZ_TEST(HugeRegionTest, FuzzRegion)
    ;

TEST(HugeRegionTest, b339521569) {
  FuzzRegion(std::string(
      "L\220\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000"
      "\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000"
      "\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000"
      "\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000"
      "\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000"
      "\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000"
      "\000\000\000\000\000\301\233",
      115));
}

}  // namespace
}  // namespace tcmalloc::tcmalloc_internal
