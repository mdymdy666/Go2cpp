/**
 * @file weaknet_compare.hpp
 * @brief QUIC vs TCP 弱网公平对照：同一个内核 netem 注入器同时作用于 UDP 与 TCP。
 *
 * @details
 * 背景：此前只测过「QUIC + 用户态 UDP relay 丢包」（1% 丢包 p50 2463µs），
 * **没有 TCP 的丢包对照**，所以不能断言 QUIC 在弱网下更差——缺可比基线。
 *
 * ## 为什么注入点必须在内核
 *
 * TCP 是**字节流**。用户态 relay 丢一个 `read()` 块等于在字节流中间挖洞，
 * 接收端会直接把帧解析搞坏，而不是走「丢段 → SACK/快速重传 → 恢复」的真实
 * 路径。只有内核 qdisc 丢 TCP 段，TCP 才走它真正的重传逻辑；QUIC 的 UDP
 * 数据报在同一个 qdisc 上被同一套概率模型丢弃。因此两者的注入**不是"等价"，
 * 而是同一个注入器**（同一 qdisc、同一 loss/delay 参数、同一随机源），无需
 * 再做等价性论证。
 *
 * 代价是需要 CAP_NET_ADMIN：测试必须以 root 运行（WSL 下 `wsl -u root`）。
 * 非 root 时本模块直接跳过，不产生误导性数据。
 *
 * ## 场景
 *
 * baseline（无 qdisc）/ netem-0%（纯排队开销）/ loss 1%/5%/10% /
 * jitter ±20ms / ±50ms（netem `delay D J distribution normal`）/
 * RTT 50ms / 100ms（单向固定 delay）/ loss1%+RTT50ms（组合弱网）。
 *
 * 每个场景先做两级校准，证明注入确实生效：
 * 1. **单程**探测 400 个 UDP 包统计接收率（直接量出 netem 的丢包率；不用往返
 *    开方反推，因为用户态回显线程在批量突发下自身会丢包，会把丢包率算低）；
 * 2. **串行**探测 20 个 UDP 包统计往返 p50（与真实请求-响应节奏一致，避免
 *    批量突发在 netem 队列里产生的排队偏差——实测批量模式会把 50ms RTT
 *    测成 70ms，而 ping 与协议实测都是 50ms）。
 *
 * 然后分别跑 TCP 与 QUIC 的同步 RPC（同一台机器、同一回环、同一时刻），
 * 记录 p50/p99/max/QPS 与一次大载荷往返的单向吞吐。
 *
 * @author go2cpp contributors
 * @date 2026-11-02
 * @copyright MIT License
 */

#pragma once

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/wait.h>
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
#include <utility>
#include <vector>

namespace weaknet {

/// @brief 一个弱网场景的 netem 参数（0 表示该维度不注入）。
struct Scenario {
    const char* label;   ///< 报告标签。
    bool enabled;        ///< false = 移除 qdisc（真实基线，而非 netem 0%）。
    int loss_percent;    ///< 单程丢包百分比。
    int delay_ms;        ///< 单向基础延迟（毫秒）。
    int jitter_ms;       ///< 单向抖动（毫秒，正态分布 σ）。
};

/// @brief 场景清单：覆盖题面要求的基线/丢包/抖动/高 RTT 与一个组合弱网。
inline std::vector<Scenario> ScenarioList() {
    return {
        {"baseline(no netem)", false, 0, 0, 0},
        {"netem-0%", true, 0, 0, 0},
        {"loss-1%", true, 1, 0, 0},
        {"loss-5%", true, 5, 0, 0},
        {"loss-10%", true, 10, 0, 0},
        {"jitter-±20ms", true, 0, 20, 20},
        {"jitter-±50ms", true, 0, 50, 50},
        {"rtt-50ms", true, 0, 25, 0},
        {"rtt-100ms", true, 0, 50, 0},
        {"loss-1%+rtt-50ms", true, 1, 25, 0},
    };
}

/// @brief 该场景的预期往返（用于校准收包窗口）。
inline double ExpectedRttMs(const Scenario& scenario) {
    return 2.0 * (static_cast<double>(scenario.delay_ms) +
                  static_cast<double>(scenario.jitter_ms)) +
           1.0;
}

/// @brief 执行命令并收集合并后的 stdout/stderr。
inline std::string Shell(const std::string& command, int* exit_code) {
    const std::string full = command + " 2>&1";
    std::FILE* pipe = ::popen(full.c_str(), "r");
    if (pipe == nullptr) {
        if (exit_code != nullptr) {
            *exit_code = -1;
        }
        return "popen 失败";
    }
    std::string output;
    char buffer[512];
    while (std::fgets(buffer, sizeof(buffer), pipe) != nullptr) {
        output += buffer;
    }
    const int status = ::pclose(pipe);
    if (exit_code != nullptr) {
        if (status == -1) {
            *exit_code = -1;
        } else if (WIFEXITED(status)) {
            *exit_code = WEXITSTATUS(status);
        } else {
            *exit_code = status;
        }
    }
    return output;
}

/// @brief 内核 netem 控制器（作用于 `lo`，TCP 与 UDP 一视同仁）。
class NetemController {
   public:
    NetemController() = default;
    NetemController(const NetemController&) = delete;
    NetemController& operator=(const NetemController&) = delete;
    ~NetemController() { Clear(); }

    /// @brief 是否具备注入条件（root + iproute2 `tc`）。
    static bool Available() {
        if (::geteuid() != 0) {
            return false;
        }
        return std::system("command -v tc >/dev/null 2>&1") == 0;
    }

    /// @brief 应用场景；`enabled=false` 时移除 qdisc。
    bool Apply(const Scenario& scenario, std::string* error) {
        m_command.clear();
        if (!scenario.enabled) {
            Clear();
            return true;
        }
        std::string command = "tc qdisc replace dev lo root netem limit 20000";
        if (scenario.loss_percent > 0) {
            command += " loss " + std::to_string(scenario.loss_percent) + "%";
        }
        if (scenario.delay_ms > 0 || scenario.jitter_ms > 0) {
            const int base = scenario.delay_ms > 0 ? scenario.delay_ms : 1;
            command += " delay " + std::to_string(base) + "ms";
            if (scenario.jitter_ms > 0) {
                command += " " + std::to_string(scenario.jitter_ms) + "ms distribution normal";
            }
        }
        int code = 0;
        const std::string output = Shell(command, &code);
        if (code != 0) {
            if (error != nullptr) {
                *error = command + " -> " + output;
            }
            return false;
        }
        m_active = true;
        m_command = command;
        return true;
    }

    /// @brief 移除 qdisc（幂等；无 qdisc 时 tc 返回非零，忽略）。
    void Clear() {
        if (!m_active) {
            return;
        }
        int code = 0;
        (void)Shell("tc qdisc del dev lo root", &code);
        m_active = false;
        m_command.clear();
    }

    /// @brief 最近一次成功应用的命令文本（写报告用）。
    const std::string& command() const { return m_command; }

   private:
    bool m_active = false;
    std::string m_command;
};

/// @brief 场景校准结果：证明注入生效且对两种传输一致。
struct Calibration {
    int loss_sent = 0;
    int loss_received = 0;
    double loss_percent = 0.0;  ///< 单程丢包估计（往返成功率开方反推）。
    int rtt_probes = 0;
    double rtt_p50_ms = 0.0;
    double rtt_p99_ms = 0.0;
};

/// @brief 一对 127.0.0.1 UDP socket：一个回显，一个探测。
class UdpEchoPair {
   public:
    UdpEchoPair() {
        m_echo = ::socket(AF_INET, SOCK_DGRAM, 0);
        m_probe = ::socket(AF_INET, SOCK_DGRAM, 0);
        if (m_echo < 0 || m_probe < 0) {
            return;
        }
        sockaddr_in bind_addr{};
        bind_addr.sin_family = AF_INET;
        bind_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        bind_addr.sin_port = 0;
        if (::bind(m_echo, reinterpret_cast<sockaddr*>(&bind_addr), sizeof(bind_addr)) != 0 ||
            ::bind(m_probe, reinterpret_cast<sockaddr*>(&bind_addr), sizeof(bind_addr)) != 0) {
            return;
        }
        socklen_t echo_len = sizeof(m_echo_addr);
        (void)::getsockname(m_echo, reinterpret_cast<sockaddr*>(&m_echo_addr), &echo_len);
        m_running.store(true, std::memory_order_release);
        m_worker = std::thread([this]() {
            std::uint8_t buffer[64];
            while (m_running.load(std::memory_order_acquire)) {
                pollfd pfd{};
                pfd.fd = m_echo;
                pfd.events = POLLIN;
                if (::poll(&pfd, 1, 1) <= 0) {
                    continue;
                }
                sockaddr_in from{};
                socklen_t from_len = sizeof(from);
                const ssize_t bytes =
                    ::recvfrom(m_echo, buffer, sizeof(buffer), 0,
                               reinterpret_cast<sockaddr*>(&from), &from_len);
                if (bytes > 0) {
                    (void)::sendto(m_echo, buffer, static_cast<std::size_t>(bytes), 0,
                                   reinterpret_cast<const sockaddr*>(&from), from_len);
                }
            }
        });
    }

    ~UdpEchoPair() {
        m_running.store(false, std::memory_order_release);
        if (m_worker.joinable()) {
            m_worker.join();
        }
        if (m_echo >= 0) {
            ::close(m_echo);
        }
        if (m_probe >= 0) {
            ::close(m_probe);
        }
    }

    UdpEchoPair(const UdpEchoPair&) = delete;
    UdpEchoPair& operator=(const UdpEchoPair&) = delete;

    bool valid() const { return m_echo >= 0 && m_probe >= 0; }

    /// @brief 发一个带时间戳的探测包（不等待）。
    void Send(int seq) {
        std::uint8_t payload[16];
        std::memcpy(payload, "WNET", 4);
        const std::uint32_t index = static_cast<std::uint32_t>(seq);
        std::memcpy(payload + 4, &index, 4);
        const std::uint64_t stamp = static_cast<std::uint64_t>(rpctest::NowMicros());
        std::memcpy(payload + 8, &stamp, 8);
        (void)::sendto(m_probe, payload, sizeof(payload), 0,
                       reinterpret_cast<const sockaddr*>(&m_echo_addr), sizeof(m_echo_addr));
    }

    /// @brief 收一个回包；成功返回 true 并写出 RTT（微秒）。
    bool Receive(double timeout_ms, double* rtt_us) {
        pollfd pfd{};
        pfd.fd = m_probe;
        pfd.events = POLLIN;
        if (::poll(&pfd, 1, static_cast<int>(timeout_ms)) <= 0) {
            return false;
        }
        std::uint8_t buffer[64];
        const ssize_t bytes = ::recv(m_probe, buffer, sizeof(buffer), 0);
        if (bytes < 16) {
            return false;
        }
        std::uint64_t stamp = 0;
        std::memcpy(&stamp, buffer + 8, 8);
        const double now_us = rpctest::NowMicros();
        *rtt_us = now_us - static_cast<double>(stamp);
        return true;
    }

    /// @brief 批量发 `probes` 个包（间隔 `gap_us`），统计回包数——只用于估丢包率。
    int BulkRoundTrip(int probes, int gap_us, double collect_ms) {
        for (int index = 0; index < probes; ++index) {
            Send(index);
            ::usleep(static_cast<useconds_t>(gap_us));
        }
        int received = 0;
        const double deadline = rpctest::NowMicros() + collect_ms * 1000.0;
        while (received < probes && rpctest::NowMicros() < deadline) {
            double rtt = 0.0;
            if (Receive(5.0, &rtt)) {
                ++received;
            }
        }
        return received;
    }

    /// @brief 串行 ping-pong（与真实请求-响应同节奏）——用于测准确 RTT。
    std::vector<double> SerialRtt(int probes, double timeout_ms) {
        std::vector<double> samples;
        samples.reserve(static_cast<std::size_t>(probes));
        for (int index = 0; index < probes; ++index) {
            Send(index);
            double rtt = 0.0;
            if (Receive(timeout_ms, &rtt)) {
                samples.push_back(rtt);
            }
        }
        return samples;
    }

   private:
    int m_echo = -1;
    int m_probe = -1;
    sockaddr_in m_echo_addr{};
    std::atomic<bool> m_running{false};
    std::thread m_worker;
};

/// @brief 单程丢包探测：一发一收（接收端只计数不回包），直接量出 netem 的真实丢包率。
/// @note 不用「往返 + 开方」是因为用户态回显线程在批量突发下自身会丢包
///       （实测 baseline 240 包往返就丢 2 个），会把 netem 的丢包率算低。
class UdpLossProbe {
   public:
    UdpLossProbe() {
        m_sink = ::socket(AF_INET, SOCK_DGRAM, 0);
        m_probe = ::socket(AF_INET, SOCK_DGRAM, 0);
        if (m_sink < 0 || m_probe < 0) {
            return;
        }
        int buffer_bytes = 4 * 1024 * 1024;
        (void)::setsockopt(m_sink, SOL_SOCKET, SO_RCVBUF, &buffer_bytes,
                           sizeof(buffer_bytes));
        sockaddr_in bind_addr{};
        bind_addr.sin_family = AF_INET;
        bind_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        bind_addr.sin_port = 0;
        if (::bind(m_sink, reinterpret_cast<sockaddr*>(&bind_addr), sizeof(bind_addr)) != 0 ||
            ::bind(m_probe, reinterpret_cast<sockaddr*>(&bind_addr), sizeof(bind_addr)) != 0) {
            return;
        }
        socklen_t sink_len = sizeof(m_sink_addr);
        (void)::getsockname(m_sink, reinterpret_cast<sockaddr*>(&m_sink_addr), &sink_len);
        m_running.store(true, std::memory_order_release);
        m_worker = std::thread([this]() {
            std::uint8_t buffer[64];
            while (m_running.load(std::memory_order_acquire)) {
                pollfd pfd{};
                pfd.fd = m_sink;
                pfd.events = POLLIN;
                if (::poll(&pfd, 1, 1) <= 0) {
                    continue;
                }
                if (::recv(m_sink, buffer, sizeof(buffer), 0) > 0) {
                    m_received.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    }

    ~UdpLossProbe() {
        m_running.store(false, std::memory_order_release);
        if (m_worker.joinable()) {
            m_worker.join();
        }
        if (m_sink >= 0) {
            ::close(m_sink);
        }
        if (m_probe >= 0) {
            ::close(m_probe);
        }
    }

    UdpLossProbe(const UdpLossProbe&) = delete;
    UdpLossProbe& operator=(const UdpLossProbe&) = delete;

    bool valid() const { return m_sink >= 0 && m_probe >= 0; }

    /// @brief 发 `probes` 个单程包并返回 sink 实际收到的数量。
    int Run(int probes, int gap_us, double settle_ms) {
        std::uint8_t payload[16];
        std::memcpy(payload, "WNET", 4);
        for (int index = 0; index < probes; ++index) {
            const std::uint32_t seq = static_cast<std::uint32_t>(index);
            std::memcpy(payload + 4, &seq, 4);
            (void)::sendto(m_probe, payload, sizeof(payload), 0,
                           reinterpret_cast<const sockaddr*>(&m_sink_addr),
                           sizeof(m_sink_addr));
            ::usleep(static_cast<useconds_t>(gap_us));
        }
        const double deadline = rpctest::NowMicros() + settle_ms * 1000.0;
        while (rpctest::NowMicros() < deadline &&
               m_received.load(std::memory_order_relaxed) < probes) {
            ::usleep(500);
        }
        return m_received.load(std::memory_order_relaxed);
    }

   private:
    int m_sink = -1;
    int m_probe = -1;
    sockaddr_in m_sink_addr{};
    std::atomic<int> m_received{0};
    std::atomic<bool> m_running{false};
    std::thread m_worker;
};

/// @brief 两级校准：单程批量估丢包 + 串行测准确 RTT。
inline Calibration Calibrate(double expected_rtt_ms) {
    Calibration result;
    UdpLossProbe loss_probe;
    if (!loss_probe.valid()) {
        return result;
    }

    const int loss_probes = 400;
    result.loss_sent = loss_probes;
    result.loss_received =
        loss_probe.Run(loss_probes, 1000, expected_rtt_ms * 0.5 + 100.0);
    result.loss_percent =
        (1.0 - static_cast<double>(result.loss_received) /
                   static_cast<double>(result.loss_sent)) *
        100.0;

    UdpEchoPair pair;
    if (!pair.valid()) {
        return result;
    }

    // 串行探测：每次等回包再发下一个，避免批量突发在 netem 队列里的排队偏差。
    const int rtt_probes = 20;
    const double rtt_timeout = std::max(50.0, expected_rtt_ms * 3.0 + 20.0);
    std::vector<double> rtts = pair.SerialRtt(rtt_probes, rtt_timeout);
    result.rtt_probes = static_cast<int>(rtts.size());
    std::vector<double> sorted = rtts;
    std::sort(sorted.begin(), sorted.end());
    if (!sorted.empty()) {
        result.rtt_p50_ms = rpctest::Percentile(sorted, 0.50) / 1000.0;
        result.rtt_p99_ms = rpctest::Percentile(sorted, 0.99) / 1000.0;
    }
    return result;
}

/// @brief 一次协议测量的结果。
struct CallRun {
    bool connected = false;
    int connect_attempts = 0;
    double connect_us = 0.0;
    int attempts = 0;
    int succeeded = 0;
    int failed = 0;
    double wall_us = 0.0;
    std::vector<double> samples_us;
    bool bulk_ok = false;
    double bulk_mb_s = 0.0;
};

/// @brief 弱网测量用的客户端选项（两种传输完全一致）。
inline go2cpp::rpc::ClientOptions ClientOptionsForWeakNet() {
    go2cpp::rpc::ClientOptions options;
    options.connect_timeout = std::chrono::milliseconds(3000);
    options.timeout = std::chrono::milliseconds(8000);
    return options;
}

/// @brief 连续失败多少次判定连接已死（避免整段白跑）。
constexpr int kDeadAfterFailures = 40;

/// @brief 在已注入的网络上跑 TCP 同步调用。
inline CallRun RunTcpCalls(go2cpp::rpc::Server& server, int iterations, int warmup,
                           std::size_t bulk_bytes) {
    CallRun run;
    std::unique_ptr<go2cpp::rpc::Client> client;
    for (int attempt = 1; attempt <= 3; ++attempt) {
        ++run.connect_attempts;
        const double begin = rpctest::NowMicros();
        auto candidate = std::make_unique<go2cpp::rpc::Client>(server.address(),
                                                               ClientOptionsForWeakNet());
        if (candidate->connected()) {
            run.connected = true;
            run.connect_us = rpctest::NowMicros() - begin;
            client = std::move(candidate);
            break;
        }
    }
    if (!client) {
        return run;
    }

    for (int index = 0; index < warmup; ++index) {
        rpctest::User user;
        (void)client->TryCall("User.Get", &user, rpctest::UserId{index});
    }

    int consecutive_failures = 0;
    const double start = rpctest::NowMicros();
    for (int index = 0; index < iterations; ++index) {
        rpctest::User user;
        const double begin = rpctest::NowMicros();
        const go2cpp::rpc::Status status =
            client->TryCall("User.Get", &user, rpctest::UserId{index});
        const double elapsed = rpctest::NowMicros() - begin;
        ++run.attempts;
        if (status.ok() && user.id == index) {
            run.samples_us.push_back(elapsed);
            ++run.succeeded;
            consecutive_failures = 0;
        } else {
            ++run.failed;
            if (++consecutive_failures >= kDeadAfterFailures) {
                break;
            }
        }
    }
    run.wall_us = rpctest::NowMicros() - start;

    if (bulk_bytes > 0 && run.connected) {
        const std::vector<std::uint8_t> payload(bulk_bytes, 0xA5);
        double best_mb_s = 0.0;
        for (int round = 0; round < 3; ++round) {
            std::vector<std::uint8_t> out;
            const double begin = rpctest::NowMicros();
            const go2cpp::rpc::Status status =
                client->TryCall("User.EchoBytes", &out, payload);
            const double elapsed = rpctest::NowMicros() - begin;
            if (status.ok() && out.size() == payload.size() && elapsed > 0.0) {
                const double mb_s =
                    (static_cast<double>(payload.size()) / (1024.0 * 1024.0)) /
                    (elapsed / 1e6);
                best_mb_s = std::max(best_mb_s, mb_s);
            }
        }
        run.bulk_ok = best_mb_s > 0.0;
        run.bulk_mb_s = best_mb_s;
    }
    return run;
}

/// @brief 在已注入的网络上跑 QUIC 同步调用（同一时刻、同一回环）。
inline CallRun RunQuicCalls(go2cpp::rpc::Server& server,
                            const go2cpp::rpc::QuicOptions& options, int iterations,
                            int warmup, std::size_t bulk_bytes,
                            go2cpp::rpc::QuicStats* stats_out) {
    CallRun run;
    QuicServer quic;
    std::string error;
    if (!quic.Start(&server, options, &error)) {
        std::printf("  [weaknet] QUIC listener 启动失败：%s\n", error.c_str());
        return run;
    }
    QuicClientSession session;
    for (int attempt = 1; attempt <= 3; ++attempt) {
        ++run.connect_attempts;
        const double begin = rpctest::NowMicros();
        if (session.Dial(Loopback(quic.port()), options, ClientOptionsForWeakNet(), &error)) {
            run.connected = true;
            run.connect_us = rpctest::NowMicros() - begin;
            break;
        }
        session.client.reset();
        session.quic.reset();
    }
    if (!session.client) {
        quic.listener.Stop();
        return run;
    }

    for (int index = 0; index < warmup; ++index) {
        rpctest::User user;
        (void)session.client->TryCall("User.Get", &user, rpctest::UserId{index});
    }

    int consecutive_failures = 0;
    const double start = rpctest::NowMicros();
    for (int index = 0; index < iterations; ++index) {
        rpctest::User user;
        const double begin = rpctest::NowMicros();
        const go2cpp::rpc::Status status =
            session.client->TryCall("User.Get", &user, rpctest::UserId{index});
        const double elapsed = rpctest::NowMicros() - begin;
        ++run.attempts;
        if (status.ok() && user.id == index) {
            run.samples_us.push_back(elapsed);
            ++run.succeeded;
            consecutive_failures = 0;
        } else {
            ++run.failed;
            if (++consecutive_failures >= kDeadAfterFailures) {
                break;
            }
        }
    }
    run.wall_us = rpctest::NowMicros() - start;

    if (bulk_bytes > 0) {
        const std::vector<std::uint8_t> payload(bulk_bytes, 0xA5);
        double best_mb_s = 0.0;
        for (int round = 0; round < 3; ++round) {
            std::vector<std::uint8_t> out;
            const double begin = rpctest::NowMicros();
            const go2cpp::rpc::Status status =
                session.client->TryCall("User.EchoBytes", &out, payload);
            const double elapsed = rpctest::NowMicros() - begin;
            if (status.ok() && out.size() == payload.size() && elapsed > 0.0) {
                const double mb_s =
                    (static_cast<double>(payload.size()) / (1024.0 * 1024.0)) /
                    (elapsed / 1e6);
                best_mb_s = std::max(best_mb_s, mb_s);
            }
        }
        run.bulk_ok = best_mb_s > 0.0;
        run.bulk_mb_s = best_mb_s;
    }
    if (stats_out != nullptr && session.quic) {
        *stats_out = session.quic->stats();
    }
    session.client.reset();
    session.quic.reset();
    quic.listener.Stop();
    return run;
}

/// @brief 一行结果（用于汇总比值）。
struct Row {
    std::string scenario;
    std::string proto;
    CallRun run;
};

/// @brief 打印一行表格。
inline void PrintRow(const Row& row) {
    const rpctest::LatencySummary summary = rpctest::Summarize(row.run.samples_us);
    const double qps = row.run.wall_us > 0.0
                           ? static_cast<double>(row.run.succeeded) * 1e6 / row.run.wall_us
                           : 0.0;
    std::printf("| %s | %s | %d/%d | %.0f | %.0f | %.0f | %.0f | %.2f |\n",
                row.scenario.c_str(), row.proto.c_str(), row.run.succeeded,
                row.run.attempts, summary.p50_us, summary.p99_us, summary.max_us, qps,
                row.run.bulk_ok ? row.run.bulk_mb_s : 0.0);
    std::fflush(stdout);
}

/// @brief 打印两协议比值（QUIC / TCP；>1 表示 QUIC 更差）。
inline void PrintRatios(const Row& tcp, const Row& quic) {
    const rpctest::LatencySummary tcp_summary = rpctest::Summarize(tcp.run.samples_us);
    const rpctest::LatencySummary quic_summary = rpctest::Summarize(quic.run.samples_us);
    const double p50_ratio = tcp_summary.p50_us > 0.0 ? quic_summary.p50_us / tcp_summary.p50_us
                                                      : 0.0;
    const double p99_ratio = tcp_summary.p99_us > 0.0 ? quic_summary.p99_us / tcp_summary.p99_us
                                                      : 0.0;
    std::printf("  -> %s：QUIC/TCP p50 = %.2fx，p99 = %.2fx\n", tcp.scenario.c_str(),
                p50_ratio, p99_ratio);
    std::fflush(stdout);
}

/// @brief 弱网公平对照主流程。
inline void WeakNetCompare(go2cpp::rpc::Server& server, int iterations) {
    rpctest::Announce("weaknet: 同一内核 netem 注入器下 QUIC vs TCP 公平对照");
    if (!NetemController::Available()) {
        std::printf("  跳过：需要 root（CAP_NET_ADMIN）与 iproute2 tc，"
                    "请用 `wsl -u root` 运行\n");
        return;
    }
    const std::vector<Scenario> scenarios = ScenarioList();
    const int warmup = 20;
    const std::size_t bulk_bytes = 512u * 1024u;
    std::vector<Row> rows;
    rows.reserve(scenarios.size() * 2);

    go2cpp::rpc::QuicOptions quic_options;
    quic_options.streams = 1;
    quic_options.mode = go2cpp::rpc::QuicMode::kLowLatency;
    quic_options.connect_timeout_ms = 3000;
    quic_options.send_timeout_ms = 8000;
    quic_options.peer_timeout_ms = 30000;

    std::printf("- 迭代：每场景每协议 %d 次同步 RPC（warmup %d，批量载荷 %zu KiB）\n",
                iterations, warmup, bulk_bytes / 1024u);
    std::printf("- 注入：单个 `tc qdisc ... dev lo root netem`，TCP 段与 QUIC 数据报"
                "走同一 qdisc\n");
    std::printf("- 机器：nproc=%ld，回环，QUIC 默认 kLowLatency（无 BBR/pacing）\n",
                ::sysconf(_SC_NPROCESSORS_ONLN));

    for (std::size_t index = 0; index < scenarios.size(); ++index) {
        const Scenario& scenario = scenarios[index];
        NetemController netem;
        std::string error;
        if (!netem.Apply(scenario, &error)) {
            std::printf("\n### %s：netem 应用失败，跳过（%s）\n", scenario.label,
                        error.c_str());
            continue;
        }
        const Calibration calib = Calibrate(ExpectedRttMs(scenario));
        std::printf("\n### %s\n", scenario.label);
        std::printf("  netem : %s\n",
                    netem.command().empty() ? "（无，已移除 qdisc）" : netem.command().c_str());
        std::printf("  校准  : 实测单程丢包 %.2f%%（%d/%d 单程收包），串行 RTT p50=%.2fms "
                    "p99=%.2fms（%d 个样本）\n",
                    calib.loss_percent, calib.loss_received, calib.loss_sent,
                    calib.rtt_p50_ms, calib.rtt_p99_ms, calib.rtt_probes);
        std::fflush(stdout);

        // ABBA：偶数场景 TCP 先，奇数场景 QUIC 先，抵消两者的顺序漂移。
        const bool tcp_first = (index % 2U) == 0U;
        Row tcp_row;
        Row quic_row;
        auto run_tcp = [&]() {
            tcp_row.scenario = scenario.label;
            tcp_row.proto = "TCP";
            tcp_row.run = RunTcpCalls(server, iterations, warmup, bulk_bytes);
        };
        auto run_quic = [&]() {
            quic_row.scenario = scenario.label;
            quic_row.proto = "QUIC";
            go2cpp::rpc::QuicStats stats;
            quic_row.run =
                RunQuicCalls(server, quic_options, iterations, warmup, bulk_bytes, &stats);
            std::printf("  QUIC 统计：重传 %llu，srtt=%llums，pto=%llums，"
                        "帧 %llu/%llu\n",
                        static_cast<unsigned long long>(stats.retransmits),
                        static_cast<unsigned long long>(stats.srtt_ms),
                        static_cast<unsigned long long>(stats.pto_ms),
                        static_cast<unsigned long long>(stats.frames_sent),
                        static_cast<unsigned long long>(stats.frames_received));
        };
        if (tcp_first) {
            run_tcp();
            run_quic();
        } else {
            run_quic();
            run_tcp();
        }
        std::printf("  顺序  : %s；TCP 建连 %.2fms（%d 次尝试）| QUIC 建连 %.2fms"
                    "（%d 次尝试）\n",
                    tcp_first ? "TCP→QUIC" : "QUIC→TCP", tcp_row.run.connect_us / 1000.0,
                    tcp_row.run.connect_attempts, quic_row.run.connect_us / 1000.0,
                    quic_row.run.connect_attempts);
        PrintRow(tcp_row);
        PrintRow(quic_row);
        PrintRatios(tcp_row, quic_row);
        rows.push_back(std::move(tcp_row));
        rows.push_back(std::move(quic_row));
    }

    // 结尾总表（markdown，可直接粘进报告）。
    std::printf("\n| 场景 | 协议 | 成功/尝试 | p50 (µs) | p99 (µs) | max (µs) | QPS | "
                "512KiB 单向 (MB/s) |\n");
    std::printf("|---|---|---|---|---|---|---|---|\n");
    for (const Row& row : rows) {
        PrintRow(row);
    }
    std::printf("\n");
}

}  // namespace weaknet
