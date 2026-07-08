package(
    default_applicable_licenses = ["//tcmalloc:license"],
    default_visibility = ["//visibility:private"],
)

platform(
    name = "rbe",
    constraint_values = [
        "@platforms//os:linux",
        "@platforms//cpu:x86_64",
    ],
    exec_properties = {
        "container-image": "docker://gcr.io/google.com/absl-177019/gloop-linux-latest@sha256:903f3d7733dd615bc1542b548fab4b649cf6e48118251a2c6527ce4e30543c6b",
        "OSArch": "amd64",
        "dockerRunAsRoot": "true",
        "cache-silo-key": "n2-highmem-8-2026-07-07",
    },
)
