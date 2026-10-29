/**
 * @file audit_crypto_stress.cpp
 * @brief crypto / x25519 模块审查用压测、随机与场景程序（不注册进 CTest）。
 * @author go2cpp contributors
 * @date 2026-10-29
 * @copyright MIT License
 *
 * @details 与 `tests/bento/test_{crypto,x25519,x25519_handshake}.cpp` 的分工：
 *          - 注册进 CTest 的那三份是确定性回归测试，只放毫秒级用例；
 *          - 本程序是审查压测工具，覆盖百万级 Seal/Open（32/256/1400B）、
 *            百万级重放窗口、1000+ 随机密钥/nonce/明文/篡改、5 个复杂
 *            场景（X25519 握手、篡改重放、前向保密、低阶点、旧端降级）、
 *            多线程一致性与 RSS 观测。
 *
 * 用法：
 * @code
 *   audit_crypto_stress 1.0                  # 全量（约 20s）
 *   audit_crypto_stress 1.0 --mode=bench     # 只跑吞吐
 *   audit_crypto_stress 1.0 --mode=random    # 随机密钥/明文/篡改
 *   audit_crypto_stress 1.0 --mode=replay    # 重放窗口压测
 *   audit_crypto_stress 1.0 --mode=scenarios # 5 个复杂场景
 *   audit_crypto_stress 0.01 --mode=all      # ASan/UBSan 小规模
 *   audit_crypto_stress 0.002 --mode=leaks   # valgrind 小规模
 *   audit_crypto_stress 1.0 --seed=12345     # 换随机种子
 * @endcode
 *
 * 退出码：0 = 全部断言通过；1 = 有失败（失败点打印 CHECK 位置）。
 */

#include <sys/resource.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "go2cpp/bento/net/crypto.hpp"
#include "go2cpp/bento/net/link.hpp"
#include "go2cpp/bento/net/protocol.hpp"
#include "go2cpp/bento/net/x25519.hpp"

namespace {

using namespace go2cpp::bento;

using Key = std::array<std::uint8_t, kX25519KeyBytes>;
using Psk = std::array<std::uint8_t, kPskBytes>;

int g_checks = 0;
int g_failures = 0;

#define CHECK(condition)                                                                     \
    do {                                                                                     \
        ++g_checks;                                                                          \
        if (!(condition)) {                                                                  \
            std::fprintf(stderr, "[FAIL] %s:%d: %s\n", __FILE__, __LINE__, #condition);      \
            ++g_failures;                                                                    \
        }                                                                                    \
    } while (0)

#define CHECK_EQ(actual, expected)                                                           \
    do {                                                                                     \
        ++g_checks;                                                                          \
        if (!((actual) == (expected))) {                                                     \
            std::fprintf(stderr, "[FAIL] %s:%d: %s == %s\n", __FILE__, __LINE__, #actual,    \
                         #expected);                                                         \
            ++g_failures;                                                                    \
        }                                                                                    \
    } while (0)

/// 压测循环里使用的轻量断言：只计数，避免百万次断言把 g_checks 撑爆。
#define BENCH_CHECK(condition)                                                               \
    do {                                                                                     \
        if (!(condition)) {                                                                  \
            std::fprintf(stderr, "[FAIL] %s:%d: %s\n", __FILE__, __LINE__, #condition);      \
            ++g_failures;                                                                    \
        }                                                                                    \
    } while (0)

struct Config {
    double scale = 1.0;
    std::string mode = "all";
    std::uint32_t seed = 0xC0FFEE01U;
};

double WallSeconds() {
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    return std::chrono::duration<double>(now).count();
}

double CpuSeconds() {
    timespec value{};
    clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &value);
    return static_cast<double>(value.tv_sec) + static_cast<double>(value.tv_nsec) * 1e-9;
}

long CurrentRssKb() {
    std::FILE* file = std::fopen("/proc/self/statm", "r");
    if (file == nullptr) {
        return -1;
    }
    long total_pages = 0;
    long resident_pages = 0;
    const int scanned = std::fscanf(file, "%ld %ld", &total_pages, &resident_pages);
    std::fclose(file);
    if (scanned != 2) {
        return -1;
    }
    return resident_pages * (static_cast<long>(sysconf(_SC_PAGESIZE)) / 1024);
}

long PeakRssKb() {
    rusage usage{};
    if (getrusage(RUSAGE_SELF, &usage) != 0) {
        return -1;
    }
    return static_cast<long>(usage.ru_maxrss);
}

void FillRandom(std::mt19937_64& rng, std::uint8_t* out, std::size_t bytes) {
    std::size_t offset = 0;
    while (offset + 8 <= bytes) {
        const std::uint64_t value = rng();
        for (int byte = 0; byte < 8; ++byte) {
            out[offset + static_cast<std::size_t>(byte)] =
                static_cast<std::uint8_t>(value >> (8 * byte));
        }
        offset += 8;
    }
    if (offset < bytes) {
        const std::uint64_t value = rng();
        for (std::size_t i = 0; offset < bytes; ++i, ++offset) {
            out[offset] = static_cast<std::uint8_t>(value >> (8 * i));
        }
    }
}

void FillPattern(std::uint8_t* out, std::size_t bytes, std::uint8_t seed) {
    for (std::size_t i = 0; i < bytes; ++i) {
        out[i] = static_cast<std::uint8_t>((seed + i * 31U + 7U) & 0xFFU);
    }
}

Psk MakePsk(std::uint8_t seed) {
    Psk psk{};
    FillPattern(psk.data(), psk.size(), seed);
    return psk;
}

bool IsZeroBytes(const std::uint8_t* data, std::size_t bytes) {
    std::uint8_t accumulator = 0;
    for (std::size_t i = 0; i < bytes; ++i) {
        accumulator = static_cast<std::uint8_t>(accumulator | data[i]);
    }
    return accumulator == 0;
}

bool IsZeroVector(const std::vector<std::uint8_t>& bytes) {
    return bytes.empty() || IsZeroBytes(bytes.data(), bytes.size());
}

std::vector<std::uint8_t> MakePayload(std::uint32_t index, std::size_t bytes) {
    std::vector<std::uint8_t> payload(bytes, 0);
    for (std::size_t i = 0; i < bytes; ++i) {
        payload[i] = static_cast<std::uint8_t>((index * 29U + i * 5U + 3U) & 0xFFU);
    }
    return payload;
}

FrameHeader StreamHeader(std::uint32_t seq = 1) {
    FrameHeader header;
    header.type = FrameType::kStream;
    header.channel = 0;
    header.conn_id = 0x12345678U;
    header.seq = seq;
    return header;
}

std::vector<std::uint8_t> MakeInnerFrame(const std::vector<std::uint8_t>& payload) {
    return BuildFrame(StreamHeader(), nullptr, payload.data(), payload.size());
}

// ---------------------------------------------------------------------------
// 吞吐压测
// ---------------------------------------------------------------------------

/// 一次压测的测量结果。
struct BenchStat {
    double wall_seconds = 0.0;
    double cpu_seconds = 0.0;
    double payload_mbps = 0.0;
    double wire_mbps = 0.0;
    double ns_per_op = 0.0;
    double mops = 0.0;
    long rss_delta_kb = 0;
};

void ReportBench(const char* name, const BenchStat& stat, long operations) {
    const double cpu_percent =
        stat.wall_seconds > 0.0 ? stat.cpu_seconds / stat.wall_seconds * 100.0 : 0.0;
    std::printf(
        "[BENCH] %-26s ops=%-9ld wall=%.3fs cpu=%.3fs(%5.1f%%) %8.3f Mops/s "
        "%9.1f MB/s(payload) %9.1f MB/s(wire) %6.1f ns/op rss=%+ldKB\n",
        name, operations, stat.wall_seconds, stat.cpu_seconds, cpu_percent, stat.mops,
        stat.payload_mbps, stat.wire_mbps, stat.ns_per_op, stat.rss_delta_kb);
}

/// 纯 AEAD 原语：Seal 与 Open 分开计时（不含帧编解码与 vector 开销）。
void BenchAeadPrimitive(const Config& config, std::size_t bytes, int iterations) {
    (void)config;
    const std::vector<std::uint8_t> key = MakePayload(1, kChaCha20KeyBytes);
    std::vector<std::uint8_t> nonce(kChaCha20NonceBytes, 0);
    FillPattern(nonce.data(), nonce.size(), 0x22);
    std::vector<std::uint8_t> plaintext(bytes, 0);
    FillPattern(plaintext.data(), plaintext.size(), 0x5A);
    std::vector<std::uint8_t> ciphertext(bytes, 0);
    std::uint8_t tag[kAeadTagBytes] = {};

    for (int i = 0; i < 256; ++i) {
        AeadSeal(key.data(), nonce.data(), nullptr, 0, plaintext.data(), bytes, ciphertext.data(),
                 tag);
    }

    const long rss_before = CurrentRssKb();
    const double wall_start = WallSeconds();
    const double cpu_start = CpuSeconds();
    for (int i = 0; i < iterations; ++i) {
        AeadSeal(key.data(), nonce.data(), nullptr, 0, plaintext.data(), bytes, ciphertext.data(),
                 tag);
    }
    const double wall_end = WallSeconds();
    const double cpu_end = CpuSeconds();
    const long rss_after = CurrentRssKb();

    BenchStat stat;
    stat.wall_seconds = wall_end - wall_start;
    stat.cpu_seconds = cpu_end - cpu_start;
    stat.ns_per_op = stat.wall_seconds * 1e9 / static_cast<double>(iterations);
    stat.mops = static_cast<double>(iterations) / stat.wall_seconds / 1e6;
    stat.payload_mbps =
        static_cast<double>(bytes) * static_cast<double>(iterations) / stat.wall_seconds / 1e6;
    stat.wire_mbps = stat.payload_mbps;
    stat.rss_delta_kb = rss_after - rss_before;
    char name[64] = {};
    std::snprintf(name, sizeof(name), "aead-seal-%zuB", bytes);
    ReportBench(name, stat, iterations);

    const double open_wall_start = WallSeconds();
    const double open_cpu_start = CpuSeconds();
    std::vector<std::uint8_t> output(bytes, 0);
    for (int i = 0; i < iterations; ++i) {
        BENCH_CHECK(AeadOpen(key.data(), nonce.data(), nullptr, 0, ciphertext.data(), bytes, tag,
                             output.data()));
    }
    const double open_wall_end = WallSeconds();
    const double open_cpu_end = CpuSeconds();

    BenchStat open_stat;
    open_stat.wall_seconds = open_wall_end - open_wall_start;
    open_stat.cpu_seconds = open_cpu_end - open_cpu_start;
    open_stat.ns_per_op = open_stat.wall_seconds * 1e9 / static_cast<double>(iterations);
    open_stat.mops = static_cast<double>(iterations) / open_stat.wall_seconds / 1e6;
    open_stat.payload_mbps = static_cast<double>(bytes) * static_cast<double>(iterations) /
                             open_stat.wall_seconds / 1e6;
    open_stat.wire_mbps = open_stat.payload_mbps;
    open_stat.rss_delta_kb = 0;
    std::snprintf(name, sizeof(name), "aead-open-%zuB", bytes);
    ReportBench(name, open_stat, iterations);
    BENCH_CHECK(output == plaintext);
}

/// 完整帧封装：SealFrame / OpenFrame（含 DecodeHeader/EncodeHeader 与 vector 复用）。
void BenchFrameSealOpen(const Config& config, std::size_t payload_bytes, int iterations) {
    (void)config;
    const std::vector<std::uint8_t> key = MakePayload(2, kChaCha20KeyBytes);
    const std::vector<std::uint8_t> payload = MakePayload(3, payload_bytes);
    const std::vector<std::uint8_t> inner = MakeInnerFrame(payload);
    BENCH_CHECK(!inner.empty());
    std::vector<std::uint8_t> sealed;
    sealed.reserve(inner.size() + kSecureOverheadBytes);

    for (int i = 0; i < 256; ++i) {
        BENCH_CHECK(SealFrame(key.data(), SecureDirection::kClientToServer,
                              static_cast<std::uint32_t>(i + 1), inner.data(), inner.size(),
                              &sealed));
    }

    const long rss_before = CurrentRssKb();
    const double wall_start = WallSeconds();
    const double cpu_start = CpuSeconds();
    for (int i = 0; i < iterations; ++i) {
        BENCH_CHECK(SealFrame(key.data(), SecureDirection::kClientToServer,
                              static_cast<std::uint32_t>(i + 1), inner.data(), inner.size(),
                              &sealed));
    }
    const double wall_end = WallSeconds();
    const double cpu_end = CpuSeconds();
    const long rss_after = CurrentRssKb();

    BenchStat stat;
    stat.wall_seconds = wall_end - wall_start;
    stat.cpu_seconds = cpu_end - cpu_start;
    stat.ns_per_op = stat.wall_seconds * 1e9 / static_cast<double>(iterations);
    stat.mops = static_cast<double>(iterations) / stat.wall_seconds / 1e6;
    stat.payload_mbps =
        static_cast<double>(payload_bytes) * static_cast<double>(iterations) / stat.wall_seconds /
        1e6;
    stat.wire_mbps = static_cast<double>(sealed.size()) * static_cast<double>(iterations) /
                     stat.wall_seconds / 1e6;
    stat.rss_delta_kb = rss_after - rss_before;
    char name[64] = {};
    std::snprintf(name, sizeof(name), "seal-frame-%zuB", payload_bytes);
    ReportBench(name, stat, iterations);

    std::vector<std::uint8_t> opened;
    opened.reserve(inner.size());
    ReplayWindow window;
    std::vector<std::uint8_t> frozen = sealed;
    const double open_wall_start = WallSeconds();
    const double open_cpu_start = CpuSeconds();
    for (int i = 0; i < iterations; ++i) {
        window.Reset();
        OpenStatus status = OpenStatus::kMalformed;
        BENCH_CHECK(OpenFrame(key.data(), SecureDirection::kClientToServer, &window, frozen.data(),
                              frozen.size(), &opened, &status));
        BENCH_CHECK(status == OpenStatus::kOk);
    }
    const double open_wall_end = WallSeconds();
    const double open_cpu_end = CpuSeconds();

    BenchStat open_stat;
    open_stat.wall_seconds = open_wall_end - open_wall_start;
    open_stat.cpu_seconds = open_cpu_end - open_cpu_start;
    open_stat.ns_per_op = open_stat.wall_seconds * 1e9 / static_cast<double>(iterations);
    open_stat.mops = static_cast<double>(iterations) / open_stat.wall_seconds / 1e6;
    open_stat.payload_mbps = static_cast<double>(payload_bytes) * static_cast<double>(iterations) /
                             open_stat.wall_seconds / 1e6;
    open_stat.wire_mbps = static_cast<double>(frozen.size()) * static_cast<double>(iterations) /
                          open_stat.wall_seconds / 1e6;
    open_stat.rss_delta_kb = 0;
    std::snprintf(name, sizeof(name), "open-frame-%zuB", payload_bytes);
    ReportBench(name, open_stat, iterations);
    BENCH_CHECK(opened == inner);
}

/// 纯 ChaCha20 流加密吞吐：隔离 Poly1305 后观察 4 路并行的实际收益。
void BenchChaCha20(const Config& config) {
    constexpr std::size_t kBufferBytes = 1U << 20;
    std::vector<std::uint8_t> input(kBufferBytes, 0);
    std::vector<std::uint8_t> output(kBufferBytes, 0);
    FillPattern(input.data(), input.size(), 0x5A);
    const std::vector<std::uint8_t> key = MakePayload(5, kChaCha20KeyBytes);
    std::vector<std::uint8_t> nonce(kChaCha20NonceBytes, 0);
    FillPattern(nonce.data(), nonce.size(), 0x22);
    const int iterations = std::max(1, static_cast<int>(64 * config.scale));

    const double wall_start = WallSeconds();
    const double cpu_start = CpuSeconds();
    for (int i = 0; i < iterations; ++i) {
        ChaCha20Xor(key.data(), static_cast<std::uint32_t>(i + 1), nonce.data(), input.data(),
                    output.data(), kBufferBytes);
    }
    const double wall_end = WallSeconds();
    const double cpu_end = CpuSeconds();
    BenchStat stat;
    stat.wall_seconds = wall_end - wall_start;
    stat.cpu_seconds = cpu_end - cpu_start;
    stat.ns_per_op = stat.wall_seconds * 1e9 / static_cast<double>(iterations);
    stat.mops = static_cast<double>(iterations) / stat.wall_seconds / 1e6;
    stat.payload_mbps = static_cast<double>(kBufferBytes) * static_cast<double>(iterations) /
                        stat.wall_seconds / 1e6;
    stat.wire_mbps = stat.payload_mbps;
    ReportBench("chacha20-xor-1MB", stat, iterations);
    CHECK(output != input);
}

void BenchKdf(const Config& config) {
    const Psk psk = MakePsk(0x51);
    std::uint8_t shared[kX25519KeyBytes] = {};
    FillPattern(shared, sizeof(shared), 0xE1);
    std::uint8_t client_nonce[kHandshakeNonceBytes] = {};
    std::uint8_t server_nonce[kHandshakeNonceBytes] = {};
    FillPattern(client_nonce, sizeof(client_nonce), 0x33);
    FillPattern(server_nonce, sizeof(server_nonce), 0x55);
    std::uint8_t out[kChaCha20KeyBytes] = {};
    const int iterations = std::max(1, static_cast<int>(2000000 * config.scale));

    const double wall_start = WallSeconds();
    const double cpu_start = CpuSeconds();
    for (int i = 0; i < iterations; ++i) {
        DeriveSessionKeyWithX25519(psk.data(), shared, client_nonce, server_nonce,
                                   SecureDirection::kClientToServer, out);
        shared[0] = static_cast<std::uint8_t>(out[0]);  // 防优化，且每轮输入不同
    }
    const double wall_end = WallSeconds();
    const double cpu_end = CpuSeconds();
    BenchStat stat;
    stat.wall_seconds = wall_end - wall_start;
    stat.cpu_seconds = cpu_end - cpu_start;
    stat.ns_per_op = stat.wall_seconds * 1e9 / static_cast<double>(iterations);
    stat.mops = static_cast<double>(iterations) / stat.wall_seconds / 1e6;
    stat.payload_mbps = 0.0;
    stat.wire_mbps = 0.0;
    ReportBench("kdf-x25519-mix", stat, iterations);

    // HChaCha20 单次吞吐。
    std::uint8_t hout[32] = {};
    const int h_iterations = std::max(1, static_cast<int>(10000000 * config.scale));
    const double h_wall_start = WallSeconds();
    const double h_cpu_start = CpuSeconds();
    for (int i = 0; i < h_iterations; ++i) {
        HChaCha20(psk.data(), out, hout);
        out[0] = hout[0];
    }
    const double h_wall_end = WallSeconds();
    const double h_cpu_end = CpuSeconds();
    BenchStat hstat;
    hstat.wall_seconds = h_wall_end - h_wall_start;
    hstat.cpu_seconds = h_cpu_end - h_cpu_start;
    hstat.ns_per_op = hstat.wall_seconds * 1e9 / static_cast<double>(h_iterations);
    hstat.mops = static_cast<double>(h_iterations) / hstat.wall_seconds / 1e6;
    hstat.payload_mbps = 0.0;
    hstat.wire_mbps = 0.0;
    ReportBench("hchacha20-32B", hstat, h_iterations);
}

void BenchX25519(const Config& config) {
    const int iterations = std::max(1, static_cast<int>(20000 * config.scale));
    Key private_key{};
    Key peer_private{};
    FillPattern(private_key.data(), private_key.size(), 7);
    FillPattern(peer_private.data(), peer_private.size(), 8);
    Key peer_public{};
    BENCH_CHECK(X25519PublicKeyFromPrivate(peer_private.data(), peer_public.data()));
    Key shared{};

    for (int i = 0; i < 32; ++i) {
        BENCH_CHECK(X25519ComputeSharedSecret(private_key.data(), peer_public.data(),
                                              shared.data()));
    }

    const double wall_start = WallSeconds();
    const double cpu_start = CpuSeconds();
    for (int i = 0; i < iterations; ++i) {
        private_key[0] = static_cast<std::uint8_t>(i);
        BENCH_CHECK(X25519ComputeSharedSecret(private_key.data(), peer_public.data(),
                                              shared.data()));
        private_key[0] = static_cast<std::uint8_t>(shared[0] ^ private_key[0]);
    }
    const double wall_end = WallSeconds();
    const double cpu_end = CpuSeconds();
    BenchStat stat;
    stat.wall_seconds = wall_end - wall_start;
    stat.cpu_seconds = cpu_end - cpu_start;
    stat.ns_per_op = stat.wall_seconds * 1e9 / static_cast<double>(iterations);
    stat.mops = static_cast<double>(iterations) / stat.wall_seconds / 1e6;
    stat.payload_mbps = 0.0;
    stat.wire_mbps = 0.0;
    ReportBench("x25519-scalarmult", stat, iterations);
}

void BenchReplayWindow(const Config& config) {
    ReplayWindow window;
    const int iterations = std::max(1, static_cast<int>(5000000 * config.scale));
    std::uint32_t accepted = 0;
    std::uint32_t rejected = 0;
    const double wall_start = WallSeconds();
    const double cpu_start = CpuSeconds();
    std::uint32_t seq = 1;
    for (int i = 0; i < iterations; ++i) {
        // 70% 顺序推进、20% 窗口内乱序、10% 重复/窗口外。
        const std::uint32_t lane = static_cast<std::uint32_t>(i % 10);
        std::uint32_t candidate = seq;
        if (lane < 7) {
            candidate = seq++;
        } else if (lane < 9) {
            candidate = seq > 10 ? seq - 1 - (static_cast<std::uint32_t>(i) % 9) : 1;
        } else {
            candidate = seq > 100 ? seq - 100 : 1;
        }
        if (candidate != 0 && window.Accept(candidate)) {
            ++accepted;
        } else {
            ++rejected;
        }
    }
    const double wall_end = WallSeconds();
    const double cpu_end = CpuSeconds();
    BenchStat stat;
    stat.wall_seconds = wall_end - wall_start;
    stat.cpu_seconds = cpu_end - cpu_start;
    stat.ns_per_op = stat.wall_seconds * 1e9 / static_cast<double>(iterations);
    stat.mops = static_cast<double>(iterations) / stat.wall_seconds / 1e6;
    ReportBench("replay-window-mixed", stat, iterations);
    std::printf("[INFO] replay window: accepted=%u rejected=%u highest=%u\n", accepted, rejected,
                window.highest());
    CHECK(accepted + rejected == static_cast<std::uint32_t>(iterations));
    CHECK(window.highest() >= static_cast<std::uint32_t>(iterations / 2));
}

void RunBench(const Config& config) {
    const int iterations = std::max(1, static_cast<int>(1000000 * config.scale));
    std::printf("[MODE] bench scale=%.3f iterations=%d\n", config.scale, iterations);
    for (const std::size_t bytes : {std::size_t{32}, std::size_t{256}, std::size_t{1400}}) {
        BenchAeadPrimitive(config, bytes, iterations);
    }
    // 协议单帧载荷上限 kMaxFramePayload=1300，1400B 只能分片；帧级压测取
    // 32/256/1300 三个尺寸，1400B 由上面的 AEAD 原语覆盖。
    for (const std::size_t bytes : {std::size_t{32}, std::size_t{256}, std::size_t{1300}}) {
        BenchFrameSealOpen(config, bytes, iterations);
    }
    BenchChaCha20(config);
    BenchKdf(config);
    BenchX25519(config);
    BenchReplayWindow(config);
}

// ---------------------------------------------------------------------------
// 随机测试
// ---------------------------------------------------------------------------

void RunRandomAead(const Config& config, int rounds) {
    std::printf("[MODE] random-aead rounds=%d\n", rounds);
    std::mt19937_64 rng(config.seed);
    std::vector<std::uint8_t> plaintext;
    std::vector<std::uint8_t> ciphertext;
    std::vector<std::uint8_t> decrypted;
    std::vector<std::uint8_t> aad;
    int accepted_tamper = 0;
    for (int round = 0; round < rounds; ++round) {
        std::uint8_t key[kChaCha20KeyBytes] = {};
        std::uint8_t nonce[kChaCha20NonceBytes] = {};
        FillRandom(rng, key, sizeof(key));
        FillRandom(rng, nonce, sizeof(nonce));
        const std::size_t plaintext_bytes = static_cast<std::size_t>(rng() % 1025);
        const std::size_t aad_bytes = static_cast<std::size_t>(rng() % 65);
        plaintext.resize(plaintext_bytes);
        aad.resize(aad_bytes);
        FillRandom(rng, plaintext.data(), plaintext.size());
        FillRandom(rng, aad.data(), aad.size());
        ciphertext.assign(plaintext_bytes, 0);
        std::uint8_t tag[kAeadTagBytes] = {};
        AeadSeal(key, nonce, aad.data(), aad.size(), plaintext.data(), plaintext_bytes,
                 ciphertext.data(), tag);

        decrypted.assign(plaintext_bytes, 0);
        CHECK(AeadOpen(key, nonce, aad.data(), aad.size(), ciphertext.data(), plaintext_bytes,
                       tag, decrypted.data()));
        CHECK(decrypted == plaintext);

        // 四种篡改：1 字节、多字节、截断、tag。
        const std::uint32_t mode = static_cast<std::uint32_t>(rng() % 4);
        std::vector<std::uint8_t> tampered = ciphertext;
        std::size_t tampered_bytes = tampered.size();
        bool valid_shape = true;
        std::uint8_t tampered_tag[kAeadTagBytes] = {};
        std::memcpy(tampered_tag, tag, sizeof(tag));
        if (mode == 0 && !tampered.empty()) {
            tampered[rng() % tampered.size()] ^= static_cast<std::uint8_t>(1U << (rng() % 8));
        } else if (mode == 1 && !tampered.empty()) {
            const std::size_t changes = 2 + static_cast<std::size_t>(rng() % 5);
            for (std::size_t i = 0; i < changes; ++i) {
                tampered[rng() % tampered.size()] ^= static_cast<std::uint8_t>(rng() & 0xFFU);
            }
        } else if (mode == 2 && !tampered.empty()) {
            tampered_bytes = tampered.size() > 8 ? tampered.size() - (1 + rng() % 8) : 0;
        } else if (mode == 3) {
            tampered_tag[rng() % sizeof(tampered_tag)] ^=
                static_cast<std::uint8_t>(1U << (rng() % 8));
        } else {
            valid_shape = false;
        }
        if (!valid_shape) {
            continue;
        }
        // 多字节随机翻转可能互相抵消、退化成原始密文；本轮跳过，避免把
        // “没有真正篡改”误判成认证失败漏检（与 test_crypto 保持一致）。
        if (tampered == ciphertext && tampered_bytes == ciphertext.size() &&
            std::memcmp(tampered_tag, tag, kAeadTagBytes) == 0) {
            continue;
        }
        // 认证失败时不得写入明文：先填哨兵再检查。
        std::vector<std::uint8_t> guarded(tampered_bytes, 0xA5);
        const bool opened = AeadOpen(key, nonce, aad.data(), aad.size(), tampered.data(),
                                     tampered_bytes, tampered_tag, guarded.data());
        if (opened) {
            ++accepted_tamper;
        }
        CHECK(!opened);
        if (!opened) {
            bool untouched = true;
            for (std::uint8_t byte : guarded) {
                untouched = untouched && byte == 0xA5;
            }
            CHECK(untouched);
        }
        // 错误密钥同样不能打开，且失败时不得写入明文。
        std::uint8_t wrong_key[kChaCha20KeyBytes] = {};
        std::memcpy(wrong_key, key, sizeof(wrong_key));
        wrong_key[0] ^= 0xFF;
        std::vector<std::uint8_t> wrong_out(plaintext_bytes, 0x5A);
        CHECK(!AeadOpen(wrong_key, nonce, aad.data(), aad.size(), ciphertext.data(),
                        plaintext_bytes, tag, wrong_out.data()));
        bool wrong_untouched = true;
        for (std::uint8_t byte : wrong_out) {
            wrong_untouched = wrong_untouched && byte == 0x5A;
        }
        CHECK(wrong_untouched);
    }
    CHECK_EQ(accepted_tamper, 0);
    std::printf("[INFO] random-aead: %d rounds, tamper accepted=%d\n", rounds, accepted_tamper);
}

void RunRandomFrames(const Config& config, int rounds) {
    std::printf("[MODE] random-frames rounds=%d\n", rounds);
    std::mt19937_64 rng(config.seed ^ 0x9E3779B9U);
    int accepted = 0;
    int rejected = 0;
    for (int round = 0; round < rounds; ++round) {
        std::uint8_t key[kChaCha20KeyBytes] = {};
        FillRandom(rng, key, sizeof(key));
        const std::size_t payload_bytes = 1 + static_cast<std::size_t>(rng() % 512);
        const std::vector<std::uint8_t> payload = MakePayload(static_cast<std::uint32_t>(round),
                                                              payload_bytes);
        const std::vector<std::uint8_t> inner = MakeInnerFrame(payload);
        const std::uint32_t seq = static_cast<std::uint32_t>(rng() | 1U);
        std::vector<std::uint8_t> sealed;
        CHECK(SealFrame(key, SecureDirection::kClientToServer, seq, inner.data(), inner.size(),
                        &sealed));
        ReplayWindow window;
        std::vector<std::uint8_t> opened;
        OpenStatus status = OpenStatus::kMalformed;
        CHECK(OpenFrame(key, SecureDirection::kClientToServer, &window, sealed.data(),
                        sealed.size(), &opened, &status));
        CHECK(status == OpenStatus::kOk);
        CHECK(opened == inner);

        // 随机篡改：头/密文/tag/截断。
        std::vector<std::uint8_t> tampered = sealed;
        const std::uint32_t mode = static_cast<std::uint32_t>(rng() % 5);
        std::size_t tampered_bytes = tampered.size();
        if (mode == 0) {
            tampered[rng() % tampered.size()] ^= 0x01;
        } else if (mode == 1) {
            const std::size_t changes = 2 + static_cast<std::size_t>(rng() % 4);
            for (std::size_t i = 0; i < changes; ++i) {
                tampered[rng() % tampered.size()] ^= static_cast<std::uint8_t>(rng() & 0xFFU);
            }
        } else if (mode == 2) {
            tampered_bytes = tampered.size() > 16 ? tampered.size() - (1 + rng() % 16) : 0;
        } else if (mode == 3) {
            tampered[3] = static_cast<std::uint8_t>(tampered[3] & ~kFlagEncrypted);
        } else {
            tampered[0] = static_cast<std::uint8_t>(tampered[0] ^ 0xFF);  // 版本号
        }
        if (tampered == sealed && tampered_bytes == sealed.size()) {
            continue;  // 随机翻转互相抵消，本轮没有真实篡改。
        }
        ReplayWindow fresh;
        std::vector<std::uint8_t> output;
        OpenStatus tamper_status = OpenStatus::kMalformed;
        const bool ok = OpenFrame(key, SecureDirection::kClientToServer, &fresh, tampered.data(),
                                  tampered_bytes, &output, &tamper_status);
        if (ok) {
            ++accepted;
        } else {
            ++rejected;
            CHECK(tamper_status == OpenStatus::kAuthFailed ||
                  tamper_status == OpenStatus::kMalformed ||
                  tamper_status == OpenStatus::kReplay ||
                  tamper_status == OpenStatus::kNotEncrypted);
            CHECK(IsZeroVector(output));
        }
        // 重放已接受帧必须被窗口拒绝。
        ReplayWindow replay_window;
        std::vector<std::uint8_t> replay_out;
        OpenStatus replay_status = OpenStatus::kMalformed;
        CHECK(OpenFrame(key, SecureDirection::kClientToServer, &replay_window, sealed.data(),
                        sealed.size(), &replay_out, &replay_status));
        OpenStatus second_status = OpenStatus::kMalformed;
        CHECK(!OpenFrame(key, SecureDirection::kClientToServer, &replay_window, sealed.data(),
                         sealed.size(), &replay_out, &second_status));
        CHECK(second_status == OpenStatus::kReplay);
    }
    CHECK_EQ(accepted, 0);
    std::printf("[INFO] random-frames: %d rounds accepted_tamper=%d rejected=%d\n", rounds,
                accepted, rejected);
}

void RunRandomTickets(const Config& config, int rounds) {
    std::printf("[MODE] random-tickets rounds=%d\n", rounds);
    std::mt19937_64 rng(config.seed ^ 0x51ED270BU);
    const Psk psk = MakePsk(0x71);
    int accepted = 0;
    for (int round = 0; round < rounds; ++round) {
        SessionTicket ticket;
        ticket.conn_id = static_cast<std::uint32_t>(rng() | 1U);
        ticket.session_id = static_cast<std::uint32_t>(rng() | 1U);
        ticket.issued_ms = rng() % 1000000U;
        ticket.expires_ms = ticket.issued_ms + 1 + rng() % 100000U;
        FillRandom(rng, ticket.client_nonce, kHandshakeNonceBytes);
        FillRandom(rng, ticket.server_nonce, kHandshakeNonceBytes);
        ticket.client_next_wire_seq = static_cast<std::uint32_t>(rng() | 1U);
        ticket.server_next_wire_seq = static_cast<std::uint32_t>(rng() | 1U);
        std::vector<std::uint8_t> encoded;
        CHECK(EncodeSessionTicket(ticket, psk.data(), &encoded));
        CHECK_EQ(encoded.size(), kSessionTicketBytes);
        SessionTicket decoded;
        CHECK(DecodeSessionTicket(encoded.data(), encoded.size(), psk.data(), &decoded));
        CHECK_EQ(decoded.conn_id, ticket.conn_id);
        CHECK_EQ(decoded.session_id, ticket.session_id);
        CHECK(std::memcmp(decoded.client_nonce, ticket.client_nonce, kHandshakeNonceBytes) == 0);

        // 单字节翻转必须被 MAC 拒绝。
        std::vector<std::uint8_t> tampered = encoded;
        const std::size_t offset = static_cast<std::size_t>(rng() % tampered.size());
        tampered[offset] ^= static_cast<std::uint8_t>(1U << (rng() % 8));
        SessionTicket rejected_ticket;
        const bool ok = DecodeSessionTicket(tampered.data(), tampered.size(), psk.data(),
                                            &rejected_ticket);
        if (ok) {
            ++accepted;
        }
        CHECK(!ok);
        // 错误 PSK 必须被拒绝。
        Psk wrong_psk = psk;
        wrong_psk[static_cast<std::size_t>(rng() % wrong_psk.size())] ^= 0x80;
        CHECK(!DecodeSessionTicket(encoded.data(), encoded.size(), wrong_psk.data(),
                                   &rejected_ticket));
        // 长度不符必须被拒绝。
        CHECK(!DecodeSessionTicket(encoded.data(), encoded.size() - 1, psk.data(),
                                   &rejected_ticket));
    }
    CHECK_EQ(accepted, 0);
    std::printf("[INFO] random-tickets: %d rounds, tamper accepted=%d\n", rounds, accepted);
}

void RunRandomReplayWindow(const Config& config, int rounds) {
    std::printf("[MODE] random-replay rounds=%d\n", rounds);
    std::mt19937_64 rng(config.seed ^ 0x12345678U);
    int violations = 0;
    for (int round = 0; round < rounds; ++round) {
        // InitializeBefore 之后，窗口内所有旧序号都必须视为已消费。
        const std::uint32_t next_seq = 2 + static_cast<std::uint32_t>(rng() % 1000000U);
        ReplayWindow window;
        window.InitializeBefore(next_seq);
        CHECK(window.initialized());
        CHECK_EQ(window.highest(), next_seq - 1);
        for (std::uint32_t age = 0; age < ReplayWindow::kWindow && age < next_seq; ++age) {
            const std::uint32_t old_seq = next_seq - 1 - age;
            if (old_seq == 0) {
                continue;
            }
            if (window.WouldAccept(old_seq)) {
                ++violations;
            }
        }
        if (next_seq < 0xFFFFFFFFU) {
            CHECK(window.WouldAccept(next_seq));
            CHECK(window.Accept(next_seq));
            CHECK(!window.WouldAccept(next_seq));
        }
        // 随机乱序：窗口内未收过的序号可接受，收过的不行。
        ReplayWindow ordering;
        std::vector<std::uint32_t> delivered;
        for (int step = 0; step < 64; ++step) {
            const std::uint32_t seq = static_cast<std::uint32_t>(1 + rng() % 128);
            const bool expected = ordering.WouldAccept(seq);
            const bool actual = ordering.Accept(seq);
            CHECK_EQ(actual, expected);
            if (actual) {
                // 再次接受必须失败。
                CHECK(!ordering.Accept(seq));
            }
        }
    }
    CHECK_EQ(violations, 0);
    std::printf("[INFO] random-replay: %d rounds, stale accepted=%d\n", rounds, violations);
}

void RunRandom(const Config& config) {
    RunRandomAead(config, std::max(1, static_cast<int>(1000 * config.scale)));
    RunRandomFrames(config, std::max(1, static_cast<int>(1000 * config.scale)));
    RunRandomTickets(config, std::max(1, static_cast<int>(500 * config.scale)));
    RunRandomReplayWindow(config, std::max(1, static_cast<int>(1000 * config.scale)));
}

// ---------------------------------------------------------------------------
// 复杂场景
// ---------------------------------------------------------------------------

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

std::uint64_t WallClockMs() {
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(now).count());
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
    config.random_seed = 0x5EC0DE03U;
    config.pre_shared_key.assign(psk.begin(), psk.end());
    return config;
}

bool PumpForPacket(Link& link, UdpSocket& peer, Datagram* packet, int timeout_ms) {
    const std::uint64_t deadline = NowMs() + static_cast<std::uint64_t>(timeout_ms);
    while (NowMs() < deadline) {
        link.RunOnce(1);
        if (ReceiveDatagram(peer, packet, 1)) {
            return true;
        }
    }
    return false;
}

/// Link 双端交替 Pump，直到谓词为真或超时。
template <typename Predicate>
bool LinkPump(Link& first, Link& second, Predicate done, int timeout_ms) {
    const std::uint64_t deadline = NowMs() + static_cast<std::uint64_t>(timeout_ms);
    while (NowMs() < deadline) {
        first.RunOnce(1);
        second.RunOnce(1);
        if (done()) {
            return true;
        }
    }
    return done();
}

/// 单端 Pump。
template <typename Predicate>
bool LinkPump(Link& link, Predicate done, int timeout_ms) {
    const std::uint64_t deadline = NowMs() + static_cast<std::uint64_t>(timeout_ms);
    while (NowMs() < deadline) {
        link.RunOnce(1);
        if (done()) {
            return true;
        }
    }
    return done();
}

/// 场景 1：PSK + X25519 前向保密的完整加密握手，双向数据，统计校验。
void ScenarioSecureHandshake(const Config& config) {
    std::printf("[SCENARIO] 1. PSK + X25519 secure handshake + bidirectional data\n");
    const int rounds = std::max(1, static_cast<int>(3 * config.scale));
    for (int round = 0; round < rounds; ++round) {
        const Psk psk = MakePsk(static_cast<std::uint8_t>(0x11 + round));
        Link server(ServerConfig(psk));
        std::string error;
        CHECK(server.Open(&error));
        CHECK(server.secure());
        Link client(ClientConfig(server.local_port(), psk));
        CHECK(client.Open(&error));
        CHECK(client.secure());

        bool server_connected = false;
        bool client_connected = false;
        server.on_connected = [&server_connected]() { server_connected = true; };
        client.on_connected = [&client_connected]() { client_connected = true; };
        std::vector<std::vector<std::uint8_t>> server_received;
        std::vector<std::vector<std::uint8_t>> client_received;
        server.on_stream = [&server_received](std::uint8_t /*channel*/, const std::uint8_t* data,
                                              std::size_t bytes) {
            server_received.emplace_back(data, data + bytes);
        };
        client.on_stream = [&client_received](std::uint8_t /*channel*/, const std::uint8_t* data,
                                              std::size_t bytes) {
            client_received.emplace_back(data, data + bytes);
        };
        client.Connect();
        CHECK(LinkPump(client, server, [&]() { return server_connected && client_connected; },
                       5000));
        CHECK(server.session_established());
        CHECK(client.session_established());
        CHECK_EQ(server.secure_stats().x25519_handshakes, 1U);
        CHECK_EQ(client.secure_stats().x25519_handshakes, 1U);
        CHECK_EQ(server.secure_stats().psk_fallbacks, 0U);

        constexpr std::uint32_t kMessages = 100;
        for (std::uint32_t i = 0; i < kMessages; ++i) {
            const std::vector<std::uint8_t> payload = MakePayload(i, 96);
            CHECK(client.SendStream(0, payload.data(), payload.size()));
            CHECK(server.SendStream(1, payload.data(), payload.size()));
        }
        CHECK(LinkPump(client, server,
                       [&]() {
                           return server_received.size() == kMessages &&
                                  client_received.size() == kMessages;
                       },
                       10000));
        CHECK_EQ(server_received.size(), kMessages);
        CHECK_EQ(client_received.size(), kMessages);
        CHECK(server_received[0] == MakePayload(0, 96));
        CHECK(client_received[kMessages - 1] == MakePayload(kMessages - 1, 96));
        CHECK(server.secure_stats().packets_opened > 0);
        CHECK(client.secure_stats().packets_sealed > 0);
        CHECK_EQ(server.secure_stats().auth_failures, 0U);
        CHECK_EQ(server.secure_stats().replay_drops, 0U);
        // X25519 会话不允许导出 0-RTT 票据（否则会静默降级成纯 PSK）。
        CHECK(client.ExportSessionTicket().empty());
        CHECK(server.ExportSessionTicket().empty());
        client.Close();
        server.Close();
    }
}

/// 场景 2：64 位滑动窗口下的乱序、重放、篡改与高序号 DoS。
void ScenarioTamperReplayWindow(const Config&) {
    std::printf("[SCENARIO] 2. tamper/replay under 64-packet sliding window\n");
    const std::vector<std::uint8_t> key = MakePayload(0x31, kChaCha20KeyBytes);
    constexpr std::uint32_t kFrames = 100;
    std::vector<std::vector<std::uint8_t>> sealed;
    sealed.reserve(kFrames);
    for (std::uint32_t seq = 1; seq <= kFrames; ++seq) {
        const std::vector<std::uint8_t> payload = MakePayload(seq, 200);
        const std::vector<std::uint8_t> inner = MakeInnerFrame(payload);
        std::vector<std::uint8_t> frame;
        CHECK(SealFrame(key.data(), SecureDirection::kClientToServer, seq, inner.data(), inner.size(),
                        &frame));
        sealed.push_back(std::move(frame));
    }

    ReplayWindow window;
    std::vector<std::uint8_t> opened;
    OpenStatus status = OpenStatus::kMalformed;
    // 顺序 1..50。
    for (std::uint32_t seq = 1; seq <= 50; ++seq) {
        CHECK(OpenFrame(key.data(), SecureDirection::kClientToServer, &window, sealed[seq - 1].data(),
                        sealed[seq - 1].size(), &opened, &status));
        CHECK(status == OpenStatus::kOk);
    }
    // 跳到 100，再把 60..99 乱序投递（全部在窗口内）。
    CHECK(OpenFrame(key.data(), SecureDirection::kClientToServer, &window, sealed[99].data(),
                    sealed[99].size(), &opened, &status));
    CHECK_EQ(window.highest(), 100U);
    for (std::uint32_t seq = 99; seq >= 60; --seq) {
        CHECK(window.WouldAccept(seq));
        CHECK(OpenFrame(key.data(), SecureDirection::kClientToServer, &window, sealed[seq - 1].data(),
                        sealed[seq - 1].size(), &opened, &status));
    }
    CHECK_EQ(window.highest(), 100U);
    // 51..59 仍在窗口内（100-51=49 < 64），必须还能接受——这正是滑动窗口
    // 容忍乱序的语义；36 已经滑出（age=64），必须拒绝。
    CHECK(!window.WouldAccept(36));
    CHECK(window.WouldAccept(51));
    // 重放已接受的 100 与 50。
    for (const std::uint32_t seq : {100U, 50U, 60U}) {
        CHECK(!OpenFrame(key.data(), SecureDirection::kClientToServer, &window, sealed[seq - 1].data(),
                         sealed[seq - 1].size(), &opened, &status));
        CHECK(status == OpenStatus::kReplay);
    }
    // 篡改：1 字节、多字节、截断、tag。
    for (std::uint32_t variant = 0; variant < 4; ++variant) {
        ReplayWindow fresh;
        std::vector<std::uint8_t> tampered = sealed[12];
        std::size_t bytes = tampered.size();
        if (variant == 0) {
            tampered[20] ^= 0x01;
        } else if (variant == 1) {
            tampered[30] ^= 0xFF;
            tampered[31] ^= 0x0F;
            tampered[32] ^= 0x80;
        } else if (variant == 2) {
            bytes -= 5;
        } else {
            tampered.back() ^= 0x01;
        }
        CHECK(!OpenFrame(key.data(), SecureDirection::kClientToServer, &fresh, tampered.data(), bytes,
                         &opened, &status));
        CHECK(status == OpenStatus::kAuthFailed || status == OpenStatus::kMalformed);
        CHECK(IsZeroVector(opened));
    }
    // 高序号伪造包：必须先通过 WouldAccept、认证失败、窗口不推进。
    std::vector<std::uint8_t> forged = sealed[41];  // 内容合法但 seq 被改成 100000
    forged[8] = 0x00;
    forged[9] = 0x01;
    forged[10] = 0x86;
    forged[11] = 0xA0;
    ReplayWindow fresh;
    CHECK(OpenFrame(key.data(), SecureDirection::kClientToServer, &fresh, sealed[41].data(),
                    sealed[41].size(), &opened, &status));
    CHECK_EQ(fresh.highest(), 42U);
    CHECK(!OpenFrame(key.data(), SecureDirection::kClientToServer, &fresh, forged.data(), forged.size(),
                     &opened, &status));
    CHECK(status == OpenStatus::kAuthFailed);
    CHECK_EQ(fresh.highest(), 42U);  // 伪造高序号不能推高窗口
    // 之后合法 seq=43 仍可接受（DoS 未生效）。
    CHECK(OpenFrame(key.data(), SecureDirection::kClientToServer, &fresh, sealed[42].data(),
                    sealed[42].size(), &opened, &status));
    CHECK_EQ(fresh.highest(), 43U);
}

/// 场景 3：前向保密——持有 PSK、双方临时公钥与 nonce 也无法解开会话。
void ScenarioForwardSecrecy(const Config& config) {
    std::printf("[SCENARIO] 3. forward secrecy: handshake material cannot open session\n");
    const int rounds = std::max(1, static_cast<int>(10 * config.scale));
    const Psk psk = MakePsk(0x91);
    int leaked = 0;
    for (int round = 0; round < rounds; ++round) {
        Key client_private{};
        Key server_private{};
        Key client_public{};
        Key server_public{};
        X25519GeneratePrivateKey(client_private.data());
        X25519GeneratePrivateKey(server_private.data());
        CHECK(X25519PublicKeyFromPrivate(client_private.data(), client_public.data()));
        CHECK(X25519PublicKeyFromPrivate(server_private.data(), server_public.data()));
        std::uint8_t client_shared[kX25519KeyBytes] = {};
        std::uint8_t server_shared[kX25519KeyBytes] = {};
        CHECK(X25519ComputeSharedSecret(client_private.data(), server_public.data(),
                                        client_shared));
        CHECK(X25519ComputeSharedSecret(server_private.data(), client_public.data(),
                                        server_shared));
        CHECK(std::memcmp(client_shared, server_shared, kX25519KeyBytes) == 0);

        std::uint8_t client_nonce[kHandshakeNonceBytes] = {};
        std::uint8_t server_nonce[kHandshakeNonceBytes] = {};
        FillPattern(client_nonce, sizeof(client_nonce), static_cast<std::uint8_t>(round * 7));
        FillPattern(server_nonce, sizeof(server_nonce),
                    static_cast<std::uint8_t>(round * 11 + 1));
        std::uint8_t session_key[kChaCha20KeyBytes] = {};
        DeriveSessionKeyWithX25519(psk.data(), client_shared, client_nonce, server_nonce,
                                   SecureDirection::kClientToServer, session_key);

        const std::vector<std::uint8_t> plaintext = MakePayload(
            static_cast<std::uint32_t>(round), 128);
        std::uint8_t nonce[kChaCha20NonceBytes] = {};
        MakeSecureNonce(SecureDirection::kClientToServer, static_cast<std::uint32_t>(round + 1),
                        nonce);
        std::vector<std::uint8_t> ciphertext(plaintext.size(), 0);
        std::uint8_t tag[kAeadTagBytes] = {};
        AeadSeal(session_key, nonce, nullptr, 0, plaintext.data(), plaintext.size(),
                 ciphertext.data(), tag);

        // 攻击者视角：PSK + 双方公钥 + 双方 nonce，但没有临时私钥。
        std::uint8_t attacker_key[kChaCha20KeyBytes] = {};
        std::vector<std::uint8_t> output(plaintext.size(), 0x3C);
        DeriveSessionKey(psk.data(), client_nonce, server_nonce,
                         SecureDirection::kClientToServer, attacker_key);
        if (AeadOpen(attacker_key, nonce, nullptr, 0, ciphertext.data(), ciphertext.size(), tag,
                     output.data())) {
            ++leaked;
        }
        // 把公钥当共享密钥、把共享密钥全零、把方向搞反，都必须失败。
        struct Attempt {
            const std::uint8_t* shared;
            SecureDirection direction;
        };
        const Attempt attempts[] = {
            {client_public.data(), SecureDirection::kClientToServer},
            {server_public.data(), SecureDirection::kClientToServer},
            {client_public.data(), SecureDirection::kServerToClient},
        };
        for (const Attempt& attempt : attempts) {
            std::uint8_t wrong_key[kChaCha20KeyBytes] = {};
            DeriveSessionKeyWithX25519(psk.data(), attempt.shared, client_nonce, server_nonce,
                                       attempt.direction, wrong_key);
            std::vector<std::uint8_t> wrong_out(plaintext.size(), 0x3C);
            if (AeadOpen(wrong_key, nonce, nullptr, 0, ciphertext.data(), ciphertext.size(), tag,
                         wrong_out.data())) {
                ++leaked;
            }
            CHECK(wrong_out[0] == 0x3C);  // 未写入明文
        }
        std::uint8_t zero_shared[kX25519KeyBytes] = {};
        std::uint8_t zero_key[kChaCha20KeyBytes] = {};
        DeriveSessionKeyWithX25519(psk.data(), zero_shared, client_nonce, server_nonce,
                                   SecureDirection::kClientToServer, zero_key);
        CHECK(std::memcmp(zero_key, session_key, kChaCha20KeyBytes) != 0);
        // 公钥替换一个 bit：派生结果必须不同。
        Key tampered_public = client_public;
        tampered_public[0] ^= 0x01;
        std::uint8_t tampered_shared[kX25519KeyBytes] = {};
        CHECK(X25519ComputeSharedSecret(client_private.data(), tampered_public.data(),
                                        tampered_shared));
        std::uint8_t tampered_key[kChaCha20KeyBytes] = {};
        DeriveSessionKeyWithX25519(psk.data(), tampered_shared, client_nonce, server_nonce,
                                   SecureDirection::kClientToServer, tampered_key);
        CHECK(std::memcmp(tampered_key, session_key, kChaCha20KeyBytes) != 0);
    }
    CHECK_EQ(leaked, 0);
}

/// 场景 4：低阶点（u=0/1/p-1 及两个大低阶点）必须被拒绝。
void ScenarioLowOrderRejects(const Config& config) {
    std::printf("[SCENARIO] 4. low-order point rejection (u=0/1/p-1 + large points)\n");
    Key zero{};
    Key one{};
    one[0] = 1;
    Key p_minus_one{};
    p_minus_one.fill(0xFF);
    p_minus_one[0] = 0xEC;
    p_minus_one[31] = 0x7F;
    Key p{};
    p.fill(0xFF);
    p[0] = 0xED;
    p[31] = 0x7F;
    Key p_plus_one{};
    p_plus_one.fill(0xFF);
    p_plus_one[0] = 0xEE;
    p_plus_one[31] = 0x7F;
    Key order8_a{};
    Key order8_b{};
    const std::uint8_t order8_a_hex[32] = {0xE0, 0xEB, 0x7A, 0x7C, 0x3B, 0x41, 0xB8, 0xAE,
                                           0x16, 0x56, 0xE3, 0xFA, 0xF1, 0x9F, 0xC4, 0x6A,
                                           0xDA, 0x09, 0x8D, 0xEB, 0x9C, 0x32, 0xB1, 0xFD,
                                           0x86, 0x62, 0x05, 0x16, 0x5F, 0x49, 0xB8, 0x00};
    const std::uint8_t order8_b_hex[32] = {0x5F, 0x9C, 0x95, 0xBC, 0xA3, 0x50, 0x8C, 0x24,
                                           0xB1, 0xD0, 0xB1, 0x55, 0x9C, 0x83, 0xEF, 0x5B,
                                           0x04, 0x44, 0x5C, 0xC4, 0x58, 0x1C, 0x8E, 0x86,
                                           0xD8, 0x22, 0x4E, 0xDD, 0xD0, 0x9F, 0x11, 0x57};
    std::memcpy(order8_a.data(), order8_a_hex, sizeof(order8_a_hex));
    std::memcpy(order8_b.data(), order8_b_hex, sizeof(order8_b_hex));
    const Key points[] = {zero, one, p_minus_one, p, p_plus_one, order8_a, order8_b};
    const int rounds = std::max(1, static_cast<int>(20 * config.scale));
    int accepted = 0;
    std::mt19937_64 rng(config.seed ^ 0xABCDEF01U);
    for (const Key& point : points) {
        for (int round = 0; round < rounds; ++round) {
            Key private_key{};
            FillRandom(rng, private_key.data(), private_key.size());
            Key shared{};
            shared.fill(0x7E);
            const bool ok = X25519ComputeSharedSecret(private_key.data(), point.data(),
                                                      shared.data());
            if (ok) {
                ++accepted;
            }
            CHECK(!ok);
            CHECK(IsZeroBytes(shared.data(), shared.size()));
        }
    }
    CHECK_EQ(accepted, 0);

    // Link 层：三种低阶公钥的 HELLO 都不得建立会话、不得回 ACK。
    const Psk psk = MakePsk(0xB1);
    Link server(ServerConfig(psk));
    std::string error;
    CHECK(server.Open(&error));
    bool connected = false;
    server.on_connected = [&connected]() { connected = true; };
    UdpSocket attacker;
    CHECK(attacker.Open("127.0.0.1", 0, &error));
    sockaddr_storage server_address{};
    socklen_t server_address_len = 0;
    CHECK(UdpSocket::Resolve("127.0.0.1", server.local_port(), &server_address,
                             &server_address_len, &error));
    // 注意：u=0 在协议里表示“本端不提供公钥”，监听方会按设计回退纯 PSK
    // （见场景 5），因此这里只用手工构造的非零低阶点。
    const Key link_points[] = {one, p_minus_one, order8_a};
    for (std::size_t index = 0; index < 3; ++index) {
        HelloBody hello;
        hello.session = static_cast<std::uint32_t>(0x33445500U + index);
        FillPattern(hello.nonce, kHandshakeNonceBytes, static_cast<std::uint8_t>(0xC1 + index));
        hello.timestamp_ms = WallClockMs();
        std::memcpy(hello.x25519_public, link_points[index].data(), kX25519KeyBytes);
        std::uint8_t raw[kHelloBodyBytes] = {};
        CHECK(EncodeHelloBody(hello, raw, sizeof(raw)));
        std::vector<std::uint8_t> packet;
        CHECK(SealHandshakeFrame(psk.data(), SecureDirection::kClientToServer,
                                 static_cast<std::uint8_t>(FrameType::kHello), hello.session,
                                 0x99AABB00U + static_cast<std::uint32_t>(index), 1, raw,
                                 sizeof(raw), &packet));
        CHECK(attacker.SendTo(server_address, server_address_len, packet.data(), packet.size()));
        (void)LinkPump(server, []() { return false; }, 300);
        Datagram reply;
        CHECK(!ReceiveDatagram(attacker, &reply, 100));
    }
    CHECK(!connected);
    CHECK(!server.session_established());
    CHECK(server.state() != LinkState::kConnected);
    CHECK(server.secure_stats().auth_failures >= 3U);
    CHECK_EQ(server.secure_stats().x25519_handshakes, 0U);
    CHECK_EQ(server.secure_stats().psk_fallbacks, 0U);
    server.Close();
}

/// 场景 5：旧端降级——HELLO 无 X25519 公钥（旧 36B 布局）→ 纯 PSK。
void ScenarioLegacyFallback(const Config& config) {
    std::printf("[SCENARIO] 5. legacy peer without X25519 key falls back to pure PSK\n");
    const int rounds = std::max(1, static_cast<int>(3 * config.scale));
    for (int round = 0; round < rounds; ++round) {
        const Psk psk = MakePsk(static_cast<std::uint8_t>(0xD1 + round));
        Link server(ServerConfig(psk));
        std::string error;
        CHECK(server.Open(&error));
        bool connected = false;
        std::vector<std::vector<std::uint8_t>> received;
        server.on_connected = [&connected]() { connected = true; };
        server.on_stream = [&received](std::uint8_t /*channel*/, const std::uint8_t* data,
                                       std::size_t bytes) {
            received.emplace_back(data, data + bytes);
        };
        UdpSocket legacy_client;
        CHECK(legacy_client.Open("127.0.0.1", 0, &error));
        sockaddr_storage server_address{};
        socklen_t server_address_len = 0;
        CHECK(UdpSocket::Resolve("127.0.0.1", server.local_port(), &server_address,
                                 &server_address_len, &error));

        HelloBody hello;
        hello.session = static_cast<std::uint32_t>(0x55667700U + static_cast<std::uint32_t>(round));
        FillPattern(hello.nonce, kHandshakeNonceBytes,
                    static_cast<std::uint8_t>(0xE1 + round));
        hello.timestamp_ms = WallClockMs();
        // 偶数轮模拟旧对端（36B 旧布局，无公钥字段）；奇数轮模拟“新格式但
        // 显式给全零公钥”的对端。两者都必须回退纯 PSK。
        std::uint8_t raw[kHelloBodyBytes] = {};
        const std::size_t hello_bytes =
            (round % 2 == 0) ? kLegacyHelloBodyBytes : kHelloBodyBytes;
        CHECK(EncodeHelloBody(hello, raw, hello_bytes));
        std::vector<std::uint8_t> hello_packet;
        CHECK(SealHandshakeFrame(psk.data(), SecureDirection::kClientToServer,
                                 static_cast<std::uint8_t>(FrameType::kHello), hello.session,
                                 0x11223300U + static_cast<std::uint32_t>(round), 1, raw,
                                 hello_bytes, &hello_packet));
        CHECK(legacy_client.SendTo(server_address, server_address_len, hello_packet.data(),
                                   hello_packet.size()));

        Datagram ack_packet;
        CHECK(PumpForPacket(server, legacy_client, &ack_packet, 3000));
        std::uint8_t inner_type = 0;
        std::uint32_t ack_session = 0;
        std::uint32_t ack_conn_id = 0;
        std::vector<std::uint8_t> body;
        OpenStatus status = OpenStatus::kMalformed;
        CHECK(OpenHandshakeFrame(psk.data(), SecureDirection::kServerToClient,
                                 ack_packet.data.data(), ack_packet.data.size(), &inner_type,
                                 &ack_session, &ack_conn_id, &body, &status));
        CHECK_EQ(static_cast<std::uint8_t>(FrameType::kHelloAck), inner_type);
        HelloAckBody ack;
        CHECK(DecodeHelloAckBody(body.data(), body.size(), &ack));
        CHECK_EQ(body.size(), kLegacyHelloAckBodyBytes);  // 回退时用旧 52B 布局
        CHECK(IsZeroBytes(ack.x25519_public, kX25519KeyBytes));
        CHECK_EQ(ack.session, hello.session);
        CHECK(connected);
        CHECK_EQ(server.secure_stats().psk_fallbacks, 1U);
        CHECK_EQ(server.secure_stats().x25519_handshakes, 0U);

        // 用纯 PSK 派生密钥注入一条 STREAM，验证回退会话仍可用。
        std::uint8_t tx_key[kChaCha20KeyBytes] = {};
        DeriveSessionKey(psk.data(), hello.nonce, ack.server_nonce,
                         SecureDirection::kClientToServer, tx_key);
        const std::vector<std::uint8_t> payload = MakePayload(9, 48);
        FrameHeader header;
        header.type = FrameType::kStream;
        header.channel = 0;
        header.conn_id = ack_conn_id;
        header.seq = 1;
        const std::vector<std::uint8_t> inner =
            BuildFrame(header, nullptr, payload.data(), payload.size());
        std::vector<std::uint8_t> sealed;
        CHECK(SealFrame(tx_key, SecureDirection::kClientToServer, 1, inner.data(), inner.size(),
                        &sealed));
        CHECK(legacy_client.SendTo(server_address, server_address_len, sealed.data(),
                                   sealed.size()));
        CHECK(LinkPump(server, [&]() { return !received.empty(); }, 3000));
        CHECK_EQ(received.size(), 1U);
        CHECK(received[0] == payload);
        server.Close();
    }
}

void RunScenarios(const Config& config) {
    ScenarioSecureHandshake(config);
    ScenarioTamperReplayWindow(config);
    ScenarioForwardSecrecy(config);
    ScenarioLowOrderRejects(config);
    ScenarioLegacyFallback(config);
}

// ---------------------------------------------------------------------------
// 并发与泄漏
// ---------------------------------------------------------------------------

void RunConcurrency(const Config& config) {
    std::printf("[MODE] concurrency\n");
    constexpr int kThreads = 4;
    const int iterations = std::max(1, static_cast<int>(20000 * config.scale));
    std::atomic<int> failures{0};
    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (int thread = 0; thread < kThreads; ++thread) {
        threads.emplace_back([thread, iterations, &failures]() {
            std::uint8_t key[kChaCha20KeyBytes] = {};
            FillPattern(key, sizeof(key), static_cast<std::uint8_t>(0x10 + thread));
            std::vector<std::uint8_t> plaintext(static_cast<std::size_t>(32 + thread * 64), 0x5A);
            FillPattern(plaintext.data(), plaintext.size(), static_cast<std::uint8_t>(thread));
            std::vector<std::uint8_t> ciphertext(plaintext.size(), 0);
            std::vector<std::uint8_t> decrypted(plaintext.size(), 0);
            for (int i = 0; i < iterations; ++i) {
                std::uint8_t nonce[kChaCha20NonceBytes] = {};
                MakeSecureNonce(SecureDirection::kClientToServer,
                                static_cast<std::uint32_t>(i + 1), nonce);
                std::uint8_t tag[kAeadTagBytes] = {};
                AeadSeal(key, nonce, nullptr, 0, plaintext.data(), plaintext.size(),
                         ciphertext.data(), tag);
                if (!AeadOpen(key, nonce, nullptr, 0, ciphertext.data(), ciphertext.size(), tag,
                              decrypted.data()) ||
                    decrypted != plaintext) {
                    ++failures;
                }
                // X25519 也必须无全局状态（每 500 轮做一次，避免压测被标量乘主导）。
                if (i % 500 == 0) {
                    Key private_key{};
                    Key public_key{};
                    FillPattern(private_key.data(), private_key.size(),
                                static_cast<std::uint8_t>(thread * 31 + i));
                    if (!X25519PublicKeyFromPrivate(private_key.data(), public_key.data())) {
                        ++failures;
                    }
                }
            }
        });
    }
    for (std::thread& thread : threads) {
        thread.join();
    }
    CHECK_EQ(failures.load(), 0);
    std::printf("[INFO] concurrency: %d threads x %d ops, failures=%d\n", kThreads, iterations,
                failures.load());
}

void RunLeaks(const Config& config) {
    std::printf("[MODE] leaks\n");
    const int cycles = std::max(1, static_cast<int>(2000 * config.scale));
    std::vector<std::uint8_t> ciphertext;
    std::vector<std::uint8_t> decrypted;
    std::vector<std::uint8_t> inner;
    const long rss_before = CurrentRssKb();
    for (int i = 0; i < cycles; ++i) {
        std::uint8_t key[kChaCha20KeyBytes] = {};
        FillPattern(key, sizeof(key), static_cast<std::uint8_t>(i));
        const std::vector<std::uint8_t> payload = MakePayload(static_cast<std::uint32_t>(i), 256);
        inner = MakeInnerFrame(payload);
        if (!SealFrame(key, SecureDirection::kClientToServer, 1, inner.data(), inner.size(),
                       &ciphertext)) {
            ++g_failures;
        }
        ReplayWindow window;
        OpenStatus status = OpenStatus::kMalformed;
        if (!OpenFrame(key, SecureDirection::kClientToServer, &window, ciphertext.data(),
                       ciphertext.size(), &decrypted, &status)) {
            ++g_failures;
        }
        Key private_key{};
        Key public_key{};
        Key shared{};
        FillPattern(private_key.data(), private_key.size(),
                    static_cast<std::uint8_t>(i * 13));
        if (!X25519PublicKeyFromPrivate(private_key.data(), public_key.data()) ||
            !X25519ComputeSharedSecret(private_key.data(), public_key.data(), shared.data())) {
            ++g_failures;
        }
    }
    const long rss_after = CurrentRssKb();
    std::printf("[LEAK] cycles=%d rss_before=%ldKB rss_after=%ldKB delta=%ldKB peak=%ldKB\n",
                cycles, rss_before, rss_after, rss_after - rss_before, PeakRssKb());
    CHECK(rss_after - rss_before < 4096);  // 2000 轮内不应有持续增长
}

// ---------------------------------------------------------------------------
// 入口
// ---------------------------------------------------------------------------

Config ParseArgs(int argc, char** argv) {
    Config config;
    for (int i = 1; i < argc; ++i) {
        const std::string argument = argv[i];
        if (argument.rfind("--mode=", 0) == 0) {
            config.mode = argument.substr(7);
        } else if (argument.rfind("--seed=", 0) == 0) {
            config.seed = static_cast<std::uint32_t>(std::stoul(argument.substr(7)));
        } else {
            config.scale = std::stod(argument);
        }
    }
    // 0 / 负数 / NaN 会让所有循环空转，测试变成假通过；统一回落到 1.0。
    if (!(config.scale > 0.0)) {
        config.scale = 1.0;
    }
    return config;
}

}  // namespace

int main(int argc, char** argv) {
    const Config config = ParseArgs(argc, argv);
    std::printf("audit_crypto_stress: mode=%s scale=%.3f seed=%08x\n", config.mode.c_str(),
                config.scale, config.seed);
    std::printf("[ENV] cpus=%ld rss=%ldKB peak=%ldKB\n", sysconf(_SC_NPROCESSORS_ONLN),
                CurrentRssKb(), PeakRssKb());
    const double start = WallSeconds();

    if (config.mode == "all" || config.mode == "bench") {
        RunBench(config);
    }
    if (config.mode == "all" || config.mode == "random") {
        RunRandom(config);
    }
    if (config.mode == "all" || config.mode == "replay") {
        RunRandomReplayWindow(config, std::max(1, static_cast<int>(2000 * config.scale)));
    }
    if (config.mode == "all" || config.mode == "scenarios") {
        RunScenarios(config);
    }
    if (config.mode == "all" || config.mode == "concurrency") {
        RunConcurrency(config);
    }
    if (config.mode == "all" || config.mode == "leaks") {
        RunLeaks(config);
    }

    const double elapsed = WallSeconds() - start;
    std::printf("[SUMMARY] checks=%d failures=%d elapsed=%.3fs rss=%ldKB peak=%ldKB\n",
                g_checks, g_failures, elapsed, CurrentRssKb(), PeakRssKb());
    return g_failures == 0 ? 0 : 1;
}
