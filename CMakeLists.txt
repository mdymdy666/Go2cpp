cmake_minimum_required(VERSION 3.20)

project(go2cpp_runtime VERSION 0.1.0 LANGUAGES CXX)

if (CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_SIZEOF_VOID_P EQUAL 8 AND
    NOT CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64|riscv|ppc|s390")
    enable_language(ASM)
endif()

include(cmake/Go2CppOptions.cmake)
include(cmake/Go2CppModules.cmake)
# 内核监控模块（go2cpp::os::monitor，eBPF）：需要在 Modules 之后定义（依赖
# network/scheduler/channel 目标）；eBPF 不可用时自动降级为仅 /proc 采集。
include(cmake/Go2CppMonitor.cmake)
include(cmake/Go2CppTests.cmake)
include(cmake/Go2CppInstall.cmake)

# 统一命令行入口（tools/cli）。默认 OFF：
# 不改变库、测试、示例的默认构建，也不进入 install(EXPORT)。
option(GO2CPP_WITH_CLI "Build go2cpp-cli (tools/cli unified command line)" OFF)
if (GO2CPP_WITH_CLI)
    add_subdirectory(tools/cli)
endif()

# MCP 框架（内置）：默认 OFF，`-DGO2CPP_WITH_MCP=ON` 启用。
# 依赖本仓库的 go2cpp_runtime 目标（同仓构建，不再 vendored 副本）。
option(GO2CPP_WITH_MCP "Build MCP framework (mcp/)" OFF)
if (GO2CPP_WITH_MCP)
    add_subdirectory(mcp)
endif()

# Bento 传输协议（自研 UDP 可靠/不可靠混合协议）：默认 OFF。
# -DGO2CPP_WITH_BENTO=ON 构建 go2cpp_bento 与测试。
include(cmake/Go2CppBento.cmake)
