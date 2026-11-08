# ---------------------------------------------------------------------------
# reorder_histogram：乱序距离直方图（P0-2，抑制 NACK 误重传）
#
# 定位：**纯计算**模块（L2）——只依赖 C++17 标准库，不依赖 fiber / scheduler
#        / 任何 IO。可靠层在「是否发起重传」前调用 `IsLikelyLost(seq)`。
#
# 可裁剪：`-DGO2CPP_WITH_REORDER_HISTOGRAM=OFF` 即完全移出构建；当前它不被
#         任何现有运行库链接，因此关掉不影响 go2cpp_runtime / bento / tunnel。
#         默认 ON：构建成本可忽略，且测试需要它。
#
# 测试：`go2cpp_reorder_histogram_test`（纯乱序 / 真丢包 / 混合 / 收敛 /
#       停流兜底 / 配置边界 / 6 个网络画像 / 100 万随机包对照）。
# ---------------------------------------------------------------------------

option(GO2CPP_WITH_REORDER_HISTOGRAM
    "Build go2cpp_reorder_histogram (P0-2: reorder histogram / NACK suppression)" ON)

if (NOT GO2CPP_WITH_REORDER_HISTOGRAM)
    message(STATUS "reorder_histogram: OFF (GO2CPP_WITH_REORDER_HISTOGRAM=OFF)")
    return()
endif()

include(GNUInstallDirs)

set(GO2CPP_REORDER_HISTOGRAM_ROOT ${CMAKE_CURRENT_SOURCE_DIR})

add_library(go2cpp_reorder_histogram STATIC
    ${GO2CPP_REORDER_HISTOGRAM_ROOT}/src/net/reorder_histogram.cpp)
add_library(go2cpp::reorder_histogram ALIAS go2cpp_reorder_histogram)

target_include_directories(go2cpp_reorder_histogram
    PUBLIC
        $<BUILD_INTERFACE:${GO2CPP_REORDER_HISTOGRAM_ROOT}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)
target_compile_features(go2cpp_reorder_histogram PUBLIC cxx_std_17)
set_target_properties(go2cpp_reorder_histogram PROPERTIES POSITION_INDEPENDENT_CODE ON)

if (MSVC)
    target_compile_options(go2cpp_reorder_histogram PRIVATE
        /W4 /WX /permissive- /Zc:__cplusplus)
else()
    target_compile_options(go2cpp_reorder_histogram PRIVATE
        -Wall -Wextra -Wpedantic)
endif()

if (GO2CPP_BUILD_TESTS)
    # 重复调用 enable_testing() 无害：本文件也可能在 Go2CppTests.cmake
    # 的测试块之外被 include。
    enable_testing()

    add_executable(go2cpp_reorder_histogram_test
        ${GO2CPP_REORDER_HISTOGRAM_ROOT}/tests/net/test_reorder_histogram.cpp)
    target_link_libraries(go2cpp_reorder_histogram_test
        PRIVATE go2cpp_reorder_histogram)

    if (MSVC)
        target_compile_options(go2cpp_reorder_histogram_test PRIVATE
            /W4 /WX /permissive- /Zc:__cplusplus)
    else()
        # -Werror：模块是新增纯计算代码，警告必须当场清零，不留给后续。
        target_compile_options(go2cpp_reorder_histogram_test PRIVATE
            -O2 -Wall -Wextra -Wpedantic -Werror)
    endif()

    add_test(NAME go2cpp_reorder_histogram_test
             COMMAND go2cpp_reorder_histogram_test)
    set_tests_properties(go2cpp_reorder_histogram_test PROPERTIES TIMEOUT 300)
endif()
