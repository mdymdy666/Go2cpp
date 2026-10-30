# MCP 框架（作为 Go2cpp 的子目录构建；C++17 / Threads 由顶层设置）。
find_package(Threads REQUIRED)

# ---------------------------------------------------------------------------
# 核心库：协议 / 注册表 / 服务端 / 传输 / 内置工具
# ---------------------------------------------------------------------------
set(MCP_CORE_SOURCES
    src/protocol/json_rpc.cpp
    src/server/registry.cpp
    src/server/server.cpp
    src/transport/stdio_transport.cpp
    src/transport/http_transport.cpp
    src/plugin/plugin_loader.cpp
    src/util/process.cpp
    # LLM 客户端与 agent 循环
    src/llm/llm_client.cpp
    src/llm/agent.cpp
    # 配置层 / 校验层 / 向量层 / 检索层
    src/config/config.cpp
    src/net/http_post.cpp
    src/validation/schema.cpp
    src/validation/validators.cpp
    src/vector/embedding.cpp
    src/vector/vector_index.cpp
    src/retrieval/tool_retriever.cpp
    src/retrieval/document_store.cpp
    src/app/application.cpp
    # 内置工具 / 提示 / 资源
    src/tools/builtin_tools.cpp
    src/tools/echo.cpp
    src/tools/calculator.cpp
    src/tools/sleep.cpp
    src/tools/weather.cpp
    src/tools/notification.cpp
    src/tools/robot_control.cpp
    src/tools/resources.cpp
    src/tools/prompts.cpp
    src/tools/retrieval_tools.cpp
    src/tools/monitor_tools.cpp
    # Socket 可观测：本进程 socket 注册表/事件/审计 + 四个 socket_* 工具。
    src/tools/socket_registry.cpp
    src/tools/socket_tools.cpp
    # Socket 自动登记：安装 network 生命周期观察者，业务 socket 创建即入表。
    src/tools/socket_autoregister.cpp
    # MonitorClient 抽象：HTTP 实现 + 测试替身（工具层不直接碰 HTTP）。
    src/tools/monitor_client.cpp
)

add_library(mcp_core STATIC ${MCP_CORE_SOURCES})
target_include_directories(mcp_core
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party
        # Go2Cpp 运行时头（`#include "go2cpp/os/proc.hpp"`）：内置模式下
        # 运行时就是父仓库，include 根是 <repo>/include。
        ${CMAKE_SOURCE_DIR}/include
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_link_libraries(mcp_core PUBLIC go2cpp_runtime go2cpp_os PRIVATE ${CMAKE_DL_LIBS})
# network 的 socket.cpp 引用 hook::mark_system_nonblocking；内置构建下
# 共享库要求显式链接 hook（--as-needed 会因测试自身不引用 hook 而丢弃）。
if (TARGET go2cpp_hook)
    target_link_libraries(mcp_core PUBLIC go2cpp_hook)
    target_link_options(mcp_core INTERFACE "LINKER:--no-as-needed")
endif()
target_compile_options(mcp_core PRIVATE -Wall -Wextra -Wpedantic)

# ---------------------------------------------------------------------------
# 可执行文件
# ---------------------------------------------------------------------------
add_executable(mcp_server src/main.cpp)
target_include_directories(mcp_server PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(mcp_server PRIVATE mcp_core)
target_compile_options(mcp_server PRIVATE -Wall -Wextra -Wpedantic)

# ---------------------------------------------------------------------------
# 示例程序：命令行 agent
# ---------------------------------------------------------------------------
option(MCP_BUILD_EXAMPLES "Build MCP examples" ON)
if (MCP_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

# ---------------------------------------------------------------------------
# 测试
# ---------------------------------------------------------------------------
if (MCP_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
