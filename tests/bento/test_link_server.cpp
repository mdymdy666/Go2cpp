/**
 * @file test_link_server.cpp
 * @brief LinkServer 骨架测试：连接/可靠/不可靠/多 peer/断线/关闭/背压/并发。
 * @details 覆盖 docs/modules/bento-roadmap.md §2.2 的 `AsLinkServer()`：
 *
 *   1. Listen/Start/Stop 生命周期与重复 Listen 拒绝；
 *   2. 可靠 STREAM 双向收发（含 1300B 分片边界与 20KB 多分片消息）；
 *   3. 不可靠 DATAGRAM 双向收发与通道隔离；
 *   4. 1 服务端 : 3 客户端，广播 + 单播按 peer 隔离；
 *   5. 客户端静默后服务端按 peer_timeout 回收并回调断开；
 *   6. Stop 语义（断开回调/状态复位/幂等）与重新 Listen 重启；
 *   7. 背压：发送窗口耗尽返回 kWouldBlock，恢复读取后可靠消息全部按序到达；
 *   8. 并发：4 线程同时 Send + 主线程 Broadcast，消息不丢不串；
 *   9. 回调重入（连接回调内发送）与用户回调异常隔离；
 *  10. 参数校验与 endpoint 解析失败；
 *  11. 外部驱动模式（start_receiver_thread=false + RunOnce）。
 *
 * 客户端统一用 `PeerHub`（Bento 多 peer 主动端），因此本测试同时验证
 * LinkServer 与既有 Hub 线格式互通。
 *
 * @author go2cpp contributors
 * @date 2026-10-30
 * @copyright MIT License
 */

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "go2cpp/bento/net/link_server.hpp"
#include "go2cpp/bento/net/peer_hub.hpp"
#include "test_util.hpp"

namespace {

using go2cpp::bento::LinkReliability;
using go2cpp::bento::LinkSendResult;
using go2cpp::bento::LinkServer;
using go2cpp::bento::LinkServerConfig;
using go2cpp::bento::NowMs;
using go2cpp::bento::PeerHub;
using go2cpp::bento::PeerHubConfig;
using go2cpp::bento::PeerState;
using go2cpp::bento::SendStatus;

constexpr std::uint8_t kStreamChannel = 1;
constexpr std::uint8_t kDatagramChannel = 2;
constexpr std::uint32_t kInvalidSeq = 0xFFFFFFFFU;

// ---- 测试用线格式：8B 头（seq/tag/长度/魔数）+ 可校验填充 ----

void WriteU32(std::uint8_t* out, std::uint32_t value) {
    out[0] = static_cast<std::uint8_t>(value >> 24);
    out[1] = static_cast<std::uint8_t>(value >> 16);
    out[2] = static_cast<std::uint8_t>(value >> 8);
    out[3] = static_cast<std::uint8_t>(value);
}

std::uint32_t ReadU32(const std::uint8_t* in) {
    return (static_cast<std::uint32_t>(in[0]) << 24) | (static_cast<std::uint32_t>(in[1]) << 16) |
           (static_cast<std::uint32_t>(in[2]) << 8) | static_cast<std::uint32_t>(in[3]);
}

std::vector<std::uint8_t> MakePayload(std::uint32_t seq, std::uint8_t tag, std::size_t bytes) {
    const std::size_t total = std::max<std::size_t>(bytes, 8);
    std::vector<std::uint8_t> data(total, 0);
    WriteU32(data.data(), seq);
    data[4] = tag;
    data[5] = static_cast<std::uint8_t>(total & 0xFFU);
    data[6] = static_cast<std::uint8_t>((total >> 8) & 0xFFU);
    data[7] = 0x5A;
    for (std::size_t i = 8; i < total; ++i) {
        data[i] = static_cast<std::uint8_t>((seq * 31U + i * 7U + tag) & 0xFFU);
    }
    return data;
}

/// 校验并返回 seq；不合法返回 `kInvalidSeq`。
std::uint32_t VerifyPayload(const std::uint8_t* data, std::size_t bytes, std::uint8_t tag) {
    if (bytes < 8 || data[4] != tag || data[7] != 0x5A) {
        return kInvalidSeq;
    }
    const std::size_t declared =
        static_cast<std::size_t>(data[5]) | (static_cast<std::size_t>(data[6]) << 8);
    if (declared != bytes) {
        return kInvalidSeq;
    }
    const std::uint32_t seq = ReadU32(data);
    for (std::size_t i = 8; i < bytes; ++i) {
        if (data[i] != static_cast<std::uint8_t>((seq * 31U + i * 7U + tag) & 0xFFU)) {
            return kInvalidSeq;
        }
    }
    return seq;
}

// ---- 客户端（PeerHub 主动端） ----

PeerHubConfig MakeClientConfig() {
    PeerHubConfig config;
    // LinkServer 的既有用例是明文链路；显式关闭默认加密策略。
    config.require_encryption = false;
    config.bind_address = "127.0.0.1";
    config.bind_port = 0;
    config.heartbeat_interval_ms = 100;
    config.peer_timeout_ms = 10000;
    config.handshake_timeout_ms = 3000;
    config.hello_initial_pto_ms = 50;
    config.hello_max_pto_ms = 500;
    config.hello_max_attempts = 16;
    return config;
}

LinkServerConfig MakeServerConfig() {
    LinkServerConfig config;
    config.hub.bind_address = "127.0.0.1";
    config.hub.bind_port = 0;
    config.hub.heartbeat_interval_ms = 100;
    config.hub.peer_timeout_ms = 1500;
    config.hub.handshake_timeout_ms = 3000;
    config.hub.hello_initial_pto_ms = 50;
    config.hub.hello_max_pto_ms = 500;
    config.hub.hello_max_attempts = 16;
    config.hub.max_peers = 8;
    config.hub.require_encryption = false;
    config.poll_timeout_ms = 2;
    return config;
}

struct Received {
    std::vector<std::uint8_t> payload;
    std::uint8_t channel = 0;
};

struct TestClient {
    PeerHub hub;
    std::uint32_t peer_id = 1;
    bool opened = false;
    bool connected = false;
    std::vector<Received> streams;
    std::vector<Received> datagrams;

    TestClient() : hub(MakeClientConfig()) {
        std::string error;
        opened = hub.Open(&error);
        if (!opened) {
            std::fprintf(stderr, "[client] hub.Open failed: %s\n", error.c_str());
        }
        hub.on_peer_connected = [this](std::uint32_t) { connected = true; };
        hub.on_stream = [this](std::uint32_t, std::uint8_t channel, const std::uint8_t* data,
                               std::size_t bytes) {
            streams.push_back(Received{std::vector<std::uint8_t>(data, data + bytes), channel});
        };
        hub.on_datagram = [this](std::uint32_t, std::uint8_t channel, const std::uint8_t* data,
                                 std::size_t bytes) {
            datagrams.push_back(Received{std::vector<std::uint8_t>(data, data + bytes), channel});
        };
    }

    bool Connect(std::uint16_t port, std::uint32_t id) {
        peer_id = id;
        if (!opened) {
            return false;
        }
        std::string error;
        if (!hub.AddPeer(peer_id, "127.0.0.1", port, &error)) {
            std::fprintf(stderr, "[client] AddPeer failed: %s\n", error.c_str());
            return false;
        }
        return PumpUntil([this]() { return connected; }, 5000);
    }

    template <typename Predicate>
    bool PumpUntil(Predicate done, int timeout_ms) {
        const std::uint64_t deadline =
            NowMs() + static_cast<std::uint64_t>(std::max(timeout_ms, 0));
        while (NowMs() < deadline) {
            hub.RunOnce(1);
            if (done()) {
                return true;
            }
        }
        return done();
    }

    void PumpFor(int duration_ms) {
        const std::uint64_t deadline =
            NowMs() + static_cast<std::uint64_t>(std::max(duration_ms, 0));
        while (NowMs() < deadline) {
            hub.RunOnce(1);
        }
    }

    bool SendReliable(std::uint8_t channel, const std::vector<std::uint8_t>& payload,
                      int timeout_ms = 5000) {
        const std::uint64_t deadline =
            NowMs() + static_cast<std::uint64_t>(std::max(timeout_ms, 0));
        while (NowMs() < deadline) {
            const SendStatus status =
                hub.TrySendStream(peer_id, channel, payload.data(), payload.size());
            if (status == SendStatus::kOk) {
                return true;
            }
            if (status == SendStatus::kError) {
                return false;
            }
            hub.RunOnce(1);
        }
        return false;
    }

    bool SendUnreliable(std::uint8_t channel, const std::vector<std::uint8_t>& payload) {
        return hub.SendDatagram(peer_id, channel, payload.data(), payload.size());
    }
};

// ---- 服务端事件探针（接收线程回调，主线程读取，必须加锁） ----

struct PeerEvents {
    mutable std::mutex mutex;
    std::vector<std::uint32_t> connected;
    std::vector<std::pair<std::uint32_t, std::string>> disconnected;

    void Install(LinkServer& server) {
        server.OnPeerConnected([this](std::uint32_t peer) {
            std::lock_guard<std::mutex> lock(mutex);
            connected.push_back(peer);
        });
        server.OnPeerDisconnected([this](std::uint32_t peer, const char* reason) {
            std::lock_guard<std::mutex> lock(mutex);
            disconnected.emplace_back(peer,
                                      reason == nullptr ? std::string() : std::string(reason));
        });
    }

    std::size_t ConnectedCount() const {
        std::lock_guard<std::mutex> lock(mutex);
        return connected.size();
    }

    std::size_t DisconnectedCount() const {
        std::lock_guard<std::mutex> lock(mutex);
        return disconnected.size();
    }

    std::vector<std::uint32_t> Connected() const {
        std::lock_guard<std::mutex> lock(mutex);
        return connected;
    }

    std::string LastDisconnectReason() const {
        std::lock_guard<std::mutex> lock(mutex);
        return disconnected.empty() ? std::string() : disconnected.back().second;
    }

    void Clear() {
        std::lock_guard<std::mutex> lock(mutex);
        connected.clear();
        disconnected.clear();
    }
};

template <typename Predicate>
bool WaitFor(Predicate done, int timeout_ms) {
    const std::uint64_t deadline = NowMs() + static_cast<std::uint64_t>(std::max(timeout_ms, 0));
    while (NowMs() < deadline) {
        if (done()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return done();
}

using ClientList = std::vector<std::unique_ptr<TestClient>>;

/// 交替驱动多个客户端，直到条件满足或超时（多 peer 场景必须同时喂心跳/ACK）。
template <typename Predicate>
bool PumpClientsUntil(const ClientList& clients, Predicate done, int timeout_ms) {
    const std::uint64_t deadline = NowMs() + static_cast<std::uint64_t>(std::max(timeout_ms, 0));
    while (NowMs() < deadline) {
        for (const auto& client : clients) {
            client->hub.RunOnce(1);
        }
        if (done()) {
            return true;
        }
    }
    return done();
}

void PumpClientsFor(const ClientList& clients, int duration_ms) {
    const std::uint64_t deadline = NowMs() + static_cast<std::uint64_t>(std::max(duration_ms, 0));
    while (NowMs() < deadline) {
        for (const auto& client : clients) {
            client->hub.RunOnce(1);
        }
    }
}

// ---- 用例 1：Listen/Start/Stop 生命周期与 peer 查询 ----

void TestListenConnectAndLifecycle() {
    PeerEvents events;
    LinkServer server;
    events.Install(server);

    const LinkServerConfig config = MakeServerConfig();
    std::string error;
    BENTO_CHECK(server.Listen("127.0.0.1", 0, config, &error));
    BENTO_CHECK(error.empty());
    BENTO_CHECK(server.is_open());
    BENTO_CHECK(!server.running());
    const std::uint16_t port = server.local_port();
    BENTO_CHECK(port != 0);

    std::string duplicate_error;
    BENTO_CHECK(!server.Listen("127.0.0.1", 0, config, &duplicate_error));
    BENTO_CHECK(!duplicate_error.empty());
    BENTO_CHECK_EQ(server.local_port(), port);

    BENTO_CHECK(server.Start());
    BENTO_CHECK(server.running());

    TestClient client;
    BENTO_CHECK(client.Connect(port, 1));
    BENTO_CHECK(WaitFor([&]() { return events.ConnectedCount() == 1; }, 3000));
    BENTO_CHECK_EQ(server.peer_count(), std::size_t{1});
    const std::vector<std::uint32_t> peers = server.Peers();
    BENTO_CHECK_EQ(peers.size(), std::size_t{1});
    if (peers.size() == 1) {
        BENTO_CHECK(server.HasPeer(peers[0]));
        BENTO_CHECK(server.peer_state(peers[0]) == PeerState::kConnected);
        BENTO_CHECK(server.peer_state(peers[0] + 1000) == PeerState::kClosed);
    }
    BENTO_CHECK_EQ(server.Stats().peers_accepted, std::uint64_t{1});

    server.Stop();
    BENTO_CHECK(!server.running());
    BENTO_CHECK(!server.is_open());
    BENTO_CHECK_EQ(server.local_port(), 0);
    BENTO_CHECK_EQ(server.peer_count(), 0);
    BENTO_CHECK(WaitFor([&]() { return events.DisconnectedCount() == 1; }, 2000));
    BENTO_CHECK_EQ(server.Stats().peers_disconnected, std::uint64_t{1});
    server.Stop();  // 幂等。
    BENTO_CHECK(!server.running());
    client.hub.Close();
}

// ---- 用例 2：可靠 STREAM 双向收发（单帧/分片边界/多分片） ----

void TestReliableStreamEcho() {
    std::atomic<int> echo_failures{0};
    LinkServer server;
    server.OnStream([&](std::uint32_t peer, std::uint8_t channel, const std::uint8_t* data,
                        std::size_t bytes) {
        if (server.Send(peer, channel, data, bytes, LinkReliability::kReliable) !=
            LinkSendResult::kOk) {
            echo_failures.fetch_add(1);
        }
    });

    const LinkServerConfig config = MakeServerConfig();
    std::string error;
    BENTO_CHECK(server.Listen("127.0.0.1", 0, config, &error));
    BENTO_CHECK(server.Start());

    TestClient client;
    BENTO_CHECK(client.Connect(server.local_port(), 1));

    constexpr std::uint32_t kCount = 30;
    const std::size_t kSizes[] = {16, 64, 1300, 1301, 5000, 20000};
    for (std::uint32_t i = 0; i < kCount; ++i) {
        const std::size_t bytes = kSizes[i % 6] + (i % 3);
        BENTO_CHECK(client.SendReliable(kStreamChannel, MakePayload(i, 0xA1, bytes)));
    }

    BENTO_CHECK(client.PumpUntil([&]() { return client.streams.size() >= kCount; }, 20000));
    BENTO_CHECK_EQ(client.streams.size(), std::size_t{kCount});
    for (std::size_t i = 0; i < client.streams.size(); ++i) {
        BENTO_CHECK_EQ(client.streams[i].channel, kStreamChannel);
        BENTO_CHECK_EQ(VerifyPayload(client.streams[i].payload.data(),
                                     client.streams[i].payload.size(), 0xA1),
                       static_cast<std::uint32_t>(i));
    }
    BENTO_CHECK_EQ(echo_failures.load(), 0);
    const auto stats = server.Stats();
    BENTO_CHECK_EQ(stats.stream_messages_received, std::uint64_t{kCount});
    BENTO_CHECK(stats.sends_ok >= kCount);
    server.Stop();
}

// ---- 用例 3：不可靠 DATAGRAM 双向收发与通道隔离 ----

void TestUnreliableDatagramEcho() {
    std::atomic<int> echo_failures{0};
    LinkServer server;
    server.OnDatagram([&](std::uint32_t peer, std::uint8_t channel, const std::uint8_t* data,
                          std::size_t bytes) {
        if (server.Send(peer, channel, data, bytes, LinkReliability::kUnreliable) !=
            LinkSendResult::kOk) {
            echo_failures.fetch_add(1);
        }
    });

    const LinkServerConfig config = MakeServerConfig();
    std::string error;
    BENTO_CHECK(server.Listen("127.0.0.1", 0, config, &error));
    BENTO_CHECK(server.Start());

    TestClient client;
    BENTO_CHECK(client.Connect(server.local_port(), 1));

    constexpr std::uint32_t kCount = 40;
    for (std::uint32_t i = 0; i < kCount; ++i) {
        const std::size_t bytes = 64 + static_cast<std::size_t>(i % 5) * 17;
        BENTO_CHECK(client.SendUnreliable(kDatagramChannel, MakePayload(i, 0xB2, bytes)));
    }

    BENTO_CHECK(client.PumpUntil([&]() { return client.datagrams.size() >= kCount; }, 10000));
    BENTO_CHECK_EQ(client.datagrams.size(), std::size_t{kCount});
    for (std::size_t i = 0; i < client.datagrams.size(); ++i) {
        BENTO_CHECK_EQ(client.datagrams[i].channel, kDatagramChannel);
        BENTO_CHECK_EQ(VerifyPayload(client.datagrams[i].payload.data(),
                                     client.datagrams[i].payload.size(), 0xB2),
                       static_cast<std::uint32_t>(i));
    }
    BENTO_CHECK_EQ(echo_failures.load(), 0);
    BENTO_CHECK(client.streams.empty());  // STREAM 通道未被 DATAGRAM 污染。
    const auto stats = server.Stats();
    BENTO_CHECK_EQ(stats.datagram_messages_received, std::uint64_t{kCount});
    BENTO_CHECK_EQ(stats.stream_messages_received, std::uint64_t{0});
    server.Stop();
}

// ---- 用例 4：1 服务端 : 3 客户端，广播 + 单播按 peer 隔离 ----

void TestMultiplePeersBroadcastAndIsolation() {
    std::mutex map_mutex;
    std::map<std::uint32_t, std::uint32_t> peer_to_client;  // peer_id -> client 编号
    LinkServer server;
    server.OnStream([&](std::uint32_t peer, std::uint8_t, const std::uint8_t* data,
                        std::size_t bytes) {
        if (bytes >= 8 && data[4] == 0x11) {
            std::lock_guard<std::mutex> lock(map_mutex);
            peer_to_client[peer] = ReadU32(data);  // hello 的 seq 即 client 编号。
        }
    });

    PeerEvents events;
    events.Install(server);
    // 多 peer 场景下主线程按顺序等待，客户端可能短暂不 RunOnce；放宽
    // peer_timeout，避免用例测到「断线」而不是广播语义。
    LinkServerConfig config = MakeServerConfig();
    config.hub.peer_timeout_ms = 10000;
    std::string error;
    BENTO_CHECK(server.Listen("127.0.0.1", 0, config, &error));
    BENTO_CHECK(server.Start());

    constexpr int kClients = 3;
    std::vector<std::unique_ptr<TestClient>> clients;
    for (int i = 0; i < kClients; ++i) {
        auto client = std::make_unique<TestClient>();
        BENTO_CHECK(client->Connect(server.local_port(), 1));
        BENTO_CHECK(client->SendReliable(kStreamChannel,
                                         MakePayload(static_cast<std::uint32_t>(i), 0x11, 32)));
        clients.push_back(std::move(client));
    }
    // 连接事件与 hello 映射都必须在客户端持续 pump 下等待：可靠重传与心跳
    // 依赖 RunOnce，主线程 sleep 等待会让 peer 在服务端看起来「静默」。
    BENTO_CHECK(PumpClientsUntil(
        clients, [&]() { return events.ConnectedCount() == kClients; }, 5000));
    BENTO_CHECK(PumpClientsUntil(
        clients,
        [&]() {
            std::lock_guard<std::mutex> lock(map_mutex);
            return peer_to_client.size() == static_cast<std::size_t>(kClients);
        },
        5000));

    // 可靠广播 -> 3 个客户端全部收到。
    const auto stream_broadcast = MakePayload(1000, 0xC1, 200);
    const std::size_t stream_delivered = server.Broadcast(
        kStreamChannel, stream_broadcast.data(), stream_broadcast.size(),
        LinkReliability::kReliable);
    BENTO_CHECK_EQ(stream_delivered, std::size_t{kClients});
    BENTO_CHECK(PumpClientsUntil(
        clients,
        [&]() {
            for (const auto& client : clients) {
                if (client->streams.empty()) {
                    return false;
                }
            }
            return true;
        },
        5000));
    for (auto& client : clients) {
        BENTO_CHECK_EQ(client->streams.size(), std::size_t{1});
        if (client->streams.empty()) {
            continue;
        }
        BENTO_CHECK_EQ(client->streams[0].channel, kStreamChannel);
        BENTO_CHECK_EQ(VerifyPayload(client->streams[0].payload.data(),
                                     client->streams[0].payload.size(), 0xC1),
                       std::uint32_t{1000});
    }

    // 不可靠广播 -> 3 个客户端全部收到。
    const auto datagram_broadcast = MakePayload(1001, 0xD1, 64);
    const std::size_t datagram_delivered = server.Broadcast(
        kDatagramChannel, datagram_broadcast.data(), datagram_broadcast.size(),
        LinkReliability::kUnreliable);
    BENTO_CHECK_EQ(datagram_delivered, std::size_t{kClients});
    BENTO_CHECK(PumpClientsUntil(
        clients,
        [&]() {
            for (const auto& client : clients) {
                if (client->datagrams.empty()) {
                    return false;
                }
            }
            return true;
        },
        5000));
    for (auto& client : clients) {
        if (client->datagrams.empty()) {
            continue;
        }
        BENTO_CHECK_EQ(VerifyPayload(client->datagrams[0].payload.data(),
                                     client->datagrams[0].payload.size(), 0xD1),
                       std::uint32_t{1001});
    }

    // 只向 client 1 单播：其余客户端不得收到。
    std::uint32_t target_peer = 0;
    {
        std::lock_guard<std::mutex> lock(map_mutex);
        for (const auto& entry : peer_to_client) {
            if (entry.second == 1) {
                target_peer = entry.first;
            }
        }
    }
    BENTO_CHECK(target_peer != 0);
    const auto unicast = MakePayload(2000, 0xE1, 150);
    BENTO_CHECK(server.Send(target_peer, kStreamChannel, unicast.data(), unicast.size(),
                            LinkReliability::kReliable) == LinkSendResult::kOk);
    BENTO_CHECK(
        PumpClientsUntil(clients, [&]() { return clients[1]->streams.size() >= 2; }, 5000));
    PumpClientsFor(clients, 100);
    BENTO_CHECK_EQ(clients[1]->streams.size(), std::size_t{2});
    if (clients[1]->streams.size() >= 2) {
        BENTO_CHECK_EQ(VerifyPayload(clients[1]->streams[1].payload.data(),
                                     clients[1]->streams[1].payload.size(), 0xE1),
                       std::uint32_t{2000});
    }
    BENTO_CHECK_EQ(clients[0]->streams.size(), std::size_t{1});
    BENTO_CHECK_EQ(clients[2]->streams.size(), std::size_t{1});

    const auto stats = server.Stats();
    BENTO_CHECK_EQ(stats.broadcasts, std::uint64_t{2});
    BENTO_CHECK_EQ(stats.broadcast_deliveries,
                  static_cast<std::uint64_t>(stream_delivered + datagram_delivered));
    server.Stop();
    for (auto& client : clients) {
        client->hub.Close();
    }
}

// ---- 用例 5：客户端静默 -> 服务端按 peer_timeout 回收 ----

void TestPeerTimeoutDisconnects() {
    PeerEvents events;
    LinkServer server;
    events.Install(server);
    LinkServerConfig config = MakeServerConfig();
    config.hub.peer_timeout_ms = 300;

    std::string error;
    BENTO_CHECK(server.Listen("127.0.0.1", 0, config, &error));
    BENTO_CHECK(server.Start());

    TestClient client;
    BENTO_CHECK(client.Connect(server.local_port(), 1));
    BENTO_CHECK(WaitFor([&]() { return events.ConnectedCount() == 1; }, 3000));
    const std::vector<std::uint32_t> peers = events.Connected();
    BENTO_CHECK_EQ(peers.size(), std::size_t{1});
    if (peers.empty()) {
        server.Stop();
        return;
    }

    // 客户端不再 RunOnce：不回 ACK、不发心跳，服务端按 peer_timeout 断开。
    BENTO_CHECK(WaitFor([&]() { return events.DisconnectedCount() == 1; }, 5000));
    BENTO_CHECK_EQ(events.LastDisconnectReason(), std::string("peer timeout"));
    BENTO_CHECK(WaitFor([&]() { return server.peer_count() == 0; }, 2000));
    BENTO_CHECK_EQ(server.Stats().peers_disconnected, std::uint64_t{1});

    const auto payload = MakePayload(7, 0x33, 64);
    BENTO_CHECK(server.Send(peers[0], kStreamChannel, payload.data(), payload.size(),
                            LinkReliability::kReliable) == LinkSendResult::kNotConnected);
    BENTO_CHECK_EQ(server.Stats().sends_not_connected, std::uint64_t{1});
    BENTO_CHECK(server.running());  // 服务端本身不受单个 peer 超时影响。
    server.Stop();
    client.hub.Close();
}

// ---- 用例 6：Stop 语义（断开回调/状态复位/幂等）与重新 Listen ----

void TestStopAndRestart() {
    PeerEvents events;
    LinkServer server;
    events.Install(server);
    const LinkServerConfig config = MakeServerConfig();
    std::string error;
    BENTO_CHECK(server.Listen("127.0.0.1", 0, config, &error));
    BENTO_CHECK(server.Start());

    constexpr int kClients = 2;
    std::vector<std::unique_ptr<TestClient>> clients;
    for (int i = 0; i < kClients; ++i) {
        auto client = std::make_unique<TestClient>();
        BENTO_CHECK(client->Connect(server.local_port(), 1));
        clients.push_back(std::move(client));
    }
    BENTO_CHECK(WaitFor([&]() { return server.peer_count() == kClients; }, 5000));

    server.Stop();
    BENTO_CHECK(!server.running());
    BENTO_CHECK(!server.is_open());
    BENTO_CHECK_EQ(server.local_port(), 0);
    BENTO_CHECK_EQ(server.peer_count(), 0);
    BENTO_CHECK(WaitFor([&]() { return events.DisconnectedCount() == kClients; }, 2000));
    BENTO_CHECK_EQ(server.Stats().peers_disconnected, std::uint64_t{kClients});

    const std::uint8_t byte = 1;
    BENTO_CHECK(server.Send(1, kStreamChannel, &byte, 1, LinkReliability::kReliable) ==
                LinkSendResult::kNotConnected);
    BENTO_CHECK_EQ(server.Broadcast(kStreamChannel, &byte, 1, LinkReliability::kReliable), 0);
    BENTO_CHECK(!server.Start());  // socket 已关闭，必须重新 Listen。
    server.Stop();                 // 幂等。

    // 重新 Listen + Start：统计复位，新客户端可连接。
    error.clear();
    BENTO_CHECK(server.Listen("127.0.0.1", 0, config, &error));
    BENTO_CHECK(server.Start());
    BENTO_CHECK(server.running());
    BENTO_CHECK(server.local_port() != 0);
    BENTO_CHECK_EQ(server.Stats().peers_accepted, std::uint64_t{0});

    events.Clear();
    TestClient fresh;
    BENTO_CHECK(fresh.Connect(server.local_port(), 1));
    BENTO_CHECK(WaitFor([&]() { return events.ConnectedCount() == 1; }, 3000));
    BENTO_CHECK_EQ(server.peer_count(), std::size_t{1});

    const auto payload = MakePayload(1, 0x44, 96);
    BENTO_CHECK(server.Send(events.Connected()[0], kStreamChannel, payload.data(),
                            payload.size(), LinkReliability::kReliable) == LinkSendResult::kOk);
    BENTO_CHECK(fresh.PumpUntil([&]() { return !fresh.streams.empty(); }, 3000));
    if (!fresh.streams.empty()) {
        BENTO_CHECK_EQ(VerifyPayload(fresh.streams[0].payload.data(),
                                     fresh.streams[0].payload.size(), 0x44),
                       std::uint32_t{1});
    }

    server.Stop();
    fresh.hub.Close();
    for (auto& client : clients) {
        client->hub.Close();
    }
}

// ---- 用例 7：背压（发送窗口耗尽 -> kWouldBlock -> 恢复后全量按序到达） ----

void TestBackpressureThenRecovery() {
    LinkServer server;
    LinkServerConfig config = MakeServerConfig();
    // 本用例刻意让客户端一段时间不 RunOnce（不回收 ACK），服务端必须
    // 在整段观察期内保持 peer 存活，否则测的就是断线而不是背压。
    config.hub.peer_timeout_ms = 60000;
    std::string error;
    BENTO_CHECK(server.Listen("127.0.0.1", 0, config, &error));
    BENTO_CHECK(server.Start());

    TestClient client;
    BENTO_CHECK(client.Connect(server.local_port(), 1));
    const std::uint32_t peer = 1;

    constexpr std::uint32_t kTotal = 400;
    std::vector<std::vector<std::uint8_t>> payloads;
    payloads.reserve(kTotal);
    for (std::uint32_t i = 0; i < kTotal; ++i) {
        payloads.push_back(MakePayload(i, 0x77, 1000));
    }

    // 客户端不 RunOnce（不回收 ACK），服务端发送窗口（256 条）必然耗尽。
    std::size_t accepted = 0;
    std::uint64_t would_block = 0;
    while (accepted < kTotal) {
        const auto result =
            server.Send(peer, kStreamChannel, payloads[accepted].data(),
                        payloads[accepted].size(), LinkReliability::kReliable);
        if (result == LinkSendResult::kWouldBlock) {
            ++would_block;
            break;
        }
        BENTO_CHECK(result == LinkSendResult::kOk);
        if (result != LinkSendResult::kOk) {
            break;
        }
        ++accepted;
    }
    BENTO_CHECK(would_block > 0);
    BENTO_CHECK(accepted < kTotal);
    BENTO_CHECK_EQ(server.Stats().sends_would_block, would_block);

    // 客户端恢复读取；生产者在 WouldBlock 时短暂 pump 后重试同一条消息。
    while (accepted < kTotal) {
        const auto result =
            server.Send(peer, kStreamChannel, payloads[accepted].data(),
                        payloads[accepted].size(), LinkReliability::kReliable);
        if (result == LinkSendResult::kOk) {
            ++accepted;
            continue;
        }
        if (result != LinkSendResult::kWouldBlock) {
            std::fprintf(stderr, "[backpressure] unexpected send result: %s\n",
                         go2cpp::bento::LinkSendResultName(result));
            BENTO_CHECK(result == LinkSendResult::kWouldBlock);
            break;
        }
        client.PumpFor(1);
    }
    BENTO_CHECK_EQ(accepted, std::size_t{kTotal});
    BENTO_CHECK(client.PumpUntil([&]() { return client.streams.size() >= kTotal; }, 30000));
    BENTO_CHECK_EQ(client.streams.size(), std::size_t{kTotal});
    for (std::size_t i = 0; i < client.streams.size(); ++i) {
        BENTO_CHECK_EQ(VerifyPayload(client.streams[i].payload.data(),
                                     client.streams[i].payload.size(), 0x77),
                       static_cast<std::uint32_t>(i));
    }
    server.Stop();
}

// ---- 用例 8：多线程并发 Send + 主线程 Broadcast ----

void TestConcurrentSenders() {
    std::atomic<int> bad_payloads{0};
    LinkServer server;
    server.OnStream([&](std::uint32_t, std::uint8_t, const std::uint8_t* data,
                        std::size_t bytes) {
        if (bytes < 8) {
            bad_payloads.fetch_add(1);
            return;
        }
        const std::uint8_t tag = data[4];
        if (tag < 0x90 || tag > 0x93 || VerifyPayload(data, bytes, tag) == kInvalidSeq) {
            bad_payloads.fetch_add(1);
        }
    });
    const LinkServerConfig config = MakeServerConfig();
    std::string error;
    BENTO_CHECK(server.Listen("127.0.0.1", 0, config, &error));
    BENTO_CHECK(server.Start());

    TestClient client;
    BENTO_CHECK(client.Connect(server.local_port(), 1));
    const std::uint32_t peer = 1;

    constexpr int kThreads = 4;
    constexpr std::uint32_t kPerThread = 50;
    std::atomic<int> send_failures{0};
    std::atomic<int> done_threads{0};
    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&, t]() {
            for (std::uint32_t seq = 0; seq < kPerThread; ++seq) {
                const auto payload =
                    MakePayload(seq, static_cast<std::uint8_t>(0x90 + t), 200);
                for (;;) {
                    const auto result = server.Send(peer, kStreamChannel, payload.data(),
                                                    payload.size(), LinkReliability::kReliable);
                    if (result == LinkSendResult::kOk) {
                        break;
                    }
                    if (result != LinkSendResult::kWouldBlock) {
                        send_failures.fetch_add(1);
                        break;
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
            }
            done_threads.fetch_add(1);
        });
    }
    // 主线程持续驱动客户端，避免发送窗口/ACK 停摆。
    while (done_threads.load() < kThreads) {
        client.PumpFor(1);
    }
    for (auto& thread : threads) {
        thread.join();
    }
    BENTO_CHECK_EQ(send_failures.load(), 0);

    // 主线程同时广播不可靠帧。
    constexpr std::uint32_t kBroadcasts = 10;
    for (std::uint32_t i = 0; i < kBroadcasts; ++i) {
        const auto payload = MakePayload(i, 0xD5, 64);
        BENTO_CHECK_EQ(server.Broadcast(kDatagramChannel, payload.data(), payload.size(),
                                        LinkReliability::kUnreliable),
                       std::size_t{1});
    }

    constexpr std::size_t kExpectedStreams = kThreads * kPerThread;
    BENTO_CHECK(client.PumpUntil(
        [&]() {
            return client.streams.size() >= kExpectedStreams &&
                   client.datagrams.size() >= kBroadcasts;
        },
        20000));
    BENTO_CHECK_EQ(client.streams.size(), kExpectedStreams);
    BENTO_CHECK_EQ(client.datagrams.size(), std::size_t{kBroadcasts});

    std::set<std::pair<int, std::uint32_t>> seen;
    for (const auto& received : client.streams) {
        if (received.payload.size() < 8) {
            BENTO_CHECK(false);
            continue;
        }
        const std::uint8_t tag = received.payload[4];
        BENTO_CHECK(tag >= 0x90 && tag <= 0x93);
        const std::uint32_t seq =
            VerifyPayload(received.payload.data(), received.payload.size(), tag);
        if (seq == kInvalidSeq) {
            BENTO_CHECK(false);
            continue;
        }
        seen.emplace(static_cast<int>(tag - 0x90), seq);
    }
    BENTO_CHECK_EQ(seen.size(), kExpectedStreams);
    for (int t = 0; t < kThreads; ++t) {
        for (std::uint32_t seq = 0; seq < kPerThread; ++seq) {
            BENTO_CHECK(seen.count({t, seq}) == 1);
        }
    }
    BENTO_CHECK_EQ(bad_payloads.load(), 0);
    BENTO_CHECK(server.Stats().sends_ok >= kExpectedStreams);
    server.Stop();
}

// ---- 用例 9：连接回调内发送（重入）与用户回调异常隔离 ----

void TestCallbackReentrancyAndExceptionIsolation() {
    std::atomic<int> callback_failures{0};
    std::atomic<int> stream_calls{0};
    LinkServer server;
    server.OnPeerConnected([&](std::uint32_t peer) {
        const auto welcome = MakePayload(0xF0F0U, 0xF1, 128);
        if (server.Send(peer, kStreamChannel, welcome.data(), welcome.size(),
                        LinkReliability::kReliable) != LinkSendResult::kOk) {
            callback_failures.fetch_add(1);
        }
    });
    server.OnStream([&](std::uint32_t peer, std::uint8_t channel, const std::uint8_t* data,
                        std::size_t bytes) {
        const int call = stream_calls.fetch_add(1) + 1;
        if (call == 1) {
            throw std::runtime_error("intentional test exception");
        }
        if (server.Send(peer, channel, data, bytes, LinkReliability::kReliable) !=
            LinkSendResult::kOk) {
            callback_failures.fetch_add(1);
        }
    });

    const LinkServerConfig config = MakeServerConfig();
    std::string error;
    BENTO_CHECK(server.Listen("127.0.0.1", 0, config, &error));
    BENTO_CHECK(server.Start());

    TestClient client;
    BENTO_CHECK(client.Connect(server.local_port(), 1));

    // 第 1 条：用户回调抛异常；第 2/3 条正常 echo。
    BENTO_CHECK(client.SendReliable(kStreamChannel, MakePayload(1, 0xF2, 100)));
    BENTO_CHECK(client.SendReliable(kStreamChannel, MakePayload(2, 0xF2, 110)));
    BENTO_CHECK(client.SendReliable(kStreamChannel, MakePayload(3, 0xF2, 120)));

    BENTO_CHECK(client.PumpUntil([&]() { return client.streams.size() >= 3; }, 10000));
    BENTO_CHECK_EQ(client.streams.size(), std::size_t{3});
    if (client.streams.size() == 3) {
        BENTO_CHECK_EQ(VerifyPayload(client.streams[0].payload.data(),
                                     client.streams[0].payload.size(), 0xF1),
                       std::uint32_t{0xF0F0U});
        BENTO_CHECK_EQ(VerifyPayload(client.streams[1].payload.data(),
                                     client.streams[1].payload.size(), 0xF2),
                       std::uint32_t{2});
        BENTO_CHECK_EQ(VerifyPayload(client.streams[2].payload.data(),
                                     client.streams[2].payload.size(), 0xF2),
                       std::uint32_t{3});
    }
    BENTO_CHECK_EQ(callback_failures.load(), 0);
    BENTO_CHECK_EQ(server.Stats().callback_exceptions, std::uint64_t{1});
    BENTO_CHECK(server.running());
    BENTO_CHECK_EQ(server.peer_count(), std::size_t{1});
    server.Stop();
}

// ---- 用例 10：参数校验与 endpoint 解析 ----

void TestValidationAndEndpointParsing() {
    LinkServer server;
    const std::uint8_t byte = 1;
    BENTO_CHECK(!server.Start());
    BENTO_CHECK(!server.is_open());
    BENTO_CHECK_EQ(server.local_port(), 0);
    BENTO_CHECK(server.Send(1, kStreamChannel, &byte, 1, LinkReliability::kReliable) ==
                LinkSendResult::kNotConnected);
    BENTO_CHECK_EQ(server.Broadcast(kStreamChannel, &byte, 1, LinkReliability::kReliable), 0);
    BENTO_CHECK(server.peer_state(1) == PeerState::kClosed);
    BENTO_CHECK(server.Peers().empty());

    std::string error;
    BENTO_CHECK(!server.Listen("127.0.0.1:abc", LinkServerConfig{}, &error));
    BENTO_CHECK(!error.empty());
    error.clear();
    BENTO_CHECK(!server.Listen("", LinkServerConfig{}, &error));
    BENTO_CHECK(!error.empty());
    error.clear();
    BENTO_CHECK(!server.Listen("host:99999", LinkServerConfig{}, &error));
    BENTO_CHECK(!error.empty());
    error.clear();
    BENTO_CHECK(!server.Listen("[::1", LinkServerConfig{}, &error));
    BENTO_CHECK(!error.empty());
    error.clear();
    BENTO_CHECK(!server.Listen(":9000", LinkServerConfig{}, &error));
    BENTO_CHECK(!error.empty());

    // endpoint 形式（kernel 分配端口）与通配地址。
    error.clear();
    BENTO_CHECK(server.Listen("127.0.0.1", 0, MakeServerConfig(), &error));
    BENTO_CHECK(server.Start());
    BENTO_CHECK(server.local_port() != 0);

    BENTO_CHECK(server.Send(42, kStreamChannel, &byte, 1, LinkReliability::kReliable) ==
                LinkSendResult::kNotConnected);
    BENTO_CHECK(server.Send(42, kStreamChannel, nullptr, 1, LinkReliability::kReliable) ==
                LinkSendResult::kInvalid);
    BENTO_CHECK(server.Send(42, kStreamChannel, &byte, 1, LinkReliability::kUnreliable) ==
                LinkSendResult::kNotConnected);
    BENTO_CHECK(server.Send(42, kStreamChannel, &byte, 0, LinkReliability::kReliable) ==
                LinkSendResult::kInvalid);
    BENTO_CHECK(server.Send(42, kStreamChannel, &byte, 1,
                            static_cast<LinkReliability>(7)) == LinkSendResult::kInvalid);

    const std::vector<std::uint8_t> too_big_datagram(
        go2cpp::bento::kMaxFramePayload + 1, 0);
    BENTO_CHECK(server.Send(42, kDatagramChannel, too_big_datagram.data(),
                            too_big_datagram.size(),
                            LinkReliability::kUnreliable) == LinkSendResult::kInvalid);
    const std::vector<std::uint8_t> too_big_stream(
        go2cpp::bento::kMaxStreamMessageBytes + 1, 0);
    BENTO_CHECK(server.Send(42, kStreamChannel, too_big_stream.data(), too_big_stream.size(),
                            LinkReliability::kReliable) == LinkSendResult::kInvalid);
    BENTO_CHECK_EQ(server.Broadcast(kStreamChannel, nullptr, 0, LinkReliability::kReliable), 0);
    BENTO_CHECK_EQ(server.Stats().sends_invalid, std::uint64_t{5});
    BENTO_CHECK_EQ(server.Stats().sends_not_connected, std::uint64_t{2});

    // 重复 Listen（未 Stop）必须失败。
    error.clear();
    BENTO_CHECK(!server.Listen("127.0.0.1", 0, MakeServerConfig(), &error));
    BENTO_CHECK(!error.empty());
    server.Stop();
}

// ---- 用例 11：外部驱动模式（不建线程，调用方 RunOnce） ----

void TestExternalDriverMode() {
    LinkServer server;
    LinkServerConfig config = MakeServerConfig();
    config.start_receiver_thread = false;

    std::string error;
    BENTO_CHECK(server.Listen("127.0.0.1", 0, config, &error));
    BENTO_CHECK(server.Start());
    BENTO_CHECK(server.running());

    std::atomic<int> received{0};
    std::atomic<int> send_failures{0};
    server.OnStream([&](std::uint32_t peer, std::uint8_t channel, const std::uint8_t* data,
                        std::size_t bytes) {
        received.fetch_add(1);
        if (server.Send(peer, channel, data, bytes, LinkReliability::kReliable) !=
            LinkSendResult::kOk) {
            send_failures.fetch_add(1);
        }
    });

    TestClient client;
    BENTO_CHECK(client.opened);
    BENTO_CHECK(client.hub.AddPeer(1, "127.0.0.1", server.local_port(), &error));
    const std::uint64_t handshake_deadline = NowMs() + 5000;
    while (NowMs() < handshake_deadline && !client.connected) {
        server.RunOnce(1);
        client.hub.RunOnce(1);
    }
    BENTO_CHECK(client.connected);

    const auto payload = MakePayload(9, 0x66, 256);
    BENTO_CHECK(client.SendReliable(kStreamChannel, payload));
    const std::uint64_t echo_deadline = NowMs() + 5000;
    while (NowMs() < echo_deadline && client.streams.empty()) {
        server.RunOnce(1);
        client.hub.RunOnce(1);
    }
    BENTO_CHECK_EQ(received.load(), 1);
    BENTO_CHECK_EQ(send_failures.load(), 0);
    BENTO_CHECK_EQ(client.streams.size(), std::size_t{1});
    if (!client.streams.empty()) {
        BENTO_CHECK_EQ(VerifyPayload(client.streams[0].payload.data(),
                                     client.streams[0].payload.size(), 0x66),
                       std::uint32_t{9});
    }

    server.Stop();
    BENTO_CHECK(!server.running());
    BENTO_CHECK(!server.is_open());
    client.hub.Close();
}

}  // namespace

int main() {
    struct TestCase {
        const char* name;
        void (*run)();
    };
    const TestCase cases[] = {
        {"listen/connect/lifecycle", TestListenConnectAndLifecycle},
        {"reliable stream echo", TestReliableStreamEcho},
        {"unreliable datagram echo", TestUnreliableDatagramEcho},
        {"multi-peer broadcast/isolation", TestMultiplePeersBroadcastAndIsolation},
        {"peer timeout disconnect", TestPeerTimeoutDisconnects},
        {"stop/restart semantics", TestStopAndRestart},
        {"backpressure then recovery", TestBackpressureThenRecovery},
        {"concurrent senders", TestConcurrentSenders},
        {"callback reentrancy/exception", TestCallbackReentrancyAndExceptionIsolation},
        {"validation/endpoint parsing", TestValidationAndEndpointParsing},
        {"external driver mode", TestExternalDriverMode},
    };
    for (const TestCase& test : cases) {
        const std::uint64_t started_ms = go2cpp::bento::NowMs();
        const int failures_before = bento_test::g_failures;
        test.run();
        std::printf("[case] %-34s %6llu ms  failures=%d\n", test.name,
                    static_cast<unsigned long long>(go2cpp::bento::NowMs() - started_ms),
                    bento_test::g_failures - failures_before);
    }
    return bento_test::Finish("bento_link_server");
}
