/**
 * @file bench_batch3_link_socket.cpp
 * @brief 批 3 结构性优化（Link 收发分离 / Socket 去锁）的前后对比基准。
 * @details 本程序是**独立基准**（不进 ctest），用同一份源码在改动前后各跑
 *          一次，给出可对比的数字：
 *
 *          - `link_tx`：Link 发送 API 调用延迟（p50/p99/max）。
 *            两个子场景：
 *              * `idle`：接收线程 `RunOnce(5)` 大部分时间阻塞在 poll；
 *              * `busy`：对端持续灌包，接收线程每轮处理到 kMaxFramesPerRun。
 *            两者差值就是「发送等接收循环」造成的退化。
 *          - `link_stress`：单链路不可靠通道 1000 万包吞吐（packets/s）。
 *          - `sock_rtt`：Socket 层 ping-pong（unix socketpair / UDP / TCP），
 *            每次 op 都走 `begin_operation()` + `end_operation()`，用于观察
 *            去锁优化后的每 op 成本。
 *
 * @author go2cpp contributors
 * @date 2026-11-01
 * @copyright MIT License
 */

#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "go2cpp/bento/net/link.hpp"
#include "go2cpp/network/socket.hpp"

namespace {

using Clock = std::chrono::steady_clock;

double NowNs() {
    return static_cast<double>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch())
            .count());
}

/// @brief 进程 CPU 时间（用户 + 系统），纳秒。用于估算每 op 的 CPU 成本。
double CpuNs() {
    timespec ts{};
    clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts);
    return static_cast<double>(ts.tv_sec) * 1e9 + static_cast<double>(ts.tv_nsec);
}

/// @brief 延迟样本集合（微秒）。
class Samples {
   public:
    explicit Samples(std::size_t reserve) {
        m_values.reserve(reserve);
    }
    void Add(double us) {
        m_values.push_back(us);
    }
    double Percentile(double p) const {
        if (m_values.empty()) {
            return 0.0;
        }
        std::vector<double> copy = m_values;
        std::sort(copy.begin(), copy.end());
        const std::size_t index = static_cast<std::size_t>(
            std::min<double>(static_cast<double>(copy.size() - 1), p * (copy.size() - 1)));
        return copy[index];
    }
    double Max() const {
        return m_values.empty() ? 0.0 : *std::max_element(m_values.begin(), m_values.end());
    }
    double Mean() const {
        if (m_values.empty()) {
            return 0.0;
        }
        double sum = 0.0;
        for (double v : m_values) {
            sum += v;
        }
        return sum / static_cast<double>(m_values.size());
    }
    std::size_t size() const {
        return m_values.size();
    }

   private:
    std::vector<double> m_values;
};

/// @brief 把一段可调用对象记为一次延迟样本。
template <typename Fn>
double TimeUs(Fn&& fn) {
    const double begin = NowNs();
    fn();
    return (NowNs() - begin) / 1000.0;
}

go2cpp::bento::LinkConfig MakeServerConfig() {
    go2cpp::bento::LinkConfig config;
    config.require_encryption = false;
    config.bind_address = "127.0.0.1";
    config.heartbeat_interval_ms = 200;
    config.peer_timeout_ms = 10000;
    return config;
}

go2cpp::bento::LinkConfig MakeClientConfig(std::uint16_t server_port) {
    go2cpp::bento::LinkConfig config;
    config.require_encryption = false;
    config.bind_address = "127.0.0.1";
    config.remote = "127.0.0.1";
    config.peer_port = server_port;
    config.heartbeat_interval_ms = 200;
    config.peer_timeout_ms = 10000;
    config.random_seed = 0x3A3A0001U;
    return config;
}

/// @brief 连接 + 泵线程；返回是否在 5s 内双方 connected。
bool ConnectPair(go2cpp::bento::Link& server, go2cpp::bento::Link& client,
                 std::atomic<bool>& stop, std::thread& server_thread, std::thread& client_thread,
                 std::atomic<std::uint64_t>& server_received,
                 std::atomic<std::uint64_t>& client_received) {
    server.on_datagram = [&server_received](std::uint8_t, const std::uint8_t*, std::size_t) {
        server_received.fetch_add(1, std::memory_order_relaxed);
    };
    client.on_datagram = [&client_received](std::uint8_t, const std::uint8_t*, std::size_t) {
        client_received.fetch_add(1, std::memory_order_relaxed);
    };
    client.Connect();
    const double deadline = NowNs() + 5e9;
    while (NowNs() < deadline) {
        server.RunOnce(1);
        client.RunOnce(1);
        if (server.connected() && client.connected()) {
            break;
        }
    }
    if (!server.connected() || !client.connected()) {
        return false;
    }
    server_thread = std::thread([&server, &stop]() {
        while (!stop.load(std::memory_order_relaxed)) {
            server.RunOnce(1);
        }
    });
    client_thread = std::thread([&client, &stop]() {
        while (!stop.load(std::memory_order_relaxed)) {
            client.RunOnce(5);
        }
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    return true;
}

// ---------------------------------------------------------------------------
// link_tx：发送 API 调用延迟（空闲 / 接收繁忙）
// ---------------------------------------------------------------------------

void RunLinkTxCase(const char* name, bool busy) {
    go2cpp::bento::Link server{MakeServerConfig()};
    std::string error;
    if (!server.Open(&error)) {
        std::fprintf(stderr, "server.Open failed: %s\n", error.c_str());
        return;
    }
    go2cpp::bento::Link client{MakeClientConfig(server.local_port())};
    if (!client.Open(&error)) {
        std::fprintf(stderr, "client.Open failed: %s\n", error.c_str());
        return;
    }
    std::atomic<bool> stop{false};
    std::atomic<std::uint64_t> server_received{0};
    std::atomic<std::uint64_t> client_received{0};
    std::thread server_thread;
    std::thread client_thread;
    if (!ConnectPair(server, client, stop, server_thread, client_thread, server_received,
                     client_received)) {
        std::fprintf(stderr, "[%s] handshake failed\n", name);
        stop.store(true);
        if (server_thread.joinable()) {
            server_thread.join();
        }
        if (client_thread.joinable()) {
            client_thread.join();
        }
        return;
    }

    // 繁忙场景：服务端线程以最大速度向客户端灌 64B 数据报，把客户端的
    // RunOnce 压到「poll 立即返回 + 每轮处理 256 帧」的状态。
    std::atomic<bool> flood_stop{false};
    std::thread flood_thread;
    if (busy) {
        flood_thread = std::thread([&server, &flood_stop]() {
            std::uint8_t payload[64];
            std::memset(payload, 0x5A, sizeof(payload));
            while (!flood_stop.load(std::memory_order_relaxed)) {
                for (int i = 0; i < 64; ++i) {
                    (void)server.SendDatagram(1, payload, sizeof(payload));
                }
            }
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
    }

    constexpr int kSamples = 5000;
    constexpr double kIntervalNs = 200e3;  // 200µs 一个探测点
    std::uint8_t payload[64];
    std::memset(payload, 0xA5, sizeof(payload));
    Samples samples(kSamples);
    std::uint64_t sent = 0;
    double next = NowNs();
    for (int i = 0; i < kSamples; ++i) {
        next += kIntervalNs;
        while (NowNs() < next) {
            // 自旋到下一个探测点，避免 sleep 粒度把样本噪声放大。
        }
        const double us = TimeUs([&]() { sent += client.SendDatagram(1, payload, sizeof(payload)); });
        samples.Add(us);
    }
    flood_stop.store(true);
    if (flood_thread.joinable()) {
        flood_thread.join();
    }
    stop.store(true);
    if (server_thread.joinable()) {
        server_thread.join();
    }
    if (client_thread.joinable()) {
        client_thread.join();
    }

    std::printf(
        "[link_tx:%s] send=%llu ok=%llu p50=%.1fus p99=%.1fus p999=%.1fus max=%.1fus "
        "mean=%.1fus server_rx=%llu client_rx=%llu\n",
        name, static_cast<unsigned long long>(kSamples), static_cast<unsigned long long>(sent),
        samples.Percentile(0.50), samples.Percentile(0.99), samples.Percentile(0.999),
        samples.Max(), samples.Mean(), static_cast<unsigned long long>(server_received.load()),
        static_cast<unsigned long long>(client_received.load()));
    std::fflush(stdout);
}

// ---------------------------------------------------------------------------
// link_stress：1000 万包吞吐
// ---------------------------------------------------------------------------

void RunLinkStress(std::uint64_t total) {
    go2cpp::bento::Link server{MakeServerConfig()};
    std::string error;
    if (!server.Open(&error)) {
        std::fprintf(stderr, "server.Open failed: %s\n", error.c_str());
        return;
    }
    go2cpp::bento::Link client{MakeClientConfig(server.local_port())};
    if (!client.Open(&error)) {
        std::fprintf(stderr, "client.Open failed: %s\n", error.c_str());
        return;
    }
    std::atomic<bool> stop{false};
    std::atomic<std::uint64_t> server_received{0};
    std::atomic<std::uint64_t> client_received{0};
    std::thread server_thread;
    std::thread client_thread;
    if (!ConnectPair(server, client, stop, server_thread, client_thread, server_received,
                     client_received)) {
        std::fprintf(stderr, "[link_stress] handshake failed\n");
        stop.store(true);
        if (server_thread.joinable()) {
            server_thread.join();
        }
        if (client_thread.joinable()) {
            client_thread.join();
        }
        return;
    }
    std::uint8_t payload[64];
    std::memset(payload, 0x11, sizeof(payload));
    std::uint64_t ok = 0;
    const double cpu_before = CpuNs();
    const double begin = NowNs();
    for (std::uint64_t i = 0; i < total; ++i) {
        if (client.SendDatagram(1, payload, sizeof(payload))) {
            ++ok;
        }
    }
    const double elapsed = NowNs() - begin;
    const double cpu = CpuNs() - cpu_before;
    stop.store(true);
    if (server_thread.joinable()) {
        server_thread.join();
    }
    if (client_thread.joinable()) {
        client_thread.join();
    }
    std::printf(
        "[link_stress] sent=%llu ok=%llu rx=%llu elapsed=%.3fs pps=%.0f cpu_ms=%.0f "
        "cpu_ns_per_pkt=%.1f\n",
        static_cast<unsigned long long>(total), static_cast<unsigned long long>(ok),
        static_cast<unsigned long long>(server_received.load()), elapsed / 1e9,
        static_cast<double>(ok) / (elapsed / 1e9), cpu / 1e6, cpu / static_cast<double>(ok));
    std::fflush(stdout);
}

// ---------------------------------------------------------------------------
// sock_rtt：Socket 层 ping-pong
// ---------------------------------------------------------------------------

struct RttReport {
    const char* name = "";
    std::uint64_t rounds = 0;
    double elapsed_ns = 0;
    double cpu_ns = 0;
};

void PrintRtt(const RttReport& r) {
    const double per_round_us = r.elapsed_ns / static_cast<double>(r.rounds) / 1e3;
    const double ops = static_cast<double>(r.rounds) * 2.0;  // 一 send 一 recv
    std::printf("[sock_rtt:%s] rounds=%llu ops_per_s=%.0f us_per_round=%.3f cpu_ns_per_op=%.1f\n",
                r.name, static_cast<unsigned long long>(r.rounds), ops / (r.elapsed_ns / 1e9),
                per_round_us, r.cpu_ns / ops);
    std::fflush(stdout);
}

void RunUnixPairRtt(std::uint64_t rounds) {
    int fds[2] = {-1, -1};
    if (::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, fds) != 0) {
        std::perror("socketpair");
        return;
    }
    auto a = go2cpp::net::Socket::FromFd(fds[0], AF_UNIX, SOCK_STREAM);
    auto b = go2cpp::net::Socket::FromFd(fds[1], AF_UNIX, SOCK_STREAM);
    std::uint8_t payload[64];
    std::memset(payload, 0x33, sizeof(payload));
    std::uint8_t buffer[128];
    RttReport report;
    report.name = "unix";
    report.rounds = rounds;
    const double cpu_before = CpuNs();
    const double begin = NowNs();
    for (std::uint64_t i = 0; i < rounds; ++i) {
        (void)a->send(payload, sizeof(payload));
        (void)b->recv(buffer, sizeof(buffer));
        (void)b->send(payload, sizeof(payload));
        (void)a->recv(buffer, sizeof(buffer));
    }
    report.elapsed_ns = NowNs() - begin;
    report.cpu_ns = CpuNs() - cpu_before;
    PrintRtt(report);
}

void RunUdpRtt(std::uint64_t rounds) {
    auto server = go2cpp::net::Socket::CreateUDP(AF_INET);
    auto client = go2cpp::net::Socket::CreateUDP(AF_INET);
    auto server_addr = go2cpp::net::ResolveUDPAddr("127.0.0.1", 0);
    if (!server || !client || !server_addr || !server->bind(*server_addr)) {
        std::fprintf(stderr, "[sock_rtt:udp] setup failed\n");
        return;
    }
    auto bound = server->local_address();
    if (!bound) {
        std::fprintf(stderr, "[sock_rtt:udp] local_address failed\n");
        return;
    }
    (void)client->connect(*bound);
    std::uint8_t payload[64];
    std::memset(payload, 0x44, sizeof(payload));
    std::uint8_t buffer[128];
    RttReport report;
    report.name = "udp";
    report.rounds = rounds;
    const double cpu_before = CpuNs();
    const double begin = NowNs();
    for (std::uint64_t i = 0; i < rounds; ++i) {
        (void)client->send(payload, sizeof(payload));
        (void)server->recv(buffer, sizeof(buffer));
        (void)server->send_to(payload, sizeof(payload), *bound);
        (void)client->recv(buffer, sizeof(buffer));
    }
    report.elapsed_ns = NowNs() - begin;
    report.cpu_ns = CpuNs() - cpu_before;
    PrintRtt(report);
}

void RunTcpRtt(std::uint64_t rounds) {
    auto listener = go2cpp::net::Socket::CreateTCP(AF_INET);
    auto listen_addr = go2cpp::net::ResolveTCPAddr("127.0.0.1", 0);
    if (!listener || !listen_addr || !listener->bind(*listen_addr) || !listener->listen(64)) {
        std::fprintf(stderr, "[sock_rtt:tcp] listen failed\n");
        return;
    }
    auto bound = listener->local_address();
    if (!bound) {
        return;
    }
    std::atomic<bool> ready{false};
    std::thread server_thread([&]() {
        auto conn = listener->accept();
        if (!conn) {
            return;
        }
        ready.store(true);
        std::uint8_t buffer[128];
        std::uint8_t payload[64];
        std::memset(payload, 0x66, sizeof(payload));
        for (;;) {
            const auto received = conn->recv(buffer, sizeof(buffer));
            if (received.error != 0 || received.transferred == 0) {
                break;
            }
            const auto sent = conn->send(payload, sizeof(payload));
            if (sent.error != 0 || sent.transferred == 0) {
                break;
            }
        }
    });
    auto client = go2cpp::net::Socket::CreateTCP(AF_INET);
    if (!client || !client->connect(*bound)) {
        std::fprintf(stderr, "[sock_rtt:tcp] connect failed\n");
        listener->close();
        server_thread.join();
        return;
    }
    while (!ready.load()) {
        std::this_thread::sleep_for(std::chrono::microseconds(50));
    }
    std::uint8_t payload[64];
    std::memset(payload, 0x77, sizeof(payload));
    std::uint8_t buffer[128];
    RttReport report;
    report.name = "tcp";
    report.rounds = rounds;
    const double cpu_before = CpuNs();
    const double begin = NowNs();
    for (std::uint64_t i = 0; i < rounds; ++i) {
        (void)client->send(payload, sizeof(payload));
        (void)client->recv(buffer, sizeof(buffer));
    }
    report.elapsed_ns = NowNs() - begin;
    report.cpu_ns = CpuNs() - cpu_before;
    PrintRtt(report);
    client->close();
    server_thread.join();
    listener->close();
}

}  // namespace

int main(int argc, char** argv) {
    const std::string mode = argc > 1 ? argv[1] : "all";
    std::setvbuf(stdout, nullptr, _IOLBF, 0);
    std::printf("=== bench_batch3 (mode=%s) ===\n", mode.c_str());
    if (mode == "link_tx" || mode == "all") {
        RunLinkTxCase("idle", false);
        RunLinkTxCase("busy", true);
    }
    if (mode == "link_stress" || mode == "all") {
        const std::uint64_t total = argc > 2
                                        ? static_cast<std::uint64_t>(std::strtoull(argv[2], nullptr, 10))
                                        : 10000000ULL;
        RunLinkStress(total);
    }
    if (mode == "sock_rtt" || mode == "all") {
        RunUnixPairRtt(200000);
        RunUdpRtt(100000);
        RunTcpRtt(100000);
    }
    return 0;
}
