# ---------------------------------------------------------------------------
# Go 接口对接层（include/go2cpp/go/**）
#
# 目标：把 JinShan relay 使用的 Go 标准库接口（net/sync/chan/context/error/time/io）
# 以「Go 风格封装」的方式落到 Go2Cpp 现有实现上，并提供可执行的兼容性测试。
#
# 默认 OFF：不改变既有库、测试与安装面。
# 打开方式：
#   cmake -S . -B .build-bento-wsl -DGO2CPP_WITH_GO_API=ON
# 在 Bento standalone 构建里（GO2CPP_BENTO_STANDALONE=ON），复用
# `go2cpp_socket_bufio_standalone` 作为 runtime，并补一个只含
# `src/concurrency/channel.cpp` 的支撑库（Select/错误构造器不在 standalone 库里）。
# ---------------------------------------------------------------------------
option(GO2CPP_WITH_GO_API "Build the Go-style API compatibility layer tests" OFF)

if (NOT GO2CPP_WITH_GO_API)
    return()
endif()

enable_testing()

set(GO_API_BASE_TARGETS "")
if (TARGET go2cpp_socket_bufio_standalone)
    # 独立构建：该目标已包含 socket/context/sync/scheduler/timer/fiber/log/error。
    list(APPEND GO_API_BASE_TARGETS go2cpp_socket_bufio_standalone)

    if (NOT TARGET go2cpp_go_api_channel)
        add_library(go2cpp_go_api_channel STATIC
            ${CMAKE_CURRENT_SOURCE_DIR}/src/concurrency/channel.cpp)
        target_include_directories(go2cpp_go_api_channel
            PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
        target_compile_features(go2cpp_go_api_channel PUBLIC cxx_std_17)
        # 依赖源码有既存警告，本目标保持与 socket_bufio_standalone 相同的策略：
        # 只开常规警告，不把既有问题升级为构建失败。
        target_compile_options(go2cpp_go_api_channel PRIVATE
            -Wall -Wextra -Wpedantic)
        target_link_libraries(go2cpp_go_api_channel
            PUBLIC go2cpp_socket_bufio_standalone Threads::Threads)
    endif()
    list(APPEND GO_API_BASE_TARGETS go2cpp_go_api_channel)
else()
    # 完整构建：按需链接运行时模块（模块目标名见 cmake/Go2CppModules.cmake）。
    foreach(go_api_module
            go2cpp_error go2cpp_context go2cpp_channel go2cpp_scheduler
            go2cpp_fiber go2cpp_sync go2cpp_io go2cpp_network go2cpp_log)
        if (TARGET ${go_api_module})
            list(APPEND GO_API_BASE_TARGETS ${go_api_module})
        endif()
    endforeach()

    if (NOT GO_API_BASE_TARGETS)
        message(WARNING
            "GO2CPP_WITH_GO_API: no runtime library targets found; "
            "Go API tests will not be built")
        return()
    endif()
endif()

add_library(go2cpp_go_api INTERFACE)
target_include_directories(go2cpp_go_api INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(go2cpp_go_api INTERFACE cxx_std_17)

foreach(go_api_test
        go_api_net
        go_api_concurrency
        go_api_stress)
    set(go_api_source "")
    if (go_api_test STREQUAL "go_api_net")
        set(go_api_source ${CMAKE_CURRENT_SOURCE_DIR}/tests/go_api/test_go_net.cpp)
    elseif (go_api_test STREQUAL "go_api_concurrency")
        set(go_api_source
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/go_api/test_go_concurrency.cpp)
    else()
        set(go_api_source
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/go_api/test_go_api_stress.cpp)
    endif()
    add_executable(${go_api_test} ${go_api_source})
    target_link_libraries(${go_api_test} PRIVATE
        go2cpp_go_api ${GO_API_BASE_TARGETS} Threads::Threads)
    # 本目录的头/测试自身按零警告维护；-Wno-unused-variable 是给
    # include/go2cpp/concurrency/rwmutex.hpp 里既存的未使用变量警告留的出口
    # （该文件不在本任务变更范围内）。
    target_compile_options(${go_api_test} PRIVATE
        -Wall -Wextra -Wpedantic -Werror -Wno-unused-variable)
    add_test(NAME ${go_api_test} COMMAND ${go_api_test})
    set_tests_properties(${go_api_test} PROPERTIES TIMEOUT 180)
endforeach()

message(STATUS "Go API compatibility layer: ON (tests=go_api_net/go_api_concurrency/go_api_stress)")
