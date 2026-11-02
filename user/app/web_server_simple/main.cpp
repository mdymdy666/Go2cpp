/**
 * @file main.cpp
 * @brief web_server 简单版：单文件启动一个可用的 HTTP 服务。
 * @details 组合了三个最小构件：
 *          ① bento::HttpServer —— 监听端口、分发请求、优雅 Stop；
 *          ② Scheduler + Channel —— 后台 Fiber 等停止信号，收信号后关闭服务；
 *          ③ sigwait —— 主线程阻塞等 SIGINT/SIGTERM，避免信号打断 worker。
 *
 *          运行：./go2cpp_web_server_simple [port]
 *          验证：curl http://127.0.0.1:8080/
 *                curl "http://127.0.0.1:8080/api/echo?name=go2cpp&times=2"
 *          关闭：Ctrl-C（SIGINT）或 kill -TERM <pid>
 * @author go2cpp contributors
 * @date 2026-10-31
 * @copyright MIT License
 */

#include "go2cpp/bento/net/http_server.hpp"
#include "go2cpp/channel.hpp"
#include "go2cpp/reflect/json.hpp"
#include "go2cpp/scheduler.hpp"

#include <pthread.h>

#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <utility>

namespace {

using go2cpp::MakeChannel;
using go2cpp::Scheduler;
using go2cpp::bento::HttpServer;
using go2cpp::bento::HttpServerConfig;
using go2cpp::http::Method;
using go2cpp::http::Request;
using go2cpp::http::Response;
using go2cpp::http::Status;

/// @brief 构造带 Content-Type 的响应；body 与类型都按值移动，避免复制。
Response MakeResponse(Status status, std::string body, std::string content_type) {
    Response response;
    response.status = status;
    response.header.set("Content-Type", std::move(content_type));
    response.body = std::move(body);
    return response;
}

/// @brief 统一的 JSON 响应入口（固定 application/json + utf-8）。
Response MakeJson(Status status, std::string body) {
    return MakeResponse(status, std::move(body), "application/json; charset=utf-8");
}

/// @brief GET /：一个可在浏览器里直接点按钮的极简页面。
Response IndexPage() {
    static const char kPage[] =
        "<!doctype html><html lang=\"zh-CN\"><head><meta charset=\"utf-8\">"
        "<title>Go2Cpp web_server_simple</title></head><body>"
        "<h1>Go2Cpp 简单版 HTTP 服务</h1>"
        "<p>GET /api/echo?name=go2cpp&amp;times=2 返回 JSON。</p>"
        "<button onclick=\"fetch('/api/echo?name=go2cpp&times=2')"
        ".then(r=>r.json()).then(j=>document.getElementById('out').textContent="
        "JSON.stringify(j,null,2))\">调用 /api/echo</button>"
        "<pre id=\"out\">（尚未调用）</pre></body></html>";
    return MakeResponse(Status::OK, kPage, "text/html; charset=utf-8");
}

/// @brief GET /api/echo：解析查询参数 name/times 并回显，同时给出服务器时间。
Response EchoApi(const Request& request) {
    go2cpp::http::QueryValues params;
    if (!go2cpp::http::decode_query(request.query(), params)) {
        return MakeJson(Status::BadRequest, R"({"ok":false,"error":"查询串格式非法"})");
    }

    std::string name = "world";
    if (const auto found = params.find("name"); found != params.end() && !found->second.empty()) {
        name = found->second.front();
    }
    if (name.size() > 64) {
        return MakeJson(Status::BadRequest, R"({"ok":false,"error":"name 不能超过 64 字节"})");
    }

    int times = 1;
    if (const auto found = params.find("times"); found != params.end() && !found->second.empty()) {
        const std::string& text = found->second.front();
        if (text.size() > 2 || text.find_first_not_of("0123456789") != std::string::npos) {
            return MakeJson(Status::BadRequest, R"({"ok":false,"error":"times 必须是整数"})");
        }
        times = std::atoi(text.c_str());
        if (times < 1 || times > 5) {
            return MakeJson(Status::BadRequest, R"({"ok":false,"error":"times 范围 1..5"})");
        }
    }

    const auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::system_clock::now().time_since_epoch())
                            .count();
    std::string json;
    go2cpp::reflect::JsonWriter writer(json);
    writer.BeginObject();
    writer.Key("ok");
    writer.Bool(true);
    writer.Key("name");
    writer.String(name);
    writer.Key("times");
    writer.Int(times);
    writer.Key("server_time_ms");
    writer.Uint(static_cast<std::uint64_t>(now_ms));
    writer.EndObject();
    if (!writer.ok()) {
        return MakeJson(Status::InternalError, R"({"ok":false,"error":"JSON 编码失败"})");
    }
    return MakeJson(Status::OK, std::move(json));
}

/// @brief 路由入口：方法 + 路径精确匹配；未命中返回 404 而不是空响应。
Response HandleRequest(const Request& request) {
    if (request.method != Method::Get) {
        return MakeResponse(Status::MethodNotAllowed, "只支持 GET\n", "text/plain; charset=utf-8");
    }
    const std::string path = request.path();
    if (path == "/") {
        return IndexPage();
    }
    if (path == "/health") {
        return MakeJson(Status::OK, R"({"status":"ok"})");
    }
    if (path == "/api/echo") {
        return EchoApi(request);
    }
    return MakeResponse(Status::NotFound, "404 not found: " + path + "\n",
                        "text/plain; charset=utf-8");
}

}  // namespace

int main(int argc, char** argv) {
    std::uint16_t port = 8080;
    if (argc > 1) {
        const int parsed = std::atoi(argv[1]);
        if (parsed <= 0 || parsed > 65535) {
            std::cerr << "端口非法: " << argv[1] << "（期望 1..65535）\n";
            return 2;
        }
        port = static_cast<std::uint16_t>(parsed);
    }

    // 先屏蔽信号再启动服务线程：所有线程继承屏蔽字，只有主线程的 sigwait
    // 能取到信号，服务的 accept/worker 线程不会被信号打断。
    sigset_t signals;
    sigemptyset(&signals);
    sigaddset(&signals, SIGINT);
    sigaddset(&signals, SIGTERM);
    if (pthread_sigmask(SIG_BLOCK, &signals, nullptr) != 0) {
        std::cerr << "pthread_sigmask 失败\n";
        return 1;
    }

    HttpServerConfig config;
    config.worker_threads = 4;
    HttpServer server;
    const auto listen_result = server.Listen("127.0.0.1", port, config);
    if (!listen_result.ok()) {
        std::cerr << "监听失败: " << listen_result.message << "（system_error="
                  << listen_result.system_error << "）\n";
        return 1;
    }
    server.OnRequest(HandleRequest);
    const auto start_result = server.Start();
    if (!start_result.ok()) {
        std::cerr << "启动失败: " << start_result.message << "\n";
        return 1;
    }
    std::cout << "[web_simple] 已启动 http://127.0.0.1:" << server.Port() << "\n";

    Scheduler scheduler(2);
    scheduler.start();
    auto stop_reason = MakeChannel<std::string>(1);
    auto closer = scheduler.spawn([&server, stop_reason] {
        const auto received = stop_reason->Recv();
        const std::string reason = received.Ok() ? received.ValueOrDefault() : "channel closed";
        std::cout << "[web_simple] 收到 " << reason << "，开始优雅关闭\n";
        server.Stop();
    });

    int signal_number = 0;
    if (sigwait(&signals, &signal_number) != 0) {
        std::cerr << "sigwait 失败\n";
    }
    const char* reason = (signal_number == SIGINT) ? "SIGINT" : "SIGTERM";
    std::cout << "[web_simple] 收到信号 " << reason << "\n";
    (void)stop_reason->Send(std::string(reason));
    const bool closed = closer->wait();
    scheduler.shutdown();
    std::cout << "[web_simple] 已退出（关闭任务完成=" << closed << "）\n";
    return closed ? 0 : 1;
}
