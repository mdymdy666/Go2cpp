/**
 * @file monitor_tools.hpp
 * @brief 工具层：把监控数据暴露成四个 MCP 工具，只依赖 `MonitorClient` 接口。
 *
 * @details 监控数据由 worker 侧 `Pusher` POST 到 Manager，Manager 提供四个查询
 * 接口。这里为每个接口注册一个工具，让 MCP client（AI）能直接查询：
 *
 * - `os_monitor_latest` → 最近快照；
 * - `os_monitor_stats`  → 每主机汇总 + 健康评分；
 * - `os_monitor_kernel` → syscall/sched/futex/tcp/fileio 摘要；
 * - `os_monitor_health` → Manager 存活与接收计数。
 *
 * 与检索类工具一样，本组工具由应用层在装配阶段注册（不放进
 * `RegisterBuiltinTools`），因为它们依赖「Manager 监听在哪」这一部署配置。
 * 注册只捕获客户端，不发起任何网络请求：Manager 未启动时服务照常可用，调用
 * 工具才返回明确的连接错误。
 *
 * 解耦：本头文件与实现都不 include 网络头文件；生产注入
 * `HttpMonitorClient`，测试注入 `MockMonitorClient`（见 monitor_client.hpp）。
 */

#pragma once

#include <memory>
#include <string>

#include "mcp/server/registry.hpp"
#include "mcp/tools/monitor_client.hpp"

namespace go2cpp::mcp::tools {

/**
 * @brief 注册四个监控查询工具。
 * @param registry 目标注册表
 * @param client   查询实现；为空时仍注册四个工具，但调用返回配置错误
 *
 * 注册后立刻出现在 `tools/list` 中，并随注册表事件进入工具检索索引。
 */
void RegisterMonitorTools(Registry& registry,
                          std::shared_ptr<MonitorClient> client);

/**
 * @brief 便利重载：用配置创建默认的 `HttpMonitorClient` 后注册。
 * @param options Manager 地址与默认超时（`mcp.monitor.*`）
 */
void RegisterMonitorTools(Registry& registry, MonitorOptions options);

}  // namespace go2cpp::mcp::tools
