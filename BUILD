# Copyright 2024 The TCMalloc Authors
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

load("@bazel_skylib//rules:native_binary.bzl", "native_binary")
load("@rules_cc//cc:defs.bzl", "cc_import")
load("@rules_rust_bindgen//:defs.bzl", "rust_bindgen_toolchain")

native_binary(
    name = "clang",
    src = "@llvm_toolchain_llvm//:bin/clang",
    visibility = ["//tcmalloc_rs:__subpackages__"],
)

cc_import(
    name = "libclang",
    shared_library = "@llvm_toolchain_llvm//:libclang",
    visibility = ["//tcmalloc_rs:__subpackages__"],
)

cc_import(
    name = "libstdcxx",
    static_library = "@llvm_toolchain_llvm//:lib/x86_64-unknown-linux-gnu/libc++.a",
    visibility = ["//tcmalloc_rs:__subpackages__"],
)

rust_bindgen_toolchain(
    name = "rust_bindgen_toolchain",
    bindgen = "@rules_rust_bindgen//3rdparty:bindgen",
    clang = ":clang",
    libclang = ":libclang",
    libstdcxx = ":libstdcxx",
    visibility = ["//tcmalloc_rs:__subpackages__"],
)

toolchain(
    name = "default_bindgen_toolchain",
    toolchain = ":rust_bindgen_toolchain",
    toolchain_type = "@rules_rust_bindgen//:toolchain_type",
    visibility = ["//tcmalloc_rs:__subpackages__"],
)