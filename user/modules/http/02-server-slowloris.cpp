// Copyright 2026 Go2Cpp Authors. All rights reserved.
/**
 * @file 02-server-slowloris.cpp
 * @brief 真实场景：服务端遇到慢客户端（Slowloris），连接被无限续期。
 *
 * ## 场景
 * 攻击者/劣质客户端建立大量连接，每个连接每几百毫秒只发 1 个字节，
 * 让「每次 recv 最多等 30s」的实现永远等不到超时；几百个半开连接就能
 * 占满 worker。真实网关（app/gateway.cpp）用**单请求总 deadline** 防它。
 *
 * ## 默认行为（file:line）
 * - `app::Config::request_timeout` 默认 **10000ms**（include/go2cpp/app/gateway.hpp:358），
 *   注释明确「从本次请求第一次 recv 前起算，覆盖头部+body；0 按 10000ms」；
 * - gateway 在 `ServeConnection` 里为每个请求算一次
 *   `deadline = now + config.request_timeout`（src/app/gateway.cpp:1301-1302），
 *   每次 recv 只等「剩余时间」与 30s 的较小值（gateway.cpp:1357-1367）；
 * - 到期**直接关闭连接**，不写响应、`request_timeouts++`（gateway.cpp:1355-1357），
 *   这就是 slowloris 的默认处理：不给你继续拖的机会；
 * - 对比：`http::Server::serve_connection()` 本身**不设** recv/write 超时
 *   （include/go2cpp/network/http.hpp:570-577），直接用它会暴露同样的风险。
 *
 * ## 可选策略
 * 1. **调大**：正常移动网络/大文件上传（请求头慢）需要更长预算，
 *    例如 30s；只对白名单路由放宽，全局仍保持 10s。
 * 2. **调小**：内网 RPC/健康检查 1-3s 足够，缩短半开连接占用时间。
 * 3. **限流**：并发连接数超过阈值时直接回 503 + Connection: close
 *    （gateway 用 `max_queued_connections` 做背压，gateway.hpp:352-353）；
 * 4. **直接 503**：维护窗口/过载时在 accept 后立刻回 503，不读请求体。
 *
 * ## 如何选择
 * - 公网入口 → 默认 10s + 并发上限（本示例的组合），不要只调大超时；
 * - 上传/大请求 → 按路由单独放宽（例如 /upload 60s），并配合 body 大小上限；
 * - 内网低延迟调用 → 1-3s，快速失败让上游重试别处；
 * - 过载保护 → 先限流（503）再考虑调大超时；只调大超时等于把内存让给攻击者。
 *
 * 本示例用真实 TCP + 真实 `http::Parser` 复刻 gateway 的 deadline 逻辑
 *（示例把 10s 缩成 300ms，便于 1 秒内跑完；调大场景用 1500ms）。
 */
#include "go2cpp/http.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <memory>
#include <string>
#include <thread>

namespace {

using namespace std::chrono_literals;

struct Stats {
    std::atomic<std::uint64_t> served{0};
    std::atomic<std::uint64_t> timeouts{0};
    std::atomic<std::uint64_t> rejected{0};
    /// 当前活跃连接数；每个 GatewaySim 实例独立（不要用函数内 static，
    /// 否则上一个实例未退出的连接会污染下一个实例的限流计数）。
    std::atomic<std::size_t> active{0};
};

struct Config {
    /// 单请求读取总预算；gateway 默认 10s，这里默认 300ms 便于演示。
    std::chrono::milliseconds request_timeout{300};
    /// 并发连接上限；超过直接 503（gateway 的 max_queued_connections 是同类背压）。
    std::size_t max_concurrent{2};
    /// 维护模式：accept 后直接 503，不读请求。
    bool maintenance{false};
};

bool WriteAll(int fd, std::string_view data) {
    std::size_t sent = 0;
    while (sent < data.size()) {
        const ssize_t n = ::send(fd, data.data() + sent, data.size() - sent,
                                 MSG_NOSIGNAL);
        if (n <= 0) {
            return false;
        }
        sent += static_cast<std::size_t>(n);
    }
    return true;
}

std::string MakeResponse(int status, std::string_view reason,
                         std::string_view body) {
    go2cpp::http::Response response;
    response.status = static_cast<go2cpp::http::Status>(status);
    response.header.set("Content-Type", "application/json");
    response.body = std::string(body);
    response.close = true;  // 每个连接只处理一个请求，避免 keep-alive 掩盖问题。
    (void)reason;
    return response.to_string();
}

/// @brief 单个连接的读取 + 响应：复刻 gateway 的「总 deadline」语义。
void ServeOne(int fd, Config config, std::shared_ptr<Stats> stats) {
    // ① 维护模式：直接 503，不读 body（真实网关的维护窗口同理）。
    if (config.maintenance) {
        WriteAll(fd, MakeResponse(503, "Service Unavailable", R"({"error":"maintenance"})"));
        ::close(fd);
        return;
    }

    // ② 限流：先占坑再判断；超过上限立即 503，不进入读循环。
    const std::size_t now_active =
        stats->active.fetch_add(1, std::memory_order_relaxed) + 1;
    if (now_active > config.max_concurrent) {
        stats->rejected.fetch_add(1, std::memory_order_relaxed);
        WriteAll(fd, MakeResponse(503, "Service Unavailable", R"({"error":"too many connections"})"));
        stats->active.fetch_sub(1);
        ::close(fd);
        return;
    }

    // ③ 单请求总 deadline：从第一次 recv 前起算（gateway.cpp:1301-1302）。
    const auto deadline = std::chrono::steady_clock::now() + config.request_timeout;
    go2cpp::http::Parser parser;
    go2cpp::http::Request request;
    char buffer[4096];
    bool timed_out = false;

    for (;;) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
            timed_out = true;
            break;  // gateway 到期直接 return，不写响应、关闭连接。
        }
        const auto remaining =
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
        pollfd entry{fd, POLLIN, 0};
        const int ready = ::poll(&entry, 1, static_cast<int>(remaining.count()));
        if (ready == 0) {
            timed_out = true;
            break;
        }
        if (ready < 0) {
            break;
        }
        const ssize_t n = ::recv(fd, buffer, sizeof(buffer), 0);
        if (n <= 0) {
            break;  // 客户端断开
        }
        const auto parsed =
            parser.feed(std::string_view(buffer, static_cast<std::size_t>(n)), request);
        if (parsed.state == go2cpp::http::ParseState::Error) {
            WriteAll(fd, MakeResponse(400, "Bad Request", R"({"error":"bad request"})"));
            stats->active.fetch_sub(1);
            ::close(fd);
            return;
        }
        if (parsed.state == go2cpp::http::ParseState::Complete) {
            stats->served.fetch_add(1, std::memory_order_relaxed);
            WriteAll(fd, MakeResponse(200, "OK", R"({"ok":true})"));
            stats->active.fetch_sub(1);
            ::close(fd);
            return;
        }
    }

    if (timed_out) {
        stats->timeouts.fetch_add(1, std::memory_order_relaxed);
    }
    stats->active.fetch_sub(1);
    ::close(fd);
}

class GatewaySim final {
public:
    GatewaySim(Config config, std::shared_ptr<Stats> stats)
        : config_(config), stats_(std::move(stats)) {
        listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (listen_fd_ < 0) {
            return;
        }
        int one = 1;
        ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;
        if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 ||
            ::listen(listen_fd_, 16) != 0) {
            ::close(listen_fd_);
            listen_fd_ = -1;
            return;
        }
        socklen_t length = sizeof(addr);
        ::getsockname(listen_fd_, reinterpret_cast<sockaddr*>(&addr), &length);
        port_ = ntohs(addr.sin_port);
        accept_thread_ = std::thread([this] { AcceptLoop(); });
    }

    ~GatewaySim() {
        if (listen_fd_ >= 0) {
            ::shutdown(listen_fd_, SHUT_RDWR);
            ::close(listen_fd_);
        }
        if (accept_thread_.joinable()) {
            accept_thread_.join();
        }
    }

    std::uint16_t port() const noexcept { return port_; }

private:
    void AcceptLoop() {
        while (true) {
            const int fd = ::accept(listen_fd_, nullptr, nullptr);
            if (fd < 0) {
                return;
            }
            // 连接线程只捕获 fd/config/stats（不捕获 this），析构后也不会悬垂。
            std::thread(ServeOne, fd, config_, stats_).detach();
        }
    }

    Config config_;
    std::shared_ptr<Stats> stats_;
    int listen_fd_{-1};
    std::uint16_t port_{0};
    std::thread accept_thread_;
};

int ConnectTo(std::uint16_t port) {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(port);
    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        ::close(fd);
        return -1;
    }
    return fd;
}

std::string ReadAll(int fd) {
    std::string out;
    char buffer[1024];
    for (;;) {
        const ssize_t n = ::recv(fd, buffer, sizeof(buffer), 0);
        if (n <= 0) {
            return out;
        }
        out.append(buffer, static_cast<std::size_t>(n));
    }
}

/// @brief 慢客户端：每隔 interval 发 chunk 字节，直到发完或对端关闭。
/// @return true 表示请求发完（未提前被服务端关闭）。
bool SendSlowly(int fd, std::string_view request, std::size_t chunk,
                std::chrono::milliseconds interval) {
    std::size_t sent = 0;
    while (sent < request.size()) {
        const std::size_t n = std::min(chunk, request.size() - sent);
        const ssize_t rc =
            ::send(fd, request.data() + sent, n, MSG_NOSIGNAL);
        if (rc <= 0) {
            return false;
        }
        sent += static_cast<std::size_t>(rc);
        std::this_thread::sleep_for(interval);
    }
    return true;
}

int SendNormal(std::uint16_t port, const char* path) {
    const int fd = ConnectTo(port);
    if (fd < 0) {
        return -1;
    }
    const std::string request =
        std::string("GET ") + path + " HTTP/1.1\r\nHost: demo\r\n\r\n";
    WriteAll(fd, request);
    const std::string response = ReadAll(fd);
    ::close(fd);
    return response.find(" 200 ") != std::string::npos ? 200 : 500;
}

}  // namespace

int main() {
    bool ok = true;

    // 场景 A：默认 300ms —— 慢客户端发不完，服务端到点直接关闭。
    {
        auto stats = std::make_shared<Stats>();
        GatewaySim gateway(Config{}, stats);
        if (gateway.port() == 0) {
            std::cerr << "无法启动本地网关\n";
            return 1;
        }
        const int slow = ConnectTo(gateway.port());
        const bool finished = SendSlowly(
            slow, "GET / HTTP/1.1\r\nHost: slowloris\r\nX-Pad: 0123456789\r\n\r\n",
            4, 100ms);
        ::close(slow);
        std::this_thread::sleep_for(50ms);
        std::cout << "[默认] 慢客户端 request_sent=" << finished
                  << " timeouts=" << stats->timeouts.load()
                  << "（预期 0 / 1：300ms 内只发了约 12 字节，被关闭）\n";
        ok = ok && !finished && stats->timeouts.load() == 1;
    }

    // 场景 B：调大预算到 1500ms —— 同样慢的客户端能发完并拿到 200。
    {
        auto stats = std::make_shared<Stats>();
        Config config;
        config.request_timeout = 1500ms;
        GatewaySim gateway(config, stats);
        const int slow = ConnectTo(gateway.port());
        const bool finished = SendSlowly(
            slow, "GET / HTTP/1.1\r\nHost: slow\r\nX-Pad: 0123456789\r\n\r\n", 4,
            100ms);
        const std::string response = finished ? ReadAll(slow) : std::string();
        ::close(slow);
        std::this_thread::sleep_for(50ms);
        std::cout << "[调大] request_timeout=1500ms request_sent=" << finished
                  << " status_200=" << (response.find(" 200 ") != std::string::npos)
                  << " timeouts=" << stats->timeouts.load()
                  << "（预期 1 / 1 / 0）\n";
        ok = ok && finished &&
             response.find(" 200 ") != std::string::npos &&
             stats->timeouts.load() == 0;
    }

    // 场景 C：限流 —— 并发上限 2，占用 2 个慢连接后第 3 个直接 503。
    {
        auto stats = std::make_shared<Stats>();
        Config config;
        config.request_timeout = 1000ms;
        config.max_concurrent = 2;
        GatewaySim gateway(config, stats);
        const int hold1 = ConnectTo(gateway.port());
        const int hold2 = ConnectTo(gateway.port());
        WriteAll(hold1, "G");
        WriteAll(hold2, "G");
        std::this_thread::sleep_for(100ms);  // 让前两个连接先占住 active 计数。
        const int fd = ConnectTo(gateway.port());
        WriteAll(fd, "GET / HTTP/1.1\r\nHost: t\r\n\r\n");
        const std::string response = ReadAll(fd);
        ::close(fd);
        ::close(hold1);
        ::close(hold2);
        std::this_thread::sleep_for(100ms);
        std::cout << "[限流] max_concurrent=2 第三个连接 status_503="
                  << (response.find(" 503 ") != std::string::npos)
                  << " rejected=" << stats->rejected.load() << "（预期 1 / 1）\n";
        ok = ok && response.find(" 503 ") != std::string::npos &&
             stats->rejected.load() == 1;
    }

    // 场景 D：维护模式 —— 不读请求，accept 后直接 503。
    {
        auto stats = std::make_shared<Stats>();
        Config config;
        config.maintenance = true;
        GatewaySim gateway(config, stats);
        const int fd = ConnectTo(gateway.port());
        WriteAll(fd, "GET / HTTP/1.1\r\nHost: t\r\n\r\n");
        const std::string response = ReadAll(fd);
        ::close(fd);
        std::cout << "[维护] status_503="
                  << (response.find(" 503 ") != std::string::npos)
                  << "（预期 1）\n";
        ok = ok && response.find(" 503 ") != std::string::npos;
    }

    // 正常请求必须仍能通过（超时/限流不能误伤健康流量）。
    {
        auto stats = std::make_shared<Stats>();
        GatewaySim gateway(Config{}, stats);
        const int status = SendNormal(gateway.port(), "/health");
        std::cout << "[正常] status=" << status << " served="
                  << stats->served.load() << "（预期 200 / 1）\n";
        ok = ok && status == 200 && stats->served.load() == 1;
    }

    std::cout << (ok ? "[server-slowloris] 通过\n" : "[server-slowloris] 失败\n");
    return ok ? 0 : 1;
}
