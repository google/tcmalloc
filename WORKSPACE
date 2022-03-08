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

workspace(name = "com_google_tcmalloc")

load("@bazel_tools//tools/build_defs/repo:http.bzl", "http_archive")

# Abseil
http_archive(
    name = "com_google_absl",  # 2022-03-04T15:22:54Z
    urls = ["https://github.com/abseil/abseil-cpp/archive/04bde89e5cb33bf4a714a5496fac715481fc4831.zip"],
    strip_prefix = "abseil-cpp-04bde89e5cb33bf4a714a5496fac715481fc4831",
    sha256 = "92d469a1a652fd1944398e560bd0d92ee8e3affbd61ed41fca89bb624b59109e",
)

# GoogleTest/GoogleMock framework. Used by most unit-tests.
http_archive(
    name = "com_google_googletest",  # 2021-05-19T20:10:13Z
    urls = ["https://github.com/google/googletest/archive/aa9b44a18678dfdf57089a5ac22c1edb69f35da5.zip"],
    strip_prefix = "googletest-aa9b44a18678dfdf57089a5ac22c1edb69f35da5",
    sha256 = "8cf4eaab3a13b27a95b7e74c58fb4c0788ad94d1f7ec65b20665c4caf1d245e8",
)

# Google benchmark.
http_archive(
    name = "com_github_google_benchmark",
    urls = ["https://github.com/google/benchmark/archive/0baacde3618ca617da95375e0af13ce1baadea47.zip"],
    strip_prefix = "benchmark-0baacde3618ca617da95375e0af13ce1baadea47",
    sha256 = "62e2f2e6d8a744d67e4bbc212fcfd06647080de4253c97ad5c6749e09faf2cb0",
)

# C++ rules for Bazel.
http_archive(
    name = "rules_cc",  # 2021-05-14T14:51:14Z
    urls = ["https://github.com/bazelbuild/rules_cc/archive/68cb652a71e7e7e2858c50593e5a9e3b94e5b9a9.zip"],
    strip_prefix = "rules_cc-68cb652a71e7e7e2858c50593e5a9e3b94e5b9a9",
    sha256 = "1e19e9a3bc3d4ee91d7fcad00653485ee6c798efbbf9588d40b34cbfbded143d",
)

# Python rules
#
# This is explicitly added to workaround
# https://github.com/bazelbuild/rules_python/issues/437.
http_archive(
    name = "rules_python",
    urls = ["https://github.com/bazelbuild/rules_python/releases/download/0.1.0/rules_python-0.1.0.tar.gz"],
    sha256 = "b6d46438523a3ec0f3cead544190ee13223a52f6a6765a29eae7b7cc24cc83a0",
)

# Proto rules for Bazel and Protobuf
http_archive(
    name = "com_google_protobuf",
    urls = ["https://github.com/protocolbuffers/protobuf/archive/13d559beb6967033a467a7517c35d8ad970f8afb.zip"],
    strip_prefix = "protobuf-13d559beb6967033a467a7517c35d8ad970f8afb",
    sha256 = "9ca59193fcfe52c54e4c2b4584770acd1a6528fc35efad363f8513c224490c50",
)
load("@com_google_protobuf//:protobuf_deps.bzl", "protobuf_deps")
protobuf_deps()

http_archive(
    name = "rules_proto",
    sha256 = "66bfdf8782796239d3875d37e7de19b1d94301e8972b3cbd2446b332429b4df1",
    strip_prefix = "rules_proto-4.0.0",
    urls = [
        "https://mirror.bazel.build/github.com/bazelbuild/rules_proto/archive/refs/tags/4.0.0.tar.gz",
        "https://github.com/bazelbuild/rules_proto/archive/refs/tags/4.0.0.tar.gz",
    ],
)

load("@rules_proto//proto:repositories.bzl", "rules_proto_dependencies", "rules_proto_toolchains")
rules_proto_dependencies()
rules_proto_toolchains()

# Fuzzing
http_archive(
    name = "rules_fuzzing",
    sha256 = "a5734cb42b1b69395c57e0bbd32ade394d5c3d6afbfe782b24816a96da24660d",
    strip_prefix = "rules_fuzzing-0.1.1",
    urls = ["https://github.com/bazelbuild/rules_fuzzing/archive/v0.1.1.zip"],
)

# Protobuf
load("@rules_fuzzing//fuzzing:repositories.bzl", "rules_fuzzing_dependencies")

rules_fuzzing_dependencies()

load("@rules_fuzzing//fuzzing:init.bzl", "rules_fuzzing_init")

rules_fuzzing_init()
