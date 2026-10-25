/**
 * @file monitor_tools.cpp
 * @brief 监控查询工具实现：只依赖 `MonitorClient` 接口，HTTP 细节在客户端实现里。
 *
 * 所有失败都转换成 `isError=true` 的文本结果，不抛异常——Manager 未启动是
 * 部署状态而非程序错误，服务端必须继续可用。
 */

#include "mcp/tools/monitor_tools.hpp"

#include <algorithm>
#include <cstddef>
#include <memory>
#include <string>
#include <utility>

#include "mcp/tools/tool_builder.hpp"

namespace go2cpp::mcp::tools {

namespace {

constexpr long long kMinTimeoutMs = 100;
constexpr long long kMaxTimeoutMs = 60000;
constexpr long long kMinLimit = 1;
constexpr long long kMaxLimit = 256;
constexpr long long kDefaultLimit = 10;

/// 从参数里取字符串；缺失或类型不符返回空串（校验层通常已经拦过）。
std::string StringArgument(const json& arguments, const char* key) {
    const auto found = arguments.find(key);
    if (found == arguments.end() || !found->is_string()) {
        return {};
    }
    return found->get<std::string>();
}

/// 取整数参数；缺失或类型不符返回 fallback。
long long IntArgument(const json& arguments, const char* key, long long fallback) {
    const auto found = arguments.find(key);
    if (found == arguments.end() || !found->is_number_integer()) {
        return fallback;
    }
    return found->get<long long>();
}

/**
 * @brief 把工具参数翻译成 MonitorQuery：默认值来自客户端，参数可覆盖。
 *
 * `manager_url` 允许单次查询指向另一个 Manager；地址合法性由客户端校验
 * （HttpMonitorClient 只接受 http://），工具层不做协议判断。
 */
MonitorQuery PrepareQuery(const ToolContext& context,
                          const MonitorClient& client) {
    MonitorQuery query = client.DefaultQuery();
    const std::string override_url =
        StringArgument(context.arguments, "manager_url");
    if (!override_url.empty()) {
        query.base_url = override_url;
    }

    int default_timeout = query.timeout_ms;
    if (default_timeout <= 0) {
        default_timeout = 3000;
    }
    const long long timeout =
        IntArgument(context.arguments, "timeout_ms", default_timeout);
    query.timeout_ms = static_cast<int>(
        std::clamp<long long>(timeout, kMinTimeoutMs, kMaxTimeoutMs));

    const long long default_limit =
        query.limit > 0 ? static_cast<long long>(query.limit) : kDefaultLimit;
    const long long limit =
        IntArgument(context.arguments, "limit", default_limit);
    query.limit = static_cast<int>(
        std::clamp<long long>(limit, kMinLimit, kMaxLimit));
    return query;
}

/// 结果里展示的 Manager 地址：参数覆盖优先，否则用客户端默认地址。
std::string DisplayUrl(const MonitorQuery& query, const MonitorClient& client) {
    return query.base_url.empty() ? client.BaseUrl() : query.base_url;
}

json ManagerUrlProperty() {
    return json{{"type", "string"},
                {"pattern", "^https?://"},
                {"description",
                 "Manager 基地址，例如 http://127.0.0.1:9101；省略时使用 "
                 "mcp.monitor.base_url"}};
}

json TimeoutProperty() {
    return json{{"type", "integer"},
                {"minimum", 100},
                {"maximum", 60000},
                {"description", "本次查询超时（毫秒），默认取 mcp.monitor.timeout_ms"}};
}

json LimitProperty(const char* description) {
    return json{{"type", "integer"},
                {"minimum", 1},
                {"maximum", 256},
                {"description", description}};
}

/// 构造工具参数 schema；`limit_description` 为空表示该工具不接受 limit。
json MonitorSchema(const char* limit_description) {
    json properties = json::object();
    properties["manager_url"] = ManagerUrlProperty();
    properties["timeout_ms"] = TimeoutProperty();
    if (limit_description != nullptr) {
        properties["limit"] = LimitProperty(limit_description);
    }
    json schema = json::object();
    schema["type"] = "object";
    schema["properties"] = std::move(properties);
    schema["additionalProperties"] = false;
    return schema;
}

/// 把数组按 limit 截断；`total` 写原始条数。
json SliceArray(const json& payload, std::size_t limit, std::size_t& total,
                bool& truncated) {
    total = payload.size();
    truncated = total > limit;
    if (!truncated) {
        return payload;
    }
    json sliced = json::array();
    for (std::size_t index = 0; index < limit; ++index) {
        sliced.push_back(payload[index]);
    }
    return sliced;
}

/// 失败文本：客户端已给出中文诊断，空消息时兜底。
ToolResult Failure(const MonitorError& error) {
    return ToolResult::Text(
        error.message.empty() ? "监控查询失败：MonitorClient 未返回诊断"
                              : error.message,
        true);
}

ToolResult HandleLatest(const ToolContext& context, MonitorClient& client) {
    const MonitorQuery query = PrepareQuery(context, client);
    auto result = client.Latest(query);
    if (!result.ok) {
        return Failure(result.error);
    }
    json payload = std::move(result.value);
    if (!payload.is_array()) {
        return ToolResult::Text(
            "Manager /api/query/latest 响应格式异常：期望 JSON 数组", true);
    }

    // Manager 已按 ?n= 截断，客户端拿不到总量；这里不再声称 truncated，
    // 只如实报告本次返回条数。
    json out = json::object();
    out["manager"] = DisplayUrl(query, client);
    out["endpoint"] = "/api/query/latest";
    out["requested"] = query.limit;
    out["count"] = payload.size();
    if (payload.empty()) {
        out["hint"] =
            "Manager 尚未收到任何快照：确认 worker 的 Pusher 指向同一端口，或先向 "
            "POST /api/push 推送一条数据";
    }
    out["snapshots"] = std::move(payload);
    return ToolResult::Text(out.dump(2));
}

ToolResult HandleStats(const ToolContext& context, MonitorClient& client) {
    const MonitorQuery query = PrepareQuery(context, client);
    auto result = client.Stats(query);
    if (!result.ok) {
        return Failure(result.error);
    }
    json payload = std::move(result.value);
    if (!payload.is_array()) {
        return ToolResult::Text(
            "Manager /api/query/stats 响应格式异常：期望 JSON 数组", true);
    }

    std::size_t total = 0;
    bool truncated = false;
    json hosts = SliceArray(payload, static_cast<std::size_t>(query.limit),
                            total, truncated);
    json out = json::object();
    out["manager"] = DisplayUrl(query, client);
    out["endpoint"] = "/api/query/stats";
    out["requested"] = query.limit;
    out["count"] = hosts.size();
    out["total"] = total;
    out["truncated"] = truncated;
    if (hosts.empty()) {
        out["hint"] = "Manager 尚未收到任何主机的快照，stats 为空";
    }
    out["hosts"] = std::move(hosts);
    return ToolResult::Text(out.dump(2));
}

ToolResult HandleKernel(const ToolContext& context, MonitorClient& client) {
    const MonitorQuery query = PrepareQuery(context, client);
    auto result = client.Kernel(query);
    if (!result.ok) {
        return Failure(result.error);
    }
    json payload = std::move(result.value);
    if (!payload.is_array()) {
        return ToolResult::Text(
            "Manager /api/query/kernel 响应格式异常：期望 JSON 数组", true);
    }

    std::size_t total = 0;
    bool truncated = false;
    json hosts = SliceArray(payload, static_cast<std::size_t>(query.limit),
                            total, truncated);
    json out = json::object();
    out["manager"] = DisplayUrl(query, client);
    out["endpoint"] = "/api/query/kernel";
    out["requested"] = query.limit;
    out["count"] = hosts.size();
    out["total"] = total;
    out["truncated"] = truncated;
    if (hosts.empty()) {
        out["hint"] =
            "Manager 尚未收到快照；另外 syscall/sched/futex/tcp/fileio 需要 root + "
            "eBPF，非 root 时对应 active=false 并带 reason";
    }
    out["hosts"] = std::move(hosts);
    return ToolResult::Text(out.dump(2));
}

ToolResult HandleHealth(const ToolContext& context, MonitorClient& client) {
    const MonitorQuery query = PrepareQuery(context, client);
    auto result = client.Health(query);
    if (!result.ok) {
        return Failure(result.error);
    }
    json payload = std::move(result.value);
    if (!payload.is_object()) {
        return ToolResult::Text(
            "Manager /api/health 响应格式异常：期望 JSON 对象", true);
    }

    json out = json::object();
    out["manager"] = DisplayUrl(query, client);
    out["endpoint"] = "/api/health";
    out["health"] = std::move(payload);
    return ToolResult::Text(out.dump(2));
}

/// 未配置客户端时的统一错误工具（注册表仍保持四个工具可见）。
ToolResult MissingClient(const ToolContext&) {
    return ToolResult::Text(
        "MonitorClient 未配置：应用装配阶段应调用 RegisterMonitorTools("
        "registry, MakeHttpMonitorClient(options))",
        true);
}

}  // namespace

void RegisterMonitorTools(Registry& registry,
                          std::shared_ptr<MonitorClient> client) {
    ToolBuilder builder(registry);
    if (!client) {
        builder
            .AddTool("os_monitor_latest",
                     "查询主机监控的当前快照（MonitorClient 未配置）。",
                     MonitorSchema("返回最近多少条快照，默认 10；Manager 容量上限 256"),
                     MissingClient)
            .AddTool("os_monitor_stats",
                     "查询各主机最新监控汇总与健康评分（MonitorClient 未配置）。",
                     MonitorSchema("最多返回多少台主机，默认 10"), MissingClient)
            .AddTool("os_monitor_kernel",
                     "查询各主机的内核指标摘要（MonitorClient 未配置）。",
                     MonitorSchema("最多返回多少台主机，默认 10"), MissingClient)
            .AddTool("os_monitor_health",
                     "查询监控 Manager 自身健康（MonitorClient 未配置）。",
                     MonitorSchema(nullptr), MissingClient);
        return;
    }

    // 每个 handler 持有 shared_ptr，工具生命周期内客户端始终有效。
    builder
        .AddTool("os_monitor_latest",
                 "查询主机监控的当前快照：CPU 占比/负载、内存、磁盘、网络速率，"
                 "以及 syscall/sched/futex/TCP/文件 IO 的累计指标。"
                 "数据来自 go2cpp::os::monitor 的 Manager（GET /api/query/latest）。",
                 MonitorSchema("返回最近多少条快照，默认 10；Manager 容量上限 256"),
                 [client](const ToolContext& context) {
                     return HandleLatest(context, *client);
                 })
        .AddTool("os_monitor_stats",
                 "查询各主机最新监控汇总与健康评分（CPU 40% + 内存 30% + 磁盘 20% + "
                 "负载 10%，按网络错误率最多扣 10 分），按分数降序。"
                 "用于快速判断「哪台机器有问题」（GET /api/query/stats）。",
                 MonitorSchema("最多返回多少台主机，默认 10"),
                 [client](const ToolContext& context) {
                     return HandleStats(context, *client);
                 })
        .AddTool("os_monitor_kernel",
                 "查询各主机的内核指标摘要：syscall 延迟/热点、调度等待、futex 争用、"
                 "TCP 重传与 RTT、文件 IO 延迟（GET /api/query/kernel）。"
                 "这些指标需要 root + eBPF，不可用时对应 active=false 且带 reason。",
                 MonitorSchema("最多返回多少台主机，默认 10"),
                 [client](const ToolContext& context) {
                     return HandleKernel(context, *client);
                 })
        .AddTool("os_monitor_health",
                 "查询监控 Manager 自身健康：status、已接收/已拒绝快照数、当前缓存条数"
                 "（GET /api/health）。没有监控数据时先用它排查 Manager 是否存活。",
                 MonitorSchema(nullptr),
                 [client](const ToolContext& context) {
                     return HandleHealth(context, *client);
                 });
}

void RegisterMonitorTools(Registry& registry, MonitorOptions options) {
    RegisterMonitorTools(registry, MakeHttpMonitorClient(std::move(options)));
}

}  // namespace go2cpp::mcp::tools
