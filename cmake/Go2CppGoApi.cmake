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

# Go 标准库缺口的四个测试（os/signal、hash/crc32、crypto/rand、flag）。
# signal 依赖 fork/sigaction，只在 UNIX 上构建；其余跨平台。
set(GO_API_TESTS
    go_api_net
    go_api_concurrency
    go_api_stress
    go_api_crc32
    go_api_crypto_rand
    go_api_flag)
if (UNIX)
    list(APPEND GO_API_TESTS go_api_signal)
endif()

foreach(go_api_test ${GO_API_TESTS})
    set(go_api_source "")
    set(go_api_extra_libs "")
    set(go_api_definitions "")
    if (go_api_test STREQUAL "go_api_net")
        set(go_api_source ${CMAKE_CURRENT_SOURCE_DIR}/tests/go_api/test_go_net.cpp)
    elseif (go_api_test STREQUAL "go_api_concurrency")
        set(go_api_source
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/go_api/test_go_concurrency.cpp)
    elseif (go_api_test STREQUAL "go_api_stress")
        set(go_api_source
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/go_api/test_go_api_stress.cpp)
    elseif (go_api_test STREQUAL "go_api_crc32")
        set(go_api_source ${CMAKE_CURRENT_SOURCE_DIR}/tests/go_api/test_go_crc32.cpp)
        if (TARGET go2cpp_hash)
            list(APPEND go_api_extra_libs go2cpp_hash)
        else()
            # standalone 构建没有 hash 库：直接把实现源文件编进测试目标。
            list(APPEND go_api_source
                ${CMAKE_CURRENT_SOURCE_DIR}/src/hash/crc32.cpp
                ${CMAKE_CURRENT_SOURCE_DIR}/src/hash/hash.cpp)
        endif()
        set(go_api_definitions
            GO_API_CRC32_ORACLE="${CMAKE_CURRENT_SOURCE_DIR}/tests/go_api/testdata/crc32_oracle.txt")
    elseif (go_api_test STREQUAL "go_api_crypto_rand")
        set(go_api_source
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/go_api/test_go_crypto_rand.cpp)
        set(go_api_definitions
            GO_API_RAND_ORACLE="${CMAKE_CURRENT_SOURCE_DIR}/tests/go_api/testdata/rand_oracle.txt")
    elseif (go_api_test STREQUAL "go_api_flag")
        set(go_api_source ${CMAKE_CURRENT_SOURCE_DIR}/tests/go_api/test_go_flag.cpp)
    else()
        set(go_api_source ${CMAKE_CURRENT_SOURCE_DIR}/tests/go_api/test_go_signal.cpp)
    endif()
    add_executable(${go_api_test} ${go_api_source})
    target_link_libraries(${go_api_test} PRIVATE
        go2cpp_go_api ${GO_API_BASE_TARGETS} ${go_api_extra_libs} Threads::Threads)
    if (go_api_definitions)
        target_compile_definitions(${go_api_test} PRIVATE ${go_api_definitions})
    endif()
    # 本目录的头/测试自身按零警告维护；-Wno-unused-variable 是给
    # include/go2cpp/concurrency/rwmutex.hpp 里既存的未使用变量警告留的出口
    # （该文件不在本任务变更范围内）。
    target_compile_options(${go_api_test} PRIVATE
        -Wall -Wextra -Wpedantic -Werror -Wno-unused-variable)
    add_test(NAME ${go_api_test} COMMAND ${go_api_test})
    set_tests_properties(${go_api_test} PROPERTIES TIMEOUT 180)
endforeach()

message(STATUS "Go API compatibility layer: ON (tests=${GO_API_TESTS})")
