/**
 * @file socket_tools.cpp
 * @brief `socket_*` 工具实现：参数解析、审计注入与 JSON 结果。
 * @author go2cpp contributors
 * @date 2026-10-30
 * @copyright MIT License
 *
 * 失败统一转成 `isError=true` 的 JSON/文本，不抛异常：fd 已关闭、未登记、
 * 非本进程 socket 都是可预期的调用结果，MCP 服务端必须继续可用。
 */

#include "mcp/tools/socket_tools.hpp"

#include <algorithm>
#include <cstddef>
#include <limits>
#include <optional>
#include <string>
#include <utility>

#include "mcp/tools/socket_autoregister.hpp"
#include "mcp/tools/tool_builder.hpp"

namespace go2cpp::mcp::tools {

namespace {

constexpr long long kDefaultEventCount = 20;
constexpr long long kMaxEventCount = 256;
constexpr std::size_t kMaxReasonBytes = 200;

/// 取整数参数；缺失时返回 nullopt 且不写 error，类型/范围错误写 error。
std::optional<long long> OptionalIntegerArgument(const json& arguments,
                                                const char* key,
                                                std::string* error) {
    const auto found = arguments.find(key);
    if (found == arguments.end()) {
        return std::nullopt;
    }
    if (found->is_number_unsigned()) {
        const auto value = found->get<unsigned long long>();
        if (value > static_cast<unsigned long long>(
                        std::numeric_limits<long long>::max())) {
            if (error != nullptr) {
                *error = std::string("参数 ") + key + " 超出整数范围";
            }
            return std::nullopt;
        }
        return static_cast<long long>(value);
    }
    if (!found->is_number_integer()) {
        if (error != nullptr) {
            *error = std::string("参数 ") + key + " 必须是整数";
        }
        return std::nullopt;
    }
    return found->get<long long>();
}

/// 读必填 fd；非法时写 error。
bool ReadFdArgument(const ToolContext& context, int* fd, std::string* error) {
    const auto found = context.arguments.find("fd");
    if (found == context.arguments.end()) {
        if (error != nullptr) {
            *error = "缺少必填参数 fd";
        }
        return false;
    }
    std::string type_error;
    const auto value = OptionalIntegerArgument(context.arguments, "fd", &type_error);
    if (!value.has_value()) {
        if (error != nullptr) {
            *error = type_error.empty() ? "参数 fd 必须是整数" : type_error;
        }
        return false;
    }
    if (*value < 0 ||
        *value > static_cast<long long>(std::numeric_limits<int>::max())) {
        if (error != nullptr) {
            *error = "参数 fd 超出范围（要求 0 <= fd <= " +
                     std::to_string(std::numeric_limits<int>::max()) + "）";
        }
        return false;
    }
    *fd = static_cast<int>(*value);
    return true;
}

/// 按 UTF-8 边界截断（避免把中文切成半个字符导致 JSON 序列化失败）。
std::string TruncateUtf8(const std::string& text, std::size_t max_bytes) {
    if (text.size() <= max_bytes) {
        return text;
    }
    std::size_t cut = max_bytes;
    while (cut > 0 &&
           (static_cast<unsigned char>(text[cut]) & 0xC0U) == 0x80U) {
        --cut;
    }
    return text.substr(0, cut);
}

json BaseSchema() {
    json schema = json::object();
    schema["type"] = "object";
    schema["properties"] = json::object();
    schema["additionalProperties"] = false;
    return schema;
}

json FdProperty() {
    return json{{"type", "integer"},
                {"minimum", 0},
                {"description", "目标 fd（必须是当前进程的 socket）"}};
}

json ListSchema() {
    json schema = BaseSchema();
    schema["properties"]["include_unobserved"] = json{
        {"type", "boolean"},
        {"default", true},
        {"description",
         "是否也列出未登记的 socket（只读、不可关闭）；默认 true"}};
    return schema;
}

json StatsSchema() {
    json schema = BaseSchema();
    schema["properties"]["fd"] = FdProperty();
    schema["required"] = json::array({"fd"});
    return schema;
}

json EventsSchema() {
    json schema = BaseSchema();
    schema["properties"]["fd"] = FdProperty();
    schema["properties"]["n"] = json{
        {"type", "integer"},
        {"minimum", 1},
        {"maximum", 256},
        {"default", 20},
        {"description", "最近 n 条事件，默认 20，上限 256"}};
    schema["required"] = json::array({"fd"});
    return schema;
}

json CloseSchema() {
    json schema = BaseSchema();
    schema["properties"]["fd"] = FdProperty();
    schema["properties"]["reason"] = json{
        {"type", "string"},
        {"maxLength", 200},
        {"description", "写入审计日志的关闭原因（可选）"}};
    schema["required"] = json::array({"fd"});
    return schema;
}

ToolResult HandleList(const ToolContext& context, SocketRegistry& registry) {
    bool include_unobserved = true;
    const auto found = context.arguments.find("include_unobserved");
    if (found != context.arguments.end()) {
        if (!found->is_boolean()) {
            return ToolResult::Text("socket_list 参数 include_unobserved 必须是布尔值",
                                    true);
        }
        include_unobserved = found->get<bool>();
    }

    std::string warning;
    const std::vector<SocketSnapshot> sockets =
        registry.List(include_unobserved, &warning);
    json out = json::object();
    out["count"] = sockets.size();
    out["sockets"] = json::array();
    for (const auto& socket : sockets) {
        out["sockets"].push_back(socket.ToJson());
    }
    if (!warning.empty()) {
        out["warning"] = warning;
    }
    out["hint"] =
        "registered=true 的 socket 才能被 socket_close 关闭；未登记的只读展示";
    return ToolResult::Text(out.dump(2));
}

ToolResult HandleStats(const ToolContext& context, SocketRegistry& registry) {
    int fd = -1;
    std::string argument_error;
    if (!ReadFdArgument(context, &fd, &argument_error)) {
        return ToolResult::Text("socket_stats 参数错误: " + argument_error, true);
    }
    std::string error;
    const auto stats = registry.Stats(fd, &error);
    if (!stats.has_value()) {
        return ToolResult::Text("socket_stats(" + std::to_string(fd) +
                                    ") 失败: " + error,
                                true);
    }
    return ToolResult::Text(stats->ToJson().dump(2));
}

ToolResult HandleEvents(const ToolContext& context, SocketRegistry& registry) {
    int fd = -1;
    std::string argument_error;
    if (!ReadFdArgument(context, &fd, &argument_error)) {
        return ToolResult::Text("socket_events 参数错误: " + argument_error, true);
    }

    long long count = kDefaultEventCount;
    const auto found = context.arguments.find("n");
    if (found != context.arguments.end()) {
        std::string type_error;
        const auto value = OptionalIntegerArgument(context.arguments, "n", &type_error);
        if (!value.has_value()) {
            return ToolResult::Text(
                "socket_events 参数错误: " +
                    (type_error.empty() ? std::string("n 必须是整数") : type_error),
                true);
        }
        count = std::clamp<long long>(*value, 1, kMaxEventCount);
    }

    bool registered = false;
    bool found_record = false;
    std::string error;
    const std::vector<SocketEvent> events = registry.Events(
        fd, static_cast<std::size_t>(count), &registered, &found_record, &error);
    if (!error.empty()) {
        return ToolResult::Text("socket_events(" + std::to_string(fd) +
                                    ") 失败: " + error,
                                true);
    }

    json out = json::object();
    out["fd"] = fd;
    out["found"] = found_record;
    out["registered"] = registered;
    out["requested"] = count;
    out["count"] = events.size();
    out["events"] = json::array();
    for (const auto& event : events) {
        out["events"].push_back(event.ToJson());
    }
    if (!found_record) {
        out["hint"] =
            "该 fd 是本进程 socket 但未登记，没有事件记录；go2cpp::net::Socket "
            "由 MCP 自动登记，裸 fd 需业务方调用 SocketRegistry::Register() 才"
            "会进入可观测白名单";
    } else if (events.empty()) {
        out["hint"] = "该 socket 尚无事件";
    }
    return ToolResult::Text(out.dump(2));
}

ToolResult HandleClose(const ToolContext& context, SocketRegistry& registry,
                       SocketAuditSink& audit) {
    int fd = -1;
    std::string argument_error;
    if (!ReadFdArgument(context, &fd, &argument_error)) {
        return ToolResult::Text("socket_close 参数错误: " + argument_error, true);
    }

    std::string reason;
    const auto found = context.arguments.find("reason");
    if (found != context.arguments.end()) {
        if (!found->is_string()) {
            return ToolResult::Text("socket_close 参数 reason 必须是字符串", true);
        }
        reason = TruncateUtf8(found->get<std::string>(), kMaxReasonBytes);
    }

    const SocketCloseOutcome outcome = registry.Close(fd, reason, audit);
    if (outcome.ok) {
        // 工具直接 ::close(fd)、绕过 Socket 对象，不会触发观察者 closed
        // 事件；同步自动登记集合，避免 fd 号复用后被误判。
        ForgetAutoRegisteredSocket(fd);
    }
    json audit_info = json::object();
    audit_info["id"] = outcome.audit_id;
    audit_info["sink"] = outcome.audit_sink;
    audit_info["result"] = outcome.ok ? "ok" : "denied";

    json out = json::object();
    out["fd"] = fd;
    out["closed"] = outcome.ok;
    out["audit"] = std::move(audit_info);
    if (!outcome.ok) {
        out["error"] = outcome.error;
        return ToolResult::Text(out.dump(2), true);
    }
    return ToolResult::Text(out.dump(2));
}

}  // namespace

void RegisterSocketTools(Registry& registry,
                         std::shared_ptr<SocketAuditSink> audit) {
    // 安装 network 侧生命周期钩子：业务用 go2cpp::net::Socket 创建的 socket
    // （含 accept 连接）自动进入注册表，socket_close 不再只对显式登记 fd 生效。
    // 幂等；未启用 MCP 的进程永远不会走到这里，基座热路径只剩一次原子读。
    InstallSocketAutoRegister();
    if (!audit) {
        audit = std::make_shared<FileSocketAuditSink>(DefaultSocketAuditPath());
    }
    SocketRegistry* socket_registry = &SocketRegistry::Instance();
    ToolBuilder builder(registry);
    builder
        .AddTool("socket_list",
                 "列出当前进程的全部 socket：fd、协议（tcp/udp/unix）、本地/远端"
                 "地址、状态（listening/connected/unconnected）。数据来自 "
                 "/proc/self/fd + getsockname/getpeername；未登记的 socket 只读展示，"
                 "registered=false 且 closable=false。",
                 ListSchema(),
                 [socket_registry](const ToolContext& context) {
                     return HandleList(context, *socket_registry);
                 })
        .AddTool("socket_stats",
                 "查询单个 socket 的收发字节、错误数与超时数。TCP 额外返回内核 "
                 "TCP_INFO：已确认发送字节、接收字节、重传次数、RTT 与内核状态；"
                 "UDP/Unix 没有这些字段（tcp.available=false），只报告应用层埋点。",
                 StatsSchema(),
                 [socket_registry](const ToolContext& context) {
                     return HandleStats(context, *socket_registry);
                 })
        .AddTool("socket_events",
                 "查询已登记 socket 的最近事件（登记/连接/读写/错误/超时/关闭），"
                 "按时间升序返回；TCP 流量还会按内核字节增量自动追加 read/write "
                 "事件（detail=tcp_info）。每个 fd 环形保留 256 条，满后丢弃最旧并"
                 "在 stats 里累加 dropped_events。",
                 EventsSchema(),
                 [socket_registry](const ToolContext& context) {
                     return HandleEvents(context, *socket_registry);
                 })
        .AddTool("socket_close",
                 "关闭本进程**已登记**的 socket，并在关闭前写审计日志（JSON Lines）。"
                 "未登记、非本进程、fd 已复用或审计写入失败一律拒绝（fail-closed）；"
                 "MCP 服务自身的 fd 不在白名单内。",
                 CloseSchema(),
                 [socket_registry, audit](const ToolContext& context) {
                     return HandleClose(context, *socket_registry, *audit);
                 });
}

}  // namespace go2cpp::mcp::tools
