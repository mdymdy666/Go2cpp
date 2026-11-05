/**
 * @file go2cpp_debug_tools.cpp
 * @brief Go2Cpp 运行时探针 MCP 工具：HTTP GET DebugServer 的 6 个只读端点。
 *
 * ## 定位
 *
 * `monitor_tools.cpp` 面向**外部** `go2cpp::os::monitor::Manager`；本文件面向
 * **目标进程自己**的 `debug::DebugServer`（`GO2CPP_DEBUG_DUMP=ON` 时随
 * `Start()` 自动监听 `127.0.0.1:39101`）。因此工具描述必须明确区分，避免 AI
 * 把两者混为一谈。
 *
 * ## 设计
 *
 * - 每个工具 = 一个端点，参数只做超时/地址/条数覆盖；
 * - 响应体（服务端已格式化的 JSON）原样返回，不再二次包装，AI 可直接解析；
 * - 网络失败、非 2xx、超大响应都转成 `isError=true` 的中文诊断，不抛异常；
 * - 注册阶段零网络访问：DebugServer 未启动只是部署状态。
 *
 * ## 边界
 *
 * - 默认地址来自 `GO2CPP_DEBUG_HTTP_URL`（完整 URL）或
 *   `GO2CPP_DEBUG_HTTP_PORT`（端口，默认 39101）；
 * - 只支持 `http://`（运行时没有 TLS 客户端）；
 * - 单次响应上限 8 MiB，超限提示缩小 limit。
 */

#include "go2cpp_debug_tools.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <string>

#include "go2cpp/network/http.hpp"
#include "mcp/tools/tool_builder.hpp"

namespace go2cpp::mcp::tools {
namespace {

constexpr long long kDefaultTimeoutMs = 3000;
constexpr long long kMaxTimeoutMs = 10000;
constexpr long long kMinTimeoutMs = 100;
constexpr long long kDefaultLimit = 64;
constexpr long long kMaxLimit = 1024;
constexpr std::size_t kMaxBodyBytes = 8ULL * 1024ULL * 1024ULL;
constexpr std::size_t kMaxErrorBodyChars = 2000;
constexpr const char* kDefaultPort = "39101";

/// 默认探针地址：完整 URL 环境变量优先，否则按端口拼 127.0.0.1。
std::string DefaultBaseUrl() {
    const char* explicit_url = std::getenv("GO2CPP_DEBUG_HTTP_URL");
    if (explicit_url != nullptr && *explicit_url != '\0') {
        return explicit_url;
    }
    const char* port = std::getenv("GO2CPP_DEBUG_HTTP_PORT");
    const std::string chosen =
        (port != nullptr && *port != '\0') ? port : kDefaultPort;
    return "http://127.0.0.1:" + chosen;
}

std::string StringArgument(const json& arguments, const char* key) {
    const auto found = arguments.find(key);
    if (found == arguments.end() || !found->is_string()) {
        return {};
    }
    return found->get<std::string>();
}

long long IntArgument(const json& arguments, const char* key, long long fallback) {
    const auto found = arguments.find(key);
    if (found == arguments.end() || !found->is_number_integer()) {
        return fallback;
    }
    return found->get<long long>();
}

/// 只接受 `http://host[:port][/path]`，去掉尾部斜杠；失败写中文诊断。
bool NormalizeBaseUrl(const std::string& input, std::string& normalized,
                      std::string& error) {
    constexpr std::size_t kSchemeSize = 7;  // "http://"
    if (input.empty()) {
        error = "探针地址为空：请传 base_url，或设置 GO2CPP_DEBUG_HTTP_URL/"
                "GO2CPP_DEBUG_HTTP_PORT";
        return false;
    }
    if (input.compare(0, kSchemeSize, "http://") != 0) {
        error = "不支持的探针地址: " + input +
                "（仅支持 http://host:port；Go2Cpp 没有可用的 TLS 客户端）";
        return false;
    }
    std::string base = input;
    while (base.size() > kSchemeSize && base.back() == '/') {
        base.pop_back();
    }
    const std::size_t slash = base.find('/', kSchemeSize);
    const std::string authority =
        base.substr(kSchemeSize, slash == std::string::npos ? std::string::npos
                                                            : slash - kSchemeSize);
    if (authority.empty()) {
        error = "探针地址缺少主机名: " + input + "（应形如 http://127.0.0.1:39101）";
        return false;
    }
    const std::size_t colon = authority.rfind(':');
    if (colon != std::string::npos) {
        const std::string host = authority.substr(0, colon);
        const std::string port_text = authority.substr(colon + 1);
        if (host.empty() || port_text.empty()) {
            error = "探针地址格式错误: " + input + "（应形如 http://127.0.0.1:39101）";
            return false;
        }
        try {
            const long port = std::stol(port_text);
            if (port < 1 || port > 65535) {
                error = "探针端口超出范围: " + port_text + "（有效范围 1-65535）";
                return false;
            }
        } catch (const std::exception&) {
            error = "探针端口不是数字: " + port_text;
            return false;
        }
    }
    normalized = std::move(base);
    return true;
}

std::string Truncate(const std::string& text, std::size_t limit) {
    if (text.size() <= limit) {
        return text;
    }
    return text.substr(0, limit) + "...(已截断)";
}

json DebugSchema(bool with_limit, bool with_format) {
    json properties = json::object();
    properties["base_url"] = json{
        {"type", "string"},
        {"pattern", "^https?://"},
        {"description",
         "Go2Cpp DebugServer 基地址，默认取 GO2CPP_DEBUG_HTTP_URL，否则 "
         "http://127.0.0.1:<GO2CPP_DEBUG_HTTP_PORT 或 39101>"}};
    properties["timeout_ms"] = json{
        {"type", "integer"},
        {"minimum", kMinTimeoutMs},
        {"maximum", kMaxTimeoutMs},
        {"description", "本次探测超时（毫秒），默认 3000"}};
    if (with_limit) {
        properties["limit"] = json{
            {"type", "integer"},
            {"minimum", 1},
            {"maximum", kMaxLimit},
            {"description", "返回条数上限，默认 64"}};
    }
    if (with_format) {
        properties["format"] = json{
            {"type", "string"},
            {"enum", json::array({"json", "text"})},
            {"description", "json=结构化 dump（默认）；text=附带的 text 段"}};
    }
    json schema = json::object();
    schema["type"] = "object";
    schema["properties"] = std::move(properties);
    schema["additionalProperties"] = false;
    return schema;
}

/// @brief 向 DebugServer 发一次 GET，返回响应体或中文诊断。
ToolResult QueryDebug(const ToolContext& context, const char* path,
                      bool with_limit, bool with_format) {
    std::string base;
    std::string error;
    std::string requested = StringArgument(context.arguments, "base_url");
    if (requested.empty()) {
        requested = DefaultBaseUrl();
    }
    if (!NormalizeBaseUrl(requested, base, error)) {
        return ToolResult::Text(error, true);
    }

    const long long timeout_ms = std::clamp<long long>(
        IntArgument(context.arguments, "timeout_ms", kDefaultTimeoutMs),
        kMinTimeoutMs, kMaxTimeoutMs);
    std::string query;
    if (with_limit) {
        const long long limit = std::clamp<long long>(
            IntArgument(context.arguments, "limit", kDefaultLimit), 1, kMaxLimit);
        query = "?limit=" + std::to_string(limit);
    }
    if (with_format && StringArgument(context.arguments, "format") == "text") {
        query += query.empty() ? "?format=text" : "&format=text";
    }
    const std::string url = base + path + query;

    const auto result = go2cpp::http::Get(
        url, std::chrono::milliseconds(static_cast<int>(timeout_ms)));
    if (!result.ok() || !result.response) {
        std::string message = "无法访问 Go2Cpp DebugServer：" + result.message +
                              "（URL: " + url + "，超时 " +
                              std::to_string(timeout_ms) + "ms）";
        if (result.error == go2cpp::http::Result::Error::Connect ||
            result.error == go2cpp::http::Result::Error::Resolve) {
            message +=
                "；请确认目标进程以 GO2CPP_DEBUG_DUMP=ON 构建、调用了 "
                "DebugServer::Start()，且 GO2CPP_DEBUG_HTTP_PORT 与 base_url 一致";
        } else if (result.error == go2cpp::http::Result::Error::Timeout) {
            message += "；服务端繁忙或地址错误，可增大 timeout_ms 或改用 base_url";
        }
        return ToolResult::Text(std::move(message), true);
    }

    const int status = static_cast<int>(result.response->status);
    const std::string& body = result.response->body;
    if (status < 200 || status >= 300) {
        return ToolResult::Text("DebugServer 返回 HTTP " + std::to_string(status) +
                                    "（URL: " + url + "）：" +
                                    Truncate(body, kMaxErrorBodyChars),
                                true);
    }
    if (body.empty()) {
        return ToolResult::Text("DebugServer 返回空响应（URL: " + url + "）", true);
    }
    if (body.size() > kMaxBodyBytes) {
        return ToolResult::Text(
            "DebugServer 响应超过 8 MiB（" + std::to_string(body.size()) +
                " 字节）：请缩小 limit，或改用 /debug/events 精确查询",
            true);
    }
    return ToolResult::Text(body);
}

ToolResult HandleFibers(const ToolContext& context) {
    return QueryDebug(context, "/debug/fibers", false, false);
}

ToolResult HandleScheduler(const ToolContext& context) {
    return QueryDebug(context, "/debug/scheduler", false, false);
}

ToolResult HandleSync(const ToolContext& context) {
    return QueryDebug(context, "/debug/sync", false, false);
}

ToolResult HandleResources(const ToolContext& context) {
    return QueryDebug(context, "/debug/resources", false, false);
}

ToolResult HandleEvents(const ToolContext& context) {
    return QueryDebug(context, "/debug/events", true, false);
}

ToolResult HandleDump(const ToolContext& context) {
    return QueryDebug(context, "/debug/dump", true, true);
}

constexpr const char* kProbeNote =
    "这是 Go2Cpp 运行时探针（目标进程自己的 DebugServer HTTP 端点），"
    "不是外部 os_monitor Manager。";

}  // namespace

void RegisterGo2CppDebugTools(Registry& registry) {
    ToolBuilder builder(registry);
    builder
        .AddTool("go2cpp_debug_fibers",
                 std::string("查询 Go2Cpp 目标进程的 fiber 列表与调度器摘要"
                             "（GET /debug/fibers）：fiber_count、DebugRegistry "
                             "元数据、scheduler_summary、已注册组件。") +
                     kProbeNote,
                 DebugSchema(false, false), HandleFibers)
        .AddTool("go2cpp_debug_scheduler",
                 std::string("查询 Go2Cpp 调度器 P/M/worker/runqueue 与 metrics"
                             "（GET /debug/scheduler）。") +
                     kProbeNote,
                 DebugSchema(false, false), HandleScheduler)
        .AddTool("go2cpp_debug_sync",
                 std::string("查询 Go2Cpp 同步原语：mutex owner/waiters、条件变量、"
                             "parking_condition，用于判断锁等待与死锁候选"
                             "（GET /debug/sync）。") +
                     kProbeNote,
                 DebugSchema(false, false), HandleSync)
        .AddTool("go2cpp_debug_resources",
                 std::string("查询 Go2Cpp 进程资源：RSS/VSZ/线程数/fd 数、资源注册表"
                             "按类型计数、事件环状态（GET /debug/resources）。") +
                     kProbeNote,
                 DebugSchema(false, false), HandleResources)
        .AddTool("go2cpp_debug_events",
                 std::string("查询 Go2Cpp debug_event_ring 最近事件"
                             "（GET /debug/events?limit=N），用于还原 park/wake/"
                             "runqueue 时序。") +
                     kProbeNote,
                 DebugSchema(true, false), HandleEvents)
        .AddTool("go2cpp_debug_dump",
                 std::string("触发并读取一次完整聚合 dump（GET /debug/dump），"
                             "包含全部已注册组件；format=text 时附带原 text 段。") +
                     kProbeNote,
                 DebugSchema(true, true), HandleDump);
}

}  // namespace go2cpp::mcp::tools
