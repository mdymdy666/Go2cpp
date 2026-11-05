/**
 * @file go2cpp_debug_tools.hpp
 * @brief Go2Cpp 运行时探针的 MCP 工具注册入口。
 *
 * 与 `monitor_tools.hpp`（外部 os::monitor Manager）不同：这些工具直连
 * **目标 Go2Cpp 进程自己**的 DebugServer HTTP 端点（默认 127.0.0.1:39101），
 * 读取 scheduler / fiber / sync / resources / events / dump。
 */

#pragma once

#include "mcp/server/registry.hpp"

namespace go2cpp::mcp::tools {

/// 注册 6 个 `go2cpp_debug_*` 工具。
/// @param registry [in,out] 工具注册表；同名工具按注册表语义覆盖。
/// @note 只读；每次调用才发起 HTTP 请求，注册阶段不访问网络。
void RegisterGo2CppDebugTools(Registry& registry);

}  // namespace go2cpp::mcp::tools
