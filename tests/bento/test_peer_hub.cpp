/**
 * @file test_peer_hub.cpp
 * @brief 多 peer 链路测试：1 个 socket 对 3 个 peer，可靠/不可靠混跑且按 peer 隔离。
 * @details 覆盖 JinShan 的「1 被控 : N 主控」场景（FR-06 / B2-2 / AX-085）：
 *          - 场景 1：被控端一个 socket 接 3 个主控，各自 50 条 STREAM + 50 条
 *            DATAGRAM，断言消息按 peer 隔离、无串流（JinShan RISK-030 反例）。
 *          - 场景 2：伪造 conn_id 的数据帧被丢弃，不影响已建立会话。
 *          - 场景 3：一个主控静默后按 peer_timeout 独立回收，其余 peer 不受影响。
 *          - 场景 4：AddPeer 参数校验（重复 id / 0 / 未 Open）。
 *          - 场景 5：3 主控 × 200 条 × 512B 的吞吐与全量送达。
 * @author go2cpp contributors
 * @date 2026-10-27
 * @copyright MIT License
 */

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "go2cpp/bento/net/peer_hub.hpp"
#include "test_util.hpp"

namespace {

using go2cpp::bento::FrameHeader;
using go2cpp::bento::FrameType;
using go2cpp::bento::PeerHub;
using go2cpp::bento::PeerHubConfig;
using go2cpp::bento::PeerState;

constexpr std::uint32_t kMessagesPerPeer = 50;
constexpr std::uint32_t kDatagramsPerPeer = 50;
constexpr std::size_t kPayloadBytes = 64;
constexpr std::uint8_t kStreamTag = 0xA1;
constexpr std::uint8_t kDatagramTag = 0xB2;
constexpr std::uint8_t kReplyTag = 0xC3;

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

std::uint32_t CheckValue(std::uint32_t client, std::uint32_t seq, std::uint8_t tag) {
    return (client * 2654435761U) ^ seq ^ tag;
}

std::vector<std::uint8_t> MakeMessage(std::uint32_t client, std::uint32_t seq, std::uint8_t tag) {
    std::vector<std::uint8_t> data(kPayloadBytes, 0);
    WriteU32(data.data(), client);
    WriteU32(data.data() + 4, seq);
    WriteU32(data.data() + 8, CheckValue(client, seq, tag));
    for (std::size_t i = 12; i < data.size(); ++i) {
        data[i] = static_cast<std::uint8_t>((client * 31U + seq * 7U + i * 3U) & 0xFFU);
    }
    return data;
}

/// 校验消息三元组，返回 payload 里的 client 编号（失败返回 0xFFFFFFFF）。
std::uint32_t VerifyMessage(const std::uint8_t* data, std::size_t bytes, std::uint8_t tag) {
    if (bytes != kPayloadBytes) {
        return 0xFFFFFFFFU;
    }
    const std::uint32_t client = ReadU32(data);
    const std::uint32_t seq = ReadU32(data + 4);
    if (ReadU32(data + 8) != CheckValue(client, seq, tag)) {
        return 0xFFFFFFFFU;
    }
    for (std::size_t i = 12; i < bytes; ++i) {
        const std::uint8_t expected =
            static_cast<std::uint8_t>((client * 31U + seq * 7U + i * 3U) & 0xFFU);
        if (data[i] != expected) {
            return 0xFFFFFFFFU;
        }
    }
    return client;
}

/// 多 hub 交替驱动，直到 done 为真或超时。
bool PumpHubs(const std::vector<PeerHub*>& hubs, const std::function<bool()>& done, int timeout_ms) {
    const std::uint64_t deadline =
        go2cpp::bento::NowMs() + static_cast<std::uint64_t>(timeout_ms);
    while (go2cpp::bento::NowMs() < deadline) {
        for (PeerHub* hub : hubs) {
            hub->RunOnce(1);
        }
        if (done()) {
            return true;
        }
    }
    return done();
}

/// 多 hub 交替驱动固定时长（不判断条件）。
void PumpFor(const std::vector<PeerHub*>& hubs, int duration_ms) {
    const std::uint64_t deadline =
        go2cpp::bento::NowMs() + static_cast<std::uint64_t>(duration_ms);
    while (go2cpp::bento::NowMs() < deadline) {
        for (PeerHub* hub : hubs) {
            hub->RunOnce(1);
        }
    }
}

PeerHubConfig MakeConfig(const char* address) {
    PeerHubConfig config;
    config.bind_address = address;
    config.bind_port = 0;
    config.heartbeat_interval_ms = 100;
    config.peer_timeout_ms = 5000;
    config.max_peers = 8;
    config.accept_unknown_peers = true;
    return config;
}

/// 场景 1：1 被控 3 主控，双向 STREAM/DATAGRAM 各 50 条，断言按 peer 隔离。
void TestOneServerThreePeers() {
    PeerHub server(MakeConfig("127.0.0.1"));
    std::string error;
    BENTO_CHECK(server.Open(&error));

    constexpr int kClients = 3;
    std::vector<std::unique_ptr<PeerHub>> clients;
    for (int i = 0; i < kClients; ++i) {
        clients.push_back(std::make_unique<PeerHub>(MakeConfig("127.0.0.1")));
        BENTO_CHECK(clients.back()->Open(&error));
        BENTO_CHECK(clients.back()->AddPeer(1, "127.0.0.1", server.local_port(), &error));
    }

    std::vector<PeerHub*> hubs{&server};
    for (auto& client : clients) {
        hubs.push_back(client.get());
    }

    // ---- 服务端观测状态（单线程驱动，无需锁） ----
    std::map<std::uint32_t, std::uint32_t> peer_client;       // peer_id → client 编号
    std::map<std::uint32_t, std::uint32_t> stream_expected;   // peer_id → 下一个 seq
    std::map<std::uint32_t, std::uint32_t> datagram_expected;
    int stream_total = 0;
    int datagram_total = 0;
    int bad_payload = 0;
    int cross_talk = 0;
    int out_of_order = 0;

    server.on_stream = [&](std::uint32_t peer_id, std::uint8_t channel, const std::uint8_t* data,
                           std::size_t bytes) {
        if (channel != 0) {
            ++bad_payload;
            return;
        }
        const std::uint32_t client = VerifyMessage(data, bytes, kStreamTag);
        if (client == 0xFFFFFFFFU) {
            ++bad_payload;
            return;
        }
        const auto bound = peer_client.find(peer_id);
        if (bound == peer_client.end()) {
            peer_client[peer_id] = client;
        } else if (bound->second != client) {
            // 同一个 peer 混进了别的客户端的数据 —— 即“串流”。
            ++cross_talk;
            return;
        }
        const std::uint32_t seq = ReadU32(data + 4);
        std::uint32_t& expected = stream_expected[peer_id];
        if (seq != expected) {
            ++out_of_order;
            return;
        }
        ++expected;
        ++stream_total;
        // 回一条可靠响应（channel 2），客户端据此确认回程路由也没串。
        const std::vector<std::uint8_t> reply = MakeMessage(client, seq, kReplyTag);
        (void)server.SendStream(peer_id, 2, reply.data(), reply.size());
    };
    server.on_datagram = [&](std::uint32_t peer_id, std::uint8_t channel, const std::uint8_t* data,
                             std::size_t bytes) {
        if (channel != 1) {
            ++bad_payload;
            return;
        }
        const std::uint32_t client = VerifyMessage(data, bytes, kDatagramTag);
        if (client == 0xFFFFFFFFU) {
            ++bad_payload;
            return;
        }
        const auto bound = peer_client.find(peer_id);
        if (bound == peer_client.end()) {
            peer_client[peer_id] = client;
        } else if (bound->second != client) {
            ++cross_talk;
            return;
        }
        const std::uint32_t seq = ReadU32(data + 4);
        std::uint32_t& expected = datagram_expected[peer_id];
        if (seq != expected) {
            ++out_of_order;
            return;
        }
        ++expected;
        ++datagram_total;
    };

    std::vector<int> replies(kClients, 0);
    std::vector<int> wrong_reply(kClients, 0);
    for (int i = 0; i < kClients; ++i) {
        clients[static_cast<std::size_t>(i)]->on_stream =
            [&, i](std::uint32_t peer_id, std::uint8_t channel, const std::uint8_t* data,
                   std::size_t bytes) {
                if (peer_id != 1 || channel != 2) {
                    ++wrong_reply[static_cast<std::size_t>(i)];
                    return;
                }
                const std::uint32_t client = VerifyMessage(data, bytes, kReplyTag);
                if (client != static_cast<std::uint32_t>(i)) {
                    ++wrong_reply[static_cast<std::size_t>(i)];
                    return;
                }
                ++replies[static_cast<std::size_t>(i)];
            };
    }

    const bool connected = PumpHubs(
        hubs,
        [&]() {
            if (server.stats().peers_active != kClients) {
                return false;
            }
            for (auto& client : clients) {
                if (!client->peer_connected(1)) {
                    return false;
                }
            }
            return true;
        },
        8000);
    BENTO_CHECK(connected);
    BENTO_CHECK_EQ(server.Peers().size(), static_cast<std::size_t>(kClients));
    for (const auto& peer : server.Peers()) {
        BENTO_CHECK_EQ(peer.state, PeerState::kConnected);
        BENTO_CHECK(!peer.outgoing);
    }

    // ---- 每个主控发 50 条可靠 + 50 条不可靠 ----
    // 可靠消息一次入队（窗口 256 足够），由可靠层按 cwnd/pacing 自己排空。
    for (int i = 0; i < kClients; ++i) {
        PeerHub& client = *clients[static_cast<std::size_t>(i)];
        for (std::uint32_t seq = 0; seq < kMessagesPerPeer; ++seq) {
            const std::vector<std::uint8_t> payload =
                MakeMessage(static_cast<std::uint32_t>(i), seq, kStreamTag);
            BENTO_CHECK(client.SendStream(1, 0, payload.data(), payload.size()));
        }
    }
    // 不可靠消息按轮次交错发送、每 10 轮让网络消化一次：真实客户端也是
    // 「边发边收」，不会把 150 个包无限突发到内核队列里（那样丢包是
    // UDP 语义允许的，但会让测试无法区分「实现串流」与「队列溢出」）。
    for (std::uint32_t seq = 0; seq < kDatagramsPerPeer; ++seq) {
        for (int i = 0; i < kClients; ++i) {
            const std::vector<std::uint8_t> payload =
                MakeMessage(static_cast<std::uint32_t>(i), seq, kDatagramTag);
            BENTO_CHECK(
                clients[static_cast<std::size_t>(i)]->SendDatagram(1, 1, payload.data(),
                                                                   payload.size()));
        }
        if (seq % 10 == 9) {
            PumpFor(hubs, 5);
        }
    }

    const int expected_stream = static_cast<int>(kMessagesPerPeer) * kClients;
    const int expected_datagrams = static_cast<int>(kDatagramsPerPeer) * kClients;
    const bool delivered = PumpHubs(
        hubs,
        [&]() {
            if (stream_total < expected_stream || datagram_total < expected_datagrams) {
                return false;
            }
            for (int count : replies) {
                if (count < static_cast<int>(kMessagesPerPeer)) {
                    return false;
                }
            }
            return true;
        },
        20000);
    BENTO_CHECK(delivered);
    BENTO_CHECK_EQ(stream_total, expected_stream);
    BENTO_CHECK_EQ(datagram_total, expected_datagrams);
    BENTO_CHECK_EQ(bad_payload, 0);
    BENTO_CHECK_EQ(cross_talk, 0);
    BENTO_CHECK_EQ(out_of_order, 0);
    for (int i = 0; i < kClients; ++i) {
        BENTO_CHECK_EQ(replies[static_cast<std::size_t>(i)], static_cast<int>(kMessagesPerPeer));
        BENTO_CHECK_EQ(wrong_reply[static_cast<std::size_t>(i)], 0);
    }
    // 每个 peer 的可靠层必须各自独立完成 50 条交付与确认。
    for (std::uint32_t peer_id = 1; peer_id <= static_cast<std::uint32_t>(kClients); ++peer_id) {
        const auto stats = server.reliable_stats(peer_id, 0);
        BENTO_CHECK_EQ(stats.messages_delivered, kMessagesPerPeer);
        BENTO_CHECK_EQ(stats.duplicates_dropped, 0U);
        const auto dgram = server.unreliable_stats(peer_id, 1);
        std::printf(
            "[INFO] peer=%u dgram delivered=%llu stale=%llu invalid=%llu deferred=%llu\n",
            peer_id, static_cast<unsigned long long>(dgram.messages_delivered),
            static_cast<unsigned long long>(dgram.stale_dropped),
            static_cast<unsigned long long>(dgram.invalid_frames),
            static_cast<unsigned long long>(dgram.dependency_deferred));
        BENTO_CHECK_EQ(dgram.messages_delivered, kDatagramsPerPeer);
    }
    const auto hub_stats = server.stats();
    BENTO_CHECK_EQ(hub_stats.invalid_frames, 0U);
    BENTO_CHECK_EQ(hub_stats.unknown_peer_frames, 0U);
    BENTO_CHECK_EQ(hub_stats.conn_id_conflicts, 0U);
    // 单 socket：服务端只监听一个端口，3 个主控都连这个端口。
    BENTO_CHECK(server.local_port() != 0);
    BENTO_CHECK_EQ(server.is_open(), true);
    std::printf(
        "[INFO] multi_peer: peers=%zu stream=%d datagram=%d replies=%d/%d/%d "
        "frames_rx=%llu frames_tx=%llu\n",
        server.Peers().size(), stream_total, datagram_total, replies[0], replies[1], replies[2],
        static_cast<unsigned long long>(hub_stats.frames_received),
        static_cast<unsigned long long>(hub_stats.frames_sent));
}

/// 场景 2：伪造 conn_id 的数据帧被丢弃，已建立的会话不受影响。
void TestUnknownConnIdRejected() {
    PeerHub server(MakeConfig("127.0.0.1"));
    std::string error;
    BENTO_CHECK(server.Open(&error));
    PeerHub client(MakeConfig("127.0.0.1"));
    BENTO_CHECK(client.Open(&error));
    BENTO_CHECK(client.AddPeer(1, "127.0.0.1", server.local_port(), &error));

    int server_streams = 0;
    server.on_stream = [&](std::uint32_t, std::uint8_t, const std::uint8_t*, std::size_t) {
        ++server_streams;
    };
    const bool connected = PumpHubs({&server, &client},
                                    [&]() { return server.peer_connected(1) &&
                                                   client.peer_connected(1); },
                                    8000);
    BENTO_CHECK(connected);

    // 用一个独立 UDP socket 冒充第 4 个客户端：conn_id 不在服务端路由表里。
    const int raw = ::socket(AF_INET, SOCK_DGRAM, 0);
    BENTO_CHECK(raw >= 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(server.local_port());
    BENTO_CHECK(::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr) == 1);

    FrameHeader header;
    header.type = FrameType::kStream;
    header.channel = 0;
    header.conn_id = 0xDEADBEEFU;
    header.seq = 1;
    const std::vector<std::uint8_t> payload = MakeMessage(99, 0, kStreamTag);
    const std::vector<std::uint8_t> frame =
        go2cpp::bento::BuildFrame(header, nullptr, payload.data(), payload.size());
    BENTO_CHECK(!frame.empty());
    BENTO_CHECK(::sendto(raw, frame.data(), frame.size(), 0,
                         reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) ==
                static_cast<ssize_t>(frame.size()));
    const std::uint64_t before = server.stats().unknown_peer_frames;
    for (int i = 0; i < 50; ++i) {
        server.RunOnce(1);
        client.RunOnce(1);
    }
    BENTO_CHECK(server.stats().unknown_peer_frames > before);
    BENTO_CHECK_EQ(server_streams, 0);
    BENTO_CHECK(server.peer_connected(1));
    ::close(raw);
    std::printf("[INFO] unknown_conn_id: unknown_peer_frames=%llu\n",
                static_cast<unsigned long long>(server.stats().unknown_peer_frames));
}

/// 场景 3：一个 peer 静默超时，另一个 peer 不受影响。
void TestPeerTimeoutIsolation() {
    PeerHubConfig server_config = MakeConfig("127.0.0.1");
    server_config.peer_timeout_ms = 800;
    PeerHub server(server_config);
    std::string error;
    BENTO_CHECK(server.Open(&error));

    PeerHub alive(MakeConfig("127.0.0.1"));
    PeerHub silent(MakeConfig("127.0.0.1"));
    BENTO_CHECK(alive.Open(&error));
    BENTO_CHECK(silent.Open(&error));
    BENTO_CHECK(alive.AddPeer(1, "127.0.0.1", server.local_port(), &error));
    BENTO_CHECK(silent.AddPeer(1, "127.0.0.1", server.local_port(), &error));

    std::vector<std::uint32_t> timed_out;
    server.on_peer_disconnected = [&](std::uint32_t peer_id, const char* reason) {
        if (std::strcmp(reason, "peer timeout") == 0) {
            timed_out.push_back(peer_id);
        }
    };
    int alive_streams = 0;
    server.on_stream = [&](std::uint32_t, std::uint8_t, const std::uint8_t*, std::size_t) {
        ++alive_streams;
    };

    BENTO_CHECK(PumpHubs({&server, &alive, &silent},
                         [&]() { return server.stats().peers_active == 2; }, 8000));

    // silent 不再 RunOnce（不发心跳）；只驱动 server 与 alive。
    const bool dropped = PumpHubs({&server, &alive},
                                  [&]() { return !timed_out.empty(); }, 5000);
    BENTO_CHECK(dropped);
    BENTO_CHECK_EQ(timed_out.size(), static_cast<std::size_t>(1));
    const std::uint32_t dead_peer = timed_out.front();
    BENTO_CHECK_EQ(server.peer_state(dead_peer), PeerState::kTimedOut);
    // 另一个 peer 仍然可用：发一条消息能送达。
    const std::vector<std::uint8_t> payload = MakeMessage(7, 0, kStreamTag);
    BENTO_CHECK(alive.SendStream(1, 0, payload.data(), payload.size()));
    BENTO_CHECK(PumpHubs({&server, &alive}, [&]() { return alive_streams >= 1; }, 5000));
    BENTO_CHECK_EQ(server.stats().peers_active, 1U);
    std::printf("[INFO] timeout_isolation: timed_out_peer=%u remaining=%llu\n", dead_peer,
                static_cast<unsigned long long>(server.stats().peers_active));
}

/// 场景 4：AddPeer 参数校验。
void TestAddPeerValidation() {
    PeerHub hub(MakeConfig("127.0.0.1"));
    std::string error;
    BENTO_CHECK(!hub.AddPeer(1, "127.0.0.1", 9, &error));  // 未 Open
    BENTO_CHECK(hub.Open(&error));
    BENTO_CHECK(!hub.AddPeer(0, "127.0.0.1", 9, &error));
    BENTO_CHECK(hub.AddPeer(1, "127.0.0.1", 9, &error));
    BENTO_CHECK(!hub.AddPeer(1, "127.0.0.1", 10, &error));       // 重复 id
    BENTO_CHECK(!hub.AddPeer(2, "127.0.0.1", 9, &error));        // 重复地址
    BENTO_CHECK(!hub.AddPeer(3, "not-a-host.invalid", 9, &error));  // DNS 失败
    BENTO_CHECK_EQ(hub.PeerIds().size(), 1U);
    BENTO_CHECK(hub.RemovePeer(1));
    BENTO_CHECK(!hub.RemovePeer(1));
    BENTO_CHECK_EQ(hub.PeerIds().size(), 0U);
    // 未注册的 peer 发送必须失败，而不是静默丢弃。
    const std::vector<std::uint8_t> payload(32, 0x5A);
    BENTO_CHECK(!hub.SendStream(99, 0, payload.data(), payload.size()));
    BENTO_CHECK(!hub.SendDatagram(99, 0, payload.data(), payload.size()));
    BENTO_CHECK(hub.TrySendStream(99, 0, payload.data(), payload.size()) ==
                go2cpp::bento::SendStatus::kError);
}

/// 场景 5：3 主控 × 200 条 × 512B 吞吐，全量送达且顺序正确。
void TestThroughput() {
    constexpr std::uint32_t kTotal = 200;
    constexpr std::size_t kBytes = 512;
    PeerHub server(MakeConfig("127.0.0.1"));
    std::string error;
    BENTO_CHECK(server.Open(&error));
    constexpr int kClients = 3;
    std::vector<std::unique_ptr<PeerHub>> clients;
    for (int i = 0; i < kClients; ++i) {
        clients.push_back(std::make_unique<PeerHub>(MakeConfig("127.0.0.1")));
        BENTO_CHECK(clients.back()->Open(&error));
        BENTO_CHECK(clients.back()->AddPeer(1, "127.0.0.1", server.local_port(), &error));
    }
    std::vector<PeerHub*> hubs{&server};
    for (auto& client : clients) {
        hubs.push_back(client.get());
    }
    int received = 0;
    int out_of_order = 0;
    std::map<std::uint32_t, std::uint32_t> expected;
    server.on_stream = [&](std::uint32_t peer_id, std::uint8_t channel, const std::uint8_t* data,
                           std::size_t bytes) {
        if (channel != 3 || bytes != kBytes) {
            ++out_of_order;
            return;
        }
        const std::uint32_t seq = ReadU32(data);
        std::uint32_t& next = expected[peer_id];
        if (seq != next) {
            ++out_of_order;
            return;
        }
        ++next;
        ++received;
    };
    BENTO_CHECK(PumpHubs(hubs, [&]() { return server.stats().peers_active == kClients; }, 8000));

    std::vector<std::uint8_t> payload(kBytes, 0);
    const std::uint64_t start = go2cpp::bento::NowMs();
    std::vector<std::uint32_t> sent(kClients, 0);
    bool all_sent = true;
    const std::uint64_t deadline = start + 30000;
    while (go2cpp::bento::NowMs() < deadline && received < static_cast<int>(kTotal) * kClients) {
        for (int i = 0; i < kClients; ++i) {
            auto& client = clients[static_cast<std::size_t>(i)];
            while (sent[static_cast<std::size_t>(i)] < kTotal) {
                const std::uint32_t seq = sent[static_cast<std::size_t>(i)];
                WriteU32(payload.data(), seq);
                for (std::size_t k = 4; k < payload.size(); ++k) {
                    payload[k] = static_cast<std::uint8_t>((i * 17 + seq + k) & 0xFF);
                }
                const go2cpp::bento::SendStatus status =
                    client->TrySendStream(1, 3, payload.data(), payload.size());
                if (status == go2cpp::bento::SendStatus::kWouldBlock) {
                    break;
                }
                if (status != go2cpp::bento::SendStatus::kOk) {
                    all_sent = false;
                    break;
                }
                ++sent[static_cast<std::size_t>(i)];
            }
        }
        for (PeerHub* hub : hubs) {
            hub->RunOnce(1);
        }
    }
    const std::uint64_t elapsed = go2cpp::bento::NowMs() - start;
    BENTO_CHECK(all_sent);
    BENTO_CHECK_EQ(received, static_cast<int>(kTotal) * kClients);
    BENTO_CHECK_EQ(out_of_order, 0);
    double bps = 0.0;
    if (elapsed > 0) {
        bps = static_cast<double>(received) * static_cast<double>(kBytes) * 8.0 /
              (static_cast<double>(elapsed) / 1000.0);
    }
    std::printf("[INFO] throughput: messages=%d bytes=%zu elapsed=%llums ~%.1f Mbit/s\n", received,
                static_cast<std::size_t>(received) * kBytes,
                static_cast<unsigned long long>(elapsed), bps / 1e6);
}

}  // namespace

int main() {
    TestOneServerThreePeers();
    TestUnknownConnIdRejected();
    TestPeerTimeoutIsolation();
    TestAddPeerValidation();
    TestThroughput();
    return bento_test::Finish("test_peer_hub");
}
