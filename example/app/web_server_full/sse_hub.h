#pragma once

/**
 * @file sse_hub.h
 * @brief SSE 广播中心：后台 Fiber 定时推送，SSE handler 按订阅者消费 Channel。
 * @details HttpServer 的 SSE handler 在 worker 线程里同步执行，而事件源由
 *          Scheduler 的 Fiber 产生。两者通过「每个订阅者一个有界 Channel」
 *          交接：Fiber 只做非阻塞 TrySend，慢客户端丢事件而不是拖慢别人；
 *          handler 侧 RecvFor 超时就写心跳注释。
 * @author go2cpp contributors
 * @date 2026-10-31
 * @copyright MIT License
 */

#include <atomic>
#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>

#include "go2cpp/bento/net/http_server.hpp"
#include "go2cpp/channel.hpp"
#include "go2cpp/concurrency/scheduler.hpp"

namespace app {

/// @brief 一条广播事件（事件名 + data 字段）。
struct SseEvent {
    std::string event;  ///< SSE event 字段。
    std::string data;   ///< SSE data 字段。
};

/// @brief 基于 HttpServer::AsSse + Scheduler + Channel 的广播中心。
class SseHub final {
   public:
    SseHub(go2cpp::bento::HttpServer& server, go2cpp::Scheduler& scheduler,
           std::chrono::milliseconds interval);
    ~SseHub();

    SseHub(const SseHub&) = delete;
    SseHub& operator=(const SseHub&) = delete;

    /// @brief 注册 /events 并启动定时 Fiber；需在 scheduler.start() 之后调用。
    void Start();
    /// @brief 停止定时 Fiber、关闭所有订阅者队列；幂等。
    void Stop();

    /// @brief 广播一条事件；没有订阅者时静默返回。
    void Publish(const std::string& event, const std::string& data);

    std::size_t subscriber_count() const;

   private:
    using Queue = go2cpp::ChannelPtr<SseEvent>;

    /// @brief AsSse 回调：订阅、推欢迎事件、循环消费直到停止/写失败。
    void Serve(const go2cpp::http::Request& request, go2cpp::sse::Stream& stream);
    /// @brief 定时 Fiber：周期 Publish tick，Stop 时由 m_wake 提前唤醒。
    void TickLoop();

    go2cpp::bento::HttpServer& m_server;     ///< 非拥有；取 IsStopping 与注册 SSE 路由。
    go2cpp::Scheduler& m_scheduler;          ///< 非拥有；承载 TickLoop Fiber。
    std::chrono::milliseconds m_interval;    ///< tick 间隔。
    go2cpp::ChannelPtr<int> m_wake;          ///< Stop 唤醒 TickLoop 的通道。
    std::shared_ptr<go2cpp::Task> m_ticker;  ///< 定时 Fiber 句柄。

    mutable std::mutex m_mutex;                        ///< 保护 m_subscribers。
    std::map<std::uint64_t, Queue> m_subscribers;      ///< 会话 id -> 有界事件队列。
    std::atomic<std::uint64_t> m_next_id{0};           ///< 单调订阅 id。
    std::atomic<bool> m_running{false};                ///< Start/Stop 状态。
};

}  // namespace app
