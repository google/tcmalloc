// Copyright 2020 The TCMalloc Authors
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
#include <random>
#include <type_traits>
#include <variant>
#include <vector>

#include "fuzztest/fuzztest.h"
#include "absl/log/check.h"
#include "absl/strings/str_format.h"
#include "absl/time/time.h"
#include "absl/types/span.h"
#include "tcmalloc/central_freelist.h"
#include "tcmalloc/common.h"
#include "tcmalloc/internal/config.h"
#include "tcmalloc/internal/logging.h"
#include "tcmalloc/mock_static_forwarder.h"
#include "tcmalloc/sizemap.h"
#include "tcmalloc/span_stats.h"

GOOGLE_MALLOC_SECTION_BEGIN
namespace tcmalloc::tcmalloc_internal {
namespace {

using CentralFreeList = central_freelist_internal::CentralFreeList<
    tcmalloc_internal::MockStaticForwarder>;
using CentralFreelistEnv = FakeCentralFreeListEnvironment<CentralFreeList>;

struct Allocate {
  uint8_t num_objects;
  template <typename Sink>
  friend void AbslStringify(Sink& sink, const Allocate& a) {
    absl::Format(&sink, "Allocate{.num_objects=%d}", a.num_objects);
  }
};

struct Deallocate {
  uint8_t num_objects;
  template <typename Sink>
  friend void AbslStringify(Sink& sink, const Deallocate& d) {
    absl::Format(&sink, "Deallocate{.num_objects=%d}", d.num_objects);
  }
};

struct Shuffle {
  int seed;
  template <typename Sink>
  friend void AbslStringify(Sink& sink, const Shuffle& s) {
    absl::Format(&sink, "Shuffle{.seed=%d}", s.seed);
  }
};

struct CheckStats {
  template <typename Sink>
  friend void AbslStringify(Sink& sink, const CheckStats&) {
    absl::Format(&sink, "CheckStats{}");
  }
};

struct PrintStats {
  template <typename Sink>
  friend void AbslStringify(Sink& sink, const PrintStats&) {
    absl::Format(&sink, "PrintStats{}");
  }
};

struct HandleLongLivedSpans {
  template <typename Sink>
  friend void AbslStringify(Sink& sink, const HandleLongLivedSpans&) {
    absl::Format(&sink, "HandleLongLivedSpans{}");
  }
};

struct AdvanceClock {
  uint32_t value;
  template <typename Sink>
  friend void AbslStringify(Sink& sink, const AdvanceClock& a) {
    absl::Format(&sink, "AdvanceClock{.value=%d}", a.value);
  }
};

using InstructionVariant =
    std::variant<Allocate, Deallocate, Shuffle, CheckStats, PrintStats,
                 HandleLongLivedSpans, AdvanceClock>;

struct Instruction {
  InstructionVariant instr;

  template <typename Sink>
  friend void AbslStringify(Sink& sink, const Instruction& i) {
    std::visit([&](auto&& arg) { AbslStringify(sink, arg); }, i.instr);
  }
};

template <class>
inline constexpr bool always_false_v = false;

void FuzzCFL(size_t object_size, size_t num_pages, size_t num_objects_to_move,
             const std::vector<Instruction>& instructions) {
  // TODO(271282540): Add support for multiple size classes for fuzzing.
  if (!SizeMap::IsValidSizeClass(object_size, num_pages, num_objects_to_move)) {
    return;
  }
  CentralFreelistEnv env(object_size, num_pages, num_objects_to_move);
  std::vector<void*> objects;

  for (const auto& instruction_wrapper : instructions) {
    std::visit(
        [&](auto&& arg) {
          using T = std::decay_t<decltype(arg)>;
          if constexpr (std::is_same_v<T, Allocate>) {
            void* batch[kMaxObjectsToMove];
            const size_t n = arg.num_objects;
            int allocated =
                env.central_freelist().RemoveRange(absl::MakeSpan(batch, n));
            objects.insert(objects.end(), batch, batch + allocated);
          } else if constexpr (std::is_same_v<T, Deallocate>) {
            if (objects.empty()) return;

            const size_t n = std::min<size_t>(arg.num_objects, objects.size());
            env.central_freelist().InsertRange(
                {&objects[objects.size() - n], n});
            objects.resize(objects.size() - n);
          } else if constexpr (std::is_same_v<T, Shuffle>) {
            // Shuffle allocated objects such that we don't return them in the
            // same order we allocated them.
            std::mt19937 rng(arg.seed);
            // Limit number of elements to shuffle so that we don't spend a lot
            // of time in shuffling a large number of objects.
            constexpr int kMaxToShuffle = 10 * kMaxObjectsToMove;
            if (objects.size() <= kMaxToShuffle) {
              std::shuffle(objects.begin(), objects.end(), rng);
            } else {
              std::shuffle(objects.end() - kMaxToShuffle, objects.end(), rng);
            }
          } else if constexpr (std::is_same_v<T, CheckStats>) {
            // Check stats.
            tcmalloc_internal::SpanStats stats =
                env.central_freelist().GetSpanStats();
            // Spans with objects_per_span = 1 skip most of the logic in the
            // central freelist including stats updates.  So skip the check for
            // objects_per_span = 1.
            if (env.objects_per_span() != 1) {
              CHECK_EQ(env.central_freelist().length() + objects.size(),
                       stats.obj_capacity);
              if (objects.empty()) {
                CHECK_EQ(stats.num_live_spans(), 0);
              } else {
                CHECK_GT(stats.num_live_spans(), 0);
              }
            }
          } else if constexpr (std::is_same_v<T, PrintStats>) {
            std::string s;
            s.resize(1 << 20);
            Printer p(&s[0], s.size());
            env.central_freelist().PrintSpanUtilStats(p);
            env.central_freelist().PrintSpanLifetimeStats(p);

            PbtxtRegion region(p, kTop);
            env.central_freelist().PrintSpanUtilStatsInPbtxt(region);
            env.central_freelist().PrintSpanLifetimeStatsInPbtxt(region);
          } else if constexpr (std::is_same_v<T, HandleLongLivedSpans>) {
            env.central_freelist().HandleLongLivedSpans();
          } else if constexpr (std::is_same_v<T, AdvanceClock>) {
            env.forwarder().AdvanceClock(absl::Milliseconds(arg.value));
          } else {
            static_assert(always_false_v<T>, "Unhandled type");
          }
        },
        instruction_wrapper.instr);
  }

  // Clean up.
  const size_t allocated = objects.size();
  size_t returned = 0;
  while (returned < allocated) {
    const size_t to_return = std::min(allocated - returned, kMaxObjectsToMove);
    env.central_freelist().InsertRange({&objects[returned], to_return});
    returned += to_return;
  }
}

auto GetInstructionDomain() {
  return fuzztest::OneOf(
      fuzztest::Map([](uint8_t n) { return Instruction{Allocate{n}}; },
                    fuzztest::InRange<uint8_t>(1, kMaxObjectsToMove)),
      fuzztest::Map([](uint8_t n) { return Instruction{Deallocate{n}}; },
                    fuzztest::InRange<uint8_t>(1, kMaxObjectsToMove)),
      fuzztest::Map([](int s) { return Instruction{Shuffle{s}}; },
                    fuzztest::Arbitrary<int>()),
      fuzztest::Map([](CheckStats c) { return Instruction{c}; },
                    fuzztest::Arbitrary<CheckStats>()),
      fuzztest::Map([](PrintStats p) { return Instruction{p}; },
                    fuzztest::Arbitrary<PrintStats>()),
      fuzztest::Map([](HandleLongLivedSpans h) { return Instruction{h}; },
                    fuzztest::Arbitrary<HandleLongLivedSpans>()),
      fuzztest::Map([](uint32_t v) { return Instruction{AdvanceClock{v}}; },
                    fuzztest::Arbitrary<uint32_t>()));
}

FUZZ_TEST(CentralFreeListTest, FuzzCFL)
    .WithDomains(fuzztest::InRange<size_t>(0, kMaxSize),
                 fuzztest::Arbitrary<size_t>(), fuzztest::Arbitrary<size_t>(),
                 fuzztest::VectorOf(GetInstructionDomain()));

}  // namespace
}  // namespace tcmalloc::tcmalloc_internal
GOOGLE_MALLOC_SECTION_END
