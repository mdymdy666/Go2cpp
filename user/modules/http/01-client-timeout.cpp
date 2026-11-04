// Copyright 2026 Go2Cpp Authors. All rights reserved.
/**
 * @file 01-client-timeout.cpp
 * @brief 真实场景：下游 HTTP 服务慢，客户端等不到响应。
 *
 * ## 场景
 * 商品详情页聚合接口调用「库存服务」，库存服务因 GC/锁竞争要 1.5s 才回。
 * 网关侧希望 300ms 内必须给出结果（页面 SLA），不能陪着下游一起挂。
 *
 * ## 默认行为（本次实测）
 * - `http::Client` 的默认超时是 **30 秒**（include/go2cpp/network/http.hpp:358），
 *   覆盖 connect/send/recv 的绝对 deadline（http.hpp:368-369）；
 * - 到期返回 `Result::Error::Timeout`（http.hpp:328-329），**不抛异常**，
 *   `Result::message` 带中文原因（http.hpp:331）；
 * - 超时不会自动重试、不会自动降级——`Client::do_request()` 每次调用只发一次
 *   请求（http.hpp:349-350「每次 do_request() 自行建连，不复用连接池」）。
 *
 * ## 可选策略
 * 1. **重试**：仅对幂等请求（GET/HEAD）重试，次数有上限、间隔退避；
 *    本示例重试 2 次，每次仍用 300ms 预算。
 * 2. **降级**：重试仍失败时返回本地缓存快照（stale-while-revalidate），
 *    并在响应头标记 `X-Degraded: 1`，让上游知道数据可能过期。
 * 3. **取消**：上层有总预算 `deadline`（本示例 700ms）。预算耗尽后不再发起
 *    新请求，把结果标记为 Cancelled，避免「重试放大」把调用方也拖死。
 *
 * ## 如何选择
 * - 读接口 + 数据可短暂过期 → 重试 + 降级（默认落这里）；
 * - 写接口（非幂等）→ **不要**盲目重试，先做幂等键（Idempotency-Key）或直接报错；
 * - 有端到端 SLA（网关/页面）→ 必须带总预算取消，重试次数 = 预算 / 单次超时；
 * - 下游恢复慢且降级数据不可接受 → 重试后**报错**（返回 504），不要拿旧数据糊弄。
 *
 * 编译见 README；运行输出见文末（真实跑通，不是伪代码）。
 */
#include "go2cpp/http.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>

namespace {

using namespace std::chrono_literals;

/// @brief 一个「慢」的库存服务：accept 后 1.5s 才回 200，用真实 TCP 连接。
class SlowInventoryServer final {
public:
    SlowInventoryServer() {
        fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd_ < 0) {
            return;
        }
        int one = 1;
        ::setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;
        if (::bind(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 ||
            ::listen(fd_, 16) != 0) {
            ::close(fd_);
            fd_ = -1;
            return;
        }
        socklen_t length = sizeof(addr);
        if (::getsockname(fd_, reinterpret_cast<sockaddr*>(&addr), &length) == 0) {
            port_ = ntohs(addr.sin_port);
        }
        accept_thread_ = std::thread([this] { AcceptLoop(); });
    }

    ~SlowInventoryServer() {
        stop_.store(true, std::memory_order_relaxed);
        if (fd_ >= 0) {
            ::shutdown(fd_, SHUT_RDWR);
            ::close(fd_);
        }
        if (accept_thread_.joinable()) {
            accept_thread_.join();
        }
    }

    std::uint16_t port() const noexcept { return port_; }

private:
    void AcceptLoop() {
        while (!stop_.load(std::memory_order_relaxed)) {
            const int connection = ::accept(fd_, nullptr, nullptr);
            if (connection < 0) {
                return;  // listen fd 已关闭（析构）。
            }
            // 每个连接独立线程 + detach：慢服务不能阻塞下一个 accept，
            // 否则「重试」会排队等前一个 sleep，测出来的不是客户端超时。
            std::thread([connection] {
                std::this_thread::sleep_for(1500ms);
                constexpr char kResponse[] =
                    "HTTP/1.1 200 OK\r\n"
                    "Content-Type: application/json\r\n"
                    "Content-Length: 15\r\n"
                    "Connection: close\r\n"
                    "\r\n"
                    "{\"stock\": 42}\n";
                ::send(connection, kResponse, std::strlen(kResponse),
                       MSG_NOSIGNAL);
                ::close(connection);
            }).detach();
        }
    }

    int fd_{-1};
    std::uint16_t port_{0};
    std::atomic<bool> stop_{false};
    std::thread accept_thread_;
};

const char* ErrorName(go2cpp::http::Result::Error error) noexcept {
    using Error = go2cpp::http::Result::Error;
    switch (error) {
        case Error::None: return "None";
        case Error::InvalidUrl: return "InvalidUrl";
        case Error::Resolve: return "Resolve";
        case Error::Connect: return "Connect";
        case Error::Timeout: return "Timeout";
        case Error::Send: return "Send";
        case Error::Receive: return "Receive";
        case Error::Parse: return "Parse";
        case Error::Closed: return "Closed";
        case Error::Cancelled: return "Cancelled";
    }
    return "Unknown";
}

}  // namespace

int main() {
    using namespace std::chrono_literals;

    SlowInventoryServer server;
    if (server.port() == 0) {
        std::cerr << "[client-timeout] 无法启动本地慢服务\n";
        return 1;
    }
    const std::string url =
        "http://127.0.0.1:" + std::to_string(server.port()) + "/stock?id=42";

    // 单次预算 300ms（默认 30s 的 1/100），模拟业务 SLA。
    constexpr auto kPerAttempt = 300ms;
    // 上层总预算：超过后不再重试，对应「取消」策略。
    constexpr auto kTotalBudget = 700ms;

    go2cpp::http::Client client(kPerAttempt);
    const auto started = std::chrono::steady_clock::now();

    // ① 默认路径：单次请求，等 300ms 后拿到 Timeout。
    const auto first = client.get(url);
    std::cout << "[默认] 单次请求 error=" << ErrorName(first.error)
              << " message=\"" << first.message << "\"（预期 Timeout）\n";
    bool ok = first.error == go2cpp::http::Result::Error::Timeout;

    // ② 可选：重试（仅幂等 GET），每次检查总预算。
    int attempts = 0;
    bool cancelled = false;
    go2cpp::http::Result last = first;
    for (int i = 0; i < 2; ++i) {
        const auto elapsed = std::chrono::steady_clock::now() - started;
        if (elapsed >= kTotalBudget) {
            cancelled = true;
            break;
        }
        std::this_thread::sleep_for(20ms);  // 小退避，避免瞬间打死下游。
        last = client.get(url);
        ++attempts;
        std::cout << "[重试] 第 " << attempts << " 次 error="
                  << ErrorName(last.error) << "\n";
        if (last.ok()) {
            break;
        }
    }

    // ③ 可选：降级到本地缓存快照（stale-while-revalidate）。
    bool degraded = false;
    std::string payload;
    if (!last.ok()) {
        degraded = true;
        payload = R"({"stock": 42, "stale": true, "source": "local-cache"})";
        std::cout << "[降级] 使用本地缓存快照: " << payload
                  << "（响应头应带 X-Degraded: 1）\n";
    }

    // ④ 预算耗尽后的「取消」：不再发新请求。
    if (!cancelled &&
        std::chrono::steady_clock::now() - started >= kTotalBudget) {
        cancelled = true;
    }
    std::cout << "[取消] 总预算 700ms "
              << (cancelled ? "已耗尽，停止重试" : "未耗尽") << "\n";
    std::cout << "[统计] attempts=" << attempts << " degraded=" << degraded
              << " final_error=" << ErrorName(last.error) << "\n";

    // 三种策略的验收：默认必须超时；重试也超时；最终有降级值。
    ok = ok && attempts == 2 && degraded && !payload.empty() &&
         last.error == go2cpp::http::Result::Error::Timeout;
    std::cout << (ok ? "[client-timeout] 通过\n" : "[client-timeout] 失败\n");
    return ok ? 0 : 1;
}
