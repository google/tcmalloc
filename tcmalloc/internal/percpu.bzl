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

"""Helper functions to simplify TCMalloc percpu tests."""

load("@rules_cc//cc:cc_test.bzl", "cc_test")

percpu_test_variants = [
    {
        "name": "",
        "env": {},
    },
    {
        "name": "_flat",
        "env": {"PERCPU_VCPU_MODE": "flat"},
    },
    {
        "name": "_no_glibc_rseq",
        "env": {"GLIBC_TUNABLES": "glibc.pthread.rseq=0"},
    },
    {
        "name": "_real",
        "env": {"PERCPU_VCPU_MODE": "none"},
    },
]

def create_percpu_tcmalloc_testsuite(
        name,
        srcs = ["percpu_tcmalloc_test.cc"],
        copts = [],
        deps = [],
        env = {},
        linkstatic = 1,
        malloc = ":system_malloc",
        tags = [],
        timeout = "long",
        **kwargs):
    """Creates percpu test targets for all percpu modes."""
    targets = []
    for variant in percpu_test_variants:
        test_name = name + variant["name"]
        targets.append(test_name)
        variant_env = dict(variant["env"])
        variant_env.update(env)
        cc_test(
            name = test_name,
            srcs = srcs,
            copts = copts,
            deps = deps,
            env = variant_env,
            linkstatic = linkstatic,
            malloc = malloc,
            tags = tags,
            timeout = timeout,
            **kwargs
        )
    return targets

percpu_testsuite = create_percpu_tcmalloc_testsuite
