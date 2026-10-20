package(default_visibility = ["//visibility:private"])

platform(
    name = "rbe",
    constraint_values = [
        "@platforms//os:linux",
        "@platforms//cpu:x86_64",
    ],
    exec_properties = {
        "container-image": "docker://gcr.io/google.com/absl-177019/oss-linux-latest@sha256:fd9e2ce179533e027466d47157a7581aec99de5a72b46ac43165a5d58855d3ba",
        "OSArch": "amd64",
        "dockerRunAsRoot": "true",
        "cache-silo-key": "n2-highmem-4-2026-10-20",
    },
)

platform(
    name = "linux_gcc",
    constraint_values = [
        "@platforms//os:linux",
        "@platforms//cpu:x86_64",
        "@bazel_tools//tools/cpp:gcc",
    ],
)

platform(
    name = "linux_clang",
    constraint_values = [
        "@platforms//os:linux",
        "@platforms//cpu:x86_64",
        "@bazel_tools//tools/cpp:clang",
    ],
)
