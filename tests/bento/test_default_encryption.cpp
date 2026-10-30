/**
 * @file test_default_encryption.cpp
 * @brief 默认加密（准 P0）：默认值、无 PSK 拒绝、显式明文、旧端降级拒绝、加密收发。
 * @details 对应 backlog §3「Bento 全链路无加密」的验收：
 *          - 默认值：`LinkConfig` / `PeerHubConfig` 的 require_encryption=true、
 *            allow_legacy_peer=false，以及 SecurityError 稳定错误串。
 *          - 默认拒绝：无 PSK 时 Open/Connect 返回 kEncryptionRequired。
 *          - 显式明文：require_encryption=false 时保留旧行为（端到端可收发）。
 *          - 默认加密：只配 PSK、其余默认，Link 与 PeerHub 都能完成 X25519
 *            握手并双向收发，且 psk_fallbacks==0（未发生静默降级）。
 *          - 旧端拒绝：一端只有纯 PSK（无 X25519 公钥）时，默认策略在两个
 *            方向都拒绝；显式 allow_legacy_peer=true 才回退并计数。
 * @author go2cpp contributors
 * @date 2026-10-30
 * @copyright MIT License
 */

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstdint>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

#include "go2cpp/bento/net/link.hpp"
#include "go2cpp/bento/net/peer_hub.hpp"
#include "test_util.hpp"

namespace {

using go2cpp::bento::Link;
using go2cpp::bento::LinkConfig;
using go2cpp::bento::NowMs;
using go2cpp::bento::PeerHub;
using go2cpp::bento::PeerHubConfig;
using go2cpp::bento::SecurityError;
using go2cpp::bento::SecurityErrorMessage;
using go2cpp::bento::SecurityErrorName;

std::vector<std::uint8_t> MakePsk(std::uint8_t seed) {
    std::vector<std::uint8_t> key(32, 0);
    for (std::size_t i = 0; i < key.size(); ++i) {
        key[i] = static_cast<std::uint8_t>((seed + i * 7U) & 0xFFU);
    }
    return key;
}

LinkConfig MakeListenerConfig() {
    LinkConfig config;
    config.bind_address = "127.0.0.1";
    config.bind_port = 0;
    config.heartbeat_interval_ms = 50;
    config.peer_timeout_ms = 3000;
    config.handshake_timeout_ms = 2000;
    config.hello_initial_pto_ms = 100;
    return config;
}

LinkConfig MakeClientConfig(std::uint16_t port) {
    LinkConfig config = MakeListenerConfig();
    config.peer_address = "127.0.0.1";
    config.peer_port = port;
    config.handshake_timeout_ms = 2000;
    config.random_seed = 0x0DEFACEDU;
    return config;
}

PeerHubConfig MakeHubConfig() {
    PeerHubConfig config;
    config.bind_address = "127.0.0.1";
    config.bind_port = 0;
    config.heartbeat_interval_ms = 50;
    config.peer_timeout_ms = 3000;
    config.handshake_timeout_ms = 2000;
    config.hello_initial_pto_ms = 100;
    config.max_peers = 4;
    return config;
}

bool PumpHubs(const std::vector<PeerHub*>& hubs, const std::function<bool()>& done,
              int timeout_ms) {
    const std::uint64_t deadline = NowMs() + static_cast<std::uint64_t>(timeout_ms);
    while (NowMs() < deadline) {
        for (PeerHub* hub : hubs) {
            hub->RunOnce(1);
        }
        if (done()) {
            return true;
        }
    }
    return done();
}

/// 场景 1：默认值必须是「要求加密 + 不接受旧端」。
void TestDefaultsAreSecure() {
    std::printf("[CASE] defaults: require_encryption=true, allow_legacy_peer=false\n");
    LinkConfig link;
    BENTO_CHECK(link.require_encryption);
    BENTO_CHECK(!link.allow_legacy_peer);

    PeerHubConfig hub;
    BENTO_CHECK(hub.require_encryption);
    BENTO_CHECK(!hub.allow_legacy_peer);

    // 错误码是稳定的公开契约：名字与错误串都不能随意改。
    BENTO_CHECK(std::strcmp(SecurityErrorName(SecurityError::kEncryptionRequired),
                            "kEncryptionRequired") == 0);
    BENTO_CHECK(std::strcmp(SecurityErrorName(SecurityError::kLegacyPeerRejected),
                            "kLegacyPeerRejected") == 0);
    BENTO_CHECK(std::strstr(SecurityErrorMessage(SecurityError::kEncryptionRequired),
                            "kEncryptionRequired") != nullptr);
    BENTO_CHECK(std::strstr(SecurityErrorMessage(SecurityError::kLegacyPeerRejected),
                            "allow_legacy_peer") != nullptr);
}

/// 场景 2：默认策略下没有 PSK 的 Link 必须 fail fast，并给出明确错误码。
void TestLinkRejectsMissingPsk() {
    std::printf("[CASE] link: no PSK rejected with kEncryptionRequired\n");
    LinkConfig config = MakeListenerConfig();
    Link link(config);
    std::string error;
    BENTO_CHECK(!link.Open(&error));
    BENTO_CHECK(error.find("kEncryptionRequired") != std::string::npos);
    BENTO_CHECK(link.security_error() == SecurityError::kEncryptionRequired);
    BENTO_CHECK(!link.Connect());
    BENTO_CHECK(link.security_error() == SecurityError::kEncryptionRequired);

    // 同一对象在 Open 前补 PSK 仍然可以正常工作（SetPreSharedKey 先于 Open）。
    const std::vector<std::uint8_t> psk = MakePsk(0x11);
    BENTO_CHECK(link.SetPreSharedKey(psk.data(), psk.size()));
    BENTO_CHECK(link.secure());
    BENTO_CHECK(link.Open(&error));
    BENTO_CHECK(link.security_error() == SecurityError::kOk);
    link.Close();

    // 错误 PSK 长度必须报 kInvalidPsk，而不是退回明文。
    LinkConfig bad = MakeListenerConfig();
    bad.pre_shared_key = std::vector<std::uint8_t>(31, 0x22);
    Link bad_link(bad);
    BENTO_CHECK(!bad_link.Open(&error));
    BENTO_CHECK(error.find("kInvalidPsk") != std::string::npos);
    BENTO_CHECK(bad_link.security_error() == SecurityError::kInvalidPsk);
}

/// 场景 3：显式关闭后的明文链路仍能端到端收发（旧行为，文档标注不安全）。
void TestExplicitPlaintextAllowed() {
    std::printf("[CASE] link: explicit require_encryption=false allows plaintext\n");
    LinkConfig server_config = MakeListenerConfig();
    server_config.require_encryption = false;
    Link server(server_config);
    std::string error;
    BENTO_CHECK(server.Open(&error));
    BENTO_CHECK(server.insecure_plaintext());

    LinkConfig client_config = MakeClientConfig(server.local_port());
    client_config.require_encryption = false;
    Link client(client_config);
    BENTO_CHECK(client.Open(&error));
    BENTO_CHECK(client.insecure_plaintext());

    bool server_connected = false;
    bool client_connected = false;
    server.on_connected = [&server_connected]() { server_connected = true; };
    client.on_connected = [&client_connected]() { client_connected = true; };
    std::vector<std::string> received;
    server.on_stream = [&received](std::uint8_t, const std::uint8_t* data, std::size_t bytes) {
        received.emplace_back(reinterpret_cast<const char*>(data), bytes);
    };

    BENTO_CHECK(client.Connect());
    BENTO_CHECK(bento_test::PumpUntil(
        client, server, [&]() { return server_connected && client_connected; }, 3000));
    const char kMessage[] = "plaintext-explicit";
    BENTO_CHECK(client.SendStream(0, kMessage, sizeof(kMessage) - 1));
    BENTO_CHECK(bento_test::PumpUntil(
        client, server, [&]() { return !received.empty(); }, 3000));
    BENTO_CHECK_EQ(received.size(), 1U);
    BENTO_CHECK(received.front() == kMessage);
    BENTO_CHECK(!server.secure());
    BENTO_CHECK(!client.secure());
    client.Close();
    server.Close();
}

/// 场景 4：只配置 PSK、其余全默认 -> 默认加密链路正常收发（X25519 生效）。
void TestLinkDefaultEncryption() {
    std::printf("[CASE] link: default policy + PSK -> X25519 encrypted traffic\n");
    const std::vector<std::uint8_t> psk = MakePsk(0x31);
    LinkConfig server_config = MakeListenerConfig();
    server_config.pre_shared_key = psk;
    Link server(server_config);
    std::string error;
    BENTO_CHECK(server.Open(&error));

    LinkConfig client_config = MakeClientConfig(server.local_port());
    client_config.pre_shared_key = psk;
    Link client(client_config);
    BENTO_CHECK(client.Open(&error));

    bool server_connected = false;
    bool client_connected = false;
    server.on_connected = [&server_connected]() { server_connected = true; };
    client.on_connected = [&client_connected]() { client_connected = true; };
    std::vector<std::string> server_received;
    std::vector<std::string> client_received;
    server.on_stream = [&server_received](std::uint8_t, const std::uint8_t* data,
                                          std::size_t bytes) {
        server_received.emplace_back(reinterpret_cast<const char*>(data), bytes);
    };
    client.on_datagram = [&client_received](std::uint8_t, const std::uint8_t* data,
                                            std::size_t bytes) {
        client_received.emplace_back(reinterpret_cast<const char*>(data), bytes);
    };

    BENTO_CHECK(client.Connect());
    BENTO_CHECK(bento_test::PumpUntil(
        client, server, [&]() { return server_connected && client_connected; }, 5000));
    BENTO_CHECK(server.session_established());
    BENTO_CHECK(client.session_established());
    BENTO_CHECK_EQ(server.secure_stats().x25519_handshakes, 1U);
    BENTO_CHECK_EQ(client.secure_stats().x25519_handshakes, 1U);
    BENTO_CHECK_EQ(server.secure_stats().psk_fallbacks, 0U);
    BENTO_CHECK_EQ(client.secure_stats().psk_fallbacks, 0U);
    BENTO_CHECK(server.security_error() == SecurityError::kOk);
    BENTO_CHECK(client.security_error() == SecurityError::kOk);

    const char kPing[] = "encrypted-ping";
    const char kPong[] = "encrypted-pong";
    BENTO_CHECK(client.SendStream(0, kPing, sizeof(kPing) - 1));
    BENTO_CHECK(server.SendDatagram(1, kPong, sizeof(kPong) - 1));
    BENTO_CHECK(bento_test::PumpUntil(
        client, server,
        [&]() { return !server_received.empty() && !client_received.empty(); }, 5000));
    BENTO_CHECK_EQ(server_received.size(), 1U);
    if (!server_received.empty()) {
        BENTO_CHECK(server_received.front() == kPing);
    }
    BENTO_CHECK_EQ(client_received.size(), 1U);
    if (!client_received.empty()) {
        BENTO_CHECK(client_received.front() == kPong);
    }
    BENTO_CHECK(server.secure_stats().packets_opened > 0);
    BENTO_CHECK(client.secure_stats().packets_opened > 0);

    client.Close();
    server.Close();
}

/// 场景 5：旧监听端（无 X25519 公钥）+ 默认客户端 -> 客户端明确拒绝。
void TestLegacyServerRejected() {
    std::printf("[CASE] link: legacy listener (no x25519) rejected by default client\n");
    const std::vector<std::uint8_t> psk = MakePsk(0x41);
    LinkConfig server_config = MakeListenerConfig();
    server_config.pre_shared_key = psk;
    server_config.enable_x25519 = false;  // 显式纯 PSK（模拟旧端）
    server_config.allow_legacy_peer = true;
    Link server(server_config);
    std::string error;
    BENTO_CHECK(server.Open(&error));

    LinkConfig client_config = MakeClientConfig(server.local_port());
    client_config.pre_shared_key = psk;
    Link client(client_config);
    BENTO_CHECK(client.Open(&error));
    bool connected = false;
    client.on_connected = [&connected]() { connected = true; };
    BENTO_CHECK(client.Connect());
    BENTO_CHECK(bento_test::PumpUntil(
        client, server,
        [&]() { return client.security_error() == SecurityError::kLegacyPeerRejected; },
        3000));
    BENTO_CHECK(client.security_error() == SecurityError::kLegacyPeerRejected);
    BENTO_CHECK(!connected);
    BENTO_CHECK(!client.connected());
    BENTO_CHECK_EQ(client.secure_stats().legacy_peer_rejections, 1U);
    BENTO_CHECK_EQ(client.secure_stats().psk_fallbacks, 0U);
    // 监听端确实发出了不含公钥的旧格式 ACK（回退路径本身是通的）。
    BENTO_CHECK_EQ(server.secure_stats().psk_fallbacks, 1U);
    server.Close();
}

/// 场景 6：旧客户端（无 X25519 公钥）+ 默认监听端 -> 监听端拒绝且不回 ACK。
void TestLegacyClientRejected() {
    std::printf("[CASE] link: legacy client (no x25519) rejected by default server\n");
    const std::vector<std::uint8_t> psk = MakePsk(0x51);
    LinkConfig server_config = MakeListenerConfig();
    server_config.pre_shared_key = psk;
    Link server(server_config);
    std::string error;
    BENTO_CHECK(server.Open(&error));
    bool connected = false;
    server.on_connected = [&connected]() { connected = true; };

    LinkConfig client_config = MakeClientConfig(server.local_port());
    client_config.pre_shared_key = psk;
    client_config.enable_x25519 = false;  // 显式纯 PSK（模拟旧端）
    client_config.allow_legacy_peer = true;
    Link client(client_config);
    BENTO_CHECK(client.Open(&error));
    BENTO_CHECK(client.Connect());

    BENTO_CHECK(bento_test::PumpUntil(
        client, server,
        [&]() { return server.secure_stats().legacy_peer_rejections >= 1; }, 2000));
    BENTO_CHECK_EQ(server.secure_stats().legacy_peer_rejections, 1U);
    BENTO_CHECK(!connected);
    BENTO_CHECK(!server.connected());
    BENTO_CHECK_EQ(server.stats().handshakes, 0U);
    // 监听端拒绝时不回 ACK：客户端始终进不了 Connected。
    BENTO_CHECK(!client.connected());
    client.Close();
    server.Close();
}

/// 场景 7：显式 allow_legacy_peer=true 时才允许回退纯 PSK，并计数。
void TestLegacyPeerExplicitOptIn() {
    std::printf("[CASE] link: allow_legacy_peer=true opts into PSK fallback\n");
    const std::vector<std::uint8_t> psk = MakePsk(0x61);
    LinkConfig server_config = MakeListenerConfig();
    server_config.pre_shared_key = psk;
    server_config.enable_x25519 = false;
    server_config.allow_legacy_peer = true;
    Link server(server_config);
    std::string error;
    BENTO_CHECK(server.Open(&error));

    LinkConfig client_config = MakeClientConfig(server.local_port());
    client_config.pre_shared_key = psk;
    client_config.allow_legacy_peer = true;
    Link client(client_config);
    BENTO_CHECK(client.Open(&error));
    bool server_connected = false;
    bool client_connected = false;
    server.on_connected = [&server_connected]() { server_connected = true; };
    client.on_connected = [&client_connected]() { client_connected = true; };
    std::vector<std::string> received;
    server.on_datagram = [&received](std::uint8_t, const std::uint8_t* data,
                                     std::size_t bytes) {
        received.emplace_back(reinterpret_cast<const char*>(data), bytes);
    };

    BENTO_CHECK(client.Connect());
    BENTO_CHECK(bento_test::PumpUntil(
        client, server, [&]() { return server_connected && client_connected; }, 5000));
    BENTO_CHECK_EQ(client.secure_stats().psk_fallbacks, 1U);
    BENTO_CHECK_EQ(server.secure_stats().psk_fallbacks, 1U);
    BENTO_CHECK_EQ(client.secure_stats().x25519_handshakes, 0U);
    BENTO_CHECK_EQ(client.secure_stats().legacy_peer_rejections, 0U);
    const char kMessage[] = "legacy-psk-ok";
    BENTO_CHECK(client.SendDatagram(1, kMessage, sizeof(kMessage) - 1));
    BENTO_CHECK(bento_test::PumpUntil(
        client, server, [&]() { return !received.empty(); }, 3000));
    BENTO_CHECK_EQ(received.size(), 1U);
    client.Close();
    server.Close();
}

/// 场景 8：PeerHub 默认策略同样拒绝无 PSK 的 Hub。
void TestPeerHubRejectsMissingPsk() {
    std::printf("[CASE] peer_hub: no PSK rejected with kEncryptionRequired\n");
    PeerHub hub(MakeHubConfig());
    std::string error;
    BENTO_CHECK(!hub.Open(&error));
    BENTO_CHECK(error.find("kEncryptionRequired") != std::string::npos);
    BENTO_CHECK(hub.security_error() == SecurityError::kEncryptionRequired);
    BENTO_CHECK(!hub.is_open());

    PeerHubConfig config = MakeHubConfig();
    config.pre_shared_key = MakePsk(0x71);
    PeerHub secure_hub(config);
    BENTO_CHECK(secure_hub.Open(&error));
    BENTO_CHECK(secure_hub.secure());
    BENTO_CHECK(secure_hub.security_error() == SecurityError::kOk);
    secure_hub.Close();
}

/// 场景 9：PeerHub 默认加密链路正常收发（X25519 生效，无静默降级）。
void TestPeerHubDefaultEncryption() {
    std::printf("[CASE] peer_hub: default policy + PSK -> x25519 encrypted traffic\n");
    const std::vector<std::uint8_t> psk = MakePsk(0x81);
    PeerHubConfig server_config = MakeHubConfig();
    server_config.pre_shared_key = psk;
    PeerHub server(server_config);
    std::string error;
    BENTO_CHECK(server.Open(&error));

    PeerHubConfig client_config = MakeHubConfig();
    client_config.pre_shared_key = psk;
    PeerHub client(client_config);
    BENTO_CHECK(client.Open(&error));
    BENTO_CHECK(client.AddPeer(1, "127.0.0.1", server.local_port(), &error));

    std::vector<std::string> server_received;
    std::vector<std::string> client_received;
    server.on_stream = [&server_received](std::uint32_t, std::uint8_t, const std::uint8_t* data,
                                          std::size_t bytes) {
        server_received.emplace_back(reinterpret_cast<const char*>(data), bytes);
    };
    client.on_stream = [&client_received](std::uint32_t, std::uint8_t, const std::uint8_t* data,
                                          std::size_t bytes) {
        client_received.emplace_back(reinterpret_cast<const char*>(data), bytes);
    };
    BENTO_CHECK(PumpHubs(
        {&client, &server},
        [&]() { return client.peer_connected(1) && server.stats().peers_active == 1; }, 5000));
    BENTO_CHECK(client.peer_secure_ready(1));
    BENTO_CHECK(server.secure_stats().x25519_handshakes >= 1);
    BENTO_CHECK(client.secure_stats().x25519_handshakes >= 1);
    BENTO_CHECK_EQ(server.secure_stats().psk_fallbacks, 0U);
    BENTO_CHECK_EQ(client.secure_stats().psk_fallbacks, 0U);

    const char kPing[] = "hub-ping";
    const char kPong[] = "hub-pong";
    BENTO_CHECK(client.SendStream(1, 0, kPing, sizeof(kPing) - 1));
    BENTO_CHECK(server.SendStream(1, 1, kPong, sizeof(kPong) - 1));
    BENTO_CHECK(PumpHubs(
        {&client, &server},
        [&]() { return !server_received.empty() && !client_received.empty(); }, 5000));
    BENTO_CHECK_EQ(server_received.size(), 1U);
    if (!server_received.empty()) {
        BENTO_CHECK(server_received.front() == kPing);
    }
    BENTO_CHECK_EQ(client_received.size(), 1U);
    if (!client_received.empty()) {
        BENTO_CHECK(client_received.front() == kPong);
    }
    BENTO_CHECK(server.secure_stats().packets_opened > 0);
    BENTO_CHECK(client.secure_stats().packets_opened > 0);

    client.Close();
    server.Close();
}

/// 场景 10：PeerHub 监听端默认拒绝无 X25519 公钥的旧端 HELLO（不回 ACK）。
void TestPeerHubLegacyPeerRejected() {
    std::printf("[CASE] peer_hub: legacy hello without x25519 rejected by default\n");
    const std::vector<std::uint8_t> psk = MakePsk(0x91);
    PeerHubConfig config = MakeHubConfig();
    config.pre_shared_key = psk;
    PeerHub server(config);
    std::string error;
    BENTO_CHECK(server.Open(&error));

    go2cpp::bento::UdpSocket raw;
    BENTO_CHECK(raw.Open("127.0.0.1", 0, &error));
    sockaddr_storage address{};
    socklen_t address_len = 0;
    BENTO_CHECK(go2cpp::bento::UdpSocket::Resolve("127.0.0.1", server.local_port(), &address,
                                                 &address_len, &error));

    // 旧端 HELLO：36B 布局，不含 X25519 公钥。
    go2cpp::bento::HelloBody hello;
    hello.session = 0x12345678U;
    go2cpp::bento::SecureRandomBytes(hello.nonce, go2cpp::bento::kHandshakeNonceBytes);
    hello.timestamp_ms = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
    std::uint8_t body[go2cpp::bento::kLegacyHelloBodyBytes] = {};
    BENTO_CHECK(go2cpp::bento::EncodeHelloBody(hello, body, sizeof(body)));
    std::vector<std::uint8_t> packet;
    BENTO_CHECK(go2cpp::bento::SealHandshakeFrame(
        psk.data(), go2cpp::bento::SecureDirection::kClientToServer,
        static_cast<std::uint8_t>(go2cpp::bento::FrameType::kHello), hello.session, 0x0A0B0C0DU,
        1, body, sizeof(body), &packet));
    BENTO_CHECK(raw.SendTo(address, address_len, packet.data(), packet.size()));
    BENTO_CHECK(PumpHubs(
        {&server},
        [&]() { return server.secure_stats().legacy_peer_rejections >= 1; }, 1000));
    BENTO_CHECK_EQ(server.secure_stats().legacy_peer_rejections, 1U);
    BENTO_CHECK_EQ(server.stats().handshakes, 0U);
    BENTO_CHECK(server.PeerIds().empty());
}

/// 场景 11：PeerHub 显式明文模式仍可用（迁移期旧行为）。
void TestPeerHubExplicitPlaintext() {
    std::printf("[CASE] peer_hub: explicit require_encryption=false allows plaintext\n");
    PeerHubConfig server_config = MakeHubConfig();
    server_config.require_encryption = false;
    PeerHub server(server_config);
    std::string error;
    BENTO_CHECK(server.Open(&error));

    PeerHubConfig client_config = MakeHubConfig();
    client_config.require_encryption = false;
    PeerHub client(client_config);
    BENTO_CHECK(client.Open(&error));
    BENTO_CHECK(client.AddPeer(1, "127.0.0.1", server.local_port(), &error));
    std::vector<std::string> received;
    server.on_stream = [&received](std::uint32_t, std::uint8_t, const std::uint8_t* data,
                                   std::size_t bytes) {
        received.emplace_back(reinterpret_cast<const char*>(data), bytes);
    };
    BENTO_CHECK(PumpHubs(
        {&client, &server},
        [&]() { return client.peer_connected(1) && server.stats().peers_active == 1; }, 5000));
    BENTO_CHECK(!client.peer_secure(1));
    const char kMessage[] = "hub-plaintext";
    BENTO_CHECK(client.SendStream(1, 0, kMessage, sizeof(kMessage) - 1));
    BENTO_CHECK(PumpHubs({&client, &server}, [&]() { return !received.empty(); }, 3000));
    BENTO_CHECK_EQ(received.size(), 1U);
    if (!received.empty()) {
        BENTO_CHECK(received.front() == kMessage);
    }
    client.Close();
    server.Close();
}

}  // namespace

int main() {
    std::printf("== Bento default encryption (quasi-P0) ==\n");
    TestDefaultsAreSecure();
    TestLinkRejectsMissingPsk();
    TestExplicitPlaintextAllowed();
    TestLinkDefaultEncryption();
    TestLegacyServerRejected();
    TestLegacyClientRejected();
    TestLegacyPeerExplicitOptIn();
    TestPeerHubRejectsMissingPsk();
    TestPeerHubDefaultEncryption();
    TestPeerHubLegacyPeerRejected();
    TestPeerHubExplicitPlaintext();
    return bento_test::Finish("bento_default_encryption");
}
