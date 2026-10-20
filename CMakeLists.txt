cmake_minimum_required(VERSION 3.20)

project(go2cpp_runtime VERSION 0.1.0 LANGUAGES CXX)

if (CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_SIZEOF_VOID_P EQUAL 8 AND
    NOT CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64|riscv|ppc|s390")
    enable_language(ASM)
endif()

include(cmake/Go2CppOptions.cmake)
include(cmake/Go2CppModules.cmake)
include(cmake/Go2CppTests.cmake)
include(cmake/Go2CppInstall.cmake)

# 统一命令行入口（tools/cli）。默认 OFF：
# 不改变库、测试、示例的默认构建，也不进入 install(EXPORT)。
option(GO2CPP_WITH_CLI "Build go2cpp-cli (tools/cli unified command line)" OFF)
if (GO2CPP_WITH_CLI)
    add_subdirectory(tools/cli)
endif()
