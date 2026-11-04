#pragma once

/**
 * @file api.h
 * @brief JSON API 与健康检查：把业务 handler 注册到路由表。
 * @details 计数器用原子量，因为 handler 在多个 worker 线程并发执行；
 *          SSE 广播通过 SseHub 完成，Api 只持非拥有指针。
 * @author go2cpp contributors
 * @date 2026-10-31
 * @copyright MIT License
 */

#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>

#include "router.h"

namespace app {

class SseHub;

/// @brief 示例业务 API；生命周期与 Application 相同。
class Api final {
   public:
    Api();

    /// @brief 注册全部 /api、/health 路由；同时保存 router 指针供 /api/routes。
    void Register(Router& router);

    /// @brief 记录一次请求（每个 HTTP 请求调用一次，含 404）。
    void NoteRequest();

    /// @brief 关联 SSE 广播中心；必须在 Register 之后、Start 之前调用。
    void SetSseHub(SseHub* hub);

    std::uint64_t request_count() const {
        return m_requests.load(std::memory_order_acquire);
    }
    std::int64_t uptime_seconds() const;

   private:
    go2cpp::http::Response Echo(const go2cpp::http::Request& request);
    go2cpp::http::Response EchoPost(const go2cpp::http::Request& request);
    go2cpp::http::Response Item(const go2cpp::http::Request& request);
    go2cpp::http::Response Health(const go2cpp::http::Request& request);
    go2cpp::http::Response Stats(const go2cpp::http::Request& request);
    go2cpp::http::Response Time(const go2cpp::http::Request& request);
    go2cpp::http::Response Routes(const go2cpp::http::Request& request);
    go2cpp::http::Response Boom(const go2cpp::http::Request& request);
    go2cpp::http::Response Broadcast(const go2cpp::http::Request& request);

    std::atomic<std::uint64_t> m_requests{0};                       ///< 累计请求数。
    std::chrono::steady_clock::time_point m_started;                ///< 启动时刻。
    Router* m_router{nullptr};                                     ///< 非拥有；Register 时写入。
    SseHub* m_sse{nullptr};                                        ///< 非拥有；SetSseHub 时写入。
};

}  // namespace app
