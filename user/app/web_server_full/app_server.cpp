/**
 * @file app_server.cpp
 * @brief Application 的启动/关闭编排与请求分发。
 * @author go2cpp contributors
 * @date 2026-10-31
 * @copyright MIT License
 */

#include "app_server.h"

#include <chrono>
#include <exception>
#include <iostream>
#include <map>
#include <string>
#include <utility>

#include "api.h"
#include "logger.h"
#include "sse_hub.h"

namespace app {
namespace {

using go2cpp::http::Request;
using go2cpp::http::Response;
using go2cpp::http::Status;

}  // namespace

Application::Application(Config config) : m_config(std::move(config)) {}

Application::~Application() { Stop(); }

bool Application::Start(std::string* error) {
    std::string log_error;
    if (!InitLogger(m_config.log_file, m_config.log_level, &log_error)) {
        // 日志文件不可写不阻止服务启动：stdout 仍然可用，只提示调用方。
        std::cerr << "[web_full] 日志文件初始化失败，仅输出到 stdout: " << log_error << "\n";
    }
    LogInfo("启动 web_server_full: " + m_config.Describe());

    m_api = std::make_unique<Api>();
    m_api->Register(m_router);
    m_chain.Use(MakeRecoverMiddleware());
    m_chain.Use(MakeRequestIdMiddleware());
    m_chain.Use(MakeAccessLogMiddleware());
    m_chain.Use(MakeCorsMiddleware());
    m_static.root = m_config.web_root;

    go2cpp::bento::HttpServerConfig server_config;
    server_config.worker_threads = m_config.worker_threads;
    server_config.max_body_bytes = 1024 * 1024;
    server_config.max_requests_per_connection = 200;
    server_config.sse_options.heartbeat_interval = std::chrono::seconds(5);

    const auto listened = m_server.Listen(m_config.host, m_config.port, server_config);
    if (!listened.ok()) {
        if (error != nullptr) {
            *error = listened.message + "（system_error=" + std::to_string(listened.system_error) +
                     "）";
        }
        ShutdownLogger();
        return false;
    }

    m_scheduler.start();
    m_sse = std::make_unique<SseHub>(m_server, m_scheduler,
                                     std::chrono::milliseconds(m_config.sse_interval_ms));
    m_api->SetSseHub(m_sse.get());
    m_server.OnRequest([this](const Request& request) { return Dispatch(request); });
    m_sse->Start();

    const auto started = m_server.Start();
    if (!started.ok()) {
        if (error != nullptr) {
            *error = started.message;
        }
        m_sse->Stop();
        m_server.Stop();
        m_scheduler.shutdown();
        ShutdownLogger();
        return false;
    }

    m_started = true;
    LogInfo("监听 http://" + m_server.LocalAddress() + "，worker=" +
            std::to_string(m_config.worker_threads));
    LogInfo("路由:\n" + m_router.Describe());
    return true;
}

void Application::Stop() {
    if (!m_started) {
        return;
    }
    LogInfo("开始优雅关闭");
    if (m_sse) {
        m_sse->Stop();
    }
    m_server.Stop();
    m_scheduler.shutdown();
    LogInfo("已关闭，累计请求 " + std::to_string(m_api ? m_api->request_count() : 0));
    ShutdownLogger();
    m_started = false;
}

std::uint16_t Application::port() const { return m_server.Port(); }

std::string Application::local_address() const { return m_server.LocalAddress(); }

Response Application::Dispatch(const Request& request) {
    m_api->NoteRequest();
    Handler handler;
    std::map<std::string, std::string> params;
    if (m_router.Match(request, &handler, &params)) {
        Request routed = request;
        routed.path_values = std::move(params);
        return m_chain.Run(routed, [&handler](const Request& current) { return handler(current); });
    }

    Response static_response;
    if (TryServeStatic(request, m_static, &static_response)) {
        return m_chain.Run(request, [&static_response](const Request&) { return static_response; });
    }

    return m_chain.Run(request, [](const Request& current) {
        return MakeJsonError(Status::NotFound, "未找到: " + current.path());
    });
}

}  // namespace app
