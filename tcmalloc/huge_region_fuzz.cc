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
#include <cstring>
#include <functional>
#include <type_traits>
#include <utility>
#include <variant>
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
#include "tcmalloc/internal/system_allocator.h"
#include "tcmalloc/pages.h"
#include "tcmalloc/stats.h"

namespace tcmalloc::tcmalloc_internal {
namespace {

class MockUnback final : public MemoryModifyFunction {
 public:
  [[nodiscard]] MemoryModifyStatus operator()(Range r) override {
    release_callback_();

    if (!unback_success_) {
      return {.success = false, .error_number = 0};
    }

    PageId end = r.p + r.n;
    for (; r.p != end; ++r.p) {
      released_.insert(r.p);
    }

    return {.success = true, .error_number = 0};
  }

  absl::flat_hash_set<PageId> released_;
  bool unback_success_ = true;
  std::function<void()> release_callback_;
};

struct Allocate {
  uint32_t length;

  template <typename Sink>
  friend void AbslStringify(Sink& sink, const Allocate& a) {
    absl::Format(&sink, "Allocate{.length=%d}", a.length);
  }
};

struct Deallocate {
  uint32_t index;
  bool release;

  template <typename Sink>
  friend void AbslStringify(Sink& sink, const Deallocate& d) {
    absl::Format(&sink, "Deallocate{.index=%d, .release=%v}", d.index,
                 d.release);
  }
};

struct Release {
  uint32_t length;

  template <typename Sink>
  friend void AbslStringify(Sink& sink, const Release& r) {
    absl::Format(&sink, "Release{.length=%d}", r.length);
  }
};

struct Stats {
  template <typename Sink>
  friend void AbslStringify(Sink& sink, const Stats&) {
    sink.Append("Stats");
  }
};

struct Toggle {
  template <typename Sink>
  friend void AbslStringify(Sink& sink, const Toggle&) {
    sink.Append("Toggle");
  }
};

struct Reentrant;

struct GatherStatsPbtxt {
  template <typename Sink>
  friend void AbslStringify(Sink& sink, const GatherStatsPbtxt&) {
    sink.Append("GatherStatsPbtxt");
  }
};

struct PrintStats {
  template <typename Sink>
  friend void AbslStringify(Sink& sink, const PrintStats&) {
    sink.Append("PrintStats");
  }
};

using Instruction = std::variant<Allocate, Deallocate, Release, Stats, Toggle,
                                 Reentrant, GatherStatsPbtxt, PrintStats>;

struct Reentrant {
  std::vector<Instruction> subprogram;
};

template <typename Sink>
void AbslStringify(Sink& sink, const Reentrant& r) {
  absl::Format(
      &sink, "Reentrant{.subprogram={%s}}",
      absl::StrJoin(
          r.subprogram, ", ", [](std::string* out, const Instruction& i) {
            std::visit(
                [&](auto&& arg) { absl::StrAppend(out, absl::StrCat(arg)); },
                i);
          }));
}
void FuzzRegion(const std::vector<Instruction>& instructions,
                bool reentrant_release) {
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
  std::vector<std::vector<Instruction>> reentrant_stack;

  std::string output;
  output.resize(1 << 20);

  auto run_instructions = [&](auto& self,
                              const std::vector<Instruction>& instrs) -> void {
    for (const auto& inst : instrs) {
      std::visit(
          [&](auto&& arg) {
            using T = std::decay_t<decltype(arg)>;
            if constexpr (std::is_same_v<T, Allocate>) {
              const Length n =
                  Length(std::max<size_t>(arg.length % (1 << 18), 1));
              PageId p;
              bool from_released;
              if (!region.MaybeGet(n, &p, &from_released)) {
                return;
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
            } else if constexpr (std::is_same_v<T, Deallocate>) {
              if (allocs.empty()) return;
              int index = arg.index % allocs.size();
              const bool release = arg.release;
              auto alloc = allocs[index];
              using std::swap;
              swap(allocs[index], allocs.back());
              allocs.resize(allocs.size() - 1);
              region.Put(alloc, release);
            } else if constexpr (std::is_same_v<T, Release>) {
              const Length len = Length(arg.length % (1 << 18));
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
            } else if constexpr (std::is_same_v<T, Stats>) {
              region.stats();
              SmallSpanStats small;
              LargeSpanStats large;
              region.AddSpanStats(&small, &large);
            } else if constexpr (std::is_same_v<T, Toggle>) {
              unback.unback_success_ = !unback.unback_success_;
            } else if constexpr (std::is_same_v<T, Reentrant>) {
              reentrant_stack.push_back(arg.subprogram);
            } else if constexpr (std::is_same_v<T, GatherStatsPbtxt>) {
              Printer p(&output[0], output.size());
              {
                PbtxtRegion r(p, kTop);
                region.PrintInPbtxt(r);
              }
              CHECK_LE(p.SpaceRequired(), output.size());
            } else if constexpr (std::is_same_v<T, PrintStats>) {
              Printer p(&output[0], output.size());
              region.Print(p);
            }
          },
          inst);
    }
  };

  unback.release_callback_ = [&]() {
    if (!reentrant_release) return;
    if (reentrant_stack.empty()) return;
    ABSL_CONST_INIT static int depth = 0;
    if (depth >= 5) return;

    auto prog = std::move(reentrant_stack.back());
    reentrant_stack.pop_back();

    depth++;
    run_instructions(run_instructions, prog);
    depth--;
  };

  run_instructions(run_instructions, instructions);

  reentrant_stack.clear();
  for (const auto& alloc : allocs) {
    region.Put(alloc, false);
  }
}

fuzztest::Domain<Instruction> GetInstructionDomain(int depth);

auto GetFlatInstructionDomain() {
  return fuzztest::OneOf(
      fuzztest::Map([](Allocate a) -> Instruction { return Instruction{a}; },
                    fuzztest::Arbitrary<Allocate>()),
      fuzztest::Map([](Deallocate d) -> Instruction { return Instruction{d}; },
                    fuzztest::Arbitrary<Deallocate>()),
      fuzztest::Map([](Release r) -> Instruction { return Instruction{r}; },
                    fuzztest::Arbitrary<Release>()),
      fuzztest::Map([](Stats s) -> Instruction { return Instruction{s}; },
                    fuzztest::Arbitrary<Stats>()),
      fuzztest::Map([](Toggle t) -> Instruction { return Instruction{t}; },
                    fuzztest::Arbitrary<Toggle>()),
      fuzztest::Map(
          [](GatherStatsPbtxt g) -> Instruction { return Instruction{g}; },
          fuzztest::Arbitrary<GatherStatsPbtxt>()),
      fuzztest::Map([](PrintStats p) -> Instruction { return Instruction{p}; },
                    fuzztest::Arbitrary<PrintStats>()));
}

fuzztest::Domain<Instruction> GetInstructionDomain(int depth) {
  if (depth <= 0) {
    return fuzztest::OneOf(
        GetFlatInstructionDomain(),
        fuzztest::Map(
            [](std::vector<Instruction> sub) -> Instruction {
              return Instruction{Reentrant{sub}};
            },
            fuzztest::VectorOf(fuzztest::Just(Instruction{Allocate{1}}))
                .WithSize(0)));
  } else {
    return fuzztest::OneOf(
        GetFlatInstructionDomain(),
        fuzztest::Map(
            [](std::vector<Instruction> sub) -> Instruction {
              return Instruction{Reentrant{sub}};
            },
            fuzztest::VectorOf(GetInstructionDomain(depth - 1))));
  }
}

FUZZ_TEST(HugeRegionTest, FuzzRegion)
    .WithDomains(fuzztest::VectorOf(GetInstructionDomain(5)),
                 fuzztest::Arbitrary<bool>());

TEST(HugeRegionTest, b339521569) {
  std::vector<Instruction> p = {
      Allocate{0},
  };

  FuzzRegion(p, false);
}

}  // namespace
}  // namespace tcmalloc::tcmalloc_internal
