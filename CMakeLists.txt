cmake_minimum_required(VERSION 3.20)

project(go2cpp_runtime VERSION 0.1.0 LANGUAGES CXX)

# 立即绑定所有 PLT/GOT 重定位（等价 LD_BIND_NOW=1）。
#
# 共享库默认使用懒绑定：首次调用某个跨库符号时，ld.so 的
# _dl_runtime_resolve_xsavec 会把完整的 xsave 状态（AVX-512 机器上可达
# 数 KB）压到**当前栈**上。Bento 的 Fiber 栈初始只有 1 页（默认
# fiber_stack_size=2048 归一化到 4KB），在较深的调度/关停调用链里首次
# 解析符号时，这个 xsave 区域会越过栈边界写进 guard page，表现为
# `do_lookup_x`/`_dl_fixup` 处的 SIGSEGV（实测 scheduler 测试必现）。
# 启动期一次性绑定把这段运行期栈开销彻底消除，代价只是进程启动时多解析
# 几百个符号。
if (UNIX AND NOT APPLE)
    add_link_options(-Wl,-z,now)
endif()

if (CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_SIZEOF_VOID_P EQUAL 8 AND
    NOT CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64|riscv|ppc|s390")
    enable_language(ASM)
endif()

include(cmake/Go2CppOptions.cmake)
# Reflect 库定义必须在 Modules 之前：go2cpp_config 的配置编解码依赖
# go2cpp_reflect（字段反射 + JSON + ReflectCodec）。测试目标放在
# Go2CppReflectTests.cmake，于 Bento 之后 include。
include(cmake/Go2CppReflect.cmake)
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

# Reflect 测试（帧对接测试优先复用 go2cpp_bento，因此必须在 Bento 之后）。
include(cmake/Go2CppReflectTests.cmake)
