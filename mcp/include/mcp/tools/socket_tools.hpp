/**
 * @file socket_tools.hpp
 * @brief 工具层：把本进程 socket 可观测能力暴露成四个 MCP 工具。
 * @author go2cpp contributors
 * @date 2026-10-30
 * @copyright MIT License
 *
 * @details
 * - `socket_list()`：本进程全部 socket（fd/协议/地址/状态/是否可关闭）；
 * - `socket_stats(fd)`：收发字节、错误、超时；TCP 附内核 `TCP_INFO`；
 * - `socket_events(fd, n)`：最近 n 条事件（登记/连接/读写/错误/超时/关闭）；
 * - `socket_close(fd)`：只关闭白名单（已登记）内的 socket，并写审计。
 *
 * 数据源是 `SocketRegistry`（见 socket_registry.hpp）。工具层只做参数解析与
 * JSON 输出，不做系统调用；审计汇可注入（测试用内存实现）。
 */

#pragma once

#include <memory>

#include "mcp/server/registry.hpp"
#include "mcp/tools/socket_registry.hpp"

namespace go2cpp::mcp::tools {

/**
 * @brief 注册四个 `socket_*` 工具。
 * @param registry 目标注册表
 * @param audit    审计汇；为空时使用默认文件汇
 *                 （`GO2CPP_MCP_SOCKET_AUDIT` 或 `log/mcp-socket-audit.log`）
 */
void RegisterSocketTools(Registry& registry,
                         std::shared_ptr<SocketAuditSink> audit = nullptr);

}  // namespace go2cpp::mcp::tools
