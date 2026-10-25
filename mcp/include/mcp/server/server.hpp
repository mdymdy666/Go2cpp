/**
 * @file server.hpp
 * @brief MCP 服务端：把传输收到的 JSON-RPC 消息分发到注册表，并写回响应。
 *
 * @details
 * 参考项目的 `Server` 在构造函数里塞了传输、线程、通知队列和一堆命令处理，
 * 且命令表是运行时 `OverrideCallback` 覆盖的裸 `std::function`，行为依赖
 * `main.cpp` 里注册顺序。这里的分层是：
 *
 * ```
 * Serve()            // 循环：读消息 -> HandleMessage -> 写响应
 *   HandleMessage()  // 纯函数：解析 + 分发 + 编码，不碰 IO
 *     Dispatch()     // 按 method 路由到具体 Handle*
 * ```
 *
 * 好处：`HandleMessage` 不依赖传输，单元测试可以直接喂 JSON 断言输出，
 * 不需要起进程或伪造 socket。GMP 相关逻辑（并发派发、通知队列）放在
 * `Serve` 一层，替换调度策略时不必改动协议层。
 */

#pragma once

#include <atomic>
#include <cstddef>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>

#include "mcp/protocol/json_rpc.hpp"
#include "mcp/server/registry.hpp"
#include "mcp/transport/transport.hpp"

namespace go2cpp::mcp {

/**
 * @brief 分发过程中抛出的、带 JSON-RPC 错误码的异常。
 *
 * 用异常而不是返回错误对象，是为了让 `Dispatch` 里的正常路径保持直白的
 * `return result;`，错误路径集中在 `HandleMessage` 一处编码。
 */
class RpcException : public std::runtime_error {
public:
    RpcException(RpcError code, const std::string& message)
        : std::runtime_error(message), m_code(code) {}

    RpcError code() const noexcept { return m_code; }

private:
    RpcError m_code;
};

/// 服务端配置。
struct ServerOptions {
    std::string name = "go2cpp-mcp";                  ///< 服务名，initialize 响应里回填
    std::string version = "0.1.0";                    ///< 服务版本
    std::string protocol_version = "2025-06-18";      ///< 支持的 MCP 协议版本
    bool verbose = false;                             ///< 是否把协议帧打到 stderr
    /// 并发 worker 数：1 表示在调用线程内顺序处理；>1 时用 Go2Cpp Scheduler
    /// 为每个请求派生 Fiber（响应可能乱序，客户端按 id 匹配）。
    int workers = 1;
};

/**
 * @brief MCP 服务端。
 *
 * 线程模型：`Serve()` 在读循环线程处理请求；`workers > 1` 时把每条消息投进
 * Go2Cpp Fiber。插件可以在自己的线程调用 `SendNotification`，因此通知出口用
 * 互斥量串行写传输，传输指针本身是 atomic（`Serve` 退出时置空，避免插件线程
 * 在服务停止后拿到悬空指针）。
 */
class Server {
public:
    // Registry 由调用方持有：应用层可能同时把同一个注册表交给检索层，
    // 按值持有会在 move 后让检索层的引用指向空表。
    Server(ServerOptions options, Registry& registry);

    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;

    /**
     * @brief 阻塞运行读-处理-写循环，直到传输 EOF 或 `Stop()`。
     * @return 正常退出返回 0。
     */
    int Serve(ITransport& transport);

    /// 请求退出（线程安全，可从其他线程调用）。
    void Stop();

    /// 处理一条原始文本消息并写回响应（并发模式下由 Fiber 调用）。
    void ProcessMessage(const std::string& raw);

    /// 处理一条原始文本消息，返回需要写回的响应；通知或无需响应时返回 nullopt。
    /// 该函数不做 IO，供 `Serve()` 与测试共用。
    std::optional<std::string> HandleMessage(const std::string& raw);

    /// 主动向客户端推送**定向**通知（工具/进度：发给发起请求的会话）。
    /// 线程安全：工具（Fiber）与插件工作线程都可调用。
    /// 没有 sink 且 `Serve` 未运行/已退出时通知被**丢弃**（不缓存），
    /// 并累加 `notifications_dropped()`。
    void SendNotification(const std::string& method, json params);

    /// 主动向**所有活跃客户端**推送通知（插件主动通知，stdio + HTTP-SSE 全量）。
    /// 优先使用广播出口；传输层未注册广播出口时退回定向出口（stdio 二者等价）。
    void SendBroadcastNotification(const std::string& method, json params);

    /**
     * @brief 设置定向通知出口。
     * @details stdio 模式下通知直接写传输；HTTP/SSE 模式下传输是「请求-响应」，
     *          通知要进发起请求的会话队列，由 HTTP 层通过 sink 接管。
     */
    void SetNotificationSink(std::function<void(const std::string&)> sink);

    /**
     * @brief 设置广播通知出口（HTTP/SSE 层投递到所有活跃会话）。
     * @details 不设置时 `SendBroadcastNotification` 退回定向出口。stdio 传输
     *          不需要设置：写传输本身就是发给唯一的客户端。
     */
    void SetBroadcastSink(std::function<void(const std::string&)> sink);

    /// 因没有可用出口而丢弃的通知数（服务未运行、传输已关闭、写失败）。
    std::size_t notifications_dropped() const noexcept {
        return m_notifications_dropped.load(std::memory_order_relaxed);
    }

    Registry& registry() { return m_registry; }
    const ServerOptions& options() const { return m_options; }

private:
    /// 分发一条已解析的请求，返回 result 或抛出带错误码的异常。
    json Dispatch(const Request& request);

    json HandleInitialize(const json& params);
    json HandleToolsCall(const json& params);
    json HandlePromptsGet(const json& params);
    json HandleResourcesRead(const json& params);
    json HandleLoggingSetLevel(const json& params);
    json HandleCompletionComplete(const json& params);
    json HandleRootsList(const json& params);

    /// 处理客户端发来的通知（无响应）。
    void HandleClientNotification(const Request& request);

    /// 定向投递：优先定向 sink，其次写传输，都没有则计数丢弃。
    void EmitDirectedNotification(const std::string& payload);

    ServerOptions m_options;
    Registry& m_registry;  ///< 不持有所有权：生命周期由应用层保证
    std::atomic<bool> m_stopping{false};
    /// 传输指针只在 `Serve` 期间非空；插件线程可能并发读，所以用 atomic。
    std::atomic<ITransport*> m_transport{nullptr};
    std::mutex m_write_mutex;  ///< 串行化对传输的写
    std::mutex m_sink_mutex;
    std::function<void(const std::string&)> m_notification_sink;
    std::function<void(const std::string&)> m_broadcast_sink;
    std::atomic<std::size_t> m_notifications_dropped{0};
};

}  // namespace go2cpp::mcp
