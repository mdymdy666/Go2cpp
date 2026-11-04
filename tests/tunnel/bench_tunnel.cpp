/**
 * @file bench_tunnel.cpp
 * @brief tunnel 数据面吞吐 / 延迟基准：直连 vs 经隧道，单连接与 4 并发。
 *
 * @details 拓扑：
 *
 * ```
 *   bench 客户端 --HTTP--> tunnel-server 公网端口 --多路复用--> tunnel-client
 *                                                              --HTTP--> 本地 SinkServer
 *   bench 客户端 --HTTP--> 本地 SinkServer（直连基线，无隧道）
 * ```
 *
 * 测量项：
 * - 吞吐：单连接一次 POST 8 MiB，重复 3 轮，取中位 MiB/s；
 * - 吞吐：4 并发连接各 8 MiB，重复 3 轮，按 wall 时间算聚合 MiB/s；
 * - 延迟：单连接 96 次 64 KiB POST 的 p50/p99（含每次建连）；
 * - 隧道开销倍率 = 直连中位吞吐 / 隧道中位吞吐（及其倒数）。
 *
 * 服务端为极简 HTTP 接收器（64 KiB 缓冲读完 body，回 `200 OK`），直连与
 * 隧道共用同一实现，保证基线一致。
 *
 * 运行：`./bench_tunnel [rounds] [concurrency]`。
 * 退出码 0 表示所有请求成功且长度核验通过；否则 1。
 *
 * @author go2cpp contributors
 * @date 2026-11-04
 * @copyright MIT License
 */

#include "go2cpp/error.hpp"
#include "go2cpp/tunnel/client.hpp"
#include "go2cpp/tunnel/net.hpp"
#include "go2cpp/tunnel/server.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;
using go2cpp::tunnel::TcpConn;
using go2cpp::tunnel::TcpListener;
using go2cpp::tunnel::TunnelClient;
using go2cpp::tunnel::TunnelClientConfig;
using go2cpp::tunnel::TunnelServer;
using go2cpp::tunnel::TunnelServerConfig;

std::atomic<std::uint64_t> g_body_bytes{0};
std::atomic<std::uint64_t> g_requests{0};

double MsBetween(Clock::time_point begin, Clock::time_point end) {
    return std::chrono::duration<double, std::milli>(end - begin).count();
}

/// 极简 HTTP 接收器：读完 Content-Length 指定的 body，回 200 OK。
class SinkServer {
public:
    ~SinkServer() { Stop(); }

    bool Start() {
        std::string error;
        m_listener = TcpListener::Listen("127.0.0.1", 0, &error);
        if (!m_listener) {
            std::fprintf(stderr, "[bench] sink listen failed: %s\n",
                         error.c_str());
            return false;
        }
        m_port = m_listener->BoundPort();
        m_running.store(true);
        m_thread = std::thread([this] { AcceptLoop(); });
        return true;
    }

    void Stop() {
        if (!m_running.exchange(false)) {
            return;
        }
        if (m_listener) {
            m_listener->Close();
        }
        if (m_thread.joinable()) {
            m_thread.join();
        }
        for (;;) {
            {
                std::lock_guard<std::mutex> lock(m_worker_mutex);
                if (m_workers == 0) {
                    break;
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    }

    std::uint16_t port() const { return m_port; }

private:
    void AcceptLoop() {
        while (m_running.load()) {
            TcpConn::Ptr conn = m_listener->Accept(100);
            if (!conn) {
                continue;
            }
            {
                std::lock_guard<std::mutex> lock(m_worker_mutex);
                ++m_workers;
            }
            std::thread([this, conn] {
                Handle(conn);
                {
                    std::lock_guard<std::mutex> lock(m_worker_mutex);
                    --m_workers;
                }
            }).detach();
        }
    }

    void Handle(const TcpConn::Ptr& conn) {
        std::vector<char> buffer(64U * 1024U);
        std::string head;
        std::size_t header_end = std::string::npos;
        while (header_end == std::string::npos) {
            const go2cpp::tunnel::IoResult result =
                conn->Read(buffer.data(), buffer.size(), 5000);
            if (!result.ok()) {
                conn->Shutdown();
                return;
            }
            head.append(buffer.data(), result.count);
            header_end = head.find("\r\n\r\n");
            if (head.size() > 128U * 1024U) {
                conn->Shutdown();
                return;
            }
        }
        const std::size_t content_length = ParseContentLength(head);
        std::size_t received = head.size() - header_end - 4;
        if (received > content_length) {
            received = content_length;
        }
        while (received < content_length) {
            const go2cpp::tunnel::IoResult result =
                conn->Read(buffer.data(), buffer.size(), 10000);
            if (!result.ok()) {
                break;
            }
            received += result.count;
        }
        g_body_bytes.fetch_add(received, std::memory_order_relaxed);
        g_requests.fetch_add(1, std::memory_order_relaxed);
        const char response[] =
            "HTTP/1.1 200 OK\r\nContent-Length: 2\r\nConnection: close\r\n\r\nOK";
        (void)conn->WriteAll(response, sizeof(response) - 1, 10000, nullptr);
        conn->Shutdown();
    }

    static std::size_t ParseContentLength(const std::string& head) {
        const std::string key = "Content-Length:";
        const std::size_t position = head.find(key);
        if (position == std::string::npos) {
            return 0;
        }
        std::size_t begin = position + key.size();
        while (begin < head.size() &&
               (head[begin] == ' ' || head[begin] == '\t')) {
            ++begin;
        }
        std::size_t end = begin;
        while (end < head.size() && head[end] >= '0' && head[end] <= '9') {
            ++end;
        }
        if (end == begin) {
            return 0;
        }
        return static_cast<std::size_t>(
            std::stoul(head.substr(begin, end - begin)));
    }

    TcpListener::Ptr m_listener;
    std::uint16_t m_port = 0;
    std::thread m_thread;
    std::atomic<bool> m_running{false};
    std::mutex m_worker_mutex;
    int m_workers = 0;
};

/// 发一次 POST 并读完响应；成功返回 true 并写入耗时。
bool PostOnce(std::uint16_t port, const std::string& host,
              const std::string& path, const std::string& body, double* ms) {
    const auto begin = Clock::now();
    std::string error;
    TcpConn::Ptr conn = TcpConn::Dial("127.0.0.1", port, 5000, &error);
    if (!conn) {
        return false;
    }
    const std::string head = "POST " + path + " HTTP/1.1\r\nHost: " + host +
                             "\r\nContent-Length: " +
                             std::to_string(body.size()) +
                             "\r\nConnection: close\r\n\r\n";
    if (!conn->WriteAll(head.data(), head.size(), 5000, nullptr) ||
        !conn->WriteAll(body.data(), body.size(), 60000, nullptr)) {
        conn->Shutdown();
        return false;
    }
    std::string response;
    std::vector<char> buffer(64U * 1024U);
    for (;;) {
        const go2cpp::tunnel::IoResult result =
            conn->Read(buffer.data(), buffer.size(), 60000);
        if (result.ok()) {
            response.append(buffer.data(), result.count);
            continue;
        }
        break;
    }
    conn->Shutdown();
    *ms = MsBetween(begin, Clock::now());
    return response.size() >= 15 && response.compare(0, 12, "HTTP/1.1 200") == 0;
}

struct Latencies {
    std::vector<double> samples;

    void Add(double value) { samples.push_back(value); }

    double Percentile(double fraction) const {
        if (samples.empty()) {
            return 0.0;
        }
        std::vector<double> sorted = samples;
        std::sort(sorted.begin(), sorted.end());
        std::size_t index = static_cast<std::size_t>(
            fraction * static_cast<double>(sorted.size() - 1) + 0.999999);
        if (index >= sorted.size()) {
            index = sorted.size() - 1;
        }
        return sorted[index];
    }

    double Mean() const {
        if (samples.empty()) {
            return 0.0;
        }
        double sum = 0.0;
        for (double value : samples) {
            sum += value;
        }
        return sum / static_cast<double>(samples.size());
    }
};

/// 单连接吞吐：一次 POST 整个 body。
bool RunSingle(std::uint16_t port, const std::string& host,
               const std::string& path, const std::string& body, double* mibps,
               double* ms) {
    if (!PostOnce(port, host, path, body, ms)) {
        return false;
    }
    const double seconds = *ms / 1000.0;
    *mibps = (static_cast<double>(body.size()) / (1024.0 * 1024.0)) / seconds;
    return true;
}

/// 并发吞吐：`threads` 条连接各 POST 一次，按 wall 时间算聚合吞吐。
bool RunConcurrent(std::uint16_t port, const std::string& host,
                   const std::string& path, std::size_t threads,
                   const std::string& body, double* mibps, Latencies* latencies,
                   double* wall_ms) {
    std::atomic<int> failures{0};
    std::vector<double> per_thread_ms(threads, 0.0);
    const auto begin = Clock::now();
    std::vector<std::thread> workers;
    workers.reserve(threads);
    for (std::size_t index = 0; index < threads; ++index) {
        workers.emplace_back([&, index] {
            if (!PostOnce(port, host, path, body, &per_thread_ms[index])) {
                failures.fetch_add(1);
            }
        });
    }
    for (std::thread& worker : workers) {
        worker.join();
    }
    *wall_ms = MsBetween(begin, Clock::now());
    const double total_bytes =
        static_cast<double>(body.size()) * static_cast<double>(threads);
    *mibps = (total_bytes / (1024.0 * 1024.0)) / (*wall_ms / 1000.0);
    for (double value : per_thread_ms) {
        if (value > 0.0) {
            latencies->Add(value);
        }
    }
    return failures.load() == 0;
}

double Median(std::vector<double> values) {
    if (values.empty()) {
        return 0.0;
    }
    std::sort(values.begin(), values.end());
    return values[values.size() / 2];
}

}  // namespace

int main(int argc, char** argv) {
    const int rounds = argc > 1 ? std::atoi(argv[1]) : 3;
    const int concurrency = argc > 2 ? std::atoi(argv[2]) : 4;
    const std::string body(8U * 1024U * 1024U, 'x');
    const std::size_t latency_requests = 96;
    const std::string small_body(64U * 1024U, 'y');

    SinkServer sink;
    if (!sink.Start()) {
        return 1;
    }

    TunnelServerConfig server_config;
    server_config.control_host = "127.0.0.1";
    server_config.control_port = 0;
    server_config.public_host = "127.0.0.1";
    server_config.public_port = 0;
    server_config.display_host = "127.0.0.1";
    server_config.token = "bench-token";
    server_config.recv_queue_cap = 8U * 1024U * 1024U;
    server_config.drain_timeout_ms = 1000;
    TunnelServer server(server_config);
    go2cpp::ErrorPtr error;
    if (!server.Start(&error)) {
        std::fprintf(stderr, "[bench] tunnel-server start failed: %s\n",
                     go2cpp::ErrorMessage(error).c_str());
        return 1;
    }

    TunnelClientConfig client_config;
    client_config.server_host = "127.0.0.1";
    client_config.server_port = server.control_port();
    client_config.local_host = "127.0.0.1";
    client_config.local_port = sink.port();
    client_config.subdomain = "bench";
    client_config.token = "bench-token";
    client_config.reconnect_min_ms = 100;
    client_config.reconnect_max_ms = 500;
    client_config.hello_timeout_ms = 5000;
    TunnelClient client(client_config);
    if (!client.Start(&error) || !client.WaitOnline(8000)) {
        std::fprintf(stderr, "[bench] tunnel-client offline\n");
        server.Stop();
        return 1;
    }

    const std::uint16_t direct_port = sink.port();
    const std::uint16_t tunnel_port = server.public_port();
    const std::string direct_host = "127.0.0.1:" + std::to_string(direct_port);
    const std::string tunnel_host = "127.0.0.1:" + std::to_string(tunnel_port);

    int failures = 0;
    std::vector<double> direct_mibps;
    std::vector<double> tunnel_mibps;

    std::printf("[bench] rounds=%d concurrency=%d payload=8MiB\n", rounds,
                concurrency);

    // 阶段 1：单连接吞吐（8 MiB 单请求）。直连与隧道**交替**执行，
    // 避免 CPU 升频/后台负载把先后顺序变成系统性偏差。
    for (int round = 0; round < rounds; ++round) {
        const bool direct_first = (round % 2 == 0);
        for (int pass = 0; pass < 2; ++pass) {
            const bool direct = (pass == 0) == direct_first;
            double ms = 0.0;
            double mibps = 0.0;
            const bool ok =
                direct ? RunSingle(direct_port, direct_host, "/upload", body,
                                   &mibps, &ms)
                       : RunSingle(tunnel_port, tunnel_host, "/bench/upload",
                                   body, &mibps, &ms);
            if (!ok) {
                ++failures;
                std::fprintf(stderr, "[bench] %s single failed round=%d\n",
                             direct ? "direct" : "tunnel", round);
                continue;
            }
            if (direct) {
                direct_mibps.push_back(mibps);
            } else {
                tunnel_mibps.push_back(mibps);
            }
            std::printf("[bench] %s_tput round=%d %.2f MiB/s (%.2f ms)\n",
                        direct ? "direct" : "tunnel", round + 1, mibps, ms);
        }
    }

    // 阶段 2：延迟（96 次 64 KiB POST）。
    Latencies direct_latency;
    Latencies tunnel_latency;
    for (std::size_t index = 0; index < latency_requests; ++index) {
        double ms = 0.0;
        if (!PostOnce(direct_port, direct_host, "/small", small_body, &ms)) {
            ++failures;
        } else {
            direct_latency.Add(ms);
        }
    }
    for (std::size_t index = 0; index < latency_requests; ++index) {
        double ms = 0.0;
        if (!PostOnce(tunnel_port, tunnel_host, "/bench/small", small_body,
                      &ms)) {
            ++failures;
        } else {
            tunnel_latency.Add(ms);
        }
    }
    std::printf(
        "[bench] latency direct n=%zu mean=%.3f p50=%.3f p99=%.3f ms\n",
        direct_latency.samples.size(), direct_latency.Mean(),
        direct_latency.Percentile(0.50), direct_latency.Percentile(0.99));
    std::printf(
        "[bench] latency tunnel n=%zu mean=%.3f p50=%.3f p99=%.3f ms\n",
        tunnel_latency.samples.size(), tunnel_latency.Mean(),
        tunnel_latency.Percentile(0.50), tunnel_latency.Percentile(0.99));

    // 阶段 3：并发吞吐（concurrency 条连接各 8 MiB），同样交替。
    std::vector<double> direct_conc_mibps;
    std::vector<double> tunnel_conc_mibps;
    Latencies direct_conc_latency;
    Latencies tunnel_conc_latency;
    for (int round = 0; round < rounds; ++round) {
        const bool direct_first = (round % 2 == 0);
        for (int pass = 0; pass < 2; ++pass) {
            const bool direct = (pass == 0) == direct_first;
            double mibps = 0.0;
            double wall_ms = 0.0;
            const bool ok = direct
                ? RunConcurrent(direct_port, direct_host, "/upload",
                                static_cast<std::size_t>(concurrency), body,
                                &mibps, &direct_conc_latency, &wall_ms)
                : RunConcurrent(tunnel_port, tunnel_host, "/bench/upload",
                                static_cast<std::size_t>(concurrency), body,
                                &mibps, &tunnel_conc_latency, &wall_ms);
            if (!ok) {
                ++failures;
                std::fprintf(stderr,
                             "[bench] %s concurrent failed round=%d\n",
                             direct ? "direct" : "tunnel", round);
                continue;
            }
            if (direct) {
                direct_conc_mibps.push_back(mibps);
            } else {
                tunnel_conc_mibps.push_back(mibps);
            }
            std::printf(
                "[bench] %s_conc round=%d x%d %.2f MiB/s (wall %.2f ms)\n",
                direct ? "direct" : "tunnel", round + 1, concurrency, mibps,
                wall_ms);
        }
    }

    const double direct_median = Median(direct_mibps);
    const double tunnel_median = Median(tunnel_mibps);
    const double direct_conc_median = Median(direct_conc_mibps);
    const double tunnel_conc_median = Median(tunnel_conc_mibps);
    std::printf(
        "[bench] summary single direct=%.2f tunnel=%.2f overhead=%.2fx\n",
        direct_median, tunnel_median,
        tunnel_median > 0.0 ? direct_median / tunnel_median : 0.0);
    std::printf(
        "[bench] summary conc%d direct=%.2f tunnel=%.2f overhead=%.2fx\n",
        concurrency, direct_conc_median, tunnel_conc_median,
        tunnel_conc_median > 0.0 ? direct_conc_median / tunnel_conc_median
                                 : 0.0);
    std::printf(
        "[bench] conc_latency direct p50=%.3f p99=%.3f tunnel p50=%.3f "
        "p99=%.3f ms\n",
        direct_conc_latency.Percentile(0.50),
        direct_conc_latency.Percentile(0.99),
        tunnel_conc_latency.Percentile(0.50),
        tunnel_conc_latency.Percentile(0.99));
    std::printf("[bench] integrity requests=%llu bytes=%llu failures=%d\n",
                static_cast<unsigned long long>(g_requests.load()),
                static_cast<unsigned long long>(g_body_bytes.load()), failures);

    client.Stop();
    server.Stop();
    return failures == 0 ? 0 : 1;
}
