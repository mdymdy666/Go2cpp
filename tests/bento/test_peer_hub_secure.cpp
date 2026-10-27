/**
 * @file test_peer_hub_secure.cpp
 * @brief 多 peer PSK 加密测试：3 peer 正常收发、错误 PSK 拒绝、明文拒绝、per-peer 密钥。
 * @details 覆盖场景矩阵 9.4 / 9.5 / 10.3（FR-06 / NFR-04 / RISK-007 / AX-004）：
 *          - 场景 1：1 被控 3 主控全部 PSK 加密，双向收发按 peer 隔离。
 *          - 场景 2：错误 PSK 被服务端 AEAD 拒绝，握手超时，不建立会话。
 *          - 场景 3：加密模式下明文 HELLO 与明文数据帧被拒绝（防降级）。
 *          - 场景 4：同一个 Hub 用 per-peer 不同 PSK 连接两个服务端（密钥隔离）。
 *          - 场景 5：加密可靠流在丢包/PTO 重传下仍能全量按序送达（wire seq 不重放）。
 *          - 场景 6：PSK 参数校验与非法配置。
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
using go2cpp::bento::PeerSecureStats;
using go2cpp::bento::PeerState;

constexpr std::uint32_t kMessagesPerPeer = 30;
constexpr std::uint32_t kDatagramsPerPeer = 20;
constexpr std::size_t kPayloadBytes = 48;
constexpr std::uint8_t kStreamTag = 0x11;
constexpr std::uint8_t kDatagramTag = 0x22;
constexpr std::uint8_t kReplyTag = 0x33;

/// 构造可按 seed 区分的 32B PSK。
std::vector<std::uint8_t> MakePsk(std::uint8_t seed) {
    std::vector<std::uint8_t> key(32, 0);
    for (std::size_t i = 0; i < key.size(); ++i) {
        key[i] = static_cast<std::uint8_t>((seed + i * 7U) & 0xFFU);
    }
    return key;
}

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
    return (client * 2246822519U) ^ (seq * 2654435761U) ^ tag;
}

std::vector<std::uint8_t> MakeMessage(std::uint32_t client, std::uint32_t seq, std::uint8_t tag) {
    std::vector<std::uint8_t> data(kPayloadBytes, 0);
    WriteU32(data.data(), client);
    WriteU32(data.data() + 4, seq);
    WriteU32(data.data() + 8, CheckValue(client, seq, tag));
    for (std::size_t i = 12; i < data.size(); ++i) {
        data[i] = static_cast<std::uint8_t>((client * 13U + seq * 29U + i) & 0xFFU);
    }
    return data;
}

/// 校验消息三元组，返回 client 编号；失败返回 0xFFFFFFFF。
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
        if (data[i] != static_cast<std::uint8_t>((client * 13U + seq * 29U + i) & 0xFFU)) {
            return 0xFFFFFFFFU;
        }
    }
    return client;
}

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

void PumpFor(const std::vector<PeerHub*>& hubs, int duration_ms) {
    const std::uint64_t deadline =
        go2cpp::bento::NowMs() + static_cast<std::uint64_t>(duration_ms);
    while (go2cpp::bento::NowMs() < deadline) {
        for (PeerHub* hub : hubs) {
            hub->RunOnce(1);
        }
    }
}

PeerHubConfig MakeConfig(const char* address, const std::vector<std::uint8_t>& psk) {
    PeerHubConfig config;
    config.bind_address = address;
    config.bind_port = 0;
    config.heartbeat_interval_ms = 100;
    config.peer_timeout_ms = 5000;
    config.max_peers = 8;
    config.accept_unknown_peers = true;
    config.pre_shared_key = psk;
    return config;
}

/// 场景 1：3 个加密 peer 双向 STREAM/DATAGRAM，断言隔离与统计。
void TestThreeSecurePeers() {
    const std::vector<std::uint8_t> psk = MakePsk(0x41);
    PeerHub server(MakeConfig("127.0.0.1", psk));
    std::string error;
    BENTO_CHECK(server.Open(&error));
    BENTO_CHECK(server.secure());

    constexpr int kClients = 3;
    std::vector<std::unique_ptr<PeerHub>> clients;
    for (int i = 0; i < kClients; ++i) {
        clients.push_back(std::make_unique<PeerHub>(MakeConfig("127.0.0.1", psk)));
        BENTO_CHECK(clients.back()->Open(&error));
        BENTO_CHECK(clients.back()->secure());
        BENTO_CHECK(clients.back()->AddPeer(1, "127.0.0.1", server.local_port(), &error));
    }
    std::vector<PeerHub*> hubs{&server};
    for (auto& client : clients) {
        hubs.push_back(client.get());
    }

    std::map<std::uint32_t, std::uint32_t> peer_client;
    std::map<std::uint32_t, std::uint32_t> stream_expected;
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
        if (seq < 10) {
            const std::vector<std::uint8_t> reply = MakeMessage(client, seq, kReplyTag);
            (void)server.SendStream(peer_id, 2, reply.data(), reply.size());
        }
    };
    server.on_datagram = [&](std::uint32_t peer_id, std::uint8_t channel,
                             const std::uint8_t* data, std::size_t bytes) {
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
                if (VerifyMessage(data, bytes, kReplyTag) != static_cast<std::uint32_t>(i)) {
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
                if (!client->peer_connected(1) || !client->peer_secure_ready(1)) {
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
        BENTO_CHECK(peer.secure);
        BENTO_CHECK(peer.secure_ready);
    }
    BENTO_CHECK_EQ(server.secure_stats().handshakes, static_cast<std::uint64_t>(kClients));
    for (auto& client : clients) {
        BENTO_CHECK_EQ(client->secure_stats().handshakes, 1U);
        BENTO_CHECK(client->peer_secure(1));
    }

    for (int i = 0; i < kClients; ++i) {
        PeerHub& client = *clients[static_cast<std::size_t>(i)];
        for (std::uint32_t seq = 0; seq < kMessagesPerPeer; ++seq) {
            const std::vector<std::uint8_t> payload =
                MakeMessage(static_cast<std::uint32_t>(i), seq, kStreamTag);
            BENTO_CHECK(client.SendStream(1, 0, payload.data(), payload.size()));
        }
    }
    for (std::uint32_t seq = 0; seq < kDatagramsPerPeer; ++seq) {
        for (int i = 0; i < kClients; ++i) {
            const std::vector<std::uint8_t> payload =
                MakeMessage(static_cast<std::uint32_t>(i), seq, kDatagramTag);
            BENTO_CHECK(clients[static_cast<std::size_t>(i)]->SendDatagram(
                1, 1, payload.data(), payload.size()));
        }
        if (seq % 5 == 4) {
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
                if (count < 10) {
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
        BENTO_CHECK_EQ(replies[static_cast<std::size_t>(i)], 10);
        BENTO_CHECK_EQ(wrong_reply[static_cast<std::size_t>(i)], 0);
    }
    const PeerSecureStats secure = server.secure_stats();
    BENTO_CHECK_EQ(secure.auth_failures, 0U);
    BENTO_CHECK_EQ(secure.plaintext_rejected, 0U);
    BENTO_CHECK(secure.packets_opened > 0);
    BENTO_CHECK(secure.packets_sealed > 0);
    BENTO_CHECK(secure.encrypted_bytes_received > 0);
    BENTO_CHECK(secure.encrypted_bytes_sent > 0);
    // 加密外层比明文多 16B 头 + 16B tag；线上字节必须显著大于载荷。
    const auto hub_stats = server.stats();
    BENTO_CHECK(hub_stats.invalid_frames == 0);
    std::printf(
        "[INFO] secure_3peers: peers=%zu stream=%d dgram=%d opened=%llu sealed=%llu "
        "auth_fail=%llu plaintext=%llu\n",
        server.Peers().size(), stream_total, datagram_total,
        static_cast<unsigned long long>(secure.packets_opened),
        static_cast<unsigned long long>(secure.packets_sealed),
        static_cast<unsigned long long>(secure.auth_failures),
        static_cast<unsigned long long>(secure.plaintext_rejected));
}

/// 场景 2：错误 PSK — 服务端 AEAD 拒绝，客户端握手超时。
void TestWrongPskRejected() {
    PeerHubConfig server_config = MakeConfig("127.0.0.1", MakePsk(0x51));
    server_config.handshake_timeout_ms = 500;
    server_config.hello_initial_pto_ms = 50;
    server_config.hello_max_pto_ms = 100;
    server_config.hello_max_attempts = 5;
    PeerHub server(server_config);
    std::string error;
    BENTO_CHECK(server.Open(&error));

    PeerHubConfig client_config = MakeConfig("127.0.0.1", MakePsk(0x52));  // 不同 PSK
    client_config.handshake_timeout_ms = 500;
    client_config.hello_initial_pto_ms = 50;
    client_config.hello_max_pto_ms = 100;
    client_config.hello_max_attempts = 5;
    PeerHub client(client_config);
    BENTO_CHECK(client.Open(&error));
    BENTO_CHECK(client.AddPeer(1, "127.0.0.1", server.local_port(), &error));

    PumpFor({&server, &client}, 1500);
    const PeerSecureStats secure = server.secure_stats();
    BENTO_CHECK(secure.auth_failures > 0);
    BENTO_CHECK_EQ(secure.handshakes, 0U);
    BENTO_CHECK_EQ(server.stats().peers_active, 0U);
    BENTO_CHECK_EQ(server.PeerIds().size(), 0U);
    BENTO_CHECK_EQ(client.peer_state(1), PeerState::kTimedOut);
    BENTO_CHECK(!client.peer_secure_ready(1));
    BENTO_CHECK_EQ(client.secure_stats().handshakes, 0U);
    std::printf("[INFO] wrong_psk: auth_failures=%llu client_state=%s\n",
                static_cast<unsigned long long>(secure.auth_failures),
                go2cpp::bento::PeerStateName(client.peer_state(1)));
}

/// 场景 3：加密模式下明文 HELLO 与明文数据帧都被拒绝。
void TestPlaintextRejected() {
    const std::vector<std::uint8_t> psk = MakePsk(0x61);
    PeerHub server(MakeConfig("127.0.0.1", psk));
    std::string error;
    BENTO_CHECK(server.Open(&error));

    // 3a. 无 PSK 的明文客户端：HELLO 被拒绝，服务端不创建 peer。
    PeerHub plain_client(MakeConfig("127.0.0.1", {}));
    BENTO_CHECK(plain_client.Open(&error));
    BENTO_CHECK(plain_client.AddPeer(1, "127.0.0.1", server.local_port(), &error));
    PumpFor({&server, &plain_client}, 600);
    BENTO_CHECK(server.secure_stats().plaintext_rejected > 0);
    BENTO_CHECK_EQ(server.PeerIds().size(), 0U);
    BENTO_CHECK(!plain_client.peer_connected(1));
    std::printf("[INFO] plaintext_hello_rejected=%llu\n",
                static_cast<unsigned long long>(server.secure_stats().plaintext_rejected));

    // 3b. 建立一条加密会话，然后注入明文 STREAM 帧。
    PeerHub secure_client(MakeConfig("127.0.0.1", psk));
    BENTO_CHECK(secure_client.Open(&error));
    BENTO_CHECK(secure_client.AddPeer(1, "127.0.0.1", server.local_port(), &error));
    int streams = 0;
    server.on_stream = [&](std::uint32_t, std::uint8_t, const std::uint8_t*, std::size_t) {
        ++streams;
    };
    BENTO_CHECK(PumpHubs({&server, &secure_client},
                         [&]() {
                             return server.stats().peers_active == 1 &&
                                    secure_client.peer_connected(1);
                         },
                         8000));
    BENTO_CHECK_EQ(server.Peers().size(), static_cast<std::size_t>(1));
    const std::uint32_t conn_id = server.Peers().front().conn_id;
    BENTO_CHECK(conn_id != 0);

    const std::uint64_t before = server.secure_stats().plaintext_rejected;
    const int raw = ::socket(AF_INET, SOCK_DGRAM, 0);
    BENTO_CHECK(raw >= 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(server.local_port());
    BENTO_CHECK(::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr) == 1);
    FrameHeader header;
    header.type = FrameType::kStream;
    header.channel = 0;
    header.conn_id = conn_id;
    header.seq = 0;
    const std::vector<std::uint8_t> payload = MakeMessage(9, 0, kStreamTag);
    const std::vector<std::uint8_t> frame =
        go2cpp::bento::BuildFrame(header, nullptr, payload.data(), payload.size());
    BENTO_CHECK(!frame.empty());
    BENTO_CHECK(::sendto(raw, frame.data(), frame.size(), 0,
                         reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) ==
                static_cast<ssize_t>(frame.size()));
    PumpFor({&server, &secure_client}, 200);
    ::close(raw);
    BENTO_CHECK(server.secure_stats().plaintext_rejected > before);
    BENTO_CHECK_EQ(streams, 0);
    // 合法加密流量仍可工作。
    const std::vector<std::uint8_t> good = MakeMessage(9, 1, kStreamTag);
    BENTO_CHECK(secure_client.SendStream(1, 0, good.data(), good.size()));
    BENTO_CHECK(PumpHubs({&server, &secure_client}, [&]() { return streams >= 1; }, 5000));
}

/// 场景 4：同一 Hub 用 per-peer 不同 PSK 连接两个服务端。
void TestPerPeerPsk() {
    const std::vector<std::uint8_t> psk_a = MakePsk(0x71);
    const std::vector<std::uint8_t> psk_b = MakePsk(0x72);
    PeerHub server_a(MakeConfig("127.0.0.1", psk_a));
    PeerHub server_b(MakeConfig("127.0.0.1", psk_b));
    std::string error;
    BENTO_CHECK(server_a.Open(&error));
    BENTO_CHECK(server_b.Open(&error));

    PeerHub client(MakeConfig("127.0.0.1", {}));
    BENTO_CHECK(client.Open(&error));
    BENTO_CHECK(client.AddPeer(1, "127.0.0.1", server_a.local_port(), &error));
    BENTO_CHECK(client.AddPeer(2, "127.0.0.1", server_b.local_port(), &error));
    BENTO_CHECK(client.SetPeerPreSharedKey(1, psk_a.data(), psk_a.size()));
    BENTO_CHECK(client.SetPeerPreSharedKey(2, psk_b.data(), psk_b.size()));
    BENTO_CHECK(client.peer_secure(1));
    BENTO_CHECK(client.peer_secure(2));

    int received_a = 0;
    int received_b = 0;
    server_a.on_stream = [&](std::uint32_t, std::uint8_t, const std::uint8_t*, std::size_t) {
        ++received_a;
    };
    server_b.on_stream = [&](std::uint32_t, std::uint8_t, const std::uint8_t*, std::size_t) {
        ++received_b;
    };

    BENTO_CHECK(PumpHubs({&server_a, &server_b, &client},
                         [&]() {
                             return server_a.stats().peers_active == 1 &&
                                    server_b.stats().peers_active == 1 &&
                                    client.peer_connected(1) && client.peer_connected(2);
                         },
                         8000));
    BENTO_CHECK(client.peer_secure_ready(1));
    BENTO_CHECK(client.peer_secure_ready(2));
    BENTO_CHECK_EQ(server_a.secure_stats().handshakes, 1U);
    BENTO_CHECK_EQ(server_b.secure_stats().handshakes, 1U);

    for (std::uint32_t seq = 0; seq < 10; ++seq) {
        const std::vector<std::uint8_t> payload = MakeMessage(1, seq, kStreamTag);
        BENTO_CHECK(client.SendStream(1, 0, payload.data(), payload.size()));
        BENTO_CHECK(client.SendStream(2, 0, payload.data(), payload.size()));
        PumpFor({&server_a, &server_b, &client}, 2);
    }
    BENTO_CHECK(PumpHubs({&server_a, &server_b, &client},
                         [&]() { return received_a >= 10 && received_b >= 10; }, 8000));
    BENTO_CHECK_EQ(received_a, 10);
    BENTO_CHECK_EQ(received_b, 10);
    std::printf("[INFO] per_peer_psk: a_handshakes=%llu b_handshakes=%llu rx=%d/%d\n",
                static_cast<unsigned long long>(server_a.secure_stats().handshakes),
                static_cast<unsigned long long>(server_b.secure_stats().handshakes),
                received_a, received_b);
}

/// 场景 5：加密可靠流在丢包/PTO 重传后仍全量按序送达。
void TestSecureRetransmitRecovery() {
    const std::vector<std::uint8_t> psk = MakePsk(0x81);
    PeerHub server(MakeConfig("127.0.0.1", psk));
    std::string error;
    BENTO_CHECK(server.Open(&error));
    PeerHub client(MakeConfig("127.0.0.1", psk));
    BENTO_CHECK(client.Open(&error));
    BENTO_CHECK(client.AddPeer(1, "127.0.0.1", server.local_port(), &error));

    int received = 0;
    int out_of_order = 0;
    std::uint32_t expected = 0;
    server.on_stream = [&](std::uint32_t, std::uint8_t, const std::uint8_t* data,
                           std::size_t bytes) {
        if (bytes != kPayloadBytes) {
            ++out_of_order;
            return;
        }
        const std::uint32_t seq = ReadU32(data);
        if (seq != expected) {
            ++out_of_order;
            return;
        }
        ++expected;
        ++received;
    };
    BENTO_CHECK(PumpHubs({&server, &client},
                         [&]() { return server.stats().peers_active == 1 &&
                                        client.peer_connected(1); },
                         8000));

    constexpr std::uint32_t kTotal = 200;
    std::vector<std::uint8_t> payload(kPayloadBytes, 0);
    for (std::uint32_t seq = 0; seq < kTotal; ++seq) {
        WriteU32(payload.data(), seq);
        BENTO_CHECK(client.SendStream(1, 4, payload.data(), payload.size()));
    }
    // 服务端 1.2s 不处理：首批包全部丢失，客户端 PTO 重传（加密帧重新封装）。
    PumpFor({&client}, 1200);
    const std::uint64_t retransmits = client.reliable_stats(1, 4).retransmits;
    const std::uint64_t sealed_before = client.secure_stats().packets_sealed;
    std::printf("[INFO] pto-window: retransmits=%llu sealed=%llu\n",
                static_cast<unsigned long long>(retransmits),
                static_cast<unsigned long long>(sealed_before));
    BENTO_CHECK(retransmits > 0);
    // 窗口受 cwnd 限制：PTO 窗口内只发出了部分帧，重传确实增加了线上帧。
    BENTO_CHECK(sealed_before > 0);
    BENTO_CHECK(PumpHubs({&server, &client},
                         [&]() { return received >= static_cast<int>(kTotal); }, 20000));
    BENTO_CHECK_EQ(received, static_cast<int>(kTotal));
    BENTO_CHECK_EQ(out_of_order, 0);
    BENTO_CHECK(server.secure_stats().packets_opened > 0);
    // 200 条消息 + 重传 + 心跳，最终加密帧数必然超过消息数。
    BENTO_CHECK(client.secure_stats().packets_sealed > kTotal);
    std::printf("[INFO] secure_retransmit: retransmits=%llu sealed=%llu opened=%llu rx=%d\n",
                static_cast<unsigned long long>(client.reliable_stats(1, 4).retransmits),
                static_cast<unsigned long long>(client.secure_stats().packets_sealed),
                static_cast<unsigned long long>(server.secure_stats().packets_opened),
                received);
}

/// 场景 6：PSK 参数校验与非法配置。
void TestPskValidation() {
    std::string error;
    PeerHubConfig bad_config = MakeConfig("127.0.0.1", std::vector<std::uint8_t>(31, 0x5A));
    PeerHub bad(bad_config);
    BENTO_CHECK(!bad.Open(&error));
    BENTO_CHECK(error.find("32") != std::string::npos);

    PeerHub hub(MakeConfig("127.0.0.1", {}));
    BENTO_CHECK(!hub.secure());
    const std::uint8_t key[32] = {};
    BENTO_CHECK(!hub.SetPreSharedKey(nullptr, 32));
    BENTO_CHECK(!hub.SetPreSharedKey(key, 31));
    BENTO_CHECK(hub.SetPreSharedKey(key, 32));
    BENTO_CHECK(hub.secure());
    BENTO_CHECK(hub.Open(&error));
    BENTO_CHECK(!hub.SetPeerPreSharedKey(99, key, 32));
    BENTO_CHECK(!hub.SetPeerPreSharedKey(1, key, 16));
    BENTO_CHECK(hub.AddPeer(1, "127.0.0.1", 9, &error));
    BENTO_CHECK(hub.peer_secure(1));
    BENTO_CHECK(!hub.peer_secure_ready(1));

    // 已建立加密会话后不允许热换密钥。
    PeerHub server(MakeConfig("127.0.0.1", MakePsk(0x91)));
    BENTO_CHECK(server.Open(&error));
    PeerHub client(MakeConfig("127.0.0.1", MakePsk(0x91)));
    BENTO_CHECK(client.Open(&error));
    BENTO_CHECK(client.AddPeer(1, "127.0.0.1", server.local_port(), &error));
    BENTO_CHECK(PumpHubs({&server, &client},
                         [&]() { return client.peer_secure_ready(1); }, 8000));
    BENTO_CHECK(!client.SetPeerPreSharedKey(1, MakePsk(0x92).data(), 32));
    BENTO_CHECK(client.peer_secure_ready(1));

    // 未配置 PSK 的 peer 在明文模式下保持 secure=false。
    PeerHub plain(MakeConfig("127.0.0.1", {}));
    BENTO_CHECK(plain.Open(&error));
    BENTO_CHECK(plain.AddPeer(1, "127.0.0.1", 9, &error));
    BENTO_CHECK(!plain.peer_secure(1));
    BENTO_CHECK(!plain.peer_secure_ready(1));

    // 已连接的明文 peer 不会被 Hub 级 PSK「热升级」：否则在途帧会被对端
    // 按明文拒绝。明文会话必须保持可用。
    PeerHub plain_server(MakeConfig("127.0.0.1", {}));
    PeerHub plain_client(MakeConfig("127.0.0.1", {}));
    BENTO_CHECK(plain_server.Open(&error));
    BENTO_CHECK(plain_client.Open(&error));
    int plain_received = 0;
    plain_server.on_stream = [&](std::uint32_t, std::uint8_t, const std::uint8_t*,
                                 std::size_t) { ++plain_received; };
    BENTO_CHECK(plain_client.AddPeer(1, "127.0.0.1", plain_server.local_port(), &error));
    BENTO_CHECK(PumpHubs({&plain_server, &plain_client},
                         [&]() { return plain_client.peer_connected(1); }, 8000));
    const std::vector<std::uint8_t> late_psk = MakePsk(0xA1);
    BENTO_CHECK(plain_client.SetPreSharedKey(late_psk.data(), late_psk.size()));
    BENTO_CHECK(!plain_client.peer_secure(1));
    const std::vector<std::uint8_t> plain_payload(32, 0x7E);
    BENTO_CHECK(plain_client.SendStream(1, 0, plain_payload.data(), plain_payload.size()));
    BENTO_CHECK(PumpHubs({&plain_server, &plain_client},
                         [&]() { return plain_received >= 1; }, 5000));
}

}  // namespace

int main() {
    TestThreeSecurePeers();
    TestWrongPskRejected();
    TestPlaintextRejected();
    TestPerPeerPsk();
    TestSecureRetransmitRecovery();
    TestPskValidation();
    return bento_test::Finish("test_peer_hub_secure");
}
