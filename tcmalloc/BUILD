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
# Description:
#
# tcmalloc is a fast malloc implementation.  See
# https://github.com/google/tcmalloc/tree/master/docs/design.md for a high-level description of
# how this malloc works.

load("//tcmalloc:copts.bzl", "TCMALLOC_DEFAULT_COPTS")

package(default_visibility = ["//visibility:private"])

licenses(["notice"])

exports_files(["LICENSE"])

config_setting(
    name = "llvm",
    flag_values = {
        "@bazel_tools//tools/cpp:compiler": "llvm",
    },
    visibility = ["//visibility:private"],
)

NO_BUILTIN_MALLOC = [
    "-fno-builtin-malloc",
    "-fno-builtin-free",
]

cc_library(
    name = "experiment",
    srcs = ["experiment.cc"],
    hdrs = [
        "experiment.h",
        "experiment_config.h",
    ],
    copts = TCMALLOC_DEFAULT_COPTS,
    deps = [
        ":malloc_extension",
        "//tcmalloc/internal:logging",
        "//tcmalloc/internal:util",
        "@com_google_absl//absl/base:core_headers",
        "@com_google_absl//absl/strings",
        "@com_google_absl//absl/types:optional",
    ],
)

cc_library(
    name = "percpu_tcmalloc",
    hdrs = ["percpu_tcmalloc.h"],
    copts = TCMALLOC_DEFAULT_COPTS,
    deps = [
        "//tcmalloc/internal:mincore",
        "//tcmalloc/internal:percpu",
        "@com_google_absl//absl/base",
        "@com_google_absl//absl/base:dynamic_annotations",
    ],
)

# Dependencies required by :tcmalloc and its variants.  Since :common is built
# several different ways, it should not be included on this list.
tcmalloc_deps = [
    ":experiment",
    ":malloc_extension",
    "@com_google_absl//absl/base",
    "@com_google_absl//absl/base:config",
    "@com_google_absl//absl/base:core_headers",
    "@com_google_absl//absl/base:dynamic_annotations",
    "@com_google_absl//absl/debugging:leak_check",
    "@com_google_absl//absl/debugging:stacktrace",
    "@com_google_absl//absl/debugging:symbolize",
    "@com_google_absl//absl/memory",
    "@com_google_absl//absl/strings",
    "//tcmalloc/internal:declarations",
    "//tcmalloc/internal:linked_list",
    "//tcmalloc/internal:logging",
    "//tcmalloc/internal:memory_stats",
    "//tcmalloc/internal:percpu",
]

# This library provides tcmalloc always
cc_library(
    name = "tcmalloc",
    srcs = [
        "libc_override.h",
        "libc_override_gcc_and_weak.h",
        "libc_override_glibc.h",
        "sampler.h",
        "tcmalloc.cc",
        "tcmalloc.h",
    ],
    copts = TCMALLOC_DEFAULT_COPTS,
    linkstatic = 1,
    visibility = ["//visibility:public"],
    deps = tcmalloc_deps + [
        ":common",
    ],
    alwayslink = 1,
)

# Provides tcmalloc always; use per-thread mode.
#
cc_library(
    name = "tcmalloc_deprecated_perthread",
    srcs = [
        "libc_override.h",
        "libc_override_gcc_and_weak.h",
        "libc_override_glibc.h",
        "tcmalloc.cc",
        "tcmalloc.h",
    ],
    copts = ["-DTCMALLOC_DEPRECATED_PERTHREAD"] + TCMALLOC_DEFAULT_COPTS,
    linkstatic = 1,
    visibility = [
        "//tcmalloc/testing:__pkg__",
    ],
    deps = tcmalloc_deps + [
        ":common_deprecated_perthread",
    ],
    alwayslink = 1,
)

# An opt tcmalloc build with ASSERTs forced on (by turning off
# NDEBUG). Useful for tracking down crashes in production binaries.
# To use add malloc = "//tcmalloc:opt_with_assertions" in your
# target's build rule.
cc_library(
    name = "opt_with_assertions",
    srcs = [
        "libc_override.h",
        "libc_override_gcc_and_weak.h",
        "libc_override_glibc.h",
        "tcmalloc.cc",
        "tcmalloc.h",
    ],
    copts = [
        "-O2",
        "-UNDEBUG",
    ] + TCMALLOC_DEFAULT_COPTS,
    linkstatic = 1,
    visibility = ["//visibility:public"],
    deps = tcmalloc_deps + [
        ":common",
    ],
    alwayslink = 1,
)

cc_library(
    name = "size_class_info",
    hdrs = ["size_class_info.h"],
    copts = TCMALLOC_DEFAULT_COPTS,
    deps = [
        "//tcmalloc/internal:logging",
    ],
)

# List of common source files used by the various tcmalloc libraries.
common_srcs = [
    "arena.cc",
    "arena.h",
    "central_freelist.cc",
    "central_freelist.h",
    "common.cc",
    "common.h",
    "cpu_cache.cc",
    "cpu_cache.h",
    "experimental_56_size_class.cc",
    "experimental_4k_size_class.cc",
    "guarded_page_allocator.h",
    "guarded_page_allocator.cc",
    "huge_address_map.cc",
    "huge_allocator.cc",
    "huge_allocator.h",
    "huge_cache.cc",
    "huge_cache.h",
    "huge_region.h",
    "huge_page_aware_allocator.cc",
    "huge_page_aware_allocator.h",
    "huge_page_filler.h",
    "huge_pages.h",
    "libc_override.h",
    "libc_override_gcc_and_weak.h",
    "libc_override_glibc.h",
    "libc_override_redefine.h",
    "page_allocator.cc",
    "page_allocator.h",
    "page_allocator_interface.cc",
    "page_allocator_interface.h",
    "page_heap.cc",
    "page_heap.h",
    "page_heap_allocator.h",
    "pagemap.cc",
    "pagemap.h",
    "parameters.cc",
    "peak_heap_tracker.cc",
    "sampler.cc",
    "sampler.h",
    "size_classes.cc",
    "span.cc",
    "span.h",
    "stack_trace_table.cc",
    "stack_trace_table.h",
    "static_vars.cc",
    "static_vars.h",
    "stats.cc",
    "system-alloc.cc",
    "system-alloc.h",
    "tcmalloc.h",
    "thread_cache.cc",
    "thread_cache.h",
    "tracking.h",
    "transfer_cache.cc",
    "transfer_cache.h",
]

common_hdrs = [
    "arena.h",
    "central_freelist.h",
    "common.h",
    "cpu_cache.h",
    "guarded_page_allocator.h",
    "huge_address_map.h",
    "huge_allocator.h",
    "tcmalloc_policy.h",
    "huge_cache.h",
    "huge_page_filler.h",
    "huge_pages.h",
    "huge_region.h",
    "huge_page_aware_allocator.h",
    "libc_override.h",
    "page_allocator.h",
    "page_allocator_interface.h",
    "page_heap.h",
    "page_heap_allocator.h",
    "pagemap.h",
    "parameters.h",
    "peak_heap_tracker.h",
    "sampler.h",
    "span.h",
    "stack_trace_table.h",
    "stats.h",
    "static_vars.h",
    "system-alloc.h",
    "tcmalloc.h",
    "thread_cache.h",
    "tracking.h",
    "transfer_cache.h",
]

common_deps = [
    ":experiment",
    ":malloc_extension",
    ":noruntime_size_classes",
    ":percpu_tcmalloc",
    ":size_class_info",
    "@com_google_absl//absl/algorithm:container",
    "@com_google_absl//absl/base",
    "@com_google_absl//absl/base:config",
    "@com_google_absl//absl/base:core_headers",
    "@com_google_absl//absl/base:dynamic_annotations",
    "@com_google_absl//absl/debugging:debugging_internal",
    "@com_google_absl//absl/debugging:stacktrace",
    "@com_google_absl//absl/debugging:symbolize",
    "@com_google_absl//absl/hash:hash",
    "@com_google_absl//absl/memory",
    "@com_google_absl//absl/strings",
    "@com_google_absl//absl/strings:str_format",
    "@com_google_absl//absl/time",
    "@com_google_absl//absl/types:optional",
    "@com_google_absl//absl/types:span",
    "//tcmalloc/internal:atomic_stats_counter",
    "//tcmalloc/internal:bits",
    "//tcmalloc/internal:config",
    "//tcmalloc/internal:declarations",
    "//tcmalloc/internal:linked_list",
    "//tcmalloc/internal:logging",
    "//tcmalloc/internal:mincore",
    "//tcmalloc/internal:parameter_accessors",
    "//tcmalloc/internal:percpu",
    "//tcmalloc/internal:range_tracker",
    "//tcmalloc/internal:timeseries_tracker",
    "//tcmalloc/internal:util",
]

cc_library(
    name = "common",
    srcs = common_srcs,
    hdrs = common_hdrs,
    copts = TCMALLOC_DEFAULT_COPTS,
    linkstatic = 1,
    visibility = ["//tcmalloc:tcmalloc_tests"],
    deps = common_deps,
    alwayslink = 1,
)

cc_library(
    name = "common_deprecated_perthread",
    srcs = common_srcs,
    hdrs = common_hdrs,
    copts = ["-DTCMALLOC_DEPRECATED_PERTHREAD"] + TCMALLOC_DEFAULT_COPTS,
    linkstatic = 1,
    deps = common_deps,
    alwayslink = 1,
)

# TEMPORARY. WILL BE REMOVED.
# Add a dep to this if you want your binary to use hugepage-aware
# allocator.
cc_library(
    name = "want_hpaa",
    srcs = ["want_hpaa.cc"],
    copts = ["-g0"] + TCMALLOC_DEFAULT_COPTS,
    visibility = ["//visibility:public"],
    deps = [
        "@com_google_absl//absl/base:core_headers",
    ],
    alwayslink = 1,
)

# TEMPORARY. WILL BE REMOVED.
# Add a dep to this if you want your binary to use hugepage-aware
# allocator with hpaa_subrelease=true.
cc_library(
    name = "want_hpaa_subrelease",
    srcs = ["want_hpaa_subrelease.cc"],
    copts = ["-g0"] + TCMALLOC_DEFAULT_COPTS,
    visibility = ["//visibility:public"],
    deps = [
        "@com_google_absl//absl/base:core_headers",
    ],
    alwayslink = 1,
)

# TEMPORARY. WILL BE REMOVED.
# Add a dep to this if you want your binary to not use hugepage-aware
# allocator.
cc_library(
    name = "want_no_hpaa",
    srcs = ["want_no_hpaa.cc"],
    copts = ["-g0"] + TCMALLOC_DEFAULT_COPTS,
    visibility = [
        "//tcmalloc/testing:__pkg__",
    ],
    deps = [
        "@com_google_absl//absl/base:core_headers",
    ],
    alwayslink = 1,
)

cc_library(
    name = "runtime_size_classes",
    srcs = ["runtime_size_classes.cc"],
    hdrs = ["runtime_size_classes.h"],
    copts = TCMALLOC_DEFAULT_COPTS,
    visibility = [
        "//visibility:private",
    ],
    deps = [
        ":size_class_info",
        "//tcmalloc/internal:logging",
        "//tcmalloc/internal:util",
        "@com_google_absl//absl/base:core_headers",
        "@com_google_absl//absl/strings",
    ],
    alwayslink = 1,
)

cc_library(
    name = "noruntime_size_classes",
    srcs = ["noruntime_size_classes.cc"],
    hdrs = ["runtime_size_classes.h"],
    copts = TCMALLOC_DEFAULT_COPTS,
    deps = [
        ":size_class_info",
        "@com_google_absl//absl/base:core_headers",
        "@com_google_absl//absl/strings",
    ],
    alwayslink = 1,
)

cc_library(
    name = "tcmalloc_large_pages",
    srcs = [
        "libc_override.h",
        "libc_override_gcc_and_weak.h",
        "libc_override_glibc.h",
        "tcmalloc.cc",
        "tcmalloc.h",
    ],
    copts = ["-DTCMALLOC_LARGE_PAGES"] + TCMALLOC_DEFAULT_COPTS,
    linkstatic = 1,
    visibility = ["//visibility:public"],
    deps = tcmalloc_deps + [
        ":common_large_pages",
    ],
    alwayslink = 1,
)

cc_library(
    name = "common_large_pages",
    srcs = common_srcs,
    hdrs = common_hdrs,
    copts = ["-DTCMALLOC_LARGE_PAGES"] + TCMALLOC_DEFAULT_COPTS,
    linkstatic = 1,
    deps = common_deps,
    alwayslink = 1,
)

# This is another large page configuration (256k)
cc_library(
    name = "tcmalloc_256k_pages",
    srcs = [
        "libc_override.h",
        "libc_override_gcc_and_weak.h",
        "libc_override_glibc.h",
        "tcmalloc.cc",
        "tcmalloc.h",
    ],
    copts = ["-DTCMALLOC_256K_PAGES"] + TCMALLOC_DEFAULT_COPTS,
    linkstatic = 1,
    visibility = ["//visibility:public"],
    deps = tcmalloc_deps + [
        ":common_256k_pages",
    ],
    alwayslink = 1,
)

cc_library(
    name = "common_256k_pages",
    srcs = common_srcs,
    hdrs = common_hdrs,
    copts = ["-DTCMALLOC_256K_PAGES"] + TCMALLOC_DEFAULT_COPTS,
    linkstatic = 1,
    deps = common_deps,
    alwayslink = 1,
)

cc_library(
    name = "tcmalloc_small_but_slow",
    srcs = [
        "libc_override.h",
        "libc_override_gcc_and_weak.h",
        "libc_override_glibc.h",
        "tcmalloc.cc",
        "tcmalloc.h",
    ],
    copts = ["-DTCMALLOC_SMALL_BUT_SLOW"] + TCMALLOC_DEFAULT_COPTS,
    linkstatic = 1,
    visibility = ["//visibility:public"],
    deps = tcmalloc_deps + [
        ":common_small_but_slow",
    ],
    alwayslink = 1,
)

cc_library(
    name = "common_small_but_slow",
    srcs = common_srcs,
    hdrs = common_hdrs,
    copts = ["-DTCMALLOC_SMALL_BUT_SLOW"] + TCMALLOC_DEFAULT_COPTS,
    linkstatic = 1,
    deps = common_deps,
    alwayslink = 1,
)

# Export some header files to tcmalloc/testing/...
package_group(
    name = "tcmalloc_tests",
    packages = [
        "//tcmalloc/testing/...",
    ],
)

cc_library(
    name = "headers_for_tests",
    srcs = [
        "arena.h",
        "central_freelist.h",
        "guarded_page_allocator.h",
        "huge_address_map.h",
        "huge_allocator.h",
        "huge_cache.h",
        "huge_page_aware_allocator.h",
        "huge_page_filler.h",
        "huge_pages.h",
        "huge_region.h",
        "page_allocator.h",
        "page_allocator_interface.h",
        "page_heap.h",
        "page_heap_allocator.h",
        "pagemap.h",
        "parameters.h",
        "peak_heap_tracker.h",
        "stack_trace_table.h",
        "transfer_cache.h",
    ],
    hdrs = [
        "common.h",
        "sampler.h",
        "size_class_info.h",
        "span.h",
        "static_vars.h",
        "stats.h",
        "system-alloc.h",
    ],
    copts = TCMALLOC_DEFAULT_COPTS,
    visibility = ["//tcmalloc:tcmalloc_tests"],
    deps = common_deps,
)

cc_library(
    name = "page_allocator_test_util",
    testonly = 1,
    srcs = [
        "page_allocator_test_util.h",
    ],
    hdrs = ["page_allocator_test_util.h"],
    copts = TCMALLOC_DEFAULT_COPTS,
    deps = [
        ":common",
        ":malloc_extension",
    ],
)

cc_test(
    name = "page_heap_test",
    srcs = ["page_heap_test.cc"],
    copts = NO_BUILTIN_MALLOC + TCMALLOC_DEFAULT_COPTS,
    deps = [
        ":common",
        "@com_github_google_benchmark//:benchmark",
        "@com_google_absl//absl/base",
        "@com_google_absl//absl/memory",
        "@com_google_googletest//:gtest_main",
    ],
)

cc_test(
    name = "huge_cache_test",
    srcs = ["huge_cache_test.cc"],
    copts = TCMALLOC_DEFAULT_COPTS,
    deps = [
        ":common",
        "//tcmalloc/internal:logging",
        "@com_github_google_benchmark//:benchmark",
        "@com_google_absl//absl/base",
        "@com_google_absl//absl/memory",
        "@com_google_absl//absl/random",
        "@com_google_absl//absl/strings",
        "@com_google_absl//absl/time",
        "@com_google_googletest//:gtest_main",
    ],
)

cc_test(
    name = "huge_allocator_test",
    srcs = ["huge_allocator_test.cc"],
    copts = TCMALLOC_DEFAULT_COPTS,
    deps = [
        ":common",
        "//tcmalloc/internal:logging",
        "@com_github_google_benchmark//:benchmark",
        "@com_google_absl//absl/base",
        "@com_google_absl//absl/base:core_headers",
        "@com_google_absl//absl/random",
        "@com_google_absl//absl/time",
        "@com_google_googletest//:gtest_main",
    ],
)

cc_test(
    name = "huge_page_filler_test",
    timeout = "long",
    srcs = ["huge_page_filler_test.cc"],
    copts = TCMALLOC_DEFAULT_COPTS,
    linkstatic = 1,
    deps = [
        ":common",
        "//tcmalloc/internal:logging",
        "@com_github_google_benchmark//:benchmark",
        "@com_google_absl//absl/algorithm:container",
        "@com_google_absl//absl/base",
        "@com_google_absl//absl/base:core_headers",
        "@com_google_absl//absl/container:flat_hash_map",
        "@com_google_absl//absl/container:flat_hash_set",
        "@com_google_absl//absl/flags:flag",
        "@com_google_absl//absl/memory",
        "@com_google_absl//absl/random",
        "@com_google_absl//absl/random:distributions",
        "@com_google_absl//absl/synchronization",
        "@com_google_absl//absl/time",
        "@com_google_googletest//:gtest_main",
    ],
)

cc_test(
    name = "huge_page_aware_allocator_test",
    srcs = ["huge_page_aware_allocator_test.cc"],
    copts = TCMALLOC_DEFAULT_COPTS,
    linkstatic = 1,
    malloc = "//tcmalloc",
    deps = [
        ":common",
        ":malloc_extension",
        ":page_allocator_test_util",
        "//tcmalloc/internal:logging",
        "@com_github_google_benchmark//:benchmark",
        "@com_google_absl//absl/base",
        "@com_google_absl//absl/base:core_headers",
        "@com_google_absl//absl/container:flat_hash_map",
        "@com_google_absl//absl/flags:flag",
        "@com_google_absl//absl/random",
        "@com_google_absl//absl/strings",
        "@com_google_absl//absl/strings:str_format",
        "@com_google_absl//absl/synchronization",
        "@com_google_absl//absl/time",
        "@com_google_googletest//:gtest_main",
    ],
)

cc_test(
    name = "huge_region_test",
    srcs = ["huge_region_test.cc"],
    copts = TCMALLOC_DEFAULT_COPTS,
    deps = [
        ":common",
        "//tcmalloc/internal:logging",
        "@com_github_google_benchmark//:benchmark",
        "@com_google_absl//absl/base",
        "@com_google_absl//absl/random",
        "@com_google_absl//absl/time",
        "@com_google_googletest//:gtest_main",
    ],
)

cc_test(
    name = "guarded_page_allocator_test",
    srcs = ["guarded_page_allocator_test.cc"],
    copts = NO_BUILTIN_MALLOC + TCMALLOC_DEFAULT_COPTS,
    malloc = "//tcmalloc",
    deps = [
        ":common",
        "//tcmalloc/internal:logging",
        "@com_github_google_benchmark//:benchmark",
        "@com_google_absl//absl/base",
        "@com_google_absl//absl/memory",
        "@com_google_absl//absl/strings",
        "@com_google_absl//absl/time",
        "@com_google_googletest//:gtest_main",
    ],
)

cc_test(
    name = "pagemap_test",
    srcs = ["pagemap_test.cc"],
    copts = NO_BUILTIN_MALLOC + TCMALLOC_DEFAULT_COPTS,
    deps = [
        ":common",
        "@com_github_google_benchmark//:benchmark",
        "@com_google_absl//absl/random",
        "@com_google_googletest//:gtest_main",
    ],
)

cc_test(
    name = "realloc_test",
    srcs = ["realloc_test.cc"],
    copts = NO_BUILTIN_MALLOC + TCMALLOC_DEFAULT_COPTS,
    malloc = "//tcmalloc",
    deps = [
        "@com_github_google_benchmark//:benchmark",
        "@com_google_absl//absl/random",
        "@com_google_absl//absl/random:distributions",
        "@com_google_googletest//:gtest_main",
    ],
)

cc_test(
    name = "stack_trace_table_test",
    srcs = ["stack_trace_table_test.cc"],
    copts = NO_BUILTIN_MALLOC + TCMALLOC_DEFAULT_COPTS,
    deps = [
        ":common",
        "//tcmalloc/internal:logging",
        "@com_github_google_benchmark//:benchmark",
        "@com_google_absl//absl/base",
        "@com_google_absl//absl/base:core_headers",
        "@com_google_absl//absl/debugging:stacktrace",
        "@com_google_absl//absl/flags:flag",
        "@com_google_absl//absl/strings",
        "@com_google_absl//absl/strings:str_format",
        "@com_google_googletest//:gtest_main",
    ],
)

cc_test(
    name = "system-alloc_test",
    srcs = ["system-alloc_test.cc"],
    copts = NO_BUILTIN_MALLOC + TCMALLOC_DEFAULT_COPTS,
    malloc = "//tcmalloc",
    deps = [
        ":common",
        ":malloc_extension",
        "//tcmalloc/internal:logging",
        "@com_github_google_benchmark//:benchmark",
        "@com_google_absl//absl/base",
        "@com_google_googletest//:gtest_main",
    ],
)

# This test has been named "large" since before tests were s/m/l.
# The "large" refers to large allocation sizes.
cc_test(
    name = "tcmalloc_large_test",
    size = "small",
    timeout = "moderate",
    srcs = ["tcmalloc_large_test.cc"],
    copts = NO_BUILTIN_MALLOC + TCMALLOC_DEFAULT_COPTS,
    malloc = "//tcmalloc",
    deps = [
        ":common",
        ":malloc_extension",
        "//tcmalloc/internal:logging",
        "@com_github_google_benchmark//:benchmark",
        "@com_google_absl//absl/base",
        "@com_google_absl//absl/base:core_headers",
        "@com_google_absl//absl/container:node_hash_set",
        "@com_google_googletest//:gtest_main",
    ],
)

cc_test(
    name = "malloc_extension_system_malloc_test",
    srcs = ["malloc_extension_system_malloc_test.cc"],
    copts = TCMALLOC_DEFAULT_COPTS,
    malloc = "//tcmalloc/internal:system_malloc",
    deps = [
        ":malloc_extension",
        "@com_github_google_benchmark//:benchmark",
        "@com_google_absl//absl/random",
        "@com_google_googletest//:gtest_main",
    ],
)

cc_test(
    name = "malloc_extension_test",
    srcs = ["malloc_extension_test.cc"],
    copts = NO_BUILTIN_MALLOC + TCMALLOC_DEFAULT_COPTS,
    malloc = "//tcmalloc",
    deps = [
        ":malloc_extension",
        "@com_github_google_benchmark//:benchmark",
        "@com_google_googletest//:gtest_main",
    ],
)

cc_library(
    name = "malloc_extension_fuzz",
    testonly = 1,
    srcs = ["malloc_extension_fuzz.cc"],
    copts = TCMALLOC_DEFAULT_COPTS,
    deps = [
        ":malloc_extension",
        "@com_google_absl//absl/strings",
        "@com_google_absl//absl/types:optional",
    ],
)

cc_test(
    name = "page_allocator_test",
    srcs = ["page_allocator_test.cc"],
    copts = TCMALLOC_DEFAULT_COPTS,
    linkstatic = 1,
    deps = [
        ":common",
        ":malloc_extension",
        ":page_allocator_test_util",
        "//tcmalloc/internal:logging",
        "@com_github_google_benchmark//:benchmark",
        "@com_google_absl//absl/base",
        "@com_google_absl//absl/base:core_headers",
        "@com_google_absl//absl/memory",
        "@com_google_absl//absl/strings",
        "@com_google_absl//absl/time",
        "@com_google_googletest//:gtest_main",
    ],
)

cc_test(
    name = "profile_test",
    size = "medium",
    srcs = ["profile_test.cc"],
    copts = NO_BUILTIN_MALLOC + TCMALLOC_DEFAULT_COPTS,
    linkstatic = 1,
    malloc = "//tcmalloc",
    shard_count = 2,
    deps = [
        ":malloc_extension",
        "//tcmalloc/internal:declarations",
        "//tcmalloc/internal:linked_list",
        "@com_github_google_benchmark//:benchmark",
        "@com_google_absl//absl/container:flat_hash_map",
        "@com_google_absl//absl/synchronization",
        "@com_google_googletest//:gtest_main",
    ],
)

cc_test(
    name = "thread_cache_test",
    size = "medium",
    srcs = ["thread_cache_test.cc"],
    copts = NO_BUILTIN_MALLOC + TCMALLOC_DEFAULT_COPTS,
    linkstatic = 1,
    malloc = "//tcmalloc:tcmalloc_deprecated_perthread",
    deps = [
        ":malloc_extension",
        "//tcmalloc/internal:logging",
        "//tcmalloc/internal:memory_stats",
        "@com_github_google_benchmark//:benchmark",
        "@com_google_absl//absl/base:core_headers",
        "@com_google_absl//absl/strings",
        "@com_google_googletest//:gtest_main",
    ],
)

cc_test(
    name = "size_classes_test",
    srcs = ["size_classes_test.cc"],
    copts = NO_BUILTIN_MALLOC + TCMALLOC_DEFAULT_COPTS,
    malloc = "//tcmalloc",
    deps = [
        ":common",
        ":size_class_info",
        "@com_github_google_benchmark//:benchmark",
        "@com_google_googletest//:gtest_main",
    ],
)

cc_test(
    name = "size_classes_test_large_pages",
    srcs = ["size_classes_test.cc"],
    copts = ["-DTCMALLOC_LARGE_PAGES"] + NO_BUILTIN_MALLOC + TCMALLOC_DEFAULT_COPTS,
    malloc = "//tcmalloc:tcmalloc_large_pages",
    deps = [
        ":common_large_pages",
        ":size_class_info",
        "@com_github_google_benchmark//:benchmark",
        "@com_google_googletest//:gtest_main",
    ],
)

cc_test(
    name = "size_classes_test_256k_pages",
    srcs = ["size_classes_test.cc"],
    copts = ["-DTCMALLOC_256K_PAGES"] + NO_BUILTIN_MALLOC + TCMALLOC_DEFAULT_COPTS,
    malloc = "//tcmalloc:tcmalloc_256k_pages",
    deps = [
        ":common_256k_pages",
        ":size_class_info",
        "@com_github_google_benchmark//:benchmark",
        "@com_google_googletest//:gtest_main",
    ],
)

cc_test(
    name = "size_classes_test_small_but_slow",
    srcs = ["size_classes_test.cc"],
    copts = ["-DTCMALLOC_SMALL_BUT_SLOW"] + NO_BUILTIN_MALLOC + TCMALLOC_DEFAULT_COPTS,
    malloc = "//tcmalloc:tcmalloc_small_but_slow",
    deps = [
        ":common_small_but_slow",
        ":size_class_info",
        "@com_github_google_benchmark//:benchmark",
        "@com_google_googletest//:gtest_main",
    ],
)

cc_test(
    name = "size_classes_test_with_runtime_size_classes",
    srcs = ["size_classes_with_runtime_size_classes_test.cc"],
    copts = NO_BUILTIN_MALLOC + TCMALLOC_DEFAULT_COPTS,
    linkstatic = 1,
    malloc = "//tcmalloc",
    deps = [
        ":common",
        ":runtime_size_classes",
        ":size_class_info",
        "@com_github_google_benchmark//:benchmark",
        "@com_google_absl//absl/strings",
        "@com_google_absl//absl/strings:str_format",
        "@com_google_googletest//:gtest_main",
    ],
)

cc_test(
    name = "heap_profiling_test",
    srcs = ["heap_profiling_test.cc"],
    copts = NO_BUILTIN_MALLOC + TCMALLOC_DEFAULT_COPTS,
    malloc = "//tcmalloc",
    deps = [
        ":common",
        ":malloc_extension",
        "//tcmalloc/internal:logging",
        "@com_github_google_benchmark//:benchmark",
        "@com_google_googletest//:gtest_main",
    ],
)

cc_test(
    name = "runtime_size_classes_test",
    srcs = ["runtime_size_classes_test.cc"],
    copts = NO_BUILTIN_MALLOC + TCMALLOC_DEFAULT_COPTS,
    linkstatic = 1,
    malloc = "//tcmalloc",
    deps = [
        ":runtime_size_classes",
        "@com_github_google_benchmark//:benchmark",
        "@com_google_googletest//:gtest_main",
    ],
)

cc_test(
    name = "span_test",
    srcs = ["span_test.cc"],
    copts = NO_BUILTIN_MALLOC + TCMALLOC_DEFAULT_COPTS,
    malloc = "//tcmalloc",
    deps = [
        ":common",
        "//tcmalloc/internal:logging",
        "@com_github_google_benchmark//:benchmark",
        "@com_google_absl//absl/base",
        "@com_google_absl//absl/container:flat_hash_set",
        "@com_google_absl//absl/random",
        "@com_google_googletest//:gtest_main",
    ],
)

cc_test(
    name = "span_test_small_but_slow",
    srcs = ["span_test.cc"],
    copts = ["-DTCMALLOC_SMALL_BUT_SLOW"] + NO_BUILTIN_MALLOC + TCMALLOC_DEFAULT_COPTS,
    malloc = "//tcmalloc:tcmalloc_small_but_slow",
    deps = [
        ":common_small_but_slow",
        "//tcmalloc/internal:logging",
        "@com_github_google_benchmark//:benchmark",
        "@com_google_absl//absl/base",
        "@com_google_absl//absl/container:flat_hash_set",
        "@com_google_absl//absl/random",
        "@com_google_googletest//:gtest_main",
    ],
)

cc_test(
    name = "span_test_large_pages",
    srcs = ["span_test.cc"],
    copts = ["-DTCMALLOC_LARGE_PAGES"] + NO_BUILTIN_MALLOC + TCMALLOC_DEFAULT_COPTS,
    malloc = "//tcmalloc:tcmalloc_large_pages",
    deps = [
        ":common_large_pages",
        "//tcmalloc/internal:logging",
        "@com_github_google_benchmark//:benchmark",
        "@com_google_absl//absl/base",
        "@com_google_absl//absl/container:flat_hash_set",
        "@com_google_absl//absl/random",
        "@com_google_googletest//:gtest_main",
    ],
)

cc_test(
    name = "span_test_256k_pages",
    srcs = ["span_test.cc"],
    copts = ["-DTCMALLOC_256K_PAGES"] + NO_BUILTIN_MALLOC + TCMALLOC_DEFAULT_COPTS,
    malloc = "//tcmalloc:tcmalloc_256k_pages",
    deps = [
        ":common_256k_pages",
        "//tcmalloc/internal:logging",
        "@com_github_google_benchmark//:benchmark",
        "@com_google_absl//absl/base",
        "@com_google_absl//absl/container:flat_hash_set",
        "@com_google_absl//absl/random",
        "@com_google_googletest//:gtest_main",
    ],
)

cc_test(
    name = "stats_test",
    srcs = ["stats_test.cc"],
    copts = TCMALLOC_DEFAULT_COPTS,
    malloc = "//tcmalloc",
    deps = [
        ":common",
        "@com_github_google_benchmark//:benchmark",
        "@com_google_absl//absl/base",
        "@com_google_absl//absl/time",
        "@com_google_googletest//:gtest_main",
    ],
)

cc_test(
    name = "huge_address_map_test",
    srcs = ["huge_address_map_test.cc"],
    copts = TCMALLOC_DEFAULT_COPTS,
    deps = [
        ":common",
        "@com_github_google_benchmark//:benchmark",
        "@com_google_googletest//:gtest_main",
    ],
)

cc_library(
    name = "malloc_extension",
    srcs = ["malloc_extension.cc"],
    hdrs = [
        "internal_malloc_extension.h",
        "malloc_extension.h",
    ],
    copts = TCMALLOC_DEFAULT_COPTS,
    visibility = [
        "//visibility:public",
    ],
    deps = [
        "//tcmalloc/internal:parameter_accessors",
        "@com_google_absl//absl/base:core_headers",
        "@com_google_absl//absl/base:dynamic_annotations",
        "@com_google_absl//absl/base:malloc_internal",
        "@com_google_absl//absl/functional:function_ref",
        "@com_google_absl//absl/memory",
        "@com_google_absl//absl/strings",
        "@com_google_absl//absl/types:optional",
        "@com_google_absl//absl/types:span",
    ],
)

cc_test(
    name = "experiment_config_test",
    srcs = ["experiment_config_test.cc"],
    copts = TCMALLOC_DEFAULT_COPTS,
    deps = [
        ":experiment",
        "@com_github_google_benchmark//:benchmark",
        "@com_google_googletest//:gtest_main",
    ],
)

cc_library(
    name = "experiment_fuzz",
    testonly = 1,
    srcs = ["experiment_fuzz.cc"],
    copts = TCMALLOC_DEFAULT_COPTS,
    deps = [
        ":experiment",
        "@com_google_absl//absl/strings",
    ],
)

cc_library(
    name = "runtime_size_classes_fuzz",
    testonly = 1,
    srcs = ["runtime_size_classes_fuzz.cc"],
    copts = TCMALLOC_DEFAULT_COPTS,
    deps = [
        ":common",
        ":runtime_size_classes",
        ":size_class_info",
        "@com_google_absl//absl/strings",
    ],
)

cc_test(
    name = "cpu_cache_test",
    srcs = ["cpu_cache_test.cc"],
    copts = TCMALLOC_DEFAULT_COPTS,
    malloc = ":tcmalloc_deprecated_perthread",
    deps = [
        ":common_deprecated_perthread",
        "//tcmalloc/internal:util",
        "@com_github_google_benchmark//:benchmark",
        "@com_google_googletest//:gtest_main",
    ],
)
