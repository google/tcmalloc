load("@rules_fuzzing//fuzzing:repositories.bzl", "rules_fuzzing_dependencies")

def _rules_fuzzing_deps_impl(_ctx):
    rules_fuzzing_dependencies()

rules_fuzzing_deps = module_extension(
    implementation = _rules_fuzzing_deps_impl,
)
