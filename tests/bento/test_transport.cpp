/**
 * @file test_transport.cpp
 * @brief `ITransport` / `TcpTransport` / `MemoryTransport` / `Frame` 的端到端测试。
 * @author go2cpp contributors
 * @date 2026-10-28
 * @copyright MIT License
 *
 * @details 覆盖：
 *   - MemoryTransport：100 KiB 双向一致、半关闭、超时、幂等 Close、部分读写；
 *   - TcpTransport：127.0.0.1 回环 100 KiB 双向、accept/connect 超时、半关闭、
 *     Close、多连接隔离、连接拒绝、IPv6 回环（环境支持时）；
 *   - Frame：复用 MOD-10 的 20B 帧实现，覆盖正常/空负载、粘包、半包、
 *     非法 magic、版本/reserved、长度超限、校验失败。
 *
 * 断言只在主线程执行：`test_util.hpp` 的计数器不是原子变量，工作线程只负责
 * 搬运数据，结果交回主线程检查。
 */

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "go2cpp/bento/net/frame.hpp"
#include "go2cpp/bento/net/memory_transport.hpp"
#include "go2cpp/bento/net/tcp_transport.hpp"
#include "test_util.hpp"

namespace {

using go2cpp::bento::net::Code;
using go2cpp::bento::net::Endpoint;
using go2cpp::bento::net::MemoryTransport;
using go2cpp::bento::net::Result;
using go2cpp::bento::net::ComputeFrameChecksum;
using go2cpp::bento::net::Decode;
using go2cpp::bento::net::Encode;
using go2cpp::bento::net::Frame;
using go2cpp::bento::net::FrameDecoder;
using go2cpp::bento::net::FrameStatus;
using go2cpp::bento::net::kFrameMagic;
using go2cpp::bento::net::kFrameMaxPayload;
using go2cpp::bento::net::kFrameVersion;
using go2cpp::bento::net::kGenericFrameHeaderBytes;
using go2cpp::bento::net::TcpTransport;

using Clock = std::chrono::steady_clock;
using std::chrono::milliseconds;

constexpr std::size_t kLargeSize = 100 * 1024;
constexpr int kIoTimeoutMs = 5000;

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
    for (std::size_t i = 0; i < size; ++i) {
        data[i] = static_cast<std::uint8_t>(NextRandom(&state) & 0xFFU);
    }
    return data;
}

long long ElapsedMs(Clock::time_point start) {
    return std::chrono::duration_cast<milliseconds>(Clock::now() - start).count();
}

/// 建立一对回环 TCP 连接；失败时打印原因（断言由调用方负责）。
bool SetupTcpPair(TcpTransport* listener, TcpTransport* client, TcpTransport* server) {
    Result result = listener->Listen({"127.0.0.1", 0});
    if (!result.ok()) {
        std::fprintf(stderr, "[FAIL] listener.Listen: %s (errno=%d)\n",
                     go2cpp::bento::net::CodeName(result.code), result.system_error);
        return false;
    }

    Endpoint local;
    result = listener->LocalEndpoint(&local);
    if (!result.ok()) {
        std::fprintf(stderr, "[FAIL] listener.LocalEndpoint: errno=%d\n", result.system_error);
        return false;
    }

    result = client->Connect(local, milliseconds(2000));
    if (!result.ok()) {
        std::fprintf(stderr, "[FAIL] client.Connect(%s:%u): errno=%d\n", local.host.c_str(),
                     static_cast<unsigned>(local.port), result.system_error);
        return false;
    }

    result = listener->Accept(server, milliseconds(2000));
    if (!result.ok()) {
        std::fprintf(stderr, "[FAIL] listener.Accept: errno=%d\n", result.system_error);
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// MemoryTransport
// ---------------------------------------------------------------------------

void TestMemoryBidirectional100K() {
    PrintCase("memory: bidirectional 100 KiB");
    MemoryTransport::Pair pair = MemoryTransport::CreatePair();
    MemoryTransport& first = *pair.first;
    MemoryTransport& second = *pair.second;

    const std::vector<std::uint8_t> first_payload = MakePattern(kLargeSize, 0x12345678U);
    const std::vector<std::uint8_t> second_payload = MakePattern(kLargeSize, 0x9ABCDEF0U);

    Result first_write;
    Result second_write;
    std::thread first_thread([&] {
        first_write = first.WriteFull(first_payload.data(), first_payload.size(),
                                      milliseconds(kIoTimeoutMs));
    });
    std::thread second_thread([&] {
        second_write = second.WriteFull(second_payload.data(), second_payload.size(),
                                        milliseconds(kIoTimeoutMs));
    });

    std::vector<std::uint8_t> got_by_second(kLargeSize);
    std::vector<std::uint8_t> got_by_first(kLargeSize);
    const Result second_read =
        second.ReadFull(got_by_second.data(), got_by_second.size(), milliseconds(kIoTimeoutMs));
    const Result first_read =
        first.ReadFull(got_by_first.data(), got_by_first.size(), milliseconds(kIoTimeoutMs));

    first_thread.join();
    second_thread.join();

    BENTO_CHECK(first_write.ok());
    BENTO_CHECK(second_write.ok());
    BENTO_CHECK(first_write.bytes == first_payload.size());
    BENTO_CHECK(second_write.bytes == second_payload.size());
    BENTO_CHECK(second_read.ok());
    BENTO_CHECK(first_read.ok());
    BENTO_CHECK(got_by_second == first_payload);
    BENTO_CHECK(got_by_first == second_payload);
}

void TestMemoryHalfClose() {
    PrintCase("memory: half-close keeps the opposite direction alive");
    MemoryTransport::Pair pair = MemoryTransport::CreatePair();
    MemoryTransport& first = *pair.first;
    MemoryTransport& second = *pair.second;

    const std::vector<std::uint8_t> request = {'r', 'e', 'q', 'u', 'e', 's', 't'};
    Result result = first.WriteFull(request.data(), request.size(), milliseconds(1000));
    BENTO_CHECK(result.ok());
    BENTO_CHECK(first.ShutdownWrite().ok());
    BENTO_CHECK(first.ShutdownWrite().ok());  // 幂等
    BENTO_CHECK(first.Write(request.data(), request.size(), milliseconds(100)).code ==
                Code::kClosed);

    std::vector<std::uint8_t> buffer(request.size());
    result = second.ReadFull(buffer.data(), buffer.size(), milliseconds(1000));
    BENTO_CHECK(result.ok());
    BENTO_CHECK(buffer == request);
    result = second.Read(buffer.data(), buffer.size(), milliseconds(100));
    BENTO_CHECK(result.code == Code::kClosed);

    // 对端半关闭后，本端仍可写；响应必须完整送达。
    const std::vector<std::uint8_t> response = {'r', 'e', 's', 'p'};
    BENTO_CHECK(second.WriteFull(response.data(), response.size(), milliseconds(1000)).ok());
    BENTO_CHECK(second.ShutdownWrite().ok());

    std::vector<std::uint8_t> got(response.size());
    result = first.ReadFull(got.data(), got.size(), milliseconds(1000));
    BENTO_CHECK(result.ok());
    BENTO_CHECK(got == response);
    result = first.Read(got.data(), got.size(), milliseconds(100));
    BENTO_CHECK(result.code == Code::kClosed);
}

void TestMemoryTimeout() {
    PrintCase("memory: read timeout is respected and recoverable");
    MemoryTransport::Pair pair = MemoryTransport::CreatePair();
    MemoryTransport& first = *pair.first;
    MemoryTransport& second = *pair.second;

    std::vector<std::uint8_t> buffer(32);
    const Clock::time_point start = Clock::now();
    Result result = first.Read(buffer.data(), buffer.size(), milliseconds(120));
    const long long elapsed = ElapsedMs(start);
    BENTO_CHECK(result.code == Code::kTimeout);
    BENTO_CHECK(result.bytes == 0);
    BENTO_CHECK(elapsed >= 80);
    BENTO_CHECK(elapsed < 3000);

    const std::vector<std::uint8_t> late = MakePattern(64, 0x55AAU);
    BENTO_CHECK(second.Write(late.data(), late.size(), milliseconds(1000)).ok());
    std::vector<std::uint8_t> got(late.size());
    result = first.ReadFull(got.data(), got.size(), milliseconds(1000));
    BENTO_CHECK(result.ok());
    BENTO_CHECK(got == late);
}

void TestMemoryCloseIdempotent() {
    PrintCase("memory: idempotent close propagates EOF and rejects writes");
    MemoryTransport::Pair pair = MemoryTransport::CreatePair();
    MemoryTransport& first = *pair.first;
    MemoryTransport& second = *pair.second;

    const std::vector<std::uint8_t> before_close = {'a', 'b', 'c'};
    BENTO_CHECK(first.WriteFull(before_close.data(), before_close.size(), milliseconds(1000)).ok());
    BENTO_CHECK(first.Close().ok());
    BENTO_CHECK(first.Close().ok());
    BENTO_CHECK(first.Read(nullptr, 0, milliseconds(0)).code == Code::kClosed);
    std::uint8_t dummy = 0;
    BENTO_CHECK(first.Read(&dummy, 1, milliseconds(0)).code == Code::kClosed);
    BENTO_CHECK(first.Write(before_close.data(), before_close.size(), milliseconds(100)).code ==
                Code::kClosed);

    // close 之前已经写入的数据仍然可读，之后才是 EOF。
    std::vector<std::uint8_t> got(before_close.size());
    Result result = second.ReadFull(got.data(), got.size(), milliseconds(1000));
    BENTO_CHECK(result.ok());
    BENTO_CHECK(got == before_close);
    result = second.Read(got.data(), got.size(), milliseconds(100));
    BENTO_CHECK(result.code == Code::kClosed);
    result = second.Write(before_close.data(), before_close.size(), milliseconds(100));
    BENTO_CHECK(result.code == Code::kClosed);
    BENTO_CHECK(second.Close().ok());
    BENTO_CHECK(second.Close().ok());
}

void TestMemoryPartialReads() {
    PrintCase("memory: partial reads preserve byte order");
    MemoryTransport::Pair pair = MemoryTransport::CreatePair();
    MemoryTransport& first = *pair.first;
    MemoryTransport& second = *pair.second;

    const std::vector<std::uint8_t> payload = MakePattern(4096, 0xCAFEBABEU);
    Result write_result;
    std::thread writer([&] {
        write_result = second.WriteFull(payload.data(), payload.size(), milliseconds(kIoTimeoutMs));
    });

    std::vector<std::uint8_t> got;
    got.reserve(payload.size());
    std::vector<std::uint8_t> chunk(97);
    while (got.size() < payload.size()) {
        const std::size_t want = std::min(chunk.size(), payload.size() - got.size());
        const Result read_result = first.Read(chunk.data(), want, milliseconds(kIoTimeoutMs));
        BENTO_CHECK(read_result.ok());
        BENTO_CHECK(read_result.bytes > 0);
        BENTO_CHECK(read_result.bytes <= want);
        if (!read_result.ok() || read_result.bytes == 0) {
            break;
        }
        got.insert(got.end(), chunk.begin(),
                   chunk.begin() + static_cast<std::ptrdiff_t>(read_result.bytes));
    }
    writer.join();

    BENTO_CHECK(write_result.ok());
    BENTO_CHECK(got.size() == payload.size());
    BENTO_CHECK(got == payload);
}

// ---------------------------------------------------------------------------
// TcpTransport
// ---------------------------------------------------------------------------

void TestTcpBidirectional100K() {
    PrintCase("tcp: loopback bidirectional 100 KiB");
    TcpTransport listener;
    TcpTransport client;
    TcpTransport server;
    if (!SetupTcpPair(&listener, &client, &server)) {
        BENTO_CHECK(false);
        return;
    }

    const std::vector<std::uint8_t> client_payload = MakePattern(kLargeSize, 0x13579BDFU);
    const std::vector<std::uint8_t> server_payload = MakePattern(kLargeSize, 0x2468ACE0U);

    Result server_read;
    Result server_write;
    std::vector<std::uint8_t> server_got(kLargeSize);
    std::thread server_thread([&] {
        server_read = server.ReadFull(server_got.data(), server_got.size(),
                                      milliseconds(kIoTimeoutMs));
        if (server_read.ok()) {
            server_write = server.WriteFull(server_payload.data(), server_payload.size(),
                                            milliseconds(kIoTimeoutMs));
        }
    });

    const Result client_write = client.WriteFull(client_payload.data(), client_payload.size(),
                                                 milliseconds(kIoTimeoutMs));
    std::vector<std::uint8_t> client_got(kLargeSize);
    const Result client_read = client.ReadFull(client_got.data(), client_got.size(),
                                               milliseconds(kIoTimeoutMs));
    server_thread.join();

    BENTO_CHECK(client_write.ok());
    BENTO_CHECK(client_write.bytes == client_payload.size());
    BENTO_CHECK(server_read.ok());
    BENTO_CHECK(server_got == client_payload);
    BENTO_CHECK(server_write.ok());
    BENTO_CHECK(client_read.ok());
    BENTO_CHECK(client_got == server_payload);
}

void TestTcpAcceptTimeout() {
    PrintCase("tcp: accept timeout keeps the listener usable");
    TcpTransport listener;
    Result result = listener.Listen({"127.0.0.1", 0});
    BENTO_CHECK(result.ok());

    Endpoint local;
    result = listener.LocalEndpoint(&local);
    BENTO_CHECK(result.ok());

    TcpTransport accepted;
    const Clock::time_point start = Clock::now();
    result = listener.Accept(&accepted, milliseconds(120));
    const long long elapsed = ElapsedMs(start);
    BENTO_CHECK(result.code == Code::kTimeout);
    BENTO_CHECK(!accepted.is_open());
    BENTO_CHECK(elapsed >= 80);
    BENTO_CHECK(elapsed < 3000);

    // 超时不应破坏监听 socket：新的连接仍能被接受。
    TcpTransport client;
    result = client.Connect(local, milliseconds(2000));
    BENTO_CHECK(result.ok());
    result = listener.Accept(&accepted, milliseconds(2000));
    BENTO_CHECK(result.ok());
    BENTO_CHECK(accepted.is_open());

    BENTO_CHECK(accepted.Close().ok());
    BENTO_CHECK(accepted.Close().ok());
    std::uint8_t dummy = 0;
    BENTO_CHECK(accepted.Read(&dummy, 1, milliseconds(0)).code == Code::kClosed);
}

void TestTcpReadTimeout() {
    PrintCase("tcp: read timeout is respected and recoverable");
    TcpTransport listener;
    TcpTransport client;
    TcpTransport server;
    if (!SetupTcpPair(&listener, &client, &server)) {
        BENTO_CHECK(false);
        return;
    }

    std::vector<std::uint8_t> buffer(64);
    const Clock::time_point start = Clock::now();
    Result result = client.Read(buffer.data(), buffer.size(), milliseconds(120));
    const long long elapsed = ElapsedMs(start);
    BENTO_CHECK(result.code == Code::kTimeout);
    BENTO_CHECK(result.bytes == 0);
    BENTO_CHECK(elapsed >= 80);
    BENTO_CHECK(elapsed < 3000);

    const std::vector<std::uint8_t> late = {'l', 'a', 't', 'e'};
    BENTO_CHECK(server.WriteFull(late.data(), late.size(), milliseconds(1000)).ok());
    std::vector<std::uint8_t> got(late.size());
    result = client.ReadFull(got.data(), got.size(), milliseconds(1000));
    BENTO_CHECK(result.ok());
    BENTO_CHECK(got == late);
}

void TestTcpHalfClose() {
    PrintCase("tcp: half-close FIN is propagated without killing the response");
    TcpTransport listener;
    TcpTransport client;
    TcpTransport server;
    if (!SetupTcpPair(&listener, &client, &server)) {
        BENTO_CHECK(false);
        return;
    }

    const std::vector<std::uint8_t> request = {'p', 'i', 'n', 'g'};
    BENTO_CHECK(client.WriteFull(request.data(), request.size(), milliseconds(1000)).ok());
    std::vector<std::uint8_t> buffer(16);
    Result result = server.ReadFull(buffer.data(), request.size(), milliseconds(1000));
    BENTO_CHECK(result.ok());
    BENTO_CHECK(std::equal(request.begin(), request.end(), buffer.begin()));

    BENTO_CHECK(client.ShutdownWrite().ok());
    BENTO_CHECK(client.ShutdownWrite().ok());
    result = server.Read(buffer.data(), buffer.size(), milliseconds(300));
    BENTO_CHECK(result.code == Code::kClosed);
    BENTO_CHECK(client.Write(request.data(), request.size(), milliseconds(100)).code == Code::kClosed);

    // FIN 之后服务端仍可写回响应。
    const std::vector<std::uint8_t> response = {'p', 'o', 'n', 'g'};
    BENTO_CHECK(server.WriteFull(response.data(), response.size(), milliseconds(1000)).ok());
    std::vector<std::uint8_t> got(response.size());
    result = client.ReadFull(got.data(), got.size(), milliseconds(1000));
    BENTO_CHECK(result.ok());
    BENTO_CHECK(got == response);

    BENTO_CHECK(server.ShutdownWrite().ok());
    result = client.Read(buffer.data(), buffer.size(), milliseconds(300));
    BENTO_CHECK(result.code == Code::kClosed);

    BENTO_CHECK(client.Close().ok());
    BENTO_CHECK(client.Close().ok());
    BENTO_CHECK(client.Read(buffer.data(), buffer.size(), milliseconds(0)).code == Code::kClosed);
}

void TestTcpMultipleConnections() {
    PrintCase("tcp: two accepted connections stay isolated");
    TcpTransport listener;
    Result result = listener.Listen({"127.0.0.1", 0});
    BENTO_CHECK(result.ok());
    Endpoint local;
    result = listener.LocalEndpoint(&local);
    BENTO_CHECK(result.ok());

    TcpTransport first_client;
    TcpTransport first_server;
    result = first_client.Connect(local, milliseconds(2000));
    BENTO_CHECK(result.ok());
    result = listener.Accept(&first_server, milliseconds(2000));
    BENTO_CHECK(result.ok());

    TcpTransport second_client;
    TcpTransport second_server;
    result = second_client.Connect(local, milliseconds(2000));
    BENTO_CHECK(result.ok());
    result = listener.Accept(&second_server, milliseconds(2000));
    BENTO_CHECK(result.ok());

    const std::vector<std::uint8_t> first_message = {'o', 'n', 'e'};
    const std::vector<std::uint8_t> second_message = {'t', 'w', 'o'};
    BENTO_CHECK(first_client.WriteFull(first_message.data(), first_message.size(),
                                       milliseconds(1000))
                    .ok());
    BENTO_CHECK(second_client.WriteFull(second_message.data(), second_message.size(),
                                        milliseconds(1000))
                    .ok());

    std::vector<std::uint8_t> buffer(16);
    result = first_server.ReadFull(buffer.data(), first_message.size(), milliseconds(1000));
    BENTO_CHECK(result.ok());
    BENTO_CHECK(std::equal(first_message.begin(), first_message.end(), buffer.begin()));

    result = second_server.ReadFull(buffer.data(), second_message.size(), milliseconds(1000));
    BENTO_CHECK(result.ok());
    BENTO_CHECK(std::equal(second_message.begin(), second_message.end(), buffer.begin()));

    // 两条连接之间不能串流：此时先读 first_server 已经读完，second_server 也无剩余数据。
    result = second_server.Read(buffer.data(), buffer.size(), milliseconds(50));
    BENTO_CHECK(result.code == Code::kTimeout);
}

void TestTcpConnectRefused() {
    PrintCase("tcp: connect to a closed port fails with an error");
    TcpTransport probe;
    Result result = probe.Listen({"127.0.0.1", 0});
    BENTO_CHECK(result.ok());
    Endpoint local;
    result = probe.LocalEndpoint(&local);
    BENTO_CHECK(result.ok());
    BENTO_CHECK(probe.Close().ok());

    TcpTransport client;
    const Clock::time_point start = Clock::now();
    result = client.Connect(local, milliseconds(1000));
    const long long elapsed = ElapsedMs(start);
    BENTO_CHECK(result.code == Code::kError);
    BENTO_CHECK(result.system_error != 0);
    BENTO_CHECK(elapsed < 3000);
}

void TestTcpIpv6Loopback() {
    PrintCase("tcp: IPv6 loopback (skipped when unavailable)");
    TcpTransport listener;
    Result result = listener.Listen({"::1", 0});
    if (!result.ok()) {
        std::printf("[SKIP] IPv6 listen(::1) unavailable: errno=%d\n", result.system_error);
        return;
    }

    Endpoint local;
    result = listener.LocalEndpoint(&local);
    if (!result.ok() || local.host.find(':') == std::string::npos) {
        std::printf("[SKIP] IPv6 local endpoint unavailable\n");
        return;
    }

    TcpTransport client;
    result = client.Connect(local, milliseconds(1000));
    if (!result.ok()) {
        std::printf("[SKIP] IPv6 connect failed: errno=%d\n", result.system_error);
        return;
    }
    TcpTransport server;
    result = listener.Accept(&server, milliseconds(1000));
    if (!result.ok()) {
        std::printf("[SKIP] IPv6 accept failed: errno=%d\n", result.system_error);
        return;
    }

    const std::vector<std::uint8_t> message = {'v', '6'};
    BENTO_CHECK(client.WriteFull(message.data(), message.size(), milliseconds(1000)).ok());
    std::vector<std::uint8_t> got(message.size());
    const Result read_result = server.ReadFull(got.data(), got.size(), milliseconds(1000));
    BENTO_CHECK(read_result.ok());
    BENTO_CHECK(got == message);
}

// ---------------------------------------------------------------------------
// Frame（MOD-10）：复用并发 agent 的 20B 帧实现，做 transport 侧集成验证
// ---------------------------------------------------------------------------

void WriteBe16(std::uint8_t* out, std::uint16_t value) {
    out[0] = static_cast<std::uint8_t>(value >> 8);
    out[1] = static_cast<std::uint8_t>(value);
}

void WriteBe32(std::uint8_t* out, std::uint32_t value) {
    out[0] = static_cast<std::uint8_t>(value >> 24);
    out[1] = static_cast<std::uint8_t>(value >> 16);
    out[2] = static_cast<std::uint8_t>(value >> 8);
    out[3] = static_cast<std::uint8_t>(value);
}

/// 手工构造 20B 帧头（checksum 填 0），用于制造异常输入。
std::vector<std::uint8_t> MakeRawHeader(std::uint16_t version, std::uint16_t type,
                                        std::uint16_t reserved, std::uint32_t length) {
    std::vector<std::uint8_t> header(kGenericFrameHeaderBytes, 0);
    WriteBe32(header.data(), kFrameMagic);
    WriteBe16(header.data() + 4, version);
    WriteBe16(header.data() + 6, type);
    WriteBe16(header.data() + 8, 0);
    WriteBe16(header.data() + 10, reserved);
    WriteBe32(header.data() + 12, length);
    return header;
}

void TestFrameRoundTrip() {
    PrintCase("frame: encode/decode round-trip (20B header)");
    Frame frame;
    frame.type = 0x1234;
    frame.flags = 0x00F0;
    frame.payload = MakePattern(123, 0xF00DU);

    const std::vector<std::uint8_t> bytes = Encode(frame);
    BENTO_CHECK(!bytes.empty());
    BENTO_CHECK(bytes.size() == kGenericFrameHeaderBytes + frame.payload.size());
    BENTO_CHECK(bytes[0] == 0x42);
    BENTO_CHECK(bytes[1] == 0x45);
    BENTO_CHECK(bytes[2] == 0x4E);
    BENTO_CHECK(bytes[3] == 0x54);
    BENTO_CHECK(bytes[4] == 0x00);
    BENTO_CHECK(bytes[5] == 0x01);

    Frame decoded;
    std::size_t consumed = 0;
    BENTO_CHECK(Decode(bytes.data(), bytes.size(), &decoded, &consumed) == FrameStatus::kOk);
    BENTO_CHECK(consumed == bytes.size());
    BENTO_CHECK(decoded.magic == kFrameMagic);
    BENTO_CHECK(decoded.version == kFrameVersion);
    BENTO_CHECK(decoded.type == frame.type);
    BENTO_CHECK(decoded.flags == frame.flags);
    BENTO_CHECK(decoded.reserved == 0);
    BENTO_CHECK(decoded.Length() == frame.payload.size());
    BENTO_CHECK(decoded.payload == frame.payload);
    BENTO_CHECK(decoded.checksum ==
                ComputeFrameChecksum(bytes.data(), bytes.data() + kGenericFrameHeaderBytes,
                                     frame.payload.size()));

    FrameDecoder decoder;
    decoder.Append(bytes.data(), bytes.size());
    Frame streamed;
    BENTO_CHECK(decoder.Next(&streamed) == FrameStatus::kOk);
    BENTO_CHECK(streamed.payload == frame.payload);
    BENTO_CHECK(decoder.Next(&streamed) == FrameStatus::kNeedMoreData);
    BENTO_CHECK(decoder.PendingBytes() == 0U);

    Frame empty;
    empty.type = 0xABCD;
    const std::vector<std::uint8_t> empty_bytes = Encode(empty);
    BENTO_CHECK(empty_bytes.size() == kGenericFrameHeaderBytes);
    Frame empty_decoded;
    BENTO_CHECK(Decode(empty_bytes.data(), empty_bytes.size(), &empty_decoded) ==
                FrameStatus::kOk);
    BENTO_CHECK(empty_decoded.payload.empty());
}

void TestFrameStickyPackets() {
    PrintCase("frame: three frames in one append (sticky packets)");
    std::vector<std::uint8_t> stream;
    std::vector<Frame> frames;
    for (std::uint16_t i = 0; i < 3; ++i) {
        Frame frame;
        frame.type = static_cast<std::uint16_t>(0x100U + i);
        frame.flags = i;
        frame.payload = MakePattern(30 + i * 17, 0x1000U + i);
        const std::vector<std::uint8_t> bytes = Encode(frame);
        BENTO_CHECK(!bytes.empty());
        stream.insert(stream.end(), bytes.begin(), bytes.end());
        frames.push_back(std::move(frame));
    }

    FrameDecoder decoder;
    decoder.Append(stream.data(), stream.size());
    for (std::size_t i = 0; i < frames.size(); ++i) {
        Frame decoded;
        BENTO_CHECK(decoder.Next(&decoded) == FrameStatus::kOk);
        BENTO_CHECK(decoded.type == frames[i].type);
        BENTO_CHECK(decoded.flags == frames[i].flags);
        BENTO_CHECK(decoded.payload == frames[i].payload);
    }
    Frame extra;
    BENTO_CHECK(decoder.Next(&extra) == FrameStatus::kNeedMoreData);
    BENTO_CHECK(decoder.PendingBytes() == 0U);
}

void TestFrameHalfPacket() {
    PrintCase("frame: half packet fed one byte at a time");
    Frame frame;
    frame.type = 0x4242;
    frame.flags = 0x0001;
    frame.payload = MakePattern(35, 0x7777U);
    const std::vector<std::uint8_t> bytes = Encode(frame);
    BENTO_CHECK(!bytes.empty());

    FrameDecoder decoder;
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        decoder.Append(&bytes[i], 1);
        Frame decoded;
        const FrameStatus status = decoder.Next(&decoded);
        if (i + 1 < bytes.size()) {
            BENTO_CHECK(status == FrameStatus::kNeedMoreData);
        } else {
            BENTO_CHECK(status == FrameStatus::kOk);
            BENTO_CHECK(decoded.type == frame.type);
            BENTO_CHECK(decoded.payload == frame.payload);
        }
    }
    BENTO_CHECK(decoder.PendingBytes() == 0U);
}

void TestFrameBadMagic() {
    PrintCase("frame: bad magic is rejected");
    Frame frame;
    frame.type = 1;
    frame.payload = MakePattern(8, 0x1111U);
    std::vector<std::uint8_t> bytes = Encode(frame);
    BENTO_CHECK(!bytes.empty());
    bytes[0] ^= 0xFFU;

    Frame decoded;
    BENTO_CHECK(Decode(bytes.data(), bytes.size(), &decoded) == FrameStatus::kBadMagic);
    FrameDecoder decoder;
    decoder.Append(bytes.data(), bytes.size());
    BENTO_CHECK(decoder.Next(&decoded) == FrameStatus::kBadMagic);
}

void TestFrameBadVersionAndReserved() {
    PrintCase("frame: bad version/reserved are rejected");
    Frame decoded;
    std::vector<std::uint8_t> header = MakeRawHeader(2, 1, 0, 0);
    BENTO_CHECK(Decode(header.data(), header.size(), &decoded) == FrameStatus::kBadVersion);
    header = MakeRawHeader(kFrameVersion, 1, 1, 0);
    BENTO_CHECK(Decode(header.data(), header.size(), &decoded) == FrameStatus::kBadReserved);
}

void TestFrameLengthExceeded() {
    PrintCase("frame: length limit rejects oversized frames");
    Frame oversize;
    oversize.payload.resize(static_cast<std::size_t>(kFrameMaxPayload) + 1U);
    BENTO_CHECK(Encode(oversize).empty());

    const std::vector<std::uint8_t> header =
        MakeRawHeader(kFrameVersion, 1, 0, kFrameMaxPayload + 1U);
    Frame decoded;
    BENTO_CHECK(Decode(header.data(), header.size(), &decoded) == FrameStatus::kLengthExceeded);
    FrameDecoder decoder;
    decoder.Append(header.data(), header.size());
    BENTO_CHECK(decoder.Next(&decoded) == FrameStatus::kLengthExceeded);
}

void TestFrameChecksumMismatch() {
    PrintCase("frame: checksum failure is rejected");
    Frame frame;
    frame.type = 0x5151;
    frame.payload = MakePattern(64, 0x4444U);
    std::vector<std::uint8_t> bytes = Encode(frame);
    BENTO_CHECK(!bytes.empty());
    bytes.back() ^= 0x01U;

    Frame decoded;
    BENTO_CHECK(Decode(bytes.data(), bytes.size(), &decoded) == FrameStatus::kChecksumMismatch);
    FrameDecoder decoder;
    decoder.Append(bytes.data(), bytes.size());
    BENTO_CHECK(decoder.Next(&decoded) == FrameStatus::kChecksumMismatch);

    // 帧头 type 也参与 CRC：改动它同样应失败。
    std::vector<std::uint8_t> header_bytes = Encode(frame);
    BENTO_CHECK(!header_bytes.empty());
    header_bytes[7] ^= 0x01U;
    BENTO_CHECK(Decode(header_bytes.data(), header_bytes.size(), &decoded) ==
                FrameStatus::kChecksumMismatch);
}

}  // namespace

int main() {
    TestMemoryBidirectional100K();
    TestMemoryHalfClose();
    TestMemoryTimeout();
    TestMemoryCloseIdempotent();
    TestMemoryPartialReads();

    TestTcpBidirectional100K();
    TestTcpAcceptTimeout();
    TestTcpReadTimeout();
    TestTcpHalfClose();
    TestTcpMultipleConnections();
    TestTcpConnectRefused();
    TestTcpIpv6Loopback();

    TestFrameRoundTrip();
    TestFrameStickyPackets();
    TestFrameHalfPacket();
    TestFrameBadMagic();
    TestFrameBadVersionAndReserved();
    TestFrameLengthExceeded();
    TestFrameChecksumMismatch();

    return bento_test::Finish("bento_transport");
}
