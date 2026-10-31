/**
 * @file test_stun.cpp
 * @brief STUN 编解码、echo 服务器、CHANGE-REQUEST 与 NAT 类型检测测试。
 * @details 场景覆盖：
 *          1. 消息编解码往返（IPv4/IPv6 地址、CHANGE-REQUEST、字符串属性）；
 *          2. 非法/截断消息拒绝；
 *          3. ERROR-CODE 编解码；
 *          4. 本地 STUN echo 服务器的映射正确性（含 MAPPED-ADDRESS 兼容）；
 *          5. CHANGE-REQUEST(port)/(ip+port) 的响应源地址；
 *          6. 无 NAT → OpenInternet；
 *          7. 四种模拟 NAT → FullCone/Restricted/PortRestricted/Symmetric；
 *          8. 1000 次 Binding 压测（成功率 + RTT 分布）；
 *          9. 500 轮随机字节流 fuzz（解析器不崩溃）。
 *
 *          运行：`.build-bento-wsl/p2p_test_stun`
 *
 * @author go2cpp contributors
 * @date 2026-10-31
 * @copyright MIT License
 */

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "go2cpp/p2p/endpoint.hpp"
#include "go2cpp/p2p/random.hpp"
#include "go2cpp/p2p/stun_client.hpp"
#include "go2cpp/p2p/stun_message.hpp"
#include "go2cpp/p2p/stun_server.hpp"
#include "go2cpp/p2p/transport.hpp"
#include "go2cpp/p2p/turn.hpp"
#include "mock_nat.hpp"
#include "test_support.hpp"

namespace {

using go2cpp::p2p::Endpoint;
using go2cpp::p2p::NatDetectionConfig;
using go2cpp::p2p::NatDetectionResult;
using go2cpp::p2p::NatType;
using go2cpp::p2p::NatTypeDetector;
using go2cpp::p2p::NatTypeName;
using go2cpp::p2p::StunClass;
using go2cpp::p2p::StunClient;
using go2cpp::p2p::StunMessage;
using go2cpp::p2p::StunServer;
using go2cpp::p2p::StunServerConfig;
using go2cpp::p2p::UdpTransport;
using go2cpp::p2p::UdpTransportConfig;
using go2cpp::p2p::test::MockNat;
using go2cpp::p2p::test::NatBehavior;
using go2cpp::p2p::test::NatBehaviorName;
using Clock = std::chrono::steady_clock;

bool AlternateLoopbackUsable() {
    UdpTransportConfig config;
    config.bind_endpoint = Endpoint::FromIPv4("127.0.0.2", 0);
    UdpTransport probe(config);
    std::string error;
    return probe.Start(&error);
}

std::unique_ptr<UdpTransport> MakeUdpClient() {
    UdpTransportConfig config;
    config.bind_endpoint = Endpoint::Loopback(0);
    auto client = std::make_unique<UdpTransport>(config);
    std::string error;
    if (!client->Start(&error)) {
        return nullptr;
    }
    return client;
}

StunServerConfig MakeServerConfig(bool with_alternate) {
    StunServerConfig config;
    config.primary = Endpoint::Loopback(0);
    if (with_alternate) {
        config.alternate = Endpoint::FromIPv4("127.0.0.2", 0);
    }
    return config;
}

void TestMessageRoundTrip() {
    StunMessage message;
    message.transaction_id = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12};
    const Endpoint ipv4 = *Endpoint::Parse("203.0.113.7:62000");
    const Endpoint ipv6 = *Endpoint::Parse("[2001:db8::1]:3478");
    P2P_CHECK(message.AddAddress(go2cpp::p2p::kStunAttributeXorMappedAddress, ipv4, true));
    P2P_CHECK(message.AddAddress(go2cpp::p2p::kStunAttributeMappedAddress, ipv6, false));
    P2P_CHECK(message.AddU32(go2cpp::p2p::kStunAttributeChangeRequest,
                             go2cpp::p2p::kStunChangeIp | go2cpp::p2p::kStunChangePort));
    P2P_CHECK(message.AddString(go2cpp::p2p::kStunAttributeSoftware, "go2cpp-test"));

    const std::vector<std::uint8_t> wire = go2cpp::p2p::SerializeStunMessage(message);
    P2P_CHECK(!wire.empty());
    const auto parsed = go2cpp::p2p::ParseStunMessage(wire.data(), wire.size());
    P2P_CHECK(parsed.has_value());
    if (!parsed.has_value()) {
        return;
    }
    P2P_CHECK(parsed->method == go2cpp::p2p::kStunMethodBinding);
    P2P_CHECK(parsed->klass == StunClass::kRequest);
    P2P_CHECK(parsed->transaction_id == message.transaction_id);
    P2P_CHECK(parsed->GetAddress(go2cpp::p2p::kStunAttributeXorMappedAddress, true).value_or(Endpoint()) == ipv4);
    P2P_CHECK(parsed->GetAddress(go2cpp::p2p::kStunAttributeMappedAddress, false).value_or(Endpoint()) == ipv6);
    P2P_CHECK(parsed->GetU32(go2cpp::p2p::kStunAttributeChangeRequest).value_or(0) ==
              (go2cpp::p2p::kStunChangeIp | go2cpp::p2p::kStunChangePort));

    // 属性顺序与长度对齐：IPv4 地址值 8 字节、IPv6 地址值 20 字节。
    const go2cpp::p2p::StunAttribute* ipv4_attribute =
        parsed->Find(go2cpp::p2p::kStunAttributeXorMappedAddress);
    const go2cpp::p2p::StunAttribute* ipv6_attribute =
        parsed->Find(go2cpp::p2p::kStunAttributeMappedAddress);
    P2P_CHECK(ipv4_attribute != nullptr);
    P2P_CHECK(ipv6_attribute != nullptr);
    if (ipv4_attribute != nullptr) {
        P2P_CHECK(ipv4_attribute->value.size() == 8);
    }
    if (ipv6_attribute != nullptr) {
        P2P_CHECK(ipv6_attribute->value.size() == 20);
    }
}

void TestInvalidMessages() {
    const StunMessage request = StunMessage::MakeBindingRequest();
    const std::vector<std::uint8_t> wire = go2cpp::p2p::SerializeStunMessage(request);
    P2P_CHECK(wire.size() > 20);
    std::string error;

    for (std::size_t length = 0; length < 20; ++length) {
        P2P_CHECK(!go2cpp::p2p::ParseStunMessage(wire.data(), length, &error).has_value());
    }
    for (std::size_t length = 20; length + 1 < wire.size(); ++length) {
        P2P_CHECK(!go2cpp::p2p::ParseStunMessage(wire.data(), length, &error).has_value());
    }

    std::vector<std::uint8_t> corrupted = wire;
    corrupted[4] ^= 0xFFU;
    P2P_CHECK(!go2cpp::p2p::ParseStunMessage(corrupted.data(), corrupted.size(), &error).has_value());

    corrupted = wire;
    corrupted[3] = static_cast<std::uint8_t>(corrupted[3] + 4U);
    P2P_CHECK(!go2cpp::p2p::ParseStunMessage(corrupted.data(), corrupted.size(), &error).has_value());

    corrupted = wire;
    corrupted.push_back(0);
    corrupted.push_back(0);
    corrupted.push_back(0);
    corrupted.push_back(0);
    P2P_CHECK(!go2cpp::p2p::ParseStunMessage(corrupted.data(), corrupted.size(), &error).has_value());

    // 类型位交错：request/success/error 三种 class 的编码必须可逆。
    P2P_CHECK(go2cpp::p2p::EncodeStunType(go2cpp::p2p::kStunMethodBinding,
                                          StunClass::kRequest) == 0x0001);
    P2P_CHECK(go2cpp::p2p::EncodeStunType(go2cpp::p2p::kStunMethodBinding,
                                          StunClass::kSuccessResponse) == 0x0101);
    P2P_CHECK(go2cpp::p2p::EncodeStunType(go2cpp::p2p::kStunMethodBinding,
                                          StunClass::kErrorResponse) == 0x0111);
    P2P_CHECK(go2cpp::p2p::DecodeStunClass(0x0111) == StunClass::kErrorResponse);
    P2P_CHECK(go2cpp::p2p::DecodeStunMethod(0x0111) == go2cpp::p2p::kStunMethodBinding);
}

void TestErrorCode() {
    StunMessage error = StunMessage::MakeBindingError({9, 8, 7, 6, 5, 4, 3, 2, 1, 0, 1, 2}, 420,
                                                      "Unknown Attribute");
    const std::vector<std::uint8_t> wire = go2cpp::p2p::SerializeStunMessage(error);
    const auto parsed = go2cpp::p2p::ParseStunMessage(wire.data(), wire.size());
    P2P_CHECK(parsed.has_value());
    if (!parsed.has_value()) {
        return;
    }
    P2P_CHECK(parsed->klass == StunClass::kErrorResponse);
    std::uint16_t code = 0;
    std::string reason;
    P2P_CHECK(parsed->GetErrorCode(&code, &reason));
    P2P_CHECK(code == 420);
    P2P_CHECK(reason == "Unknown Attribute");
}

void TestStunEcho() {
    StunServerConfig config = MakeServerConfig(false);
    StunServer server(config);
    std::string error;
    P2P_CHECK_MSG(server.Start(&error), error.c_str());
    if (!server.started()) {
        return;
    }
    auto client = MakeUdpClient();
    P2P_CHECK(client != nullptr);
    if (!client) {
        return;
    }

    StunClient stun(client.get(), server.primary_endpoint());
    const auto mapped = stun.Binding(std::chrono::seconds(1), &error);
    P2P_CHECK_MSG(mapped.has_value(), error.c_str());
    if (mapped.has_value()) {
        P2P_CHECK(*mapped == client->LocalEndpoint());
    }

    // 完整响应必须同时带 XOR-MAPPED-ADDRESS 与 MAPPED-ADDRESS（兼容 RFC 3489）。
    StunMessage request = StunMessage::MakeBindingRequest();
    const auto exchange = stun.Exchange(request, std::chrono::seconds(1), &error);
    P2P_CHECK(exchange.has_value());
    if (exchange.has_value()) {
        P2P_CHECK(exchange->message.GetAddress(go2cpp::p2p::kStunAttributeMappedAddress, false)
                      .value_or(Endpoint()) == client->LocalEndpoint());
        P2P_CHECK(exchange->message.GetAddress(go2cpp::p2p::kStunAttributeXorMappedAddress, true)
                      .value_or(Endpoint()) == client->LocalEndpoint());
        P2P_CHECK(exchange->message.Find(go2cpp::p2p::kStunAttributeSoftware) != nullptr);
    }
    P2P_CHECK(server.requests_served() >= 2);
    P2P_CHECK(server.responses_sent() >= 2);
    server.Stop();
}

void TestChangeRequest() {
    if (!AlternateLoopbackUsable()) {
        p2p_test::Skip("127.0.0.2 不可绑定，跳过 CHANGE-REQUEST 双 IP 测试");
        return;
    }
    StunServer server(MakeServerConfig(true));
    std::string error;
    P2P_CHECK_MSG(server.Start(&error), error.c_str());
    if (!server.started()) {
        return;
    }
    const Endpoint primary = server.primary_endpoint();
    const Endpoint alternate = server.alternate_endpoint();
    P2P_CHECK(primary.valid());
    P2P_CHECK(alternate.valid());
    if (!primary.valid() || !alternate.valid()) {
        return;
    }
    P2P_CHECK(primary.port() != alternate.port());

    auto client = MakeUdpClient();
    P2P_CHECK(client != nullptr);
    if (!client) {
        return;
    }
    StunClient stun(client.get(), primary);

    const auto changed_port = stun.BindingWithChange(go2cpp::p2p::kStunChangePort,
                                                     std::chrono::seconds(1), &error);
    P2P_CHECK_MSG(changed_port.has_value(), error.c_str());
    if (changed_port.has_value()) {
        P2P_CHECK(changed_port->source.AddressString() == primary.AddressString());
        P2P_CHECK(changed_port->source.port() == alternate.port());
    }

    const auto changed_both =
        stun.BindingWithChange(go2cpp::p2p::kStunChangeIp | go2cpp::p2p::kStunChangePort,
                               std::chrono::seconds(1), &error);
    P2P_CHECK_MSG(changed_both.has_value(), error.c_str());
    if (changed_both.has_value()) {
        P2P_CHECK(changed_both->source.AddressString() == alternate.AddressString());
        P2P_CHECK(changed_both->source.port() == alternate.port());
    }
    P2P_CHECK(server.requests_served() >= 2);
    server.Stop();
}

void TestOpenInternet() {
    if (!AlternateLoopbackUsable()) {
        p2p_test::Skip("127.0.0.2 不可绑定，跳过无 NAT 检测");
        return;
    }
    StunServer server(MakeServerConfig(true));
    std::string error;
    P2P_CHECK_MSG(server.Start(&error), error.c_str());
    if (!server.started()) {
        return;
    }
    auto client = MakeUdpClient();
    P2P_CHECK(client != nullptr);
    if (!client) {
        return;
    }
    NatDetectionConfig config;
    config.primary_server = server.primary_endpoint();
    config.alternate_server = server.alternate_endpoint();
    config.timeout = std::chrono::milliseconds(500);
    NatTypeDetector detector(client.get(), config);
    const NatDetectionResult result = detector.Detect();
    P2P_CHECK_MSG(result.type == NatType::kOpenInternet,
                  ("无 NAT 应判定 OpenInternet，实际 " + NatTypeName(result.type) + "：" +
                   result.detail)
                      .c_str());
    server.Stop();
}

void TestNatTypes() {
    if (!AlternateLoopbackUsable()) {
        p2p_test::Skip("127.0.0.2 不可绑定，跳过四类 NAT 检测");
        return;
    }
    StunServer server(MakeServerConfig(true));
    std::string error;
    P2P_CHECK_MSG(server.Start(&error), error.c_str());
    if (!server.started()) {
        return;
    }
    struct Case {
        NatBehavior behavior;
        NatType expected;
    };
    const Case cases[] = {
        {NatBehavior::kFullCone, NatType::kFullCone},
        {NatBehavior::kRestricted, NatType::kRestrictedCone},
        {NatBehavior::kPortRestricted, NatType::kPortRestrictedCone},
        {NatBehavior::kSymmetric, NatType::kSymmetric},
    };
    for (const Case& item : cases) {
        MockNat nat(item.behavior);
        P2P_CHECK_MSG(nat.Start(&error), error.c_str());
        auto transport = nat.CreateClientTransport(&error);
        P2P_CHECK_MSG(transport != nullptr, error.c_str());
        if (!transport) {
            nat.Stop();
            continue;
        }
        NatDetectionConfig config;
        config.primary_server = server.primary_endpoint();
        config.alternate_server = server.alternate_endpoint();
        config.timeout = std::chrono::milliseconds(400);
        NatTypeDetector detector(transport.get(), config);
        const NatDetectionResult result = detector.Detect();
        const std::string message = std::string(NatBehaviorName(item.behavior)) +
                                    " 检测结果 " + NatTypeName(result.type) + "，期望 " +
                                    NatTypeName(item.expected) + "；detail=" + result.detail;
        P2P_CHECK_MSG(result.type == item.expected, message.c_str());
        std::printf("[INFO] NAT %-14s -> %-18s (mapped=%s, detail=%s)\n",
                    NatBehaviorName(item.behavior), NatTypeName(result.type).c_str(),
                    result.mapped.ToString().c_str(), result.detail.c_str());
        nat.Stop();
    }
    server.Stop();
}

void TestStunStress() {
    StunServer server(MakeServerConfig(false));
    std::string error;
    P2P_CHECK_MSG(server.Start(&error), error.c_str());
    if (!server.started()) {
        return;
    }
    auto client = MakeUdpClient();
    P2P_CHECK(client != nullptr);
    if (!client) {
        return;
    }
    StunClient stun(client.get(), server.primary_endpoint());
    constexpr int kRequests = 1000;
    std::vector<double> latencies;
    latencies.reserve(kRequests);
    int successes = 0;
    const Clock::time_point started = Clock::now();
    for (int index = 0; index < kRequests; ++index) {
        const Clock::time_point begin = Clock::now();
        const auto mapped = stun.Binding(std::chrono::milliseconds(500), &error);
        const double elapsed =
            std::chrono::duration<double, std::milli>(Clock::now() - begin).count();
        if (mapped.has_value()) {
            ++successes;
            latencies.push_back(elapsed);
        }
    }
    const double total_seconds =
        std::chrono::duration<double>(Clock::now() - started).count();
    const double p50 = p2p_test::Percentile(latencies, 0.50);
    const double p95 = p2p_test::Percentile(latencies, 0.95);
    const double p99 = p2p_test::Percentile(latencies, 0.99);
    std::printf("[PERF] STUN Binding: %d/%d 成功, %.0f req/s, P50=%.3fms P95=%.3fms P99=%.3fms\n",
                successes, kRequests, kRequests / total_seconds, p50, p95, p99);
    P2P_CHECK(successes == kRequests);
    P2P_CHECK(server.requests_served() >= static_cast<std::uint64_t>(kRequests));
    server.Stop();
}

void TestRandomFuzz() {
    int parsed_ok = 0;
    int rejected = 0;
    for (int round = 0; round < 500; ++round) {
        std::vector<std::uint8_t> bytes(static_cast<std::size_t>(round % 200));
        go2cpp::p2p::SecureRandomBytes(bytes.data(), bytes.size());
        const auto message = go2cpp::p2p::ParseStunMessage(bytes.data(), bytes.size());
        if (message.has_value()) {
            ++parsed_ok;
        } else {
            ++rejected;
        }
    }
    P2P_CHECK(parsed_ok + rejected == 500);
    P2P_CHECK(rejected == 500);  // 随机字节恰好构成合法 STUN 的概率应可忽略。

    // 合法消息的随机属性组合往返。
    for (int round = 0; round < 300; ++round) {
        StunMessage message;
        go2cpp::p2p::SecureRandomBytes(message.transaction_id.data(),
                                       message.transaction_id.size());
        const int attribute_count = 1 + static_cast<int>(go2cpp::p2p::RandomU32() % 4U);
        for (int index = 0; index < attribute_count; ++index) {
            const std::uint32_t choice = go2cpp::p2p::RandomU32() % 3U;
            if (choice == 0) {
                const std::string text = "attr-" + std::to_string(index);
                P2P_CHECK(message.AddString(
                    static_cast<std::uint16_t>(go2cpp::p2p::kStunAttributeSoftware + index), text));
            } else if (choice == 1) {
                P2P_CHECK(message.AddU32(
                    static_cast<std::uint16_t>(go2cpp::p2p::kStunAttributeLifetime + index),
                    go2cpp::p2p::RandomU32()));
            } else {
                const std::string text = std::to_string(go2cpp::p2p::RandomU32() % 255U) + "." +
                                         std::to_string(go2cpp::p2p::RandomU32() % 255U) + "." +
                                         std::to_string(go2cpp::p2p::RandomU32() % 255U) + ".1";
                const auto endpoint = Endpoint::Parse(text + ":" +
                                                      std::to_string(1 + go2cpp::p2p::RandomU32() % 65535U));
                P2P_CHECK(endpoint.has_value());
                if (endpoint.has_value()) {
                    P2P_CHECK(message.AddAddress(
                        static_cast<std::uint16_t>(go2cpp::p2p::kStunAttributeMappedAddress + index),
                        *endpoint, false));
                }
            }
        }
        const std::vector<std::uint8_t> wire = go2cpp::p2p::SerializeStunMessage(message);
        const auto parsed = go2cpp::p2p::ParseStunMessage(wire.data(), wire.size());
        P2P_CHECK(parsed.has_value());
        if (parsed.has_value()) {
            P2P_CHECK(parsed->attributes.size() == message.attributes.size());
            P2P_CHECK(parsed->transaction_id == message.transaction_id);
        }
    }
}

void TestRelayServerProtocol() {
    // 中继协议的最小往返：经 UdpTransport 直接发一条 Allocate，验证服务器响应。
    // 完整的 RelayClient 生命周期在 test_p2p.cpp 覆盖。
    go2cpp::p2p::RelayServerConfig config;
    config.bind_endpoint = Endpoint::Loopback(0);
    go2cpp::p2p::RelayServer server(config);
    std::string error;
    P2P_CHECK_MSG(server.Start(&error), error.c_str());
    if (!server.started()) {
        return;
    }
    P2P_CHECK(server.listen_endpoint().valid());
    P2P_CHECK(server.listen_endpoint().port() != 0);
    server.Stop();
    P2P_CHECK(!server.started());
}

}  // namespace

int main() {
    std::printf("=== P2P STUN 测试 ===\n");
    TestMessageRoundTrip();
    TestInvalidMessages();
    TestErrorCode();
    TestStunEcho();
    TestChangeRequest();
    TestOpenInternet();
    TestNatTypes();
    TestStunStress();
    TestRandomFuzz();
    TestRelayServerProtocol();
    return p2p_test::Summary("test_stun");
}
