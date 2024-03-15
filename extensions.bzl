load("@bazel_tools//tools/build_defs/repo:http.bzl", "http_archive")

def _non_module_dependencies_impl(_ctx):
    http_archive(
        name = "rules_fuzzing",
        sha256 = "23bb074064c6f488d12044934ab1b0631e8e6898d5cf2f6bde087adb01111573",
        strip_prefix = "rules_fuzzing-0.3.1",
        urls = ["https://github.com/bazelbuild/rules_fuzzing/archive/v0.3.1.zip"],
    )
    http_archive(
        name = "com_google_protobuf",
        sha256 = "9ca59193fcfe52c54e4c2b4584770acd1a6528fc35efad363f8513c224490c50",
        strip_prefix = "protobuf-13d559beb6967033a467a7517c35d8ad970f8afb",
        urls = ["https://github.com/protocolbuffers/protobuf/archive/13d559beb6967033a467a7517c35d8ad970f8afb.zip"],
    )

non_module_dependencies = module_extension(
    implementation = _non_module_dependencies_impl,
)
