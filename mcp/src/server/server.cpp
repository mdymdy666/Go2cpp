/**
 * @file server.cpp
 * @brief MCP 服务端实现：读循环、分发、响应编码。
 */

#include "mcp/server/server.hpp"

#include <cstdio>
#include <exception>
#include <memory>
#include <utility>
#include <vector>

#include "go2cpp/scheduler/scheduler.hpp"
#include "go2cpp/tools/go.hpp"
#include "mcp/validation/validators.hpp"

namespace go2cpp::mcp {

namespace {

/// 从 params 取对象字段；params 不是对象时返回空。
json ObjectField(const json& params, const char* key) {
    if (!params.is_object()) {
        return nullptr;
    }
    const auto it = params.find(key);
    return it == params.end() ? json(nullptr) : *it;
}

}  // namespace

Server::Server(ServerOptions options, Registry& registry)
    : m_options(std::move(options)), m_registry(registry) {}

int Server::Serve(ITransport& transport) {
    m_transport.store(&transport, std::memory_order_release);
    m_stopping.store(false);

    if (m_options.workers <= 1) {
        while (!m_stopping.load()) {
            auto message = transport.ReadMessage();
            if (!message) {
                break;
            }
            ProcessMessage(*message);
        }
        m_transport.store(nullptr, std::memory_order_release);
        return 0;
    }

    // 并发模式：读循环留在调用线程，每个请求作为一个 Fiber 提交到 Go2Cpp
    // 调度器。GMP 调试的目标场景就在这里（P/M 扩展、park 与唤醒）。
    go2cpp::Scheduler scheduler(static_cast<std::size_t>(m_options.workers));
    scheduler.start();

    std::vector<std::shared_ptr<go2cpp::Task>> tasks;
    constexpr std::size_t kMaxInflight = 256;

    while (!m_stopping.load()) {
        auto message = transport.ReadMessage();
        if (!message) {
            break;
        }
        std::string raw = std::move(*message);
        tasks.push_back(go2cpp::go(scheduler, [this, raw = std::move(raw)]() mutable {
            ProcessMessage(raw);
        }));

        // 有界积压：任务过多时等待最老的一批完成再继续读，避免内存无界增长。
        if (tasks.size() >= kMaxInflight) {
            for (auto& task : tasks) {
                task->wait();
            }
            tasks.clear();
        }
    }

    for (auto& task : tasks) {
        task->wait();
    }
    scheduler.shutdown();

    m_transport.store(nullptr, std::memory_order_release);
    return 0;
}

void Server::ProcessMessage(const std::string& raw) {
    if (m_options.verbose) {
        std::fprintf(stderr, "[mcp] <- %s\n", raw.c_str());
    }
    auto response = HandleMessage(raw);
    if (!response) {
        return;
    }
    if (m_options.verbose) {
        std::fprintf(stderr, "[mcp] -> %s\n", response->c_str());
    }
    std::lock_guard<std::mutex> lock(m_write_mutex);
    ITransport* transport = m_transport.load(std::memory_order_acquire);
    if (transport != nullptr) {
        transport->WriteMessage(*response);
    }
}

void Server::Stop() {
    m_stopping.store(true);
    ITransport* transport = m_transport.load(std::memory_order_acquire);
    if (transport != nullptr) {
        transport->Stop();
    }
}

std::optional<std::string> Server::HandleMessage(const std::string& raw) {
    json document;
    try {
        document = json::parse(raw);
    } catch (const json::parse_error& error) {
        return MakeError(nullptr, RpcError::kParseError, std::string("parse error: ") + error.what()).dump();
    }

    const ParseOutcome outcome = ParseRequest(document);
    if (!outcome.ok) {
        return outcome.error.dump();
    }

    const Request& request = outcome.request;
    if (!request.ExpectsResponse()) {
        HandleClientNotification(request);
        return std::nullopt;
    }

    json result;
    try {
        result = Dispatch(request);
    } catch (const RpcException& error) {
        return MakeError(request.id, error.code(), error.what()).dump();
    } catch (const std::exception& error) {
        return MakeError(request.id, RpcError::kInternalError, error.what()).dump();
    }
    return MakeResult(request.id, std::move(result)).dump();
}

void Server::SetNotificationSink(std::function<void(const std::string&)> sink) {
    std::lock_guard<std::mutex> lock(m_sink_mutex);
    m_notification_sink = std::move(sink);
}

void Server::SetBroadcastSink(std::function<void(const std::string&)> sink) {
    std::lock_guard<std::mutex> lock(m_sink_mutex);
    m_broadcast_sink = std::move(sink);
}

void Server::SendNotification(const std::string& method, json params) {
    const std::string payload = MakeNotification(method, std::move(params)).dump();
    EmitDirectedNotification(payload);
}

void Server::SendBroadcastNotification(const std::string& method, json params) {
    const std::string payload = MakeNotification(method, std::move(params)).dump();

    // 先取 sink 快照再调用：HTTP 的 sink 会进会话队列，stdio 没有 sink。
    // 持锁调用会让插件线程在 IO 上阻塞其他通知，也容易埋下锁序问题。
    std::function<void(const std::string&)> sink;
    {
        std::lock_guard<std::mutex> lock(m_sink_mutex);
        sink = m_broadcast_sink;
    }
    if (sink) {
        sink(payload);
        return;
    }
    // 传输层没有广播出口（stdio，或 HTTP 尚未注册广播 sink）：退回定向出口。
    // HTTP 的会话路由完全由传输层决定，插件通知不绕过它。
    EmitDirectedNotification(payload);
}

void Server::EmitDirectedNotification(const std::string& payload) {
    std::function<void(const std::string&)> sink;
    {
        std::lock_guard<std::mutex> lock(m_sink_mutex);
        sink = m_notification_sink;
    }
    if (sink) {
        sink(payload);
        return;
    }

    ITransport* transport = m_transport.load(std::memory_order_acquire);
    if (transport == nullptr) {
        // 没有客户端连接（服务未启动或已停止）：丢弃而不是缓存。
        // 缓存会在服务重启后向新客户端推送过期通知，语义上更糟。
        m_notifications_dropped.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    std::lock_guard<std::mutex> lock(m_write_mutex);
    if (!transport->WriteMessage(payload)) {
        m_notifications_dropped.fetch_add(1, std::memory_order_relaxed);
    }
}

json Server::Dispatch(const Request& request) {
    const std::string& method = request.method;

    if (method == "initialize") {
        return HandleInitialize(request.params);
    }
    if (method == "ping") {
        return json::object();
    }
    if (method == "tools/list") {
        return m_registry.ToolList();
    }
    if (method == "tools/call") {
        return HandleToolsCall(request.params);
    }
    if (method == "prompts/list") {
        return m_registry.PromptList();
    }
    if (method == "prompts/get") {
        return HandlePromptsGet(request.params);
    }
    if (method == "resources/list") {
        return m_registry.ResourceList();
    }
    if (method == "resources/read") {
        return HandleResourcesRead(request.params);
    }
    // MCP 2025-06-18 已把订阅语义收敛为 resources/updated 通知，这里保留
    // 空实现以兼容旧客户端的调用习惯。
    if (method == "resources/subscribe" || method == "resources/unsubscribe") {
        return json::object();
    }
    if (method == "logging/setLevel") {
        return HandleLoggingSetLevel(request.params);
    }
    if (method == "completion/complete") {
        return HandleCompletionComplete(request.params);
    }
    if (method == "roots/list") {
        return HandleRootsList(request.params);
    }

    throw RpcException(RpcError::kMethodNotFound, "method not found: " + method);
}

json Server::HandleInitialize(const json& params) {
    (void)params;

    json result = json::object();
    result["protocolVersion"] = m_options.protocol_version;

    json capabilities = json::object();
    capabilities["tools"] = json::object();
    capabilities["prompts"] = json::object();
    capabilities["resources"] = json::object();
    result["capabilities"] = std::move(capabilities);

    json server_info = json::object();
    server_info["name"] = m_options.name;
    server_info["version"] = m_options.version;
    result["serverInfo"] = std::move(server_info);
    return result;
}

json Server::HandleToolsCall(const json& params) {
    const json name = ObjectField(params, "name");
    if (name.is_null() || !name.is_string()) {
        throw RpcException(RpcError::kInvalidParams, "tools/call requires a string params.name");
    }

    json arguments = ObjectField(params, "arguments");
    if (arguments.is_null()) {
        arguments = json::object();
    }
    if (!arguments.is_object()) {
        throw RpcException(RpcError::kInvalidParams, "tools/call params.arguments must be an object");
    }

    json meta = ObjectField(params, "_meta");
    if (meta.is_null()) {
        meta = json::object();
    }

    ToolContext context;
    context.arguments = std::move(arguments);
    context.meta = std::move(meta);
    context.notify = [this](const std::string& method, json notification) {
        SendNotification(method, std::move(notification));
    };

    // 校验层：按工具声明的 inputSchema 先校验参数。参数不合法属于客户端错误
    // （-32602），不该等到工具内部取值崩溃才暴露。
    const std::optional<json> schema = m_registry.ToolSchema(name.get<std::string>());
    const validation::Result arguments_check =
        validation::ValidateToolArguments(schema ? *schema : json::object(), context.arguments);
    if (!arguments_check.ok) {
        throw RpcException(RpcError::kInvalidParams,
                           "invalid arguments: " + arguments_check.Summary());
    }

    std::optional<ToolResult> result;
    try {
        result = m_registry.InvokeTool(name.get<std::string>(), context);
    } catch (const std::exception& error) {
        // 工具自身的异常转成业务错误结果，而不是 -32603：客户端通常只把
        // isError 结果回喂给模型，协议级错误则可能直接终止会话。
        return ToolResult::Text(std::string("tool failed: ") + error.what(), true).ToJson();
    }

    if (!result) {
        throw RpcException(RpcError::kToolNotFound, "unknown tool: " + name.get<std::string>());
    }
    return result->ToJson();
}

json Server::HandlePromptsGet(const json& params) {
    const json name = ObjectField(params, "name");
    if (name.is_null() || !name.is_string()) {
        throw RpcException(RpcError::kInvalidParams, "prompts/get requires a string params.name");
    }

    json arguments = ObjectField(params, "arguments");
    if (arguments.is_null()) {
        arguments = json::object();
    }

    std::optional<json> result;
    try {
        result = m_registry.GetPrompt(name.get<std::string>(), arguments);
    } catch (const std::exception& error) {
        // 提示参数校验失败属于客户端错误，转 -32602。
        throw RpcException(RpcError::kInvalidParams, error.what());
    }
    if (!result) {
        throw RpcException(RpcError::kInvalidParams, "unknown prompt: " + name.get<std::string>());
    }
    return *result;
}

json Server::HandleResourcesRead(const json& params) {
    const json uri = ObjectField(params, "uri");
    if (uri.is_null() || !uri.is_string()) {
        throw RpcException(RpcError::kInvalidParams, "resources/read requires a string params.uri");
    }

    // 校验层：URI 非法在协议层就拒绝，避免资源实现各自做格式判断。
    const validation::Result uri_check = validation::ValidateResourceUri(uri.get<std::string>());
    if (!uri_check.ok) {
        throw RpcException(RpcError::kInvalidParams, "invalid resource uri: " + uri_check.Summary());
    }

    std::optional<json> result;
    try {
        result = m_registry.ReadResource(uri.get<std::string>());
    } catch (const std::exception& error) {
        throw RpcException(RpcError::kServerError, error.what());
    }
    if (!result) {
        // MCP 对未知资源使用 -32002（Resource not found）。
        throw RpcException(RpcError::kServerError, "unknown resource: " + uri.get<std::string>());
    }
    return *result;
}

json Server::HandleLoggingSetLevel(const json& params) {
    const json level = ObjectField(params, "level");
    if (level.is_null() || !level.is_string()) {
        throw RpcException(RpcError::kInvalidParams, "logging/setLevel requires params.level");
    }
    // 当前只记录级别，不接入日志系统；保留方法本身以兼容原项目能力。
    return json::object();
}

json Server::HandleCompletionComplete(const json& params) {
    (void)params;
    json completion = json::object();
    completion["values"] = json::array();
    completion["total"] = 0;
    completion["hasMore"] = false;
    json result = json::object();
    result["completion"] = std::move(completion);
    return result;
}

json Server::HandleRootsList(const json& params) {
    (void)params;
    json result = json::object();
    result["roots"] = json::array();
    return result;
}

void Server::HandleClientNotification(const Request& request) {
    // notifications/initialized：客户端确认握手完成，无需动作。
    // notifications/cancelled：当前是顺序处理，没有可取消的请求。
    if (request.method == "notifications/initialized" || request.method == "notifications/cancelled") {
        return;
    }
    if (m_options.verbose) {
        std::fprintf(stderr, "[mcp] ignore notification: %s\n", request.method.c_str());
    }
}

}  // namespace go2cpp::mcp
