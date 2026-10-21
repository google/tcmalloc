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

load("@rules_cc//cc:cc_binary.bzl", "cc_binary")
load("@rules_cc//cc:cc_library.bzl", "cc_library")
load("@rules_cc//cc:cc_test.bzl", "cc_test")

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
        "name": "latency_injection",
        "copts": ["-DTCMALLOC_INTERNAL_8K_PAGES", "-DTCMALLOC_INTERNAL_LATENCY_INJECTION"],
    },
    {
        "name": "legacy_locking2",
        "copts": ["-DTCMALLOC_INTERNAL_8K_PAGES", "-DTCMALLOC_INTERNAL_LEGACY_LOCKING2"],
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
        "name": "sonic_sharded_transfer_cache",
        "malloc": "//tcmalloc",
        "deps": [
            "//tcmalloc:common_8k_pages",
        ],
        "env": {"BORG_EXPERIMENTS": "TCMALLOC_SONIC_SHARDED_TRANSFER_CACHE"},
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
        "name": "partitioned_enabled_runtime",
        "malloc": "//tcmalloc",
        "deps": ["//tcmalloc:common_8k_pages"],
        "env": {"TCMALLOC_HEAP_PARTITIONING": "true"},
        # sanitizers disable heap partitioning & the environment variable is ignored.
        "tags": ["noubsan", "noasan", "nomsan", "notsan"],
    },
    {
        "name": "partitioned_light_runtime",
        "malloc": "//tcmalloc",
        "deps": ["//tcmalloc:common_8k_pages"],
        "env": {"TCMALLOC_HEAP_PARTITIONING": "light"},
        "tags": ["noubsan", "noasan", "nomsan", "notsan"],
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
        "name": "pgho_experiment",
        "malloc": "//tcmalloc",
        "deps": ["//tcmalloc:common_8k_pages"],
        "env": {"BORG_EXPERIMENTS": "TCMALLOC_PGHO_EXPERIMENT"},
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
        "name": "mm_vcpu_cpu_caches",
        "malloc": "//tcmalloc",
        "deps": [
            "//tcmalloc:common_8k_pages",
        ],
        "env": {
            "BORG_EXPERIMENTS": "TEST_ONLY_MM_VCPU",
            "GLIBC_TUNABLES": "glibc.pthread.rseq=0",
        },
    },
    {
        "name": "latency_injection",
        "malloc": "//tcmalloc:tcmalloc_latency_injection",
        "deps": ["//tcmalloc:common_latency_injection"],
        "copts": ["-DTCMALLOC_INTERNAL_8K_PAGES", "-DTCMALLOC_INTERNAL_LATENCY_INJECTION"],
        "tags": ["noubsan"],
    },
    {
        "name": "legacy_locking2",
        "malloc": "//tcmalloc:tcmalloc_legacy_locking2",
        "deps": ["//tcmalloc:common_legacy_locking2"],
        "copts": ["-DTCMALLOC_INTERNAL_8K_PAGES", "-DTCMALLOC_INTERNAL_LEGACY_LOCKING2"],
        "tags": ["noubsan"],
    },
    {
        "name": "tcmalloc_release_free_stale",
        "malloc": "//tcmalloc",
        "deps": ["//tcmalloc:common_8k_pages"],
        "env": {"BORG_EXPERIMENTS": "TCMALLOC_RELEASE_FREE_STALE"},
    },
    {
        "name": "tcmalloc_madv_nohugepage_regions",
        "malloc": "//tcmalloc",
        "deps": ["//tcmalloc:common_8k_pages"],
        "env": {"BORG_EXPERIMENTS": "TCMALLOC_SONIC_MADV_NOHUGEPAGE_REGIONS"},
    },
    {
        "name": "tcmalloc_cfl_prioritization_ablation",
        "malloc": "//tcmalloc",
        "deps": ["//tcmalloc:common_8k_pages"],
        "env": {"BORG_EXPERIMENTS": "TCMALLOC_CFL_PRIORITIZATION_ABLATION"},
    },
]

def create_tcmalloc_library(
        name,
        copts = [],
        linkopts = [],
        srcs = [],
        deps = [],
        **kwargs):
    cc_library(
        name = name,
        srcs = srcs,
        copts = copts,
        linkopts = linkopts,
        deps = deps,
        **kwargs
    )

def create_tcmalloc_build_variant_targets(create_one, name, srcs = [], **kwargs):
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
    variant_deps = kwargs.pop("variant_deps", [])
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
            deps = deps + variant.get("deps", []) + [
                d + "_" + variant["name"]
                for d in variant_deps
            ],
            tags = tags + variant.get("tags", []),
            **kwargs
        )

    return variant_targets

# Create test_suite of name containing build variants.
def create_tcmalloc_libraries(name, srcs = [], **kwargs):
    create_tcmalloc_build_variant_targets(
        create_tcmalloc_library,
        name,
        srcs,
        **kwargs
    )

def _env_target_impl(ctx):
    out = ctx.actions.declare_file(ctx.label.name)
    ctx.actions.symlink(
        output = out,
        target_file = ctx.executable.src,
        is_executable = True,
    )
    runfiles = ctx.runfiles(files = [ctx.executable.src] + ctx.files.data)
    runfiles = runfiles.merge_all([
        d[DefaultInfo].default_runfiles
        for d in ctx.attr.data + [ctx.attr.src]
    ])

    test_env = {}
    inherited_env = []
    if RunEnvironmentInfo in ctx.attr.src:
        test_env.update(ctx.attr.src[RunEnvironmentInfo].environment)
        inherited_env = ctx.attr.src[RunEnvironmentInfo].inherited_environment
    for k in ctx.attr.unset_env:
        test_env.pop(k, None)
    for k, v in ctx.attr.env.items():
        test_env[k] = ctx.expand_location(v)

    providers = [
        DefaultInfo(
            executable = out,
            files = depset([out]),
            runfiles = runfiles,
        ),
        RunEnvironmentInfo(
            environment = test_env,
            inherited_environment = inherited_env,
        ),
    ]
    if testing.ExecutionInfo in ctx.attr.src:
        providers.append(ctx.attr.src[testing.ExecutionInfo])
    if InstrumentedFilesInfo in ctx.attr.src:
        providers.append(ctx.attr.src[InstrumentedFilesInfo])
    return providers

_ENV_TARGET_ATTRS = {
    "src": attr.label(
        executable = True,
        allow_files = True,
        cfg = "target",
        mandatory = True,
    ),
    "data": attr.label_list(allow_files = True),
    "env": attr.string_dict(),
    "unset_env": attr.string_list(),
}

_tcmalloc_env_test = rule(
    implementation = _env_target_impl,
    attrs = _ENV_TARGET_ATTRS,
    test = True,
)

_tcmalloc_env_binary = rule(
    implementation = _env_target_impl,
    attrs = _ENV_TARGET_ATTRS,
    executable = True,
)

def create_tcmalloc_test_variant_targets(create_one, name, srcs = [], **kwargs):
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
    variant_deps = kwargs.pop("variant_deps", [])
    linkopts = kwargs.pop("linkopts", [])
    tags = kwargs.pop("tags", [])
    variants = kwargs.pop("variants", None)

    env0 = kwargs.pop("env", {})

    canonical_binaries = {}
    variant_targets = []
    for variant in test_variants:
        if variants != None and variant["name"] not in variants:
            continue
        inner_target_name = name + "_" + variant["name"]
        variant_targets.append(inner_target_name)
        env = dict(variant.get("env", {}))
        env.update(env0)

        build_variant = variant.get("build_variant")
        if not build_variant:
            for d in variant.get("deps", []):
                if "common_" in d:
                    build_variant = d.rsplit("common_", 1)[1]
                    break
        if not build_variant:
            build_variant = variant["name"]

        variant_copts = copts + variant.get("copts", [])
        variant_linkopts = linkopts + variant.get("linkopts", [])
        variant_malloc = variant.get("malloc")
        variant_full_deps = deps + variant.get("deps", []) + [
            d + "_" + build_variant
            for d in variant_deps
        ]
        build_key = (
            variant_malloc,
            tuple(variant.get("deps", [])),
            tuple(variant.get("copts", [])),
            tuple(variant.get("linkopts", [])),
            build_variant,
        )

        if build_key not in canonical_binaries:
            canonical_binaries[build_key] = (
                inner_target_name,
                list(variant.get("env", {}).keys()),
            )
            create_one(
                inner_target_name,
                copts = variant_copts,
                linkopts = variant_linkopts,
                malloc = variant_malloc,
                srcs = srcs,
                deps = variant_full_deps,
                env = env,
                tags = tags + variant.get("tags", []),
                **kwargs
            )
        else:
            canonical_name, canonical_env_keys = canonical_binaries[build_key]
            unset_env = [k for k in canonical_env_keys if k not in env]
            create_one(
                inner_target_name,
                src_binary = ":" + canonical_name,
                env = env,
                unset_env = unset_env,
                tags = tags + variant.get("tags", []),
                **kwargs
            )

    return variant_targets

def create_tcmalloc_test(
        name,
        copts = [],
        linkopts = [],
        malloc = None,
        srcs = [],
        deps = [],
        **kwargs):
    """Declares an individual TCMalloc test target.

    Args:
      name: Target name.
      copts: Compiler options passed to cc_test.
      linkopts: Linker options passed to cc_test.
      malloc: The malloc target passed to cc_test.
      srcs: Source files to be built.
      deps: Dependencies passed to cc_test.
      **kwargs: Other arguments passed through to cc_test or _tcmalloc_env_test.
    """
    src_binary = kwargs.pop("src_binary", None)
    unset_env = kwargs.pop("unset_env", [])
    if src_binary != None:
        kwargs.pop("linkstatic", None)
        _tcmalloc_env_test(
            name = name,
            src = src_binary,
            unset_env = unset_env,
            **kwargs
        )
        return
    cc_test(
        name = name,
        srcs = srcs,
        copts = copts,
        linkopts = linkopts,
        malloc = malloc,
        deps = deps,
        **kwargs
    )

# Create test_suite of name containing tests variants.
def create_tcmalloc_testsuite(name, srcs = [], **kwargs):
    tags = kwargs.get("tags")
    variant_targets = create_tcmalloc_test_variant_targets(
        create_tcmalloc_test,
        name,
        srcs,
        **kwargs
    )
    native.test_suite(name = name, tests = variant_targets, tags = tags)

# Declare a single benchmark binary.
def create_tcmalloc_benchmark(
        name,
        srcs = [],
        **kwargs):
    """Declares a single TCMalloc benchmark binary.

    Args:
      name: Target name.
      srcs: Source files to be built.
      **kwargs: Other arguments passed through to cc_binary or _tcmalloc_env_binary.
    """
    src_binary = kwargs.pop("src_binary", None)
    unset_env = kwargs.pop("unset_env", [])
    if src_binary != None:
        kwargs.pop("linkstatic", None)
        _tcmalloc_env_binary(
            name = name,
            src = src_binary,
            testonly = 1,
            unset_env = unset_env,
            **kwargs
        )
        return
    deps = kwargs.pop("deps")
    malloc = kwargs.pop("malloc", "//tcmalloc")
    cc_binary(
        name = name,
        srcs = srcs,
        malloc = malloc,
        testonly = 1,
        linkstatic = 1,
        deps = deps + ["@com_github_google_benchmark//:benchmark_main"],
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
