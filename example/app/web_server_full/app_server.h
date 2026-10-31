#pragma once

/**
 * @file app_server.h
 * @brief 组装层：配置 -> 路由/中间件/静态文件/SSE -> HttpServer 生命周期。
 * @details Application 拥有所有子模块，Start/Stop 顺序在这里固定下来：
 *          SSE 先停（停定时 Fiber）、再停 HttpServer（关连接/join worker）、
 *          最后 shutdown Scheduler。反过来会留下在途 Fiber 访问已销毁对象。
 * @author go2cpp contributors
 * @date 2026-10-31
 * @copyright MIT License
 */

#include <cstdint>
#include <memory>
#include <string>

#include "config.h"
#include "go2cpp/bento/net/http_server.hpp"
#include "go2cpp/concurrency/scheduler.hpp"
#include "middleware.h"
#include "router.h"
#include "static_files.h"

namespace app {

class Api;
class SseHub;

/// @brief web_server_full 的应用对象；一个进程一个。
class Application final {
   public:
    explicit Application(Config config);
    ~Application();

    Application(const Application&) = delete;
    Application& operator=(const Application&) = delete;

    /// @brief 初始化日志、注册路由、启动 SSE Fiber 与 HttpServer。
    bool Start(std::string* error);
    /// @brief 逆序关闭；幂等。
    void Stop();

    const Config& config() const { return m_config; }
    std::uint16_t port() const;
    std::string local_address() const;

   private:
    /// @brief 每个请求的总入口：路由 -> 静态文件 -> 404，全部经过中间件链。
    go2cpp::http::Response Dispatch(const go2cpp::http::Request& request);

    Config m_config;                              ///< 启动快照，只读。
    go2cpp::bento::HttpServer m_server;           ///< HTTP/SSE 服务端。
    go2cpp::Scheduler m_scheduler{2};             ///< 承载 SSE 定时 Fiber 的调度器。
    Router m_router;                              ///< 方法 + 路径路由表。
    MiddlewareChain m_chain;                      ///< 请求中间件链。
    StaticOptions m_static;                       ///< 静态文件配置。
    std::unique_ptr<Api> m_api;                   ///< 业务 API。
    std::unique_ptr<SseHub> m_sse;                ///< SSE 广播中心。
    bool m_started{false};                        ///< Start/Stop 状态。
};

}  // namespace app
