# ---------------------------------------------------------------------------
# fec_controller：自适应 FEC 冗余度（P1-2，丢包闭环 / 滞后 / 50% 硬上限）
#
# 定位：**纯计算**模块——只依赖 C++17 标准库，不依赖 fiber / scheduler /
#       任何 IO 或时钟。通道层每个保护周期喂一次丢包率，拿
#       `RecommendedRedundancy()`（或 `RecommendedParityShards(k)`）去改
#       `bento::FecConfig::parity_shards`。
#
# 可裁剪：`-DGO2CPP_WITH_FEC_CONTROLLER=OFF` 即完全移出构建；它不被任何现有
#         运行库链接，关掉不影响 go2cpp_runtime / bento / tunnel。
#         默认 ON：构建成本可忽略，且测试需要它。
#
# 测试：`go2cpp_fec_controller_test`（0% / 10% / 50% 丢包场景、抖动滞后、
#       闭环升降、配置边界、与固定 20% 的带宽对照）。
# ---------------------------------------------------------------------------

option(GO2CPP_WITH_FEC_CONTROLLER
    "Build go2cpp_fec_controller (P1-2: adaptive FEC redundancy)" ON)

if (NOT GO2CPP_WITH_FEC_CONTROLLER)
    message(STATUS "fec_controller: OFF (GO2CPP_WITH_FEC_CONTROLLER=OFF)")
    return()
endif()

include(GNUInstallDirs)

set(GO2CPP_FEC_CONTROLLER_ROOT ${CMAKE_CURRENT_SOURCE_DIR})

add_library(go2cpp_fec_controller STATIC
    ${GO2CPP_FEC_CONTROLLER_ROOT}/src/net/fec_controller.cpp)
add_library(go2cpp::fec_controller ALIAS go2cpp_fec_controller)

target_include_directories(go2cpp_fec_controller
    PUBLIC
        $<BUILD_INTERFACE:${GO2CPP_FEC_CONTROLLER_ROOT}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)
target_compile_features(go2cpp_fec_controller PUBLIC cxx_std_17)
set_target_properties(go2cpp_fec_controller PROPERTIES POSITION_INDEPENDENT_CODE ON)

if (MSVC)
    target_compile_options(go2cpp_fec_controller PRIVATE
        /W4 /WX /permissive- /Zc:__cplusplus)
else()
    target_compile_options(go2cpp_fec_controller PRIVATE
        -Wall -Wextra -Wpedantic)
endif()

if (GO2CPP_BUILD_TESTS)
    # 重复调用 enable_testing() 无害：本文件也可能在 Go2CppTests.cmake
    # 的测试块之外被 include。
    enable_testing()

    add_executable(go2cpp_fec_controller_test
        ${GO2CPP_FEC_CONTROLLER_ROOT}/tests/net/test_fec_controller.cpp)
    target_link_libraries(go2cpp_fec_controller_test
        PRIVATE go2cpp_fec_controller)

    if (MSVC)
        target_compile_options(go2cpp_fec_controller_test PRIVATE
            /W4 /WX /permissive- /Zc:__cplusplus)
    else()
        # -Werror：模块是新增纯计算代码，警告必须当场清零，不留给后续。
        target_compile_options(go2cpp_fec_controller_test PRIVATE
            -O2 -Wall -Wextra -Wpedantic -Werror)
    endif()

    add_test(NAME go2cpp_fec_controller_test
             COMMAND go2cpp_fec_controller_test)
    set_tests_properties(go2cpp_fec_controller_test PROPERTIES TIMEOUT 300)
endif()
