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

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <iterator>
#include <map>
#include <memory>
#include <new>
#include <vector>

#include "absl/algorithm/container.h"
#include "absl/base/internal/cycleclock.h"
#include "absl/base/thread_annotations.h"
#include "absl/container/flat_hash_map.h"
#include "absl/random/random.h"
#include "absl/synchronization/mutex.h"
#include "absl/time/time.h"
#include "absl/types/span.h"
#include "benchmark/benchmark.h"
#include "tcmalloc/central_freelist.h"
#include "tcmalloc/common.h"
#include "tcmalloc/internal/bytes.h"
#include "tcmalloc/internal/config.h"
#include "tcmalloc/internal/logging.h"
#include "tcmalloc/pagemap.h"
#include "tcmalloc/pages.h"
#include "tcmalloc/span.h"
#include "tcmalloc/static_vars.h"
#include "tcmalloc/tcmalloc_policy.h"

namespace tcmalloc {
namespace tcmalloc_internal {
namespace {

void* BenchmarkMetaDataAlloc(size_t bytes) { return ::operator new(bytes); }

#ifdef TCMALLOC_USE_PAGEMAP3
using BenchmarkPageMap =
    PageMap3<kAddressBits - kPageShift, BenchmarkMetaDataAlloc>;
#else
using BenchmarkPageMap =
    PageMap2<kAddressBits - kPageShift, BenchmarkMetaDataAlloc>;
#endif

// BenchmarkStaticForwarder provides a wrapper around ordinary TCMalloc and a
// PageMap to allow us to carve up memory we obtained into our own objects.
// This lets us elide the full page heap for benchmarking purposes.
class BenchmarkStaticForwarder
    : public central_freelist_internal::StaticForwarder {
 public:
  BenchmarkStaticForwarder()
      : class_size_(0),
        pages_per_span_(0),
        num_objects_to_move_(0),
        page_size_(kPageSize),
        pagemap_(std::make_unique<BenchmarkPageMap>()) {}

  void Init(size_t class_size, Bytes span_bytes, size_t num_objects_to_move,
            size_t page_size) {
    class_size_ = class_size;
    pages_per_span_ = BytesToLengthCeil(span_bytes);
    num_objects_to_move_ = num_objects_to_move;
    page_size_ = page_size;
  }

  ~BenchmarkStaticForwarder() {
    absl::MutexLock l(mu_);
    for (Span* span : allocated_spans_) {
      void* mem = span->start_address();
      ::operator delete(mem, std::align_val_t(page_size_));
      delete span;
    }
  }

  size_t class_to_size(int size_class) const { return class_size_; }
  Length class_to_pages(int size_class) const { return pages_per_span_; }
  size_t num_objects_to_move(int size_class) const {
    return num_objects_to_move_;
  }

  void MapObjectsToSpans(absl::Span<void*> batch, Span** spans,
                         int expected_size_class) {
    for (size_t i = 0; i < batch.size(); ++i) {
      Span* span = pagemap_->get(PageIdContaining(batch[i]).index());
      span->Prefetch();
      spans[i] = span;
    }
  }

  [[nodiscard]] Span* AllocateSpan(int size_class, size_t objects_per_span,
                                   Length pages_per_span) {
    absl::MutexLock l(mu_);
    if (!free_spans_.empty()) {
      Span* span = free_spans_.back();
      free_spans_.pop_back();
      new (span) Span(Range(span->first_page(), pages_per_span));
      RegisterSpanLocked(span);
      return span;
    }

    // Allocate backing memory dynamically.
    void* mem =
        ::operator new(pages_per_span.in_bytes(), std::align_val_t(page_size_));
    if (mem == nullptr) return nullptr;

    PageId page = PageIdContaining(mem);

    // Ensure the PageMap covers this range.
    if (!pagemap_->Ensure(page.index(), pages_per_span.raw_num())) {
      ::operator delete(mem, std::align_val_t(page_size_));
      return nullptr;
    }

    Span* span = new Span(Range(page, pages_per_span));
    allocated_spans_.push_back(span);
    RegisterSpanLocked(span);
    return span;
  }

  void DeallocateSpans(size_t objects_per_span, absl::Span<Span*> free_spans) {
    absl::MutexLock l(mu_);
    for (Span* span : free_spans) {
      UnregisterSpanLocked(span);
      free_spans_.push_back(span);
    }
  }

 private:
  void RegisterSpanLocked(Span* span) ABSL_EXCLUSIVE_LOCKS_REQUIRED(mu_) {
    PageId page = span->first_page();
    Length num = span->num_pages();
    for (size_t i = 0; i < num.raw_num(); ++i) {
      pagemap_->set(page.index() + i, span);
    }
  }

  void UnregisterSpanLocked(Span* span) ABSL_EXCLUSIVE_LOCKS_REQUIRED(mu_) {
    PageId page = span->first_page();
    Length num = span->num_pages();
    for (size_t i = 0; i < num.raw_num(); ++i) {
      pagemap_->set(page.index() + i, nullptr);
    }
  }

  size_t class_size_;
  Length pages_per_span_;
  size_t num_objects_to_move_;
  size_t page_size_;

  std::unique_ptr<BenchmarkPageMap> pagemap_;

  absl::Mutex mu_;
  std::vector<Span*> free_spans_ ABSL_GUARDED_BY(mu_);
  std::vector<Span*> allocated_spans_ ABSL_GUARDED_BY(mu_);
};

using CentralFreeList =
    central_freelist_internal::CentralFreeList<BenchmarkStaticForwarder>;

class BenchmarkEnv {
 public:
  static constexpr int kSizeClass = 1;

  BenchmarkEnv(size_t class_size, Bytes span_bytes,
               size_t num_objects_to_move) {
    cache_.forwarder().Init(class_size, span_bytes, num_objects_to_move,
                            kPageSize);
    cache_.Init(kSizeClass);
  }

  CentralFreeList& central_freelist() { return cache_; }
  BenchmarkStaticForwarder& forwarder() { return cache_.forwarder(); }

 private:
  CentralFreeList cache_;
};

struct SameSpanHistogram {
  size_t size;
  double counts[32];
  size_t num_counts;
};

// Data is as of June 2026, drawn across different values of kPageSize.
static constexpr SameSpanHistogram kSameSpanHistograms[] = {
    {8,
     {0.05189, 0.063,   0.03723, 0.02431, 0.01647, 0.0133,  0.01002, 0.0104,
      0.0105,  0.01069, 0.0087,  0.00936, 0.01386, 0.02439, 0.0299,  0.02652,
      0.02259, 0.03316, 0.02478, 0.01806, 0.01286, 0.00927, 0.00774, 0.00726,
      0.00773, 0.0072,  0.18827, 0.18567, 0.0655,  0.03478, 0.03703, 0.07591},
     32},
    {16,
     {0.08197, 0.03993, 0.01635, 0.01086, 0.00902, 0.00876, 0.00835, 0.00906,
      0.01326, 0.02261, 0.02804, 0.02766, 0.02394, 0.01641, 0.01096, 0.00864,
      0.00723, 0.00812, 0.00849, 0.01742, 0.0625,  0.12101, 0.12138, 0.08555,
      0.0626,  0.04446, 0.03225, 0.02321, 0.01505, 0.01188, 0.01662, 0.05501},
     32},
    {32,
     {0.07615,  0.022068, 0.02135,  0.022668, 0.024895, 0.029745, 0.033663,
      0.041892, 0.046332, 0.049198, 0.047285, 0.043842, 0.038427, 0.033253,
      0.03158,  0.036922, 0.04658,  0.02081,  0.01882,  0.01945,  0.017882,
      0.01719,  0.016045, 0.018283, 0.019717, 0.022242, 0.024508, 0.024113,
      0.024658, 0.026203, 0.0319,   0.069787},
     32},
    {64,
     {0.094315, 0.030528, 0.03032,  0.035362, 0.04491,  0.052283, 0.055175,
      0.051328, 0.044463, 0.036759, 0.029627, 0.025654, 0.021548, 0.019637,
      0.019052, 0.018953, 0.021935, 0.019123, 0.02071,  0.021307, 0.022162,
      0.023287, 0.022112, 0.021965, 0.023628, 0.021142, 0.01967,  0.019278,
      0.01896,  0.019993, 0.02524,  0.044705},
     32},
    {80,
     {0.062679,  0.068723, 0.089683, 0.073822, 0.065388, 0.046707, 0.038945,
      0.032018,  0.02637,  0.018193, 0.017543, 0.016287, 0.015207, 0.012048,
      0.0094017, 0.010395, 0.020418, 0.05906,  0.08037,  0.058618, 0.056002,
      0.039703,  0.037605, 0.027362, 0.018503, 0.017613, 0.013095, 0.01071,
      0.013035,  0.010918, 0.011372, 0.030128},
     32},
    {96,
     {0.098481, 0.047266, 0.030145, 0.027202, 0.027503, 0.026852, 0.027149,
      0.029028, 0.032817, 0.034994, 0.034903, 0.033453, 0.030918, 0.027619,
      0.025242, 0.023205, 0.023167, 0.027762, 0.023916, 0.023532, 0.021557,
      0.019212, 0.018348, 0.017552, 0.016829, 0.016148, 0.016357, 0.016955,
      0.017522, 0.019676, 0.028189, 0.047544},
     32},
    {112,
     {0.084945, 0.027275, 0.019865, 0.01874,  0.018375, 0.017705, 0.01829,
      0.02242,  0.030335, 0.038725, 0.045725, 0.04705,  0.0459,   0.040615,
      0.035495, 0.0303,   0.02805,  0.026325, 0.0241,   0.02172,  0.020535,
      0.01897,  0.01818,  0.01627,  0.015705, 0.01564,  0.01656,  0.017465,
      0.02014,  0.02427,  0.032405, 0.062205},
     32},
    {128,
     {0.085335, 0.04357,  0.038305, 0.040205, 0.045855, 0.041765, 0.04128,
      0.03842,  0.042725, 0.04085,  0.035425, 0.02969,  0.02442,  0.02036,
      0.01646,  0.015955, 0.01941,  0.02659,  0.031,    0.035155, 0.028815,
      0.02642,  0.022675, 0.02041,  0.016805, 0.01616,  0.015485, 0.01591,
      0.01675,  0.01787,  0.025145, 0.05177},
     32},
    {144,
     {0.111725, 0.045265, 0.036045, 0.030755, 0.028805, 0.02782,  0.028,
      0.02845,  0.02939,  0.03214,  0.035185, 0.037965, 0.03873,  0.039695,
      0.03912,  0.03744,  0.035155, 0.033455, 0.03086,  0.02875,  0.027115,
      0.025795, 0.024535, 0.0228,   0.021695, 0.02109,  0.020505, 0.020545,
      0.020425, 0.02176,  0.024525, 0.024935},
     32},
    {176,
     {0.104465, 0.04465,  0.03527,  0.033915, 0.036455, 0.041,    0.049545,
      0.0595,   0.0626,   0.05824,  0.04813,  0.037345, 0.02779,  0.02063,
      0.01662,  0.014075, 0.01312,  0.01296,  0.01297,  0.012075, 0.011975,
      0.012275, 0.012325, 0.013285, 0.013935, 0.014635, 0.01584,  0.017035,
      0.017355, 0.018175, 0.021175, 0.02419},
     32},
    {208,
     {0.05943, 0.03118, 0.02151, 0.01576, 0.0153,  0.01725, 0.02074, 0.0199,
      0.02811, 0.0256,  0.03254, 0.03932, 0.04349, 0.04396, 0.04437, 0.04331,
      0.0421,  0.0408,  0.03563, 0.03187, 0.02711, 0.02439, 0.02197, 0.02174,
      0.02066, 0.0203,  0.02252, 0.02667, 0.02777, 0.0347,  0.04379, 0.04343},
     32},
    {256,
     {0.135775, 0.042225, 0.027895, 0.02981,  0.032465, 0.03276,  0.036605,
      0.06439,  0.03391,  0.03528,  0.033925, 0.030915, 0.033045, 0.021555,
      0.017415, 0.015055, 0.01408,  0.014325, 0.014425, 0.015655, 0.01711,
      0.01732,  0.016655, 0.01564,  0.014665, 0.013325, 0.012765, 0.01266,
      0.01425,  0.01451,  0.018095, 0.018955},
     32},
    {304,
     {0.097195, 0.03846,  0.022495, 0.01873,  0.0203,   0.024495, 0.027435,
      0.020255, 0.01836,  0.03032,  0.017115, 0.020785, 0.02462,  0.02484,
      0.02671,  0.027535, 0.028395, 0.02945,  0.02903,  0.027755, 0.024435,
      0.021075, 0.01763,  0.015555, 0.013895, 0.01298,  0.01324,  0.01372,
      0.01495,  0.017255, 0.02254,  0.02491},
     32},
    {384,
     {0.08996,  0.03731,  0.033165, 0.03554,  0.039,   0.04262,  0.041585,
      0.03957,  0.03989,  0.02685,  0.021575, 0.0188,  0.017045, 0.015755,
      0.015435, 0.015495, 0.01496,  0.014055, 0.01389, 0.01344,  0.01351,
      0.01297,  0.01341,  0.01384,  0.0132,   0.01278, 0.012565, 0.012145,
      0.01277,  0.014775, 0.01857,  0.017585},
     32},
    {448,
     {0.08829, 0.03415, 0.01847, 0.01478, 0.01623, 0.01998, 0.02107, 0.02177,
      0.02392, 0.02854, 0.03567, 0.04232, 0.04624, 0.04472, 0.03964, 0.03467,
      0.02847, 0.02422, 0.01964, 0.01658, 0.01418, 0.01285, 0.01117, 0.01147,
      0.01098, 0.01266, 0.01303, 0.01451, 0.01611, 0.01759, 0.02225, 0.02217},
     32},
    {512,
     {0.09164,  0.03677,  0.027905, 0.0273,   0.02766,  0.029045, 0.03,
      0.02975,  0.02698,  0.0247,   0.0223,   0.02129,  0.02099,  0.02236,
      0.02376,  0.024555, 0.023445, 0.02067,  0.01882,  0.01641,  0.01592,
      0.015415, 0.01578,  0.016185, 0.017075, 0.018345, 0.019815, 0.020625,
      0.02252,  0.02356,  0.02579,  0.02643},
     32},
    {576,
     {0.085705, 0.04014,  0.0337,   0.03344,  0.03437,  0.03213,  0.029015,
      0.026525, 0.023805, 0.022585, 0.02232,  0.021745, 0.0206,   0.020195,
      0.020275, 0.02056,  0.021145, 0.021195, 0.021245, 0.02023,  0.02028,
      0.020025, 0.02011,  0.020155, 0.02194,  0.023165, 0.018175, 0.01848,
      0.01907,  0.020615, 0.025325, 0.027605},
     32},
    {704,
     {0.086645, 0.04826, 0.045645, 0.04693,  0.04816,  0.046475, 0.04188,
      0.037,    0.03136, 0.02852,  0.02775,  0.02683,  0.025145, 0.02524,
      0.025905, 0.02728, 0.03191,  0.04176,  0.025895, 0.024745, 0.023695,
      0.023615, 0.02199, 0.02213,  0.022415, 0.022785, 0.02488,  0.02589,
      0.028405, 0.03267, 0.0539,   0.090115},
     32},
    {896,
     {0.09611, 0.05862, 0.05118, 0.05481, 0.04581, 0.0331,  0.02956, 0.03436,
      0.04292, 0.05052, 0.05535, 0.05828, 0.05444, 0.05118, 0.04569, 0.03792,
      0.03006, 0.02356, 0.01904, 0.01583, 0.01527, 0.0143,  0.01594, 0.01744,
      0.01887, 0.02029, 0.01955, 0.02011, 0.01946, 0.01981, 0.02193, 0.02094},
     32},
    {1024,
     {0.30668, 0.19332, 0.0492,  0.0255,  0.0238,  0.02075, 0.02033, 0.02298,
      0.02928, 0.03614, 0.03959, 0.03536, 0.02819, 0.02006, 0.01266, 0.00804,
      0.00555, 0.00449, 0.00511, 0.0059,  0.0069,  0.00792, 0.0078,  0.00698,
      0.00613, 0.00517, 0.00427, 0.00455, 0.00502, 0.00656, 0.00773, 0.00781},
     32},
    {1152,
     {0.08245, 0.04068, 0.04404, 0.05116, 0.05768, 0.05804, 0.05533, 0.04444,
      0.03115, 0.02108, 0.01449, 0.01004, 0.0094,  0.00878, 0.00832, 0.00798,
      0.0075,  0.0077,  0.00742, 0.00759, 0.00728, 0.00697, 0.00628, 0.00499,
      0.00385, 0.00376, 0.00471, 0.00296, 0.0019,  0.00234, 0.0026,  0.00274},
     32},
    {1408,
     {0.06598, 0.02008, 0.01738, 0.01954, 0.02173, 0.01938, 0.01517, 0.01371,
      0.01483, 0.01715, 0.01736, 0.01648, 0.0133,  0.00957, 0.00662, 0.00401,
      0.00278, 0.00199, 0.00225, 0.00252, 0.00317, 0.00326, 0.00298, 0.00259,
      0.00187, 0.00158, 0.00119, 0.00093, 0.00083, 0.00083, 0.00103, 0.00124},
     32},
    {1664,
     {0.17606,  0.09391,  0.03835,  0.03465,  0.042355, 0.04287,  0.0387,
      0.034775, 0.033685, 0.03476,  0.03496,  0.034715, 0.033255, 0.030375,
      0.026885, 0.024415, 0.02341,  0.022975, 0.02348,  0.02539,  0.02737,
      0.03101,  0.031855, 0.031615, 0.02946,  0.027245, 0.02494,  0.02343,
      0.02276,  0.0254,   0.02372,  0.01962},
     32},
    {2048,
     {0.25938,  0.073825, 0.038275, 0.03587,  0.032425, 0.02906,  0.024705,
      0.020515, 0.01818,  0.019,    0.019345, 0.02047,  0.02077,  0.02062,
      0.020205, 0.017665, 0.016005, 0.014455, 0.01341,  0.01203,  0.01129,
      0.01026,  0.00962,  0.00856,  0.008115, 0.00754,  0.007215, 0.00707,
      0.006465, 0.006125, 0.00574,  0.004985},
     32},
    {2304,
     {0.2373,  0.15894, 0.1208,  0.09554, 0.08054, 0.07014, 0.05582,
      0.04328, 0.03242, 0.02582, 0.0224,  0.02028, 0.01774, 0.01472,
      0.01312, 0.01178, 0.0085,  0.00758, 0.00542, 0.0041,  0.00304,
      0.00266, 0.00214, 0.00172, 0.00206, 0.00194, 0.0022,  0.00208},
     28},
    {2688,
     {0.134983,  0.098358,  0.104217,  0.1017,    0.0878167,  0.0689667,
      0.04925,   0.0368583, 0.0298083, 0.0254417, 0.02655,    0.0254167,
      0.025475,  0.0242333, 0.0228417, 0.0194833, 0.0168,     0.0151667,
      0.0126833, 0.0123833, 0.0107167, 0.0105,    0.00934167, 0.00705},
     24},
    {3328,
     {0.29576, 0.15044, 0.14676, 0.14216, 0.12636, 0.09456, 0.06612, 0.04588,
      0.0318, 0.02656, 0.01896, 0.01332, 0.0108, 0.00828, 0.00632, 0.00544,
      0.00472, 0.00404, 0.00344},
     19},
    {4096,
     {0.993076, 0.0053138, 0.000839507, 8.76412e-05, 4.6127e-05, 7.84158e-05,
      5.53524e-05, 7.38031e-05, 3.69016e-05, 1.84508e-05, 1.84508e-05,
      1.84508e-05, 1.84508e-05, 5.53524e-05, 0.000253698, 0.000549333},
     16},
    {4480,
     {0.938361, 0.00109564, 0.00338925, 0.00761019, 0.0124665, 0.0146183,
      0.0114009, 0.00590333, 0.00176889, 0.000274961, 2.24301e-05, 3.86727e-07,
      0, 3.86727e-07},
     14},
    {5504,
     {0.573886, 0.30737, 0.0953802, 0.0184983, 0.00238753, 0.000596884,
      0.000477507, 0.000955014, 0.00035813, 0.000119377, 0},
     11},
    {6528,
     {0.661839, 0.168654, 0.0792164, 0.0536627, 0.0255537, 0.00511073,
      0.00255537, 0.00085179, 0.00085179, 0},
     10},
    {8192,
     {0.566342, 0.298924, 0.0909324, 0.0148566, 0.00230533, 0.00128074,
      0.0120389, 0.0261271},
     8},
    {9344,
     {0.428489, 0.201733, 0.169475, 0.119403, 0.0524795, 0.0154068, 0.00288878},
     7},
    {11392, {0.680062, 0.277103, 0.0393023, 0.00264957, 0.00088319}, 5},
    {13696, {0.794071, 0.180237, 0.0241107, 0.00158103}, 4},
    {16384, {0.917628, 0.0788117, 0.00294623, 0.000613803}, 4},
    {18688, {0.942326, 0.0530233, 0.00465116}, 3},
    {21760, {0.896388, 0.101711, 0.00190114}, 3},
    {26112, {0.921354, 0.0786463}, 2},
    {29056, {0.804969, 0.195031}, 2},
    {32768, {0.999901, 9.85783e-05}, 2},
    {37376, {0.89642, 0.10358}, 2},
    {43648, {0.993548, 0.00645161}, 2},
    {52352, {0.927823, 0.0721773}, 2},
    {65536, {0.933277, 0.066723}, 2},
    {87296, {0.986706, 0.0132939}, 2},
    {104832, {0.965966, 0.034034}, 2},
    {131072, {0.989343, 0.0106572}, 2},
    {174720, {0.981791, 0.0182092}, 2},
    {262144, {1}, 1}};

const SameSpanHistogram& GetSameSpanHistogram(size_t size) {
  auto it = absl::c_lower_bound(kSameSpanHistograms, size,
                                [](const SameSpanHistogram& entry, size_t val) {
                                  return entry.size < val;
                                });
  if (it == std::end(kSameSpanHistograms)) {
    return kSameSpanHistograms[std::size(kSameSpanHistograms) - 1];
  }
  return *it;
}

void DrainHeldObjects(BenchmarkEnv& env, absl::Span<void*> held_objects,
                      int batch_size) {
  int index = 0;
  int held_size = static_cast<int>(held_objects.size());
  while (index < held_size) {
    int count = std::min(batch_size, held_size - index);
    env.central_freelist().InsertRange(
        {&held_objects[index], static_cast<size_t>(count)});
    index += count;
  }
}

// Benchmark interacting with simulated page heap
void BM_Populate(benchmark::State& state) {
  size_t object_size = state.range(0);
  const size_t size_class =
      tc_globals.sizemap().SizeClass(CppPolicy(), object_size);
  const Bytes span_bytes =
      Bytes(tc_globals.sizemap().class_to_pages(size_class).in_bytes());
  const int batch_size = tc_globals.sizemap().num_objects_to_move(size_class);
  int num_objects = 64 * 1024 * 1024 / object_size;

  BenchmarkEnv env(object_size, span_bytes, batch_size);

  std::vector<void*> buffer(num_objects);
  int64_t items_processed = 0;

  while (state.KeepRunningBatch(num_objects)) {
    int index = 0;
    while (index < num_objects) {
      int count = std::min(batch_size, num_objects - index);
      int got = env.central_freelist().RemoveRange(
          absl::MakeSpan(buffer).subspan(index, count));
      index += got;
    }

    state.PauseTiming();
    index = 0;
    while (index < num_objects) {
      int count = std::min(batch_size, num_objects - index);
      env.central_freelist().InsertRange(
          {&buffer[index], static_cast<size_t>(count)});
      index += count;
    }
    items_processed += index;
    state.ResumeTiming();
  }
  state.SetItemsProcessed(items_processed);
}

BENCHMARK(BM_Populate)
    ->Arg(8)
    ->Arg(24)
    ->Arg(32)
    ->Arg(48)
    ->Arg(64)
    ->Arg(80)
    ->Arg(4096)
    ->Arg(28672);

struct ActiveObjectPool {
  std::vector<void*> fifo_queue;
  size_t queue_size = 0;
  size_t head = 0;
  size_t tail = 0;
  std::vector<void*> held_objects;
};

// Sets up the central freelist occupancy by allocating spans, mapping objects,
// and holding back a single object from every span to prevent the spans from
// being deallocated and returned to the page heap.
ActiveObjectPool SetupFreelistOccupancy(BenchmarkEnv& env,
                                        size_t objects_per_span,
                                        int target_free, int batch_size,
                                        size_t size_class, size_t object_size) {
  int free_per_span = objects_per_span - 1;
  // Use a much larger queue to guarantee good mixing and no ringbuffer
  // collision
  int num_spans =
      std::max<int>(1024, (target_free + free_per_span - 1) / free_per_span);

  ActiveObjectPool pool;
  size_t total_objects = num_spans * objects_per_span;
  std::vector<void*> temp_buffer(total_objects);
  int allocated = 0;
  int temp_buffer_size = static_cast<int>(temp_buffer.size());
  while (allocated < temp_buffer_size) {
    int to_get = std::min(batch_size, temp_buffer_size - allocated);
    int got = env.central_freelist().RemoveRange(
        absl::MakeSpan(temp_buffer).subspan(allocated, to_get));
    TC_CHECK_NE(got, 0);
    allocated += got;
  }

  std::vector<Span*> spans(temp_buffer.size());
  env.forwarder().MapObjectsToSpans(absl::MakeSpan(temp_buffer), spans.data(),
                                    size_class);

  std::vector<std::vector<void*>> span_objects;
  absl::flat_hash_map<Span*, int> span_to_idx;
  for (int i = 0; i < temp_buffer_size; ++i) {
    auto [it, inserted] = span_to_idx.try_emplace(
        spans[i], static_cast<int>(span_objects.size()));
    if (inserted) {
      span_objects.push_back({});
    }
    span_objects[it->second].push_back(temp_buffer[i]);
  }

  for (auto& objects : span_objects) {
    if (!objects.empty()) {
      pool.held_objects.push_back(objects[0]);
      objects.erase(objects.begin());
    }
  }

  int total_free_objects = 0;
  for (const auto& objects : span_objects) {
    total_free_objects += static_cast<int>(objects.size());
  }

  const SameSpanHistogram& hist = GetSameSpanHistogram(object_size);
  int num_batches = std::max<int>(1, total_free_objects / batch_size);
  std::vector<int> run_lengths;

  for (int k = 0; k < hist.num_counts; ++k) {
    int num_k_batches =
        static_cast<int>(std::round(hist.counts[k] * num_batches));
    int actual_k = std::min(k, batch_size - 1);
    int R = batch_size - actual_k;
    for (int b = 0; b < num_k_batches; ++b) {
      int base = actual_k / R;
      int rem = actual_k % R;
      for (int i = 0; i < R; ++i) {
        run_lengths.push_back(1 + base + (i < rem ? 1 : 0));
      }
    }
  }

  int current_sum = 0;
  for (int L : run_lengths) {
    current_sum += L;
  }
  while (current_sum < total_free_objects) {
    run_lengths.push_back(1);
    current_sum++;
  }

  absl::BitGen rnd;
  absl::c_shuffle(run_lengths, rnd);

  int next_span_idx = 0;
  int span_objects_size = static_cast<int>(span_objects.size());
  for (int L : run_lengths) {
    int s = next_span_idx;
    next_span_idx = (next_span_idx + 1) % span_objects_size;
    int attempts = 0;
    while (span_objects[s].empty() && attempts < span_objects_size) {
      s = (s + 1) % span_objects_size;
      attempts++;
    }
    if (span_objects[s].empty()) {
      break;
    }
    int count = std::min<int>(L, static_cast<int>(span_objects[s].size()));
    for (int i = 0; i < count; ++i) {
      pool.fifo_queue.push_back(span_objects[s].back());
      span_objects[s].pop_back();
    }
  }

  size_t active_count = pool.fifo_queue.size();
  pool.queue_size = active_count + 16 * batch_size;
  pool.fifo_queue.resize(pool.queue_size);
  pool.head = 0;
  pool.tail = active_count;

  return pool;
}

// Benchmark steady state (no page heap interaction).
void BM_SteadyState(benchmark::State& state) {
  const size_t object_size = state.range(0);
  const size_t size_class =
      tc_globals.sizemap().SizeClass(CppPolicy(), object_size);
  const Bytes span_bytes =
      Bytes(tc_globals.sizemap().class_to_pages(size_class).in_bytes());
  const int batch_size = tc_globals.sizemap().num_objects_to_move(size_class);
  const int objects_per_span = span_bytes.raw_num() / object_size;

  if (objects_per_span <= 1) {
    state.SkipWithError("Only for >1 objects per span");
    return;
  }

  BenchmarkEnv env(object_size, span_bytes, batch_size);

  ActiveObjectPool pool =
      SetupFreelistOccupancy(env, objects_per_span, 2 * batch_size, batch_size,
                             size_class, object_size);

  std::vector<void*> to_insert(batch_size);
  std::vector<void*> received(batch_size);
  int64_t items_processed = 0;

  while (state.KeepRunningBatch(batch_size)) {
    for (int i = 0; i < batch_size; ++i) {
      to_insert[i] = pool.fifo_queue[pool.head];
      pool.head = (pool.head + 1) % pool.queue_size;
    }

    env.central_freelist().InsertRange(absl::MakeSpan(to_insert));
    int got = env.central_freelist().RemoveRange(absl::MakeSpan(received));
    TC_CHECK_NE(got, 0);

    for (int i = 0; i < got; ++i) {
      pool.fifo_queue[pool.tail] = received[i];
      pool.tail = (pool.tail + 1) % pool.queue_size;
    }

    items_processed += got;
  }
  state.SetItemsProcessed(items_processed);

  DrainHeldObjects(env, absl::MakeSpan(pool.held_objects), batch_size);
}

BENCHMARK(BM_SteadyState)
    ->Arg(8)
    ->Arg(24)
    ->Arg(32)
    ->Arg(48)
    ->Arg(64)
    ->Arg(80)
    ->Arg(4096)
    ->Arg(28672);

struct MultithreadedState {
  std::unique_ptr<BenchmarkEnv> env;
  std::vector<ActiveObjectPool> thread_pools;
};

static MultithreadedState* g_mt_state = nullptr;

void BM_Multithreaded_Setup(const benchmark::State& state) {
  const size_t object_size = state.range(0);
  const size_t size_class =
      tc_globals.sizemap().SizeClass(CppPolicy(), object_size);
  const Bytes span_bytes =
      Bytes(tc_globals.sizemap().class_to_pages(size_class).in_bytes());
  const int batch_size = tc_globals.sizemap().num_objects_to_move(size_class);
  const int objects_per_span = span_bytes.raw_num() / object_size;

  if (objects_per_span <= 1) {
    return;
  }

  g_mt_state = new MultithreadedState();
  g_mt_state->env =
      std::make_unique<BenchmarkEnv>(object_size, span_bytes, batch_size);
  g_mt_state->thread_pools.resize(state.threads());
  for (int t = 0; t < state.threads(); ++t) {
    g_mt_state->thread_pools[t] = SetupFreelistOccupancy(
        *g_mt_state->env, objects_per_span, 2 * batch_size, batch_size,
        size_class, object_size);
  }
}

void BM_Multithreaded_Teardown(const benchmark::State& state) {
  if (g_mt_state == nullptr) {
    return;
  }
  const size_t object_size = state.range(0);
  const size_t size_class =
      tc_globals.sizemap().SizeClass(CppPolicy(), object_size);
  const int batch_size = tc_globals.sizemap().num_objects_to_move(size_class);

  for (size_t t = 0; t < g_mt_state->thread_pools.size(); ++t) {
    DrainHeldObjects(*g_mt_state->env,
                     absl::MakeSpan(g_mt_state->thread_pools[t].held_objects),
                     batch_size);
  }

  delete g_mt_state;
  g_mt_state = nullptr;
}

void BM_Multithreaded(benchmark::State& state) {
  const size_t object_size = state.range(0);
  const size_t size_class =
      tc_globals.sizemap().SizeClass(CppPolicy(), object_size);
  const Bytes span_bytes =
      Bytes(tc_globals.sizemap().class_to_pages(size_class).in_bytes());
  const int batch_size = tc_globals.sizemap().num_objects_to_move(size_class);
  const int objects_per_span = span_bytes.raw_num() / object_size;

  if (objects_per_span <= 1) {
    state.SkipWithError("Only for >1 objects per span");
    return;
  }

  ActiveObjectPool& pool = g_mt_state->thread_pools[state.thread_index()];

  std::vector<void*> to_insert(batch_size);
  std::vector<void*> received(batch_size);
  int64_t items_processed = 0;

  // Note: We might end up losing some of the permutation chunking over time
  // due to parallel inserts/removes across threads.
  while (state.KeepRunningBatch(batch_size)) {
    for (int i = 0; i < batch_size; ++i) {
      to_insert[i] = pool.fifo_queue[pool.head];
      pool.head = (pool.head + 1) % pool.queue_size;
    }

    g_mt_state->env->central_freelist().InsertRange(absl::MakeSpan(to_insert));
    int got = g_mt_state->env->central_freelist().RemoveRange(
        absl::MakeSpan(received));
    TC_CHECK_NE(got, 0);

    for (int i = 0; i < got; ++i) {
      pool.fifo_queue[pool.tail] = received[i];
      pool.tail = (pool.tail + 1) % pool.queue_size;
    }

    items_processed += got;
  }
  state.SetItemsProcessed(items_processed);
}

BENCHMARK(BM_Multithreaded)
    ->ThreadRange(1, 8)
    ->Arg(8)
    ->Arg(24)
    ->Arg(32)
    ->Arg(48)
    ->Arg(64)
    ->Arg(80)
    ->Arg(4096)
    ->Arg(28672)
    ->Setup(BM_Multithreaded_Setup)
    ->Teardown(BM_Multithreaded_Teardown);

}  // namespace
}  // namespace tcmalloc_internal
}  // namespace tcmalloc
