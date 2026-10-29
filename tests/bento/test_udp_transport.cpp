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
 *   - IPv6 ::1 回环（环境不支持时跳过）；
 *   - 审计补充（2026-10-29）：`milliseconds::max()` 超时钳制、0 超时消费内核里
 *     已就绪的数据报、通配 Bind、connected socket 上 `SendTo` 其它对端、
 *     4 线程并发 `SendTo` 的数据报边界、Bind/Close 与失败 Bind 的 fd 泄漏、
 *     connected `Read` 的静默截断（文档已声明）。
 */

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <string>
#include <thread>
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
    // 头文件写明「失败时内容不保证被修改」，所以不断言 info 保持旧值；只确认
    // 失败结果没有把旧数据当成有效字节数返回。
    BENTO_CHECK_EQ(received.bytes, static_cast<std::size_t>(0));
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
    // 实测约 0.4us/次；50ms 上界足以吸收调度抖动，又能抓住「0 超时其实阻塞了」。
    BENTO_CHECK(elapsed < 50);
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
        // 只有「环境没有 IPv6」才允许 SKIP；其它 errno 是真实回归，不能逃逸成
        // SKIP（否则 CTest 会把整份测试标成 Skipped，掩盖 14 个用例的结果）。
        const bool unsupported = receiver_bind.system_error == EAFNOSUPPORT ||
                                 receiver_bind.system_error == EADDRNOTAVAIL ||
                                 receiver_bind.system_error == EPROTONOSUPPORT;
        BENTO_CHECK(unsupported);
        if (unsupported) {
            bento_test::Skip("IPv6 loopback unavailable");
        }
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

// ---------------------------------------------------------------------------
// 审计补充（2026-10-29）：超时钳制、非阻塞就绪、通配绑定、并发发送、fd 泄漏
// ---------------------------------------------------------------------------

/// 统计本进程打开的 fd（/proc/self/fd）；opendir 自身占一个，扣掉。
int CountOpenFds() {
    DIR* dir = ::opendir("/proc/self/fd");
    if (dir == nullptr) {
        return -1;
    }
    int count = 0;
    while (const dirent* entry = ::readdir(dir)) {
        if (std::strcmp(entry->d_name, ".") != 0 && std::strcmp(entry->d_name, "..") != 0) {
            ++count;
        }
    }
    ::closedir(dir);
    return count - 1;
}

void TestTimeoutClamp() {
    PrintCase("udp: milliseconds::max() timeout is clamped and still receives");
    UdpTransport receiver;
    Endpoint local;
    BENTO_CHECK(BindLoopback(&receiver, &local));

    // 修复前：deadline 加法溢出（UB）→ deadline 变成过去时间 → 0ms 立即 kTimeout。
    std::thread delayed_sender([local]() {
        std::this_thread::sleep_for(milliseconds(30));
        UdpTransport sender;
        if (!sender.Bind({"127.0.0.1", 0}).ok()) {
            return;
        }
        const std::uint8_t payload[3] = {7, 8, 9};
        (void)sender.SendTo(local, payload, sizeof(payload), milliseconds(500));
    });
    std::vector<std::uint8_t> buffer(16);
    const Clock::time_point start = Clock::now();
    const Result received = receiver.RecvFrom(buffer.data(), buffer.size(),
                                              milliseconds::max(), nullptr, nullptr);
    const long long elapsed = ElapsedMs(start);
    delayed_sender.join();
    BENTO_CHECK(received.ok());
    BENTO_CHECK_EQ(received.bytes, static_cast<std::size_t>(3));
    BENTO_CHECK(elapsed >= 20);
    BENTO_CHECK(elapsed < 2000);
}

void TestNonBlockingReadyData() {
    PrintCase("udp: zero timeout consumes datagram already queued");
    UdpTransport receiver;
    UdpTransport sender;
    Endpoint receiver_local;
    Endpoint sender_local;
    BENTO_CHECK(BindLoopback(&receiver, &receiver_local));
    BENTO_CHECK(BindLoopback(&sender, &sender_local));

    const std::vector<std::uint8_t> payload = {1, 2, 3, 4, 5};
    BENTO_CHECK(sender
                    .SendTo(receiver_local, payload.data(), payload.size(),
                            milliseconds(kTimeoutMs))
                    .ok());
    std::vector<std::uint8_t> buffer(16);
    UdpTransport::RecvInfo info;
    const Result first =
        receiver.RecvFrom(buffer.data(), buffer.size(), milliseconds(0), nullptr, &info);
    BENTO_CHECK(first.ok());
    BENTO_CHECK_EQ(first.bytes, payload.size());
    BENTO_CHECK_EQ(info.datagram_bytes, payload.size());
    BENTO_CHECK(std::equal(payload.begin(), payload.end(), buffer.begin()));

    const Result second =
        receiver.RecvFrom(buffer.data(), buffer.size(), milliseconds(0), nullptr, nullptr);
    BENTO_CHECK_EQ(second.code, Code::kTimeout);
}

void TestWildcardBind() {
    PrintCase("udp: wildcard Bind receives IPv4 datagram and replies to peer");
    UdpTransport server;
    BENTO_CHECK(server.Bind({"", 0}).ok());
    Endpoint server_local;
    BENTO_CHECK(server.LocalEndpoint(&server_local).ok());
    BENTO_CHECK(server_local.port != 0);
    BENTO_CHECK(server_local.host == "0.0.0.0" || server_local.host == "::");

    UdpTransport client;
    Endpoint client_local;
    BENTO_CHECK(BindLoopback(&client, &client_local));

    const std::vector<std::uint8_t> payload = {'w', 'i', 'l', 'd'};
    BENTO_CHECK(client
                    .SendTo({"127.0.0.1", server_local.port}, payload.data(),
                            payload.size(), milliseconds(kTimeoutMs))
                    .ok());
    std::vector<std::uint8_t> buffer(64);
    Endpoint peer;
    UdpTransport::RecvInfo info;
    const Result received = server.RecvFrom(buffer.data(), buffer.size(),
                                            milliseconds(kTimeoutMs), &peer, &info);
    BENTO_CHECK(received.ok());
    BENTO_CHECK_EQ(received.bytes, payload.size());
    BENTO_CHECK_EQ(peer.port, client_local.port);
    BENTO_CHECK(std::equal(payload.begin(), payload.end(), buffer.begin()));

    // 回给来源地址：peer 可能是 127.0.0.1，也可能是 ::ffff:127.0.0.1。
    BENTO_CHECK(server
                    .SendTo(peer, payload.data(), payload.size(), milliseconds(kTimeoutMs))
                    .ok());
    BENTO_CHECK(client
                    .RecvFrom(buffer.data(), buffer.size(), milliseconds(kTimeoutMs), nullptr,
                              nullptr)
                    .ok());
}

void TestSendToOtherPeerOnConnectedSocket() {
    PrintCase("udp: connected socket can SendTo a different peer");
    UdpTransport first;
    UdpTransport second;
    UdpTransport client;
    Endpoint first_local;
    Endpoint second_local;
    Endpoint client_local;
    BENTO_CHECK(BindLoopback(&first, &first_local));
    BENTO_CHECK(BindLoopback(&second, &second_local));
    BENTO_CHECK(BindLoopback(&client, &client_local));
    BENTO_CHECK(client.Connect(first_local, milliseconds(kTimeoutMs)).ok());

    std::vector<std::uint8_t> buffer(32);
    const std::vector<std::uint8_t> direct = {'d'};
    BENTO_CHECK(client.Write(direct.data(), direct.size(), milliseconds(kTimeoutMs)).ok());
    BENTO_CHECK_EQ(first
                       .RecvFrom(buffer.data(), buffer.size(), milliseconds(kTimeoutMs), nullptr,
                                 nullptr)
                       .bytes,
                   direct.size());

    const std::vector<std::uint8_t> other = {'o', 't', 'h', 'e', 'r'};
    BENTO_CHECK(client
                    .SendTo(second_local, other.data(), other.size(), milliseconds(kTimeoutMs))
                    .ok());
    Endpoint peer;
    UdpTransport::RecvInfo info;
    const Result received = second.RecvFrom(buffer.data(), buffer.size(),
                                            milliseconds(kTimeoutMs), &peer, &info);
    BENTO_CHECK(received.ok());
    BENTO_CHECK_EQ(received.bytes, other.size());
    BENTO_CHECK_EQ(peer.port, client_local.port);
    BENTO_CHECK(std::equal(other.begin(), other.end(), buffer.begin()));
}

void TestConnectedReadTruncation() {
    PrintCase("udp: connected Read truncates silently (documented)");
    UdpTransport reader;
    UdpTransport writer;
    Endpoint reader_local;
    Endpoint writer_local;
    BENTO_CHECK(BindLoopback(&reader, &reader_local));
    BENTO_CHECK(BindLoopback(&writer, &writer_local));
    BENTO_CHECK(writer.Connect(reader_local, milliseconds(kTimeoutMs)).ok());
    BENTO_CHECK(reader.Connect(writer_local, milliseconds(kTimeoutMs)).ok());

    const std::vector<std::uint8_t> payload = MakePattern(4096, 0x4096U);
    BENTO_CHECK(writer.Write(payload.data(), payload.size(), milliseconds(kTimeoutMs)).ok());
    std::vector<std::uint8_t> small(64);
    const Result truncated = reader.Read(small.data(), small.size(), milliseconds(kTimeoutMs));
    BENTO_CHECK(truncated.ok());
    BENTO_CHECK_EQ(truncated.bytes, small.size());
    BENTO_CHECK(std::equal(small.begin(), small.end(), payload.begin()));
    // 截断的剩余部分被内核丢弃，UDP 没有流式补读。
    const Result extra = reader.Read(small.data(), small.size(), milliseconds(20));
    BENTO_CHECK_EQ(extra.code, Code::kTimeout);
}

void TestConcurrentSend() {
    PrintCase("udp: concurrent SendTo from four threads keeps boundaries");
    constexpr int kThreads = 4;
    constexpr int kPerThread = 250;
    UdpTransport server;
    UdpTransport sender;
    Endpoint server_local;
    Endpoint sender_local;
    BENTO_CHECK(BindLoopback(&server, &server_local));
    BENTO_CHECK(BindLoopback(&sender, &sender_local));

    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (int index = 0; index < kThreads; ++index) {
        threads.emplace_back([&sender, server_local, index]() {
            std::vector<std::uint8_t> payload(128, static_cast<std::uint8_t>(index + 1));
            for (int i = 0; i < kPerThread; ++i) {
                payload[0] = static_cast<std::uint8_t>(index + 1);
                payload[1] = static_cast<std::uint8_t>(i & 0xFF);
                (void)sender.SendTo(server_local, payload.data(), payload.size(),
                                    milliseconds(kTimeoutMs));
                if ((i % 8) == 7) {
                    std::this_thread::sleep_for(std::chrono::microseconds(200));
                }
            }
        });
    }

    std::vector<std::uint8_t> buffer(kIoBufferBytes);
    std::array<int, kThreads> received = {0, 0, 0, 0};
    int corrupt = 0;
    int total = 0;
    const Clock::time_point deadline = Clock::now() + milliseconds(5000);
    while (total < kThreads * kPerThread && Clock::now() < deadline) {
        Endpoint peer;
        UdpTransport::RecvInfo info;
        const Result got = server.RecvFrom(buffer.data(), buffer.size(), milliseconds(200),
                                           &peer, &info);
        if (!got.ok()) {
            continue;
        }
        const int tag = buffer[0];
        bool uniform = got.bytes == 128 && tag >= 1 && tag <= kThreads;
        for (std::size_t i = 2; uniform && i < got.bytes; ++i) {
            if (buffer[i] != static_cast<std::uint8_t>(tag)) {
                uniform = false;
            }
        }
        if (!uniform) {
            ++corrupt;
            continue;
        }
        ++received[static_cast<std::size_t>(tag - 1)];
        ++total;
    }
    for (std::thread& thread : threads) {
        thread.join();
    }
    BENTO_CHECK_EQ(corrupt, 0);
    BENTO_CHECK_EQ(total, kThreads * kPerThread);
    for (int index = 0; index < kThreads; ++index) {
        BENTO_CHECK_EQ(received[static_cast<std::size_t>(index)], kPerThread);
    }
}

void TestNoFdLeak() {
    PrintCase("udp: Bind/Close and failed Bind do not leak fds");
    const int before = CountOpenFds();
    BENTO_CHECK(before >= 0);
    bool all_ok = true;
    for (int i = 0; i < 200; ++i) {
        UdpTransport transport;
        if (!transport.Bind({"127.0.0.1", 0}).ok() || !transport.Close().ok()) {
            all_ok = false;
            break;
        }
    }
    BENTO_CHECK(all_ok);

    UdpTransport holder;
    BENTO_CHECK(holder.Bind({"127.0.0.1", 0}).ok());
    Endpoint held;
    BENTO_CHECK(holder.LocalEndpoint(&held).ok());
    bool all_failed = true;
    for (int i = 0; i < 50; ++i) {
        UdpTransport transport;
        if (transport.Bind({held.host, held.port}).ok()) {
            all_failed = false;
            break;
        }
    }
    BENTO_CHECK(all_failed);
    // holder 仍占着一个 fd；先关掉再比对基线。
    BENTO_CHECK(holder.Close().ok());
    BENTO_CHECK_EQ(CountOpenFds(), before);
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
    TestTimeoutClamp();
    TestNonBlockingReadyData();
    TestWildcardBind();
    TestSendToOtherPeerOnConnectedSocket();
    TestConnectedReadTruncation();
    TestConcurrentSend();
    TestNoFdLeak();
    return bento_test::Finish("udp_transport");
}
