# Copyright 2019 The TCMalloc Authors
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     https://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#
# Internal libraries used for the implementation and testing of TCMalloc.

load("//tcmalloc:copts.bzl", "TCMALLOC_DEFAULT_COPTS")

package(default_visibility = ["//visibility:private"])

licenses(["notice"])  # Apache 2.0

cc_library(
    name = "atomic_danger",
    hdrs = ["atomic_danger.h"],
    copts = TCMALLOC_DEFAULT_COPTS,
    visibility = ["//visibility:private"],
)

cc_library(
    name = "atomic_stats_counter",
    hdrs = ["atomic_stats_counter.h"],
    copts = TCMALLOC_DEFAULT_COPTS,
    visibility = [
        "//tcmalloc:__subpackages__",
    ],
    deps = [
        "@com_google_absl//absl/base:core_headers",
    ],
)

cc_library(
    name = "bits",
    hdrs = ["bits.h"],
    copts = TCMALLOC_DEFAULT_COPTS,
    visibility = [
        "//tcmalloc:__subpackages__",
    ],
    deps = [
        ":logging",
        "@com_google_absl//absl/base:core_headers",
    ],
)

cc_test(
    name = "bits_test",
    srcs = ["bits_test.cc"],
    copts = TCMALLOC_DEFAULT_COPTS,
    deps = [
        ":bits",
        "@com_github_google_benchmark//:benchmark",
        "@com_google_absl//absl/random",
        "@com_google_googletest//:gtest_main",
    ],
)

cc_library(
    name = "config",
    hdrs = ["config.h"],
    copts = TCMALLOC_DEFAULT_COPTS,
    visibility = ["//tcmalloc:__subpackages__"],
)

cc_library(
    name = "declarations",
    hdrs = ["declarations.h"],
    visibility = [
        "//tcmalloc:__subpackages__",
    ],
)

cc_library(
    name = "linked_list",
    hdrs = ["linked_list.h"],
    copts = TCMALLOC_DEFAULT_COPTS,
    visibility = [
        "//tcmalloc:__subpackages__",
    ],
    deps = [
        "//tcmalloc/internal:logging",
        "@com_google_absl//absl/base:core_headers",
    ],
)

cc_test(
    name = "linked_list_test",
    size = "small",
    srcs = ["linked_list_test.cc"],
    copts = TCMALLOC_DEFAULT_COPTS,
    linkstatic = 1,
    deps = [
        ":linked_list",
        "@com_github_google_benchmark//:benchmark",
        "@com_google_absl//absl/container:node_hash_set",
        "@com_google_absl//absl/random",
        "@com_google_googletest//:gtest_main",
    ],
)

cc_library(
    name = "linux_syscall_support",
    hdrs = ["linux_syscall_support.h"],
    copts = TCMALLOC_DEFAULT_COPTS,
    visibility = [
    ],
)

cc_library(
    name = "logging",
    srcs = ["logging.cc"],
    hdrs = ["logging.h"],
    copts = TCMALLOC_DEFAULT_COPTS,
    visibility = [
        "//tcmalloc:__subpackages__",
    ],
    deps = [
        ":parameter_accessors",
        "//tcmalloc:malloc_extension",
        "@com_google_absl//absl/base",
        "@com_google_absl//absl/base:core_headers",
        "@com_google_absl//absl/debugging:stacktrace",
        "@com_google_absl//absl/strings",
    ],
)

cc_test(
    name = "logging_test",
    srcs = ["logging_test.cc"],
    copts = TCMALLOC_DEFAULT_COPTS,
    deps = [
        ":logging",
        "@com_github_google_benchmark//:benchmark",
        "@com_google_absl//absl/base:core_headers",
        "@com_google_googletest//:gtest_main",
    ],
)

cc_library(
    name = "memory_stats",
    srcs = ["memory_stats.cc"],
    hdrs = ["memory_stats.h"],
    copts = TCMALLOC_DEFAULT_COPTS,
    visibility = [
        "//tcmalloc:__subpackages__",
    ],
    deps = [
        ":logging",
        ":util",
        "@com_google_absl//absl/base:core_headers",
        "@com_google_absl//absl/strings",
    ],
)

cc_test(
    name = "memory_stats_test",
    srcs = ["memory_stats_test.cc"],
    copts = TCMALLOC_DEFAULT_COPTS,
    deps = [
        ":memory_stats",
        "@com_github_google_benchmark//:benchmark",
        "@com_google_absl//absl/base:core_headers",
        "@com_google_googletest//:gtest_main",
    ],
)

cc_library(
    name = "mincore",
    srcs = ["mincore.cc"],
    hdrs = ["mincore.h"],
    copts = TCMALLOC_DEFAULT_COPTS,
    linkstatic = 1,
    visibility = [
        "//tcmalloc:__subpackages__",
    ],
)

cc_test(
    name = "mincore_test",
    srcs = ["mincore_test.cc"],
    copts = TCMALLOC_DEFAULT_COPTS,
    linkstatic = 1,
    deps = [
        ":logging",
        ":mincore",
        "@com_github_google_benchmark//:benchmark",
        "@com_google_googletest//:gtest_main",
    ],
)

cc_library(
    name = "parameter_accessors",
    hdrs = ["parameter_accessors.h"],
    copts = TCMALLOC_DEFAULT_COPTS,
    visibility = [
        "//tcmalloc:__subpackages__",
    ],
    deps = [
        "@com_google_absl//absl/base:core_headers",
        "@com_google_absl//absl/types:optional",
    ],
)

cc_library(
    name = "percpu",
    srcs = [
        "percpu.cc",
        "percpu_rseq_asm.S",
        "percpu_rseq_unsupported.cc",
    ],
    hdrs = ["percpu.h"],
    copts = TCMALLOC_DEFAULT_COPTS,
    textual_hdrs = [
        "percpu_rseq_ppc.S",
        "percpu_rseq_x86_64.S",
    ],
    visibility = [
        "//tcmalloc:__subpackages__",
    ],
    deps = [
        ":atomic_danger",
        ":config",
        ":linux_syscall_support",
        ":logging",
        ":util",
        "@com_google_absl//absl/base",
        "@com_google_absl//absl/base:core_headers",
        "@com_google_absl//absl/base:dynamic_annotations",
    ],
)

cc_library(
    name = "proc_maps",
    srcs = ["proc_maps.cc"],
    hdrs = ["proc_maps.h"],
    copts = TCMALLOC_DEFAULT_COPTS,
    visibility = [
        "//tcmalloc:__subpackages__",
    ],
    deps = [
        ":logging",
        "@com_google_absl//absl/base:core_headers",
        "@com_google_absl//absl/strings:str_format",
    ],
)

cc_library(
    name = "range_tracker",
    hdrs = ["range_tracker.h"],
    copts = TCMALLOC_DEFAULT_COPTS,
    visibility = [
        "//tcmalloc:__subpackages__",
    ],
    deps = [
        ":logging",
    ],
)

cc_test(
    name = "range_tracker_test",
    srcs = ["range_tracker_test.cc"],
    copts = TCMALLOC_DEFAULT_COPTS,
    linkstatic = 1,
    deps = [
        ":range_tracker",
        "@com_github_google_benchmark//:benchmark",
        "@com_google_absl//absl/base:core_headers",
        "@com_google_absl//absl/container:fixed_array",
        "@com_google_absl//absl/random",
        "@com_google_absl//absl/random:distributions",
        "@com_google_googletest//:gtest_main",
    ],
)

# An empty rule to force libc malloc instead of TCMalloc.
cc_library(
    name = "system_malloc",
    copts = TCMALLOC_DEFAULT_COPTS,
    linkstatic = 1,
    visibility = [
        "//tcmalloc:__subpackages__",
    ],
)

cc_library(
    name = "timeseries_tracker",
    hdrs = ["timeseries_tracker.h"],
    copts = TCMALLOC_DEFAULT_COPTS,
    visibility = [
        "//tcmalloc:__subpackages__",
    ],
    deps = [
        ":logging",
        "@com_google_absl//absl/base",
        "@com_google_absl//absl/functional:function_ref",
        "@com_google_absl//absl/time",
    ],
)

cc_test(
    name = "timeseries_tracker_test",
    srcs = ["timeseries_tracker_test.cc"],
    copts = TCMALLOC_DEFAULT_COPTS,
    deps = [
        ":timeseries_tracker",
        "@com_github_google_benchmark//:benchmark",
        "@com_google_googletest//:gtest_main",
    ],
)

cc_library(
    name = "util",
    srcs = ["util.cc"],
    hdrs = ["util.h"],
    copts = TCMALLOC_DEFAULT_COPTS,
    visibility = [
        "//tcmalloc:__subpackages__",
    ],
    deps = [
        ":logging",
        "@com_google_absl//absl/base",
        "@com_google_absl//absl/base:core_headers",
        "@com_google_absl//absl/time",
        "@com_google_absl//absl/types:span",
    ],
)
