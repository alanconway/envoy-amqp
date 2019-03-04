# Workspace to build Envoy AMQP-HTTP bridge.
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

# TODO aconway 2018-05-28: SASL dependency - not portable, requires locally-installed libsasl2
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

# Based on https://github.com/envoyproxy/envoy-filter-example/blob/master/WORKSPACE

load("@envoy//bazel:repositories.bzl", "envoy_dependencies")
load("@envoy//bazel:cc_configure.bzl", "cc_configure")
envoy_dependencies()

load("@rules_foreign_cc//:workspace_definitions.bzl", "rules_foreign_cc_dependencies")
rules_foreign_cc_dependencies()
cc_configure()

load("@envoy_api//bazel:repositories.bzl", "api_dependencies")
api_dependencies()

load("@io_bazel_rules_go//go:def.bzl", "go_rules_dependencies", "go_register_toolchains")
go_rules_dependencies()
go_register_toolchains()

load("@io_bazel_rules_go//proto:def.bzl", "proto_register_toolchains")
proto_register_toolchains()
