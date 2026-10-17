/**
 * @file test_tls_alpn.cpp
 * @brief 验证 TlsTransport 的 TLS 握手与 ALPN 协商。
 *
 * 用 `openssl s_server -alpn h2` 作为对端（独立实现），验证：
 * 1. TLS 握手能完成；
 * 2. ALPN 协商结果是 `h2` —— 这是 HTTP/2 over TLS 的前置条件。
 *
 * **范围说明**：`s_server` 不懂 HTTP/2 帧，所以这里只验证 TLS 层。
 * HTTP/2 over TLS 的完整链路需要一个真正的 h2 TLS 服务器（如 nginx/nghttpd），
 * 本环境没有；但 ALPN 谈不出来就永远不可能有 HTTP/2，所以这一步是必要前提。
 */
#include "go2cpp/network/http2/tls_transport.hpp"
#include "go2cpp/network/socket.hpp"
#include "go2cpp/scheduler.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace {
using go2cpp::net::IPv4Address;
using go2cpp::net::Socket;
using go2cpp::net::http2::TlsTransport;
using go2cpp::scheduler::Scheduler;
using go2cpp::scheduler::SchedulerConfig;
using namespace std::chrono_literals;

int g_failures = 0;
void Check(bool condition, const char* what) {
    if (!condition) {
        std::fprintf(stderr, "[tls-alpn] FAILED: %s\n", what);
        ++g_failures;
    }
}

void Run(int port, std::atomic<bool>* done) {
    auto address = IPv4Address::Create("127.0.0.1", static_cast<std::uint16_t>(port));
    auto socket = Socket::CreateTCP(AF_INET);
    if (!socket || !socket->connect(*address, 5000ms)) {
        std::fprintf(stderr, "[tls-alpn] 连接失败\n");
        done->store(true);
        return;
    }
    auto transport = TlsTransport::CreateClient(socket, "localhost", false, 10s);
    Check(transport != nullptr, "TlsTransport::CreateClient failed");
    if (!transport) {
        done->store(true);
        return;
    }
    std::string error;
    const bool ok = transport->Handshake(&error);
    Check(ok, "TLS handshake failed");
    if (!ok) {
        std::fprintf(stderr, "[tls-alpn] 握手错误: %s\n", error.c_str());
        done->store(true);
        return;
    }
    std::printf("[tls-alpn] TLS 版本: %s\n", transport->TlsVersion().c_str());
    std::printf("[tls-alpn] ALPN 协商结果: '%s'\n", transport->NegotiatedProtocol().c_str());
    Check(!transport->NegotiatedProtocol().empty(), "ALPN result must not be empty");
    Check(transport->NegotiatedProtocol() == "h2", "ALPN should negotiate h2");

    // 顺手验证能在 TLS 上读写（s_server -www 会回一个 HTTP/1.1 响应）
    const char request[] = "GET / HTTP/1.0\r\n\r\n";
    const auto written = transport->Write(reinterpret_cast<const std::uint8_t*>(request),
                                          sizeof(request) - 1);
    Check(written == sizeof(request) - 1, "TLS write failed");
    std::uint8_t buffer[512] = {};
    const auto got = transport->Read(buffer, sizeof(buffer));
    std::printf("[tls-alpn] TLS 上读到 %zu 字节\n", got);
    Check(got > 0, "TLS read failed");
    done->store(true);
}

}  // namespace

int main(int argc, char** argv) {
    const int port = (argc > 1) ? std::atoi(argv[1]) : 18443;
    SchedulerConfig config;
    config.processor_count = 1;
    config.min_workers = 1;
    config.max_workers = 1;
    config.collect_metrics = false;
    Scheduler scheduler(config);
    scheduler.start();
    std::atomic<bool> done{false};
    auto task = scheduler.spawn([port, &done] { Run(port, &done); });
    (void)task->wait_for(30s);
    (void)scheduler.shutdown_for(5s);
    if (g_failures == 0) {
        std::printf("\n[tls-alpn] failures=0\n");
    } else {
        std::printf("\n[tls-alpn] failures=%d\n", g_failures);
    }
    return g_failures == 0 ? 0 : 1;
}
