# ---------------------------------------------------------------------------
# go2cpp::error 增强测试（独立工程）
#
# 为什么是独立工程：本任务的改动边界只允许把测试放在 tests/** 下，不允许
# 修改根 CMakeLists / cmake/Go2CppTests.cmake，因此这里自带一个最小工程，
# 直接编译 header-only 的错误体系与旧 error 的实现单元，注册一个 ctest 用例。
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

# 新错误体系是 header-only（不改 cmake/ 也能被主库用户直接 include）；
# 旧 error 体系与 Bento error 各有实现单元，compat.hpp / bridge.hpp 的适配器
# 需要它们的 vtable/typeinfo 与工厂/遍历实现，因此一并编译。
add_executable(error_enhanced_test
    test_error_enhanced.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../../src/tools/error.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../../src/bento/error/error.cpp)

target_include_directories(error_enhanced_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../include)

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
