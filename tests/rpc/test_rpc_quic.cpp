/**
 * @file test_rpc_quic.cpp
 * @brief RPC over QUIC 风格传输的端到端/多路复用/0-RTT/丢包/性能测试。
 *
 * @details
 * 覆盖：
 * 1. 端到端：Get/Add/EchoBytes(1MiB)/Fail/流式 Watch；
 * 2. 多流复用：一个 QuicClient（一个 UDP 五元组）上 4 条 stream 各跑一个
 *    rpc::Client，并发调用互不串号；
 * 3. call 多路复用：单条 stream 上 8 线程并发调用（RPC 层 call_id 复用）；
 * 4. 1-RTT 加密（PSK + X25519）与 0-RTT 票据恢复（纯 PSK）；
 * 5. 丢包/乱序：内置 UDP relay 模拟 1%/5% 丢包与随机抖动；
 * 6. 性能：QUIC vs TCP 的同步调用 p50/p99（`RPC_QUIC_BENCH_ITERS` 控制规模，
 *    默认 2000 次；完整 1e6 用环境变量跑）；传输层 1 字节 echo 下限。
 *
 * 环境变量：
 * - `RPC_QUIC_BENCH_ITERS`：RPC 基准次数（默认 2000，0 = 跳过）；
 * - `RPC_QUIC_ECHO_ITERS`：传输层 echo 次数（默认 5000）。
 *
 * @author go2cpp contributors
 * @date 2026-11-01
 * @copyright MIT License
 */

#include "rpc_test_support.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "go2cpp/rpc/client.hpp"
#include "go2cpp/rpc/server.hpp"
#include "go2cpp/rpc/transport_quic.hpp"

namespace {

using namespace go2cpp;
using namespace rpctest;
namespace rpc = go2cpp::rpc;

/// @brief 本机回环端点。
rpc::Endpoint Loopback(std::uint16_t port) {
    rpc::Endpoint endpoint;
    endpoint.host = "127.0.0.1";
    endpoint.port = port;
    return endpoint;
}

/// @brief 读取环境变量整数（**允许 0**，用于显式跳过某个阶段）。
/// @note 测试支撑里的 `EnvInt` 把 0 当作“未设置”回落默认值，而基准阶段需要
///       “0 = 跳过”，因此这里单独实现。
std::int64_t EnvIntAllowZero(const char* name, std::int64_t fallback) {
    const char* text = std::getenv(name);
    if (text == nullptr || *text == '\0') {
        return fallback;
    }
    char* end = nullptr;
    const long long value = std::strtoll(text, &end, 10);
    if (end == text || value < 0) {
        return fallback;
    }
    return static_cast<std::int64_t>(value);
}

/// @brief 读取布尔环境变量："0"/空 = false，其余非空 = true。
bool EnvFlag(const char* name, bool fallback) {
    const char* text = std::getenv(name);
    if (text == nullptr || *text == '\0') {
        return fallback;
    }
    return std::strcmp(text, "0") != 0;
}

/// @brief 构造 32B PSK（内容固定，便于复现）。
std::vector<std::uint8_t> MakePsk(std::uint8_t seed) {
    std::vector<std::uint8_t> psk(32, 0);
    for (std::size_t index = 0; index < psk.size(); ++index) {
        psk[index] = static_cast<std::uint8_t>((index * 73U + seed) & 0xFFU);
    }
    return psk;
}

/// @brief 测试服务端：一个 RPC Server（同时保留 TCP 监听供对照基准）。
struct TestServer {
    rpctest::UserService service;
    rpc::Server server;
    std::string address;

    TestServer() : server(rpc::ServerOptions{}) {}

    bool Start() {
        if (!server.Register("User", &service)) {
            return false;
        }
        if (!server.Listen(Loopback(0))) {
            return false;
        }
        address = "127.0.0.1:" + std::to_string(server.port());
        return true;
    }
};

/// @brief 一个 QUIC 监听器 + “新 stream 直接交给 RPC Server”的绑定。
struct QuicServer {
    rpc::QuicListener listener;
    rpc::Server* server = nullptr;

    bool Start(rpc::Server* target, const rpc::QuicOptions& options, std::string* error) {
        server = target;
        rpc::Status status;
        if (!listener.Listen(Loopback(0), options, &status)) {
            if (error != nullptr) {
                *error = status.ToString();
            }
            return false;
        }
        listener.OnStream([this](std::unique_ptr<rpc::QuicStream> stream) {
            if (server != nullptr) {
                (void)server->AddConnection(std::move(stream));
            }
        });
        if (!listener.Start()) {
            if (error != nullptr) {
                *error = "listener start failed";
            }
            return false;
        }
        return true;
    }

    std::uint16_t port() const { return listener.port(); }
};

/// @brief 一个 QUIC 客户端连接 + 由它派生的 RPC Client。
struct QuicClientSession {
    std::unique_ptr<rpc::QuicClient> quic;
    std::unique_ptr<rpc::Client> client;
    std::uint8_t channel = 0;

    bool Dial(const rpc::Endpoint& target, const rpc::QuicOptions& options,
              const rpc::ClientOptions& client_options, std::string* error) {
        rpc::Status status;
        quic = rpc::QuicClient::Dial(target, options, &status);
        if (!quic) {
            if (error != nullptr) {
                *error = status.ToString();
            }
            return false;
        }
        auto stream = quic->OpenStream(&status);
        if (!stream) {
            if (error != nullptr) {
                *error = status.ToString();
            }
            return false;
        }
        channel = stream->channel();
        client = std::make_unique<rpc::Client>(std::move(stream), client_options);
        return true;
    }
};

// 弱网公平对照：同一个内核 netem 注入器同时作用于 UDP 与 TCP。
// 必须放在 QuicServer/QuicClientSession 之后（依赖两者），且只在 root 下启用。
#include "weaknet_compare.hpp"

// ---------------------------------------------------------------------------
// UDP 丢包/乱序 relay（测试用；不改 Bento）
// ---------------------------------------------------------------------------

/// @brief 双向 UDP relay：按概率丢包，按随机微秒数延迟（制造乱序）。
class LossyRelay {
   public:
    LossyRelay(std::uint16_t server_port, int drop_percent, int jitter_us)
        : m_drop_percent(drop_percent), m_jitter_us(jitter_us), m_rng(0x5EED) {
        m_front = ::socket(AF_INET, SOCK_DGRAM, 0);
        m_back = ::socket(AF_INET, SOCK_DGRAM, 0);
        sockaddr_in bind_addr{};
        bind_addr.sin_family = AF_INET;
        bind_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        bind_addr.sin_port = 0;
        ::bind(m_front, reinterpret_cast<sockaddr*>(&bind_addr), sizeof(bind_addr));
        ::bind(m_back, reinterpret_cast<sockaddr*>(&bind_addr), sizeof(bind_addr));
        socklen_t length = sizeof(m_front_addr);
        ::getsockname(m_front, reinterpret_cast<sockaddr*>(&m_front_addr), &length);
        m_server.sin_family = AF_INET;
        m_server.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        m_server.sin_port = htons(server_port);
        m_thread = std::thread([this]() { Loop(); });
    }

    ~LossyRelay() {
        m_running.store(false, std::memory_order_release);
        if (m_thread.joinable()) {
            m_thread.join();
        }
        ::close(m_front);
        ::close(m_back);
    }

    std::uint16_t port() const { return ntohs(m_front_addr.sin_port); }

   private:
    struct Pending {
        std::vector<std::uint8_t> data;
        sockaddr_in to{};
        std::uint64_t due_us = 0;
    };

    void Loop() {
        std::vector<std::uint8_t> buffer(65536);
        while (m_running.load(std::memory_order_acquire)) {
            pollfd fds[2];
            fds[0].fd = m_front;
            fds[0].events = POLLIN;
            fds[1].fd = m_back;
            fds[1].events = POLLIN;
            const int ready = ::poll(fds, 2, 1);
            if (ready > 0) {
                if ((fds[0].revents & POLLIN) != 0) {
                    sockaddr_in from{};
                    socklen_t length = sizeof(from);
                    const ssize_t bytes =
                        ::recvfrom(m_front, buffer.data(), buffer.size(), 0,
                                   reinterpret_cast<sockaddr*>(&from), &length);
                    if (bytes > 0) {
                        m_client = from;
                        m_have_client = true;
                        Forward(buffer.data(), static_cast<std::size_t>(bytes), m_server);
                    }
                }
                if ((fds[1].revents & POLLIN) != 0) {
                    const ssize_t bytes = ::recv(m_back, buffer.data(), buffer.size(), 0);
                    if (bytes > 0 && m_have_client) {
                        Forward(buffer.data(), static_cast<std::size_t>(bytes), m_client);
                    }
                }
            }
            FlushPending();
        }
    }

    void Forward(const std::uint8_t* data, std::size_t bytes, const sockaddr_in& to) {
        std::uniform_int_distribution<int> chance(0, 99);
        if (chance(m_rng) < m_drop_percent) {
            return;
        }
        Pending pending;
        pending.data.assign(data, data + bytes);
        pending.to = to;
        std::uniform_int_distribution<int> jitter(0, m_jitter_us);
        pending.due_us = NowUs() + static_cast<std::uint64_t>(jitter(m_rng));
        m_pending.push_back(std::move(pending));
    }

    void FlushPending() {
        const std::uint64_t now = NowUs();
        std::size_t kept = 0;
        for (std::size_t index = 0; index < m_pending.size(); ++index) {
            Pending& pending = m_pending[index];
            if (pending.due_us <= now) {
                const int fd = (pending.to.sin_port == m_server.sin_port) ? m_back : m_front;
                ::sendto(fd, pending.data.data(), pending.data.size(), 0,
                         reinterpret_cast<const sockaddr*>(&pending.to), sizeof(pending.to));
            } else {
                if (kept != index) {
                    m_pending[kept] = std::move(pending);
                }
                ++kept;
            }
        }
        m_pending.resize(kept);
    }

    static std::uint64_t NowUs() {
        return static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now().time_since_epoch())
                .count());
    }

    int m_front = -1;
    int m_back = -1;
    sockaddr_in m_front_addr{};
    sockaddr_in m_server{};
    sockaddr_in m_client{};
    bool m_have_client = false;
    int m_drop_percent = 0;
    int m_jitter_us = 0;
    std::mt19937 m_rng;
    std::deque<Pending> m_pending;
    std::atomic<bool> m_running{true};
    std::thread m_thread;
};

// ---------------------------------------------------------------------------
// 1. 端到端
// ---------------------------------------------------------------------------

void TestBasic(rpc::Server& server) {
    Announce("basic: QUIC 端到端（Get/Add/EchoBytes 1MiB/Fail/Watch）");
    QuicServer quic;
    rpc::QuicOptions options;
    options.streams = 1;
    std::string error;
    RPC_CHECK(quic.Start(&server, options, &error));
    if (!quic.listener.running()) {
        std::printf("  启动失败：%s\n", error.c_str());
        return;
    }
    QuicClientSession session;
    RPC_CHECK(session.Dial(Loopback(quic.port()), options, rpc::ClientOptions{}, &error));
    RPC_CHECK(session.client != nullptr);
    if (!session.client) {
        return;
    }
    RPC_CHECK(session.client->connected());

    User user;
    RPC_CHECK(session.client->TryCall("User.Get", &user, UserId{42}).ok());
    RPC_CHECK_EQ(user.id, 42);
    RPC_CHECK_EQ(user.name, std::string("user-42"));

    std::int64_t sum = 0;
    RPC_CHECK(session.client->TryCall("User.Add", &sum, std::int64_t{7}, std::int64_t{35}).ok());
    RPC_CHECK_EQ(sum, 42);

    std::vector<std::uint8_t> big(1024 * 1024);
    for (std::size_t index = 0; index < big.size(); ++index) {
        big[index] = static_cast<std::uint8_t>(index * 31U);
    }
    std::vector<std::uint8_t> echoed;
    RPC_CHECK(session.client->TryCall("User.EchoBytes", &echoed, big).ok());
    RPC_CHECK_EQ(echoed.size(), big.size());
    RPC_CHECK(echoed == big);

    const rpc::Status failure = session.client->TryCall("User.Fail", &user, UserId{1});
    RPC_CHECK_EQ(static_cast<int>(failure.code), static_cast<int>(rpc::StatusCode::kPermissionDenied));

    auto stream = session.client->OpenStream("User.Watch", WatchRequest{5, 0});
    RPC_CHECK(stream.valid());
    std::int32_t received = 0;
    ChatMessage message;
    rpc::Status stream_status;
    while (stream.Next(&message, &stream_status)) {
        RPC_CHECK_EQ(message.seq, received);
        ++received;
    }
    RPC_CHECK_EQ(received, 5);
    RPC_CHECK(stream_status.ok());

    const rpc::QuicStats stats = session.quic->stats();
    std::printf("  streams=%llu frames_sent=%llu frames_recv=%llu bytes=%llu/%llu\n",
                static_cast<unsigned long long>(stats.streams_opened),
                static_cast<unsigned long long>(stats.frames_sent),
                static_cast<unsigned long long>(stats.frames_received),
                static_cast<unsigned long long>(stats.bytes_sent),
                static_cast<unsigned long long>(stats.bytes_received));
    RPC_CHECK(stats.streams_opened >= 1);
    RPC_CHECK(stats.bytes_sent >= big.size());
    RPC_CHECK(stats.bytes_received >= big.size());
    session.client.reset();
    session.quic.reset();
    quic.listener.Stop();
}

// ---------------------------------------------------------------------------
// 2. 多流复用：一个 UDP 连接上 4 条 stream
// ---------------------------------------------------------------------------

void TestStreamMultiplex(rpc::Server& server) {
    Announce("multiplex: 一个 QuicClient（同一五元组）上 4 条 stream 并发");
    QuicServer quic;
    rpc::QuicOptions options;
    options.streams = 4;
    std::string error;
    RPC_CHECK(quic.Start(&server, options, &error));
    rpc::Status status;
    auto quic_client = rpc::QuicClient::Dial(Loopback(quic.port()), options, &status);
    RPC_CHECK(quic_client != nullptr);
    if (!quic_client) {
        std::printf("  Dial 失败：%s\n", status.ToString().c_str());
        return;
    }
    const std::uint16_t local_port = quic_client->local_port();
    std::vector<std::unique_ptr<rpc::Client>> clients;
    std::vector<std::uint8_t> channels;
    for (int index = 0; index < 4; ++index) {
        auto stream = quic_client->OpenStream(&status);
        RPC_CHECK(stream != nullptr);
        if (!stream) {
            break;
        }
        channels.push_back(stream->channel());
        clients.push_back(std::make_unique<rpc::Client>(std::move(stream), rpc::ClientOptions{}));
    }
    RPC_CHECK_EQ(clients.size(), static_cast<std::size_t>(4));
    std::sort(channels.begin(), channels.end());
    RPC_CHECK(channels == std::vector<std::uint8_t>({0, 1, 2, 3}));

    std::atomic<int> failures{0};
    std::vector<std::thread> threads;
    for (int index = 0; index < static_cast<int>(clients.size()); ++index) {
        threads.emplace_back([&clients, &failures, index]() {
            for (int round = 0; round < 300; ++round) {
                const std::int64_t id = static_cast<std::int64_t>(index) * 100000 + round;
                User user;
                if (!clients[static_cast<std::size_t>(index)]
                         ->TryCall("User.Get", &user, UserId{id})
                         .ok() ||
                    user.id != id) {
                    failures.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    }
    for (auto& thread : threads) {
        thread.join();
    }
    RPC_CHECK_EQ(failures.load(), 0);
    RPC_CHECK_EQ(quic_client->stats().streams_opened, static_cast<std::uint64_t>(4));
    std::printf("  streams=4 local_port=%u channels=%u/%u/%u/%u failures=%d\n",
                static_cast<unsigned>(local_port), channels[0], channels[1], channels[2],
                channels[3], failures.load());
    clients.clear();
    quic_client->Close();
    quic_client.reset();
    quic.listener.Stop();
}

// ---------------------------------------------------------------------------
// 3. call_id 多路复用：单条 stream 上 8 线程并发
// ---------------------------------------------------------------------------

void TestCallMultiplex(rpc::Server& server) {
    Announce("multiplex: 单条 stream 上 8 线程并发调用（call_id 复用）");
    QuicServer quic;
    rpc::QuicOptions options;
    options.streams = 1;
    std::string error;
    RPC_CHECK(quic.Start(&server, options, &error));
    QuicClientSession session;
    RPC_CHECK(session.Dial(Loopback(quic.port()), options, rpc::ClientOptions{}, &error));
    if (!session.client) {
        return;
    }
    std::atomic<int> failures{0};
    std::vector<std::thread> threads;
    for (int worker = 0; worker < 8; ++worker) {
        threads.emplace_back([&session, &failures, worker]() {
            for (int round = 0; round < 300; ++round) {
                const std::int64_t id = static_cast<std::int64_t>(worker) * 100000 + round;
                User user;
                if (!session.client->TryCall("User.Get", &user, UserId{id}).ok() ||
                    user.id != id) {
                    failures.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    }
    for (auto& thread : threads) {
        thread.join();
    }
    RPC_CHECK_EQ(failures.load(), 0);
    std::printf("  8 线程 × 300 次：failures=%d\n", failures.load());
    session.client.reset();
    session.quic.reset();
    quic.listener.Stop();
}

// ---------------------------------------------------------------------------
// 4. 加密：1-RTT（PSK + X25519）与 0-RTT（纯 PSK 票据）
// ---------------------------------------------------------------------------

void TestEncryption(rpc::Server& server) {
    Announce("security: 1-RTT PSK + X25519 加密调用");
    QuicServer quic;
    rpc::QuicOptions options;
    options.streams = 1;
    options.pre_shared_key = MakePsk(0x11);
    options.enable_x25519 = true;
    std::string error;
    RPC_CHECK(quic.Start(&server, options, &error));
    QuicClientSession session;
    RPC_CHECK(session.Dial(Loopback(quic.port()), options, rpc::ClientOptions{}, &error));
    if (!session.client) {
        std::printf("  Dial 失败：%s\n", error.c_str());
        return;
    }
    User user;
    RPC_CHECK(session.client->TryCall("User.Get", &user, UserId{7}).ok());
    RPC_CHECK_EQ(user.id, 7);
    // X25519 会话不允许导出 0-RTT 票据（Bento 的既定策略）。
    RPC_CHECK(session.quic->session_ticket().empty());
    session.client.reset();
    session.quic.reset();
    quic.listener.Stop();
}

void TestZeroRtt(rpc::Server& server) {
    Announce("security: 0-RTT 票据恢复（纯 PSK，无 HELLO）");
    QuicServer quic;
    rpc::QuicOptions options;
    options.streams = 1;
    options.pre_shared_key = MakePsk(0x29);
    options.enable_x25519 = false;
    options.enable_0rtt = true;
    std::string error;
    RPC_CHECK(quic.Start(&server, options, &error));

    QuicClientSession first;
    RPC_CHECK(first.Dial(Loopback(quic.port()), options, rpc::ClientOptions{}, &error));
    if (!first.client) {
        std::printf("  首次 Dial 失败：%s\n", error.c_str());
        return;
    }
    User user;
    RPC_CHECK(first.client->TryCall("User.Get", &user, UserId{11}).ok());
    const std::vector<std::uint8_t> ticket = first.quic->session_ticket();
    RPC_CHECK(!ticket.empty());
    const std::uint64_t handshakes_before = quic.listener.stats().handshakes;
    const std::uint64_t resumptions_before = quic.listener.stats().resumptions;
    std::printf("  ticket=%zuB handshakes=%llu\n", ticket.size(),
                static_cast<unsigned long long>(handshakes_before));

    // 关闭旧连接（不发 FIN；服务端 30s 内仍保留会话密钥，正是 0-RTT 的前提）。
    first.client.reset();
    first.quic.reset();

    rpc::QuicOptions resumed_options = options;
    resumed_options.session_ticket = ticket;
    QuicClientSession second;
    RPC_CHECK(second.Dial(Loopback(quic.port()), resumed_options, rpc::ClientOptions{}, &error));
    if (!second.client) {
        std::printf("  恢复 Dial 失败：%s\n", error.c_str());
        return;
    }
    RPC_CHECK(second.quic->resumed());
    RPC_CHECK(second.client->TryCall("User.Get", &user, UserId{22}).ok());
    RPC_CHECK_EQ(user.id, 22);
    const rpc::QuicStats after = quic.listener.stats();
    RPC_CHECK_EQ(after.handshakes, handshakes_before);  // 没有新的 1-RTT 握手
    RPC_CHECK(after.resumptions > resumptions_before);
    std::printf("  resumed=%d handshakes=%llu resumptions=%llu\n",
                second.quic->resumed() ? 1 : 0,
                static_cast<unsigned long long>(after.handshakes),
                static_cast<unsigned long long>(after.resumptions));
    second.client.reset();
    second.quic.reset();
    quic.listener.Stop();
}

// ---------------------------------------------------------------------------
// 5. 丢包 / 乱序
// ---------------------------------------------------------------------------

struct LossResult {
    int failures = 0;
    int samples = 0;
    double p50_us = 0.0;
    double p99_us = 0.0;
    double mean_us = 0.0;
};

LossResult RunLossTrial(rpc::Server& server, int drop_percent, int jitter_us, int iterations) {
    LossResult result;
    QuicServer quic;
    rpc::QuicOptions options;
    options.streams = 1;
    options.peer_timeout_ms = 30000;
    std::string error;
    if (!quic.Start(&server, options, &error)) {
        result.failures = iterations;
        return result;
    }
    LossyRelay relay(quic.port(), drop_percent, jitter_us);
    rpc::ClientOptions client_options;
    client_options.timeout = std::chrono::seconds(5);
    QuicClientSession session;
    if (!session.Dial(Loopback(relay.port()), options, client_options, &error)) {
        result.failures = iterations;
        quic.listener.Stop();
        return result;
    }
    std::vector<double> samples;
    samples.reserve(static_cast<std::size_t>(iterations));
    for (int index = 0; index < iterations; ++index) {
        const double begin = NowMicros();
        User user;
        const rpc::Status status = session.client->TryCall("User.Get", &user, UserId{index});
        const double elapsed = NowMicros() - begin;
        if (!status.ok() || user.id != index) {
            ++result.failures;
            continue;
        }
        samples.push_back(elapsed);
    }
    const LatencySummary summary = Summarize(samples);
    result.samples = static_cast<int>(samples.size());
    result.p50_us = summary.p50_us;
    result.p99_us = summary.p99_us;
    result.mean_us = summary.mean_us;
    std::printf("  drop=%d%% jitter=%dus：成功 %d/%d，p50=%.1fus p99=%.1fus mean=%.1fus\n",
                drop_percent, jitter_us, result.samples, iterations, summary.p50_us,
                summary.p99_us, summary.mean_us);
    session.client.reset();
    session.quic.reset();
    quic.listener.Stop();
    return result;
}

void TestLossy(rpc::Server& server, int iterations) {
    Announce("loss: 1%/5% 丢包 + 随机乱序下的可靠性与延迟");
    const LossResult clean = RunLossTrial(server, 0, 0, iterations);
    const LossResult one = RunLossTrial(server, 1, 500, iterations);
    const LossResult five = RunLossTrial(server, 5, 2000, iterations);
    RPC_CHECK_EQ(clean.failures, 0);
    RPC_CHECK_EQ(one.failures, 0);
    RPC_CHECK_EQ(five.failures, 0);
    std::printf("  summary: clean p50=%.1f | 1%% p50=%.1f p99=%.1f | 5%% p50=%.1f p99=%.1f\n",
                clean.p50_us, one.p50_us, one.p99_us, five.p50_us, five.p99_us);
}

// ---------------------------------------------------------------------------
// 6. 性能：QUIC vs TCP + 传输层 echo 下限
// ---------------------------------------------------------------------------

struct BenchResult {
    double p50_us = 0.0;
    double p90_us = 0.0;
    double p99_us = 0.0;
    double mean_us = 0.0;
    double throughput = 0.0;
    double wall_us = 0.0;
    int samples = 0;
};

BenchResult SummarizeBench(const std::vector<double>& samples, double wall_us) {
    BenchResult result;
    const LatencySummary summary = Summarize(samples);
    result.p50_us = summary.p50_us;
    result.p90_us = summary.p90_us;
    result.p99_us = summary.p99_us;
    result.mean_us = summary.mean_us;
    result.samples = static_cast<int>(samples.size());
    result.wall_us = wall_us;
    if (wall_us > 0.0) {
        result.throughput = static_cast<double>(samples.size()) * 1e6 / wall_us;
    }
    return result;
}

void BenchRpcOverTcp(rpc::Server& server, int iterations, BenchResult* out,
                     std::vector<double>* samples_out = nullptr) {
    rpc::Client client(server.address());
    if (!client.connected()) {
        return;
    }
    std::vector<double> samples;
    samples.reserve(static_cast<std::size_t>(iterations));
    const double start = NowMicros();
    for (int index = 0; index < iterations; ++index) {
        User user;
        const double begin = NowMicros();
        const rpc::Status status = client.TryCall("User.Get", &user, UserId{index});
        const double elapsed = NowMicros() - begin;
        if (status.ok() && user.id == index) {
            samples.push_back(elapsed);
        }
    }
    const double wall = NowMicros() - start;
    *out = SummarizeBench(samples, wall);
    if (samples_out != nullptr) {
        *samples_out = std::move(samples);
    }
}

void BenchRpcOverQuic(rpc::Server& server, int iterations, rpc::QuicMode mode, BenchResult* out,
                      rpc::QuicStats* stats_out, bool tuned = true,
                      std::vector<double>* samples_out = nullptr) {
    QuicServer quic;
    rpc::QuicOptions options;
    options.streams = 1;
    options.mode = mode;
    // tuned=true：接收线程忙轮询 + 客户端 inline 接收 + 读自旋，回环延迟最低；
    // tuned=false：Bento 默认阻塞模型，用于隔离“配置差异”与“协议差异”。
    options.busy_poll = tuned;
    options.read_spin_us = tuned ? 30 : 0;
    options.inline_receive = tuned;
    std::string error;
    if (!quic.Start(&server, options, &error)) {
        return;
    }
    QuicClientSession session;
    if (!session.Dial(Loopback(quic.port()), options, rpc::ClientOptions{}, &error)) {
        quic.listener.Stop();
        return;
    }
    std::vector<double> samples;
    samples.reserve(static_cast<std::size_t>(iterations));
    std::vector<double> segment_p50;
    std::size_t segment_start = 0;
    const int segment_size = 100000;
    const double start = NowMicros();
    for (int index = 0; index < iterations; ++index) {
        User user;
        const double begin = NowMicros();
        const rpc::Status status = session.client->TryCall("User.Get", &user, UserId{index});
        const double elapsed = NowMicros() - begin;
        if (status.ok() && user.id == index) {
            samples.push_back(elapsed);
        }
        if (segment_size > 0 && (index + 1) % segment_size == 0) {
            const std::vector<double> slice(samples.begin() + static_cast<std::ptrdiff_t>(segment_start),
                                            samples.end());
            segment_p50.push_back(Summarize(slice).p50_us);
            segment_start = samples.size();
        }
    }
    const double wall = NowMicros() - start;
    *out = SummarizeBench(samples, wall);
    if (stats_out != nullptr) {
        *stats_out = session.quic->stats();
    }
    if (!segment_p50.empty()) {
        std::printf("- QUIC 分段 p50（每 %d 次）：", segment_size);
        for (const double value : segment_p50) {
            std::printf(" %.1f", value);
        }
        std::printf("\n");
    }
    if (samples_out != nullptr) {
        *samples_out = std::move(samples);
    }
    session.client.reset();
    session.quic.reset();
    quic.listener.Stop();
}

/// @brief 传输层 1 字节 echo：只有 QuicStream 的 Read/Write，无 RPC 解析。
BenchResult BenchTransportEcho(int iterations) {
    rpc::QuicListener echo;
    rpc::QuicOptions options;
    options.streams = 1;
    options.busy_poll = true;
    rpc::Status status;
    if (!echo.Listen(Loopback(0), options, &status)) {
        return {};
    }
    // 每个 stream 一个 detached echo 线程：读 1 字节写回，直到 Read 返回 0。
    echo.OnStream([](std::unique_ptr<rpc::QuicStream> stream) {
        std::thread([owned = std::move(stream)]() mutable {
            std::uint8_t byte = 0;
            while (true) {
                const std::ptrdiff_t read = owned->Read(&byte, 1, nullptr);
                if (read <= 0) {
                    break;
                }
                if (!owned->WriteAll(&byte, 1, nullptr)) {
                    break;
                }
            }
        }).detach();
    });
    if (!echo.Start()) {
        return {};
    }
    rpc::QuicOptions client_options;
    client_options.streams = 1;
    client_options.busy_poll = true;
    client_options.inline_receive = true;
    auto quic = rpc::QuicClient::Dial(Loopback(echo.port()), client_options, &status);
    if (!quic) {
        echo.Stop();
        return {};
    }
    auto stream = quic->OpenStream(&status);
    if (!stream) {
        echo.Stop();
        return {};
    }
    std::vector<double> samples;
    samples.reserve(static_cast<std::size_t>(iterations));
    const double start = NowMicros();
    std::uint8_t byte = 0x5A;
    for (int index = 0; index < iterations; ++index) {
        const double begin = NowMicros();
        if (!stream->WriteAll(&byte, 1, nullptr)) {
            break;
        }
        if (stream->Read(&byte, 1, nullptr) != 1) {
            break;
        }
        samples.push_back(NowMicros() - begin);
    }
    const double wall = NowMicros() - start;
    stream.reset();
    quic->Close();
    quic.reset();
    echo.Stop();
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    return SummarizeBench(samples, wall);
}

// ---------------------------------------------------------------------------
// 1e7 级吞吐压测：单向 STREAM 消息（无 RPC 解析，纯传输数据面）
// ---------------------------------------------------------------------------

struct ThroughputResult {
    int messages = 0;
    double wall_ms = 0.0;
    double msg_per_sec = 0.0;
    double mb_per_sec = 0.0;
    std::uint64_t received_bytes = 0;
};

/// @brief 客户端连续 WriteAll 定长消息，服务端读出并计数。
/// @note 这是数据面吞吐：每条消息一条 Bento STREAM 帧（8B 载荷 + 24B 头），
///       发送窗口 256 条，窗口由对端 ACK 推进，因此不会无界灌包。
ThroughputResult BenchThroughput(int messages, std::size_t payload_bytes) {
    ThroughputResult result;
    rpc::QuicListener sink;
    rpc::QuicOptions options;
    options.streams = 1;
    rpc::Status status;
    if (!sink.Listen(Loopback(0), options, &status)) {
        return result;
    }
    std::atomic<std::uint64_t> received{0};
    sink.OnStream([&received](std::unique_ptr<rpc::QuicStream> stream) {
        std::thread([owned = std::move(stream), &received]() mutable {
            std::vector<std::uint8_t> buffer(256 * 1024);
            while (true) {
                const std::ptrdiff_t read = owned->Read(buffer.data(), buffer.size(), nullptr);
                if (read <= 0) {
                    break;
                }
                received.fetch_add(static_cast<std::uint64_t>(read),
                                   std::memory_order_relaxed);
            }
        }).detach();
    });
    if (!sink.Start()) {
        return result;
    }
    auto quic = rpc::QuicClient::Dial(Loopback(sink.port()), options, &status);
    if (!quic) {
        sink.Stop();
        return result;
    }
    auto stream = quic->OpenStream(&status);
    if (!stream) {
        sink.Stop();
        return result;
    }
    const std::vector<std::uint8_t> payload(payload_bytes, 0x5A);
    const double start = NowMicros();
    int sent = 0;
    for (int index = 0; index < messages; ++index) {
        if (!stream->WriteAll(payload.data(), payload.size(), nullptr)) {
            break;
        }
        ++sent;
    }
    const std::uint64_t expected = static_cast<std::uint64_t>(sent) * payload_bytes;
    const double deadline = NowMicros() + 5e6;
    while (received.load(std::memory_order_acquire) < expected && NowMicros() < deadline) {
        std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
    const double wall = NowMicros() - start;
    result.messages = sent;
    result.wall_ms = wall / 1000.0;
    result.received_bytes = received.load(std::memory_order_acquire);
    if (wall > 0.0) {
        result.msg_per_sec = static_cast<double>(sent) * 1e6 / wall;
        result.mb_per_sec =
            static_cast<double>(result.received_bytes) / (wall / 1e6) / 1e6;
    }
    stream.reset();
    quic->Close();
    quic.reset();
    sink.Stop();
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    return result;
}

void Benchmark(rpc::Server& server, int iterations, int echo_iterations, bool tuned = true) {
    Announce("bench: QUIC vs TCP（同步调用，复用连接）");
    BenchResult tcp;
    BenchRpcOverTcp(server, iterations, &tcp);
    BenchResult quic_low;
    rpc::QuicStats quic_stats;
    BenchRpcOverQuic(server, iterations, rpc::QuicMode::kLowLatency, &quic_low, &quic_stats,
                     tuned);
    BenchResult quic_cc;
    rpc::QuicStats quic_cc_stats;
    BenchRpcOverQuic(server, iterations, rpc::QuicMode::kCongestionControl, &quic_cc,
                     &quic_cc_stats, tuned);
    const BenchResult echo = BenchTransportEcho(echo_iterations);

    std::printf("\n| 路径 | 样本 | p50 (µs) | p90 | p99 | 均值 | 吞吐 (次/s) |\n");
    std::printf("|---|---|---|---|---|---|---|\n");
    std::printf("| TCP 复用连接 | %d | %.1f | %.1f | %.1f | %.1f | %.0f |\n", tcp.samples,
                tcp.p50_us, tcp.p90_us, tcp.p99_us, tcp.mean_us, tcp.throughput);
    std::printf("| QUIC 低延迟（无 BBR/pacing） | %d | %.1f | %.1f | %.1f | %.1f | %.0f |\n",
                quic_low.samples, quic_low.p50_us, quic_low.p90_us, quic_low.p99_us,
                quic_low.mean_us, quic_low.throughput);
    std::printf("| QUIC BBR+pacing | %d | %.1f | %.1f | %.1f | %.1f | %.0f |\n",
                quic_cc.samples, quic_cc.p50_us, quic_cc.p90_us, quic_cc.p99_us,
                quic_cc.mean_us, quic_cc.throughput);
    std::printf("| 传输层 QUIC echo 1B（下限） | %d | %.1f | %.1f | %.1f | %.1f | %.0f |\n",
                echo.samples, echo.p50_us, echo.p90_us, echo.p99_us, echo.mean_us,
                echo.throughput);
    std::printf("\n- QUIC 统计：帧 %llu/%llu，块 %llu/%llu，重传 %llu，srtt=%llums pto=%llums\n",
                static_cast<unsigned long long>(quic_stats.frames_sent),
                static_cast<unsigned long long>(quic_stats.frames_received),
                static_cast<unsigned long long>(quic_stats.chunks_sent),
                static_cast<unsigned long long>(quic_stats.chunks_received),
                static_cast<unsigned long long>(quic_stats.retransmits),
                static_cast<unsigned long long>(quic_stats.srtt_ms),
                static_cast<unsigned long long>(quic_stats.pto_ms));
    if (quic_low.p50_us > 0.0 && tcp.p50_us > 0.0) {
        std::printf("- p50 相对 TCP：%.2fx（低延迟）/ %.2fx（BBR）\n",
                    quic_low.p50_us / tcp.p50_us, quic_cc.p50_us / tcp.p50_us);
    }
    RPC_CHECK(tcp.samples == iterations);
    RPC_CHECK(quic_low.samples == iterations);
}

/// @brief ABBA 交替对比：A=TCP、B=QUIC，各跑两段，抵消机器漂移。
/// @details 顺序 A→B→B→A；A 与 B 各自合并两段样本后给出 p50/p90/p99/均值/
///          吞吐，并报告段间漂移。这是“QUIC 是否真的比 TCP 快”的判据：只有
///          合并后仍领先、且段间漂移小于差值，才说明差异真实存在。
void BenchAbba(rpc::Server& server, int iterations, rpc::QuicMode mode, bool tuned) {
    Announce("bench: ABBA 交替对比（A=TCP，B=QUIC）");
    std::printf("  每段 %d 次；顺序 A(TCP) → B(QUIC) → B(QUIC) → A(TCP)；tuned=%d\n",
                iterations, tuned ? 1 : 0);
    std::fflush(stdout);

    BenchResult segment[4];
    std::vector<double> samples[4];
    rpc::QuicStats quic_stats[2];
    const char* names[4] = {"1/A TCP ", "2/B QUIC", "3/B QUIC", "4/A TCP "};
    for (int index = 0; index < 4; ++index) {
        const bool is_quic = (index == 1 || index == 2);
        std::printf("  [%s] 开始…\n", names[index]);
        std::fflush(stdout);
        if (is_quic) {
            BenchRpcOverQuic(server, iterations, mode, &segment[index], &quic_stats[index - 1],
                             tuned, &samples[index]);
        } else {
            BenchRpcOverTcp(server, iterations, &segment[index], &samples[index]);
        }
        std::printf("  [%s] n=%d p50=%.1f p90=%.1f p99=%.1f mean=%.1f 吞吐=%.0f/s wall=%.1fms\n",
                    names[index], segment[index].samples, segment[index].p50_us,
                    segment[index].p90_us, segment[index].p99_us, segment[index].mean_us,
                    segment[index].throughput, segment[index].wall_us / 1000.0);
        std::fflush(stdout);
    }

    std::vector<double> merged_a = samples[0];
    merged_a.insert(merged_a.end(), samples[3].begin(), samples[3].end());
    std::vector<double> merged_b = samples[1];
    merged_b.insert(merged_b.end(), samples[2].begin(), samples[2].end());
    const BenchResult a = SummarizeBench(merged_a, segment[0].wall_us + segment[3].wall_us);
    const BenchResult b = SummarizeBench(merged_b, segment[1].wall_us + segment[2].wall_us);

    std::printf("\n| 段 | 路径 | 样本 | p50 (µs) | p90 | p99 | 均值 | 吞吐 (次/s) |\n");
    std::printf("|---|---|---|---|---|---|---|---|\n");
    for (int index = 0; index < 4; ++index) {
        std::printf("| %s | %s | %d | %.1f | %.1f | %.1f | %.1f | %.0f |\n", names[index],
                    (index == 1 || index == 2) ? "QUIC" : "TCP", segment[index].samples,
                    segment[index].p50_us, segment[index].p90_us, segment[index].p99_us,
                    segment[index].mean_us, segment[index].throughput);
    }
    std::printf("| 合并 A | TCP | %d | %.1f | %.1f | %.1f | %.1f | %.0f |\n", a.samples, a.p50_us,
                a.p90_us, a.p99_us, a.mean_us, a.throughput);
    std::printf("| 合并 B | QUIC | %d | %.1f | %.1f | %.1f | %.1f | %.0f |\n", b.samples, b.p50_us,
                b.p90_us, b.p99_us, b.mean_us, b.throughput);
    const double tcp_drift = 100.0 * (segment[3].p50_us - segment[0].p50_us) / segment[0].p50_us;
    const double quic_drift = 100.0 * (segment[2].p50_us - segment[1].p50_us) / segment[1].p50_us;
    std::printf("\n- 段间漂移：TCP p50 %.1f→%.1f（%+.1f%%），QUIC p50 %.1f→%.1f（%+.1f%%）\n",
                segment[0].p50_us, segment[3].p50_us, tcp_drift, segment[1].p50_us,
                segment[2].p50_us, quic_drift);
    std::printf("- 合并 p50 比：QUIC/TCP = %.3fx；p99 比 = %.3fx；吞吐比 = %.3fx\n",
                b.p50_us / a.p50_us, b.p99_us / a.p99_us, b.throughput / a.throughput);
    std::printf("- QUIC 统计（合并两段）：帧 %llu/%llu，块 %llu/%llu，重传 %llu，srtt=%llums\n",
                static_cast<unsigned long long>(quic_stats[0].frames_sent +
                                                quic_stats[1].frames_sent),
                static_cast<unsigned long long>(quic_stats[0].frames_received +
                                                quic_stats[1].frames_received),
                static_cast<unsigned long long>(quic_stats[0].chunks_sent +
                                                quic_stats[1].chunks_sent),
                static_cast<unsigned long long>(quic_stats[0].chunks_received +
                                                quic_stats[1].chunks_received),
                static_cast<unsigned long long>(quic_stats[0].retransmits +
                                                quic_stats[1].retransmits),
                static_cast<unsigned long long>(quic_stats[1].srtt_ms));
    RPC_CHECK_EQ(a.samples, iterations * 2);
    RPC_CHECK_EQ(b.samples, iterations * 2);
}

}  // namespace

int main() {
    TestServer test_server;
    if (!test_server.Start()) {
        std::printf("服务端启动失败\n");
        return 1;
    }
    std::printf("# RPC over QUIC 测试（%s）\n", __DATE__);
    std::printf("- TCP 地址：%s\n", test_server.address.c_str());

    // 弱网公平对照（需要 root：内核 netem）。默认 0 = 关闭，不影响既有测试。
    const int weaknet_iterations =
        static_cast<int>(EnvIntAllowZero("RPC_QUIC_WEAKNET_ITERS", 0));
    if (weaknet_iterations > 0) {
        weaknet::WeakNetCompare(test_server.server, weaknet_iterations);
    }

    const bool bench_only = std::getenv("RPC_QUIC_BENCH_ONLY") != nullptr;
    const int loss_iterations = static_cast<int>(EnvIntAllowZero("RPC_QUIC_LOSS_ITERS", 500));
    if (!bench_only) {
        TestBasic(test_server.server);
        TestStreamMultiplex(test_server.server);
        TestCallMultiplex(test_server.server);
        TestEncryption(test_server.server);
        TestZeroRtt(test_server.server);
        TestLossy(test_server.server, loss_iterations);
    }

    // 基准阶段：`RPC_QUIC_BENCH_ITERS=0` 显式跳过（EnvIntAllowZero 允许 0）。
    const std::int64_t bench_iterations = EnvIntAllowZero("RPC_QUIC_BENCH_ITERS", 2000);
    const bool tuned = EnvFlag("RPC_QUIC_TUNED", true);
    const std::int64_t abba = EnvIntAllowZero("RPC_QUIC_ABBA", 0);
    rpc::QuicMode bench_mode = rpc::QuicMode::kLowLatency;
    if (const char* mode_text = std::getenv("RPC_QUIC_MODE");
        mode_text != nullptr && std::strcmp(mode_text, "bbr") == 0) {
        bench_mode = rpc::QuicMode::kCongestionControl;
    }
    if (bench_iterations > 0) {
        if (abba > 0) {
            BenchAbba(test_server.server, static_cast<int>(bench_iterations), bench_mode, tuned);
        } else {
            Benchmark(test_server.server, static_cast<int>(bench_iterations),
                      static_cast<int>(EnvIntAllowZero("RPC_QUIC_ECHO_ITERS", 5000)), tuned);
        }
    }
    const std::int64_t throughput_iterations = EnvInt("RPC_QUIC_THROUGHPUT_ITERS", 0);
    if (throughput_iterations > 0) {
        Announce("throughput: 单向 STREAM 消息吞吐（数据面）");
        const ThroughputResult result =
            BenchThroughput(static_cast<int>(throughput_iterations), 8);
        std::printf("\n| 路径 | 消息数 | 接收字节 | 墙钟 (ms) | 消息/s | 接收吞吐 (MB/s) |\n");
        std::printf("|---|---|---|---|---|---|\n");
        std::printf("| QUIC 单向 8B STREAM | %d | %llu | %.1f | %.0f | %.2f |\n",
                    result.messages,
                    static_cast<unsigned long long>(result.received_bytes), result.wall_ms,
                    result.msg_per_sec, result.mb_per_sec);
        RPC_CHECK_EQ(result.received_bytes,
                     static_cast<std::uint64_t>(result.messages) * 8U);
    }

    test_server.server.Stop();
    return Summary("rpc_quic");
}
