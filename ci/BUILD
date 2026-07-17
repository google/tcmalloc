package(default_visibility = ["//visibility:private"])

platform(
    name = "rbe",
    constraint_values = [
        "@platforms//os:linux",
        "@platforms//cpu:x86_64",
    ],
    exec_properties = {
        "container-image": "docker://gcr.io/google.com/absl-177019/gloop-linux-latest@sha256:e199623ad0b61b30d240e8bbddf473f65cb1a11da09462001d83b5066dbd9e98",
        "OSArch": "amd64",
        "dockerRunAsRoot": "true",
        "cache-silo-key": "n2-highmem-8-2026-07-07",
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
