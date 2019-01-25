package(default_visibility = ["//visibility:public"])


genrule(
    name = "build",
    visibility = ["//visibility:private"],
    srcs = glob(["**"]),
    outs = ["cpp/libqpid-proton-cpp-static.a",
            "c/libqpid-proton-proactor-static.a",
            "c/libqpid-proton-core-static.a",
            "cpp/config_presets.hpp",
            "ruby/cproton.so"],
    cmd = "SRC=$$PWD/$$(dirname $(location CMakeLists.txt)) && cd $(@D) && cmake -DBUILD_BINDINGS=\"cpp;ruby\" -DBUILD_STATIC_LIBS=YES -DSSL_IMPL=none -DENABLE_JSONCPP=NO -DENABLE_FUZZ_TESTING=NO $$SRC && cmake --build ."
)

cc_library(
    name = "cpp_lib",
    srcs = ["cpp/libqpid-proton-cpp-static.a",
            "c/libqpid-proton-proactor-static.a",
            "c/libqpid-proton-core-static.a"],
)

cc_library(
    name = "cpp_interface",
    hdrs = glob(["**/*.h", "**/*.hpp"]) + ["cpp/config_presets.hpp"],
    includes = ["c/include", "cpp/include", "cpp"],
)

filegroup(
    name = "ruby",
    srcs = glob(["ruby/lib/**/*.rb"]) + ["ruby/cproton.so"],
)
