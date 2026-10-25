/**
 * @file http_transport.hpp
 * @brief HTTP 传输：Streamable HTTP（POST /mcp）与旧版 SSE（GET /sse + POST /message）。
 *
 * @details
 * 参考项目把 HTTP/SSE 做成 `ITransport` 实现，但 HTTP 本质是「一请求一响应」，
 * 塞进「阻塞读一条消息」的接口会逼出一堆状态机。这里换个划分：
 * - `HttpTransport` 自己监听、accept、解析 HTTP；
 * - 每条 POST 请求交给 `Server::HandleMessage` 处理；
 * - 通知走 `Server` 的定向/广播 sink：定向按**当前执行请求的 SSE 会话**投递，
 *   广播投递所有活跃会话（插件主动通知）。
 *
 * ## 多客户端模型
 *
 * 每个 `GET /sse` 连接是一个独立会话：
 * - 分配唯一 `sessionId`，建立 `Channel<std::string>` 作为推送队列；
 * - 先推 `event: endpoint`（`/message?sessionId=<id>`），再推 `event: message`；
 * - `POST /message?sessionId=<id>` 只处理并写回该会话，未知 id 返回 404；
 * - 工具执行期间的通知通过 `go2cpp::FiberLocal` 绑定当前请求的会话，
 *   只进该会话；无绑定的服务级通知（插件、`POST /mcp`）走
 *   `BroadcastNotification` 广播给所有活跃会话；
 * - SSE 循环周期性探测对端关闭并检查空闲超时，断开后从会话表移除。
 *
 * 并发：accept 循环在调用线程，每个连接一个 Fiber（Go2Cpp Scheduler），
 * 读取/解析都在该 Fiber 内串行；SSE 等待队列、HTTP 读等待都走 park，
 * 不占住 worker。
 */

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>

#include "go2cpp/concurrency/channel.hpp"
#include "go2cpp/concurrency/fiber_local.hpp"
#include "go2cpp/network/http.hpp"
#include "go2cpp/network/socket.hpp"
#include "mcp/server/server.hpp"

namespace go2cpp::mcp {

/**
 * @brief MCP 的 HTTP 前端。
 *
 * 路由：
 * - `POST /mcp`：请求体是 JSON-RPC，响应体是 JSON-RPC 响应（或 204 表示通知）；
 * - `GET /sse`：建立 SSE 流，先推 `endpoint` 事件，之后推该会话的通知/响应；
 * - `POST /message?sessionId=<id>`：旧版协议，响应 200，结果经对应 SSE 推送；
 *   `POST /messages?sessionId=<id>` 是兼容别名。
 */
class HttpTransport {
public:
    struct Options {
        std::string host = "127.0.0.1";  ///< 监听地址
        std::uint16_t port = 8765;       ///< 监听端口；0 表示由系统分配
        int workers = 4;                 ///< 连接级 Fiber 数
        /// SSE 心跳间隔；连接空闲时按此间隔写 `: ping`。
        int heartbeat_interval_ms = 15000;
        /// SSE 轮询间隔：用于探测对端关闭、检查空闲超时。
        int poll_interval_ms = 500;
        /// 会话空闲超时（毫秒）；<=0 表示不超时。只统计「有请求/有事件」的活动。
        int session_idle_timeout_ms = 300000;
        /// 每个会话的推送队列容量；响应做 5s 背压，通知在不阻塞的前提下投递。
        std::size_t session_queue_capacity = 1024;
    };

    HttpTransport(Server& server, Options options);

    ~HttpTransport();

    /**
     * @brief 创建监听 socket。
     * @param actual_port 输出实际端口（port=0 时有用）。
     */
    bool Start(std::uint16_t& actual_port, std::string& error);

    /// 阻塞 accept 循环，直到 Stop()。
    void Run();

    void Stop();

    /**
     * @brief 向所有活跃 SSE 会话广播一条已序列化的 JSON-RPC 通知。
     *
     * 插件主动通知（无归属会话）的出口；线程安全、非阻塞（队列满则对该会话丢弃）。
     * `POST /mcp` 与插件工作线程的通知没有 FiberLocal 绑定，也会自动走这里。
     * 定向通知走请求 Fiber 的绑定（`POST /message` 内的 `ToolContext::notify`），
     * 两者互不影响：广播全部收到，定向只进发起会话。
     */
    void BroadcastNotification(const std::string& payload);

private:
    /// 一个 SSE 会话：推送队列 + 结束标记 + 最近活动时间。
    struct Session {
        std::string id;
        std::shared_ptr<go2cpp::Channel<std::string>> queue;
        std::atomic<bool> closed{false};
        std::atomic<long long> last_active_ms{0};
    };

    void HandleConnection(go2cpp::Socket::Ptr connection);
    bool ServePost(go2cpp::Socket::Ptr connection, const go2cpp::http::Request& request);
    bool ServeSessionPost(go2cpp::Socket::Ptr connection,
                          const go2cpp::http::Request& request, const std::string& query);
    bool ServeSse(go2cpp::Socket::Ptr connection);

    /// 摘除定向/广播两个通知 sink，并把间接层 transport 置空。
    void DetachNotificationSinks();

    /// 按 id 查活跃会话；找不到或已关闭时返回空。
    std::shared_ptr<Session> FindSession(const std::string& id);
    /// 从会话表移除、标记关闭并关闭推送队列。
    void CloseSession(const std::shared_ptr<Session>& session);
    /**
     * @brief 把 payload 投递到指定会话。
     * @param wait true：响应使用有界阻塞（背压）；false：通知非阻塞，队列满则丢。
     */
    bool PushToSession(const std::string& id, const std::string& payload, bool wait);
    /// 通知出口：请求 Fiber 内定向到会话；无绑定（POST /mcp、插件线程）时广播。
    void RouteNotification(const std::string& payload);
    /// 生成进程内唯一 sessionId。
    std::string NewSessionId();

    Server& m_server;
    Options m_options;
    go2cpp::Socket::Ptr m_listener;
    std::atomic<bool> m_stopped{false};
    std::mutex m_sessions_mutex;
    std::map<std::string, std::shared_ptr<Session>> m_sessions;
    std::atomic<std::uint64_t> m_next_session_id{0};
    /// 当前请求 Fiber 绑定的 SSE 会话；通知 sink 读取它做定向投递。
    go2cpp::FiberLocal<std::string> m_bound_session;

    /// 通知 sink 的安全间接层：析构时把 transport 置空，插件线程不会回调悬空 this。
    struct NotificationTarget {
        std::mutex mutex;
        HttpTransport* transport = nullptr;
    };
    std::shared_ptr<NotificationTarget> m_notification_target;
};

}  // namespace go2cpp::mcp
