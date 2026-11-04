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

#ifndef TCMALLOC_ACCESS_DENSITY_PREDICTOR_H_
#define TCMALLOC_ACCESS_DENSITY_PREDICTOR_H_

#include <stddef.h>
#include <stdint.h>

#include <algorithm>
#include <atomic>
#include <cmath>

#include "absl/base/internal/spinlock.h"
#include "absl/base/optimization.h"
#include "absl/base/thread_annotations.h"
#include "absl/time/time.h"
#include "tcmalloc/central_freelist_options.h"
#include "tcmalloc/common.h"
#include "tcmalloc/internal/allocation_guard.h"
#include "tcmalloc/internal/config.h"
#include "tcmalloc/internal/logging.h"
#include "tcmalloc/pages.h"
#include "tcmalloc/span.h"

GOOGLE_MALLOC_SECTION_BEGIN
namespace tcmalloc {
namespace tcmalloc_internal {

// Dynamically classifies 1-page normal size classes into kDense (hotly
// allocated) vs kSparse (colder allocation rate) per heap partition using a
// windowed EWMA of per-size-class object allocation rates.
//
// All fields are zero-initialized so an instance may reside in Static (.bss).
class AccessDensityPredictor {
 public:
  static constexpr double kEwmaAlpha = 0.3;
  static constexpr double kDenseShareThreshold = 0.925;

  struct SizeClassInfo {
    size_t object_size = 0;
    Length pages_per_span = Length(0);
    uint64_t frontend_allocated_objects = 0;
    uint64_t backend_allocated_objects = 0;
  };

  struct ClassGeometry {
    size_t object_size = 0;
    size_t objects_per_span = 0;
    Length pages_per_span = Length(0);
  };

  constexpr AccessDensityPredictor() = default;

  AccessDensityPredictor(const AccessDensityPredictor&) = delete;
  AccessDensityPredictor& operator=(const AccessDensityPredictor&) = delete;

  [[nodiscard]] static AccessDensityPrediction StaticPredict(
      size_t objects_per_span, Length pages_per_span) {
    return (pages_per_span == Length(1) &&
            objects_per_span >
                central_freelist_internal::kFewObjectsAllocMaxLimit)
               ? AccessDensityPrediction::kDense
               : AccessDensityPrediction::kSparse;
  }

  // Lock-free query on CentralFreeList span allocation/deallocation paths.
  // Enforces HugePageFiller's single-page invariant for kDense spans and falls
  // back to the static objects-per-span heuristic for any partition that has
  // not yet observed allocation activity.
  [[nodiscard]] AccessDensityPrediction Predict(int size_class,
                                                size_t objects_per_span,
                                                Length pages_per_span) const {
    if (pages_per_span != Length(1) || size_class <= 0 ||
        static_cast<size_t>(size_class) >= kColdClassesStart) {
      return AccessDensityPrediction::kSparse;
    }
    const size_t partition = static_cast<size_t>(size_class) / kNumBaseClasses;
    if (ABSL_PREDICT_TRUE(partition_initialized_[partition].load(
            std::memory_order_acquire))) {
      return is_dense_[size_class].load(std::memory_order_relaxed)
                 ? AccessDensityPrediction::kDense
                 : AccessDensityPrediction::kSparse;
    }
    return StaticPredict(objects_per_span, pages_per_span);
  }

  [[nodiscard]] uint64_t updates() const {
    return updates_.load(std::memory_order_relaxed);
  }

  [[nodiscard]] bool AnyPartitionInitialized() const {
    for (size_t p = 0; p < kNormalPartitions; ++p) {
      if (partition_initialized_[p].load(std::memory_order_acquire)) {
        return true;
      }
    }
    return false;
  }

  [[nodiscard]] bool IsPartitionInitialized(size_t partition) const {
    return partition < kNormalPartitions &&
           partition_initialized_[partition].load(std::memory_order_acquire);
  }

  // Updates per-size-class allocation rates and refreshes dense/sparse
  // predictions independently for each active normal partition. The first call
  // records the counter/timestamp baseline; subsequent calls with strictly
  // advancing timestamps update EWMA rates and publish predictions.
  void Update(absl::Time now, size_t active_partitions,
              const SizeClassInfo (&classes)[kColdClassesStart])
      ABSL_LOCKS_EXCLUDED(lock_) {
    const int64_t now_ns = absl::ToUnixNanos(now);
    AllocationGuardSpinLockHolder l(lock_);

    if (!has_baseline_) {
      for (size_t sc = 1; sc < kColdClassesStart; ++sc) {
        prev_frontend_allocs_[sc] = classes[sc].frontend_allocated_objects;
        prev_backend_allocs_[sc] = classes[sc].backend_allocated_objects;
      }
      last_update_ns_ = now_ns;
      has_baseline_ = true;
      return;
    }

    const int64_t elapsed_ns = now_ns - last_update_ns_;
    if (elapsed_ns <= 0) {
      return;
    }
    const double elapsed_sec = static_cast<double>(elapsed_ns) * 1e-9;
    last_update_ns_ = now_ns;

    const bool first_rate_sample = !has_rate_sample_;
    has_rate_sample_ = true;

    for (size_t sc = 1; sc < kColdClassesStart; ++sc) {
      const uint64_t cur_frontend_allocs =
          classes[sc].frontend_allocated_objects;
      const uint64_t delta_frontend_allocs =
          cur_frontend_allocs >= prev_frontend_allocs_[sc]
              ? (cur_frontend_allocs - prev_frontend_allocs_[sc])
              : 0;
      prev_frontend_allocs_[sc] = cur_frontend_allocs;
      const double inst_frontend_rate =
          static_cast<double>(delta_frontend_allocs) / elapsed_sec;
      ewma_frontend_rate_[sc] =
          first_rate_sample ? inst_frontend_rate
                            : (kEwmaAlpha * inst_frontend_rate +
                               (1.0 - kEwmaAlpha) * ewma_frontend_rate_[sc]);

      const uint64_t cur_backend_allocs = classes[sc].backend_allocated_objects;
      const uint64_t delta_backend_allocs =
          cur_backend_allocs >= prev_backend_allocs_[sc]
              ? (cur_backend_allocs - prev_backend_allocs_[sc])
              : 0;
      prev_backend_allocs_[sc] = cur_backend_allocs;
      const double inst_backend_rate =
          static_cast<double>(delta_backend_allocs) / elapsed_sec;
      ewma_backend_rate_[sc] =
          first_rate_sample ? inst_backend_rate
                            : (kEwmaAlpha * inst_backend_rate +
                               (1.0 - kEwmaAlpha) * ewma_backend_rate_[sc]);
    }

    struct Candidate {
      size_t size_class;
      double rate;
      double weight;
    };

    const size_t num_partitions =
        std::min(active_partitions, kNormalPartitions);
    for (size_t p = 0; p < num_partitions; ++p) {
      const size_t sc_base = p * kNumBaseClasses;
      const size_t sc_begin = sc_base + 1;
      const size_t sc_end = sc_base + kNumBaseClasses;

      Candidate candidates[kNumBaseClasses];
      size_t num_candidates = 0;
      double total_weight = 0.0;

      for (size_t sc = sc_begin; sc < sc_end; ++sc) {
        if (classes[sc].object_size == 0 ||
            classes[sc].pages_per_span != Length(1)) {
          is_dense_[sc].store(false, std::memory_order_relaxed);
          continue;
        }
        const double rate =
            std::max(ewma_frontend_rate_[sc], ewma_backend_rate_[sc]);
        if (rate <= 0.0) {
          continue;
        }
        const double weight = std::sqrt(rate);
        candidates[num_candidates++] =
            Candidate{.size_class = sc, .rate = rate, .weight = weight};
        total_weight += weight;
      }

      if (num_candidates == 0 || total_weight <= 0.0) {
        continue;
      }

      std::sort(candidates, candidates + num_candidates,
                [](const Candidate& a, const Candidate& b) {
                  return a.rate != b.rate ? a.rate > b.rate
                                          : a.size_class < b.size_class;
                });

      bool next_dense[kNumBaseClasses] = {};
      double cum_share = 0.0;
      for (size_t i = 0; i < num_candidates; ++i) {
        if (i == 0 || cum_share < kDenseShareThreshold) {
          next_dense[candidates[i].size_class - sc_base] = true;
          cum_share += candidates[i].weight / total_weight;
        }
      }

      for (size_t sc = sc_begin; sc < sc_end; ++sc) {
        is_dense_[sc].store(next_dense[sc - sc_base],
                            std::memory_order_relaxed);
      }
      partition_initialized_[p].store(true, std::memory_order_release);
    }
    updates_.fetch_add(1, std::memory_order_relaxed);
  }

  template <typename GeometryFn>
  void Print(Printer& out, bool enabled, size_t active_partitions,
             GeometryFn&& geometry_fn) const ABSL_LOCKS_EXCLUDED(lock_) {
    out.printf("------------------------------------------------\n");
    out.printf("AccessDensityPredictor: enabled=%d, updates=%llu\n",
               enabled ? 1 : 0, static_cast<unsigned long long>(updates()));
    const size_t num_partitions = std::min<size_t>(
        std::max<size_t>(1, active_partitions), kNormalPartitions);
    for (size_t p = 0; p < num_partitions; ++p) {
      bool dense_mask[kNumBaseClasses] = {};
      const PartitionCounts counts =
          SnapshotPartition(p, enabled, geometry_fn, dense_mask);
      out.printf(
          "AccessDensityPredictor: partition %zu: initialized=%d, "
          "dense_size_classes=%zu, sparse_size_classes=%zu\n",
          p, IsPartitionInitialized(p) ? 1 : 0, counts.dense, counts.sparse);

      for (const bool want_dense : {true, false}) {
        out.printf("  %s:", want_dense ? "dense_classes" : "sparse_classes");
        size_t printed = 0;
        ForEachPartitionClass(
            p, dense_mask, geometry_fn, [&](const ClassSnapshot& s) {
              if (s.dense == want_dense) {
                out.printf(" %zu(%zuB)", s.size_class, s.object_size);
                ++printed;
              }
            });
        if (printed == 0) {
          out.printf(" none");
        }
        out.printf("\n");
      }

      ForEachPartitionClass(
          p, dense_mask, geometry_fn, [&](const ClassSnapshot& s) {
            out.printf(
                "  class %3zu [ %8zu bytes ] : %3zu pages; %8zu objs/span; "
                "filler=%s;%s frontend_rate=%10.1f objs/s; "
                "backend_rate=%10.1f objs/s\n",
                s.size_class, s.object_size, s.pages_per_span,
                s.objects_per_span, s.dense ? "dense" : "sparse",
                s.dense ? " " : "", s.frontend_alloc_rate,
                s.backend_alloc_rate);
          });
    }
  }

  template <typename GeometryFn>
  void PrintInPbtxt(PbtxtRegion& region, bool enabled, size_t active_partitions,
                    GeometryFn&& geometry_fn) const ABSL_LOCKS_EXCLUDED(lock_) {
    PbtxtRegion stats = region.CreateSubRegion("access_density_predictor");
    stats.PrintBool("enabled", enabled);
    stats.PrintI64("updates", static_cast<int64_t>(updates()));
    const size_t num_partitions = std::min<size_t>(
        std::max<size_t>(1, active_partitions), kNormalPartitions);
    for (size_t p = 0; p < num_partitions; ++p) {
      bool dense_mask[kNumBaseClasses] = {};
      const PartitionCounts counts =
          SnapshotPartition(p, enabled, geometry_fn, dense_mask);
      PbtxtRegion part = stats.CreateSubRegion("partitions");
      part.PrintI64("partition", static_cast<int64_t>(p));
      part.PrintBool("initialized", IsPartitionInitialized(p));
      part.PrintI64("dense_size_classes", static_cast<int64_t>(counts.dense));
      part.PrintI64("sparse_size_classes", static_cast<int64_t>(counts.sparse));
      ForEachPartitionClass(
          p, dense_mask, geometry_fn, [&](const ClassSnapshot& s) {
            PbtxtRegion sc = part.CreateSubRegion("size_classes");
            sc.PrintI64("size_class", static_cast<int64_t>(s.size_class));
            sc.PrintI64("object_size", static_cast<int64_t>(s.object_size));
            sc.PrintI64("pages_per_span",
                        static_cast<int64_t>(s.pages_per_span));
            sc.PrintI64("objects_per_span",
                        static_cast<int64_t>(s.objects_per_span));
            sc.PrintBool("dense", s.dense);
            sc.PrintDouble("frontend_alloc_rate", s.frontend_alloc_rate);
            sc.PrintDouble("backend_alloc_rate", s.backend_alloc_rate);
          });
    }
  }

  void ResetForTesting() ABSL_LOCKS_EXCLUDED(lock_) {
    AllocationGuardSpinLockHolder l(lock_);
    for (size_t p = 0; p < kNormalPartitions; ++p) {
      partition_initialized_[p].store(false, std::memory_order_release);
    }
    for (size_t sc = 0; sc < kColdClassesStart; ++sc) {
      is_dense_[sc].store(false, std::memory_order_relaxed);
      prev_frontend_allocs_[sc] = 0;
      prev_backend_allocs_[sc] = 0;
      ewma_frontend_rate_[sc] = 0.0;
      ewma_backend_rate_[sc] = 0.0;
    }
    updates_.store(0, std::memory_order_relaxed);
    last_update_ns_ = 0;
    has_baseline_ = false;
    has_rate_sample_ = false;
  }

 private:
  struct PartitionCounts {
    size_t dense = 0;
    size_t sparse = 0;
  };

  struct ClassRates {
    double frontend = 0.0;
    double backend = 0.0;
  };

  struct ClassSnapshot {
    size_t size_class = 0;
    size_t object_size = 0;
    size_t pages_per_span = 0;
    size_t objects_per_span = 0;
    bool dense = false;
    double frontend_alloc_rate = 0.0;
    double backend_alloc_rate = 0.0;
  };

  [[nodiscard]] ClassRates GetClassRates(size_t sc) const
      ABSL_LOCKS_EXCLUDED(lock_) {
    if (sc == 0 || sc >= kColdClassesStart) {
      return {};
    }
    AllocationGuardSpinLockHolder l(lock_);
    return ClassRates{
        .frontend = ewma_frontend_rate_[sc],
        .backend = ewma_backend_rate_[sc],
    };
  }

  template <typename GeometryFn>
  [[nodiscard]] PartitionCounts SnapshotPartition(
      size_t partition, bool enabled, GeometryFn&& geometry_fn,
      bool (&dense_mask)[kNumBaseClasses]) const {
    PartitionCounts counts;
    const size_t sc_base = partition * kNumBaseClasses;
    for (size_t i = 1; i < kNumBaseClasses; ++i) {
      const size_t sc = sc_base + i;
      const ClassGeometry g = geometry_fn(sc);
      if (g.object_size == 0) {
        dense_mask[i] = false;
        continue;
      }
      const AccessDensityPrediction pred =
          enabled ? Predict(static_cast<int>(sc), g.objects_per_span,
                            g.pages_per_span)
                  : StaticPredict(g.objects_per_span, g.pages_per_span);
      const bool dense = (pred == AccessDensityPrediction::kDense);
      dense_mask[i] = dense;
      if (dense) {
        ++counts.dense;
      } else {
        ++counts.sparse;
      }
    }
    return counts;
  }

  template <typename GeometryFn, typename Callback>
  void ForEachPartitionClass(size_t partition,
                             const bool (&dense_mask)[kNumBaseClasses],
                             GeometryFn&& geometry_fn,
                             Callback&& callback) const
      ABSL_LOCKS_EXCLUDED(lock_) {
    const size_t sc_base = partition * kNumBaseClasses;
    for (size_t i = 1; i < kNumBaseClasses; ++i) {
      const size_t sc = sc_base + i;
      const ClassGeometry g = geometry_fn(sc);
      if (g.object_size == 0) {
        continue;
      }
      const ClassRates rates = GetClassRates(sc);
      callback(ClassSnapshot{
          .size_class = sc,
          .object_size = g.object_size,
          .pages_per_span = g.pages_per_span.raw_num(),
          .objects_per_span = g.objects_per_span,
          .dense = dense_mask[i],
          .frontend_alloc_rate = rates.frontend,
          .backend_alloc_rate = rates.backend,
      });
    }
  }

  mutable absl::base_internal::SpinLock lock_{
      absl::base_internal::SCHEDULE_KERNEL_ONLY};
  std::atomic<bool> partition_initialized_[kNormalPartitions]{};
  std::atomic<bool> is_dense_[kColdClassesStart]{};
  std::atomic<uint64_t> updates_{};
  int64_t last_update_ns_ ABSL_GUARDED_BY(lock_) = 0;
  bool has_baseline_ ABSL_GUARDED_BY(lock_) = false;
  bool has_rate_sample_ ABSL_GUARDED_BY(lock_) = false;
  uint64_t prev_frontend_allocs_[kColdClassesStart] ABSL_GUARDED_BY(lock_){};
  uint64_t prev_backend_allocs_[kColdClassesStart] ABSL_GUARDED_BY(lock_){};
  double ewma_frontend_rate_[kColdClassesStart] ABSL_GUARDED_BY(lock_){};
  double ewma_backend_rate_[kColdClassesStart] ABSL_GUARDED_BY(lock_){};
};

}  // namespace tcmalloc_internal
}  // namespace tcmalloc
GOOGLE_MALLOC_SECTION_END

#endif  // TCMALLOC_ACCESS_DENSITY_PREDICTOR_H_
