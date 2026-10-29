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
 *          - 场景 6：回调内 RemovePeer/Close 不崩溃、不 UAF（PEB-01，ASan 重点）。
 *          - 场景 7：超时后 Connect 重连，双向数据恢复（PEB-02）。
 *          - 场景 8：被动 peer 超时回收后释放 max_peers 配额（PEB-04）。
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
    // PEB-04：监听端被动 peer 超时后自动回收，不再占用 max_peers。
    BENTO_CHECK(!server.HasPeer(dead_peer));
    BENTO_CHECK_EQ(server.peer_state(dead_peer), PeerState::kClosed);
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

/// 场景 6（PEB-01）：回调内 RemovePeer/Close 不破坏投递路径（ASan 重点）。
void TestCallbackRemoval() {
    // 6a. on_stream 内 RemovePeer：ReliableChannel::AdvanceDelivery 在回调
    //     返回后仍会写 m_next_rx_seq，通道必须保活到安全点。
    {
        PeerHub server(MakeConfig("127.0.0.1"));
        PeerHub client(MakeConfig("127.0.0.1"));
        std::string error;
        BENTO_CHECK(server.Open(&error));
        BENTO_CHECK(client.Open(&error));
        int streams = 0;
        bool second_remove = true;
        server.on_stream = [&](std::uint32_t peer_id, std::uint8_t, const std::uint8_t*,
                               std::size_t) {
            ++streams;
            BENTO_CHECK(server.RemovePeer(peer_id));
            // 回调内重复删除必须幂等：旧实现会二次 erase / 迭代器失效。
            second_remove = server.RemovePeer(peer_id);
        };
        BENTO_CHECK(client.AddPeer(1, "127.0.0.1", server.local_port(), &error));
        BENTO_CHECK(PumpHubs({&server, &client},
                             [&]() { return client.peer_connected(1) && server.peer_connected(1); },
                             8000));
        const std::vector<std::uint8_t> payload = MakeMessage(1, 0, kStreamTag);
        BENTO_CHECK(client.SendStream(1, 0, payload.data(), payload.size()));
        BENTO_CHECK(PumpHubs({&server, &client}, [&]() { return streams >= 1; }, 5000));
        BENTO_CHECK_EQ(streams, 1);
        BENTO_CHECK(!second_remove);
        BENTO_CHECK(!server.HasPeer(1));
        BENTO_CHECK_EQ(server.PeerIds().size(), 0U);
    }

    // 6b. on_datagram 内 RemovePeer：UnreliableChannel::OnFrame 投递后还会
    //     执行 FlushPending()，同样要求通道保活到安全点。
    {
        PeerHub server(MakeConfig("127.0.0.1"));
        PeerHub client(MakeConfig("127.0.0.1"));
        std::string error;
        BENTO_CHECK(server.Open(&error));
        BENTO_CHECK(client.Open(&error));
        int datagrams = 0;
        server.on_datagram = [&](std::uint32_t peer_id, std::uint8_t, const std::uint8_t*,
                                 std::size_t) {
            ++datagrams;
            (void)server.RemovePeer(peer_id);
        };
        BENTO_CHECK(client.AddPeer(1, "127.0.0.1", server.local_port(), &error));
        BENTO_CHECK(PumpHubs({&server, &client},
                             [&]() { return client.peer_connected(1) && server.peer_connected(1); },
                             8000));
        const std::vector<std::uint8_t> payload = MakeMessage(2, 0, kDatagramTag);
        BENTO_CHECK(client.SendDatagram(1, 0, payload.data(), payload.size()));
        BENTO_CHECK(PumpHubs({&server, &client}, [&]() { return datagrams >= 1; }, 5000));
        BENTO_CHECK_EQ(datagrams, 1);
        BENTO_CHECK(!server.HasPeer(1));
    }

    // 6c. on_peer_disconnected 里删除同一 peer：DropPeer 正在 Tick 遍历中。
    {
        PeerHubConfig server_config = MakeConfig("127.0.0.1");
        server_config.peer_timeout_ms = 300;
        PeerHub server(server_config);
        PeerHub client(MakeConfig("127.0.0.1"));
        std::string error;
        BENTO_CHECK(server.Open(&error));
        BENTO_CHECK(client.Open(&error));
        int disconnects = 0;
        server.on_peer_disconnected = [&](std::uint32_t peer_id, const char*) {
            ++disconnects;
            (void)server.RemovePeer(peer_id);
        };
        BENTO_CHECK(client.AddPeer(1, "127.0.0.1", server.local_port(), &error));
        BENTO_CHECK(PumpHubs({&server, &client},
                             [&]() { return server.peer_connected(1); }, 8000));
        // 客户端不再运行 -> 服务端 peer 超时，回调里删除同一 peer。
        BENTO_CHECK(PumpHubs({&server}, [&]() { return disconnects >= 1; }, 5000));
        BENTO_CHECK_EQ(disconnects, 1);
        BENTO_CHECK_EQ(server.PeerIds().size(), 0U);
    }

    // 6d. on_stream 内 Close()：整表清空也要推迟到安全点。
    {
        PeerHub server(MakeConfig("127.0.0.1"));
        PeerHub client(MakeConfig("127.0.0.1"));
        std::string error;
        BENTO_CHECK(server.Open(&error));
        BENTO_CHECK(client.Open(&error));
        int streams = 0;
        server.on_stream = [&](std::uint32_t, std::uint8_t, const std::uint8_t*, std::size_t) {
            ++streams;
            server.Close();
        };
        BENTO_CHECK(client.AddPeer(1, "127.0.0.1", server.local_port(), &error));
        BENTO_CHECK(PumpHubs({&server, &client},
                             [&]() { return client.peer_connected(1) && server.peer_connected(1); },
                             8000));
        const std::vector<std::uint8_t> payload = MakeMessage(3, 0, kStreamTag);
        BENTO_CHECK(client.SendStream(1, 0, payload.data(), payload.size()));
        BENTO_CHECK(PumpHubs({&server, &client}, [&]() { return streams >= 1; }, 5000));
        BENTO_CHECK_EQ(streams, 1);
        BENTO_CHECK(!server.is_open());
        BENTO_CHECK_EQ(server.PeerIds().size(), 0U);
        // Close 后继续驱动客户端不应崩溃（服务端已无 socket）。
        PumpFor({&client}, 50);
    }
}

/// 场景 7（PEB-02）：明文模式超时后 Connect 重连，双向数据恢复。
void TestReconnectAfterTimeout() {
    PeerHubConfig server_config = MakeConfig("127.0.0.1");
    server_config.peer_timeout_ms = 3000;
    PeerHub server(server_config);
    PeerHubConfig client_config = MakeConfig("127.0.0.1");
    client_config.peer_timeout_ms = 300;
    PeerHub client(client_config);
    std::string error;
    BENTO_CHECK(server.Open(&error));
    BENTO_CHECK(client.Open(&error));
    int server_streams = 0;
    int client_streams = 0;
    server.on_stream = [&](std::uint32_t, std::uint8_t, const std::uint8_t*, std::size_t) {
        ++server_streams;
    };
    client.on_stream = [&](std::uint32_t, std::uint8_t, const std::uint8_t*, std::size_t) {
        ++client_streams;
    };
    BENTO_CHECK(client.AddPeer(1, "127.0.0.1", server.local_port(), &error));
    BENTO_CHECK(PumpHubs({&server, &client},
                         [&]() { return client.peer_connected(1) && server.peer_connected(1); },
                         8000));
    // 首条双向数据确认链路正常。
    const std::vector<std::uint8_t> payload = MakeMessage(5, 0, kStreamTag);
    BENTO_CHECK(client.SendStream(1, 0, payload.data(), payload.size()));
    BENTO_CHECK(server.SendStream(server.PeerIds().front(), 0, payload.data(), payload.size()));
    BENTO_CHECK(PumpHubs(
        {&server, &client},
        [&]() { return server_streams >= 1 && client_streams >= 1; }, 5000));

    // 只驱动客户端：收不到服务端心跳 -> 客户端超时（主动 peer 保留记录）。
    PumpFor({&client}, 800);
    BENTO_CHECK_EQ(client.peer_state(1), PeerState::kTimedOut);
    BENTO_CHECK_EQ(server.stats().peers_active, 1U);

    // 重连：新 session + 新 conn_id；服务端按新会话重置通道/接收窗口。
    BENTO_CHECK(client.Connect(1));
    BENTO_CHECK(PumpHubs({&server, &client},
                         [&]() { return client.peer_connected(1); }, 8000));
    BENTO_CHECK_EQ(server.stats().peers_active, 1U);
    const int server_before = server_streams;
    const int client_before = client_streams;
    BENTO_CHECK(client.SendStream(1, 0, payload.data(), payload.size()));
    BENTO_CHECK(server.SendStream(server.PeerIds().front(), 0, payload.data(), payload.size()));
    BENTO_CHECK(PumpHubs(
        {&server, &client},
        [&]() {
            return server_streams > server_before && client_streams > client_before;
        },
        5000));
    std::printf("[INFO] reconnect_plain: server_rx=%d client_rx=%d\n", server_streams,
                client_streams);
}

/// 场景 8（PEB-04）：被动 peer 超时回收后释放 max_peers 配额。
void TestTimedOutPeerReclaimed() {
    PeerHubConfig server_config = MakeConfig("127.0.0.1");
    server_config.peer_timeout_ms = 300;
    server_config.max_peers = 4;
    PeerHub server(server_config);
    std::string error;
    BENTO_CHECK(server.Open(&error));
    std::vector<std::unique_ptr<PeerHub>> clients;
    for (int i = 0; i < 4; ++i) {
        clients.push_back(std::make_unique<PeerHub>(MakeConfig("127.0.0.1")));
        BENTO_CHECK(clients.back()->Open(&error));
        BENTO_CHECK(clients.back()->AddPeer(1, "127.0.0.1", server.local_port(), &error));
    }
    std::vector<PeerHub*> hubs{&server};
    for (auto& hub : clients) {
        hubs.push_back(hub.get());
    }
    BENTO_CHECK(PumpHubs(hubs, [&]() { return server.stats().peers_active == 4; }, 8000));
    BENTO_CHECK_EQ(server.PeerIds().size(), 4U);

    // 客户端全部停止驱动：4 个被动 peer 全部超时并回收。
    BENTO_CHECK(PumpHubs({&server}, [&]() { return server.stats().timeouts >= 4; }, 5000));
    BENTO_CHECK_EQ(server.PeerIds().size(), 0U);

    // 配额已释放：第 5 个连接可以进入，且不再计 peers_rejected。
    const std::uint64_t rejected_before = server.stats().peers_rejected;
    PeerHub newcomer(MakeConfig("127.0.0.1"));
    BENTO_CHECK(newcomer.Open(&error));
    BENTO_CHECK(newcomer.AddPeer(1, "127.0.0.1", server.local_port(), &error));
    BENTO_CHECK(PumpHubs({&server, &newcomer},
                         [&]() { return server.stats().peers_active == 1; }, 8000));
    BENTO_CHECK_EQ(server.stats().peers_rejected, rejected_before);
    BENTO_CHECK_EQ(server.PeerIds().size(), 1U);
    std::printf("[INFO] peer_reclaim: timeouts=%llu peers=%zu\n",
                static_cast<unsigned long long>(server.stats().timeouts),
                server.PeerIds().size());
}


/// 场景 9（P1 回归）：明文 HELLO 重传在 conn_id 冲突后必须幂等。
/// @details 触发旧缺陷的最小条件：客户端 B 提议的 conn_id 已被 A 占用，
///          服务端按冲突换新值并在 ACK 里告知；此时 B 的 ACK 丢失、原包
///          重传。旧实现只看「提议值 != 当前值」就重置通道并再换一次
///          conn_id，导致 B 的 conn_id 与服务端分叉、心跳被计 unknown、
///          peer 在 peer_timeout 后被误删。新实现记录客户端本代提议值，
///          同值重传走幂等路径。
void TestHelloRetransmitAfterConnIdConflict() {
    PeerHub server(MakeConfig("127.0.0.1"));
    std::string error;
    BENTO_CHECK(server.Open(&error));
    int streams = 0;
    server.on_stream = [&](std::uint32_t, std::uint8_t, const std::uint8_t*,
                           std::size_t) { ++streams; };

    const int raw_a = ::socket(AF_INET, SOCK_DGRAM, 0);
    const int raw_b = ::socket(AF_INET, SOCK_DGRAM, 0);
    BENTO_CHECK(raw_a >= 0);
    BENTO_CHECK(raw_b >= 0);
    sockaddr_in server_addr{};
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(server.local_port());
    BENTO_CHECK(::inet_pton(AF_INET, "127.0.0.1", &server_addr.sin_addr) == 1);
    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    local.sin_port = 0;
    BENTO_CHECK(::bind(raw_a, reinterpret_cast<sockaddr*>(&local), sizeof(local)) == 0);
    BENTO_CHECK(::bind(raw_b, reinterpret_cast<sockaddr*>(&local), sizeof(local)) == 0);

    constexpr std::uint32_t kSessionA = 0x4A4A0001U;
    constexpr std::uint32_t kSessionB = 0x4B4B0002U;
    constexpr std::uint32_t kProposed = 0x0BADF00DU;

    const auto build_hello = [](std::uint32_t session, std::uint32_t conn_id) {
        std::uint8_t payload[12] = {'B', 'E', 'N', 'T', 0, 0, 0, 0, 0, 0, 0, 0};
        WriteU32(payload + 4, session);
        WriteU32(payload + 8, conn_id);
        FrameHeader header;
        header.type = FrameType::kHello;
        header.channel = 0;
        header.conn_id = conn_id;
        header.seq = session;
        return go2cpp::bento::BuildFrame(header, nullptr, payload, sizeof(payload));
    };
    const std::vector<std::uint8_t> hello_a = build_hello(kSessionA, kProposed);
    const std::vector<std::uint8_t> hello_b = build_hello(kSessionB, kProposed);
    BENTO_CHECK(!hello_a.empty());
    BENTO_CHECK(!hello_b.empty());

    // 发一帧并取出 HELLO_ACK 的 conn_id（超时返回 0）。
    const auto exchange = [&](int fd, const std::vector<std::uint8_t>& frame) -> std::uint32_t {
        if (::sendto(fd, frame.data(), frame.size(), 0,
                     reinterpret_cast<const sockaddr*>(&server_addr), sizeof(server_addr)) !=
            static_cast<ssize_t>(frame.size())) {
            return 0;
        }
        std::uint8_t buffer[2048];
        for (int attempt = 0; attempt < 500; ++attempt) {
            server.RunOnce(0);
            const ssize_t bytes = ::recv(fd, buffer, sizeof(buffer), MSG_DONTWAIT);
            if (bytes <= 0) {
                usleep(1000);
                continue;
            }
            go2cpp::bento::FrameView view;
            if (go2cpp::bento::ParseFrame(buffer, static_cast<std::size_t>(bytes), &view) &&
                view.header.type == FrameType::kHelloAck) {
                return view.header.conn_id;
            }
        }
        return 0;
    };

    const std::uint32_t ack_a = exchange(raw_a, hello_a);
    BENTO_CHECK_EQ(ack_a, kProposed);
    // B 提议同一个 conn_id：服务端按冲突换新值，客户端以 ACK 为准。
    const std::uint32_t ack_b1 = exchange(raw_b, hello_b);
    BENTO_CHECK(ack_b1 != 0);
    BENTO_CHECK(ack_b1 != kProposed);
    BENTO_CHECK_EQ(server.PeerIds().size(), static_cast<std::size_t>(2));
    const std::uint64_t conflicts_after_first = server.stats().conn_id_conflicts;
    // B 重传原包（模拟 ACK 丢失）：必须幂等回同一个 conn_id。
    const std::uint32_t ack_b2 = exchange(raw_b, hello_b);
    BENTO_CHECK_EQ(ack_b2, ack_b1);
    BENTO_CHECK_EQ(server.stats().conn_id_conflicts, conflicts_after_first);

    // 重传后 B 的数据帧仍按 ack_b1 路由并交付（通道没有被重置成新会话）。
    std::vector<std::uint8_t> payload(64, 0x3C);
    WriteU32(payload.data(), 0xB0000001U);
    FrameHeader data_header;
    data_header.type = FrameType::kStream;
    data_header.channel = 0;
    data_header.conn_id = ack_b1;
    data_header.seq = 1;  // STREAM 的 seq=0 是保留值
    const std::vector<std::uint8_t> data =
        go2cpp::bento::BuildFrame(data_header, nullptr, payload.data(), payload.size());
    BENTO_CHECK(!data.empty());
    BENTO_CHECK(::sendto(raw_b, data.data(), data.size(), 0,
                         reinterpret_cast<const sockaddr*>(&server_addr), sizeof(server_addr)) ==
                static_cast<ssize_t>(data.size()));
    for (int attempt = 0; attempt < 2000 && streams == 0; ++attempt) {
        server.RunOnce(0);
        usleep(1000);
    }
    BENTO_CHECK_EQ(streams, 1);
    ::close(raw_a);
    ::close(raw_b);
}

/// 场景 10（P1 回归）：RemovePeer 触发 on_peer_disconnected 时，回调里
/// Connect 必须被拒绝——硬删除意图要先登记，不能出现「Connect 返回 true、
/// 返回后 peer 被销毁队列删掉」的假重连。
void TestRemovePeerRejectsReconnectInCallback() {
    PeerHub server(MakeConfig("127.0.0.1"));
    PeerHub client(MakeConfig("127.0.0.1"));
    std::string error;
    BENTO_CHECK(server.Open(&error));
    BENTO_CHECK(client.Open(&error));
    int disconnects = 0;
    bool connect_result = true;
    client.on_peer_disconnected = [&](std::uint32_t peer_id, const char* reason) {
        if (std::strcmp(reason, "removed") == 0) {
            ++disconnects;
            connect_result = client.Connect(peer_id);
        }
    };
    BENTO_CHECK(client.AddPeer(1, "127.0.0.1", server.local_port(), &error));
    BENTO_CHECK(PumpHubs({&server, &client},
                         [&]() { return client.peer_connected(1) && server.peer_connected(1); },
                         8000));
    BENTO_CHECK(client.RemovePeer(1));
    BENTO_CHECK_EQ(disconnects, 1);
    BENTO_CHECK(!connect_result);
    BENTO_CHECK(!client.HasPeer(1));
    BENTO_CHECK(client.PeerIds().empty());
    // 服务端把断链当被动 peer 超时回收，不影响断言。
    PumpFor({&server, &client}, 50);
}

/// 场景 11（P2 回归）：显式 RemovePeer 不是超时，不得污染 timeouts 计数。
void TestRemovePeerDoesNotCountAsTimeout() {
    PeerHub hub(MakeConfig("127.0.0.1"));
    std::string error;
    BENTO_CHECK(hub.Open(&error));
    BENTO_CHECK(hub.AddPeer(1, "127.0.0.1", 9, &error));
    const std::uint64_t before = hub.stats().timeouts;
    BENTO_CHECK(hub.RemovePeer(1));
    BENTO_CHECK_EQ(hub.stats().timeouts, before);
}

/// 场景 12（P2 回归）：stats().peers_active 必须与 Peers() 里 kConnected
/// 的数量一致（该字段已从 O(N) 遍历改为增量维护）。
void TestPeersActiveCountMatchesSnapshot() {
    PeerHub server(MakeConfig("127.0.0.1"));
    std::string error;
    BENTO_CHECK(server.Open(&error));
    constexpr int kClients = 4;
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
    BENTO_CHECK(PumpHubs(hubs, [&]() { return server.stats().peers_active == kClients; }, 8000));
    const auto count_connected = [&]() {
        std::size_t connected = 0;
        for (const auto& peer : server.Peers()) {
            if (peer.state == PeerState::kConnected) {
                ++connected;
            }
        }
        return connected;
    };
    BENTO_CHECK_EQ(server.stats().peers_active, static_cast<std::uint64_t>(count_connected()));
    BENTO_CHECK(server.RemovePeer(server.PeerIds().front()));
    BENTO_CHECK_EQ(server.stats().peers_active, static_cast<std::uint64_t>(count_connected()));
    PumpFor(hubs, 50);
    BENTO_CHECK_EQ(server.stats().peers_active, static_cast<std::uint64_t>(count_connected()));
}

}  // namespace

int main() {
    TestOneServerThreePeers();
    TestUnknownConnIdRejected();
    TestPeerTimeoutIsolation();
    TestAddPeerValidation();
    TestThroughput();
    TestCallbackRemoval();
    TestReconnectAfterTimeout();
    TestTimedOutPeerReclaimed();
    TestHelloRetransmitAfterConnIdConflict();
    TestRemovePeerRejectsReconnectInCallback();
    TestRemovePeerDoesNotCountAsTimeout();
    TestPeersActiveCountMatchesSnapshot();
    return bento_test::Finish("test_peer_hub");
}
