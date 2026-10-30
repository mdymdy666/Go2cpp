# ---------------------------------------------------------------------------
# go2cpp::error 验收测试（独立工程）
#
# 为什么是独立工程：本任务的改动边界只允许把测试放在 tests/** 下，不允许
# 修改根 CMakeLists / cmake/Go2CppTests.cmake，因此这里自带一个最小工程，
# 直接编译 header-only 的错误体系与旧 error 的实现单元，注册 ctest 用例。
#
# 两个可执行文件：
#   error_enhanced_test —— 23 码词表/映射/类型链/注册表/互操作/随机/压测，
#                          不依赖 Fiber 运行时；
#   error_fiber_test    —— ErrorLocal 的 Fiber 迁移语义，仅 Linux x86_64
#                          （原生 context 后端）构建。
#
# 用法：
#   tests/error/run.sh
# 或：
#   cmake -S tests/error -B /tmp/go2cpp-error-enhanced-build -DCMAKE_BUILD_TYPE=Release
#   cmake --build /tmp/go2cpp-error-enhanced-build -j
#   ctest --test-dir /tmp/go2cpp-error-enhanced-build --output-on-failure
# ---------------------------------------------------------------------------
cmake_minimum_required(VERSION 3.20)

project(go2cpp_error_enhanced_tests LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Threads REQUIRED)

set(GO2CPP_ERROR_INCLUDE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../include)

# 新错误体系是 header-only（不改 cmake/ 也能被主库用户直接 include）；
# 旧 error 体系与 Bento error 各有实现单元，compat.hpp / bridge.hpp 的适配器
# 需要它们的 vtable/typeinfo 与工厂/遍历实现，因此一并编译。
add_executable(error_enhanced_test
    test_error_enhanced.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../../src/tools/error.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../../src/bento/error/error.cpp)

target_include_directories(error_enhanced_test PRIVATE
    ${GO2CPP_ERROR_INCLUDE_DIR})

# 零警告是验收条件：与主库同级的 -Wall -Wextra -Wpedantic，并升级为 -Werror。
target_compile_options(error_enhanced_test PRIVATE -Wall -Wextra -Wpedantic -Werror)

# 主库默认构建类型为空（-O0）；独立工程在未指定时用 -O2，让深链/压测
# 的耗时更接近真实发布配置，同时不改变调用方的显式选择。
if (NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    target_compile_options(error_enhanced_test PRIVATE -O2)
endif()

target_link_libraries(error_enhanced_test PRIVATE Threads::Threads)

enable_testing()
add_test(NAME error_enhanced COMMAND error_enhanced_test)
set_tests_properties(error_enhanced PROPERTIES TIMEOUT 300)

# ---------------------------------------------------------------------------
# Fiber 迁移测试：需要原生 context 后端，只在 Linux x86_64 构建。
# ---------------------------------------------------------------------------
set(GO2CPP_ERROR_NATIVE_CONTEXT OFF)
if (CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_SIZEOF_VOID_P EQUAL 8 AND
    NOT CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64|riscv|ppc|s390")
    set(GO2CPP_ERROR_NATIVE_CONTEXT ON)
endif()

if (GO2CPP_ERROR_NATIVE_CONTEXT)
    enable_language(ASM)
    add_executable(error_fiber_test
        test_error_fiber.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../../src/concurrency/fiber.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../../src/concurrency/fiber_local.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../../src/concurrency/context_backend.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../../src/concurrency/context_x64.S
        ${CMAKE_CURRENT_SOURCE_DIR}/../../src/debug/debug_event_ring.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../../src/tools/error.cpp)

    target_include_directories(error_fiber_test PRIVATE
        ${GO2CPP_ERROR_INCLUDE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/../../third_party)
    target_compile_definitions(error_fiber_test PRIVATE GO2CPP_USE_NATIVE_CONTEXT=1)

    # fiber/debug 是既有实现，可能带历史告警（-Wall 下存在未使用函数）；
    # 这里只对测试源码本身强制 -Werror，保证新增代码零警告。
    target_compile_options(error_fiber_test PRIVATE -Wall -Wextra -Wpedantic)
    set_source_files_properties(test_error_fiber.cpp PROPERTIES COMPILE_OPTIONS "-Werror")
    if (NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
        target_compile_options(error_fiber_test PRIVATE -O2)
    endif()

    target_link_libraries(error_fiber_test PRIVATE Threads::Threads)

    add_test(NAME error_fiber COMMAND error_fiber_test)
    set_tests_properties(error_fiber PROPERTIES TIMEOUT 300)
else()
    message(STATUS "error_fiber_test: skipped (需要 Linux x86_64 原生 context)")
endif()
