/**
 * @file test_x25519_handshake.cpp
 * @brief X25519 前向保密接入 PSK 握手的测试：KDF、兼容回退、篡改与低阶点。
 * @author go2cpp contributors
 * @date 2026-10-28
 * @copyright MIT License
 *
 * @details 覆盖：
 *   - HELLO / HELLO_ACK 新旧长度编解码与零公钥兼容；
 *   - PSK + X25519 共享密钥混合派生的双向一致性与域分离；
 *   - 前向保密：只有临时公钥 + PSK + nonce 无法解密会话密文；
 *   - SecureZero 清零；
 *   - Link 默认 X25519 握手、双向密文、统计；
 *   - 新客户端 ↔ 模拟旧服务端（52B ACK）、模拟旧客户端（36B HELLO）↔ 新服务端；
 *   - 公钥被篡改 → 会话密钥不一致 → AEAD 解密失败；
 *   - 低阶公钥 → 监听方拒绝握手且不回 ACK；
 *   - 256 轮随机 KDF 一致性与 2 万次派生吞吐。
 *
 * 断言只在主线程执行；`test_util.hpp` 的计数器不是原子变量。
 */

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <random>
#include <string>
#include <vector>

#include "go2cpp/bento/net/crypto.hpp"
#include "go2cpp/bento/net/link.hpp"
#include "go2cpp/bento/net/x25519.hpp"
#include "test_util.hpp"

namespace {

using namespace go2cpp::bento;

using Key = std::array<std::uint8_t, kX25519KeyBytes>;
using Psk = std::array<std::uint8_t, kPskBytes>;

Psk MakePsk(std::uint8_t seed) {
    Psk psk{};
    for (std::size_t i = 0; i < psk.size(); ++i) {
        psk[i] = static_cast<std::uint8_t>((i * 37U + seed * 11U + 0x5AU) & 0xFFU);
    }
    return psk;
}

void FillBytes(std::uint8_t* out, std::size_t bytes, std::uint8_t seed) {
    for (std::size_t i = 0; i < bytes; ++i) {
        out[i] = static_cast<std::uint8_t>((seed + i * 31U + 7U) & 0xFFU);
    }
}

bool IsZeroBytes(const std::uint8_t* data, std::size_t bytes) {
    std::uint8_t accumulator = 0;
    for (std::size_t i = 0; i < bytes; ++i) {
        accumulator = static_cast<std::uint8_t>(accumulator | data[i]);
    }
    return accumulator == 0;
}

std::uint64_t WallClockMs() {
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(now).count());
}

std::vector<std::uint8_t> MakePayload(std::uint32_t index, std::size_t bytes) {
    std::vector<std::uint8_t> payload(bytes, 0);
    for (std::size_t i = 0; i < bytes; ++i) {
        payload[i] = static_cast<std::uint8_t>((index * 29U + i * 5U + 3U) & 0xFFU);
    }
    payload[0] = static_cast<std::uint8_t>(index);
    payload[1] = static_cast<std::uint8_t>(index >> 8);
    return payload;
}

LinkConfig ServerConfig(const Psk& psk) {
    LinkConfig config;
    config.bind_address = "127.0.0.1";
    config.bind_port = 0;
    config.heartbeat_interval_ms = 100;
    config.peer_timeout_ms = 5000;
    config.pre_shared_key.assign(psk.begin(), psk.end());
    return config;
}

LinkConfig ClientConfig(std::uint16_t server_port, const Psk& psk) {
    LinkConfig config;
    config.bind_address = "127.0.0.1";
    config.bind_port = 0;
    config.peer_address = "127.0.0.1";
    config.peer_port = server_port;
    config.heartbeat_interval_ms = 100;
    config.peer_timeout_ms = 5000;
    config.handshake_timeout_ms = 3000;
    config.random_seed = 0x5EC0DE02U;
    config.pre_shared_key.assign(psk.begin(), psk.end());
    return config;
}

struct Datagram {
    std::vector<std::uint8_t> data;
    sockaddr_storage from{};
    socklen_t from_len = 0;
};

bool ReceiveDatagram(UdpSocket& socket, Datagram* out, int timeout_ms) {
    if (socket.WaitReadable(timeout_ms) <= 0) {
        return false;
    }
    out->data.resize(65536);
    out->from_len = sizeof(out->from);
    const int bytes =
        socket.RecvFrom(out->data.data(), out->data.size(), &out->from, &out->from_len);
    if (bytes <= 0) {
        return false;
    }
    out->data.resize(static_cast<std::size_t>(bytes));
    return true;
}

/// 驱动客户端并等待原始 socket 收到一个包；用于模拟旧服务端 / 篡改者。
bool PumpClientForPacket(Link& client, UdpSocket& peer, Datagram* packet, int timeout_ms) {
    const std::uint64_t deadline = NowMs() + static_cast<std::uint64_t>(timeout_ms);
    while (NowMs() < deadline) {
        client.RunOnce(1);
        if (ReceiveDatagram(peer, packet, 1)) {
            return true;
        }
    }
    return false;
}

/// 驱动监听方并等待原始 socket 收到一个包；用于模拟旧客户端。
bool PumpServerForPacket(Link& server, UdpSocket& peer, Datagram* packet, int timeout_ms) {
    const std::uint64_t deadline = NowMs() + static_cast<std::uint64_t>(timeout_ms);
    while (NowMs() < deadline) {
        server.RunOnce(1);
        if (ReceiveDatagram(peer, packet, 1)) {
            return true;
        }
    }
    return false;
}

/// 从对端 socket 收包，用给定 c2s 会话密钥尝试解出指定 STREAM 载荷。
bool ReceiveStreamWithKey(Link& client, UdpSocket& peer, const std::uint8_t key[32],
                          const std::vector<std::uint8_t>& payload, int timeout_ms) {
    ReplayWindow window;
    const std::uint64_t deadline = NowMs() + static_cast<std::uint64_t>(timeout_ms);
    while (NowMs() < deadline) {
        client.RunOnce(1);
        Datagram packet;
        if (!ReceiveDatagram(peer, &packet, 1)) {
            continue;
        }
        std::vector<std::uint8_t> inner;
        OpenStatus status = OpenStatus::kMalformed;
        if (!OpenFrame(key, SecureDirection::kClientToServer, &window, packet.data.data(),
                       packet.data.size(), &inner, &status)) {
            continue;
        }
        FrameView view;
        if (!ParseFrame(inner.data(), inner.size(), &view)) {
            continue;
        }
        if (view.header.type == FrameType::kStream && view.payload_bytes == payload.size() &&
            std::memcmp(view.payload, payload.data(), payload.size()) == 0) {
            return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
// 握手明文体兼容性
// ---------------------------------------------------------------------------

void TestHelloBodyCompatibility() {
    std::printf("[CASE] hello/ack body: legacy + x25519 layout\n");
    BENTO_CHECK_EQ(kLegacyHelloBodyBytes, 36U);
    BENTO_CHECK_EQ(kHelloBodyBytes, 68U);
    BENTO_CHECK_EQ(kLegacyHelloAckBodyBytes, 52U);
    BENTO_CHECK_EQ(kHelloAckBodyBytes, 84U);

    HelloBody hello;
    hello.session = 0x11223344U;
    FillBytes(hello.nonce, kHandshakeNonceBytes, 0x10);
    hello.timestamp_ms = 0x0102030405060708ULL;
    FillBytes(hello.x25519_public, kX25519KeyBytes, 0x30);

    std::uint8_t encoded[kHelloBodyBytes] = {};
    BENTO_CHECK(EncodeHelloBody(hello, encoded, sizeof(encoded)));
    HelloBody decoded;
    BENTO_CHECK(DecodeHelloBody(encoded, sizeof(encoded), &decoded));
    BENTO_CHECK_EQ(decoded.session, hello.session);
    BENTO_CHECK(std::memcmp(decoded.nonce, hello.nonce, kHandshakeNonceBytes) == 0);
    BENTO_CHECK_EQ(decoded.timestamp_ms, hello.timestamp_ms);
    BENTO_CHECK(std::memcmp(decoded.x25519_public, hello.x25519_public, kX25519KeyBytes) == 0);

    // 旧长度编码：前 36B 与新格式完全一致，旧对端可原样解析。
    std::uint8_t legacy[kLegacyHelloBodyBytes] = {};
    BENTO_CHECK(EncodeHelloBody(hello, legacy, sizeof(legacy)));
    BENTO_CHECK(std::memcmp(legacy, encoded, kLegacyHelloBodyBytes) == 0);
    HelloBody legacy_decoded;
    legacy_decoded.x25519_public[0] = 0xFF;  // 预填充，验证解码会清零公钥。
    BENTO_CHECK(DecodeHelloBody(legacy, sizeof(legacy), &legacy_decoded));
    BENTO_CHECK(IsZeroBytes(legacy_decoded.x25519_public, kX25519KeyBytes));

    // 旧对端只读前 36B 时，新 HELLO 仍能被解析且公钥归零。
    HelloBody truncated;
    BENTO_CHECK(DecodeHelloBody(encoded, kLegacyHelloBodyBytes, &truncated));
    BENTO_CHECK(IsZeroBytes(truncated.x25519_public, kX25519KeyBytes));

    // 模糊长度必须拒绝，不能把半截公钥当合法输入。
    BENTO_CHECK(!EncodeHelloBody(hello, encoded, kLegacyHelloBodyBytes + 1));
    BENTO_CHECK(!DecodeHelloBody(encoded, kHelloBodyBytes - 1, &decoded));

    // magic 破坏必须拒绝。
    encoded[0] ^= 0x01;
    BENTO_CHECK(!DecodeHelloBody(encoded, sizeof(encoded), &decoded));
    encoded[0] ^= 0x01;

    HelloAckBody ack;
    ack.session = 0x55667788U;
    FillBytes(ack.client_nonce, kHandshakeNonceBytes, 0x50);
    FillBytes(ack.server_nonce, kHandshakeNonceBytes, 0x70);
    ack.timestamp_ms = 0x1112131415161718ULL;
    FillBytes(ack.x25519_public, kX25519KeyBytes, 0x90);

    std::uint8_t ack_encoded[kHelloAckBodyBytes] = {};
    BENTO_CHECK(EncodeHelloAckBody(ack, ack_encoded, sizeof(ack_encoded)));
    HelloAckBody ack_decoded;
    BENTO_CHECK(DecodeHelloAckBody(ack_encoded, sizeof(ack_encoded), &ack_decoded));
    BENTO_CHECK_EQ(ack_decoded.session, ack.session);
    BENTO_CHECK(std::memcmp(ack_decoded.client_nonce, ack.client_nonce,
                            kHandshakeNonceBytes) == 0);
    BENTO_CHECK(std::memcmp(ack_decoded.server_nonce, ack.server_nonce,
                            kHandshakeNonceBytes) == 0);
    BENTO_CHECK(std::memcmp(ack_decoded.x25519_public, ack.x25519_public,
                            kX25519KeyBytes) == 0);

    std::uint8_t ack_legacy[kLegacyHelloAckBodyBytes] = {};
    BENTO_CHECK(EncodeHelloAckBody(ack, ack_legacy, sizeof(ack_legacy)));
    HelloAckBody ack_legacy_decoded;
    ack_legacy_decoded.x25519_public[31] = 0xFF;
    BENTO_CHECK(DecodeHelloAckBody(ack_legacy, sizeof(ack_legacy), &ack_legacy_decoded));
    BENTO_CHECK(IsZeroBytes(ack_legacy_decoded.x25519_public, kX25519KeyBytes));
    BENTO_CHECK(!DecodeHelloAckBody(ack_encoded, kHelloAckBodyBytes - 1, &ack_decoded));
}

// ---------------------------------------------------------------------------
// KDF
// ---------------------------------------------------------------------------

void TestX25519Kdf() {
    std::printf("[CASE] kdf: both sides agree, psk-only differs, halves both absorbed\n");
    Key client_private{};
    Key server_private{};
    Key client_public{};
    Key server_public{};
    X25519GeneratePrivateKey(client_private.data());
    X25519GeneratePrivateKey(server_private.data());
    BENTO_CHECK(X25519PublicKeyFromPrivate(client_private.data(), client_public.data()));
    BENTO_CHECK(X25519PublicKeyFromPrivate(server_private.data(), server_public.data()));

    Key client_shared{};
    Key server_shared{};
    BENTO_CHECK(X25519ComputeSharedSecret(client_private.data(), server_public.data(),
                                          client_shared.data()));
    BENTO_CHECK(X25519ComputeSharedSecret(server_private.data(), client_public.data(),
                                          server_shared.data()));
    BENTO_CHECK(client_shared == server_shared);
    BENTO_CHECK(!IsZeroBytes(client_shared.data(), client_shared.size()));

    const Psk psk = MakePsk(0x51);
    std::uint8_t client_nonce[kHandshakeNonceBytes] = {};
    std::uint8_t server_nonce[kHandshakeNonceBytes] = {};
    FillBytes(client_nonce, sizeof(client_nonce), 0x21);
    FillBytes(server_nonce, sizeof(server_nonce), 0x41);

    std::uint8_t c2s_client[32] = {};
    std::uint8_t c2s_server[32] = {};
    std::uint8_t s2c_client[32] = {};
    DeriveSessionKeyWithX25519(psk.data(), client_shared.data(), client_nonce, server_nonce,
                               SecureDirection::kClientToServer, c2s_client);
    DeriveSessionKeyWithX25519(psk.data(), server_shared.data(), client_nonce, server_nonce,
                               SecureDirection::kClientToServer, c2s_server);
    DeriveSessionKeyWithX25519(psk.data(), client_shared.data(), client_nonce, server_nonce,
                               SecureDirection::kServerToClient, s2c_client);
    BENTO_CHECK(std::memcmp(c2s_client, c2s_server, 32) == 0);
    BENTO_CHECK(std::memcmp(c2s_client, s2c_client, 32) != 0);

    // 纯 PSK 派生必须与混合派生不同，否则前向保密形同虚设。
    std::uint8_t psk_only[32] = {};
    DeriveSessionKey(psk.data(), client_nonce, server_nonce,
                     SecureDirection::kClientToServer, psk_only);
    BENTO_CHECK(std::memcmp(c2s_client, psk_only, 32) != 0);

    // 共享密钥前后两半都要参与；只混前 16B 的实现会被这里抓住。
    Key flipped = client_shared;
    flipped[0] ^= 0x01;
    std::uint8_t flipped_key[32] = {};
    DeriveSessionKeyWithX25519(psk.data(), flipped.data(), client_nonce, server_nonce,
                               SecureDirection::kClientToServer, flipped_key);
    BENTO_CHECK(std::memcmp(c2s_client, flipped_key, 32) != 0);

    flipped = client_shared;
    flipped[31] ^= 0x01;
    DeriveSessionKeyWithX25519(psk.data(), flipped.data(), client_nonce, server_nonce,
                               SecureDirection::kClientToServer, flipped_key);
    BENTO_CHECK(std::memcmp(c2s_client, flipped_key, 32) != 0);

    // 确定性。
    std::uint8_t again[32] = {};
    DeriveSessionKeyWithX25519(psk.data(), client_shared.data(), client_nonce, server_nonce,
                               SecureDirection::kClientToServer, again);
    BENTO_CHECK(std::memcmp(c2s_client, again, 32) == 0);
}

void TestForwardSecrecyAfterPskCompromise() {
    std::printf("[CASE] forward secrecy: public keys + psk cannot decrypt session\n");
    Key client_private{};
    Key server_private{};
    Key client_public{};
    Key server_public{};
    X25519GeneratePrivateKey(client_private.data());
    X25519GeneratePrivateKey(server_private.data());
    BENTO_CHECK(X25519PublicKeyFromPrivate(client_private.data(), client_public.data()));
    BENTO_CHECK(X25519PublicKeyFromPrivate(server_private.data(), server_public.data()));

    Key shared{};
    BENTO_CHECK(X25519ComputeSharedSecret(client_private.data(), server_public.data(),
                                          shared.data()));

    const Psk psk = MakePsk(0x61);
    std::uint8_t client_nonce[kHandshakeNonceBytes] = {};
    std::uint8_t server_nonce[kHandshakeNonceBytes] = {};
    FillBytes(client_nonce, sizeof(client_nonce), 0x23);
    FillBytes(server_nonce, sizeof(server_nonce), 0x43);

    std::uint8_t session_key[32] = {};
    DeriveSessionKeyWithX25519(psk.data(), shared.data(), client_nonce, server_nonce,
                               SecureDirection::kClientToServer, session_key);

    constexpr char kMessage[] = "forward-secret-payload";
    std::uint8_t nonce[kChaCha20NonceBytes] = {};
    MakeSecureNonce(SecureDirection::kClientToServer, 1, nonce);
    std::uint8_t ciphertext[sizeof(kMessage)] = {};
    std::uint8_t tag[kAeadTagBytes] = {};
    AeadSeal(session_key, nonce, nullptr, 0, reinterpret_cast<const std::uint8_t*>(kMessage),
             sizeof(kMessage), ciphertext, tag);

    // 攻击者事后拿到 PSK、双方临时公钥和 nonce（不含任何私钥）。
    std::uint8_t attacker_psk_only[32] = {};
    DeriveSessionKey(psk.data(), client_nonce, server_nonce,
                     SecureDirection::kClientToServer, attacker_psk_only);
    BENTO_CHECK(std::memcmp(attacker_psk_only, session_key, 32) != 0);

    std::uint8_t recovered[sizeof(kMessage)] = {};
    BENTO_CHECK(!AeadOpen(attacker_psk_only, nonce, nullptr, 0, ciphertext, sizeof(ciphertext),
                          tag, recovered));

    // 用全零共享密钥假装 X25519 派生（等价于把公钥清零攻击）也打不开。
    std::uint8_t zero_shared[kX25519KeyBytes] = {};
    std::uint8_t zero_key[32] = {};
    DeriveSessionKeyWithX25519(psk.data(), zero_shared, client_nonce, server_nonce,
                               SecureDirection::kClientToServer, zero_key);
    BENTO_CHECK(!AeadOpen(zero_key, nonce, nullptr, 0, ciphertext, sizeof(ciphertext), tag,
                          recovered));

    // 临时私钥用完即清（模拟握手结束后的状态）。
    Key leaked_private = client_private;
    SecureZero(leaked_private.data(), leaked_private.size());
    BENTO_CHECK(IsZeroBytes(leaked_private.data(), leaked_private.size()));
}

void TestSecureZero() {
    std::printf("[CASE] secure zero\n");
    std::uint8_t buffer[64];
    std::memset(buffer, 0xA5, sizeof(buffer));
    SecureZero(buffer, sizeof(buffer));
    BENTO_CHECK(IsZeroBytes(buffer, sizeof(buffer)));

    buffer[0] = 0x11;
    SecureZero(buffer, 0);
    BENTO_CHECK_EQ(buffer[0], 0x11U);
    SecureZero(nullptr, 16);
    SecureZero(nullptr, 0);
}

void TestLowOrderPublicKeyRejected() {
    std::printf("[CASE] x25519 low-order peer keys rejected\n");
    Key private_key{};
    X25519GeneratePrivateKey(private_key.data());

    Key zero{};
    Key one{};
    one[0] = 1;
    Key p_minus_one{};
    p_minus_one[0] = 0xEC;
    for (std::size_t i = 1; i < kX25519KeyBytes - 1; ++i) {
        p_minus_one[i] = 0xFF;
    }
    p_minus_one[kX25519KeyBytes - 1] = 0x7F;

    Key shared{};
    shared.fill(0xFF);
    BENTO_CHECK(!X25519ComputeSharedSecret(private_key.data(), zero.data(), shared.data()));
    BENTO_CHECK(IsZeroBytes(shared.data(), shared.size()));

    shared.fill(0xFF);
    BENTO_CHECK(!X25519ComputeSharedSecret(private_key.data(), one.data(), shared.data()));
    BENTO_CHECK(IsZeroBytes(shared.data(), shared.size()));

    shared.fill(0xFF);
    BENTO_CHECK(!X25519ComputeSharedSecret(private_key.data(), p_minus_one.data(),
                                           shared.data()));
    BENTO_CHECK(IsZeroBytes(shared.data(), shared.size()));

    // 正常基点公钥必须成功。
    Key base_public{};
    BENTO_CHECK(X25519PublicKeyFromPrivate(private_key.data(), base_public.data()));
    BENTO_CHECK(X25519ComputeSharedSecret(private_key.data(), base_public.data(), shared.data()));
    BENTO_CHECK(!IsZeroBytes(shared.data(), shared.size()));
}

// ---------------------------------------------------------------------------
// Link 集成：真实 UDP 回环
// ---------------------------------------------------------------------------

void TestLinkX25519Handshake() {
    std::printf("[CASE] link: default x25519 handshake + bidirectional ciphertext\n");
    const Psk psk = MakePsk(0x71);
    LinkConfig server_config = ServerConfig(psk);
    Link server(server_config);
    std::string error;
    BENTO_CHECK(server.Open(&error));

    LinkConfig client_config = ClientConfig(server.local_port(), psk);
    Link client(client_config);
    BENTO_CHECK(client.Open(&error));

    bool server_connected = false;
    bool client_connected = false;
    server.on_connected = [&server_connected]() { server_connected = true; };
    client.on_connected = [&client_connected]() { client_connected = true; };

    std::vector<std::vector<std::uint8_t>> server_received;
    std::vector<std::vector<std::uint8_t>> client_received;
    server.on_stream = [&server_received](std::uint8_t channel, const std::uint8_t* data,
                                          std::size_t bytes) {
        BENTO_CHECK_EQ(channel, 0U);
        server_received.emplace_back(data, data + bytes);
    };
    client.on_stream = [&client_received](std::uint8_t channel, const std::uint8_t* data,
                                          std::size_t bytes) {
        BENTO_CHECK_EQ(channel, 1U);
        client_received.emplace_back(data, data + bytes);
    };

    client.Connect();
    BENTO_CHECK(bento_test::PumpUntil(
        client, server, [&]() { return server_connected && client_connected; }, 5000));
    BENTO_CHECK(server.session_established());
    BENTO_CHECK(client.session_established());
    BENTO_CHECK_EQ(server.secure_stats().x25519_handshakes, 1U);
    BENTO_CHECK_EQ(client.secure_stats().x25519_handshakes, 1U);
    BENTO_CHECK_EQ(server.secure_stats().psk_fallbacks, 0U);
    BENTO_CHECK_EQ(client.secure_stats().psk_fallbacks, 0U);

    constexpr std::uint32_t kMessages = 16;
    for (std::uint32_t i = 0; i < kMessages; ++i) {
        const std::vector<std::uint8_t> payload = MakePayload(i, 96);
        BENTO_CHECK(client.SendStream(0, payload.data(), payload.size()));
        BENTO_CHECK(server.SendStream(1, payload.data(), payload.size()));
    }
    BENTO_CHECK(bento_test::PumpUntil(
        client, server,
        [&]() {
            return server_received.size() == kMessages && client_received.size() == kMessages;
        },
        10000));
    BENTO_CHECK_EQ(server_received.size(), kMessages);
    BENTO_CHECK_EQ(client_received.size(), kMessages);
    for (std::uint32_t i = 0; i < kMessages; ++i) {
        BENTO_CHECK(server_received[i] == MakePayload(i, 96));
        BENTO_CHECK(client_received[i] == MakePayload(i, 96));
    }
    BENTO_CHECK(server.secure_stats().packets_opened > 0);
    BENTO_CHECK(client.secure_stats().packets_opened > 0);
    BENTO_CHECK_EQ(server.secure_stats().auth_failures, 0U);

    client.Close();
    server.Close();
}

void TestLegacyServerFallback() {
    std::printf("[CASE] link: new client falls back to psk against legacy server\n");
    const Psk psk = MakePsk(0x81);
    UdpSocket legacy;
    std::string error;
    BENTO_CHECK(legacy.Open("127.0.0.1", 0, &error));

    LinkConfig client_config = ClientConfig(legacy.local_port(), psk);
    // 显式接受无 X25519 公钥的旧服务端（否则默认策略会拒绝）。
    client_config.allow_legacy_peer = true;
    client_config.hello_initial_pto_ms = 100;
    Link client(client_config);
    BENTO_CHECK(client.Open(&error));
    bool connected = false;
    client.on_connected = [&connected]() { connected = true; };
    client.Connect();

    Datagram hello_packet;
    BENTO_CHECK(PumpClientForPacket(client, legacy, &hello_packet, 3000));
    std::uint8_t inner_type = 0;
    std::uint32_t session = 0;
    std::uint32_t conn_id = 0;
    std::vector<std::uint8_t> body;
    OpenStatus status = OpenStatus::kMalformed;
    BENTO_CHECK(OpenHandshakeFrame(psk.data(), SecureDirection::kClientToServer,
                                   hello_packet.data.data(), hello_packet.data.size(),
                                   &inner_type, &session, &conn_id, &body, &status));
    BENTO_CHECK_EQ(inner_type, static_cast<std::uint8_t>(FrameType::kHello));
    BENTO_CHECK_EQ(session, client.session_id());
    BENTO_CHECK_EQ(conn_id, client.conn_id());
    HelloBody hello;
    BENTO_CHECK(DecodeHelloBody(body.data(), body.size(), &hello));
    BENTO_CHECK_EQ(body.size(), kHelloBodyBytes);
    BENTO_CHECK(!IsZeroBytes(hello.x25519_public, kX25519KeyBytes));

    // 模拟旧服务端：只回 52B ACK，不含 X25519 公钥。
    HelloAckBody ack;
    ack.session = session;
    std::memcpy(ack.client_nonce, hello.nonce, kHandshakeNonceBytes);
    FillBytes(ack.server_nonce, kHandshakeNonceBytes, 0x91);
    ack.timestamp_ms = WallClockMs();
    std::uint8_t legacy_ack[kLegacyHelloAckBodyBytes] = {};
    BENTO_CHECK(EncodeHelloAckBody(ack, legacy_ack, sizeof(legacy_ack)));
    std::vector<std::uint8_t> ack_packet;
    BENTO_CHECK(SealHandshakeFrame(psk.data(), SecureDirection::kServerToClient,
                                   static_cast<std::uint8_t>(FrameType::kHelloAck), session,
                                   conn_id, 9, legacy_ack, sizeof(legacy_ack), &ack_packet));
    BENTO_CHECK(legacy.SendTo(hello_packet.from, hello_packet.from_len, ack_packet.data(),
                              ack_packet.size()));

    BENTO_CHECK(bento_test::PumpUntil(client, [&]() { return connected; }, 3000));
    BENTO_CHECK_EQ(client.secure_stats().psk_fallbacks, 1U);
    BENTO_CHECK_EQ(client.secure_stats().x25519_handshakes, 0U);
    BENTO_CHECK(client.session_established());

    // 回退会话必须真的能用纯 PSK 密钥解密客户端数据帧。
    std::uint8_t rx_key[kChaCha20KeyBytes] = {};
    DeriveSessionKey(psk.data(), hello.nonce, ack.server_nonce,
                     SecureDirection::kClientToServer, rx_key);
    const std::vector<std::uint8_t> payload = MakePayload(3, 64);
    BENTO_CHECK(client.SendStream(0, payload.data(), payload.size()));
    BENTO_CHECK(ReceiveStreamWithKey(client, legacy, rx_key, payload, 3000));

    client.Close();
}

void TestLegacyClientFallback() {
    std::printf("[CASE] link: new server falls back to psk against legacy client\n");
    const Psk psk = MakePsk(0x91);
    LinkConfig server_config = ServerConfig(psk);
    // 显式接受无 X25519 公钥的旧客户端（否则默认策略会拒绝）。
    server_config.allow_legacy_peer = true;
    Link server(server_config);
    std::string error;
    BENTO_CHECK(server.Open(&error));
    bool connected = false;
    server.on_connected = [&connected]() { connected = true; };
    std::vector<std::vector<std::uint8_t>> received;
    server.on_stream = [&received](std::uint8_t /*channel*/, const std::uint8_t* data,
                                   std::size_t bytes) {
        received.emplace_back(data, data + bytes);
    };

    UdpSocket legacy_client;
    BENTO_CHECK(legacy_client.Open("127.0.0.1", 0, &error));
    sockaddr_storage server_address{};
    socklen_t server_address_len = 0;
    BENTO_CHECK(UdpSocket::Resolve("127.0.0.1", server.local_port(), &server_address,
                                   &server_address_len, &error));

    // 模拟旧客户端：36B HELLO，无公钥。
    HelloBody hello;
    hello.session = 0x22334455U;
    FillBytes(hello.nonce, kHandshakeNonceBytes, 0xA1);
    hello.timestamp_ms = WallClockMs();
    std::uint8_t raw[kLegacyHelloBodyBytes] = {};
    BENTO_CHECK(EncodeHelloBody(hello, raw, sizeof(raw)));
    constexpr std::uint32_t kConnId = 0x55667788U;
    std::vector<std::uint8_t> hello_packet;
    BENTO_CHECK(SealHandshakeFrame(psk.data(), SecureDirection::kClientToServer,
                                   static_cast<std::uint8_t>(FrameType::kHello), hello.session,
                                   kConnId, 5, raw, sizeof(raw), &hello_packet));
    BENTO_CHECK(legacy_client.SendTo(server_address, server_address_len, hello_packet.data(),
                                     hello_packet.size()));

    Datagram ack_packet;
    BENTO_CHECK(PumpServerForPacket(server, legacy_client, &ack_packet, 3000));
    std::uint8_t inner_type = 0;
    std::uint32_t ack_session = 0;
    std::uint32_t ack_conn_id = 0;
    std::vector<std::uint8_t> body;
    OpenStatus status = OpenStatus::kMalformed;
    BENTO_CHECK(OpenHandshakeFrame(psk.data(), SecureDirection::kServerToClient,
                                   ack_packet.data.data(), ack_packet.data.size(), &inner_type,
                                   &ack_session, &ack_conn_id, &body, &status));
    HelloAckBody ack;
    BENTO_CHECK(DecodeHelloAckBody(body.data(), body.size(), &ack));
    // 回退纯 PSK 时监听方使用旧 52B 布局，便于与旧对端互操作。
    BENTO_CHECK_EQ(body.size(), kLegacyHelloAckBodyBytes);
    BENTO_CHECK(IsZeroBytes(ack.x25519_public, kX25519KeyBytes));
    BENTO_CHECK_EQ(ack.session, hello.session);
    BENTO_CHECK(std::memcmp(ack.client_nonce, hello.nonce, kHandshakeNonceBytes) == 0);
    BENTO_CHECK(connected);
    BENTO_CHECK_EQ(server.secure_stats().psk_fallbacks, 1U);
    BENTO_CHECK_EQ(server.secure_stats().x25519_handshakes, 0U);

    // 用纯 PSK 派生密钥注入一条 STREAM，验证回退会话仍可收数据。
    std::uint8_t tx_key[kChaCha20KeyBytes] = {};
    DeriveSessionKey(psk.data(), hello.nonce, ack.server_nonce,
                     SecureDirection::kClientToServer, tx_key);
    const std::vector<std::uint8_t> payload = MakePayload(4, 48);
    FrameHeader header;
    header.type = FrameType::kStream;
    header.channel = 0;
    header.conn_id = kConnId;
    header.seq = 1;
    const std::vector<std::uint8_t> inner =
        BuildFrame(header, nullptr, payload.data(), payload.size());
    BENTO_CHECK(!inner.empty());
    std::vector<std::uint8_t> sealed;
    BENTO_CHECK(SealFrame(tx_key, SecureDirection::kClientToServer, 1, inner.data(),
                          inner.size(), &sealed));
    BENTO_CHECK(legacy_client.SendTo(server_address, server_address_len, sealed.data(),
                                     sealed.size()));
    BENTO_CHECK(bento_test::PumpUntil(server, [&]() { return !received.empty(); }, 2000));
    BENTO_CHECK_EQ(received.size(), 1U);
    BENTO_CHECK(received[0] == payload);

    server.Close();
}

void TestTamperedPublicKey() {
    std::printf("[CASE] link: tampered ack public key -> key mismatch -> decrypt fails\n");
    const Psk psk = MakePsk(0xA1);
    UdpSocket evil;
    std::string error;
    BENTO_CHECK(evil.Open("127.0.0.1", 0, &error));

    LinkConfig client_config = ClientConfig(evil.local_port(), psk);
    client_config.peer_timeout_ms = 3000;
    client_config.hello_initial_pto_ms = 100;
    Link client(client_config);
    BENTO_CHECK(client.Open(&error));
    bool connected = false;
    client.on_connected = [&connected]() { connected = true; };
    std::vector<std::vector<std::uint8_t>> received;
    client.on_stream = [&received](std::uint8_t /*channel*/, const std::uint8_t* data,
                                   std::size_t bytes) {
        received.emplace_back(data, data + bytes);
    };
    client.Connect();

    Datagram hello_packet;
    BENTO_CHECK(PumpClientForPacket(client, evil, &hello_packet, 3000));
    std::uint8_t inner_type = 0;
    std::uint32_t session = 0;
    std::uint32_t conn_id = 0;
    std::vector<std::uint8_t> body;
    OpenStatus status = OpenStatus::kMalformed;
    BENTO_CHECK(OpenHandshakeFrame(psk.data(), SecureDirection::kClientToServer,
                                   hello_packet.data.data(), hello_packet.data.size(),
                                   &inner_type, &session, &conn_id, &body, &status));
    HelloBody hello;
    BENTO_CHECK(DecodeHelloBody(body.data(), body.size(), &hello));
    BENTO_CHECK(!IsZeroBytes(hello.x25519_public, kX25519KeyBytes));

    // 中间人持有 PSK：生成自己的临时密钥对，但把 ACK 里的公钥篡改 1 bit。
    Key server_private{};
    Key server_public{};
    Key true_shared{};
    X25519GeneratePrivateKey(server_private.data());
    BENTO_CHECK(X25519PublicKeyFromPrivate(server_private.data(), server_public.data()));
    BENTO_CHECK(X25519ComputeSharedSecret(server_private.data(), hello.x25519_public,
                                          true_shared.data()));
    Key tampered_public = server_public;
    tampered_public[0] ^= 0x01;

    HelloAckBody ack;
    ack.session = session;
    std::memcpy(ack.client_nonce, hello.nonce, kHandshakeNonceBytes);
    FillBytes(ack.server_nonce, kHandshakeNonceBytes, 0xB1);
    std::memcpy(ack.x25519_public, tampered_public.data(), kX25519KeyBytes);
    ack.timestamp_ms = WallClockMs();
    std::uint8_t raw[kHelloAckBodyBytes] = {};
    BENTO_CHECK(EncodeHelloAckBody(ack, raw, sizeof(raw)));
    std::vector<std::uint8_t> ack_packet;
    BENTO_CHECK(SealHandshakeFrame(psk.data(), SecureDirection::kServerToClient,
                                   static_cast<std::uint8_t>(FrameType::kHelloAck), session,
                                   conn_id, 11, raw, sizeof(raw), &ack_packet));
    BENTO_CHECK(evil.SendTo(hello_packet.from, hello_packet.from_len, ack_packet.data(),
                            ack_packet.size()));

    BENTO_CHECK(bento_test::PumpUntil(client, [&]() { return connected; }, 3000));
    BENTO_CHECK_EQ(client.secure_stats().x25519_handshakes, 1U);

    std::uint8_t true_c2s[kChaCha20KeyBytes] = {};
    std::uint8_t true_s2c[kChaCha20KeyBytes] = {};
    DeriveSessionKeyWithX25519(psk.data(), true_shared.data(), hello.nonce, ack.server_nonce,
                               SecureDirection::kClientToServer, true_c2s);
    DeriveSessionKeyWithX25519(psk.data(), true_shared.data(), hello.nonce, ack.server_nonce,
                               SecureDirection::kServerToClient, true_s2c);

    // 客户端用被篡改的公钥派生，密文用真共享密钥打不开。
    const std::vector<std::uint8_t> payload = MakePayload(5, 40);
    BENTO_CHECK(client.SendStream(0, payload.data(), payload.size()));
    ReplayWindow window;
    bool opened_stream = false;
    const std::uint64_t deadline = NowMs() + 800;
    while (NowMs() < deadline) {
        client.RunOnce(1);
        Datagram packet;
        if (!ReceiveDatagram(evil, &packet, 1)) {
            continue;
        }
        std::vector<std::uint8_t> inner;
        OpenStatus open_status = OpenStatus::kMalformed;
        if (!OpenFrame(true_c2s, SecureDirection::kClientToServer, &window, packet.data.data(),
                       packet.data.size(), &inner, &open_status)) {
            continue;
        }
        FrameView view;
        if (ParseFrame(inner.data(), inner.size(), &view) &&
            view.header.type == FrameType::kStream) {
            opened_stream = true;
            break;
        }
    }
    BENTO_CHECK(!opened_stream);

    // 反向：用真密钥发一帧给客户端，客户端无法解密，也不触发 on_stream。
    BENTO_CHECK(client.state() == LinkState::kConnected);
    FrameHeader header;
    header.type = FrameType::kStream;
    header.channel = 0;
    header.conn_id = conn_id;
    header.seq = 1;
    const std::vector<std::uint8_t> forged_payload = MakePayload(6, 40);
    const std::vector<std::uint8_t> inner =
        BuildFrame(header, nullptr, forged_payload.data(), forged_payload.size());
    BENTO_CHECK(!inner.empty());
    std::vector<std::uint8_t> sealed;
    BENTO_CHECK(SealFrame(true_s2c, SecureDirection::kServerToClient, 1, inner.data(),
                          inner.size(), &sealed));
    BENTO_CHECK(evil.SendTo(hello_packet.from, hello_packet.from_len, sealed.data(),
                            sealed.size()));
    BENTO_CHECK(bento_test::PumpUntil(
        client, [&]() { return client.secure_stats().auth_failures > 0; }, 1000));
    BENTO_CHECK(client.secure_stats().auth_failures > 0);
    BENTO_CHECK(received.empty());

    client.Close();
}

void TestLowOrderPeerKeyRejectedByServer() {
    std::printf("[CASE] link: low-order public key aborts handshake without ack\n");
    const Psk psk = MakePsk(0xB1);
    Link server(ServerConfig(psk));
    std::string error;
    BENTO_CHECK(server.Open(&error));
    bool connected = false;
    server.on_connected = [&connected]() { connected = true; };

    UdpSocket attacker;
    BENTO_CHECK(attacker.Open("127.0.0.1", 0, &error));
    sockaddr_storage server_address{};
    socklen_t server_address_len = 0;
    BENTO_CHECK(UdpSocket::Resolve("127.0.0.1", server.local_port(), &server_address,
                                   &server_address_len, &error));

    HelloBody hello;
    hello.session = 0x33445566U;
    FillBytes(hello.nonce, kHandshakeNonceBytes, 0xC1);
    hello.timestamp_ms = WallClockMs();
    hello.x25519_public[0] = 1;  // u = 1：低阶点，共享密钥必为全零。
    std::uint8_t raw[kHelloBodyBytes] = {};
    BENTO_CHECK(EncodeHelloBody(hello, raw, sizeof(raw)));
    std::vector<std::uint8_t> hello_packet;
    BENTO_CHECK(SealHandshakeFrame(psk.data(), SecureDirection::kClientToServer,
                                   static_cast<std::uint8_t>(FrameType::kHello), hello.session,
                                   0x99AABBCCU, 1, raw, sizeof(raw), &hello_packet));
    BENTO_CHECK(attacker.SendTo(server_address, server_address_len, hello_packet.data(),
                                hello_packet.size()));

    (void)bento_test::PumpUntil(server, []() { return false; }, 400);
    BENTO_CHECK(!connected);
    BENTO_CHECK(!server.session_established());
    BENTO_CHECK(server.state() != LinkState::kConnected);
    BENTO_CHECK(server.secure_stats().auth_failures >= 1U);
    BENTO_CHECK_EQ(server.secure_stats().x25519_handshakes, 0U);
    BENTO_CHECK_EQ(server.secure_stats().psk_fallbacks, 0U);

    Datagram reply;
    BENTO_CHECK(!ReceiveDatagram(attacker, &reply, 150));

    server.Close();
}

// ---------------------------------------------------------------------------
// 0-RTT 与 X25519 的互斥策略
// ---------------------------------------------------------------------------

void TestZeroRttPolicy() {
    std::printf("[CASE] zero-rtt: x25519 session has no ticket, psk session works\n");
    const Psk psk = MakePsk(0xD1);
    std::string error;

    // X25519 会话：票据会按纯 PSK 重派生，导致静默降级，必须拒绝导出。
    {
        Link server(ServerConfig(psk));
        BENTO_CHECK(server.Open(&error));
        Link client(ClientConfig(server.local_port(), psk));
        BENTO_CHECK(client.Open(&error));
        bool server_connected = false;
        bool client_connected = false;
        server.on_connected = [&server_connected]() { server_connected = true; };
        client.on_connected = [&client_connected]() { client_connected = true; };
        client.Connect();
        BENTO_CHECK(bento_test::PumpUntil(
            client, server, [&]() { return server_connected && client_connected; }, 5000));
        BENTO_CHECK_EQ(client.secure_stats().x25519_handshakes, 1U);
        BENTO_CHECK(client.ExportSessionTicket().empty());
        BENTO_CHECK(server.ExportSessionTicket().empty());
        client.Close();
        server.Close();
    }

    // 纯 PSK 会话（enable_x25519=false）：保持既有 0-RTT 能力。
    {
        LinkConfig server_config = ServerConfig(psk);
        server_config.enable_x25519 = false;
        server_config.allow_legacy_peer = true;
        Link server(server_config);
        BENTO_CHECK(server.Open(&error));
        LinkConfig client_config = ClientConfig(server.local_port(), psk);
        client_config.enable_x25519 = false;
        client_config.allow_legacy_peer = true;
        Link client(client_config);
        BENTO_CHECK(client.Open(&error));
        bool server_connected = false;
        bool client_connected = false;
        server.on_connected = [&server_connected]() { server_connected = true; };
        client.on_connected = [&client_connected]() { client_connected = true; };
        std::vector<std::string> datagrams;
        server.on_datagram = [&datagrams](std::uint8_t channel, const std::uint8_t* data,
                                          std::size_t bytes) {
            BENTO_CHECK_EQ(channel, 1U);
            datagrams.emplace_back(reinterpret_cast<const char*>(data), bytes);
        };
        client.Connect();
        BENTO_CHECK(bento_test::PumpUntil(
            client, server, [&]() { return server_connected && client_connected; }, 5000));
        BENTO_CHECK_EQ(client.secure_stats().psk_fallbacks, 1U);
        BENTO_CHECK_EQ(server.secure_stats().psk_fallbacks, 1U);
        const std::vector<std::uint8_t> ticket = client.ExportSessionTicket();
        BENTO_CHECK_EQ(ticket.size(), kSessionTicketBytes);

        Link resumed(ClientConfig(server.local_port(), psk));
        BENTO_CHECK(resumed.Open(&error));
        BENTO_CHECK(resumed.Resume(ticket));
        BENTO_CHECK(resumed.resumed());
        BENTO_CHECK(resumed.session_established());
        const char kMessage[] = "psk-0rtt";
        BENTO_CHECK(resumed.SendDatagram(1, kMessage, sizeof(kMessage) - 1));
        BENTO_CHECK(bento_test::PumpUntil(
            resumed, server, [&]() { return !datagrams.empty(); }, 3000));
        BENTO_CHECK_EQ(datagrams.size(), 1U);
        BENTO_CHECK_EQ(datagrams[0], "psk-0rtt");
        resumed.Close();
        client.Close();
        server.Close();
    }
}

// ---------------------------------------------------------------------------
// 随机与吞吐
// ---------------------------------------------------------------------------

void TestRandomizedKdf() {
    std::printf("[CASE] randomized: 256 rounds x25519 + kdf agreement\n");
    std::mt19937 rng(0x5EED1234U);
    const Psk psk = MakePsk(0xC1);
    bool ok = true;
    for (int round = 0; round < 256; ++round) {
        Key client_private{};
        Key server_private{};
        Key client_public{};
        Key server_public{};
        for (auto& byte : client_private) {
            byte = static_cast<std::uint8_t>(rng() & 0xFFU);
        }
        for (auto& byte : server_private) {
            byte = static_cast<std::uint8_t>(rng() & 0xFFU);
        }
        Key client_shared{};
        Key server_shared{};
        ok = ok && X25519PublicKeyFromPrivate(client_private.data(), client_public.data());
        ok = ok && X25519PublicKeyFromPrivate(server_private.data(), server_public.data());
        ok = ok && X25519ComputeSharedSecret(client_private.data(), server_public.data(),
                                             client_shared.data());
        ok = ok && X25519ComputeSharedSecret(server_private.data(), client_public.data(),
                                             server_shared.data());
        ok = ok && client_shared == server_shared;

        std::uint8_t client_nonce[kHandshakeNonceBytes] = {};
        std::uint8_t server_nonce[kHandshakeNonceBytes] = {};
        FillBytes(client_nonce, sizeof(client_nonce),
                  static_cast<std::uint8_t>(round * 13));
        FillBytes(server_nonce, sizeof(server_nonce),
                  static_cast<std::uint8_t>(round * 29 + 1));
        std::uint8_t client_key[32] = {};
        std::uint8_t server_key[32] = {};
        DeriveSessionKeyWithX25519(psk.data(), client_shared.data(), client_nonce,
                                   server_nonce, SecureDirection::kClientToServer,
                                   client_key);
        DeriveSessionKeyWithX25519(psk.data(), server_shared.data(), client_nonce,
                                   server_nonce, SecureDirection::kClientToServer,
                                   server_key);
        ok = ok && std::memcmp(client_key, server_key, 32) == 0;
    }
    BENTO_CHECK(ok);
}

void TestKdfThroughput() {
    std::printf("[CASE] kdf throughput\n");
    const Psk psk = MakePsk(0xD1);
    std::uint8_t shared[kX25519KeyBytes] = {};
    FillBytes(shared, sizeof(shared), 0xE1);
    std::uint8_t client_nonce[kHandshakeNonceBytes] = {};
    std::uint8_t server_nonce[kHandshakeNonceBytes] = {};
    FillBytes(client_nonce, sizeof(client_nonce), 0x33);
    FillBytes(server_nonce, sizeof(server_nonce), 0x55);
    std::uint8_t out[kChaCha20KeyBytes] = {};
    constexpr int kIterations = 20000;

    const auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < kIterations; ++i) {
        DeriveSessionKeyWithX25519(psk.data(), shared, client_nonce, server_nonce,
                                   SecureDirection::kClientToServer, out);
    }
    const auto end = std::chrono::steady_clock::now();
    const double seconds = std::chrono::duration<double>(end - start).count();
    const double ops_per_second = seconds > 0.0 ? kIterations / seconds : 0.0;
    std::printf("[INFO] x25519 kdf: %.0f ops/s (%d iterations, %.3fs)\n", ops_per_second,
                kIterations, seconds);
    BENTO_CHECK(ops_per_second > 1000.0);
}

}  // namespace

int main() {
    TestHelloBodyCompatibility();
    TestX25519Kdf();
    TestForwardSecrecyAfterPskCompromise();
    TestSecureZero();
    TestLowOrderPublicKeyRejected();
    TestLinkX25519Handshake();
    TestLegacyServerFallback();
    TestLegacyClientFallback();
    TestTamperedPublicKey();
    TestLowOrderPeerKeyRejectedByServer();
    TestZeroRttPolicy();
    TestRandomizedKdf();
    TestKdfThroughput();
    return bento_test::Finish("test_x25519_handshake");
}
