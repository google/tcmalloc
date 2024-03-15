load("@com_google_protobuf//:protobuf_deps.bzl", local_protobuf_deps = "protobuf_deps")

def _protobuf_deps_impl(_ctx):
    local_protobuf_deps()

protobuf_deps = module_extension(
    implementation = _protobuf_deps_impl,
)
