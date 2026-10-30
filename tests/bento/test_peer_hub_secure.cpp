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
 *          - 场景 7：加密数据帧重放/篡改：重放被窗口拒绝且不重复交付，篡改计
 *            auth_failures 且不影响后续合法流（200 条仍按序全量）。
 *          - 场景 8：加密模式下的 STREAM 分片/大载荷：1200B/1201B/4KB/120KB，
 *            断言内容一致与 fragments 统计对账。
 * @author go2cpp contributors
 * @date 2026-10-27
 * @copyright MIT License
 */

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <chrono>
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


/// @brief 测试内 UDP 中继：可截获、扣留并原样/篡改重发客户端→服务端的
///        加密 STREAM 帧。用于从线上抓取真实密文（客户端不暴露密钥）。
/// @details 客户端把本中继端口当作服务端地址；中继学习客户端源地址后双向
///          转发。`CaptureNext()` 截获下一帧加密 STREAM（仍正常转发），
///          `HoldNext()` 截获后扣住不转发，供测试注入篡改副本。全部由
///          测试线程调用 `Pump()` 驱动，无额外线程。
class SecureRelay final {
   public:
    SecureRelay() = default;
    ~SecureRelay() { Close(); }

    SecureRelay(const SecureRelay&) = delete;
    SecureRelay& operator=(const SecureRelay&) = delete;

    bool Start(std::uint16_t server_port, std::string* error) {
        m_server = sockaddr_in{};
        m_server.sin_family = AF_INET;
        m_server.sin_port = htons(server_port);
        if (::inet_pton(AF_INET, "127.0.0.1", &m_server.sin_addr) != 1) {
            SetError(error, "relay: inet_pton failed");
            return false;
        }
        m_fd = ::socket(AF_INET, SOCK_DGRAM, 0);
        if (m_fd < 0) {
            SetError(error, "relay: socket failed");
            return false;
        }
        sockaddr_in local{};
        local.sin_family = AF_INET;
        local.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        local.sin_port = 0;
        if (::bind(m_fd, reinterpret_cast<const sockaddr*>(&local), sizeof(local)) != 0) {
            SetError(error, "relay: bind failed");
            Close();
            return false;
        }
        socklen_t local_len = sizeof(local);
        if (::getsockname(m_fd, reinterpret_cast<sockaddr*>(&local), &local_len) != 0) {
            SetError(error, "relay: getsockname failed");
            Close();
            return false;
        }
        m_port = ntohs(local.sin_port);
        return true;
    }

    void Close() {
        if (m_fd >= 0) {
            ::close(m_fd);
            m_fd = -1;
        }
    }

    std::uint16_t port() const noexcept {
        return m_port;
    }

    void CaptureNext() noexcept {
        m_mode = Mode::kCapture;
    }
    void HoldNext() noexcept {
        m_mode = Mode::kHold;
    }
    bool captured() const noexcept {
        return !m_captured.empty();
    }

    std::vector<std::uint8_t> TakeCaptured() {
        std::vector<std::uint8_t> result;
        result.swap(m_captured);
        return result;
    }

    void SendToServer(const std::uint8_t* data, std::size_t bytes) {
        if (m_fd < 0 || data == nullptr) {
            return;
        }
        (void)::sendto(m_fd, data, bytes, 0, reinterpret_cast<const sockaddr*>(&m_server),
                       sizeof(m_server));
    }

    void SendToServer(const std::vector<std::uint8_t>& data) {
        SendToServer(data.data(), data.size());
    }

    void Pump() {
        if (m_fd < 0) {
            return;
        }
        std::uint8_t buffer[65536];
        for (;;) {
            sockaddr_in from{};
            socklen_t from_len = sizeof(from);
            const ssize_t bytes = ::recvfrom(m_fd, buffer, sizeof(buffer), MSG_DONTWAIT,
                                             reinterpret_cast<sockaddr*>(&from), &from_len);
            if (bytes <= 0) {
                return;
            }
            const std::size_t count = static_cast<std::size_t>(bytes);
            if (SameEndpoint(from, m_server)) {
                if (m_have_client) {
                    (void)::sendto(m_fd, buffer, count, 0,
                                   reinterpret_cast<const sockaddr*>(&m_client),
                                   sizeof(m_client));
                }
                continue;
            }
            if (!m_have_client) {
                m_client = from;
                m_have_client = true;
            } else if (!SameEndpoint(from, m_client)) {
                continue;
            }
            if (m_mode != Mode::kForward && IsEncryptedStream(buffer, count)) {
                m_captured.assign(buffer, buffer + count);
                if (m_mode == Mode::kCapture) {
                    ForwardToServer(buffer, count);
                }
                m_mode = Mode::kForward;
                continue;
            }
            ForwardToServer(buffer, count);
        }
    }

   private:
    enum class Mode : std::uint8_t { kForward = 0, kCapture = 1, kHold = 2 };

    static bool SameEndpoint(const sockaddr_in& a, const sockaddr_in& b) noexcept {
        return a.sin_family == b.sin_family && a.sin_port == b.sin_port &&
               a.sin_addr.s_addr == b.sin_addr.s_addr;
    }

    static bool IsEncryptedStream(const std::uint8_t* data, std::size_t bytes) {
        FrameHeader header;
        return bytes >= go2cpp::bento::kFrameHeaderBytes &&
               go2cpp::bento::DecodeHeader(data, bytes, &header) &&
               header.type == FrameType::kStream &&
               (header.flags & go2cpp::bento::kFlagEncrypted) != 0;
    }

    static void SetError(std::string* error, const char* message) {
        if (error != nullptr) {
            *error = message;
        }
    }

    void ForwardToServer(const std::uint8_t* data, std::size_t bytes) {
        (void)::sendto(m_fd, data, bytes, 0, reinterpret_cast<const sockaddr*>(&m_server),
                       sizeof(m_server));
    }

    int m_fd = -1;
    std::uint16_t m_port = 0;
    sockaddr_in m_server{};
    sockaddr_in m_client{};
    bool m_have_client = false;
    Mode m_mode = Mode::kForward;
    std::vector<std::uint8_t> m_captured;
};

/// @brief 驱动中继 + 若干 Hub，直到谓词成立或超时。
bool PumpSecure(SecureRelay& relay, const std::vector<PeerHub*>& hubs,
                const std::function<bool()>& done, int timeout_ms) {
    const std::uint64_t deadline =
        go2cpp::bento::NowMs() + static_cast<std::uint64_t>(timeout_ms);
    while (go2cpp::bento::NowMs() < deadline) {
        relay.Pump();
        for (PeerHub* hub : hubs) {
            hub->RunOnce(1);
        }
        relay.Pump();
        if (done()) {
            return true;
        }
    }
    return done();
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
    // 这是「故意用明文客户端」的用例，必须显式关闭默认加密策略才能 Open。
    PeerHubConfig plain_client_config = MakeConfig("127.0.0.1", {});
    plain_client_config.require_encryption = false;
    PeerHub plain_client(plain_client_config);
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

    // per-peer PSK 仍需一个默认 PSK 才能通过 Open 的默认安全校验。
    PeerHub client(MakeConfig("127.0.0.1", psk_a));
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


/// 场景 7：加密数据帧重放与篡改。
/// @details 直接从线上抓取真实密文（不依赖测试侧密钥）：
///          - 原样重发 → `replay_drops+1`，`packets_opened` 与交付数不增；
///          - 扣住一条尚未交付的帧并发送翻转 1 字节的副本 → `auth_failures+1`、
///            不消耗 `packets_opened`，随后原帧正常交付；
///          - 其余 198 条合法消息仍按序全量送达（连接未被误杀）。
void TestSecureReplayAndTamper() {
    constexpr std::uint32_t kClientId = 7;
    constexpr std::uint32_t kTotalMessages = 200;
    const std::vector<std::uint8_t> psk = MakePsk(0xB1);

    PeerHubConfig server_config = MakeConfig("127.0.0.1", psk);
    // 拉长心跳，避免背景帧干扰「失败帧不增加 packets_opened」的统计窗口。
    server_config.heartbeat_interval_ms = 60000;
    server_config.peer_timeout_ms = 60000;
    PeerHub server(server_config);
    std::string error;
    BENTO_CHECK(server.Open(&error));

    SecureRelay relay;
    BENTO_CHECK(relay.Start(server.local_port(), &error));

    PeerHubConfig client_config = MakeConfig("127.0.0.1", psk);
    client_config.heartbeat_interval_ms = 60000;
    client_config.peer_timeout_ms = 60000;
    PeerHub client(client_config);
    BENTO_CHECK(client.Open(&error));
    BENTO_CHECK(client.AddPeer(1, "127.0.0.1", relay.port(), &error));

    int delivered = 0;
    int bad_payload = 0;
    int out_of_order = 0;
    int duplicate_delivery = 0;
    std::uint32_t expected_seq = 0;
    server.on_stream = [&](std::uint32_t peer_id, std::uint8_t channel,
                           const std::uint8_t* data, std::size_t bytes) {
        if (peer_id != 1 || channel != 0 ||
            VerifyMessage(data, bytes, kStreamTag) != kClientId) {
            ++bad_payload;
            return;
        }
        const std::uint32_t seq = ReadU32(data + 4);
        if (seq < expected_seq) {
            ++duplicate_delivery;
        } else if (seq > expected_seq) {
            ++out_of_order;
        } else {
            ++expected_seq;
            ++delivered;
        }
    };

    BENTO_CHECK(PumpSecure(relay, {&server, &client},
                           [&]() {
                               return server.stats().peers_active == 1 &&
                                      client.peer_secure_ready(1);
                           },
                           8000));
    BENTO_CHECK_EQ(server.secure_stats().handshakes, 1U);
    BENTO_CHECK_EQ(server.Peers().size(), static_cast<std::size_t>(1));
    // 清掉握手后的背景 ACK，确保下面的统计窗口里没有其他加密帧。
    (void)PumpSecure(relay, {&server, &client}, []() { return false; }, 50);

    // ---- 1. 重放：截获一条合法密文，原样再发一次 ----
    relay.CaptureNext();
    const std::vector<std::uint8_t> first = MakeMessage(kClientId, 0, kStreamTag);
    BENTO_CHECK(client.SendStream(1, 0, first.data(), first.size()));
    BENTO_CHECK(PumpSecure(relay, {&server, &client}, [&]() { return delivered >= 1; }, 5000));
    BENTO_CHECK_EQ(delivered, 1);
    BENTO_CHECK(relay.captured());
    const std::vector<std::uint8_t> replayed = relay.TakeCaptured();
    FrameHeader replay_header;
    BENTO_CHECK(go2cpp::bento::DecodeHeader(replayed.data(), replayed.size(), &replay_header));
    BENTO_CHECK_EQ(replay_header.type, FrameType::kStream);
    BENTO_CHECK((replay_header.flags & go2cpp::bento::kFlagEncrypted) != 0);

    (void)PumpSecure(relay, {&server}, []() { return false; }, 20);
    const PeerSecureStats before_replay = server.secure_stats();
    const int delivered_before_replay = delivered;
    relay.SendToServer(replayed);
    BENTO_CHECK(PumpSecure(relay, {&server},
                           [&]() {
                               return server.secure_stats().replay_drops >
                                      before_replay.replay_drops;
                           },
                           2000));
    const PeerSecureStats after_replay = server.secure_stats();
    BENTO_CHECK_EQ(after_replay.replay_drops, before_replay.replay_drops + 1);
    BENTO_CHECK_EQ(after_replay.auth_failures, before_replay.auth_failures);
    BENTO_CHECK_EQ(after_replay.packets_opened, before_replay.packets_opened);
    BENTO_CHECK_EQ(after_replay.malformed_frames, before_replay.malformed_frames);
    BENTO_CHECK_EQ(delivered, delivered_before_replay);
    BENTO_CHECK_EQ(duplicate_delivery, 0);

    // ---- 2. 篡改：扣住一条尚未交付的帧，发送翻转 1 字节的副本 ----
    relay.HoldNext();
    const std::vector<std::uint8_t> second = MakeMessage(kClientId, 1, kStreamTag);
    BENTO_CHECK(client.SendStream(1, 0, second.data(), second.size()));
    relay.Pump();  // 只驱动中继：截获并扣住该帧（服务端尚未见过它）
    BENTO_CHECK(relay.captured());
    const std::vector<std::uint8_t> tamper_source = relay.TakeCaptured();
    FrameHeader tamper_header;
    BENTO_CHECK(go2cpp::bento::DecodeHeader(tamper_source.data(), tamper_source.size(),
                                            &tamper_header));
    BENTO_CHECK(tamper_header.seq != replay_header.seq);
    std::vector<std::uint8_t> tampered = tamper_source;
    const std::size_t flip_offset = go2cpp::bento::kFrameHeaderBytes + 20;
    BENTO_CHECK(tampered.size() > flip_offset);
    if (tampered.size() > flip_offset) {
        tampered[flip_offset] ^= 0x5AU;
    }
    const PeerSecureStats before_tamper = server.secure_stats();
    relay.SendToServer(tampered);
    BENTO_CHECK(PumpSecure(relay, {&server},
                           [&]() {
                               return server.secure_stats().auth_failures >
                                      before_tamper.auth_failures;
                           },
                           2000));
    const PeerSecureStats after_tamper = server.secure_stats();
    BENTO_CHECK_EQ(after_tamper.auth_failures, before_tamper.auth_failures + 1);
    BENTO_CHECK_EQ(after_tamper.packets_opened, before_tamper.packets_opened);
    BENTO_CHECK_EQ(after_tamper.replay_drops, before_tamper.replay_drops);
    BENTO_CHECK_EQ(after_tamper.malformed_frames, before_tamper.malformed_frames);
    BENTO_CHECK_EQ(delivered, 1);

    // 原帧重发：认证通过并正常交付，证明坏包不会误杀会话。
    relay.SendToServer(tamper_source);
    BENTO_CHECK(PumpSecure(relay, {&server, &client}, [&]() { return delivered >= 2; }, 5000));
    BENTO_CHECK_EQ(delivered, 2);

    // ---- 3. 其余 198 条合法消息：按序全量送达 ----
    for (std::uint32_t seq = 2; seq < kTotalMessages; ++seq) {
        const std::vector<std::uint8_t> payload = MakeMessage(kClientId, seq, kStreamTag);
        BENTO_CHECK(client.SendStream(1, 0, payload.data(), payload.size()));
    }
    BENTO_CHECK(PumpSecure(relay, {&server, &client},
                           [&]() { return delivered >= static_cast<int>(kTotalMessages); },
                           30000));
    BENTO_CHECK_EQ(delivered, static_cast<int>(kTotalMessages));
    BENTO_CHECK_EQ(expected_seq, kTotalMessages);
    BENTO_CHECK_EQ(out_of_order, 0);
    BENTO_CHECK_EQ(duplicate_delivery, 0);
    BENTO_CHECK_EQ(bad_payload, 0);
    BENTO_CHECK_EQ(server.secure_stats().handshakes, 1U);
    BENTO_CHECK_EQ(server.secure_stats().plaintext_rejected, 0U);
    const PeerSecureStats final_stats = server.secure_stats();
    std::printf("[INFO] secure_replay_tamper: delivered=%d replay_drops=%llu auth_failures=%llu "
                "opened=%llu sealed=%llu\n",
                delivered, static_cast<unsigned long long>(final_stats.replay_drops),
                static_cast<unsigned long long>(final_stats.auth_failures),
                static_cast<unsigned long long>(final_stats.packets_opened),
                static_cast<unsigned long long>(final_stats.packets_sealed));
}

/// 场景 8：加密模式下的 STREAM 分片/大载荷。
/// @details 1200B/1201B 位于单帧载荷边界附近；4KB 跨分片；120KB 接近
///          `kMaxStreamFragments` 上限。断言按序交付、逐字节一致、分片统计对账。
void TestSecureFragmentedStream() {
    const std::uint8_t kChannel = 3;
    const std::vector<std::uint8_t> psk = MakePsk(0xC1);
    PeerHub server(MakeConfig("127.0.0.1", psk));
    std::string error;
    BENTO_CHECK(server.Open(&error));
    PeerHub client(MakeConfig("127.0.0.1", psk));
    BENTO_CHECK(client.Open(&error));
    BENTO_CHECK(client.AddPeer(1, "127.0.0.1", server.local_port(), &error));

    const std::vector<std::size_t> sizes{1200, 1201, 4096, 120U * 1024U};
    const auto expected_fragments = [](std::size_t bytes) -> std::size_t {
        return bytes > go2cpp::bento::kMaxFramePayload
                   ? (bytes + go2cpp::bento::kStreamFragmentBytes - 1) /
                         go2cpp::bento::kStreamFragmentBytes
                   : 1;
    };
    // 锁定边界分类：1200/1201 单帧；4KB/120KB 必须分片。
    BENTO_CHECK_EQ(expected_fragments(sizes[0]), static_cast<std::size_t>(1));
    BENTO_CHECK_EQ(expected_fragments(sizes[1]), static_cast<std::size_t>(1));
    BENTO_CHECK(expected_fragments(sizes[2]) > 1);
    BENTO_CHECK(expected_fragments(sizes[3]) > 1);

    std::vector<std::vector<std::uint8_t>> expected;
    std::uint64_t expected_frame_count = 0;
    std::uint64_t expected_fragmented_count = 0;
    for (std::size_t index = 0; index < sizes.size(); ++index) {
        std::vector<std::uint8_t> payload(sizes[index]);
        for (std::size_t i = 0; i < payload.size(); ++i) {
            payload[i] = static_cast<std::uint8_t>((index * 97U + i * 31U + (i >> 7)) & 0xFFU);
        }
        WriteU32(payload.data(), static_cast<std::uint32_t>(index));
        WriteU32(payload.data() + 4, static_cast<std::uint32_t>(payload.size()));
        const std::size_t fragments = expected_fragments(payload.size());
        expected_frame_count += static_cast<std::uint64_t>(fragments);
        if (fragments > 1) {
            expected_fragmented_count += static_cast<std::uint64_t>(fragments);
        }
        expected.push_back(payload);
    }

    int delivered = 0;
    int bad = 0;
    std::vector<std::vector<std::uint8_t>> received;
    server.on_stream = [&](std::uint32_t peer_id, std::uint8_t channel,
                           const std::uint8_t* data, std::size_t bytes) {
        if (peer_id != 1 || channel != kChannel) {
            ++bad;
            return;
        }
        received.emplace_back(data, data + bytes);
        ++delivered;
    };

    BENTO_CHECK(PumpHubs({&server, &client},
                         [&]() {
                             return server.stats().peers_active == 1 &&
                                    client.peer_secure_ready(1);
                         },
                         8000));
    for (const std::vector<std::uint8_t>& payload : expected) {
        BENTO_CHECK(client.SendStream(1, kChannel, payload.data(), payload.size()));
    }
    BENTO_CHECK(PumpHubs({&server, &client},
                         [&]() { return delivered >= static_cast<int>(expected.size()); },
                         30000));
    BENTO_CHECK_EQ(delivered, static_cast<int>(expected.size()));
    BENTO_CHECK_EQ(bad, 0);
    BENTO_CHECK_EQ(received.size(), expected.size());
    const std::size_t compare_count =
        received.size() < expected.size() ? received.size() : expected.size();
    for (std::size_t i = 0; i < compare_count; ++i) {
        BENTO_CHECK_EQ(received[i].size(), expected[i].size());
        BENTO_CHECK(received[i] == expected[i]);
    }

    const go2cpp::bento::ReliableChannel::Stats tx = client.reliable_stats(1, kChannel);
    const go2cpp::bento::ReliableChannel::Stats rx = server.reliable_stats(1, kChannel);
    BENTO_CHECK_EQ(tx.messages_sent, static_cast<std::uint64_t>(expected.size()));
    BENTO_CHECK_EQ(tx.fragments_sent, expected_frame_count);
    BENTO_CHECK_EQ(rx.messages_delivered, static_cast<std::uint64_t>(expected.size()));
    BENTO_CHECK_EQ(rx.messages_reassembled, 2U);
    BENTO_CHECK_EQ(rx.fragments_received, expected_fragmented_count);
    BENTO_CHECK_EQ(rx.fragments_invalid, 0U);
    BENTO_CHECK_EQ(rx.fragments_duplicate, 0U);
    BENTO_CHECK_EQ(rx.duplicates_dropped, 0U);
    BENTO_CHECK_EQ(rx.reassembly_timeouts, 0U);
    BENTO_CHECK_EQ(rx.stale_dropped, 0U);
    BENTO_CHECK(server.secure_stats().packets_opened >= expected_frame_count);
    BENTO_CHECK(client.secure_stats().packets_sealed >= expected_frame_count);
    BENTO_CHECK_EQ(server.secure_stats().auth_failures, 0U);
    BENTO_CHECK_EQ(server.secure_stats().plaintext_rejected, 0U);
    std::printf("[INFO] secure_fragmented: msgs=%zu frames=%llu fragmented=%llu "
                "reassembled=%llu delivered=%d\n",
                expected.size(), static_cast<unsigned long long>(expected_frame_count),
                static_cast<unsigned long long>(expected_fragmented_count),
                static_cast<unsigned long long>(rx.messages_reassembled), delivered);
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
    PeerHubConfig plain_config = MakeConfig("127.0.0.1", {});
    plain_config.require_encryption = false;
    PeerHub plain(plain_config);
    BENTO_CHECK(plain.Open(&error));
    BENTO_CHECK(plain.AddPeer(1, "127.0.0.1", 9, &error));
    BENTO_CHECK(!plain.peer_secure(1));
    BENTO_CHECK(!plain.peer_secure_ready(1));

    // 已连接的明文 peer 不会被 Hub 级 PSK「热升级」：否则在途帧会被对端
    // 按明文拒绝。明文会话必须保持可用。
    PeerHubConfig plain_server_config = MakeConfig("127.0.0.1", {});
    plain_server_config.require_encryption = false;
    PeerHub plain_server(plain_server_config);
    PeerHubConfig plain_peer_client_config = MakeConfig("127.0.0.1", {});
    plain_peer_client_config.require_encryption = false;
    PeerHub plain_client(plain_peer_client_config);
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

/// 墙钟毫秒（仅构造 HELLO 时间戳用，与 PeerHub 内部一致）。
std::uint64_t WallClockNowMs() {
    using namespace std::chrono;
    return static_cast<std::uint64_t>(
        duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count());
}

/// 手工构造一个加密 HELLO（模拟真实客户端/抓包重放）。
std::vector<std::uint8_t> BuildSecureHello(
    const std::vector<std::uint8_t>& psk, std::uint32_t session,
    const std::array<std::uint8_t, go2cpp::bento::kHandshakeNonceBytes>& nonce,
    std::uint32_t conn_id, std::uint32_t tx_seq, std::uint64_t timestamp_ms) {
    go2cpp::bento::HelloBody body;
    body.session = session;
    std::memcpy(body.nonce, nonce.data(), nonce.size());
    body.timestamp_ms = timestamp_ms;
    std::uint8_t raw[go2cpp::bento::kHelloBodyBytes];
    if (!go2cpp::bento::EncodeHelloBody(body, raw, sizeof(raw))) {
        return {};
    }
    std::vector<std::uint8_t> out;
    if (!go2cpp::bento::SealHandshakeFrame(
            psk.data(), go2cpp::bento::SecureDirection::kClientToServer,
            static_cast<std::uint8_t>(FrameType::kHello), session, conn_id, tx_seq, raw,
            sizeof(raw), &out)) {
        return {};
    }
    return out;
}

bool SendRawTo(int fd, const sockaddr_in& addr, const std::vector<std::uint8_t>& payload) {
    return ::sendto(fd, payload.data(), payload.size(), 0,
                    reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) ==
           static_cast<ssize_t>(payload.size());
}

/// 场景 8（PEB-02）：加密模式超时后 Connect 重连，服务端接收窗口重置、
/// 新流 seq=1 不被判 stale，双向数据恢复。
void TestSecureReconnectAfterTimeout() {
    const std::vector<std::uint8_t> psk = MakePsk(0x71);
    PeerHubConfig server_config = MakeConfig("127.0.0.1", psk);
    server_config.peer_timeout_ms = 3000;
    PeerHub server(server_config);
    PeerHubConfig client_config = MakeConfig("127.0.0.1", psk);
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
    BENTO_CHECK(PumpHubs(
        {&server, &client},
        [&]() {
            return client.peer_connected(1) && client.peer_secure_ready(1) &&
                   server.stats().peers_active == 1;
        },
        8000));
    const std::vector<std::uint8_t> payload = MakeMessage(7, 0, kStreamTag);
    BENTO_CHECK(client.SendStream(1, 0, payload.data(), payload.size()));
    BENTO_CHECK(server.SendStream(server.PeerIds().front(), 0, payload.data(), payload.size()));
    BENTO_CHECK(PumpHubs(
        {&server, &client},
        [&]() { return server_streams >= 1 && client_streams >= 1; }, 5000));

    // 只驱动客户端 -> 客户端超时（主动 peer 保留记录，可 Connect）。
    PumpFor({&client}, 800);
    BENTO_CHECK_EQ(client.peer_state(1), PeerState::kTimedOut);
    BENTO_CHECK_EQ(server.stats().peers_active, 1U);

    // 重连：新 session/nonce/conn_id；服务端必须 ResetChannels，
    // 否则新流 seq=1 会被判 stale 并被累计 ACK 静默丢掉。
    BENTO_CHECK(client.Connect(1));
    BENTO_CHECK(PumpHubs(
        {&server, &client},
        [&]() { return client.peer_connected(1) && client.peer_secure_ready(1); }, 8000));
    const std::uint32_t server_peer = server.PeerIds().front();
    BENTO_CHECK(server.peer_secure_ready(server_peer));
    const int server_before = server_streams;
    const int client_before = client_streams;
    BENTO_CHECK(client.SendStream(1, 0, payload.data(), payload.size()));
    BENTO_CHECK(server.SendStream(server_peer, 0, payload.data(), payload.size()));
    BENTO_CHECK(PumpHubs(
        {&server, &client},
        [&]() {
            return server_streams > server_before && client_streams > client_before;
        },
        5000));
    std::printf("[INFO] secure_reconnect: server_rx=%d client_rx=%d stale=%llu\n",
                server_streams, client_streams,
                static_cast<unsigned long long>(
                    server.reliable_stats(server_peer, 0).stale_dropped));
}

/// 场景 9（PEB-03）：窗口内重放旧 HELLO 被拒，活动会话不被重置。
void TestSecureHelloReplayRejected() {
    const std::vector<std::uint8_t> psk = MakePsk(0x72);
    PeerHubConfig server_config = MakeConfig("127.0.0.1", psk);
    server_config.peer_timeout_ms = 3000;
    server_config.handshake_replay_window_ms = 30000;
    // BuildSecureHello 手工构造的 HELLO 不带 X25519 公钥（模拟旧端），
    // 本用例只验证 nonce 重放，显式允许纯 PSK 降级。
    server_config.allow_legacy_peer = true;
    PeerHub server(server_config);
    std::string error;
    BENTO_CHECK(server.Open(&error));

    const int raw = ::socket(AF_INET, SOCK_DGRAM, 0);
    BENTO_CHECK(raw >= 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(server.local_port());
    BENTO_CHECK(::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr) == 1);

    std::array<std::uint8_t, go2cpp::bento::kHandshakeNonceBytes> nonce_a{};
    std::array<std::uint8_t, go2cpp::bento::kHandshakeNonceBytes> nonce_b{};
    for (std::size_t i = 0; i < nonce_a.size(); ++i) {
        nonce_a[i] = static_cast<std::uint8_t>(0x10 + i);
        nonce_b[i] = static_cast<std::uint8_t>(0x80 + i);
    }
    constexpr std::uint32_t kSessionA = 0x11112222U;
    constexpr std::uint32_t kSessionB = 0x33334444U;
    constexpr std::uint32_t kConnA = 0xAAAA0001U;
    constexpr std::uint32_t kConnB = 0xBBBB0002U;
    const std::uint64_t now = WallClockNowMs();
    const std::vector<std::uint8_t> hello_a =
        BuildSecureHello(psk, kSessionA, nonce_a, kConnA, 0x1001U, now);
    const std::vector<std::uint8_t> hello_b =
        BuildSecureHello(psk, kSessionB, nonce_b, kConnB, 0x2002U, now + 1000U);
    BENTO_CHECK(!hello_a.empty());
    BENTO_CHECK(!hello_b.empty());

    // 旧会话 A 建立。
    BENTO_CHECK(SendRawTo(raw, addr, hello_a));
    BENTO_CHECK(PumpHubs({&server}, [&]() { return server.stats().peers_active == 1; }, 3000));
    BENTO_CHECK_EQ(server.Peers().size(), 1U);
    const std::uint32_t peer_id = server.Peers().front().peer_id;
    BENTO_CHECK_EQ(server.Peers().front().conn_id, kConnA);

    // 对端换新 nonce 重连（会话 B）。
    BENTO_CHECK(SendRawTo(raw, addr, hello_b));
    BENTO_CHECK(PumpHubs(
        {&server},
        [&]() { return server.Peers().front().conn_id == kConnB; }, 3000));
    BENTO_CHECK(server.peer_secure_ready(peer_id));

    // 同 session、新 nonce/新 conn_id（老客户端重连未换 session）：
    // 服务端也应视为新会话并采纳新 conn_id。
    std::array<std::uint8_t, go2cpp::bento::kHandshakeNonceBytes> nonce_c{};
    for (std::size_t i = 0; i < nonce_c.size(); ++i) {
        nonce_c[i] = static_cast<std::uint8_t>(0xC0 + i);
    }
    constexpr std::uint32_t kConnC = 0xCCCC0003U;
    const std::vector<std::uint8_t> hello_c =
        BuildSecureHello(psk, kSessionB, nonce_c, kConnC, 0x3003U, now + 2000U);
    BENTO_CHECK(!hello_c.empty());
    BENTO_CHECK(SendRawTo(raw, addr, hello_c));
    BENTO_CHECK(PumpHubs(
        {&server},
        [&]() { return server.Peers().front().conn_id == kConnC; }, 3000));
    BENTO_CHECK(server.peer_secure_ready(peer_id));

    // 抓包重放旧 HELLO A：必须被已见 nonce 历史拒绝，会话保持 C。
    const std::uint64_t replay_before = server.secure_stats().replay_drops;
    BENTO_CHECK(SendRawTo(raw, addr, hello_a));
    BENTO_CHECK(PumpHubs(
        {&server},
        [&]() { return server.secure_stats().replay_drops > replay_before; }, 3000));
    BENTO_CHECK_EQ(server.Peers().size(), 1U);
    BENTO_CHECK_EQ(server.Peers().front().conn_id, kConnC);
    BENTO_CHECK_EQ(server.Peers().front().state, PeerState::kConnected);
    BENTO_CHECK(server.peer_secure_ready(peer_id));
    BENTO_CHECK_EQ(server.stats().peers_active, 1U);

    // 当前会话的 HELLO 重传仍是幂等路径（重发 ACK，不重置）。
    const std::uint64_t retries_before = server.secure_stats().handshake_retries;
    BENTO_CHECK(SendRawTo(raw, addr, hello_c));
    BENTO_CHECK(PumpHubs(
        {&server},
        [&]() { return server.secure_stats().handshake_retries > retries_before; }, 3000));
    BENTO_CHECK_EQ(server.Peers().front().conn_id, kConnC);

    std::printf(
        "[INFO] secure_hello_replay: replay_drops=%llu retries=%llu conn=%08x\n",
        static_cast<unsigned long long>(server.secure_stats().replay_drops),
        static_cast<unsigned long long>(server.secure_stats().handshake_retries),
        server.Peers().front().conn_id);
    ::close(raw);
}

}  // namespace

int main() {
    TestThreeSecurePeers();
    TestWrongPskRejected();
    TestPlaintextRejected();
    TestPerPeerPsk();
    TestSecureRetransmitRecovery();
    TestSecureReplayAndTamper();
    TestSecureFragmentedStream();
    TestPskValidation();
    TestSecureReconnectAfterTimeout();
    TestSecureHelloReplayRejected();
    return bento_test::Finish("test_peer_hub_secure");
}
