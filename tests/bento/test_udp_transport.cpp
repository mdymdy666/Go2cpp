/**
 * @file test_udp_transport.cpp
 * @brief `UdpTransport` 测试：回环 100 包、超时、大包/截断、连接模式、IPv6。
 * @author go2cpp contributors
 * @date 2026-10-28
 * @copyright MIT License
 *
 * @details 覆盖：
 *   - Bind / LocalEndpoint / 重复 Bind / Close 幂等与重新 Bind；
 *   - 单包、100 包、不同大小数据报的边界保留；
 *   - RecvFrom 超时、零超时非阻塞；
 *   - 16 KiB 大包（> 以太网 MTU）完整回环；
 *   - 接收缓冲不足时的截断检测（datagram_bytes 与 truncated）；
 *   - Connect 后的 Read/Write、PeerEndpoint、重复 Connect；
 *   - 未连接时 Read/Write 的错误、非法参数、超大报文 EMSGSIZE；
 *   - 多发送方来源地址区分；
 *   - ShutdownWrite 只关闭写方向、Close 后错误；
 *   - IPv6 ::1 回环（环境不支持时跳过）。
 */

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "go2cpp/bento/net/udp_transport.hpp"
#include "test_util.hpp"

namespace {

using Clock = std::chrono::steady_clock;
using std::chrono::milliseconds;
using go2cpp::bento::net::Code;
using go2cpp::bento::net::Endpoint;
using go2cpp::bento::net::Result;
using go2cpp::bento::net::UdpTransport;

constexpr std::size_t kIoBufferBytes = 65536;
constexpr int kTimeoutMs = 2000;

void PrintCase(const char* name) {
    std::printf("[CASE] %s\n", name);
}

std::uint64_t NextRandom(std::uint64_t* state) noexcept {
    std::uint64_t value = *state;
    value ^= value << 13;
    value ^= value >> 7;
    value ^= value << 17;
    *state = value;
    return value;
}

std::vector<std::uint8_t> MakePattern(std::size_t size, std::uint64_t seed) {
    std::vector<std::uint8_t> data(size);
    std::uint64_t state = seed | 1U;
    for (std::uint8_t& byte : data) {
        byte = static_cast<std::uint8_t>(NextRandom(&state) & 0xFFU);
    }
    return data;
}

long long ElapsedMs(Clock::time_point start) {
    return std::chrono::duration_cast<milliseconds>(Clock::now() - start).count();
}

bool BindLoopback(UdpTransport* transport, Endpoint* local, const char* host = "127.0.0.1") {
    const Result bind_result = transport->Bind({host, 0});
    if (!bind_result.ok()) {
        std::fprintf(stderr, "[FAIL] Bind(%s): %s (errno=%d)\n", host,
                     go2cpp::bento::net::CodeName(bind_result.code),
                     bind_result.system_error);
        return false;
    }
    const Result local_result = transport->LocalEndpoint(local);
    if (!local_result.ok()) {
        std::fprintf(stderr, "[FAIL] LocalEndpoint: errno=%d\n", local_result.system_error);
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// 生命周期与地址
// ---------------------------------------------------------------------------

void TestBindAndLifecycle() {
    PrintCase("udp: Bind/LocalEndpoint/idempotent Close");
    UdpTransport transport;
    Endpoint local;

    BENTO_CHECK(!transport.is_open());
    BENTO_CHECK(!transport.is_connected());
    BENTO_CHECK_EQ(transport.LocalEndpoint(&local).code, Code::kClosed);

    BENTO_CHECK(transport.Bind({"127.0.0.1", 0}).ok());
    BENTO_CHECK(transport.is_open());
    BENTO_CHECK(!transport.is_connected());
    BENTO_CHECK(transport.LocalEndpoint(&local).ok());
    BENTO_CHECK_EQ(local.host, std::string("127.0.0.1"));
    BENTO_CHECK(local.port != 0);

    const Result duplicate = transport.Bind({"127.0.0.1", 0});
    BENTO_CHECK_EQ(duplicate.code, Code::kError);
    BENTO_CHECK_EQ(duplicate.system_error, EALREADY);

    const Result peer = transport.PeerEndpoint(&local);
    BENTO_CHECK_EQ(peer.code, Code::kError);
    BENTO_CHECK_EQ(peer.system_error, ENOTCONN);

    BENTO_CHECK(transport.Close().ok());
    BENTO_CHECK(!transport.is_open());
    BENTO_CHECK(transport.Close().ok());
    BENTO_CHECK_EQ(transport.LocalEndpoint(&local).code, Code::kClosed);
    BENTO_CHECK(transport.Bind({"127.0.0.1", 0}).ok());
}

// ---------------------------------------------------------------------------
// 无连接模式
// ---------------------------------------------------------------------------

void TestSingleDatagram() {
    PrintCase("udp: send/receive one datagram with peer address");
    UdpTransport receiver;
    UdpTransport sender;
    Endpoint receiver_local;
    Endpoint sender_local;
    BENTO_CHECK(BindLoopback(&receiver, &receiver_local));
    BENTO_CHECK(BindLoopback(&sender, &sender_local));

    const std::vector<std::uint8_t> payload = MakePattern(1024, 0x12345678U);
    const Result sent = sender.SendTo(receiver_local, payload.data(), payload.size(),
                                      milliseconds(kTimeoutMs));
    BENTO_CHECK(sent.ok());
    BENTO_CHECK_EQ(sent.bytes, payload.size());

    std::vector<std::uint8_t> buffer(kIoBufferBytes);
    Endpoint peer;
    UdpTransport::RecvInfo info;
    const Result received = receiver.RecvFrom(buffer.data(), buffer.size(),
                                             milliseconds(kTimeoutMs), &peer, &info);
    BENTO_CHECK(received.ok());
    BENTO_CHECK_EQ(received.bytes, payload.size());
    BENTO_CHECK_EQ(info.bytes, payload.size());
    BENTO_CHECK_EQ(info.datagram_bytes, payload.size());
    BENTO_CHECK(!info.truncated);
    BENTO_CHECK(std::equal(payload.begin(), payload.end(), buffer.begin()));
    BENTO_CHECK_EQ(peer.host, std::string("127.0.0.1"));
    BENTO_CHECK_EQ(peer.port, sender_local.port);
}

void TestHundredDatagrams() {
    PrintCase("udp: loopback 100 datagrams");
    UdpTransport receiver;
    UdpTransport sender;
    Endpoint receiver_local;
    Endpoint sender_local;
    BENTO_CHECK(BindLoopback(&receiver, &receiver_local));
    BENTO_CHECK(BindLoopback(&sender, &sender_local));

    std::vector<std::uint8_t> buffer(kIoBufferBytes);
    bool all_ok = true;
    for (int sequence = 0; sequence < 100; ++sequence) {
        std::vector<std::uint8_t> payload(64);
        payload[0] = static_cast<std::uint8_t>(sequence);
        payload[1] = static_cast<std::uint8_t>(sequence ^ 0x5A);
        for (std::size_t i = 2; i < payload.size(); ++i) {
            payload[i] = static_cast<std::uint8_t>((sequence + static_cast<int>(i)) & 0xFF);
        }

        const Result sent = sender.SendTo(receiver_local, payload.data(), payload.size(),
                                          milliseconds(kTimeoutMs));
        Endpoint peer;
        UdpTransport::RecvInfo info;
        const Result received = receiver.RecvFrom(buffer.data(), buffer.size(),
                                                 milliseconds(kTimeoutMs), &peer, &info);
        if (!sent.ok() || !received.ok() || received.bytes != payload.size() ||
            info.datagram_bytes != payload.size() || info.truncated ||
            peer.port != sender_local.port ||
            !std::equal(payload.begin(), payload.end(), buffer.begin())) {
            all_ok = false;
            std::fprintf(stderr, "[FAIL] datagram %d: sent=%s received=%s bytes=%zu/%zu\n",
                         sequence, go2cpp::bento::net::CodeName(sent.code),
                         go2cpp::bento::net::CodeName(received.code), received.bytes,
                         payload.size());
            break;
        }
    }
    BENTO_CHECK(all_ok);
}

void TestDatagramBoundaries() {
    PrintCase("udp: datagram boundaries are preserved");
    UdpTransport receiver;
    UdpTransport sender;
    Endpoint receiver_local;
    Endpoint sender_local;
    BENTO_CHECK(BindLoopback(&receiver, &receiver_local));
    BENTO_CHECK(BindLoopback(&sender, &sender_local));

    const std::array<std::size_t, 3> sizes = {1, 200, 7};
    std::vector<std::vector<std::uint8_t>> payloads;
    for (std::size_t index = 0; index < sizes.size(); ++index) {
        payloads.push_back(MakePattern(sizes[index], 0x1000U + index));
        const Result sent = sender.SendTo(receiver_local, payloads.back().data(),
                                          payloads.back().size(), milliseconds(kTimeoutMs));
        BENTO_CHECK(sent.ok());
        BENTO_CHECK_EQ(sent.bytes, payloads.back().size());
    }

    std::vector<std::uint8_t> buffer(kIoBufferBytes);
    bool boundaries_ok = true;
    for (const std::vector<std::uint8_t>& payload : payloads) {
        Endpoint peer;
        UdpTransport::RecvInfo info;
        const Result received = receiver.RecvFrom(buffer.data(), buffer.size(),
                                                 milliseconds(kTimeoutMs), &peer, &info);
        if (!received.ok() || received.bytes != payload.size() ||
            info.datagram_bytes != payload.size() || info.truncated ||
            !std::equal(payload.begin(), payload.end(), buffer.begin())) {
            boundaries_ok = false;
            break;
        }
    }
    BENTO_CHECK(boundaries_ok);
}

// ---------------------------------------------------------------------------
// 超时、大包与截断
// ---------------------------------------------------------------------------

void TestRecvTimeout() {
    PrintCase("udp: RecvFrom timeout is bounded by deadline");
    UdpTransport receiver;
    Endpoint local;
    BENTO_CHECK(BindLoopback(&receiver, &local));

    std::vector<std::uint8_t> buffer(64);
    Endpoint peer;
    UdpTransport::RecvInfo info;
    info.bytes = 12345;
    const Clock::time_point start = Clock::now();
    const Result received = receiver.RecvFrom(buffer.data(), buffer.size(), milliseconds(80),
                                             &peer, &info);
    const long long elapsed = ElapsedMs(start);
    BENTO_CHECK_EQ(received.code, Code::kTimeout);
    BENTO_CHECK_EQ(received.system_error, ETIMEDOUT);
    BENTO_CHECK(elapsed >= 50);
    BENTO_CHECK(elapsed < 2000);
    BENTO_CHECK_EQ(info.bytes, static_cast<std::size_t>(12345));
}

void TestNonBlockingTimeoutZero() {
    PrintCase("udp: RecvFrom with zero timeout is non-blocking");
    UdpTransport receiver;
    Endpoint local;
    BENTO_CHECK(BindLoopback(&receiver, &local));

    std::vector<std::uint8_t> buffer(64);
    const Clock::time_point start = Clock::now();
    const Result received = receiver.RecvFrom(buffer.data(), buffer.size(), milliseconds(0),
                                             nullptr, nullptr);
    const long long elapsed = ElapsedMs(start);
    BENTO_CHECK_EQ(received.code, Code::kTimeout);
    BENTO_CHECK(elapsed < 1000);
}

void TestLargeDatagram() {
    PrintCase("udp: 16 KiB datagram (> MTU) round trip");
    UdpTransport receiver;
    UdpTransport sender;
    Endpoint receiver_local;
    Endpoint sender_local;
    BENTO_CHECK(BindLoopback(&receiver, &receiver_local));
    BENTO_CHECK(BindLoopback(&sender, &sender_local));

    const std::vector<std::uint8_t> payload = MakePattern(16 * 1024, 0xA5A5A5A5U);
    const Result sent = sender.SendTo(receiver_local, payload.data(), payload.size(),
                                      milliseconds(kTimeoutMs));
    BENTO_CHECK(sent.ok());
    BENTO_CHECK_EQ(sent.bytes, payload.size());

    std::vector<std::uint8_t> buffer(kIoBufferBytes);
    UdpTransport::RecvInfo info;
    const Result received = receiver.RecvFrom(buffer.data(), buffer.size(),
                                             milliseconds(kTimeoutMs), nullptr, &info);
    BENTO_CHECK(received.ok());
    BENTO_CHECK_EQ(received.bytes, payload.size());
    BENTO_CHECK_EQ(info.datagram_bytes, payload.size());
    BENTO_CHECK(!info.truncated);
    BENTO_CHECK(std::equal(payload.begin(), payload.end(), buffer.begin()));
}

void TestTruncation() {
    PrintCase("udp: small receive buffer reports truncation");
    UdpTransport receiver;
    UdpTransport sender;
    Endpoint receiver_local;
    Endpoint sender_local;
    BENTO_CHECK(BindLoopback(&receiver, &receiver_local));
    BENTO_CHECK(BindLoopback(&sender, &sender_local));

    const std::vector<std::uint8_t> payload = MakePattern(4096, 0xDEADBEEFU);
    BENTO_CHECK(sender.SendTo(receiver_local, payload.data(), payload.size(),
                              milliseconds(kTimeoutMs))
                    .ok());

    std::vector<std::uint8_t> buffer(64);
    UdpTransport::RecvInfo info;
    const Result received = receiver.RecvFrom(buffer.data(), buffer.size(),
                                             milliseconds(kTimeoutMs), nullptr, &info);
    BENTO_CHECK(received.ok());
    BENTO_CHECK_EQ(received.bytes, buffer.size());
    BENTO_CHECK_EQ(info.bytes, buffer.size());
    BENTO_CHECK_EQ(info.datagram_bytes, payload.size());
    BENTO_CHECK(info.truncated);
    BENTO_CHECK(std::equal(buffer.begin(), buffer.end(), payload.begin()));

    // 零缓冲也能探测数据报长度；数据报被丢弃。
    BENTO_CHECK(sender.SendTo(receiver_local, payload.data(), payload.size(),
                              milliseconds(kTimeoutMs))
                    .ok());
    UdpTransport::RecvInfo discard_info;
    const Result discarded = receiver.RecvFrom(nullptr, 0, milliseconds(kTimeoutMs),
                                              nullptr, &discard_info);
    BENTO_CHECK(discarded.ok());
    BENTO_CHECK_EQ(discarded.bytes, static_cast<std::size_t>(0));
    BENTO_CHECK_EQ(discard_info.datagram_bytes, payload.size());
    BENTO_CHECK(discard_info.truncated);
}

void TestZeroLengthDatagram() {
    PrintCase("udp: zero-length datagram is preserved");
    UdpTransport receiver;
    UdpTransport sender;
    Endpoint receiver_local;
    Endpoint sender_local;
    BENTO_CHECK(BindLoopback(&receiver, &receiver_local));
    BENTO_CHECK(BindLoopback(&sender, &sender_local));

    BENTO_CHECK(sender.SendTo(receiver_local, nullptr, 0, milliseconds(kTimeoutMs)).ok());
    std::vector<std::uint8_t> buffer(8);
    UdpTransport::RecvInfo info;
    const Result received = receiver.RecvFrom(buffer.data(), buffer.size(),
                                             milliseconds(kTimeoutMs), nullptr, &info);
    BENTO_CHECK(received.ok());
    BENTO_CHECK_EQ(received.bytes, static_cast<std::size_t>(0));
    BENTO_CHECK_EQ(info.datagram_bytes, static_cast<std::size_t>(0));
    BENTO_CHECK(!info.truncated);
}

// ---------------------------------------------------------------------------
// 连接模式
// ---------------------------------------------------------------------------

void TestConnectMode() {
    PrintCase("udp: Connect then Read/Write and PeerEndpoint");
    UdpTransport server;
    UdpTransport client;
    Endpoint server_local;
    BENTO_CHECK(BindLoopback(&server, &server_local));

    const Result connected = client.Connect(server_local, milliseconds(kTimeoutMs));
    BENTO_CHECK(connected.ok());
    BENTO_CHECK(client.is_connected());
    BENTO_CHECK(client.is_open());

    Endpoint peer;
    BENTO_CHECK(client.PeerEndpoint(&peer).ok());
    BENTO_CHECK_EQ(peer.host, server_local.host);
    BENTO_CHECK_EQ(peer.port, server_local.port);

    const Result duplicate = client.Connect(server_local, milliseconds(kTimeoutMs));
    BENTO_CHECK_EQ(duplicate.code, Code::kError);
    BENTO_CHECK_EQ(duplicate.system_error, EALREADY);

    Endpoint client_local;
    BENTO_CHECK(client.LocalEndpoint(&client_local).ok());
    BENTO_CHECK(client_local.port != 0);

    const std::vector<std::uint8_t> request = MakePattern(128, 0xCAFEBABEU);
    const Result written = client.Write(request.data(), request.size(),
                                       milliseconds(kTimeoutMs));
    BENTO_CHECK(written.ok());
    BENTO_CHECK_EQ(written.bytes, request.size());

    std::vector<std::uint8_t> buffer(kIoBufferBytes);
    Endpoint client_peer;
    UdpTransport::RecvInfo info;
    const Result received = server.RecvFrom(buffer.data(), buffer.size(),
                                           milliseconds(kTimeoutMs), &client_peer, &info);
    BENTO_CHECK(received.ok());
    BENTO_CHECK_EQ(received.bytes, request.size());
    BENTO_CHECK(std::equal(request.begin(), request.end(), buffer.begin()));
    BENTO_CHECK_EQ(client_peer.port, client_local.port);

    const std::vector<std::uint8_t> reply = MakePattern(96, 0x0BADF00DU);
    BENTO_CHECK(server
                    .SendTo(client_peer, reply.data(), reply.size(),
                            milliseconds(kTimeoutMs))
                    .ok());
    std::vector<std::uint8_t> reply_buffer(256);
    const Result read_reply = client.Read(reply_buffer.data(), reply_buffer.size(),
                                         milliseconds(kTimeoutMs));
    BENTO_CHECK(read_reply.ok());
    BENTO_CHECK_EQ(read_reply.bytes, reply.size());
    BENTO_CHECK(std::equal(reply.begin(), reply.end(), reply_buffer.begin()));

    const Result extra = client.Read(reply_buffer.data(), reply_buffer.size(),
                                     milliseconds(20));
    BENTO_CHECK_EQ(extra.code, Code::kTimeout);
}

void TestUnconnectedErrors() {
    PrintCase("udp: unconnected Read/Write and invalid parameters");
    UdpTransport transport;
    std::vector<std::uint8_t> buffer(16);
    const std::vector<std::uint8_t> payload = {1, 2, 3, 4};

    BENTO_CHECK_EQ(transport.Read(buffer.data(), buffer.size(), milliseconds(10)).code,
                   Code::kClosed);
    BENTO_CHECK_EQ(transport.Write(payload.data(), payload.size(), milliseconds(10)).code,
                   Code::kClosed);
    BENTO_CHECK_EQ(transport.SendTo({"127.0.0.1", 9}, payload.data(), payload.size(),
                                    milliseconds(10))
                       .code,
                   Code::kClosed);

    Endpoint local;
    BENTO_CHECK(BindLoopback(&transport, &local));
    const Result read_unconnected =
        transport.Read(buffer.data(), buffer.size(), milliseconds(10));
    BENTO_CHECK_EQ(read_unconnected.code, Code::kError);
    BENTO_CHECK_EQ(read_unconnected.system_error, ENOTCONN);

    const Result write_unconnected =
        transport.Write(payload.data(), payload.size(), milliseconds(10));
    BENTO_CHECK_EQ(write_unconnected.code, Code::kError);
    BENTO_CHECK_EQ(write_unconnected.system_error, EDESTADDRREQ);

    BENTO_CHECK_EQ(transport.Connect({"", 0}, milliseconds(10)).code, Code::kError);
    BENTO_CHECK_EQ(transport.Connect({"127.0.0.1", 0}, milliseconds(10)).code,
                   Code::kError);
    BENTO_CHECK_EQ(transport.SendTo({"", 0}, payload.data(), payload.size(),
                                    milliseconds(10))
                       .code,
                   Code::kError);
    BENTO_CHECK_EQ(transport.SendTo(local, nullptr, payload.size(), milliseconds(10)).code,
                   Code::kError);
    BENTO_CHECK_EQ(transport.RecvFrom(nullptr, buffer.size(), milliseconds(10), nullptr)
                       .code,
                   Code::kError);
}

// ---------------------------------------------------------------------------
// 多对端、半关闭与错误注入
// ---------------------------------------------------------------------------

void TestMultiplePeers() {
    PrintCase("udp: multiple senders have distinct peer addresses");
    UdpTransport server;
    UdpTransport first;
    UdpTransport second;
    Endpoint server_local;
    Endpoint first_local;
    Endpoint second_local;
    BENTO_CHECK(BindLoopback(&server, &server_local));
    BENTO_CHECK(BindLoopback(&first, &first_local));
    BENTO_CHECK(BindLoopback(&second, &second_local));

    const std::vector<std::uint8_t> one = {'o', 'n', 'e'};
    const std::vector<std::uint8_t> two = {'t', 'w', 'o'};
    BENTO_CHECK(first.SendTo(server_local, one.data(), one.size(), milliseconds(kTimeoutMs))
                    .ok());
    BENTO_CHECK(second.SendTo(server_local, two.data(), two.size(), milliseconds(kTimeoutMs))
                    .ok());

    bool saw_one = false;
    bool saw_two = false;
    std::uint16_t first_peer_port = 0;
    std::uint16_t second_peer_port = 0;
    for (int index = 0; index < 2; ++index) {
        std::vector<std::uint8_t> buffer(16);
        Endpoint peer;
        const Result received = server.RecvFrom(buffer.data(), buffer.size(),
                                               milliseconds(kTimeoutMs), &peer, nullptr);
        BENTO_CHECK(received.ok());
        const std::string text(buffer.begin(), buffer.begin() + received.bytes);
        if (text == "one") {
            saw_one = true;
            first_peer_port = peer.port;
        } else if (text == "two") {
            saw_two = true;
            second_peer_port = peer.port;
        }
    }
    BENTO_CHECK(saw_one);
    BENTO_CHECK(saw_two);
    BENTO_CHECK_EQ(first_peer_port, first_local.port);
    BENTO_CHECK_EQ(second_peer_port, second_local.port);
    BENTO_CHECK(first_peer_port != second_peer_port);
}

void TestShutdownWriteAndClose() {
    PrintCase("udp: ShutdownWrite stops sending but keeps receiving");
    UdpTransport server;
    UdpTransport client;
    Endpoint server_local;
    BENTO_CHECK(BindLoopback(&server, &server_local));
    BENTO_CHECK(client.Connect(server_local, milliseconds(kTimeoutMs)).ok());

    Endpoint client_local;
    BENTO_CHECK(client.LocalEndpoint(&client_local).ok());

    BENTO_CHECK(client.ShutdownWrite().ok());
    BENTO_CHECK(client.ShutdownWrite().ok());

    const std::vector<std::uint8_t> payload = {'x', 'y'};
    const Result write_after_shutdown =
        client.Write(payload.data(), payload.size(), milliseconds(20));
    BENTO_CHECK_EQ(write_after_shutdown.code, Code::kClosed);
    BENTO_CHECK_EQ(write_after_shutdown.system_error, EPIPE);

    const Result send_after_shutdown =
        client.SendTo(server_local, payload.data(), payload.size(), milliseconds(20));
    BENTO_CHECK_EQ(send_after_shutdown.code, Code::kClosed);
    BENTO_CHECK_EQ(send_after_shutdown.system_error, EPIPE);

    const std::vector<std::uint8_t> reply = {'o', 'k'};
    BENTO_CHECK(server.SendTo(client_local, reply.data(), reply.size(),
                             milliseconds(kTimeoutMs))
                    .ok());
    std::vector<std::uint8_t> buffer(16);
    const Result read_after_shutdown =
        client.Read(buffer.data(), buffer.size(), milliseconds(kTimeoutMs));
    BENTO_CHECK(read_after_shutdown.ok());
    BENTO_CHECK_EQ(read_after_shutdown.bytes, reply.size());

    BENTO_CHECK(client.Close().ok());
    BENTO_CHECK(client.Close().ok());
    BENTO_CHECK_EQ(client.Read(buffer.data(), buffer.size(), milliseconds(10)).code,
                   Code::kClosed);
    BENTO_CHECK_EQ(client.Write(payload.data(), payload.size(), milliseconds(10)).code,
                   Code::kClosed);
}

void TestOversizedDatagram() {
    PrintCase("udp: oversized datagram is rejected with EMSGSIZE");
    UdpTransport receiver;
    UdpTransport sender;
    Endpoint receiver_local;
    Endpoint sender_local;
    BENTO_CHECK(BindLoopback(&receiver, &receiver_local));
    BENTO_CHECK(BindLoopback(&sender, &sender_local));

    // IPv4 UDP 最大载荷 65507；65508 必然超过单条数据报上限。
    const std::vector<std::uint8_t> payload(65508, 0x42);
    const Result sent = sender.SendTo(receiver_local, payload.data(), payload.size(),
                                      milliseconds(kTimeoutMs));
    BENTO_CHECK_EQ(sent.code, Code::kError);
    BENTO_CHECK_EQ(sent.system_error, EMSGSIZE);
}

// ---------------------------------------------------------------------------
// IPv6
// ---------------------------------------------------------------------------

void TestIpv6Loopback() {
    PrintCase("udp: IPv6 ::1 loopback");
    UdpTransport receiver;
    UdpTransport sender;
    Endpoint receiver_local;
    Endpoint sender_local;

    const Result receiver_bind = receiver.Bind({"::1", 0});
    if (!receiver_bind.ok()) {
        bento_test::Skip("IPv6 loopback unavailable");
        return;
    }
    BENTO_CHECK(receiver.LocalEndpoint(&receiver_local).ok());
    BENTO_CHECK(sender.Bind({"::1", 0}).ok());
    BENTO_CHECK(sender.LocalEndpoint(&sender_local).ok());

    const std::vector<std::uint8_t> payload = MakePattern(300, 0x600D600DU);
    BENTO_CHECK(sender.SendTo(receiver_local, payload.data(), payload.size(),
                              milliseconds(kTimeoutMs))
                    .ok());
    std::vector<std::uint8_t> buffer(1024);
    Endpoint peer;
    UdpTransport::RecvInfo info;
    const Result received = receiver.RecvFrom(buffer.data(), buffer.size(),
                                             milliseconds(kTimeoutMs), &peer, &info);
    BENTO_CHECK(received.ok());
    BENTO_CHECK_EQ(received.bytes, payload.size());
    BENTO_CHECK_EQ(info.datagram_bytes, payload.size());
    BENTO_CHECK(std::equal(payload.begin(), payload.end(), buffer.begin()));
    BENTO_CHECK_EQ(peer.port, sender_local.port);
    BENTO_CHECK(peer.host.find("::1") != std::string::npos);
}

}  // namespace

int main() {
    TestBindAndLifecycle();
    TestSingleDatagram();
    TestHundredDatagrams();
    TestDatagramBoundaries();
    TestRecvTimeout();
    TestNonBlockingTimeoutZero();
    TestLargeDatagram();
    TestTruncation();
    TestZeroLengthDatagram();
    TestConnectMode();
    TestUnconnectedErrors();
    TestMultiplePeers();
    TestShutdownWriteAndClose();
    TestOversizedDatagram();
    TestIpv6Loopback();
    return bento_test::Finish("udp_transport");
}
