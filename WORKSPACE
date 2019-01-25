workspace(name = "amqp_bridge")

local_repository(
    name = "envoy",
    path = "envoy",
)

new_local_repository(
    name = "proton",
    path = "proton",
    build_file = "proton.BUILD",
)

# TODO aconway 2018-05-28: SASL dependency
new_local_repository(
    name = "system_libs",
    path = "/usr/lib64",
    build_file_content = """
cc_library(
    name = "sasl2",
    srcs = ["libsasl2.so"],
    visibility = ["//visibility:public"],
)
""",
)

# Copied from envoy/WORKSPACE

load("@envoy//bazel:repositories.bzl", "GO_VERSION", "envoy_dependencies")
load("@envoy//bazel:cc_configure.bzl", "cc_configure")

envoy_dependencies()

cc_configure()

load("@envoy_api//bazel:repositories.bzl", "api_dependencies")

api_dependencies()

load("@io_bazel_rules_go//go:def.bzl", "go_register_toolchains", "go_rules_dependencies")

go_rules_dependencies()

go_register_toolchains(go_version = GO_VERSION)
