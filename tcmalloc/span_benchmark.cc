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

#include <stdlib.h>

#include <algorithm>
#include <cstdint>
#include <numeric>
#include <optional>
#include <string>
#include <type_traits>
#include <vector>

#include "absl/random/random.h"
#include "absl/types/span.h"
#include "benchmark/benchmark.h"
#include "tcmalloc/common.h"
#include "tcmalloc/internal/config.h"
#include "tcmalloc/internal/logging.h"
#include "tcmalloc/pages.h"
#include "tcmalloc/span.h"
#include "tcmalloc/static_vars.h"
#include "tcmalloc/tcmalloc_policy.h"

GOOGLE_MALLOC_SECTION_BEGIN
namespace tcmalloc {
namespace tcmalloc_internal {

namespace {

constexpr uint64_t kSpanAllocTime = 1234;

class RawSpan {
 public:
  RawSpan() = default;
  RawSpan(const RawSpan&) = delete;
  RawSpan& operator=(const RawSpan&) = delete;

  void Init(size_t size, Length npages) {
    TC_CHECK_GT(size, 0);
    size_t objects_per_span = npages.in_bytes() / size;

    void* mem;
    int res = posix_memalign(&mem, kPageSize, npages.in_bytes());
    TC_CHECK_EQ(res, 0);
    span_.emplace(Range(PageIdContaining(mem), npages));
    TC_CHECK_EQ(
        span_->BuildFreelist(size, objects_per_span, {}, kSpanAllocTime), 0);
  }

  ~RawSpan() {
    if (span_.has_value()) {
      free(span_->start_address());
    }
  }

  Span& span() { return *span_; }

 private:
  std::optional<Span> span_;
};

int FindSizeClass(size_t target_size) {
  TC_CHECK_LE(target_size, kMaxSize);
  auto result = tc_globals.sizemap().GetSizeClass(CppPolicy(), target_size);
  TC_CHECK(result.is_small);
  TC_CHECK_EQ(tc_globals.sizemap().class_to_size(result.size_class),
              target_size);
  return result.size_class;
}

// Generalized PopPush benchmark
template <typename BatchType>
void BM_SpanPopPush(benchmark::State& state) {
  const size_t target_size = state.range(0);
  size_t batch_size = state.range(1);
  const size_t num_spans = state.range(2);

  const int size_class = FindSizeClass(target_size);
  const size_t size = tc_globals.sizemap().class_to_size(size_class);

  TC_CHECK_LE(batch_size, kMaxObjectsToMove);

  auto npages = tc_globals.sizemap().class_to_pages(size_class);
  size_t objects_per_span = npages.in_bytes() / size;
  // We need at least batch_size + 1 objects to keep 1 allocated and avoid
  // FreelistPushBatch returning false on empty span.
  if (objects_per_span < batch_size + 1) {
    state.SkipWithMessage("Span too small for batch size + 1");
    return;
  }

  std::vector<RawSpan> spans(num_spans);
  std::vector<std::vector<void*>> active_batches(num_spans);
  std::vector<void*> dummy_objects(num_spans);

  for (size_t i = 0; i < num_spans; i++) {
    spans[i].Init(size, npages);
    active_batches[i].resize(batch_size);

    void* prepop[kMaxObjectsToMove + 1];
    int popped = spans[i].span().FreelistPopBatch(
        absl::MakeSpan(prepop, batch_size + 1), size);
    TC_CHECK_EQ(popped, batch_size + 1);

    dummy_objects[i] = prepop[0];
    std::copy(prepop + 1, prepop + batch_size + 1, active_batches[i].begin());
  }

  uint32_t reciprocal = Span::CalcReciprocal(size);

  // Precompute random sequence of span indices
  std::vector<size_t> seq(num_spans);
  absl::BitGen rng;
  for (size_t& x : seq) x = absl::Uniform<size_t>(rng, 0, num_spans);
  size_t seq_idx = 0;

  int64_t processed = 0;
  while (state.KeepRunningBatch(batch_size)) {
    size_t current_span = seq[seq_idx];
    seq_idx = (seq_idx + 1) % seq.size();

    // Push
    if constexpr (std::is_same_v<BatchType, void*>) {
      bool ok = spans[current_span].span().FreelistPushBatch(
          absl::MakeSpan(active_batches[current_span]), size, reciprocal);
      TC_CHECK(ok);
    } else {
      Span::ObjIdx idx_batch[kMaxObjectsToMove];
      for (size_t j = 0; j < batch_size; j++) {
        void* p = active_batches[current_span][j];
        idx_batch[j] =
            Span::UseBitmapForSize(size)
                ? spans[current_span].span().BitmapPtrToIdx(p, size, reciprocal)
                : spans[current_span].span().PtrToIdx(p, size);
      }
      bool ok = spans[current_span].span().FreelistPushBatch(
          absl::MakeSpan(idx_batch, batch_size), size, reciprocal);
      TC_CHECK(ok);
    }

    // Pop
    int n = spans[current_span].span().FreelistPopBatch(
        absl::MakeSpan(active_batches[current_span]), size);
    TC_CHECK_EQ(n, batch_size);
    processed += n;
  }
}

// Generalized DrainFill benchmark
template <typename BatchType>
void BM_SpanDrainFill(benchmark::State& state) {
  const size_t target_size = state.range(0);
  size_t batch_size = state.range(1);
  const size_t num_spans = state.range(2);

  const int size_class = FindSizeClass(target_size);
  const size_t size = tc_globals.sizemap().class_to_size(size_class);

  TC_CHECK_LE(batch_size, kMaxObjectsToMove);

  Length npages = tc_globals.sizemap().class_to_pages(size_class);
  size_t objects_per_span = npages.in_bytes() / size;
  // We leave 1 object allocated to avoid FreelistPushBatch returning false.
  size_t run_objects = objects_per_span - 1;
  if (run_objects == 0) {
    state.SkipWithMessage("Span too small (needs at least 2 objects)");
    return;
  }

  std::vector<RawSpan> spans(num_spans);
  std::vector<void*> dummy_objects(num_spans);
  for (size_t i = 0; i < num_spans; i++) {
    spans[i].Init(size, npages);
    void* dummy;
    int popped =
        spans[i].span().FreelistPopBatch(absl::MakeSpan(&dummy, 1), size);
    TC_CHECK_EQ(popped, 1);
    dummy_objects[i] = dummy;
  }

  std::vector<std::vector<void*>> all_objects(num_spans);
  for (size_t i = 0; i < num_spans; i++) {
    all_objects[i].resize(run_objects, nullptr);
  }

  uint32_t reciprocal = Span::CalcReciprocal(size);

  std::vector<size_t> drain_order(num_spans);
  std::iota(drain_order.begin(), drain_order.end(), size_t{0});
  std::vector<size_t> fill_order = drain_order;

  absl::BitGen rng;
  std::shuffle(drain_order.begin(), drain_order.end(), rng);
  std::shuffle(fill_order.begin(), fill_order.end(), rng);

  size_t total_objects = num_spans * run_objects;

  size_t processed = 0;
  while (state.KeepRunningBatch(total_objects)) {
    // 1. Drain all spans (interleaved)
    std::vector<size_t> pop_offsets(num_spans, 0);
    bool active = true;
    while (active) {
      active = false;
      for (size_t span_idx : drain_order) {
        size_t oindex = pop_offsets[span_idx];
        if (oindex < run_objects) {
          size_t to_pop = std::min(batch_size, run_objects - oindex);
          size_t popped = spans[span_idx].span().FreelistPopBatch(
              absl::MakeSpan(all_objects[span_idx]).subspan(oindex, to_pop),
              size);
          pop_offsets[span_idx] = oindex + popped;
          processed += popped;
          if (popped > 0) {
            active = true;
          }
        }
      }
    }

    // 2. Fill all spans (interleaved)
    std::vector<size_t> push_offsets = pop_offsets;
    active = true;
    while (active) {
      active = false;
      for (size_t span_idx : fill_order) {
        size_t oindex = push_offsets[span_idx];
        if (oindex > 0) {
          size_t to_push = std::min(batch_size, oindex);
          size_t start_idx = oindex - to_push;

          if constexpr (std::is_same_v<BatchType, void*>) {
            bool ok = spans[span_idx].span().FreelistPushBatch(
                absl::MakeSpan(&all_objects[span_idx][start_idx], to_push),
                size, reciprocal);
            TC_CHECK(ok);
          } else {
            Span::ObjIdx idx_batch[kMaxObjectsToMove];
            for (size_t j = 0; j < to_push; j++) {
              void* p = all_objects[span_idx][start_idx + j];
              idx_batch[j] = Span::UseBitmapForSize(size)
                                 ? spans[span_idx].span().BitmapPtrToIdx(
                                       p, size, reciprocal)
                                 : spans[span_idx].span().PtrToIdx(p, size);
            }
            bool ok = spans[span_idx].span().FreelistPushBatch(
                absl::MakeSpan(idx_batch, to_push), size, reciprocal);
            TC_CHECK(ok);
          }
          push_offsets[span_idx] = start_idx;
          active = true;
        }
      }
    }
  }
  state.SetItemsProcessed(processed);
}

template <typename F>
void ForEachConfig(F&& f) {
  std::vector<size_t> sizes = {8, 32, 48, 64, 1024};
  std::vector<size_t> spans = {1, 100, 10000};

  for (size_t size : sizes) {
    int size_class = FindSizeClass(size);

    // Batch size 1
    for (size_t span_count : spans) {
      f(size, 1, span_count);
    }

    // Batch size num_to_move
    size_t num_to_move = tc_globals.sizemap().num_objects_to_move(size_class);
    TC_CHECK_GT(num_to_move, 1);
    for (size_t span_count : spans) {
      f(size, num_to_move, span_count);
    }
  }
}

class BenchmarkRegistrar {
 public:
  BenchmarkRegistrar() {
    tc_globals.InitIfNecessary();
    // PopPush void*
    ForEachConfig([](size_t size, size_t batch, size_t spans) {
      benchmark::RegisterBenchmark("BM_SpanPopPush/void*",
                                   BM_SpanPopPush<void*>)
          ->Args({static_cast<int64_t>(size), static_cast<int64_t>(batch),
                  static_cast<int64_t>(spans)})
          ->ArgNames({"size", "batch", "spans"});
    });

    // PopPush ObjIdx
    ForEachConfig([](size_t size, size_t batch, size_t spans) {
      benchmark::RegisterBenchmark("BM_SpanPopPush/Span::ObjIdx",
                                   BM_SpanPopPush<Span::ObjIdx>)
          ->Args({static_cast<int64_t>(size), static_cast<int64_t>(batch),
                  static_cast<int64_t>(spans)})
          ->ArgNames({"size", "batch", "spans"});
    });

    // DrainFill void*
    ForEachConfig([](size_t size, size_t batch, size_t spans) {
      benchmark::RegisterBenchmark("BM_SpanDrainFill/void*",
                                   BM_SpanDrainFill<void*>)
          ->Args({static_cast<int64_t>(size), static_cast<int64_t>(batch),
                  static_cast<int64_t>(spans)})
          ->ArgNames({"size", "batch", "spans"});
    });

    // DrainFill ObjIdx
    ForEachConfig([](size_t size, size_t batch, size_t spans) {
      benchmark::RegisterBenchmark("BM_SpanDrainFill/Span::ObjIdx",
                                   BM_SpanDrainFill<Span::ObjIdx>)
          ->Args({static_cast<int64_t>(size), static_cast<int64_t>(batch),
                  static_cast<int64_t>(spans)})
          ->ArgNames({"size", "batch", "spans"});
    });
  }
};

static BenchmarkRegistrar registrar;

}  // namespace
}  // namespace tcmalloc_internal
}  // namespace tcmalloc
GOOGLE_MALLOC_SECTION_END
