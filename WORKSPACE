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
    name = "com_google_absl",
    urls = ["https://github.com/abseil/abseil-cpp/archive/2069dc796aa255f7c82861c6e83b82d001ceef4a.zip"],
    strip_prefix = "abseil-cpp-2069dc796aa255f7c82861c6e83b82d001ceef4a",
    sha256 = "b06a3df7a797807e57fc852aba250a1ee51ba8a59a874a23f8226a0d687faa3c",
)

# GoogleTest/GoogleMock framework. Used by most unit-tests.
http_archive(
    name = "com_google_googletest",
    urls = ["https://github.com/google/googletest/archive/13a433a94dd9c7e55907d7a9b75f44ff82f309eb.zip"],
    strip_prefix = "googletest-13a433a94dd9c7e55907d7a9b75f44ff82f309eb",
    sha256 = "205ddbea89a0dff059cd681f3ec9b0a6c12de7036a04cd57f0254105257593d9",
)

# Google benchmark.
http_archive(
    name = "com_github_google_benchmark",
    urls = ["https://github.com/google/benchmark/archive/16703ff83c1ae6d53e5155df3bb3ab0bc96083be.zip"],
    strip_prefix = "benchmark-16703ff83c1ae6d53e5155df3bb3ab0bc96083be",
    sha256 = "59f918c8ccd4d74b6ac43484467b500f1d64b40cc1010daa055375b322a43ba3",
)

# C++ rules for Bazel.
http_archive(
    name = "rules_cc",
    urls = [
        "https://github.com/bazelbuild/rules_cc/archive/7e650b11fe6d49f70f2ca7a1c4cb8bcc4a1fe239.zip",
    ],
    strip_prefix = "rules_cc-7e650b11fe6d49f70f2ca7a1c4cb8bcc4a1fe239",
    sha256 = "682a0ce1ccdac678d07df56a5f8cf0880fd7d9e08302b8f677b92db22e72052e",
)
