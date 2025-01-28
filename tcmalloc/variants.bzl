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

""" Helper functions to simplify TCMalloc BUILD files """

build_variants = [
    {
        "name": "8k_pages",
        "copts": ["-DTCMALLOC_INTERNAL_8K_PAGES"],
    },
    {
        "name": "deprecated_perthread",
        "copts": ["-DTCMALLOC_INTERNAL_8K_PAGES", "-DTCMALLOC_DEPRECATED_PERTHREAD"],
        "visibility": ["//tcmalloc:tcmalloc_tests"],
    },
    {
        "name": "large_pages",
        "copts": ["-DTCMALLOC_INTERNAL_32K_PAGES"],
    },
    {
        "name": "256k_pages",
        "copts": ["-DTCMALLOC_INTERNAL_256K_PAGES"],
    },
    {
        "name": "small_but_slow",
        "copts": ["-DTCMALLOC_INTERNAL_SMALL_BUT_SLOW"],
    },
    {
        "name": "numa_aware",
        "copts": ["-DTCMALLOC_INTERNAL_8K_PAGES", "-DTCMALLOC_INTERNAL_NUMA_AWARE"],
    },
    {
        "name": "256k_pages_numa_aware",
        "copts": ["-DTCMALLOC_INTERNAL_256K_PAGES", "-DTCMALLOC_INTERNAL_NUMA_AWARE"],
    },
    {
        "name": "legacy_locking",
        "copts": ["-DTCMALLOC_INTERNAL_8K_PAGES", "-DTCMALLOC_INTERNAL_LEGACY_LOCKING"],
    },
]

test_variants = [
    {
        "name": "8k_pages",
        "malloc": "//tcmalloc",
        "deps": ["//tcmalloc:common_8k_pages"],
        "copts": [],
    },
    {
        "name": "32k_pages",
        "malloc": "//tcmalloc:tcmalloc_large_pages",
        "deps": ["//tcmalloc:common_large_pages"],
        "copts": ["-DTCMALLOC_INTERNAL_32K_PAGES"],
        "tags": ["noubsan"],
    },
    {
        "name": "256k_pages",
        "malloc": "//tcmalloc:tcmalloc_256k_pages",
        "deps": ["//tcmalloc:common_256k_pages"],
        "copts": [
            "-DTCMALLOC_INTERNAL_256K_PAGES",
        ],
        "tags": ["noubsan"],
    },
    {
        "name": "small_but_slow",
        "malloc": "//tcmalloc:tcmalloc_small_but_slow",
        "deps": ["//tcmalloc:common_small_but_slow"],
        "copts": ["-DTCMALLOC_INTERNAL_SMALL_BUT_SLOW"],
        "tags": ["noubsan"],
    },
    {
        "name": "256k_pages_pow2",
        "malloc": "//tcmalloc:tcmalloc_256k_pages",
        "deps": [
            "//tcmalloc:common_256k_pages",
        ],
        "copts": ["-DTCMALLOC_INTERNAL_256K_PAGES"],
        "env": {"BORG_EXPERIMENTS": "TEST_ONLY_TCMALLOC_POW2_SIZECLASS"},
        "tags": ["noubsan"],
    },
    {
        "name": "256k_pages_sharded_transfer_cache",
        "malloc": "//tcmalloc:tcmalloc_256k_pages",
        "deps": [
            "//tcmalloc:common_256k_pages",
        ],
        "copts": ["-DTCMALLOC_INTERNAL_256K_PAGES"],
        "env": {"BORG_EXPERIMENTS": "TEST_ONLY_TCMALLOC_SHARDED_TRANSFER_CACHE"},
        "tags": ["noubsan"],
    },
    {
        "name": "numa_aware",
        "malloc": "//tcmalloc:tcmalloc_numa_aware",
        "deps": [
            "//tcmalloc:common_numa_aware",
            "//tcmalloc:want_numa_aware",
        ],
        "copts": ["-DTCMALLOC_INTERNAL_NUMA_AWARE"],
        "tags": ["noubsan"],
    },
    {
        "name": "numa_aware_enabled_runtime",
        "malloc": "//tcmalloc:tcmalloc_numa_aware",
        "deps": [
            "//tcmalloc:common_numa_aware",
        ],
        "copts": ["-DTCMALLOC_INTERNAL_NUMA_AWARE"],
        "env": {"TCMALLOC_NUMA_AWARE": "1"},
        "tags": ["noubsan"],
    },
    {
        "name": "numa_aware_disabled",
        "malloc": "//tcmalloc:tcmalloc_numa_aware",
        "deps": [
            "//tcmalloc:common_numa_aware",
            "//tcmalloc:want_numa_aware",
        ],
        "copts": ["-DTCMALLOC_INTERNAL_NUMA_AWARE"],
        "env": {"TCMALLOC_NUMA_AWARE": "0"},
        "tags": ["noubsan"],
    },
    {
        "name": "256k_pages_numa_aware",
        "malloc": "//tcmalloc:tcmalloc_256k_pages_numa_aware",
        "deps": [
            "//tcmalloc:common_256k_pages_numa_aware",
            "//tcmalloc:want_numa_aware",
        ],
        "copts": ["-DTCMALLOC_INTERNAL_256K_PAGES", "-DTCMALLOC_INTERNAL_NUMA_AWARE"],
        "tags": ["noubsan"],
    },
    {
        "name": "big_span_disable",
        "malloc": "//tcmalloc",
        "deps": [
            "//tcmalloc:common_8k_pages",
        ],
        "env": {"TCMALLOC_DISABLE_BIG_SPAN": "1"},
    },
    {
        "name": "256k_pages_pow2_sharded_transfer_cache",
        "malloc": "//tcmalloc:tcmalloc_256k_pages",
        "deps": [
            "//tcmalloc:common_256k_pages",
        ],
        "copts": ["-DTCMALLOC_INTERNAL_256K_PAGES"],
        "env": {"BORG_EXPERIMENTS": "TEST_ONLY_TCMALLOC_POW2_SIZECLASS,TEST_ONLY_TCMALLOC_SHARDED_TRANSFER_CACHE"},
        "tags": ["noubsan"],
    },
    {
        "name": "legacy_size_classes",
        "malloc": "//tcmalloc",
        "deps": [
            "//tcmalloc:common_8k_pages",
            "//tcmalloc:want_legacy_size_classes",
        ],
        "copts": [],
    },
    {
        "name": "dense_trackers_sorted_on_spans_allocated_test",
        "malloc": "//tcmalloc",
        "deps": ["//tcmalloc:common_8k_pages"],
        "env": {"BORG_EXPERIMENTS": "TEST_ONLY_TCMALLOC_DENSE_TRACKERS_SORTED_ON_SPANS_ALLOCATED"},
    },
    {
        "name": "huge_cache_release_30s",
        "malloc": "//tcmalloc",
        "deps": ["//tcmalloc:common_8k_pages"],
        "env": {"BORG_EXPERIMENTS": "TEST_ONLY_TCMALLOC_HUGE_CACHE_RELEASE_30S"},
    },
    {
        "name": "hpaa",
        "malloc": "//tcmalloc",
        "deps": [
            "//tcmalloc:common_8k_pages",
            "//tcmalloc:want_hpaa",
        ],
    },
    {
        "name": "deprecated_perthread",
        "malloc": "//tcmalloc:tcmalloc_deprecated_perthread",
        "copts": ["-DTCMALLOC_DEPRECATED_PERTHREAD"],
        "deps": [
            "//tcmalloc:common_deprecated_perthread",
        ],
        "tags": ["noubsan"],
    },
    {
        "name": "8k_hint_ablation",
        "malloc": "//tcmalloc",
        "deps": ["//tcmalloc:common_8k_pages"],
        "env": {"BORG_EXPERIMENTS": "TCMALLOC_MIN_HOT_ACCESS_HINT_ABLATION"},
    },
    {
        "name": "flat_cpu_caches",
        "malloc": "//tcmalloc",
        "deps": [
            "//tcmalloc:common_8k_pages",
        ],
        "env": {"PERCPU_VCPU_MODE": "flat"},
    },
    {
        "name": "real_cpu_caches",
        "malloc": "//tcmalloc",
        "deps": [
            "//tcmalloc:common_8k_pages",
        ],
        "env": {"PERCPU_VCPU_MODE": "none"},
    },
    {
        "name": "no_glibc_rseq",
        "malloc": "//tcmalloc",
        "deps": [
            "//tcmalloc:common_8k_pages",
        ],
        "env": {"GLIBC_TUNABLES": "glibc.pthread.rseq=0"},
    },
    {
        "name": "legacy_locking",
        "malloc": "//tcmalloc:tcmalloc_legacy_locking",
        "deps": ["//tcmalloc:common_legacy_locking"],
        "copts": ["-DTCMALLOC_INTERNAL_8K_PAGES", "-DTCMALLOC_INTERNAL_LEGACY_LOCKING"],
        "tags": ["noubsan"],
    },
]

def create_tcmalloc_library(
        name,
        copts,
        linkopts,
        srcs,
        deps,
        **kwargs):
    native.cc_library(
        name = name,
        srcs = srcs,
        copts = copts,
        linkopts = linkopts,
        deps = deps,
        **kwargs
    )

def create_tcmalloc_build_variant_targets(create_one, name, srcs, **kwargs):
    """ Invokes create_one once per TCMalloc variant

    Args:
      create_one: A function invoked once per variant with arguments
        matching those of a cc_binary or cc_test target.
      name: The base name, suffixed with variant names to form target names.
      srcs: Source files to be built.
      **kwargs: Other arguments passed through to create_one.

    Returns:
      A list of the targets generated; i.e. each name passed to create_one.
    """
    copts = kwargs.pop("copts", [])
    deps = kwargs.pop("deps", [])
    linkopts = kwargs.pop("linkopts", [])
    tags = kwargs.pop("tags", [])

    variant_targets = []
    for variant in build_variants:
        inner_target_name = name + "_" + variant["name"]
        variant_targets.append(inner_target_name)
        create_one(
            inner_target_name,
            copts = copts + variant.get("copts", []),
            linkopts = linkopts + variant.get("linkopts", []),
            srcs = srcs,
            deps = deps + variant.get("deps", []),
            tags = tags + variant.get("tags", []),
            **kwargs
        )

    return variant_targets

# Create test_suite of name containing build variants.
def create_tcmalloc_libraries(name, srcs, **kwargs):
    create_tcmalloc_build_variant_targets(
        create_tcmalloc_library,
        name,
        srcs,
        **kwargs
    )

def create_tcmalloc_test_variant_targets(create_one, name, srcs, **kwargs):
    """ Invokes create_one once per TCMalloc variant

    Args:
      create_one: A function invoked once per variant with arguments
        matching those of a cc_binary or cc_test target.
      name: The base name, suffixed with variant names to form target names.
      srcs: Source files to be built.
      **kwargs: Other arguments passed through to create_one.

    Returns:
      A list of the targets generated; i.e. each name passed to create_one.
    """
    copts = kwargs.pop("copts", [])
    deps = kwargs.pop("deps", [])
    linkopts = kwargs.pop("linkopts", [])
    tags = kwargs.pop("tags", [])

    env0 = kwargs.pop("env", {})

    variant_targets = []
    for variant in test_variants:
        inner_target_name = name + "_" + variant["name"]
        variant_targets.append(inner_target_name)
        env = dict(variant.get("env", {}))
        env.update(env0)
        create_one(
            inner_target_name,
            copts = copts + variant.get("copts", []),
            linkopts = linkopts + variant.get("linkopts", []),
            malloc = variant.get("malloc"),
            srcs = srcs,
            deps = deps + variant.get("deps", []),
            env = env,
            tags = tags + variant.get("tags", []),
            **kwargs
        )

    return variant_targets

# Declare an individual test.
def create_tcmalloc_test(
        name,
        copts,
        linkopts,
        malloc,
        srcs,
        deps,
        **kwargs):
    native.cc_test(
        name = name,
        srcs = srcs,
        copts = copts,
        linkopts = linkopts,
        malloc = malloc,
        deps = deps,
        **kwargs
    )

# Create test_suite of name containing tests variants.
def create_tcmalloc_testsuite(name, srcs, **kwargs):
    variant_targets = create_tcmalloc_test_variant_targets(
        create_tcmalloc_test,
        name,
        srcs,
        **kwargs
    )
    native.test_suite(name = name, tests = variant_targets)

# Declare a single benchmark binary.
def create_tcmalloc_benchmark(name, srcs, **kwargs):
    deps = kwargs.pop("deps")
    malloc = kwargs.pop("malloc", "//tcmalloc")

    native.cc_binary(
        name = name,
        srcs = srcs,
        malloc = malloc,
        testonly = 1,
        linkstatic = 1,
        deps = deps + ["//tcmalloc/testing:benchmark_main"],
        **kwargs
    )

# Declare a suite of benchmark binaries, one per variant.
def create_tcmalloc_benchmark_suite(name, srcs, **kwargs):
    variant_targets = create_tcmalloc_test_variant_targets(
        create_tcmalloc_benchmark,
        name,
        srcs,
        **kwargs
    )

    # The first 'variant' is the default 8k_pages configuration. We alias the
    # benchmark name without a suffix to that target so that the default
    # configuration can be invoked without a variant suffix.
    native.alias(
        name = name,
        actual = variant_targets[0],
    )
