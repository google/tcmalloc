load("@bazel_skylib//rules:common_settings.bzl", "string_flag")
load("@rules_rust//rust:defs.bzl", "rust_binary", "rust_library", "rust_test")
load("@rules_rust_bindgen//:defs.bzl", "rust_bindgen_library")

string_flag(
    name = "pagesize",
    build_setting_default = "8k",
)

config_setting(
    name = "large",
    flag_values = {
        ":pagesize": "32k",
    },
    visibility = ["//visibility:public"],
)

config_setting(
    name = "xlarge",
    flag_values = {
        ":pagesize": "256k",
    },
    visibility = ["//visibility:public"],
)

cc_static_library(
    name = "tcmalloc",
    visibility = ["//visibility:public"],
    deps = select({
        ":xlarge": ["//tcmalloc:tcmalloc_256k_pages"],
        ":large": ["//tcmalloc:tcmalloc_large_pages"],
        "//conditions:default": ["//tcmalloc"],
    }),
)

rust_bindgen_library(
    name = "tcmalloc_sys_extensions",
    bindgen_flags = [
        "--allowlist-function=.*MallocExtension_.*",
        "--allowlist-type=.*MallocExtension_Ownership",
        "--allowlist-type=.*MallocExtension_Property",
        "--blocklist-function=.*MallocExtension_GetProfileSamplingRate",
        "--blocklist-function=.*MallocExtension_SetProfileSamplingRate",
        "--blocklist-function=.*MallocExtension_GetGuardedSamplingRate",
        "--blocklist-function=.*MallocExtension_SetGuardedSamplingRate",
        "--blocklist-function=.*MallocExtension_GetSkipSubreleaseInterval",
        "--blocklist-function=.*MallocExtension_SetSkipSubreleaseInterval",
        "--opaque-type=std::optional",  # prevents unsafe comparisons which could result in overflow
        "--opaque-type=std::basic_string",  # prevents unsafe comparisons which could result in overflow
        "--opaque-type=std::basic_string_view",  # allow bindgen to use __BindgenOpaqueArray to repr std::string
        "--opaque-type=std::unique_ptr",  # prevents unsafe comparisons which could result in overflow
        "--use-core",  # don't compile with std, allows baremetal envs
    ],
    cc_lib = "//tcmalloc:malloc_extension",
    clang_flags = [
        # "-v",  # enable for debugging
        "-x",
        "c++",
        "-std=c++17",
    ],
    header = "//tcmalloc:malloc_extension.h",
    alwayslink = 1,
)

rust_bindgen_library(
    name = "tcmalloc_sys",
    bindgen_flags = [
        "--allowlist-function=malloc",
        "--allowlist-function=free",
        "--allowlist-function=calloc",
        "--allowlist-function=realloc",
        "--allowlist-function=malloc_usable_size",
        # comment out the above and uncomment the two lines below to generate bindings for everything
        # "--opaque-type=std::.*",
        # "--blocklist-item=std::value",
        "--use-core",  # don't compile with std, allows baremetal envs
    ],
    cc_lib = ":tcmalloc",
    clang_flags = [
        # "-v",  # enable for debugging
        "-x",
        "c++",
        "-std=c++17",
    ],
    header = "//tcmalloc:tcmalloc.h",
)

rust_library(
    name = "tcmalloc_rs",
    srcs = ["lib.rs"],
    visibility = ["//visibility:public"],
    deps = [":tcmalloc_sys"],
)

rust_library(
    name = "tcmalloc_extensions_rs",
    srcs = ["extension.rs"],
    visibility = ["//visibility:public"],
    deps = [":tcmalloc_sys_extensions"],
)

# bazel test //tcmalloc_rs/... --extra_toolchains=@llvm_toolchain//:cc-toolchain-x86_64-linux
rust_test(
    name = "test_malloc",
    size = "small",
    crate = ":tcmalloc_rs",
)

rust_test(
    name = "test_malloc_extension",
    size = "small",
    crate = ":tcmalloc_extensions_rs",
    deps = [":tcmalloc_rs"],
)

# bazel run //tcmalloc_rs:main --extra_toolchains=@llvm_toolchain//:cc-toolchain-x86_64-linux
rust_binary(
    name = "main",
    srcs = ["main.rs"],
    rustc_flags = [
        "-Ccodegen-units=1",
        "-Copt-level=3",
        "-Cstrip=symbols",
    ],
    deps = [
        ":tcmalloc_extensions_rs",
        ":tcmalloc_rs",
    ],
)
