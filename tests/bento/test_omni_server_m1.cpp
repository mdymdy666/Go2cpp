/**
 * @file test_omni_server_m1.cpp
 * @brief OmniNetServer M1 测试：真实 UDP LinkServer / TCP HttpServer 接入、
 *        配置驱动监听、生命周期、端口重绑、并发与 M0 Memory 兼容。
 * @author go2cpp contributors
 * @date 2026-10-30
 * @copyright MIT License
 *
 * @details 覆盖 M1 的全部交付承诺：
 *   1. 配置选择：TCP/UDP/Memory 对应的后端与视图；非法组合/未实现协议/
 *      Poll+真实后端/空地址/错误 PSK 长度在 Create 阶段返回对应错误码；
 *   2. UDP 真实回环：PeerHub 客户端经真实 socket 连接，STREAM 双向、
 *      DATAGRAM 双向、广播、多客户端隔离、PSK 加密链路；
 *   3. UDP 生命周期：Start 真实监听、Port 真实端口、重复 Start 拒绝、
 *      Stop 关闭并释放端口、Stop 幂等、Stop 后发送失败、端口重绑；
 *   4. TCP 真实 HTTP：GET/POST/404/keep-alive；SSE 三事件按序推流；
 *   5. TCP 并发：8 客户端 × 5 个 keep-alive 请求，全部成功且统计增长；
 *   6. 停止语义：有活动 SSE 长连接与空闲 keep-alive 连接时 Stop 不挂起；
 *   7. 视图手动 Listen：Start 前覆盖端点、重复 Listen 拒绝、Start 复用；
 *   8. 错误路径：端口占用/非法地址的 Start 失败与 last_error；
 *   9. Memory 兼容：M0 的 ConnectMemory + PollOnce 回环不受 M1 影响；
 *  10. 视图寿命：NetServer 析构后视图仍可安全查询（不再驱动 I/O）。
 *
 * 客户端使用 PeerHub（Bento 多 peer 主动端）与 base 的真实 TCP socket；
 * 所有用例只走 127.0.0.1 回环，无外部网络依赖。
 */

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "go2cpp/bento/net/link_server.hpp"
#include "go2cpp/bento/net/omni_server.hpp"
#include "go2cpp/bento/net/peer_hub.hpp"
#include "go2cpp/network/http.hpp"
#include "go2cpp/network/socket.hpp"
#include "test_util.hpp"

namespace {

using go2cpp::bento::LinkReliability;
using go2cpp::bento::LinkSendResult;
using go2cpp::bento::PeerHub;
using go2cpp::bento::PeerHubConfig;
using go2cpp::bento::SendStatus;
using go2cpp::bento::omni::BindEndpoint;
using go2cpp::bento::omni::ConcurrencyKind;
using go2cpp::bento::omni::IoMode;
using go2cpp::bento::omni::NetServer;
using go2cpp::bento::omni::OmniError;
using go2cpp::bento::omni::OmniNetConfig;
using go2cpp::bento::omni::ProtocolKind;
using go2cpp::bento::omni::TransportKind;
using go2cpp::http::Method;
using go2cpp::http::Request;
using go2cpp::http::Response;
using go2cpp::http::Status;
using go2cpp::net::DialTCP;
using go2cpp::net::Socket;
using go2cpp::net::SocketStream;
using go2cpp::sse::Event;
using go2cpp::sse::Stream;

using std::chrono::milliseconds;
using std::chrono::steady_clock;

std::mutex g_print_mutex;

void PrintCase(const char* name) {
    std::lock_guard<std::mutex> lock(g_print_mutex);
    std::printf("[CASE] %s\n", name);
}

template <typename Predicate>
bool WaitFor(Predicate done, int timeout_ms) {
    const auto deadline = steady_clock::now() + milliseconds(timeout_ms);
    while (steady_clock::now() < deadline) {
        if (done()) {
            return true;
        }
        std::this_thread::sleep_for(milliseconds(1));
    }
    return done();
}

/// 构造 M1 通用配置：127.0.0.1:0 + NonBlocking，避免 V4 的 Blocking+Poll 干扰。
OmniNetConfig MakeConfig(TransportKind transport, std::vector<ProtocolKind> protocols,
                         ConcurrencyKind concurrency = ConcurrencyKind::kGmp) {
    OmniNetConfig config;
    config.mode = IoMode::kNonBlocking;
    config.transport = transport;
    config.protocols = std::move(protocols);
    config.concurrency = concurrency;
    config.bind = BindEndpoint{"127.0.0.1", 0};
    return config;
}

OmniNetConfig UdpLinkConfig() {
    return MakeConfig(TransportKind::kUdp, {ProtocolKind::kLink}, ConcurrencyKind::kGmp);
}

OmniNetConfig TcpHttpConfig() {
    return MakeConfig(TransportKind::kTcp, {ProtocolKind::kHttp}, ConcurrencyKind::kGmp);
}

OmniNetConfig MemoryRawConfig() {
    return MakeConfig(TransportKind::kMemory, {ProtocolKind::kRaw}, ConcurrencyKind::kPoll);
}

std::unique_ptr<NetServer> CreateStarted(const OmniNetConfig& config) {
    std::string error;
    auto server = NetServer::Create(config, &error);
    if (server == nullptr) {
        std::fprintf(stderr, "[FAIL] Create 失败：%s\n", error.c_str());
        ++bento_test::g_failures;
        return nullptr;
    }
    if (!server->Start(&error)) {
        std::fprintf(stderr, "[FAIL] Start 失败：%s\n", error.c_str());
        ++bento_test::g_failures;
        return nullptr;
    }
    return server;
}

// ---------------------------------------------------------------------------
// UDP 客户端（PeerHub，真实 UDP socket）
// ---------------------------------------------------------------------------

struct ReceivedMessage {
    std::uint8_t channel{0};
    std::vector<std::uint8_t> payload;
};

PeerHubConfig MakePeerConfig(const std::vector<std::uint8_t>& psk) {
    PeerHubConfig config;
    config.bind_address = "127.0.0.1";
    config.bind_port = 0;
    config.heartbeat_interval_ms = 100;
    config.peer_timeout_ms = 10000;
    config.handshake_timeout_ms = 3000;
    config.hello_initial_pto_ms = 50;
    config.hello_max_pto_ms = 500;
    config.hello_max_attempts = 16;
    config.require_encryption = !psk.empty();
    config.pre_shared_key = psk;
    return config;
}

class UdpClient final {
public:
    explicit UdpClient(const std::vector<std::uint8_t>& psk = {}) : m_hub(MakePeerConfig(psk)) {
        std::string error;
        m_opened = m_hub.Open(&error);
        if (!m_opened) {
            std::fprintf(stderr, "[client] PeerHub::Open 失败：%s\n", error.c_str());
        }
        m_hub.on_peer_connected = [this](std::uint32_t) { m_connected = true; };
        m_hub.on_stream = [this](std::uint32_t, std::uint8_t channel, const std::uint8_t* data,
                                 std::size_t bytes) {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_streams.push_back(ReceivedMessage{channel, std::vector<std::uint8_t>(data, data + bytes)});
        };
        m_hub.on_datagram = [this](std::uint32_t, std::uint8_t channel, const std::uint8_t* data,
                                   std::size_t bytes) {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_datagrams.push_back(
                ReceivedMessage{channel, std::vector<std::uint8_t>(data, data + bytes)});
        };
    }

    ~UdpClient() { m_hub.Close(); }

    UdpClient(const UdpClient&) = delete;
    UdpClient& operator=(const UdpClient&) = delete;

    bool Connect(std::uint16_t port, std::uint32_t peer_id) {
        if (!m_opened) {
            return false;
        }
        m_peer_id = peer_id;
        std::string error;
        if (!m_hub.AddPeer(peer_id, "127.0.0.1", port, &error)) {
            std::fprintf(stderr, "[client] AddPeer 失败：%s\n", error.c_str());
            return false;
        }
        return PumpUntil([this]() { return m_connected.load(); }, 5000);
    }

    template <typename Predicate>
    bool PumpUntil(Predicate done, int timeout_ms) {
        const auto deadline = steady_clock::now() + milliseconds(timeout_ms);
        while (steady_clock::now() < deadline) {
            m_hub.RunOnce(1);
            if (done()) {
                return true;
            }
        }
        return done();
    }

    bool SendReliable(std::uint8_t channel, const std::vector<std::uint8_t>& payload,
                      int timeout_ms = 5000) {
        const auto deadline = steady_clock::now() + milliseconds(timeout_ms);
        while (steady_clock::now() < deadline) {
            const SendStatus status =
                m_hub.TrySendStream(m_peer_id, channel, payload.data(), payload.size());
            if (status == SendStatus::kOk) {
                return true;
            }
            if (status == SendStatus::kError) {
                return false;
            }
            m_hub.RunOnce(1);
        }
        return false;
    }

    bool SendUnreliable(std::uint8_t channel, const std::vector<std::uint8_t>& payload) {
        return m_hub.SendDatagram(m_peer_id, channel, payload.data(), payload.size());
    }

    std::vector<ReceivedMessage> Streams() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_streams;
    }

    std::vector<ReceivedMessage> Datagrams() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_datagrams;
    }

    std::size_t StreamCount() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_streams.size();
    }

    std::size_t DatagramCount() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_datagrams.size();
    }

    void Close() { m_hub.Close(); }

private:
    PeerHub m_hub;
    bool m_opened{false};
    std::uint32_t m_peer_id{0};
    std::atomic<bool> m_connected{false};
    mutable std::mutex m_mutex;
    std::vector<ReceivedMessage> m_streams;
    std::vector<ReceivedMessage> m_datagrams;
};

std::vector<std::uint8_t> Bytes(std::string_view text) {
    return std::vector<std::uint8_t>(text.begin(), text.end());
}

std::string Text(const std::vector<std::uint8_t>& bytes) {
    return std::string(bytes.begin(), bytes.end());
}

// ---------------------------------------------------------------------------
// HTTP 客户端（base http::Parser，真实 TCP socket）
// ---------------------------------------------------------------------------

class HttpClient final {
public:
    bool Connect(std::uint16_t port) {
        m_socket = DialTCP("127.0.0.1", port, milliseconds(2000));
        if (!m_socket) {
            return false;
        }
        m_socket->set_recv_timeout(milliseconds(3000));
        return true;
    }

    bool Send(std::string_view raw) {
        if (!m_socket) {
            return false;
        }
        SocketStream stream(m_socket, false);
        return stream.write_all(raw.data(), raw.size());
    }

    bool ReadResponse(Response& response) {
        for (;;) {
            if (!m_pending.empty()) {
                const auto parsed = m_parser.feed(m_pending, response);
                m_pending.clear();
                if (parsed.state == go2cpp::http::ParseState::Error) {
                    return false;
                }
                if (parsed.state == go2cpp::http::ParseState::Complete) {
                    m_pending = m_parser.take_remaining();
                    return true;
                }
                continue;
            }
            char buffer[4096];
            const auto count = m_socket->recv(buffer, sizeof(buffer));
            if (count.transferred <= 0) {
                return false;
            }
            const auto parsed = m_parser.feed(
                std::string_view(buffer, static_cast<std::size_t>(count.transferred)), response);
            if (parsed.state == go2cpp::http::ParseState::Error) {
                return false;
            }
            if (parsed.state == go2cpp::http::ParseState::Complete) {
                m_pending = m_parser.take_remaining();
                return true;
            }
        }
    }

    bool ReadUntil(std::string_view needle, std::string& collected, int timeout_ms) {
        if (!m_socket) {
            return false;
        }
        m_socket->set_recv_timeout(milliseconds(100));
        const auto deadline = steady_clock::now() + milliseconds(timeout_ms);
        while (steady_clock::now() < deadline) {
            if (collected.find(needle) != std::string::npos) {
                return true;
            }
            char buffer[4096];
            const auto count = m_socket->recv(buffer, sizeof(buffer));
            if (count.transferred <= 0) {
                continue;
            }
            collected.append(buffer, static_cast<std::size_t>(count.transferred));
        }
        return collected.find(needle) != std::string::npos;
    }

    void Close() {
        if (m_socket) {
            (void)m_socket->close();
            m_socket.reset();
        }
    }

    ~HttpClient() { Close(); }

    HttpClient() = default;
    HttpClient(const HttpClient&) = delete;
    HttpClient& operator=(const HttpClient&) = delete;

private:
    Socket::Ptr m_socket;
    go2cpp::http::Parser m_parser;
    std::string m_pending;
};

std::string GetRequest(const std::string& path) {
    return "GET " + path + " HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n";
}

// ---------------------------------------------------------------------------
// 1. 配置选择与非法组合
// ---------------------------------------------------------------------------

void TestConfigSelection() {
    PrintCase("配置：transport → 后端选择、合法/非法组合、视图有效性");

    // 合法组合：每种传输的真实后端都应被选中。
    struct LegalCase {
        TransportKind transport;
        std::vector<ProtocolKind> protocols;
        ConcurrencyKind concurrency;
    };
    const LegalCase legal[] = {
        {TransportKind::kUdp, {ProtocolKind::kLink}, ConcurrencyKind::kGmp},
        {TransportKind::kUdp, {ProtocolKind::kLink}, ConcurrencyKind::kThread},
        {TransportKind::kTcp, {ProtocolKind::kHttp}, ConcurrencyKind::kGmp},
        {TransportKind::kTcp, {ProtocolKind::kSse}, ConcurrencyKind::kThread},
        {TransportKind::kTcp, {ProtocolKind::kHttp, ProtocolKind::kSse},
         ConcurrencyKind::kGmp},
        {TransportKind::kMemory, {ProtocolKind::kRaw}, ConcurrencyKind::kPoll},
    };
    for (const auto& item : legal) {
        OmniNetConfig config = MakeConfig(item.transport, item.protocols, item.concurrency);
        std::string error;
        BENTO_CHECK_EQ(NetServer::ValidateConfig(config, &error), OmniError::kOk);
        BENTO_CHECK(error.empty());
        auto server = NetServer::Create(config, &error);
        BENTO_CHECK(server != nullptr);
        if (server == nullptr) {
            continue;
        }
        const bool udp = item.transport == TransportKind::kUdp;
        const bool tcp = item.transport == TransportKind::kTcp;
        BENTO_CHECK_EQ(server->AsLinkServer().valid(), udp);
        BENTO_CHECK_EQ(server->AsHttpServer().valid(), tcp);
        BENTO_CHECK_EQ(server->AsRawServer().valid(), item.transport == TransportKind::kMemory);
        if (udp) {
            BENTO_CHECK(server->AsLinkServer().backend() != nullptr);
        }
        if (tcp) {
            BENTO_CHECK(server->AsHttpServer().backend() != nullptr);
        }
    }

    // 设计稿默认配置（TCP + HTTP + Gmp）在 M1 必须合法。
    {
        const OmniNetConfig defaults;
        std::string error;
        BENTO_CHECK_EQ(NetServer::ValidateConfig(defaults, &error), OmniError::kOk);
        BENTO_CHECK(error.empty());
        auto server = NetServer::Create(defaults, &error);
        BENTO_CHECK(server != nullptr);
    }

    // 非法组合：矩阵/实现门/并发门的错误码必须稳定。
    struct IllegalCase {
        OmniNetConfig config;
        OmniError expected;
    };
    std::vector<IllegalCase> illegal;

    auto add = [&illegal](OmniNetConfig config, OmniError expected) {
        illegal.push_back(IllegalCase{std::move(config), expected});
    };

    {
        auto config = MakeConfig(TransportKind::kTcp, {ProtocolKind::kLink});
        add(std::move(config), OmniError::kUnsupportedCombination);
    }
    {
        auto config = MakeConfig(TransportKind::kUdp, {ProtocolKind::kHttp});
        add(std::move(config), OmniError::kUnsupportedCombination);
    }
    {
        auto config = MakeConfig(TransportKind::kUdp, {ProtocolKind::kRaw});
        add(std::move(config), OmniError::kUnsupported);
    }
    {
        auto config = MakeConfig(TransportKind::kTcp, {ProtocolKind::kRaw});
        add(std::move(config), OmniError::kUnsupported);
    }
    {
        auto config = MakeConfig(TransportKind::kTcp, {ProtocolKind::kWs});
        add(std::move(config), OmniError::kUnsupported);
    }
    {
        auto config = MakeConfig(TransportKind::kMemory, {ProtocolKind::kLink});
        add(std::move(config), OmniError::kUnsupported);
    }
    {
        auto config = MakeConfig(TransportKind::kTls, {ProtocolKind::kHttp});
        add(std::move(config), OmniError::kUnsupported);
    }
    {
        // 真实后端不接受 Poll：配置成功但 PollOnce 不推进属于"半支持"。
        auto config = MakeConfig(TransportKind::kUdp, {ProtocolKind::kLink},
                                 ConcurrencyKind::kPoll);
        config.mode = IoMode::kNonBlocking;
        add(std::move(config), OmniError::kUnsupportedCombination);
    }
    {
        auto config = MakeConfig(TransportKind::kTcp, {ProtocolKind::kHttp},
                                 ConcurrencyKind::kPoll);
        config.mode = IoMode::kNonBlocking;
        add(std::move(config), OmniError::kUnsupportedCombination);
    }
    {
        auto config = MakeConfig(TransportKind::kUdp, {ProtocolKind::kLink});
        config.bind.address.clear();
        add(std::move(config), OmniError::kInvalidConfig);
    }
    {
        auto config = MakeConfig(TransportKind::kUdp, {ProtocolKind::kLink});
        config.link_psk.assign(31, 0x11);
        add(std::move(config), OmniError::kInvalidConfig);
    }
    {
        auto config = MakeConfig(TransportKind::kUdp, {ProtocolKind::kLink,
                                                       ProtocolKind::kRaw});
        add(std::move(config), OmniError::kUnsupported);
    }

    for (const auto& item : illegal) {
        std::string error;
        BENTO_CHECK_EQ(NetServer::ValidateConfig(item.config, &error), item.expected);
        BENTO_CHECK(!error.empty());
        BENTO_CHECK(NetServer::Create(item.config, &error) == nullptr);
    }
}

// ---------------------------------------------------------------------------
// 2. UDP 真实回环：STREAM/DATAGRAM/广播/多客户端
// ---------------------------------------------------------------------------

void TestUdpRealLoopback() {
    PrintCase("UDP：真实 socket 回环（STREAM/DATAGRAM/广播/多客户端隔离）");

    OmniNetConfig config = UdpLinkConfig();
    config.limits.max_connections = 8;
    config.limits.idle_timeout = milliseconds(15000);
    auto server = CreateStarted(config);
    if (server == nullptr) {
        return;
    }
    BENTO_CHECK(server->IsRunning());
    BENTO_CHECK_EQ(server->last_error(), OmniError::kOk);
    const std::uint16_t port = server->Port();
    BENTO_CHECK(port != 0);

    auto link = server->AsLinkServer();
    BENTO_CHECK(link.valid());
    BENTO_CHECK_EQ(link.local_port(), port);
    BENTO_CHECK_EQ(link.peer_count(), std::size_t{0});

    std::atomic<int> echo_failures{0};
    std::mutex stream_mutex;
    std::vector<std::pair<std::uint32_t, std::vector<std::uint8_t>>> received_streams;
    link.OnStream([&](std::uint32_t peer, std::uint8_t channel, const std::uint8_t* data,
                      std::size_t bytes) {
        {
            std::lock_guard<std::mutex> lock(stream_mutex);
            received_streams.emplace_back(peer,
                                          std::vector<std::uint8_t>(data, data + bytes));
        }
        if (link.Send(peer, channel, data, bytes, LinkReliability::kReliable) !=
            LinkSendResult::kOk) {
            echo_failures.fetch_add(1);
        }
    });
    link.OnDatagram([&](std::uint32_t peer, std::uint8_t channel, const std::uint8_t* data,
                        std::size_t bytes) {
        if (link.Send(peer, channel, data, bytes, LinkReliability::kUnreliable) !=
            LinkSendResult::kOk) {
            echo_failures.fetch_add(1);
        }
    });

    UdpClient alice;
    UdpClient bob;
    BENTO_CHECK(alice.Connect(port, 1));
    BENTO_CHECK(bob.Connect(port, 2));
    BENTO_CHECK(WaitFor([&]() { return link.peer_count() == 2; }, 3000));
    BENTO_CHECK_EQ(server->Connections(), std::size_t{2});

    // STREAM 双向：A 发 → 服务端回显 → A 收到。
    const auto payload_a = Bytes("hello-from-alice");
    BENTO_CHECK(alice.SendReliable(1, payload_a));
    BENTO_CHECK(alice.PumpUntil([&]() { return alice.StreamCount() >= 1; }, 5000));
    {
        const auto streams = alice.Streams();
        BENTO_CHECK_EQ(streams.size(), std::size_t{1});
        if (!streams.empty()) {
            BENTO_CHECK_EQ(streams[0].channel, std::uint8_t{1});
            BENTO_CHECK_EQ(Text(streams[0].payload), std::string("hello-from-alice"));
        }
    }

    // 多客户端隔离：B 发不同内容，A 不应收到 B 的回显。
    const auto payload_b = Bytes("hello-from-bob");
    BENTO_CHECK(bob.SendReliable(1, payload_b));
    BENTO_CHECK(bob.PumpUntil([&]() { return bob.StreamCount() >= 1; }, 5000));
    BENTO_CHECK_EQ(bob.StreamCount(), std::size_t{1});
    BENTO_CHECK_EQ(alice.StreamCount(), std::size_t{1});
    {
        const auto streams = bob.Streams();
        if (!streams.empty()) {
            BENTO_CHECK_EQ(Text(streams[0].payload), std::string("hello-from-bob"));
        }
    }

    // DATAGRAM 双向。
    const auto datagram = Bytes("telemetry-42");
    BENTO_CHECK(alice.SendUnreliable(2, datagram));
    BENTO_CHECK(alice.PumpUntil([&]() { return alice.DatagramCount() >= 1; }, 5000));
    {
        const auto datagrams = alice.Datagrams();
        BENTO_CHECK_EQ(datagrams.size(), std::size_t{1});
        if (!datagrams.empty()) {
            BENTO_CHECK_EQ(datagrams[0].channel, std::uint8_t{2});
            BENTO_CHECK_EQ(Text(datagrams[0].payload), std::string("telemetry-42"));
        }
    }

    // 广播：两个客户端都应收到。
    const auto broadcast = Bytes("broadcast-omni-m1");
    BENTO_CHECK_EQ(link.Broadcast(4, broadcast.data(), broadcast.size(),
                                  LinkReliability::kReliable),
                   std::size_t{2});
    BENTO_CHECK(alice.PumpUntil([&]() { return alice.StreamCount() >= 2; }, 5000));
    BENTO_CHECK(bob.PumpUntil([&]() { return bob.StreamCount() >= 2; }, 5000));
    BENTO_CHECK_EQ(alice.StreamCount(), std::size_t{2});
    BENTO_CHECK_EQ(bob.StreamCount(), std::size_t{2});
    {
        const auto streams = alice.Streams();
        if (streams.size() >= 2) {
            BENTO_CHECK_EQ(Text(streams[1].payload), std::string("broadcast-omni-m1"));
        }
    }

    BENTO_CHECK_EQ(echo_failures.load(), 0);
    {
        std::lock_guard<std::mutex> lock(stream_mutex);
        BENTO_CHECK(received_streams.size() >= 2);  // Alice 与 Bob 各一条 STREAM。
    }

    const auto stats = server->Stats();
    BENTO_CHECK(stats.accepted >= 2);
    BENTO_CHECK(stats.active >= 2);
    BENTO_CHECK(stats.messages_in >= 2);
    BENTO_CHECK(stats.messages_out >= 2);
    BENTO_CHECK(stats.bytes_in > 0);
    BENTO_CHECK(stats.bytes_out > 0);

    // 有活动 peer 时 Stop 不应挂起，并释放端口。
    const auto stop_begin = steady_clock::now();
    server->Stop();
    const auto stop_cost = steady_clock::now() - stop_begin;
    BENTO_CHECK(stop_cost < milliseconds(3000));
    BENTO_CHECK(!server->IsRunning());
    BENTO_CHECK_EQ(server->Port(), std::uint16_t{0});
    BENTO_CHECK_EQ(link.peer_count(), std::size_t{0});
    BENTO_CHECK_EQ(link.Send(1, 1, "x", 1), LinkSendResult::kNotConnected);
    server->Stop();  // 幂等。
    alice.Close();
    bob.Close();
}

// ---------------------------------------------------------------------------
// 3. UDP PSK 加密链路
// ---------------------------------------------------------------------------

void TestUdpPskEncryption() {
    PrintCase("UDP：32B PSK 加密链路真实回环");

    std::vector<std::uint8_t> psk(32, 0);
    for (std::size_t index = 0; index < psk.size(); ++index) {
        psk[index] = static_cast<std::uint8_t>(index * 7 + 3);
    }
    OmniNetConfig config = UdpLinkConfig();
    config.link_psk = psk;
    auto server = CreateStarted(config);
    if (server == nullptr) {
        return;
    }
    auto link = server->AsLinkServer();
    BENTO_CHECK(link.valid());
    link.OnStream([&](std::uint32_t peer, std::uint8_t channel, const std::uint8_t* data,
                      std::size_t bytes) {
        (void)link.Send(peer, channel, data, bytes, LinkReliability::kReliable);
    });

    UdpClient client(psk);
    BENTO_CHECK(client.Connect(server->Port(), 7));
    const auto payload = Bytes("encrypted-omni-m1");
    BENTO_CHECK(client.SendReliable(1, payload));
    BENTO_CHECK(client.PumpUntil([&]() { return client.StreamCount() >= 1; }, 5000));
    {
        const auto streams = client.Streams();
        BENTO_CHECK_EQ(streams.size(), std::size_t{1});
        if (!streams.empty()) {
            BENTO_CHECK_EQ(Text(streams[0].payload), std::string("encrypted-omni-m1"));
        }
    }
    BENTO_CHECK_EQ(link.peer_count(), std::size_t{1});
    server->Stop();
    client.Close();
}

// ---------------------------------------------------------------------------
// 4. UDP 生命周期：手动 Listen / 重复 Start / Stop 终态 / 端口重绑
// ---------------------------------------------------------------------------

void TestUdpLifecycleAndRebind() {
    PrintCase("UDP：视图 Listen / 重复 Start / Stop 终态 / 端口重绑");

    OmniNetConfig config = UdpLinkConfig();
    auto server = NetServer::Create(config, nullptr);
    if (server == nullptr) {
        return;
    }
    auto link = server->AsLinkServer();
    BENTO_CHECK(!server->IsRunning());
    BENTO_CHECK_EQ(server->Port(), std::uint16_t{0});

    // Start 之前手动 Listen；重复 Listen 必须被拒绝。
    std::string error;
    BENTO_CHECK(link.Listen("127.0.0.1", 0, &error));
    const std::uint16_t manual_port = link.local_port();
    BENTO_CHECK(manual_port != 0);
    BENTO_CHECK(!link.Listen("127.0.0.1", 0, &error));

    // Start 复用已有监听，Port 与手动端口一致。
    BENTO_CHECK(server->Start(&error));
    BENTO_CHECK(error.empty());
    BENTO_CHECK(server->IsRunning());
    BENTO_CHECK_EQ(server->Port(), manual_port);

    // 重复 Start 必须失败，且运行状态不被破坏。
    BENTO_CHECK(!server->Start(&error));
    BENTO_CHECK(!error.empty());
    BENTO_CHECK_EQ(server->last_error(), OmniError::kAlreadyStarted);
    BENTO_CHECK(server->IsRunning());

    // 真实客户端能连上。
    UdpClient first;
    BENTO_CHECK(first.Connect(manual_port, 11));
    BENTO_CHECK(WaitFor([&]() { return link.peer_count() == 1; }, 3000));

    server->Stop();
    first.Close();
    BENTO_CHECK(!server->IsRunning());
    BENTO_CHECK_EQ(server->Port(), std::uint16_t{0});

    // Stop 终态：不能重新 Start。
    BENTO_CHECK(!server->Start(&error));
    BENTO_CHECK(!error.empty());

    // 端口重绑：新实例绑定刚释放的端口，真实可用。
    OmniNetConfig rebind = UdpLinkConfig();
    rebind.bind.port = manual_port;
    auto second = CreateStarted(rebind);
    if (second != nullptr) {
        BENTO_CHECK_EQ(second->Port(), manual_port);
        UdpClient client;
        BENTO_CHECK(client.Connect(manual_port, 12));
        BENTO_CHECK(WaitFor([&]() { return second->Connections() == 1; }, 3000));
        client.Close();
        second->Stop();
        BENTO_CHECK_EQ(second->Port(), std::uint16_t{0});
    }
}

// ---------------------------------------------------------------------------
// 5. UDP 错误路径：端口占用 / 非法地址
// ---------------------------------------------------------------------------

void TestUdpStartErrors() {
    PrintCase("UDP：非法地址的 Start 失败");

    // 说明：UDP 的 bind 在 Linux 上受 SO_REUSEADDR 影响，两个实例可以绑定
    // 同一端口，不能作为稳定的"端口占用"错误路径；端口冲突由 HTTP 用例
    // （TCP listen 不允许重复绑定）覆盖。这里只验证地址无法解析。
    OmniNetConfig bad = UdpLinkConfig();
    bad.bind.address = "this is not an address";
    auto bad_server = NetServer::Create(bad, nullptr);
    BENTO_CHECK(bad_server != nullptr);
    if (bad_server != nullptr) {
        std::string error;
        BENTO_CHECK(!bad_server->Start(&error));
        BENTO_CHECK(!error.empty());
        BENTO_CHECK_EQ(bad_server->last_error(), OmniError::kBindFailed);
        BENTO_CHECK(!bad_server->IsRunning());
        BENTO_CHECK_EQ(bad_server->Port(), std::uint16_t{0});
        bad_server->Stop();
        BENTO_CHECK(!bad_server->IsRunning());
    }

    // 合法地址真实监听，确认错误路径没有污染正常实例。
    auto good = CreateStarted(UdpLinkConfig());
    if (good != nullptr) {
        BENTO_CHECK(good->Port() != 0);
        good->Stop();
    }
}

// ---------------------------------------------------------------------------
// 6. TCP HTTP：GET/POST/404/keep-alive
// ---------------------------------------------------------------------------

void TestHttpGetPost() {
    PrintCase("HTTP：真实 TCP GET/POST/404/keep-alive");

    OmniNetConfig config = TcpHttpConfig();
    config.limits.worker_threads = 4;
    config.limits.accept_timeout = milliseconds(100);
    auto server = CreateStarted(config);
    if (server == nullptr) {
        return;
    }
    const std::uint16_t port = server->Port();
    BENTO_CHECK(port != 0);
    auto http = server->AsHttpServer();
    BENTO_CHECK(http.valid());
    BENTO_CHECK(http.backend() != nullptr);
    BENTO_CHECK(http.LocalAddress().find(std::to_string(port)) != std::string::npos);
    BENTO_CHECK(!http.LocalAddress().empty());

    http.OnRequest([](const Request& request) {
        Response response;
        if (request.method == Method::Get && request.path() == "/hello") {
            response.header.set("Content-Type", "text/plain");
            response.body = "hello-omni-m1";
            return response;
        }
        if (request.method == Method::Post && request.path() == "/echo") {
            response.status = Status::Created;
            response.header.set("X-Echo", request.header.get("X-Message"));
            response.body = request.body;
            return response;
        }
        response.status = Status::NotFound;
        response.body = "missing";
        return response;
    });

    HttpClient client;
    BENTO_CHECK(client.Connect(port));
    {
        Response response;
        BENTO_CHECK(client.Send(GetRequest("/hello")));
        BENTO_CHECK(client.ReadResponse(response));
        BENTO_CHECK(response.status == Status::OK);
        BENTO_CHECK_EQ(response.body, std::string("hello-omni-m1"));
        BENTO_CHECK_EQ(response.header.get("Content-Type"), std::string("text/plain"));
    }
    {
        const std::string body = "post-body-中文-42";
        const std::string request = "POST /echo HTTP/1.1\r\nHost: 127.0.0.1\r\n"
                                    "X-Message: omni\r\nContent-Length: " +
                                    std::to_string(body.size()) + "\r\n\r\n" + body;
        Response response;
        BENTO_CHECK(client.Send(request));
        BENTO_CHECK(client.ReadResponse(response));
        BENTO_CHECK(response.status == Status::Created);
        BENTO_CHECK_EQ(response.body, body);
        BENTO_CHECK_EQ(response.header.get("X-Echo"), std::string("omni"));
    }
    {
        // 同一连接上的第三个请求验证 keep-alive 复用。
        Response response;
        BENTO_CHECK(client.Send(GetRequest("/missing")));
        BENTO_CHECK(client.ReadResponse(response));
        BENTO_CHECK(response.status == Status::NotFound);
        BENTO_CHECK_EQ(response.body, std::string("missing"));
    }
    BENTO_CHECK(server->Stats().http_requests >= 3);
    BENTO_CHECK(server->Stats().messages_in >= 3);

    client.Close();
    server->Stop();
    BENTO_CHECK(!server->IsRunning());
    BENTO_CHECK_EQ(server->Port(), std::uint16_t{0});
}

// ---------------------------------------------------------------------------
// 7. TCP SSE 推流
// ---------------------------------------------------------------------------

void TestHttpSse() {
    PrintCase("HTTP：SSE 真实推流（3 个事件按序）");

    OmniNetConfig config = MakeConfig(TransportKind::kTcp,
                                      {ProtocolKind::kHttp, ProtocolKind::kSse});
    config.limits.worker_threads = 4;
    config.limits.accept_timeout = milliseconds(100);
    auto server = CreateStarted(config);
    if (server == nullptr) {
        return;
    }
    auto http = server->AsHttpServer();
    BENTO_CHECK(http.valid());

    std::atomic<bool> handler_returned{false};
    http.AsSse("/events", [&handler_returned](const Request&, Stream& stream) {
        for (int index = 0; index < 3; ++index) {
            Event event;
            event.id = std::to_string(index);
            event.event = "tick";
            event.data = "data-" + std::to_string(index);
            if (!stream.send(event).ok()) {
                handler_returned.store(true);
                return;
            }
            std::this_thread::sleep_for(milliseconds(50));
        }
        handler_returned.store(true);
    });

    HttpClient client;
    BENTO_CHECK(client.Connect(server->Port()));
    BENTO_CHECK(client.Send("GET /events HTTP/1.1\r\nHost: 127.0.0.1\r\n"
                            "Accept: text/event-stream\r\n\r\n"));
    std::string collected;
    BENTO_CHECK(client.ReadUntil("data: data-0", collected, 3000));
    BENTO_CHECK(client.ReadUntil("data: data-2", collected, 3000));
    BENTO_CHECK(collected.find("HTTP/1.1 200") != std::string::npos);
    BENTO_CHECK(collected.find("Content-Type: text/event-stream") != std::string::npos);
    BENTO_CHECK(collected.find("event: tick") != std::string::npos);
    const auto first = collected.find("data: data-0");
    const auto second = collected.find("data: data-1");
    const auto third = collected.find("data: data-2");
    BENTO_CHECK(first != std::string::npos);
    BENTO_CHECK(first < second);
    BENTO_CHECK(second < third);
    BENTO_CHECK(WaitFor([&]() { return handler_returned.load(); }, 3000));
    BENTO_CHECK(server->Stats().sse_events >= 1);

    client.Close();
    server->Stop();
}

// ---------------------------------------------------------------------------
// 8. TCP 并发：8 客户端 × 5 个 keep-alive 请求
// ---------------------------------------------------------------------------

void TestHttpConcurrentClients() {
    PrintCase("HTTP：8 客户端并发 keep-alive（40 个请求）");

    OmniNetConfig config = TcpHttpConfig();
    config.limits.worker_threads = 4;
    config.limits.accept_timeout = milliseconds(100);
    auto server = CreateStarted(config);
    if (server == nullptr) {
        return;
    }
    auto http = server->AsHttpServer();
    http.OnRequest([](const Request& request) {
        Response response;
        response.header.set("Content-Type", "text/plain");
        response.body = "path=" + request.path();
        return response;
    });

    constexpr int kClients = 8;
    constexpr int kRequestsPerClient = 5;
    std::atomic<int> failures{0};
    std::vector<std::thread> threads;
    threads.reserve(kClients);
    for (int client_index = 0; client_index < kClients; ++client_index) {
        threads.emplace_back([&, client_index]() {
            HttpClient client;
            if (!client.Connect(server->Port())) {
                failures.fetch_add(1);
                return;
            }
            for (int request_index = 0; request_index < kRequestsPerClient; ++request_index) {
                const std::string path = "/c" + std::to_string(client_index) + "/r" +
                                         std::to_string(request_index);
                Response response;
                if (!client.Send(GetRequest(path)) || !client.ReadResponse(response)) {
                    failures.fetch_add(1);
                    return;
                }
                if (response.status != Status::OK ||
                    response.body != ("path=" + path)) {
                    failures.fetch_add(1);
                }
            }
            client.Close();
        });
    }
    for (auto& thread : threads) {
        thread.join();
    }
    BENTO_CHECK_EQ(failures.load(), 0);
    BENTO_CHECK(server->Stats().http_requests >=
                static_cast<std::uint64_t>(kClients * kRequestsPerClient));
    server->Stop();
}

// ---------------------------------------------------------------------------
// 9. TCP 停止语义：活动 SSE + 空闲 keep-alive
// ---------------------------------------------------------------------------

void TestHttpStopWithActiveClients() {
    PrintCase("HTTP：活动 SSE + 空闲 keep-alive 时 Stop 不挂起");

    OmniNetConfig config = TcpHttpConfig();
    config.limits.worker_threads = 4;
    config.limits.accept_timeout = milliseconds(100);
    auto server = CreateStarted(config);
    if (server == nullptr) {
        return;
    }
    auto http = server->AsHttpServer();
    std::atomic<bool> sse_stopped{false};
    std::atomic<std::uint64_t> sent{0};
    http.OnRequest([](const Request&) {
        Response response;
        response.body = "idle";
        return response;
    });
    http.AsSse("/stream", [&](const Request&, Stream& stream) {
        for (int index = 0; index < 100000; ++index) {
            Event event;
            event.event = "tick";
            event.data = "tick-" + std::to_string(index);
            if (!stream.send(event).ok()) {
                break;
            }
            sent.fetch_add(1);
            std::this_thread::sleep_for(milliseconds(5));
        }
        sse_stopped.store(true);
    });

    HttpClient sse_client;
    BENTO_CHECK(sse_client.Connect(server->Port()));
    BENTO_CHECK(sse_client.Send("GET /stream HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n"));
    std::string collected;
    BENTO_CHECK(sse_client.ReadUntil("data: tick-0", collected, 3000));

    HttpClient idle_client;
    BENTO_CHECK(idle_client.Connect(server->Port()));
    {
        Response response;
        BENTO_CHECK(idle_client.Send(GetRequest("/idle")));
        BENTO_CHECK(idle_client.ReadResponse(response));
        BENTO_CHECK_EQ(response.body, std::string("idle"));
    }

    const auto begin = steady_clock::now();
    server->Stop();
    const auto cost = steady_clock::now() - begin;
    BENTO_CHECK(cost < milliseconds(3000));
    BENTO_CHECK(!server->IsRunning());
    BENTO_CHECK_EQ(server->Port(), std::uint16_t{0});
    BENTO_CHECK(WaitFor([&]() { return sse_stopped.load(); }, 3000));
    BENTO_CHECK(sent.load() >= 1);
    sse_client.Close();
    idle_client.Close();
    server->Stop();  // 幂等。
}

// ---------------------------------------------------------------------------
// 10. TCP 端口冲突 / 非法地址
// ---------------------------------------------------------------------------

void TestHttpStartErrors() {
    PrintCase("HTTP：端口冲突与非法地址的 Start 失败");

    OmniNetConfig first_config = TcpHttpConfig();
    first_config.limits.accept_timeout = milliseconds(100);
    auto first = CreateStarted(first_config);
    if (first == nullptr) {
        return;
    }
    const std::uint16_t port = first->Port();
    BENTO_CHECK(port != 0);

    OmniNetConfig conflict = TcpHttpConfig();
    conflict.bind.port = port;
    conflict.limits.accept_timeout = milliseconds(100);
    auto second = NetServer::Create(conflict, nullptr);
    BENTO_CHECK(second != nullptr);
    if (second != nullptr) {
        std::string error;
        BENTO_CHECK(!second->Start(&error));
        BENTO_CHECK(!error.empty());
        BENTO_CHECK_EQ(second->last_error(), OmniError::kBindFailed);
        BENTO_CHECK(!second->IsRunning());
        second->Stop();
    }

    OmniNetConfig bad = TcpHttpConfig();
    bad.bind.address = "this is not an address";
    auto bad_server = NetServer::Create(bad, nullptr);
    BENTO_CHECK(bad_server != nullptr);
    if (bad_server != nullptr) {
        std::string error;
        BENTO_CHECK(!bad_server->Start(&error));
        BENTO_CHECK(!error.empty());
        BENTO_CHECK_EQ(bad_server->last_error(), OmniError::kBindFailed);
        bad_server->Stop();
    }
    first->Stop();
}

// ---------------------------------------------------------------------------
// 11. TCP 视图手动 Listen + 端口重绑
// ---------------------------------------------------------------------------

void TestHttpManualListenAndRebind() {
    PrintCase("HTTP：视图手动 Listen / 重复 Listen / Start 复用 / 端口重绑");

    OmniNetConfig config = TcpHttpConfig();
    config.limits.accept_timeout = milliseconds(100);
    auto server = NetServer::Create(config, nullptr);
    if (server == nullptr) {
        return;
    }
    auto http = server->AsHttpServer();
    BENTO_CHECK(http.valid());
    BENTO_CHECK(!http.Listen("this is not an address", nullptr));

    std::string error;
    BENTO_CHECK(http.Listen("127.0.0.1", 0, &error));
    BENTO_CHECK(error.empty());
    BENTO_CHECK(!http.Listen("127.0.0.1", 0, &error));
    BENTO_CHECK(server->Start(&error));
    const std::uint16_t port = server->Port();
    BENTO_CHECK(port != 0);

    http.OnRequest([](const Request&) {
        Response response;
        response.body = "manual-listen";
        return response;
    });
    {
        HttpClient client;
        BENTO_CHECK(client.Connect(port));
        Response response;
        BENTO_CHECK(client.Send(GetRequest("/manual")));
        BENTO_CHECK(client.ReadResponse(response));
        BENTO_CHECK_EQ(response.body, std::string("manual-listen"));
        client.Close();
    }

    server->Stop();
    BENTO_CHECK_EQ(server->Port(), std::uint16_t{0});

    // 端口重绑到刚释放的端口。
    OmniNetConfig rebind = TcpHttpConfig();
    rebind.bind.port = port;
    rebind.limits.accept_timeout = milliseconds(100);
    auto second = CreateStarted(rebind);
    if (second != nullptr) {
        BENTO_CHECK_EQ(second->Port(), port);
        auto second_http = second->AsHttpServer();
        second_http.OnRequest([](const Request&) {
            Response response;
            response.body = "rebound";
            return response;
        });
        HttpClient client;
        BENTO_CHECK(client.Connect(port));
        Response response;
        BENTO_CHECK(client.Send(GetRequest("/rebound")));
        BENTO_CHECK(client.ReadResponse(response));
        BENTO_CHECK_EQ(response.body, std::string("rebound"));
        client.Close();
        second->Stop();
    }
}

// ---------------------------------------------------------------------------
// 12. Memory 兼容（M0 回环不受 M1 影响）
// ---------------------------------------------------------------------------

void TestMemoryCompatibility() {
    PrintCase("Memory：M0 回环兼容（ConnectMemory + PollOnce）");

    OmniNetConfig config = MemoryRawConfig();
    auto server = CreateStarted(config);
    if (server == nullptr) {
        return;
    }
    BENTO_CHECK(server->AsRawServer().valid());
    BENTO_CHECK(!server->AsLinkServer().valid());
    BENTO_CHECK(!server->AsHttpServer().valid());
    BENTO_CHECK_EQ(server->Port(), std::uint16_t{0});

    std::string error;
    auto client = server->ConnectMemory(&error);
    BENTO_CHECK(client != nullptr);
    BENTO_CHECK(error.empty());
    if (client == nullptr) {
        server->Stop();
        return;
    }
    auto accepted = server->AsRawServer().Accept()->TryRecv();
    BENTO_CHECK(accepted.ok);
    if (!accepted.ok) {
        server->Stop();
        return;
    }
    auto connection = *accepted.value;
    BENTO_CHECK(connection.valid());

    // 客户端 → 服务器。
    const std::string request = "memory-m1";
    BENTO_CHECK(
        client
            ->WriteFull(reinterpret_cast<const std::uint8_t*>(request.data()), request.size(),
                        milliseconds(100))
            .ok());
    std::vector<std::uint8_t> received;
    const bool got_request = WaitFor(
        [&]() {
            (void)server->PollOnce(milliseconds(0));
            auto frame = connection.Inbound()->TryRecv();
            if (!frame.ok) {
                return false;
            }
            received = frame.value->bytes;
            return true;
        },
        2000);
    BENTO_CHECK(got_request);
    BENTO_CHECK_EQ(std::string(received.begin(), received.end()), request);

    // 服务器 → 客户端。
    const std::string response = "memory-reply";
    go2cpp::bento::omni::RawFrame frame;
    frame.bytes.assign(response.begin(), response.end());
    BENTO_CHECK(connection.Outbound()->TrySend(std::move(frame)).Ok());
    std::uint8_t buffer[64] = {};
    std::size_t response_bytes = 0;
    const bool got_response = WaitFor(
        [&]() {
            (void)server->PollOnce(milliseconds(0));
            const auto read = client->Read(buffer, sizeof(buffer), milliseconds(0));
            if (!read.ok() || read.bytes == 0) {
                return false;
            }
            response_bytes = read.bytes;
            return true;
        },
        2000);
    BENTO_CHECK(got_response);
    BENTO_CHECK_EQ(
        std::string(reinterpret_cast<const char*>(buffer), response_bytes), response);

    const auto stats = server->Stats();
    BENTO_CHECK(stats.accepted >= 1);
    BENTO_CHECK(stats.messages_in >= 1);
    BENTO_CHECK(stats.messages_out >= 1);
    BENTO_CHECK_EQ(server->PollOnce(milliseconds(0)).error, OmniError::kOk);

    server->Stop();
    BENTO_CHECK(!server->IsRunning());
    BENTO_CHECK(connection.Inbound()->IsClosed());
}

// ---------------------------------------------------------------------------
// 13. 视图寿命：NetServer 析构后仍可安全查询
// ---------------------------------------------------------------------------

void TestViewLifetime() {
    PrintCase("生命周期：NetServer 析构后视图安全查询");

    std::string error;
    OmniNetConfig udp = UdpLinkConfig();
    auto server = NetServer::Create(udp, &error);
    BENTO_CHECK(server != nullptr);
    if (server == nullptr) {
        return;
    }
    auto link = server->AsLinkServer();
    BENTO_CHECK(server->Start(&error));
    const std::uint16_t port = server->Port();
    BENTO_CHECK(port != 0);
    server.reset();

    BENTO_CHECK(link.valid());
    BENTO_CHECK_EQ(link.peer_count(), std::size_t{0});
    BENTO_CHECK_EQ(link.local_port(), std::uint16_t{0});
    BENTO_CHECK(link.Listen("127.0.0.1", 0, &error) == false);

    OmniNetConfig tcp = TcpHttpConfig();
    tcp.limits.accept_timeout = milliseconds(100);
    auto http_server = NetServer::Create(tcp, &error);
    BENTO_CHECK(http_server != nullptr);
    if (http_server == nullptr) {
        return;
    }
    auto http = http_server->AsHttpServer();
    BENTO_CHECK(http_server->Start(&error));
    BENTO_CHECK(http_server->Port() != 0);
    http_server.reset();

    BENTO_CHECK(http.valid());
    BENTO_CHECK_EQ(http.Port(), std::uint16_t{0});
    http.OnRequest([](const Request&) {
        Response response;
        response.body = "after-destroy";
        return response;
    });
    BENTO_CHECK(http.backend() != nullptr);
}

}  // namespace

int main() {
    TestConfigSelection();
    TestUdpRealLoopback();
    TestUdpPskEncryption();
    TestUdpLifecycleAndRebind();
    TestUdpStartErrors();
    TestHttpGetPost();
    TestHttpSse();
    TestHttpConcurrentClients();
    TestHttpStopWithActiveClients();
    TestHttpStartErrors();
    TestHttpManualListenAndRebind();
    TestMemoryCompatibility();
    TestViewLifetime();

    std::printf("\n[SUMMARY] checks=%d failures=%d skips=%d\n", bento_test::g_checks,
                bento_test::g_failures, bento_test::g_skips);
    if (bento_test::g_skips > 0 && bento_test::g_failures == 0) {
        return bento_test::kSkipReturnCode;
    }
    return bento_test::g_failures == 0 ? 0 : 1;
}
