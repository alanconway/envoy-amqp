workspace(name = "amqp_bridge")

# Make it a remote repository? Embed proton the same way via git?
local_repository(
    name = "envoy",
    path = "envoy",
)

# Must be hand-built with static libs, see bazel-proton tag for failed attempt at bazel genrule
#     cd proton/bld && cmake -DBUILD_STATIC_LIBS=YES -DSSL_IMPL=none .. && make
new_local_repository(
    name = "proton",
    path = "proton",
    build_file_content = """
package(default_visibility = ["//visibility:public"])

cc_library(
    name = "cpp_lib",
    srcs = ["bld/cpp/libqpid-proton-cpp-static.a", "bld/c/libqpid-proton-proactor-static.a", "bld/c/libqpid-proton-core-static.a"],
)
cc_library(
    name = "cpp_interface",
    hdrs = glob(["**/*.h", "**/*.hpp"]),
    includes = ["c/include", "cpp/include", "bld/c/include", "bld/cpp", "bld/cpp/include"],
)

filegroup(
    name = "ruby",
    srcs = glob(["ruby/lib/**/*.rb", "bld/ruby/cproton.so"]),
)
"""
)

# FIXME aconway 2018-05-28: SASL dependency
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


load("@envoy//bazel:repositories.bzl", "envoy_dependencies")
load("@envoy//bazel:cc_configure.bzl", "cc_configure")

envoy_dependencies()

cc_configure()

load("@envoy_api//bazel:repositories.bzl", "api_dependencies")

api_dependencies()

load("@io_bazel_rules_go//go:def.bzl", "go_rules_dependencies", "go_register_toolchains")
load("@com_lyft_protoc_gen_validate//bazel:go_proto_library.bzl", "go_proto_repositories")

go_proto_repositories(shared = 0)

go_rules_dependencies()

go_register_toolchains()

load("@io_bazel_rules_go//proto:def.bzl", "proto_register_toolchains")
