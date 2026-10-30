/**
 * @file test_omni_server.cpp
 * @brief OmniNetServer M0 骨架测试：配置矩阵、生命周期、Memory 回环、Channel 契约、统计。
 * @author go2cpp contributors
 * @date 2026-10-30
 * @copyright MIT License
 *
 * @details 覆盖 M0 的全部承诺（设计 §5.1 的退出条件）：
 *   1. 配置矩阵：4 transport × 5 protocol 的非法组合返回对应错误码；
 *      mode × concurrency 的 2 × 3 矩阵；枚举越界、空/重复协议、limits 关系；
 *   2. Start/Stop：重复 Start、Stop 幂等、Stop 终态、Stop 后接入拒绝；
 *   3. Raw 回环：真实 MemoryTransport 双端，客户端写入 → PollOnce → Inbound 帧，
 *      Outbound 帧 → PollOnce → 客户端读到；100 轮随机长度；批量写不断言边界；
 *   4. Channel 契约：C1 同一 shared_ptr、C2 关闭可感知、C3 关闭后拒绝发送、
 *      视图/句柄晚于 NetServer 析构仍可读终值；
 *   5. 多连接：三条回环互不串数据，独立关闭不影响其它连接；
 *   6. 统计：accepted/active/closed_graceful/closed_abort/bytes/messages/raw_frames
 *      与连接级计数；
 *   7. 反压：Inbound 容量 1 时暂停读、不丢帧；Outbound 满时 TrySend 返回 kWouldBlock；
 *   8. 边界：超限出站帧按 kLimitExceeded 断开；abort 停止的关闭原因与计数。
 *
 * 断言只在主线程执行；M0 没有后台线程，因此不需要原子计数。
 */

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "go2cpp/bento/net/memory_transport.hpp"
#include "go2cpp/bento/net/omni_server.hpp"

namespace {

using go2cpp::bento::net::Code;
using go2cpp::bento::net::ITransport;
using go2cpp::bento::net::MemoryTransport;
using go2cpp::bento::net::Result;
using go2cpp::bento::omni::ChannelPtr;
using go2cpp::bento::omni::CloseMode;
using go2cpp::bento::omni::CloseReason;
using go2cpp::bento::omni::ConcurrencyKind;
using go2cpp::bento::omni::IoMode;
using go2cpp::bento::omni::NetServer;
using go2cpp::bento::omni::NetStats;
using go2cpp::bento::omni::OmniError;
using go2cpp::bento::omni::OmniNetConfig;
using go2cpp::bento::omni::ProtocolKind;
using go2cpp::bento::omni::RawConnection;
using go2cpp::bento::omni::RawFrame;
using go2cpp::bento::omni::TransportKind;

using Clock = std::chrono::steady_clock;
using Milliseconds = std::chrono::milliseconds;

int g_checks = 0;
int g_failures = 0;

#define CHECK(condition)                                                               \
    do {                                                                               \
        ++g_checks;                                                                    \
        if (!(condition)) {                                                            \
            std::fprintf(stderr, "[FAIL] %s:%d: %s\n", __FILE__, __LINE__, #condition); \
            ++g_failures;                                                              \
        }                                                                              \
    } while (0)

#define CHECK_EQ(actual, expected)                                                       \
    do {                                                                                 \
        ++g_checks;                                                                      \
        if (!((actual) == (expected))) {                                                 \
            std::fprintf(stderr, "[FAIL] %s:%d: %s == %s\n", __FILE__, __LINE__, #actual, \
                         #expected);                                                       \
            ++g_failures;                                                                \
        }                                                                                \
    } while (0)

void PrintCase(const char* name) {
    std::printf("[CASE] %s\n", name);
}

/// M0 唯一合法的运行时配置：Memory + Raw + NonBlocking + Poll。
OmniNetConfig MemoryRawConfig() {
    OmniNetConfig config;
    config.mode = IoMode::kNonBlocking;
    config.transport = TransportKind::kMemory;
    config.protocols = {ProtocolKind::kRaw};
    config.concurrency = ConcurrencyKind::kPoll;
    return config;
}

std::unique_ptr<NetServer> CreateStarted(const OmniNetConfig& config) {
    std::string error;
    auto server = NetServer::Create(config, &error);
    if (server == nullptr) {
        std::fprintf(stderr, "[FAIL] Create 失败：%s\n", error.c_str());
        ++g_failures;
        return nullptr;
    }
    if (!server->Start(&error)) {
        std::fprintf(stderr, "[FAIL] Start 失败：%s\n", error.c_str());
        ++g_failures;
        return nullptr;
    }
    return server;
}

std::vector<std::uint8_t> Bytes(std::initializer_list<std::uint8_t> values) {
    return std::vector<std::uint8_t>(values);
}

std::vector<std::uint8_t> RandomBytes(std::size_t size, std::uint64_t seed) {
    std::vector<std::uint8_t> bytes(size);
    std::uint64_t state = seed | 1U;
    for (std::size_t index = 0; index < size; ++index) {
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
        bytes[index] = static_cast<std::uint8_t>(state & 0xFFU);
    }
    return bytes;
}

bool WriteBytes(ITransport& transport, const std::vector<std::uint8_t>& bytes) {
    const Result result = transport.WriteFull(bytes.data(), bytes.size(), Milliseconds{2000});
    CHECK(result.ok());
    CHECK_EQ(result.bytes, bytes.size());
    return result.ok() && result.bytes == bytes.size();
}

bool ReadBytes(ITransport& transport, std::size_t size, std::vector<std::uint8_t>* out) {
    out->assign(size, std::uint8_t{0});
    const Result result = transport.ReadFull(out->data(), size, Milliseconds{2000});
    CHECK(result.ok());
    CHECK_EQ(result.bytes, size);
    return result.ok() && result.bytes == size;
}

/// 从通道取一帧；无数据/已关闭返回 false。
bool TakeFrame(const ChannelPtr<RawFrame>& channel, std::vector<std::uint8_t>* out) {
    auto result = channel->TryRecv();
    if (!result.ok) {
        return false;
    }
    *out = std::move(result.value->bytes);
    return true;
}

std::vector<std::vector<std::uint8_t>> DrainFrames(const ChannelPtr<RawFrame>& channel) {
    std::vector<std::vector<std::uint8_t>> frames;
    for (;;) {
        auto result = channel->TryRecv();
        if (!result.ok) {
            break;
        }
        frames.push_back(std::move(result.value->bytes));
    }
    return frames;
}

/// 反复 PollOnce 直到收到至少 `want` 帧或超时；返回收集到的帧。
bool PollForFrames(NetServer& server, const ChannelPtr<RawFrame>& channel, std::size_t want,
                   std::vector<std::vector<std::uint8_t>>* frames, int timeout_ms = 1000) {
    const Clock::time_point deadline = Clock::now() + Milliseconds{timeout_ms};
    for (;;) {
        (void)server.PollOnce(Milliseconds{0});
        *frames = DrainFrames(channel);
        if (frames->size() >= want) {
            return true;
        }
        if (Clock::now() >= deadline) {
            return false;
        }
        std::this_thread::sleep_for(Milliseconds{1});
    }
}

struct Loopback {
    std::unique_ptr<ITransport> client;
    RawConnection connection;
};

/// 建一对 Memory 回环：客户端传输 + 服务器侧连接句柄。
bool Connect(NetServer& server, Loopback* loop) {
    std::string error;
    loop->client = server.ConnectMemory(&error);
    if (loop->client == nullptr) {
        std::fprintf(stderr, "[FAIL] ConnectMemory 失败：%s\n", error.c_str());
        ++g_failures;
        return false;
    }
    auto accepted = server.AsRawServer().Accept()->TryRecv();
    if (!accepted.ok) {
        std::fprintf(stderr, "[FAIL] Accept 通道没有收到新连接\n");
        ++g_failures;
        return false;
    }
    loop->connection = *accepted.value;
    return true;
}

// ---------------------------------------------------------------------------
// 1. 配置矩阵与 Create 错误码
// ---------------------------------------------------------------------------

/// 独立于被测代码重算设计 §2.2 的 Transport × Protocol 矩阵（参考实现）。
bool ReferenceMatrix(TransportKind transport, ProtocolKind protocol) {
    switch (transport) {
        case TransportKind::kTcp:
            return protocol == ProtocolKind::kHttp || protocol == ProtocolKind::kSse ||
                   protocol == ProtocolKind::kWs || protocol == ProtocolKind::kRaw;
        case TransportKind::kUdp:
            return protocol == ProtocolKind::kLink || protocol == ProtocolKind::kRaw;
        case TransportKind::kTls:
            return protocol == ProtocolKind::kHttp || protocol == ProtocolKind::kSse ||
                   protocol == ProtocolKind::kWs || protocol == ProtocolKind::kRaw;
        case TransportKind::kMemory:
            return true;
    }
    return false;
}

OmniError ReferenceCreateResult(TransportKind transport, ProtocolKind protocol) {
    if (!ReferenceMatrix(transport, protocol)) {
        return OmniError::kUnsupportedCombination;
    }
    if (transport == TransportKind::kTls) {
        return OmniError::kUnsupported;  // V5：TLS 服务端 M4 才有。
    }
    if (transport == TransportKind::kMemory) {
        return protocol == ProtocolKind::kRaw ? OmniError::kOk : OmniError::kUnsupported;
    }
    // M1：UDP 只实现 Link、TCP 只实现 HTTP/SSE；其它协议是 kUnsupported。
    // 而本矩阵的基础配置是 NonBlocking + Poll，M1 的真实后端自带线程、
    // 不接受 Poll（kUnsupportedCombination）。真实后端的合法配置由
    // test_omni_server_m1.cpp 用 Gmp/Thread 覆盖。
    if (transport == TransportKind::kUdp && protocol != ProtocolKind::kLink) {
        return OmniError::kUnsupported;
    }
    if (transport == TransportKind::kTcp && protocol != ProtocolKind::kHttp &&
        protocol != ProtocolKind::kSse) {
        return OmniError::kUnsupported;
    }
    return OmniError::kUnsupportedCombination;
}

void TestConfigMatrix() {
    PrintCase("配置矩阵：4 transport × 5 protocol + mode×concurrency + limits");

    const TransportKind transports[] = {TransportKind::kTcp, TransportKind::kUdp,
                                        TransportKind::kTls, TransportKind::kMemory};
    const ProtocolKind protocols[] = {ProtocolKind::kHttp, ProtocolKind::kSse,
                                      ProtocolKind::kWs, ProtocolKind::kLink,
                                      ProtocolKind::kRaw};
    std::size_t legal = 0;
    std::size_t rejected = 0;
    for (const TransportKind transport : transports) {
        for (const ProtocolKind protocol : protocols) {
            OmniNetConfig config = MemoryRawConfig();
            config.transport = transport;
            config.protocols = {protocol};
            std::string error;
            const OmniError expected = ReferenceCreateResult(transport, protocol);
            const OmniError actual = NetServer::ValidateConfig(config, &error);
            CHECK_EQ(actual, expected);
            if (expected != OmniError::kOk) {
                CHECK(!error.empty());
            } else {
                CHECK(error.empty());
            }
            auto server = NetServer::Create(config, &error);
            if (expected == OmniError::kOk) {
                CHECK(server != nullptr);
                CHECK(error.empty());
                ++legal;
            } else {
                CHECK(server == nullptr);
                CHECK(!error.empty());
                ++rejected;
            }
        }
    }
    CHECK_EQ(legal, std::size_t{1});     // 只有 Memory × Raw 在 M0 落地。
    CHECK_EQ(rejected, std::size_t{19});  // 其余 19 种组合全部在 Create 阶段失败。

    // mode × concurrency：Blocking + Poll 非法，其余 5 种合法。
    const IoMode modes[] = {IoMode::kBlocking, IoMode::kNonBlocking};
    const ConcurrencyKind kinds[] = {ConcurrencyKind::kGmp, ConcurrencyKind::kThread,
                                     ConcurrencyKind::kPoll};
    for (const IoMode mode : modes) {
        for (const ConcurrencyKind kind : kinds) {
            OmniNetConfig config = MemoryRawConfig();
            config.mode = mode;
            config.concurrency = kind;
            const OmniError expected = (mode == IoMode::kBlocking && kind == ConcurrencyKind::kPoll)
                                           ? OmniError::kUnsupportedCombination
                                           : OmniError::kOk;
            CHECK_EQ(NetServer::ValidateConfig(config, nullptr), expected);
        }
    }

    // 枚举越界：V1 必须挡住 static_cast 的脏值。
    {
        OmniNetConfig config = MemoryRawConfig();
        config.mode = static_cast<IoMode>(99);
        CHECK_EQ(NetServer::ValidateConfig(config, nullptr), OmniError::kInvalidConfig);
        config = MemoryRawConfig();
        config.transport = static_cast<TransportKind>(99);
        CHECK_EQ(NetServer::ValidateConfig(config, nullptr), OmniError::kInvalidConfig);
        config = MemoryRawConfig();
        config.concurrency = static_cast<ConcurrencyKind>(99);
        CHECK_EQ(NetServer::ValidateConfig(config, nullptr), OmniError::kInvalidConfig);
        config = MemoryRawConfig();
        config.protocols = {static_cast<ProtocolKind>(99)};
        CHECK_EQ(NetServer::ValidateConfig(config, nullptr), OmniError::kInvalidConfig);
    }

    // 空协议 / 重复协议。
    {
        OmniNetConfig config = MemoryRawConfig();
        config.protocols.clear();
        CHECK_EQ(NetServer::ValidateConfig(config, nullptr), OmniError::kInvalidConfig);
        config = MemoryRawConfig();
        config.protocols = {ProtocolKind::kRaw, ProtocolKind::kRaw};
        CHECK_EQ(NetServer::ValidateConfig(config, nullptr), OmniError::kInvalidConfig);
        // Memory × {Raw, Link} 矩阵合法，但 M0 只实现 Raw → kUnsupported。
        config = MemoryRawConfig();
        config.protocols = {ProtocolKind::kRaw, ProtocolKind::kLink};
        CHECK_EQ(NetServer::ValidateConfig(config, nullptr), OmniError::kUnsupported);
    }

    // limits 关系（V6）。
    {
        OmniNetConfig config = MemoryRawConfig();
        config.limits.max_connections = 0;
        CHECK_EQ(NetServer::ValidateConfig(config, nullptr), OmniError::kInvalidConfig);
        config = MemoryRawConfig();
        config.limits.inbound_capacity = 0;
        CHECK_EQ(NetServer::ValidateConfig(config, nullptr), OmniError::kInvalidConfig);
        config = MemoryRawConfig();
        config.limits.outbound_capacity = 0;
        CHECK_EQ(NetServer::ValidateConfig(config, nullptr), OmniError::kInvalidConfig);
        config = MemoryRawConfig();
        config.limits.max_message_bytes = 0;
        CHECK_EQ(NetServer::ValidateConfig(config, nullptr), OmniError::kInvalidConfig);
        config = MemoryRawConfig();
        config.limits.read_buffer_bytes = 0;
        CHECK_EQ(NetServer::ValidateConfig(config, nullptr), OmniError::kInvalidConfig);
        config = MemoryRawConfig();
        config.limits.max_accept_per_tick = 0;
        CHECK_EQ(NetServer::ValidateConfig(config, nullptr), OmniError::kInvalidConfig);
        config = MemoryRawConfig();
        config.limits.per_connection_budget_bytes = config.limits.max_message_bytes - 1;
        CHECK_EQ(NetServer::ValidateConfig(config, nullptr), OmniError::kInvalidConfig);
        config = MemoryRawConfig();
        config.limits.read_buffer_bytes = config.limits.per_connection_budget_bytes + 1;
        CHECK_EQ(NetServer::ValidateConfig(config, nullptr), OmniError::kInvalidConfig);
        config = MemoryRawConfig();
        config.limits.idle_timeout = Milliseconds{1000};
        config.limits.close_timeout = Milliseconds{5000};
        CHECK_EQ(NetServer::ValidateConfig(config, nullptr), OmniError::kInvalidConfig);
        config = MemoryRawConfig();
        config.limits.idle_timeout = Milliseconds{0};  // 0 = 不超时，允许 close > idle。
        config.limits.close_timeout = Milliseconds{5000};
        CHECK_EQ(NetServer::ValidateConfig(config, nullptr), OmniError::kOk);
        config = MemoryRawConfig();
        config.limits.write_timeout = Milliseconds{-1};
        CHECK_EQ(NetServer::ValidateConfig(config, nullptr), OmniError::kInvalidConfig);
    }

    // M1：设计稿默认配置（Tcp + Http + Gmp）已经落地，Create 必须成功。
    {
        const OmniNetConfig defaults;
        std::string error;
        CHECK_EQ(NetServer::ValidateConfig(defaults, &error), OmniError::kOk);
        CHECK(error.empty());
        auto default_server = NetServer::Create(defaults, &error);
        CHECK(default_server != nullptr);
        CHECK(error.empty());
    }

    // 视图：Raw 合法则 valid，Link 在 M0 恒为占位视图（valid == false）。
    {
        auto server = NetServer::Create(MemoryRawConfig(), nullptr);
        CHECK(server != nullptr);
        if (server != nullptr) {
            auto raw = server->AsRawServer();
            auto link = server->AsLinkServer();
            CHECK(raw.valid());
            CHECK_EQ(raw.protocol(), ProtocolKind::kRaw);
            CHECK(!link.valid());
            CHECK_EQ(link.protocol(), ProtocolKind::kLink);
            CHECK_EQ(server->config().transport, TransportKind::kMemory);
        }
    }
}

// ---------------------------------------------------------------------------
// 2. Start / Stop 生命周期
// ---------------------------------------------------------------------------

void TestStartStop() {
    PrintCase("生命周期：Start/Stop 幂等、Stop 终态、未启动拒绝接入");

    auto server = NetServer::Create(MemoryRawConfig(), nullptr);
    CHECK(server != nullptr);
    if (server == nullptr) {
        return;
    }
    CHECK(!server->running());
    CHECK_EQ(server->Port(), std::uint16_t{0});
    CHECK_EQ(server->Connections(), std::size_t{0});

    // 未 Start：接入必须被拒绝，且不能留下连接。
    {
        auto pair = MemoryTransport::CreatePair();
        const auto id = server->AttachConnection(std::move(pair.second));
        CHECK(!id.valid());
        CHECK_EQ(server->Connections(), std::size_t{0});
        std::string error;
        CHECK(server->ConnectMemory(&error) == nullptr);
        CHECK(!error.empty());
    }

    // 未 Start：PollOnce 返回 kNotRunning。
    CHECK_EQ(server->PollOnce(Milliseconds{0}).error, OmniError::kNotRunning);

    std::string error;
    CHECK(server->Start(&error));
    CHECK(server->running());
    CHECK(error.empty());
    // 重复 Start 必须失败。
    CHECK(!server->Start(&error));
    CHECK(!error.empty());

    auto raw = server->AsRawServer();
    CHECK(raw.valid());
    const auto accept = raw.Accept();
    CHECK(accept != nullptr);

    Loopback loop;
    CHECK(Connect(*server, &loop));
    CHECK(loop.connection.valid());
    CHECK_EQ(server->Connections(), std::size_t{1});
    CHECK_EQ(server->AsRawServer().Connections().size(), std::size_t{1});

    // 未 Start 的 Poll 已经验证；运行中正常轮询（无数据 → timeout=true）。
    const auto idle = server->PollOnce(Milliseconds{0});
    CHECK(idle.ok());
    CHECK_EQ(idle.events, std::size_t{0});
    CHECK(idle.timeout);

    // 优雅 Stop：连接被关闭、计数归零、幂等。
    server->Stop();
    CHECK(!server->running());
    server->Stop();
    CHECK_EQ(server->Connections(), std::size_t{0});
    CHECK_EQ(server->Stats().active, std::uint64_t{0});
    CHECK_EQ(server->Stats().closed_graceful, std::uint64_t{1});
    CHECK(loop.connection.close_reason() == CloseReason::kShutdown);
    CHECK(loop.connection.Inbound()->IsClosed());
    CHECK(loop.connection.Outbound()->IsClosed());
    // Accept 通道随 Stop 关闭，且不会产生新连接。
    CHECK(accept->IsClosed());

    // Stop 是终态：不能重新 Start，也不能再接连接。
    CHECK(!server->Start(&error));
    auto rejected_pair = MemoryTransport::CreatePair();
    CHECK(!server->AttachConnection(std::move(rejected_pair.second)).valid());

    // 非 Poll 模式调用 PollOnce 返回 kNotPollMode。
    {
        OmniNetConfig config = MemoryRawConfig();
        config.concurrency = ConcurrencyKind::kGmp;
        auto gmp = CreateStarted(config);
        CHECK(gmp != nullptr);
        if (gmp != nullptr) {
            CHECK_EQ(gmp->PollOnce(Milliseconds{0}).error, OmniError::kNotPollMode);
        }
    }
}

// ---------------------------------------------------------------------------
// 3. Raw 回环收发
// ---------------------------------------------------------------------------

void TestRawLoopback() {
    PrintCase("Raw 回环：客户端→Inbound、Outbound→客户端、随机长度、批量、对端关闭");

    auto server = CreateStarted(MemoryRawConfig());
    CHECK(server != nullptr);
    if (server == nullptr) {
        return;
    }
    Loopback loop;
    CHECK(Connect(*server, &loop));
    if (!loop.client || !loop.connection.valid()) {
        return;
    }
    const auto inbound = loop.connection.Inbound();
    const auto outbound = loop.connection.Outbound();
    CHECK(inbound != nullptr);
    CHECK(outbound != nullptr);

    // 客户端 → 服务器：写入 → PollOnce → Inbound 收到同一帧。
    const auto first_payload = Bytes({0x00, 0x01, 0x7F, 0x80, 0xFF});
    CHECK(WriteBytes(*loop.client, first_payload));
    const auto poll_in = server->PollOnce(Milliseconds{0});
    CHECK(poll_in.ok());
    CHECK(poll_in.events > 0);
    CHECK(!poll_in.timeout);
    std::vector<std::uint8_t> received;
    CHECK(TakeFrame(inbound, &received));
    CHECK(received == first_payload);

    // 服务器 → 客户端：Outbound 帧 → PollOnce → 客户端读到同样字节。
    const auto echo_payload = Bytes({'o', 'm', 'n', 'i'});
    RawFrame echo;
    echo.bytes = echo_payload;
    CHECK(outbound->TrySend(std::move(echo)).Ok());
    const auto poll_out = server->PollOnce(Milliseconds{0});
    CHECK(poll_out.events > 0);
    std::vector<std::uint8_t> read_back;
    CHECK(ReadBytes(*loop.client, echo_payload.size(), &read_back));
    CHECK(read_back == echo_payload);

    // 100 轮随机长度，逐轮写入 → 轮询 → 校验，验证无丢字节/串字节。
    std::uint64_t seed = 0x243F6A8885A308D3ULL;
    for (int round = 0; round < 100; ++round) {
        seed = seed * 6364136223846793005ULL + 1442695040888963407ULL;
        const std::size_t size = 1 + static_cast<std::size_t>((seed >> 33) % 512U);
        const auto payload = RandomBytes(size, seed);
        CHECK(WriteBytes(*loop.client, payload));
        std::vector<std::vector<std::uint8_t>> frames;
        CHECK(PollForFrames(*server, inbound, 1, &frames));
        CHECK(!frames.empty());
        if (!frames.empty()) {
            CHECK(frames.front() == payload);
        }
    }

    // 批量写：M0 不分帧，边界不保证；但拼接后的字节必须逐字节一致（无丢失）。
    const auto chunk_a = Bytes({'a', 'a', 'a'});
    const auto chunk_b = Bytes({'b', 'b'});
    const auto chunk_c = Bytes({'c'});
    CHECK(WriteBytes(*loop.client, chunk_a));
    CHECK(WriteBytes(*loop.client, chunk_b));
    CHECK(WriteBytes(*loop.client, chunk_c));
    std::vector<std::vector<std::uint8_t>> batch;
    CHECK(PollForFrames(*server, inbound, 1, &batch));
    std::vector<std::uint8_t> concatenated;
    for (const auto& frame : batch) {
        concatenated.insert(concatenated.end(), frame.begin(), frame.end());
    }
    const auto expected = Bytes({'a', 'a', 'a', 'b', 'b', 'c'});
    CHECK(concatenated == expected);
    CHECK(!batch.empty());

    // 对端关闭：服务器读到 EOF → kPeerClosed，Inbound 关闭（已交付值先排空）。
    loop.client->Close();
    bool closed = false;
    const Clock::time_point deadline = Clock::now() + Milliseconds{1000};
    while (Clock::now() < deadline) {
        (void)server->PollOnce(Milliseconds{0});
        if (loop.connection.close_reason() == CloseReason::kPeerClosed) {
            closed = true;
            break;
        }
        std::this_thread::sleep_for(Milliseconds{1});
    }
    CHECK(closed);
    CHECK(loop.connection.Inbound()->IsClosed());
    auto tail = loop.connection.Inbound()->TryRecv();
    CHECK(tail.Closed());
    CHECK_EQ(server->Connections(), std::size_t{0});
}

// ---------------------------------------------------------------------------
// 4. Channel 契约与生命周期
// ---------------------------------------------------------------------------

void TestChannelContracts() {
    PrintCase("Channel 契约：C1 同一 shared_ptr、C2/C3 关闭感知、视图晚于 server");

    auto server = CreateStarted(MemoryRawConfig());
    CHECK(server != nullptr);
    if (server == nullptr) {
        return;
    }
    auto raw = server->AsRawServer();
    const auto accept = raw.Accept();
    Loopback loop;
    CHECK(Connect(*server, &loop));
    if (!loop.client || !loop.connection.valid()) {
        return;
    }
    const auto inbound = loop.connection.Inbound();
    const auto outbound = loop.connection.Outbound();
    // C1：连接生命周期内重复调用返回同一个 shared_ptr。
    CHECK(inbound.get() == loop.connection.Inbound().get());
    CHECK(outbound.get() == loop.connection.Outbound().get());
    CHECK(accept.get() == raw.Accept().get());

    // 空通道非阻塞接收是 kWouldBlock，而不是关闭。
    auto empty = inbound->TryRecv();
    CHECK(!empty.ok);
    CHECK(!empty.Closed());
    CHECK(empty.status == go2cpp::ChannelStatus::kWouldBlock);

    // 应用是 Outbound 的生产者：发送 → Poll → 客户端可读。
    RawFrame frame;
    frame.bytes = Bytes({1, 2, 3, 4});
    CHECK(outbound->TrySend(std::move(frame)).Ok());
    CHECK(server->PollOnce(Milliseconds{0}).events > 0);
    std::vector<std::uint8_t> read_back;
    CHECK(ReadBytes(*loop.client, 4, &read_back));
    CHECK(read_back == Bytes({1, 2, 3, 4}));

    // 连接句柄可复制，指向同一状态。
    RawConnection copy = loop.connection;
    CHECK(copy.valid());
    CHECK_EQ(copy.id(), loop.connection.id());
    CHECK(copy.Inbound().get() == inbound.get());

    // 应用 Close：C2 Inbound 关闭、C3 Outbound 拒绝发送；关闭原因终值可见。
    loop.connection.Close(CloseMode::kGraceful);
    CHECK(loop.connection.close_reason() == CloseReason::kLocalClose);
    CHECK(inbound->IsClosed());
    CHECK(outbound->IsClosed());
    auto send_after = outbound->TrySend(RawFrame{});
    CHECK(send_after.Closed());
    auto recv_after = inbound->TryRecv();
    CHECK(recv_after.Closed());
    CHECK_EQ(server->Connections(), std::size_t{0});
    // 幂等：二次 Close 不改变终值。
    loop.connection.Close(CloseMode::kAbort);
    CHECK(loop.connection.close_reason() == CloseReason::kLocalClose);
    // 无效句柄安全空转。
    RawConnection invalid;
    CHECK(!invalid.valid());
    CHECK(!invalid.id().valid());
    CHECK(invalid.Inbound() == nullptr);
    CHECK(invalid.Outbound() == nullptr);
    CHECK(invalid.close_reason() == CloseReason::kNone);
    CHECK_EQ(invalid.stats().messages_in, std::uint64_t{0});

    // 视图与句柄可以晚于 NetServer 析构：析构等价于优雅 Stop，终值仍可读。
    {
        auto server2 = CreateStarted(MemoryRawConfig());
        CHECK(server2 != nullptr);
        if (server2 != nullptr) {
            auto raw2 = server2->AsRawServer();
            Loopback loop2;
            CHECK(Connect(*server2, &loop2));
            server2.reset();
            CHECK(loop2.connection.close_reason() == CloseReason::kShutdown);
            CHECK(loop2.connection.Inbound()->IsClosed());
            CHECK(raw2.Accept()->IsClosed());
            // 服务器已析构，句柄上的 Close 仍然安全且幂等。
            loop2.connection.Close();
            CHECK(loop2.connection.close_reason() == CloseReason::kShutdown);
        }
    }
}

// ---------------------------------------------------------------------------
// 5. 多连接隔离
// ---------------------------------------------------------------------------

void TestMultipleConnections() {
    PrintCase("多连接：三路回环互不串数据、独立关闭不影响其它连接");

    auto server = CreateStarted(MemoryRawConfig());
    CHECK(server != nullptr);
    if (server == nullptr) {
        return;
    }
    constexpr std::size_t kConnections = 3;
    Loopback loops[kConnections];
    for (std::size_t index = 0; index < kConnections; ++index) {
        CHECK(Connect(*server, &loops[index]));
        CHECK(loops[index].connection.valid());
    }
    CHECK_EQ(server->Connections(), kConnections);
    CHECK_EQ(server->AsRawServer().Connections().size(), kConnections);
    // 连接 id 互不相同且都有效。
    for (std::size_t i = 0; i < kConnections; ++i) {
        CHECK(loops[i].connection.id().valid());
        for (std::size_t j = i + 1; j < kConnections; ++j) {
            CHECK(loops[i].connection.id() != loops[j].connection.id());
        }
    }

    // 每条连接写入带自身编号的载荷，一次 Poll 全部送达各自 Inbound。
    for (std::size_t index = 0; index < kConnections; ++index) {
        const std::vector<std::uint8_t> payload = {
            static_cast<std::uint8_t>(0x10U + index), 0x20, 0x30};
        CHECK(WriteBytes(*loops[index].client, payload));
    }
    CHECK(server->PollOnce(Milliseconds{0}).events > 0);
    for (std::size_t index = 0; index < kConnections; ++index) {
        std::vector<std::uint8_t> received;
        CHECK(TakeFrame(loops[index].connection.Inbound(), &received));
        const std::vector<std::uint8_t> expected = {
            static_cast<std::uint8_t>(0x10U + index), 0x20, 0x30};
        CHECK(received == expected);
    }

    // 关闭中间一条，其它连接仍可双向收发。
    loops[1].connection.Close(CloseMode::kGraceful);
    CHECK_EQ(server->Connections(), std::size_t{2});
    CHECK(loops[0].connection.close_reason() == CloseReason::kNone);
    CHECK(loops[2].connection.close_reason() == CloseReason::kNone);
    for (std::size_t index : {std::size_t{0}, std::size_t{2}}) {
        const std::vector<std::uint8_t> ping = {static_cast<std::uint8_t>(0xA0U + index)};
        CHECK(WriteBytes(*loops[index].client, ping));
        CHECK(server->PollOnce(Milliseconds{0}).events > 0);
        std::vector<std::uint8_t> received;
        CHECK(TakeFrame(loops[index].connection.Inbound(), &received));
        CHECK(received == ping);
        RawFrame pong;
        pong.bytes = {static_cast<std::uint8_t>(0xB0U + index)};
        CHECK(loops[index].connection.Outbound()->TrySend(std::move(pong)).Ok());
        CHECK(server->PollOnce(Milliseconds{0}).events > 0);
        std::vector<std::uint8_t> read_back;
        CHECK(ReadBytes(*loops[index].client, 1, &read_back));
        CHECK(read_back.size() == 1);
        CHECK(read_back[0] == static_cast<std::uint8_t>(0xB0U + index));
    }

    // 全部关闭后连接表清空。
    loops[0].connection.Close();
    loops[2].connection.Close();
    CHECK_EQ(server->Connections(), std::size_t{0});
}

// ---------------------------------------------------------------------------
// 6. 统计口径
// ---------------------------------------------------------------------------

void TestStats() {
    PrintCase("统计：server/connection 级计数与关闭分类");

    auto server = CreateStarted(MemoryRawConfig());
    CHECK(server != nullptr);
    if (server == nullptr) {
        return;
    }
    Loopback first;
    Loopback second;
    CHECK(Connect(*server, &first));
    CHECK(Connect(*server, &second));
    if (!first.client || !second.client) {
        return;
    }

    // 连接 1：两条入站消息（5 + 7 字节）。逐条写入/轮询，固定 M0 的读块边界。
    CHECK(WriteBytes(*first.client, Bytes({1, 2, 3, 4, 5})));
    std::vector<std::vector<std::uint8_t>> first_frames;
    CHECK(PollForFrames(*server, first.connection.Inbound(), 1, &first_frames));
    CHECK_EQ(first_frames.size(), std::size_t{1});
    if (!first_frames.empty()) {
        CHECK(first_frames.front() == Bytes({1, 2, 3, 4, 5}));
    }
    CHECK(WriteBytes(*first.client, Bytes({6, 7, 8, 9, 10, 11, 12})));
    CHECK(PollForFrames(*server, first.connection.Inbound(), 1, &first_frames));
    CHECK_EQ(first_frames.size(), std::size_t{1});
    if (!first_frames.empty()) {
        CHECK(first_frames.front() == Bytes({6, 7, 8, 9, 10, 11, 12}));
    }

    // 连接 2：一条入站（3 字节）+ 一条出站（4 字节）。
    CHECK(WriteBytes(*second.client, Bytes({0xAA, 0xBB, 0xCC})));
    CHECK(server->PollOnce(Milliseconds{0}).events > 0);
    std::vector<std::uint8_t> second_received;
    CHECK(TakeFrame(second.connection.Inbound(), &second_received));
    CHECK(second_received == Bytes({0xAA, 0xBB, 0xCC}));
    RawFrame outbound;
    outbound.bytes = Bytes({'p', 'o', 'n', 'g'});
    CHECK(second.connection.Outbound()->TrySend(std::move(outbound)).Ok());
    CHECK(server->PollOnce(Milliseconds{0}).events > 0);
    std::vector<std::uint8_t> client_read;
    CHECK(ReadBytes(*second.client, 4, &client_read));
    CHECK(client_read == Bytes({'p', 'o', 'n', 'g'}));

    // 一条优雅关闭、一条 abort 关闭。
    first.connection.Close(CloseMode::kGraceful);
    second.connection.Close(CloseMode::kAbort);

    const NetStats stats = server->Stats();
    CHECK_EQ(stats.accepted, std::uint64_t{2});
    CHECK_EQ(stats.active, std::uint64_t{0});
    CHECK_EQ(stats.closed_graceful, std::uint64_t{1});
    CHECK_EQ(stats.closed_abort, std::uint64_t{1});
    CHECK_EQ(stats.rejected_limit, std::uint64_t{0});
    CHECK_EQ(stats.messages_in, std::uint64_t{3});
    CHECK_EQ(stats.messages_out, std::uint64_t{1});
    CHECK_EQ(stats.bytes_in, std::uint64_t{15});   // 5 + 7 + 3
    CHECK_EQ(stats.bytes_out, std::uint64_t{4});   // "pong"
    CHECK_EQ(stats.raw_frames, std::uint64_t{3});  // M0：raw_frames 与入站帧同义
    CHECK_EQ(stats.dropped, std::uint64_t{0});

    const auto first_stats = first.connection.stats();
    CHECK_EQ(first_stats.messages_in, std::uint64_t{2});
    CHECK_EQ(first_stats.bytes_in, std::uint64_t{12});
    CHECK_EQ(first_stats.messages_out, std::uint64_t{0});
    CHECK_EQ(first_stats.errors, std::uint64_t{0});
    const auto second_stats = second.connection.stats();
    CHECK_EQ(second_stats.messages_in, std::uint64_t{1});
    CHECK_EQ(second_stats.messages_out, std::uint64_t{1});
    CHECK_EQ(second_stats.bytes_out, std::uint64_t{4});
}

// ---------------------------------------------------------------------------
// 7. 反压
// ---------------------------------------------------------------------------

void TestBackpressure() {
    PrintCase("反压：Inbound 满暂停读不丢帧；Outbound 满 TrySend 返回 kWouldBlock");

    OmniNetConfig config = MemoryRawConfig();
    config.limits.inbound_capacity = 1;
    config.limits.outbound_capacity = 1;
    config.limits.max_message_bytes = 64;
    config.limits.per_connection_budget_bytes = 128;
    config.limits.read_buffer_bytes = 64;
    auto server = CreateStarted(config);
    CHECK(server != nullptr);
    if (server == nullptr) {
        return;
    }
    Loopback loop;
    CHECK(Connect(*server, &loop));
    if (!loop.client || !loop.connection.valid()) {
        return;
    }
    const auto inbound = loop.connection.Inbound();

    // 第一条消息正常交付并让 Inbound 达到容量 1（不消费，保持满载）。
    CHECK(WriteBytes(*loop.client, Bytes({'1'})));
    CHECK(server->PollOnce(Milliseconds{0}).events > 0);
    CHECK_EQ(inbound->Len(), std::size_t{1});

    // 第二条已写入传输，但 Inbound 满 → 本轮暂停读，计一次反压，不丢数据。
    CHECK(WriteBytes(*loop.client, Bytes({'2'})));
    const auto paused = server->PollOnce(Milliseconds{0});
    CHECK(paused.ok());
    CHECK_EQ(paused.events, std::size_t{0});
    CHECK(paused.timeout);
    CHECK_EQ(inbound->Len(), std::size_t{1});
    CHECK(server->Stats().backpressure_events >= std::uint64_t{1});

    // 消费后才能继续读到第二条（不丢帧）。
    std::vector<std::uint8_t> first;
    CHECK(TakeFrame(inbound, &first));
    CHECK(first == Bytes({'1'}));
    std::vector<std::vector<std::uint8_t>> frames;
    CHECK(PollForFrames(*server, inbound, 1, &frames));
    CHECK_EQ(frames.size(), std::size_t{1});
    if (!frames.empty()) {
        CHECK(frames.front() == Bytes({'2'}));
    }
    CHECK_EQ(server->Stats().messages_in, std::uint64_t{2});
    CHECK_EQ(server->Stats().dropped, std::uint64_t{0});

    // Outbound 容量 1：第二个 TrySend 非阻塞失败（应用侧自行退避）。
    RawFrame one;
    one.bytes = Bytes({'x'});
    RawFrame two;
    two.bytes = Bytes({'y'});
    CHECK(loop.connection.Outbound()->TrySend(std::move(one)).Ok());
    const auto blocked = loop.connection.Outbound()->TrySend(std::move(two));
    CHECK(!blocked.Ok());
    CHECK(blocked.status == go2cpp::ChannelStatus::kWouldBlock);
    CHECK(server->PollOnce(Milliseconds{0}).events > 0);
    std::vector<std::uint8_t> read_back;
    CHECK(ReadBytes(*loop.client, 1, &read_back));
    CHECK(read_back == Bytes({'x'}));
    RawFrame three;
    three.bytes = Bytes({'z'});
    CHECK(loop.connection.Outbound()->TrySend(std::move(three)).Ok());
    CHECK(server->PollOnce(Milliseconds{0}).events > 0);
    CHECK(ReadBytes(*loop.client, 1, &read_back));
    CHECK(read_back == Bytes({'z'}));
}

// ---------------------------------------------------------------------------
// 8. 边界：超限帧与 abort 停止
// ---------------------------------------------------------------------------

void TestLimitsAndAbortStop() {
    PrintCase("边界：连接上限拒绝、超限出站帧 kLimitExceeded、Stop(kAbort) 关闭原因");

    // 连接上限：max_connections = 1，第二条接入必须被拒绝并计数。
    {
        OmniNetConfig config = MemoryRawConfig();
        config.limits.max_connections = 1;
        auto server = CreateStarted(config);
        CHECK(server != nullptr);
        if (server != nullptr) {
            Loopback first;
            CHECK(Connect(*server, &first));
            CHECK(first.client != nullptr);
            std::string error;
            CHECK(server->ConnectMemory(&error) == nullptr);
            CHECK(!error.empty());
            CHECK_EQ(server->Connections(), std::size_t{1});
            CHECK_EQ(server->Stats().rejected_limit, std::uint64_t{1});
            // 已建立的连接不受影响。
            CHECK(WriteBytes(*first.client, Bytes({0x42})));
            CHECK(server->PollOnce(Milliseconds{0}).events > 0);
            std::vector<std::uint8_t> received;
            CHECK(TakeFrame(first.connection.Inbound(), &received));
            CHECK(received == Bytes({0x42}));
        }
    }

    // 超限帧：max_message_bytes = 8，应用发 16 字节 → 连接被 abort。
    {
        OmniNetConfig config = MemoryRawConfig();
        config.limits.max_message_bytes = 8;
        config.limits.read_buffer_bytes = 8;
        config.limits.per_connection_budget_bytes = 16;
        auto server = CreateStarted(config);
        CHECK(server != nullptr);
        if (server != nullptr) {
            Loopback loop;
            CHECK(Connect(*server, &loop));
            if (loop.client && loop.connection.valid()) {
                RawFrame oversized;
                oversized.bytes.assign(16, std::uint8_t{0x5A});
                CHECK(loop.connection.Outbound()->TrySend(std::move(oversized)).Ok());
                (void)server->PollOnce(Milliseconds{0});
                CHECK(loop.connection.close_reason() == CloseReason::kLimitExceeded);
                CHECK(loop.connection.Inbound()->IsClosed());
                CHECK(loop.connection.Outbound()->IsClosed());
                CHECK_EQ(server->Stats().closed_abort, std::uint64_t{1});
                // 超限帧被拒绝时必须计 dropped，不能静默丢弃。
                CHECK_EQ(server->Stats().dropped, std::uint64_t{1});
                CHECK_EQ(loop.connection.stats().dropped, std::uint64_t{1});
                CHECK_EQ(server->Connections(), std::size_t{0});
            }
        }
    }

    // abort 停止：两条连接都以 kShutdown 关闭，计入 closed_abort。
    {
        auto server = CreateStarted(MemoryRawConfig());
        CHECK(server != nullptr);
        if (server != nullptr) {
            Loopback first;
            Loopback second;
            CHECK(Connect(*server, &first));
            CHECK(Connect(*server, &second));
            if (first.client && second.client) {
                server->Stop(CloseMode::kAbort);
                CHECK(first.connection.close_reason() == CloseReason::kShutdown);
                CHECK(second.connection.close_reason() == CloseReason::kShutdown);
                CHECK_EQ(server->Stats().closed_abort, std::uint64_t{2});
                CHECK_EQ(server->Stats().closed_graceful, std::uint64_t{0});
                CHECK_EQ(server->Stats().active, std::uint64_t{0});
                // abort 关闭后客户端读到 EOF。
                std::vector<std::uint8_t> buffer(1);
                const Result result = first.client->Read(buffer.data(), buffer.size(),
                                                        Milliseconds{100});
                CHECK_EQ(result.code, Code::kClosed);
            }
        }
    }
}

/// 测试收尾：输出 PASS/FAIL 并由 main 返回退出码。
int Finish() {
    if (g_failures != 0) {
        std::fprintf(stderr, "[FAIL] %d/%d checks failed\n", g_failures, g_checks);
        return 1;
    }
    std::printf("[PASS] test_omni_server (%d checks)\n", g_checks);
    return 0;
}

}  // namespace

int main() {
    TestConfigMatrix();
    TestStartStop();
    TestRawLoopback();
    TestChannelContracts();
    TestMultipleConnections();
    TestStats();
    TestBackpressure();
    TestLimitsAndAbortStop();
    return Finish();
}
