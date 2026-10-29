/**
 * @file audit_pairing_stress.cpp
 * @brief pairing 模块审查用压测/随机/场景程序（不注册进 CTest）。
 * @author go2cpp contributors
 * @date 2026-10-29
 * @copyright MIT License
 *
 * @details 与 `tests/bento/test_pairing.cpp` 的分工：
 *          - `test_pairing.cpp` 是注册进 CTest 的回归测试，只放确定性、毫秒级用例；
 *          - 本程序是**审查压测工具**，接受 scale / 随机种子 / 模式，覆盖千万级
 *            32B 帧编解码、百万级完整配对、1200 并发配对、随机帧/随机链路/
 *            随机状态机，并复核 15s 边界、1s 重传、幂等重放、key 不匹配与
 *            发送队列压力。
 *
 * 用法：
 * @code
 *   audit_pairing_stress 1.0                    # 全量（约 15s）
 *   audit_pairing_stress 1.0 --mode=bench       # 只跑吞吐
 *   audit_pairing_stress 1.0 --mode=scenarios   # 只跑真实场景
 *   audit_pairing_stress 1.0 --mode=random      # 随机帧/随机链路
 *   audit_pairing_stress 0.02 --mode=all        # ASan/UBSan 小规模
 *   audit_pairing_stress 0.01 --mode=leaks      # valgrind 小规模
 *   audit_pairing_stress 1.0 --seed=12345       # 换随机种子
 * @endcode
 *
 * 退出码：0 = 全部断言通过；1 = 有失败（失败点打印 CHECK 位置与数值）。
 */

#include <dirent.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <fstream>
#include <memory>
#include <random>
#include <string>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>

#include "go2cpp/bento/net/pairing.hpp"

namespace {

using go2cpp::bento::DecodePairingFrame;
using go2cpp::bento::EncodePairingFrame;
using go2cpp::bento::kPairingFrameBytes;
using go2cpp::bento::kPairingMaxOutbound;
using go2cpp::bento::PairingConfig;
using go2cpp::bento::PairingError;
using go2cpp::bento::PairingFrame;
using go2cpp::bento::PairingFrameType;
using go2cpp::bento::PairingSession;
using go2cpp::bento::PairingState;

using Clock = std::chrono::steady_clock;

int g_checks = 0;
int g_failures = 0;

#define CHECK(condition)                                                                \
    do {                                                                                \
        ++g_checks;                                                                     \
        if (!(condition)) {                                                             \
            std::fprintf(stderr, "[FAIL] %s:%d: %s\n", __FILE__, __LINE__, #condition); \
            ++g_failures;                                                               \
        }                                                                               \
    } while (0)

#define CHECK_EQ(actual, expected)                                                        \
    do {                                                                                  \
        ++g_checks;                                                                       \
        if (!((actual) == (expected))) {                                                  \
            std::fprintf(stderr, "[FAIL] %s:%d: %s == %s\n", __FILE__, __LINE__, #actual, \
                         #expected);                                                      \
            ++g_failures;                                                                 \
        }                                                                                 \
    } while (0)

/// 防止编译器把压测循环整个优化掉。
volatile std::uint64_t g_sink = 0;

struct Config {
    std::string mode = "all";
    double scale = 1.0;
    std::uint32_t seed = 20261029;
    int long_seconds = 0;
};

std::uint64_t NowMs() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch())
            .count());
}

/// 读取 /proc/self/status 里的 VmRSS/VmHWM（KB）；失败返回 -1。
long ProcStatusKb(const char* key) {
    std::ifstream status("/proc/self/status");
    if (!status) {
        return -1;
    }
    const std::size_t key_len = std::strlen(key);
    std::string line;
    while (std::getline(status, line)) {
        if (line.compare(0, key_len, key) == 0) {
            long value = -1;
            if (std::sscanf(line.c_str() + key_len, " %ld kB", &value) == 1) {
                return value;
            }
            return -1;
        }
    }
    return -1;
}

int FdCount() {
    DIR* dir = ::opendir("/proc/self/fd");
    if (dir == nullptr) {
        return -1;
    }
    int count = 0;
    while (::readdir(dir) != nullptr) {
        ++count;
    }
    ::closedir(dir);
    return count;
}

PairingConfig MakeConfig(std::uint64_t nonce = 0, std::uint32_t timeout_ms = 15000,
                         std::uint32_t retransmit_ms = 1000) {
    PairingConfig config;
    config.nonce = nonce;
    config.timeout_ms = timeout_ms;
    config.retransmit_ms = retransmit_ms;
    return config;
}

std::vector<PairingFrame> Drain(PairingSession& session) {
    std::vector<PairingFrame> frames;
    PairingFrame frame;
    while (session.PopOutbound(&frame)) {
        frames.push_back(frame);
    }
    return frames;
}

/// 无 vector 分配的完整配对（Register → Connect → Response → CONFIRM → ACK）。
bool PairUntilActiveFast(PairingSession& server, PairingSession& client) {
    if (server.Register("k", 0) != PairingError::kNone) {
        return false;
    }
    if (client.Connect("k", 0) != PairingError::kNone) {
        return false;
    }
    PairingFrame frame;
    if (!client.PopOutbound(&frame) || server.HandleFrame(frame, 0) != PairingError::kNone) {
        return false;
    }
    if (server.Response(true, 0) != PairingError::kNone) {
        return false;
    }
    if (!server.PopOutbound(&frame) || client.HandleFrame(frame, 0) != PairingError::kNone) {
        return false;
    }
    if (client.Activate(0) != PairingError::kNone) {
        return false;
    }
    if (!client.PopOutbound(&frame) || server.HandleFrame(frame, 0) != PairingError::kNone) {
        return false;
    }
    if (!server.PopOutbound(&frame) || client.HandleFrame(frame, 0) != PairingError::kNone) {
        return false;
    }
    return server.state() == PairingState::kActive && client.state() == PairingState::kActive;
}

bool PairUntilPaired(PairingSession& server, PairingSession& client) {
    if (server.Register("k", 0) != PairingError::kNone) {
        return false;
    }
    if (client.Connect("k", 0) != PairingError::kNone) {
        return false;
    }
    PairingFrame frame;
    if (!client.PopOutbound(&frame) || server.HandleFrame(frame, 0) != PairingError::kNone) {
        return false;
    }
    if (server.Response(true, 0) != PairingError::kNone) {
        return false;
    }
    if (!server.PopOutbound(&frame) || client.HandleFrame(frame, 0) != PairingError::kNone) {
        return false;
    }
    return server.state() == PairingState::kPaired && client.state() == PairingState::kPaired;
}

bool IsKnownState(PairingState state) {
    switch (state) {
        case PairingState::kIdle:
        case PairingState::kPending:
        case PairingState::kPaired:
        case PairingState::kActive:
        case PairingState::kClosed:
            return true;
    }
    return false;
}

bool IsKnownFrameType(PairingFrameType type) {
    switch (type) {
        case PairingFrameType::kRegister:
        case PairingFrameType::kConnect:
        case PairingFrameType::kResponse:
        case PairingFrameType::kConfirm:
            return true;
    }
    return false;
}

std::uint64_t Percentile(std::vector<std::uint64_t> samples, double ratio) {
    if (samples.empty()) {
        return 0;
    }
    std::sort(samples.begin(), samples.end());
    const double index = ratio * static_cast<double>(samples.size() - 1);
    const std::size_t offset = static_cast<std::size_t>(index + 0.5);
    return samples[std::min(offset, samples.size() - 1)];
}

// ---------------------------------------------------------------------------
// 吞吐压测
// ---------------------------------------------------------------------------

void PrintRate(const char* name, std::size_t count, double total_ns, double bytes_per_op) {
    const double per_op = total_ns / static_cast<double>(count);
    const double mops = 1.0e3 / per_op;
    const double mbps = bytes_per_op * 1.0e3 / per_op;
    std::printf("[BENCH] %-16s count=%zu ns_op=%.2f Mops=%.1f MBps=%.1f\n", name, count, per_op,
                mops, mbps);
}

void BenchCodec(const Config& config) {
    const std::size_t count = static_cast<std::size_t>(20'000'000.0 * config.scale);
    if (count == 0) {
        return;
    }
    PairingFrame frame;
    frame.type = PairingFrameType::kResponse;
    frame.key_hash = 0x0123456789abcdefULL;
    frame.nonce = 0xfedcba9876543210ULL;
    frame.pair_id = 0xdeadbeefU;
    frame.status = PairingError::kKeyMismatch;

    std::uint8_t buffer[kPairingFrameBytes] = {};
    EncodePairingFrame(frame, buffer);

    // 预热：避免首次分支预测/缓存冷启动污染数据。
    std::uint64_t warm = 0;
    for (int index = 0; index < 10000; ++index) {
        EncodePairingFrame(frame, buffer);
        warm += buffer[static_cast<std::size_t>(index) & 31U];
    }
    g_sink += warm;

    auto begin = Clock::now();
    std::uint64_t sum = 0;
    for (std::size_t index = 0; index < count; index += 8) {
        for (int repeat = 0; repeat < 8; ++repeat) {
            EncodePairingFrame(frame, buffer);
        }
        sum += buffer[index & 31U];
    }
    auto end = Clock::now();
    g_sink += sum;
    PrintRate("encode", count, std::chrono::duration<double, std::nano>(end - begin).count(),
              static_cast<double>(kPairingFrameBytes));

    PairingFrame decoded;
    begin = Clock::now();
    sum = 0;
    for (std::size_t index = 0; index < count; ++index) {
        if (!DecodePairingFrame(buffer, kPairingFrameBytes, &decoded)) {
            ++g_failures;
            break;
        }
        sum += decoded.pair_id;
    }
    end = Clock::now();
    g_sink += sum;
    PrintRate("decode", count, std::chrono::duration<double, std::nano>(end - begin).count(),
              static_cast<double>(kPairingFrameBytes));

    begin = Clock::now();
    std::size_t roundtrip_ok = 0;
    for (std::size_t index = 0; index < count; ++index) {
        EncodePairingFrame(frame, buffer);
        if (DecodePairingFrame(buffer, kPairingFrameBytes, &decoded) && decoded.pair_id == 0xdeadbeefU) {
            ++roundtrip_ok;
        }
    }
    end = Clock::now();
    PrintRate("roundtrip", count, std::chrono::duration<double, std::nano>(end - begin).count(),
              2.0 * static_cast<double>(kPairingFrameBytes));
    CHECK_EQ(roundtrip_ok, count);

    // 非法帧（magic 坏）走最早退出的拒绝路径，用来量化校验成本。
    std::uint8_t bad[kPairingFrameBytes];
    std::memcpy(bad, buffer, sizeof(bad));
    bad[0] = 0;
    begin = Clock::now();
    std::size_t rejected = 0;
    for (std::size_t index = 0; index < count; ++index) {
        if (!DecodePairingFrame(bad, kPairingFrameBytes, &decoded)) {
            ++rejected;
        }
    }
    end = Clock::now();
    PrintRate("decode_reject", count,
              std::chrono::duration<double, std::nano>(end - begin).count(),
              static_cast<double>(kPairingFrameBytes));
    CHECK_EQ(rejected, count);
}

void BenchPairing(const Config& config) {
    const std::size_t count = static_cast<std::size_t>(2'000'000.0 * config.scale);
    if (count == 0) {
        return;
    }
    std::vector<std::uint64_t> samples;
    samples.reserve(std::min<std::size_t>(count, 100000));
    std::size_t ok = 0;
    auto begin = Clock::now();
    for (std::size_t index = 0; index < count; ++index) {
        const auto start = Clock::now();
        PairingSession server(MakeConfig(index + 1));
        PairingSession client(MakeConfig(0x01000000ULL + index));
        if (PairUntilActiveFast(server, client)) {
            ++ok;
        }
        const auto finish = Clock::now();
        if (samples.size() < samples.capacity()) {
            samples.push_back(static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(finish - start).count()));
        }
    }
    auto end = Clock::now();
    const double total_ns = std::chrono::duration<double, std::nano>(end - begin).count();
    const double per_pair = total_ns / static_cast<double>(count);
    std::printf(
        "[BENCH] %-16s count=%zu ns_pair=%.1f pairs_s=%.0f p50_us=%.2f p95_us=%.2f p99_us=%.2f\n",
        "full_pairing", count, per_pair, 1.0e9 / per_pair,
        static_cast<double>(Percentile(samples, 0.50)) / 1000.0,
        static_cast<double>(Percentile(samples, 0.95)) / 1000.0,
        static_cast<double>(Percentile(samples, 0.99)) / 1000.0);
    CHECK_EQ(ok, count);
}

void BenchConstruct(const Config& config) {
    const std::size_t count = static_cast<std::size_t>(5'000'000.0 * config.scale);
    if (count == 0) {
        return;
    }
    auto begin = Clock::now();
    std::size_t idle = 0;
    for (std::size_t index = 0; index < count; ++index) {
        PairingSession session(MakeConfig(index + 1));
        if (session.state() == PairingState::kIdle && session.outbound_size() == 0) {
            ++idle;
        }
    }
    auto end = Clock::now();
    PrintRate("construct", count, std::chrono::duration<double, std::nano>(end - begin).count(),
              0.0);
    CHECK_EQ(idle, count);
}

void BenchHash(const Config& config) {
    const std::size_t count = static_cast<std::size_t>(10'000'000.0 * config.scale);
    if (count == 0) {
        return;
    }
    const std::string key = "invite-key-0123456789abcdef";
    auto begin = Clock::now();
    std::uint64_t sum = 0;
    for (std::size_t index = 0; index < count; ++index) {
        sum += go2cpp::bento::FixedKeyAuthPolicy::Hash(key);
    }
    auto end = Clock::now();
    g_sink += sum;
    PrintRate("key_hash", count, std::chrono::duration<double, std::nano>(end - begin).count(),
              static_cast<double>(key.size()));
}

void BenchHandleFrame(const Config& config) {
    const std::size_t count = static_cast<std::size_t>(10'000'000.0 * config.scale);
    if (count == 0) {
        return;
    }
    PairingSession server(MakeConfig());
    PairingSession client(MakeConfig(0x12345678ULL));
    if (!PairUntilPaired(server, client)) {
        CHECK(false);
        return;
    }
    PairingFrame connect;
    connect.type = PairingFrameType::kConnect;
    connect.key_hash = server.key_hash();
    connect.nonce = client.nonce();

    auto begin = Clock::now();
    std::size_t replies = 0;
    for (std::size_t index = 0; index < count; ++index) {
        connect.pair_id = static_cast<std::uint32_t>(index);
        (void)server.HandleFrame(connect, 0);
        PairingFrame out;
        while (server.PopOutbound(&out)) {
            ++replies;
        }
    }
    auto end = Clock::now();
    PrintRate("handle_frame", count, std::chrono::duration<double, std::nano>(end - begin).count(),
              static_cast<double>(kPairingFrameBytes));
    CHECK_EQ(replies, count);  // 每条重复 CONNECT 都重放一帧 RESPONSE
}

// ---------------------------------------------------------------------------
// 场景 1：15s 超时边界（假时钟）
// ---------------------------------------------------------------------------

void TestTimeoutBoundary() {
    // 服务端 Register@1000 → deadline 16000；15999 不超时，16000 超时。
    PairingSession server(MakeConfig());
    CHECK_EQ(server.Register("k", 1000), PairingError::kNone);
    CHECK_EQ(server.deadline_ms(), 16000U);
    CHECK(!server.Tick(15999));
    CHECK_EQ(server.state(), PairingState::kPending);
    CHECK(server.Tick(16000));
    CHECK_EQ(server.state(), PairingState::kClosed);
    CHECK_EQ(server.error(), PairingError::kPeerTimeout);
    CHECK_EQ(server.stats().timeouts, 1U);
    CHECK(!server.Tick(16000));  // 已 Closed，不应重复计超时
    CHECK_EQ(server.stats().timeouts, 1U);

    // 客户端 Pending 同样在 deadline 边界关闭。
    PairingSession client(MakeConfig(0x1111ULL));
    CHECK_EQ(client.Connect("k", 15000), PairingError::kNone);
    CHECK_EQ(client.deadline_ms(), 30000U);
    CHECK(!client.Tick(29999));
    CHECK_EQ(client.state(), PairingState::kPending);
    CHECK(client.Tick(30000));
    CHECK_EQ(client.state(), PairingState::kClosed);
    CHECK_EQ(client.error(), PairingError::kPeerTimeout);

    // Paired 阶段收到 RESPONSE 后重新计时：14900 收到 → deadline 29900。
    PairingSession late_server(MakeConfig());
    PairingSession late_client(MakeConfig(0x2222ULL));
    CHECK_EQ(late_server.Register("k", 0), PairingError::kNone);
    CHECK_EQ(late_client.Connect("k", 0), PairingError::kNone);
    auto connect = Drain(late_client);
    CHECK_EQ(connect.size(), 1U);
    CHECK_EQ(late_server.HandleFrame(connect[0], 14900), PairingError::kNone);
    CHECK_EQ(late_server.Response(true, 14900), PairingError::kNone);
    CHECK_EQ(late_server.deadline_ms(), 29900U);
    auto response = Drain(late_server);
    CHECK_EQ(late_client.HandleFrame(response[0], 14900), PairingError::kNone);
    CHECK_EQ(late_client.state(), PairingState::kPaired);
    CHECK_EQ(late_client.deadline_ms(), 29900U);
    CHECK(!late_client.Tick(29899));
    CHECK(late_client.Tick(29900));
    CHECK_EQ(late_client.state(), PairingState::kClosed);

    // 拒绝后服务端仍 Pending，deadline 刷新，等待下一个客户端。
    PairingSession reject_server(MakeConfig());
    PairingSession reject_client(MakeConfig(0x3333ULL));
    CHECK_EQ(reject_server.Register("k", 0), PairingError::kNone);
    CHECK_EQ(reject_client.Connect("k", 0), PairingError::kNone);
    auto reject_connect = Drain(reject_client);
    CHECK_EQ(reject_server.HandleFrame(reject_connect[0], 0), PairingError::kNone);
    CHECK_EQ(reject_server.Response(false, 5000), PairingError::kNone);
    CHECK_EQ(reject_server.state(), PairingState::kPending);
    CHECK_EQ(reject_server.deadline_ms(), 20000U);
    CHECK(!reject_server.Tick(19999));
    CHECK(reject_server.Tick(20000));
    CHECK_EQ(reject_server.state(), PairingState::kClosed);

    // Active 状态不因时间流逝关闭（15s 语义只约束 Pending/Paired）。
    PairingSession active_server(MakeConfig());
    PairingSession active_client(MakeConfig(0x4444ULL));
    CHECK(PairUntilActiveFast(active_server, active_client));
    CHECK(!active_server.Tick(100000000ULL));
    CHECK(!active_client.Tick(100000000ULL));
    CHECK_EQ(active_server.state(), PairingState::kActive);
    CHECK_EQ(active_client.state(), PairingState::kActive);
}

// ---------------------------------------------------------------------------
// 场景 2：1s 重传节奏（RESPONSE / CONFIRM / ACK 丢失）
// ---------------------------------------------------------------------------

void TestRetransmitSchedule() {
    // 客户端 Pending：CONNECT 每 1000ms 重发一次，999 不重发。
    PairingSession client(MakeConfig(0x1111ULL, 15000, 1000));
    CHECK_EQ(client.Connect("k", 0), PairingError::kNone);
    PairingFrame frame;
    CHECK(client.PopOutbound(&frame));
    CHECK_EQ(frame.type, PairingFrameType::kConnect);
    CHECK(!client.Tick(999));
    CHECK_EQ(client.outbound_size(), 0U);
    CHECK(!client.Tick(1000));
    CHECK_EQ(client.outbound_size(), 1U);
    CHECK(client.PopOutbound(&frame));
    CHECK_EQ(frame.type, PairingFrameType::kConnect);
    CHECK(!client.Tick(1999));
    CHECK_EQ(client.outbound_size(), 0U);
    CHECK(!client.Tick(2000));
    CHECK_EQ(client.outbound_size(), 1U);
    CHECK_EQ(client.stats().retransmits, 2U);
    CHECK_EQ(client.stats().frames_sent, 3U);  // 首发 + 2 次重传

    // RESPONSE 丢失：服务端 Paired 每 1000ms 重发 RESPONSE，客户端收到后进入 Paired。
    PairingSession server(MakeConfig());
    PairingSession lost_client(MakeConfig(0x2222ULL, 15000, 1000));
    CHECK_EQ(server.Register("k", 0), PairingError::kNone);
    CHECK_EQ(lost_client.Connect("k", 0), PairingError::kNone);
    auto connect = Drain(lost_client);
    CHECK_EQ(server.HandleFrame(connect[0], 0), PairingError::kNone);
    CHECK_EQ(server.Response(true, 0), PairingError::kNone);
    auto lost_response = Drain(server);  // 模拟 RESPONSE 丢失
    CHECK_EQ(lost_response.size(), 1U);
    CHECK(!server.Tick(999));
    CHECK_EQ(server.outbound_size(), 0U);
    CHECK(!server.Tick(1000));
    CHECK_EQ(server.outbound_size(), 1U);
    auto retry_response = Drain(server);
    CHECK_EQ(retry_response.size(), 1U);
    CHECK_EQ(retry_response[0].type, PairingFrameType::kResponse);
    CHECK_EQ(retry_response[0].pair_id, server.pair_id());
    CHECK_EQ(lost_client.HandleFrame(retry_response[0], 1000), PairingError::kNone);
    CHECK_EQ(lost_client.state(), PairingState::kPaired);
    CHECK_EQ(lost_client.pair_id(), server.pair_id());

    // CONFIRM 丢失：客户端 Paired 且已 Activate，每 1000ms 重发 CONFIRM。
    PairingSession confirm_server(MakeConfig());
    PairingSession confirm_client(MakeConfig(0x3333ULL, 15000, 1000));
    CHECK(PairUntilPaired(confirm_server, confirm_client));
    CHECK_EQ(confirm_client.Activate(0), PairingError::kNone);
    auto confirm = Drain(confirm_client);  // 模拟 CONFIRM 丢失
    CHECK_EQ(confirm.size(), 1U);
    CHECK(!confirm_client.Tick(1000));
    auto confirm_retry = Drain(confirm_client);
    CHECK_EQ(confirm_retry.size(), 1U);
    CHECK_EQ(confirm_retry[0].type, PairingFrameType::kConfirm);

    // ACK 丢失：服务端 Active 后重放 ACK，客户端收到后 Active。
    CHECK_EQ(confirm_server.HandleFrame(confirm_retry[0], 1000), PairingError::kNone);
    CHECK_EQ(confirm_server.state(), PairingState::kActive);
    auto ack = Drain(confirm_server);  // 模拟 ACK 丢失
    CHECK_EQ(ack.size(), 1U);
    CHECK(!confirm_client.Tick(2000));
    auto confirm_again = Drain(confirm_client);
    CHECK_EQ(confirm_again.size(), 1U);
    CHECK_EQ(confirm_server.HandleFrame(confirm_again[0], 2000), PairingError::kNone);
    CHECK_EQ(confirm_server.stats().replays, 1U);
    auto ack_again = Drain(confirm_server);
    CHECK_EQ(ack_again.size(), 1U);
    CHECK_EQ(confirm_client.HandleFrame(ack_again[0], 2000), PairingError::kNone);
    CHECK_EQ(confirm_client.state(), PairingState::kActive);
}

// ---------------------------------------------------------------------------
// 场景 3：幂等重放（重复 register/connect/response/confirm）
// ---------------------------------------------------------------------------

void TestIdempotentReplay() {
    // 重复 Register / Connect。
    PairingSession server(MakeConfig());
    CHECK_EQ(server.Register("k", 0), PairingError::kNone);
    for (int repeat = 0; repeat < 3; ++repeat) {
        CHECK_EQ(server.Register("k", 0), PairingError::kAlreadyRegistered);
        CHECK_EQ(server.Register("other", 0), PairingError::kAlreadyRegistered);
    }
    CHECK_EQ(server.state(), PairingState::kPending);
    CHECK_EQ(server.stats().registers, 1U);

    PairingSession client(MakeConfig(0x5555ULL));
    CHECK_EQ(client.Connect("k", 0), PairingError::kNone);
    for (int repeat = 0; repeat < 3; ++repeat) {
        CHECK_EQ(client.Connect("k", 0), PairingError::kAlreadyRegistered);
    }
    CHECK_EQ(client.stats().connects, 1U);

    // 服务端 Paired 后同一 nonce 的重复 CONNECT：重放同一 pair_id 的 RESPONSE。
    auto connect = Drain(client);
    CHECK_EQ(server.HandleFrame(connect[0], 0), PairingError::kNone);
    CHECK_EQ(server.Response(true, 0), PairingError::kNone);
    const std::uint32_t pair_id = server.pair_id();
    auto response = Drain(server);
    CHECK_EQ(client.HandleFrame(response[0], 0), PairingError::kNone);
    for (int repeat = 0; repeat < 3; ++repeat) {
        CHECK_EQ(server.HandleFrame(connect[0], 0), PairingError::kNone);
        auto replay = Drain(server);
        CHECK_EQ(replay.size(), 1U);
        CHECK_EQ(replay[0].pair_id, pair_id);
        CHECK_EQ(replay[0].nonce, connect[0].nonce);
    }
    CHECK_EQ(server.stats().replays, 3U);
    CHECK_EQ(server.pair_id(), pair_id);

    // 重复 Response(true) 被拒绝，状态不变。
    CHECK_EQ(server.Response(true, 0), PairingError::kInvalidState);
    CHECK_EQ(server.state(), PairingState::kPaired);

    // 客户端重复 Activate 不重复入队 CONFIRM。
    CHECK_EQ(client.Activate(0), PairingError::kNone);
    CHECK_EQ(client.Activate(0), PairingError::kNone);
    auto confirm = Drain(client);
    CHECK_EQ(confirm.size(), 1U);

    // 客户端 Paired 且已发 CONFIRM 时收到重复 RESPONSE：重发 CONFIRM。
    CHECK_EQ(client.HandleFrame(response[0], 0), PairingError::kNone);
    CHECK_EQ(client.outbound_size(), 1U);
    (void)Drain(client);

    // 服务端 Active 后重复 CONFIRM：每次重放 ACK。
    CHECK_EQ(server.HandleFrame(confirm[0], 0), PairingError::kNone);
    CHECK_EQ(server.state(), PairingState::kActive);
    (void)Drain(server);
    for (int repeat = 0; repeat < 3; ++repeat) {
        CHECK_EQ(server.HandleFrame(confirm[0], 0), PairingError::kNone);
        auto ack = Drain(server);
        CHECK_EQ(ack.size(), 1U);
        CHECK_EQ(ack[0].type, PairingFrameType::kConfirm);
        CHECK_EQ(ack[0].pair_id, pair_id);
    }

    // 客户端 Active 后重复 RESPONSE / ACK 全部忽略，不入队。
    auto ack = Drain(server);
    CHECK_EQ(ack.size(), 0U);
    CHECK_EQ(client.state(), PairingState::kPaired);
    PairingFrame server_ack;
    server_ack.type = PairingFrameType::kConfirm;
    server_ack.key_hash = client.key_hash();
    server_ack.nonce = client.nonce();
    server_ack.pair_id = pair_id;
    CHECK_EQ(client.HandleFrame(server_ack, 0), PairingError::kNone);
    CHECK_EQ(client.state(), PairingState::kActive);
    CHECK_EQ(client.outbound_size(), 0U);
    for (int repeat = 0; repeat < 3; ++repeat) {
        CHECK_EQ(client.HandleFrame(server_ack, 0), PairingError::kNone);
        CHECK_EQ(client.HandleFrame(response[0], 0), PairingError::kNone);
    }
    CHECK_EQ(client.outbound_size(), 0U);
    CHECK_EQ(client.state(), PairingState::kActive);
}

// ---------------------------------------------------------------------------
// 场景 4：key 不匹配拒绝（各状态矩阵）
// ---------------------------------------------------------------------------

void TestKeyMismatchMatrix() {
    // 服务端 Pending：错 key 的 CONNECT 被拒，服务端状态与 deadline 不变。
    PairingSession server(MakeConfig());
    CHECK_EQ(server.Register("alpha", 0), PairingError::kNone);
    const std::uint64_t registered_deadline = server.deadline_ms();
    PairingSession wrong(MakeConfig(0x6666ULL));
    CHECK_EQ(wrong.Connect("beta", 0), PairingError::kNone);
    auto wrong_connect = Drain(wrong);
    CHECK_EQ(wrong_connect.size(), 1U);
    CHECK_EQ(server.HandleFrame(wrong_connect[0], 0), PairingError::kKeyMismatch);
    CHECK_EQ(server.state(), PairingState::kPending);
    CHECK(!server.HasPendingConnect());
    CHECK_EQ(server.deadline_ms(), registered_deadline);
    CHECK_EQ(server.stats().key_mismatches, 1U);
    auto reject = Drain(server);
    CHECK_EQ(reject.size(), 1U);
    CHECK_EQ(reject[0].status, PairingError::kKeyMismatch);
    CHECK_EQ(wrong.HandleFrame(reject[0], 0), PairingError::kKeyMismatch);
    CHECK_EQ(wrong.state(), PairingState::kClosed);
    CHECK_EQ(wrong.error(), PairingError::kKeyMismatch);

    // 服务端 Paired：错 key 的 CONNECT（即使 nonce 相同）仍按 key 优先拒绝。
    PairingSession paired_server(MakeConfig());
    PairingSession paired_client(MakeConfig(0x7777ULL));
    CHECK(PairUntilPaired(paired_server, paired_client));
    const std::uint32_t paired_id = paired_server.pair_id();
    PairingFrame forged = wrong_connect[0];
    forged.key_hash = paired_client.key_hash() ^ 1ULL;
    forged.nonce = paired_server.nonce();
    CHECK_EQ(paired_server.HandleFrame(forged, 0), PairingError::kKeyMismatch);
    CHECK_EQ(paired_server.state(), PairingState::kPaired);
    CHECK_EQ(paired_server.pair_id(), paired_id);

    // 服务端 Paired：CONFIRM 的 pair_id 不符 → kKeyMismatch，状态不变。
    PairingFrame bad_confirm;
    bad_confirm.type = PairingFrameType::kConfirm;
    bad_confirm.key_hash = paired_server.key_hash();
    bad_confirm.nonce = paired_server.nonce();
    bad_confirm.pair_id = paired_id ^ 0xffffU;
    CHECK_EQ(paired_server.HandleFrame(bad_confirm, 0), PairingError::kKeyMismatch);
    CHECK_EQ(paired_server.state(), PairingState::kPaired);

    // 客户端 Paired：ACK 的 key_hash 不符 → kKeyMismatch，状态不变。
    CHECK_EQ(paired_client.Activate(0), PairingError::kNone);
    (void)Drain(paired_client);
    PairingFrame bad_ack = bad_confirm;
    bad_ack.key_hash = paired_client.key_hash() ^ 1ULL;
    bad_ack.pair_id = paired_client.pair_id();
    CHECK_EQ(paired_client.HandleFrame(bad_ack, 0), PairingError::kKeyMismatch);
    CHECK_EQ(paired_client.state(), PairingState::kPaired);

    // 客户端 Pending：nonce 匹配但 status=kKeyMismatch 的响应 → 关闭并记录。
    PairingSession mismatch_client(MakeConfig(0x8888ULL));
    CHECK_EQ(mismatch_client.Connect("alpha", 0), PairingError::kNone);
    (void)Drain(mismatch_client);
    PairingFrame mismatch_response;
    mismatch_response.type = PairingFrameType::kResponse;
    mismatch_response.key_hash = mismatch_client.key_hash() ^ 1ULL;
    mismatch_response.nonce = mismatch_client.nonce();
    mismatch_response.pair_id = 0;
    mismatch_response.status = PairingError::kKeyMismatch;
    CHECK_EQ(mismatch_client.HandleFrame(mismatch_response, 0), PairingError::kKeyMismatch);
    CHECK_EQ(mismatch_client.state(), PairingState::kClosed);
    CHECK_EQ(mismatch_client.stats().key_mismatches, 1U);
}

// ---------------------------------------------------------------------------
// 场景 5：1000 并发注册 / 配对
// ---------------------------------------------------------------------------

void TestConcurrentRegistration(const Config& config) {
    const std::size_t count = std::max<std::size_t>(
        1000, static_cast<std::size_t>(1000.0 * config.scale));
    const std::uint64_t rss_before = static_cast<std::uint64_t>(std::max<long>(0, ProcStatusKb("VmRSS:")));

    // 1000 个服务端同时处于 Pending，各自持有不同邀请密钥。
    std::vector<std::unique_ptr<PairingSession>> servers;
    servers.reserve(count);
    for (std::size_t index = 0; index < count; ++index) {
        servers.push_back(std::make_unique<PairingSession>(MakeConfig()));
        CHECK_EQ(servers.back()->Register("key-" + std::to_string(index), 0), PairingError::kNone);
        CHECK_EQ(servers.back()->state(), PairingState::kPending);
    }
    const std::uint64_t rss_live = static_cast<std::uint64_t>(std::max<long>(0, ProcStatusKb("VmRSS:")));

    // 每个 key 一个客户端，逐一完成配对；1000 个 key 全部成功。
    std::size_t active = 0;
    for (std::size_t index = 0; index < count; ++index) {
        PairingSession client(MakeConfig(0x100000ULL + index));
        if (client.Connect("key-" + std::to_string(index), 0) != PairingError::kNone) {
            continue;
        }
        PairingFrame frame;
        if (!client.PopOutbound(&frame) ||
            servers[index]->HandleFrame(frame, 0) != PairingError::kNone) {
            continue;
        }
        if (servers[index]->Response(true, 0) != PairingError::kNone) {
            continue;
        }
        if (!servers[index]->PopOutbound(&frame) ||
            client.HandleFrame(frame, 0) != PairingError::kNone) {
            continue;
        }
        if (client.Activate(0) != PairingError::kNone) {
            continue;
        }
        if (!client.PopOutbound(&frame) ||
            servers[index]->HandleFrame(frame, 0) != PairingError::kNone) {
            continue;
        }
        if (!servers[index]->PopOutbound(&frame) ||
            client.HandleFrame(frame, 0) != PairingError::kNone) {
            continue;
        }
        if (client.state() == PairingState::kActive &&
            servers[index]->state() == PairingState::kActive) {
            ++active;
        }
    }
    CHECK_EQ(active, count);
    CHECK(servers.size() == count);

    const long rss_after = ProcStatusKb("VmRSS:");
    std::printf(
        "[MEM] concurrent_register sessions=%zu rss_before=%lluKB rss_live=%lluKB delta=%ldKB\n",
        count, static_cast<unsigned long long>(rss_before),
        static_cast<unsigned long long>(rss_live), rss_after - static_cast<long>(rss_before));
    CHECK(rss_live - rss_before < 64ULL * 1024ULL);  // 1000 会话 < 64MB

    // 多线程并发：8 线程 × 150 个独立配对，nonce 全局唯一。
    const unsigned thread_count = 8;
    const std::size_t per_thread = 150;
    std::vector<std::thread> threads;
    threads.reserve(thread_count);
    std::vector<std::vector<std::uint64_t>> nonces(thread_count);
    std::atomic<std::size_t> failures{0};
    for (unsigned thread_index = 0; thread_index < thread_count; ++thread_index) {
        threads.emplace_back([&, thread_index]() {
            nonces[thread_index].reserve(per_thread);
            for (std::size_t index = 0; index < per_thread; ++index) {
                PairingSession server(MakeConfig());
                PairingSession client(MakeConfig());
                if (!PairUntilActiveFast(server, client)) {
                    ++failures;
                    continue;
                }
                nonces[thread_index].push_back(client.nonce());
            }
        });
    }
    for (std::thread& thread : threads) {
        thread.join();
    }
    CHECK_EQ(failures.load(), 0U);
    std::unordered_set<std::uint64_t> unique;
    for (const auto& bucket : nonces) {
        for (const std::uint64_t nonce : bucket) {
            CHECK(nonce != 0);
            unique.insert(nonce);
        }
    }
    CHECK_EQ(unique.size(), thread_count * per_thread);
}

// ---------------------------------------------------------------------------
// 场景 6：发送队列压力（应用不取帧）
// ---------------------------------------------------------------------------

void TestQueuePressure() {
    PairingSession client(MakeConfig(0x9999ULL, 15000, 1));  // 1ms 重传
    CHECK_EQ(client.Connect("k", 0), PairingError::kNone);
    std::uint64_t now = 0;
    for (int step = 0; step < 10000; ++step) {
        now += 1;
        CHECK(!client.Tick(now));
        CHECK(client.outbound_size() <= kPairingMaxOutbound);
    }
    CHECK_EQ(client.outbound_size(), kPairingMaxOutbound);
    CHECK_EQ(client.stats().queue_drops, 10001U - kPairingMaxOutbound);  // 首发 + 10000 重传
    PairingFrame frame;
    std::size_t popped = 0;
    while (client.PopOutbound(&frame)) {
        ++popped;
        CHECK_EQ(frame.type, PairingFrameType::kConnect);
        CHECK_EQ(frame.nonce, client.nonce());
    }
    CHECK_EQ(popped, kPairingMaxOutbound);
    CHECK_EQ(client.outbound_size(), 0U);

    // 超时清空队列：Closed 后不得把陈旧帧发给下一次尝试。
    PairingSession timeout_client(MakeConfig(0xaaaaULL, 1000, 1));
    CHECK_EQ(timeout_client.Connect("k", 0), PairingError::kNone);
    for (int step = 0; step < 10; ++step) {
        (void)timeout_client.Tick(static_cast<std::uint64_t>(step + 1));
    }
    CHECK(timeout_client.outbound_size() > 0);
    CHECK(timeout_client.Tick(1000));
    CHECK_EQ(timeout_client.state(), PairingState::kClosed);
    CHECK_EQ(timeout_client.outbound_size(), 0U);
}

// ---------------------------------------------------------------------------
// 场景 7：时钟异常（时间倒退 / 起点为 0 / 回绕）
// ---------------------------------------------------------------------------

void TestClockAnomaly() {
    // 时间倒退：不触发超时。
    PairingSession session(MakeConfig());
    CHECK_EQ(session.Register("k", 100000), PairingError::kNone);
    CHECK_EQ(session.deadline_ms(), 115000U);
    CHECK(!session.Tick(50000));
    CHECK_EQ(session.state(), PairingState::kPending);
    CHECK(session.Tick(115000));
    CHECK_EQ(session.state(), PairingState::kClosed);

    // 起点 0：deadline 正常为 timeout_ms。
    PairingSession zero(MakeConfig(0xbbbbULL));
    CHECK_EQ(zero.Connect("k", 0), PairingError::kNone);
    CHECK_EQ(zero.deadline_ms(), 15000U);
    CHECK(!zero.Tick(14999));
    CHECK(zero.Tick(15000));

    // 时钟接近 UINT64_MAX：now + timeout 会无符号回绕，deadline 变小；
    // 这是已知限制（真实毫秒时钟不会到达），此处固化行为并记录。
    PairingSession overflow(MakeConfig());
    const std::uint64_t huge = UINT64_MAX - 100;
    CHECK_EQ(overflow.Register("k", huge), PairingError::kNone);
    std::printf("[INFO] clock wrap: now=%llu deadline=%llu\n",
                static_cast<unsigned long long>(huge),
                static_cast<unsigned long long>(overflow.deadline_ms()));
    CHECK(overflow.deadline_ms() < huge);  // 已回绕

    // Reset 后截止时间归零，不会误触发超时。
    PairingSession reset(MakeConfig());
    CHECK_EQ(reset.Connect("k", 0), PairingError::kNone);
    reset.Reset();
    CHECK_EQ(reset.deadline_ms(), 0U);
    CHECK(!reset.Tick(0));
    CHECK_EQ(reset.state(), PairingState::kIdle);
}

// ---------------------------------------------------------------------------
// 场景 8：有损链路（丢包 30% + 重复 10% + 乱序 0..3 步）
// ---------------------------------------------------------------------------

void TestLossyLink(const Config& config) {
    std::mt19937_64 rng(static_cast<std::uint64_t>(config.seed) ^ 0x5a5a5a5aULL);
    struct InFlight {
        int deliver_step;
        bool to_server;
        PairingFrame frame;
    };
    const int trials = std::max(50, static_cast<int>(300.0 * config.scale));
    int completed = 0;
    int failed = 0;
    std::size_t inconsistencies = 0;
    for (int trial = 0; trial < trials; ++trial) {
        PairingSession server(MakeConfig());
        PairingSession client(MakeConfig());
        if (server.Register("k", 0) != PairingError::kNone ||
            client.Connect("k", 0) != PairingError::kNone) {
            ++failed;
            continue;
        }
        std::vector<InFlight> pipe;
        std::uint64_t now = 0;
        bool done = false;
        const int max_steps = 320;  // 320 × 50ms = 16s
        for (int step = 0; step < max_steps; ++step) {
            now += 50;
            auto push_outbound = [&](PairingSession& from, bool to_server) {
                PairingFrame frame;
                while (from.PopOutbound(&frame)) {
                    if (rng() % 100 < 30) {
                        continue;  // 丢包 30%
                    }
                    const int deliver = step + static_cast<int>(rng() % 4);  // 乱序 0..3
                    pipe.push_back(InFlight{deliver, to_server, frame});
                    if (rng() % 100 < 10) {
                        pipe.push_back(InFlight{deliver, to_server, frame});  // 重复 10%
                    }
                }
            };
            push_outbound(server, false);
            push_outbound(client, true);

            for (std::size_t index = 0; index < pipe.size();) {
                if (pipe[index].deliver_step <= step) {
                    if (pipe[index].to_server) {
                        (void)server.HandleFrame(pipe[index].frame, now);
                    } else {
                        (void)client.HandleFrame(pipe[index].frame, now);
                    }
                    pipe.erase(pipe.begin() + static_cast<std::ptrdiff_t>(index));
                } else {
                    ++index;
                }
            }
            if (server.HasPendingConnect()) {
                (void)server.Response(true, now);
            }
            if (client.state() == PairingState::kPaired) {
                (void)client.Activate(now);
            }
            (void)server.Tick(now);
            (void)client.Tick(now);
            if (server.state() == PairingState::kActive &&
                client.state() == PairingState::kActive) {
                done = true;
                break;
            }
        }
        if (done) {
            ++completed;
        } else {
            ++failed;
        }
        // 收敛一致性：客户端 Active 必须服务端已 Active；pair_id 两侧一致。
        if (client.state() == PairingState::kActive &&
            server.state() != PairingState::kActive) {
            ++inconsistencies;
        }
        if ((server.state() == PairingState::kPaired ||
             server.state() == PairingState::kActive) &&
            (client.state() == PairingState::kPaired ||
             client.state() == PairingState::kActive) &&
            server.pair_id() != client.pair_id()) {
            ++inconsistencies;
        }
    }
    CHECK_EQ(inconsistencies, 0U);
    std::printf("[LOSSY] trials=%d completed=%d failed=%d\n", trials, completed, failed);
    CHECK(completed * 5 >= trials * 4);  // 30% 丢包下 15s 内完成率 ≥ 80%
}

// ---------------------------------------------------------------------------
// 随机测试
// ---------------------------------------------------------------------------

void TestRandomFrames(const Config& config) {
    std::mt19937_64 rng(config.seed);
    PairingSession server(MakeConfig(0xaaaaULL, 15000, 1000));
    PairingSession client(MakeConfig(0xbbbbULL, 15000, 1000));
    const std::size_t steps = std::max<std::size_t>(
        1000, static_cast<std::size_t>(20000.0 * config.scale));
    std::size_t accepted = 0;
    std::size_t rejected = 0;

    for (std::size_t index = 0; index < steps; ++index) {
        // 20% 步先做一次合法操作，制造 Idle/Pending/Paired/Active/Closed 混合状态。
        if (rng() % 5 == 0) {
            switch (rng() % 8) {
                case 0:
                    (void)server.Register("k", index);
                    break;
                case 1:
                    (void)client.Connect("k", index);
                    break;
                case 2:
                    if (server.HasPendingConnect()) {
                        (void)server.Response((rng() % 2) == 0, index);
                    }
                    break;
                case 3:
                    (void)client.Activate(index);
                    break;
                case 4: {
                    PairingFrame frame;
                    while (client.PopOutbound(&frame)) {
                        (void)server.HandleFrame(frame, index);
                    }
                    break;
                }
                case 5: {
                    PairingFrame frame;
                    while (server.PopOutbound(&frame)) {
                        (void)client.HandleFrame(frame, index);
                    }
                    break;
                }
                case 6:
                    (void)server.Tick(index * 10);
                    break;
                default:
                    (void)client.Tick(index * 10);
                    break;
            }
        }

        PairingFrame frame;
        if (rng() % 2 == 0) {
            // 一半帧是「结构合法、语义随机」：真正进入状态机分支，
            // 而不是在版本/status 校验处被提前拒绝。
            frame.type = static_cast<PairingFrameType>(static_cast<std::uint8_t>(1 + rng() % 4));
            frame.version = 1;
            frame.status = PairingError::kNone;
        } else {
            frame.type = static_cast<PairingFrameType>(static_cast<std::uint8_t>(1 + rng() % 4));
            frame.version = (rng() % 16 == 0) ? static_cast<std::uint8_t>(rng() % 8)
                                              : static_cast<std::uint8_t>(1);
            const std::uint64_t status_pick = rng() % 16;
            if (status_pick < 4) {
                frame.status = PairingError::kNone;
            } else if (status_pick < 12) {
                frame.status = static_cast<PairingError>(static_cast<std::uint8_t>(rng() % 12));
            } else {
                frame.status = static_cast<PairingError>(static_cast<std::uint8_t>(rng() % 250));
            }
        }
        frame.key_hash = (rng() % 4 == 0) ? (rng() * 0x9e3779b97f4a7c15ULL) : server.key_hash();
        if (rng() % 4 == 0) {
            frame.nonce = rng();
        } else {
            frame.nonce = server.nonce() != 0 ? server.nonce() : client.nonce();
        }
        frame.pair_id = static_cast<std::uint32_t>(rng());

        const PairingError server_result = server.HandleFrame(frame, index);
        CHECK(go2cpp::bento::IsKnownPairingError(server_result));
        if (server_result == PairingError::kNone) {
            ++accepted;
        } else {
            ++rejected;
        }
        const PairingError client_result = client.HandleFrame(frame, index);
        CHECK(go2cpp::bento::IsKnownPairingError(client_result));
        if (client_result == PairingError::kNone) {
            ++accepted;
        } else {
            ++rejected;
        }

        CHECK(IsKnownState(server.state()));
        CHECK(IsKnownState(client.state()));
        CHECK(server.outbound_size() <= kPairingMaxOutbound);
        CHECK(client.outbound_size() <= kPairingMaxOutbound);
        // 每步排空，避免队列上限把随机性掩盖掉。
        PairingFrame drained;
        while (server.PopOutbound(&drained)) {
        }
        while (client.PopOutbound(&drained)) {
        }
    }
    std::printf("[RANDOM] frames=%zu accepted=%zu rejected=%zu\n", steps * 2, accepted, rejected);
}

void TestRandomOps(const Config& config) {
    std::mt19937_64 rng(static_cast<std::uint64_t>(config.seed) ^ 0x1234ULL);
    PairingSession server(MakeConfig(0x1111ULL, 4000, 500));
    PairingSession client(MakeConfig(0x2222ULL, 4000, 500));
    std::uint64_t now = 1000;
    std::size_t transitions = 0;
    const std::size_t steps = std::max<std::size_t>(
        1000, static_cast<std::size_t>(200000.0 * config.scale));
    for (std::size_t index = 0; index < steps; ++index) {
        const PairingState before_server = server.state();
        const PairingState before_client = client.state();
        switch (rng() % 12) {
            case 0:
                (void)server.Register("k", now);
                break;
            case 1:
                (void)client.Connect("k", now);
                break;
            case 2:
                (void)server.Response((rng() % 2) == 0, now);
                break;
            case 3:
                (void)client.Activate(now);
                break;
            case 4: {
                PairingFrame frame;
                while (client.PopOutbound(&frame)) {
                    (void)server.HandleFrame(frame, now);
                }
                break;
            }
            case 5: {
                PairingFrame frame;
                while (server.PopOutbound(&frame)) {
                    (void)client.HandleFrame(frame, now);
                }
                break;
            }
            case 6:
                (void)server.Tick(now);
                break;
            case 7:
                (void)client.Tick(now);
                break;
            case 8:
                now += 700;
                break;
            case 9:
                now += 5000;  // 跨过 15s 超时边界（timeout=4000）
                break;
            case 10:
                // 应用主动关闭是 Active 的唯一出口；两端一起关，模拟会话表收尾。
                if (server.state() == PairingState::kClosed &&
                    client.state() == PairingState::kClosed) {
                    server.Reset();
                    client.Reset();
                } else {
                    (void)server.Close();
                    (void)client.Close();
                }
                break;
            default:
                if (client.state() == PairingState::kClosed) {
                    client.Reset();
                }
                break;
        }
        if (server.state() != before_server) {
            ++transitions;
        }
        if (client.state() != before_client) {
            ++transitions;
        }
        CHECK(IsKnownState(server.state()));
        CHECK(IsKnownState(client.state()));
        CHECK(server.outbound_size() <= kPairingMaxOutbound);
        CHECK(client.outbound_size() <= kPairingMaxOutbound);
        // 客户端只有在收到服务端 ACK 后才 Active，因此服务端必须先 Active。
        if (client.state() == PairingState::kActive) {
            CHECK(server.state() == PairingState::kActive);
        }
        if (server.state() == PairingState::kPaired ||
            server.state() == PairingState::kActive) {
            CHECK(server.pair_id() != 0);
        }
        if (client.state() == PairingState::kPaired ||
            client.state() == PairingState::kActive) {
            CHECK(client.pair_id() != 0);
            if (server.state() == PairingState::kPaired ||
                server.state() == PairingState::kActive) {
                CHECK_EQ(client.pair_id(), server.pair_id());
            }
        }
    }
    std::printf("[RANDOM] ops=%zu transitions=%zu server=%s client=%s\n", steps, transitions,
                go2cpp::bento::PairingStateName(server.state()),
                go2cpp::bento::PairingStateName(client.state()));
    CHECK(transitions > 0);
}

void TestRandomDecode(const Config& config) {
    std::mt19937_64 rng(static_cast<std::uint64_t>(config.seed) ^ 0x9999ULL);
    const std::size_t count = std::max<std::size_t>(
        1000, static_cast<std::size_t>(1000000.0 * config.scale));
    std::size_t accepted = 0;
    for (std::size_t index = 0; index < count; ++index) {
        std::uint8_t buffer[kPairingFrameBytes];
        for (std::uint8_t& byte : buffer) {
            byte = static_cast<std::uint8_t>(rng());
        }
        if (rng() % 8 == 0) {
            PairingFrame frame;
            frame.type = static_cast<PairingFrameType>(static_cast<std::uint8_t>(1 + rng() % 4));
            frame.key_hash = rng();
            frame.nonce = rng();
            frame.pair_id = static_cast<std::uint32_t>(rng());
            frame.status = static_cast<PairingError>(static_cast<std::uint8_t>(rng() % 12));
            EncodePairingFrame(frame, buffer);
        }
        // 哨兵：PairingFrame 不是平凡类型，用逐字段赋值代替 memset。
        PairingFrame decoded;
        decoded.type = PairingFrameType::kRegister;
        decoded.version = 0x5a;
        decoded.key_hash = 0x5a5a5a5a5a5a5a5aULL;
        decoded.nonce = 0x5a5a5a5a5a5a5a5aULL;
        decoded.pair_id = 0x5a5a5a5aU;
        decoded.status = PairingError::kClosed;
        if (DecodePairingFrame(buffer, kPairingFrameBytes, &decoded)) {
            ++accepted;
            CHECK(IsKnownFrameType(decoded.type));
            CHECK_EQ(decoded.version, 1);
            CHECK(go2cpp::bento::IsKnownPairingError(decoded.status));
        } else {
            // 契约：解码失败不写 out。
            CHECK_EQ(decoded.key_hash, 0x5a5a5a5a5a5a5a5aULL);
            CHECK_EQ(decoded.nonce, 0x5a5a5a5a5a5a5a5aULL);
            CHECK_EQ(decoded.version, 0x5a);
        }
    }
    std::printf("[RANDOM] decode count=%zu accepted=%zu\n", count, accepted);
    CHECK(accepted > 0);
}

// ---------------------------------------------------------------------------
// 内存 / 泄漏
// ---------------------------------------------------------------------------

void TestMemory(const Config& config) {
    const std::size_t live = std::max<std::size_t>(1000,
                                                   static_cast<std::size_t>(1000.0 * config.scale));
    const long rss_before = ProcStatusKb("VmRSS:");
    {
        std::vector<std::unique_ptr<PairingSession>> sessions;
        sessions.reserve(live);
        for (std::size_t index = 0; index < live; ++index) {
            auto session = std::make_unique<PairingSession>(MakeConfig(index + 1));
            CHECK_EQ(session->Register("key-" + std::to_string(index), 0), PairingError::kNone);
            sessions.push_back(std::move(session));
        }
        const long rss_live = ProcStatusKb("VmRSS:");
        std::printf("[MEM] live_sessions=%zu rss_before=%ldKB rss_live=%ldKB delta=%ldKB\n", live,
                    rss_before, rss_live, rss_live - rss_before);
        CHECK(rss_live - rss_before < 64L * 1024L);
    }
    const long rss_after = ProcStatusKb("VmRSS:");
    std::printf("[MEM] after_release=%ldKB fd=%d\n", rss_after, FdCount());
}

void TestLeakCycles(const Config& config) {
    const std::size_t warmup = 20000;
    const std::size_t cycles =
        std::max<std::size_t>(1000, static_cast<std::size_t>(200000.0 * config.scale));

    auto run_cycles = [](std::size_t count) -> std::size_t {
        std::size_t ok = 0;
        for (std::size_t index = 0; index < count; ++index) {
            PairingSession server(MakeConfig(index + 1));
            PairingSession client(MakeConfig(0x1000000ULL + index));
            if (PairUntilActiveFast(server, client)) {
                ++ok;
            }
        }
        return ok;
    };

    CHECK_EQ(run_cycles(warmup), warmup);
    const long rss_warm = ProcStatusKb("VmRSS:");
    const std::size_t ok = run_cycles(cycles);
    const long rss_end = ProcStatusKb("VmRSS:");
    CHECK_EQ(ok, cycles);
    std::printf("[LEAK] cycles=%zu rss_warm=%ldKB rss_end=%ldKB delta=%ldKB fd=%d\n", cycles,
                rss_warm, rss_end, rss_end - rss_warm, FdCount());
    CHECK(rss_end - rss_warm < 8L * 1024L);  // 稳态后增量 < 8MB（1000 万次分配量级）
}

// ---------------------------------------------------------------------------
// 入口
// ---------------------------------------------------------------------------

bool Wants(const Config& config, const char* name) {
    return config.mode == "all" || config.mode == name;
}

Config ParseArgs(int argc, char** argv) {
    Config config;
    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        if (argument.rfind("--mode=", 0) == 0) {
            config.mode = argument.substr(7);
        } else if (argument.rfind("--scale=", 0) == 0) {
            config.scale = std::stod(argument.substr(8));
        } else if (argument.rfind("--seed=", 0) == 0) {
            config.seed = static_cast<std::uint32_t>(std::stoul(argument.substr(7)));
        } else if (argument.rfind("--long=", 0) == 0) {
            config.long_seconds = std::stoi(argument.substr(7));
        } else if (argument == "--help") {
            std::printf(
                "usage: audit_pairing_stress [scale] [--mode=all|bench|scenarios|random|leaks] "
                "[--seed=N] [--long=S]\n");
        } else if (!argument.empty() && argument[0] != '-') {
            config.scale = std::stod(argument);
        } else {
            std::printf("[WARN] unknown argument: %s\n", argument.c_str());
        }
    }
    if (config.scale <= 0.0) {
        config.scale = 1.0;
    }
    return config;
}

}  // namespace

int main(int argc, char** argv) {
    const Config config = ParseArgs(argc, argv);
    std::printf("audit_pairing_stress: mode=%s scale=%.3f seed=%u\n", config.mode.c_str(),
                config.scale, config.seed);
    std::printf("[ENV] cpus=%u rss=%ldKB fd=%d now=%llu\n",
                static_cast<unsigned>(std::thread::hardware_concurrency()), ProcStatusKb("VmRSS:"),
                FdCount(), static_cast<unsigned long long>(NowMs()));

    if (Wants(config, "bench")) {
        BenchCodec(config);
        BenchConstruct(config);
        BenchHash(config);
        BenchPairing(config);
        BenchHandleFrame(config);
    }
    if (Wants(config, "scenarios")) {
        TestTimeoutBoundary();
        TestRetransmitSchedule();
        TestIdempotentReplay();
        TestKeyMismatchMatrix();
        TestConcurrentRegistration(config);
        TestQueuePressure();
        TestClockAnomaly();
    }
    if (Wants(config, "random")) {
        TestRandomFrames(config);
        TestRandomOps(config);
        TestRandomDecode(config);
        TestLossyLink(config);
    }
    if (Wants(config, "leaks")) {
        TestMemory(config);
        TestLeakCycles(config);
    }

    std::printf("[SUMMARY] checks=%d failures=%d\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
