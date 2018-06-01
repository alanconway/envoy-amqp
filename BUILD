package(default_visibility = ["//visibility:public"])

load(
    "@envoy//bazel:envoy_build_system.bzl",
    "envoy_cc_binary",
    "envoy_cc_library",
    "envoy_cc_test",
)

envoy_cc_binary(
    name = "envoy",
    repository = "@envoy",
    deps = [
        ":amqp_bridge_lib",
        "@envoy//source/exe:envoy_main_entry_lib",
    ],
)

envoy_cc_library(
    name = "amqp_bridge_lib",
    srcs = ["amqp_bridge.cc"],
    repository = "@envoy",
    # external_deps = ["http_parser"],
    deps = [
        "@envoy//include/envoy/buffer:buffer_interface",
        "@envoy//include/envoy/network:connection_interface",
        "@envoy//include/envoy/network:filter_interface",
        "@envoy//include/envoy/registry:registry",
        "@envoy//include/envoy/server:filter_config_interface",
        "@envoy//source/common/common:assert_lib",
        "@envoy//source/common/common:logger_lib",
        "@envoy//source/common/http:conn_manager_lib",
        "@envoy//source/common/http:utility_lib",
        "@envoy//source/common/http/http1:codec_lib",
        "@envoy//source/common/http/http2:codec_lib",
        "@proton//:cpp_interface",
        "@proton//:cpp_lib",
        "@system_libs//:sasl2",
    ],
)


sh_test(
    name = "smoke_test",
    srcs = ["smoke_test.sh"],
    data = [":envoy", "amqp_bridge.yaml", "test_amqp_client.rb", "@proton//:ruby"],
    size = "small"
)

