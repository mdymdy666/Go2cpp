/**
 * @file bench_frame_stress.cpp
 * @brief MOD-10 帧编解码的压测/随机/场景/长稳工具（独立编译，不注册 ctest）。
 *
 * @details 本文件不参与 CMake 构建（避免改动 cmake/Go2CppBento.cmake 的用例
 *          计数），由审查脚本直接编译：
 *
 *          ```
 *          g++ -O2 -std=c++17 -Wall -Wextra -Wpedantic -Iinclude -Itests/bento \
 *              tests/bento/bench_frame_stress.cpp src/bento/net/frame.cpp \
 *              src/hash/crc32.cpp -o /tmp/bench_frame -pthread
 *          ```
 *
 *          覆盖：
 *          - 大数据量吞吐（Encode/Decode/流式/CRC，≥100 万帧或 ≥100MB）；
 *          - 1000+ 随机流（合法 + 坏 magic/版本/reserved/超长/截断/篡改/随机块切分），
 *            与独立参考模型逐帧对拍；
 *          - 5 个复杂场景（粘包+半包、1MiB 边界、非法帧族、并发编解码、100 万帧长稳）；
 *          - 分配计数与 RSS 采样（泄漏/内存峰值）。
 *
 * @author go2cpp contributors
 * @date 2026-10-29
 * @copyright MIT License
 */

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <string>
#include <thread>
#include <vector>

#include "go2cpp/bento/net/frame.hpp"

using namespace go2cpp::bento::net;

namespace {

// ---------------------------------------------------------------------------
// 分配计数：验证「无泄漏」与「每帧分配次数」。
// ---------------------------------------------------------------------------
std::atomic<std::uint64_t> g_alloc_count{0};
std::atomic<std::uint64_t> g_alloc_bytes{0};

std::uint64_t AllocCount() { return g_alloc_count.load(std::memory_order_relaxed); }
std::uint64_t AllocBytes() { return g_alloc_bytes.load(std::memory_order_relaxed); }

// ---------------------------------------------------------------------------
// 断言：失败即打印并计入，最终以退出码反映。
// ---------------------------------------------------------------------------
int g_failures = 0;
int g_checks = 0;

#define STRESS_CHECK(cond)                                                        \
    do {                                                                          \
        ++g_checks;                                                               \
        if (!(cond)) {                                                            \
            std::fprintf(stderr, "[FAIL] %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++g_failures;                                                         \
        }                                                                         \
    } while (0)

#define STRESS_CHECK_EQ(actual, expected)                                   \
    do {                                                                    \
        ++g_checks;                                                         \
        if (!((actual) == (expected))) {                                    \
            std::fprintf(stderr, "[FAIL] %s:%d: %s == %s\n", __FILE__,       \
                         __LINE__, #actual, #expected);                     \
            ++g_failures;                                                   \
        }                                                                   \
    } while (0)

// ---------------------------------------------------------------------------
// 时间与统计。
// ---------------------------------------------------------------------------
using Clock = std::chrono::steady_clock;

double SecondsSince(Clock::time_point begin) {
    return std::chrono::duration<double>(Clock::now() - begin).count();
}

struct Measurement {
    std::string name;
    std::uint64_t frames = 0;
    std::uint64_t bytes = 0;
    double seconds = 0.0;
    double fps = 0.0;
    double mib_per_sec = 0.0;
    double ns_per_frame = 0.0;
    std::uint64_t allocs = 0;
    std::uint64_t alloc_bytes = 0;
};

Measurement MakeMeasurement(const char* name, std::uint64_t frames, std::uint64_t bytes,
                            double seconds, std::uint64_t allocs, std::uint64_t alloc_bytes) {
    Measurement m;
    m.name = name;
    m.frames = frames;
    m.bytes = bytes;
    m.seconds = seconds;
    m.allocs = allocs;
    m.alloc_bytes = alloc_bytes;
    m.fps = seconds > 0.0 ? static_cast<double>(frames) / seconds : 0.0;
    m.mib_per_sec = seconds > 0.0 ? static_cast<double>(bytes) / seconds / (1024.0 * 1024.0) : 0.0;
    m.ns_per_frame = frames > 0 ? seconds * 1e9 / static_cast<double>(frames) : 0.0;
    return m;
}

std::vector<Measurement> g_results;

void Report(const Measurement& m) {
    g_results.push_back(m);
    std::printf(
        "%-36s frames=%-9llu bytes=%-12llu sec=%-8.4f ns/frame=%-10.1f fps=%-12.0f "
        "MiB/s=%-9.2f allocs/frame=%.2f\n",
        m.name.c_str(), static_cast<unsigned long long>(m.frames),
        static_cast<unsigned long long>(m.bytes), m.seconds, m.ns_per_frame, m.fps,
        m.mib_per_sec,
        m.frames > 0 ? static_cast<double>(m.allocs) / static_cast<double>(m.frames) : 0.0);
}

// ---------------------------------------------------------------------------
// 帧生成。
// ---------------------------------------------------------------------------
std::uint64_t g_rng = 0x9e3779b97f4a7c15ull;

std::uint32_t RandomNext() {
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 7;
    g_rng ^= g_rng << 17;
    return static_cast<std::uint32_t>(g_rng >> 32);
}

void ResetRandom(std::uint64_t seed) { g_rng = seed != 0 ? seed : 0x9e3779b97f4a7c15ull; }

Frame MakeFrame(std::uint16_t type, std::uint16_t flags, std::size_t payload_bytes,
                std::uint8_t salt) {
    Frame frame;
    frame.type = type;
    frame.flags = flags;
    frame.payload.resize(payload_bytes);
    for (std::size_t i = 0; i < payload_bytes; ++i) {
        frame.payload[i] = static_cast<std::uint8_t>(salt + i * 31u + (i >> 8));
    }
    return frame;
}

void WriteU16(std::uint8_t* out, std::uint16_t value) {
    out[0] = static_cast<std::uint8_t>(value >> 8);
    out[1] = static_cast<std::uint8_t>(value);
}

void WriteU32(std::uint8_t* out, std::uint32_t value) {
    out[0] = static_cast<std::uint8_t>(value >> 24);
    out[1] = static_cast<std::uint8_t>(value >> 16);
    out[2] = static_cast<std::uint8_t>(value >> 8);
    out[3] = static_cast<std::uint8_t>(value);
}

std::uint32_t ReadU32(const std::uint8_t* in) {
    return (static_cast<std::uint32_t>(in[0]) << 24) |
           (static_cast<std::uint32_t>(in[1]) << 16) |
           (static_cast<std::uint32_t>(in[2]) << 8) | static_cast<std::uint32_t>(in[3]);
}

std::vector<std::uint8_t> MakeStream(const std::vector<Frame>& frames) {
    std::vector<std::uint8_t> stream;
    for (const Frame& frame : frames) {
        const std::vector<std::uint8_t> bytes = Encode(frame);
        stream.insert(stream.end(), bytes.begin(), bytes.end());
    }
    return stream;
}

// ---------------------------------------------------------------------------
// 参考模型：按规范独立实现（逐位 CRC + 手工大端解码），与实现逐帧对拍。
// ---------------------------------------------------------------------------
enum class ModelStatus {
    kOk,
    kNeedMore,
    kBadMagic,
    kBadVersion,
    kBadReserved,
    kLengthExceeded,
    kChecksumMismatch,
};

/// 逐位 CRC-32/IEEE（反射），与实现的 slicing-by-8 表法互为独立路径。
std::uint32_t RefCrc32(const std::uint8_t* data, std::size_t size) {
    std::uint32_t crc = 0xffffffffu;
    for (std::size_t i = 0; i < size; ++i) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc & 1u) != 0u ? (crc >> 1) ^ 0xedb88320u : (crc >> 1);
        }
    }
    return ~crc;
}

/// 参考模型的标准 CRC：物理上把「头前 16B + 载荷」拼成一段再算。
std::uint32_t RefFrameCrc(const std::uint8_t* frame_start, std::size_t payload_bytes) {
    std::vector<std::uint8_t> material;
    material.reserve(16 + payload_bytes);
    material.insert(material.end(), frame_start, frame_start + 16);
    if (payload_bytes > 0) {
        material.insert(material.end(), frame_start + kGenericFrameHeaderBytes,
                        frame_start + kGenericFrameHeaderBytes + payload_bytes);
    }
    return RefCrc32(material.data(), material.size());
}

ModelStatus ModelNext(const std::uint8_t* data, std::size_t size, std::size_t* consumed) {
    if (size < kGenericFrameHeaderBytes) {
        return ModelStatus::kNeedMore;
    }
    if (ReadU32(data) != kFrameMagic) {
        return ModelStatus::kBadMagic;
    }
    const std::uint16_t version = static_cast<std::uint16_t>((data[4] << 8) | data[5]);
    if (version != kFrameVersion) {
        return ModelStatus::kBadVersion;
    }
    const std::uint16_t reserved = static_cast<std::uint16_t>((data[10] << 8) | data[11]);
    if (reserved != 0) {
        return ModelStatus::kBadReserved;
    }
    const std::uint32_t length = ReadU32(data + 12);
    if (length > kFrameMaxPayload) {
        return ModelStatus::kLengthExceeded;
    }
    const std::size_t total = kGenericFrameHeaderBytes + static_cast<std::size_t>(length);
    if (size < total) {
        return ModelStatus::kNeedMore;
    }
    if (ReadU32(data + 16) != RefFrameCrc(data, length)) {
        return ModelStatus::kChecksumMismatch;
    }
    *consumed = total;
    return ModelStatus::kOk;
}

FrameStatus ToFrameStatus(ModelStatus status) {
    switch (status) {
        case ModelStatus::kOk:
            return FrameStatus::kOk;
        case ModelStatus::kNeedMore:
            return FrameStatus::kNeedMoreData;
        case ModelStatus::kBadMagic:
            return FrameStatus::kBadMagic;
        case ModelStatus::kBadVersion:
            return FrameStatus::kBadVersion;
        case ModelStatus::kBadReserved:
            return FrameStatus::kBadReserved;
        case ModelStatus::kLengthExceeded:
            return FrameStatus::kLengthExceeded;
        case ModelStatus::kChecksumMismatch:
            return FrameStatus::kChecksumMismatch;
    }
    return FrameStatus::kInvalidArgument;
}

// ---------------------------------------------------------------------------
// RSS 采样（Linux /proc）。
// ---------------------------------------------------------------------------
std::uint64_t RssKb() {
    std::FILE* file = std::fopen("/proc/self/statm", "r");
    if (file == nullptr) {
        return 0;
    }
    unsigned long long total_pages = 0;
    unsigned long long resident_pages = 0;
    if (std::fscanf(file, "%llu %llu", &total_pages, &resident_pages) != 2) {
        resident_pages = 0;
    }
    std::fclose(file);
    return static_cast<std::uint64_t>(resident_pages) * 4u;
}

// ---------------------------------------------------------------------------
// 基准 A：Encode。
// ---------------------------------------------------------------------------
void BenchEncode(std::size_t payload_bytes, std::uint64_t iterations) {
    const Frame frame = MakeFrame(0x1234, 0x0007, payload_bytes, 3);
    const std::uint64_t frame_bytes = kGenericFrameHeaderBytes + payload_bytes;
    std::uint64_t sink = 0;
    const std::uint64_t allocs0 = AllocCount();
    const std::uint64_t bytes0 = AllocBytes();
    const auto begin = Clock::now();
    for (std::uint64_t i = 0; i < iterations; ++i) {
        const std::vector<std::uint8_t> bytes = Encode(frame);
        sink += bytes.size();
    }
    const double seconds = SecondsSince(begin);
    char name[96];
    std::snprintf(name, sizeof(name), "encode payload=%zu", payload_bytes);
    Report(MakeMeasurement(name, iterations, iterations * frame_bytes, seconds,
                           AllocCount() - allocs0, AllocBytes() - bytes0));
    STRESS_CHECK_EQ(sink, iterations * frame_bytes);
}

// ---------------------------------------------------------------------------
// 基准 B：一次性 Decode（复用 / 每帧新建 Frame 两种）。
// ---------------------------------------------------------------------------
void BenchDecode(std::size_t payload_bytes, std::uint64_t iterations, bool reuse) {
    const std::vector<std::uint8_t> bytes = Encode(MakeFrame(0x2222, 0x0001, payload_bytes, 5));
    const std::uint64_t frame_bytes = kGenericFrameHeaderBytes + payload_bytes;
    std::uint64_t sink = 0;
    const std::uint64_t allocs0 = AllocCount();
    const std::uint64_t bytes0 = AllocBytes();
    const auto begin = Clock::now();
    if (reuse) {
        Frame out;
        for (std::uint64_t i = 0; i < iterations; ++i) {
            const FrameStatus status = Decode(bytes.data(), bytes.size(), &out);
            sink += (status == FrameStatus::kOk ? 1u : 0u) + out.payload.size();
        }
    } else {
        for (std::uint64_t i = 0; i < iterations; ++i) {
            Frame out;
            const FrameStatus status = Decode(bytes.data(), bytes.size(), &out);
            sink += (status == FrameStatus::kOk ? 1u : 0u) + out.payload.size();
        }
    }
    const double seconds = SecondsSince(begin);
    char name[96];
    std::snprintf(name, sizeof(name), "decode-one payload=%zu reuse=%d", payload_bytes,
                  reuse ? 1 : 0);
    Report(MakeMeasurement(name, iterations, iterations * frame_bytes, seconds,
                           AllocCount() - allocs0, AllocBytes() - bytes0));
    STRESS_CHECK_EQ(sink, iterations * (1u + payload_bytes));
}

// ---------------------------------------------------------------------------
// 基准 C：流式粘包（一次 Append 多帧，循环 Next）。
// ---------------------------------------------------------------------------
void BenchDecodeSticky(std::size_t payload_bytes, std::uint64_t iterations) {
    constexpr std::uint64_t kBatch = 64;
    iterations -= iterations % kBatch;
    std::vector<Frame> frames;
    frames.reserve(static_cast<std::size_t>(kBatch));
    for (std::uint64_t i = 0; i < kBatch; ++i) {
        frames.push_back(MakeFrame(static_cast<std::uint16_t>(i), 0, payload_bytes,
                                   static_cast<std::uint8_t>(i)));
    }
    const std::vector<std::uint8_t> stream = MakeStream(frames);
    FrameDecoder decoder;
    Frame out;
    std::uint64_t received = 0;
    std::uint64_t decode_failures = 0;
    const std::uint64_t allocs0 = AllocCount();
    const std::uint64_t bytes0 = AllocBytes();
    const auto begin = Clock::now();
    std::uint64_t remaining = iterations;
    while (remaining > 0) {
        decoder.Append(stream.data(), stream.size());
        for (std::uint64_t i = 0; i < kBatch; ++i) {
            if (decoder.Next(&out) != FrameStatus::kOk) {
                ++decode_failures;
            }
            ++received;
        }
        if (decoder.Next(&out) != FrameStatus::kNeedMoreData) {
            ++decode_failures;
        }
        remaining -= kBatch;
    }
    const double seconds = SecondsSince(begin);
    char name[96];
    std::snprintf(name, sizeof(name), "stream-sticky payload=%zu batch=%llu", payload_bytes,
                  static_cast<unsigned long long>(kBatch));
    Report(MakeMeasurement(name, iterations, iterations * (kGenericFrameHeaderBytes + payload_bytes),
                           seconds, AllocCount() - allocs0, AllocBytes() - bytes0));
    STRESS_CHECK_EQ(received, iterations);
    STRESS_CHECK_EQ(decode_failures, 0U);
}

// ---------------------------------------------------------------------------
// 基准 D：逐字节半包（每次 Append 1B）。
// ---------------------------------------------------------------------------
void BenchDecodeBytewise(std::size_t payload_bytes, std::uint64_t iterations) {
    const std::vector<std::uint8_t> bytes = Encode(MakeFrame(0x3333, 0, payload_bytes, 9));
    FrameDecoder decoder;
    Frame out;
    std::uint64_t received = 0;
    std::uint64_t decode_failures = 0;
    const std::uint64_t allocs0 = AllocCount();
    const std::uint64_t bytes0 = AllocBytes();
    const auto begin = Clock::now();
    for (std::uint64_t i = 0; i < iterations; ++i) {
        for (std::size_t j = 0; j < bytes.size(); ++j) {
            decoder.Append(&bytes[j], 1);
            if (j + 1 == bytes.size()) {
                if (decoder.Next(&out) == FrameStatus::kOk) {
                    ++received;
                } else {
                    ++decode_failures;
                }
            } else if (decoder.Next(&out) != FrameStatus::kNeedMoreData) {
                ++decode_failures;
            }
        }
    }
    const double seconds = SecondsSince(begin);
    char name[96];
    std::snprintf(name, sizeof(name), "stream-bytewise payload=%zu", payload_bytes);
    Report(MakeMeasurement(name, iterations, iterations * (kGenericFrameHeaderBytes + payload_bytes),
                           seconds, AllocCount() - allocs0, AllocBytes() - bytes0));
    STRESS_CHECK_EQ(received, iterations);
    STRESS_CHECK_EQ(decode_failures, 0U);
}

// ---------------------------------------------------------------------------
// 基准 E：CRC 成本（帧校验和函数本身）。
// ---------------------------------------------------------------------------
void BenchChecksum(std::size_t payload_bytes, std::uint64_t iterations) {
    std::vector<std::uint8_t> buffer(kGenericFrameHeaderBytes + payload_bytes, 0x5a);
    std::uint32_t sink = 0;
    const std::uint64_t allocs0 = AllocCount();
    const std::uint64_t bytes0 = AllocBytes();
    const auto begin = Clock::now();
    for (std::uint64_t i = 0; i < iterations; ++i) {
        sink += ComputeFrameChecksum(buffer.data(), buffer.data() + kGenericFrameHeaderBytes,
                                     payload_bytes);
    }
    const double seconds = SecondsSince(begin);
    char name[96];
    std::snprintf(name, sizeof(name), "checksum-only payload=%zu", payload_bytes);
    Report(MakeMeasurement(name, iterations,
                           iterations * (kGenericFrameHeaderBytes + payload_bytes), seconds,
                           AllocCount() - allocs0, AllocBytes() - bytes0));
    if (sink == 0xdeadbeefu) {
        std::printf("  (checksum sink=%u)\n", sink);
    }
}

// ---------------------------------------------------------------------------
// 基准 F：缓冲压缩悬崖（小帧 + 大帧半包交替到达）。
//
// 旧实现每次 Append 都压缩已消费前缀；本场景每轮 Append 1 KiB、半包残留
// 始终保持 ~512 KiB，因此旧实现每轮都要搬移 512 KiB（64 轮 = 16 GiB），
// 新实现的阈值压缩（已消费前缀 ≥ 未消费字节才压缩）应几乎不搬移。
// ---------------------------------------------------------------------------
void BenchCompactionCliff(std::size_t iterations) {
    const Frame small = MakeFrame(1, 0, 0, 1);
    const Frame big = MakeFrame(2, 0, kFrameMaxPayload, 2);
    const std::vector<std::uint8_t> small_bytes = Encode(small);
    const std::vector<std::uint8_t> big_bytes = Encode(big);
    const std::size_t half = kGenericFrameHeaderBytes + kFrameMaxPayload / 2;
    constexpr std::size_t kChunk = 1024;
    std::uint64_t decode_failures = 0;
    const std::uint64_t allocs0 = AllocCount();
    const std::uint64_t bytes0 = AllocBytes();
    const auto begin = Clock::now();
    for (std::size_t i = 0; i < iterations; ++i) {
        FrameDecoder decoder;
        Frame out;
        decoder.Append(small_bytes.data(), small_bytes.size());
        decoder.Append(big_bytes.data(), half);
        if (decoder.Next(&out) != FrameStatus::kOk) {
            ++decode_failures;
        }
        std::size_t position = half;
        while (position < big_bytes.size()) {
            const std::size_t chunk = std::min(kChunk, big_bytes.size() - position);
            decoder.Append(big_bytes.data() + position, chunk);
            position += chunk;
            if (position < big_bytes.size() &&
                decoder.Next(&out) != FrameStatus::kNeedMoreData) {
                ++decode_failures;
            }
        }
        if (decoder.Next(&out) != FrameStatus::kOk || decoder.PendingBytes() != 0) {
            ++decode_failures;
        }
    }
    const double seconds = SecondsSince(begin);
    const std::uint64_t bytes = iterations * (small_bytes.size() + big_bytes.size());
    Report(MakeMeasurement("compaction-cliff payload=1MiB", iterations, bytes, seconds,
                           AllocCount() - allocs0, AllocBytes() - bytes0));
    STRESS_CHECK_EQ(decode_failures, 0U);
}

// ---------------------------------------------------------------------------
// 场景 1：粘包 + 半包（含「已消费前缀 + 半包残留」触发的缓冲压缩路径）。
// ---------------------------------------------------------------------------
void ScenarioStickyAndPartial() {
    std::printf("\n[scenario] sticky + partial packets\n");
    ResetRandom(0x1111);
    std::vector<Frame> frames;
    for (int i = 0; i < 100; ++i) {
        frames.push_back(MakeFrame(static_cast<std::uint16_t>(1000 + i),
                                   static_cast<std::uint16_t>(i),
                                   static_cast<std::size_t>(RandomNext() % 3000),
                                   static_cast<std::uint8_t>(i)));
    }
    const std::vector<std::uint8_t> stream = MakeStream(frames);
    {
        FrameDecoder decoder;
        decoder.Append(stream.data(), stream.size());
        Frame out;
        for (std::size_t i = 0; i < frames.size(); ++i) {
            STRESS_CHECK(decoder.Next(&out) == FrameStatus::kOk);
            STRESS_CHECK_EQ(out.type, frames[i].type);
            STRESS_CHECK_EQ(out.flags, frames[i].flags);
            STRESS_CHECK(out.payload == frames[i].payload);
        }
        STRESS_CHECK(decoder.Next(&out) == FrameStatus::kNeedMoreData);
        STRESS_CHECK_EQ(decoder.PendingBytes(), 0U);
    }
    {
        FrameDecoder decoder;
        Frame out;
        std::size_t received = 0;
        for (std::size_t i = 0; i < stream.size(); ++i) {
            decoder.Append(&stream[i], 1);
            while (decoder.Next(&out) == FrameStatus::kOk) {
                STRESS_CHECK(received < frames.size());
                if (received < frames.size()) {
                    STRESS_CHECK(out.payload == frames[received].payload);
                }
                ++received;
            }
        }
        STRESS_CHECK_EQ(received, frames.size());
    }
    {
        FrameDecoder decoder;
        Frame out;
        std::size_t received = 0;
        std::size_t position = 0;
        while (position < stream.size()) {
            const std::size_t chunk =
                std::min<std::size_t>(1 + RandomNext() % 7, stream.size() - position);
            decoder.Append(stream.data() + position, chunk);
            position += chunk;
            while (decoder.Next(&out) == FrameStatus::kOk) {
                STRESS_CHECK(received < frames.size());
                ++received;
            }
        }
        STRESS_CHECK_EQ(received, frames.size());
    }
    // 压缩路径：帧A + 帧B 前 5 字节 → Next 帧A（offset>0）→ Next 半包
    // （offset 仍 >0）→ Append 帧B 余下（触发 erase 压缩）→ Next 帧B。
    {
        const Frame frame_a = MakeFrame(1, 0, 10, 1);
        const Frame frame_b = MakeFrame(2, 0, 3000, 2);
        const std::vector<std::uint8_t> bytes_a = Encode(frame_a);
        const std::vector<std::uint8_t> bytes_b = Encode(frame_b);
        FrameDecoder decoder;
        decoder.Append(bytes_a.data(), bytes_a.size());
        decoder.Append(bytes_b.data(), 5);
        Frame out;
        STRESS_CHECK(decoder.Next(&out) == FrameStatus::kOk);
        STRESS_CHECK_EQ(out.type, frame_a.type);
        STRESS_CHECK_EQ(decoder.PendingBytes(), 5U);
        STRESS_CHECK(decoder.Next(&out) == FrameStatus::kNeedMoreData);
        STRESS_CHECK_EQ(decoder.PendingBytes(), 5U);
        decoder.Append(bytes_b.data() + 5, bytes_b.size() - 5);
        STRESS_CHECK(decoder.Next(&out) == FrameStatus::kOk);
        STRESS_CHECK_EQ(out.type, frame_b.type);
        STRESS_CHECK(out.payload == frame_b.payload);
        STRESS_CHECK_EQ(decoder.PendingBytes(), 0U);
    }
}

// ---------------------------------------------------------------------------
// 场景 2：1MiB 上限帧（边界值 / 边界+1 / 半包到达 / 篡改）。
// ---------------------------------------------------------------------------
void ScenarioMaxPayload() {
    std::printf("\n[scenario] 1 MiB max payload boundary\n");
    const Frame max_frame = MakeFrame(0x7777, 0x000f, kFrameMaxPayload, 11);
    const std::vector<std::uint8_t> max_bytes = Encode(max_frame);
    STRESS_CHECK_EQ(max_bytes.size(), kGenericFrameHeaderBytes + kFrameMaxPayload);
    STRESS_CHECK_EQ(ReadU32(max_bytes.data() + 12), kFrameMaxPayload);
    {
        Frame out;
        std::size_t consumed = 0;
        STRESS_CHECK(Decode(max_bytes.data(), max_bytes.size(), &out, &consumed) ==
                     FrameStatus::kOk);
        STRESS_CHECK_EQ(consumed, max_bytes.size());
        STRESS_CHECK_EQ(out.Length(), kFrameMaxPayload);
        STRESS_CHECK(out.payload == max_frame.payload);
        STRESS_CHECK_EQ(out.checksum, RefFrameCrc(max_bytes.data(), kFrameMaxPayload));
    }
    {
        FrameDecoder decoder;
        Frame out;
        const std::size_t half = kGenericFrameHeaderBytes + kFrameMaxPayload / 2;
        decoder.Append(max_bytes.data(), half);
        STRESS_CHECK(decoder.Next(&out) == FrameStatus::kNeedMoreData);
        STRESS_CHECK_EQ(decoder.PendingBytes(), half);
        decoder.Append(max_bytes.data() + half, max_bytes.size() - half);
        STRESS_CHECK(decoder.Next(&out) == FrameStatus::kOk);
        STRESS_CHECK(out.payload == max_frame.payload);
        STRESS_CHECK_EQ(decoder.PendingBytes(), 0U);
    }
    Frame oversize = max_frame;
    oversize.payload.resize(kFrameMaxPayload + 1);
    STRESS_CHECK(Encode(oversize).empty());
    std::vector<std::uint8_t> bad_header = max_bytes;
    WriteU32(bad_header.data() + 12, kFrameMaxPayload + 1);
    {
        Frame out;
        STRESS_CHECK(Decode(bad_header.data(), bad_header.size(), &out) ==
                     FrameStatus::kLengthExceeded);
    }
    {
        std::vector<std::uint8_t> tampered = max_bytes;
        tampered[kGenericFrameHeaderBytes + 100] ^= 0xffu;
        Frame out;
        STRESS_CHECK(Decode(tampered.data(), tampered.size(), &out) ==
                     FrameStatus::kChecksumMismatch);
    }
}

// ---------------------------------------------------------------------------
// 场景 3：非法帧族（每类错误 + 缓冲清空 + 恢复能力）。
// ---------------------------------------------------------------------------
void ScenarioInvalidFrames() {
    std::printf("\n[scenario] invalid frames\n");
    const std::vector<std::uint8_t> good = Encode(MakeFrame(0x4242, 0, 128, 6));

    struct Case {
        const char* name;
        FrameStatus expected;
        std::vector<std::uint8_t> bytes;
    };
    std::vector<Case> cases;
    {
        std::vector<std::uint8_t> bytes = good;
        bytes[0] ^= 0xffu;
        cases.push_back({"bad magic", FrameStatus::kBadMagic, bytes});
    }
    {
        std::vector<std::uint8_t> bytes = good;
        WriteU16(bytes.data() + 4, 2);
        cases.push_back({"bad version", FrameStatus::kBadVersion, bytes});
    }
    {
        std::vector<std::uint8_t> bytes = good;
        WriteU16(bytes.data() + 10, 1);
        cases.push_back({"bad reserved", FrameStatus::kBadReserved, bytes});
    }
    {
        std::vector<std::uint8_t> bytes = good;
        WriteU32(bytes.data() + 12, kFrameMaxPayload + 1);
        cases.push_back({"length exceeded", FrameStatus::kLengthExceeded, bytes});
    }
    {
        std::vector<std::uint8_t> bytes = good;
        bytes.back() ^= 0x01u;
        cases.push_back({"checksum mismatch", FrameStatus::kChecksumMismatch, bytes});
    }
    {
        std::vector<std::uint8_t> bytes = good;
        WriteU16(bytes.data() + 6, 0x9999);  // 篡改 type（CRC 覆盖头前缀）
        cases.push_back({"tampered type", FrameStatus::kChecksumMismatch, bytes});
    }
    for (const Case& item : cases) {
        Frame out;
        const FrameStatus status = Decode(item.bytes.data(), item.bytes.size(), &out);
        STRESS_CHECK_EQ(status, item.expected);
        FrameDecoder decoder;
        decoder.Append(item.bytes.data(), item.bytes.size());
        STRESS_CHECK_EQ(decoder.Next(&out), item.expected);
        STRESS_CHECK_EQ(decoder.PendingBytes(), 0U);
        decoder.Append(good.data(), good.size());
        STRESS_CHECK_EQ(decoder.Next(&out), FrameStatus::kOk);
        STRESS_CHECK_EQ(out.payload.size(), 128U);
        std::printf("  %-20s -> %-18s (recovered)\n", item.name, FrameStatusName(status));
    }
    {
        std::vector<std::uint8_t> stream = good;
        std::vector<std::uint8_t> bad = good;
        bad[0] ^= 0xffu;
        stream.insert(stream.end(), bad.begin(), bad.end());
        FrameDecoder decoder;
        decoder.Append(stream.data(), stream.size());
        Frame out;
        STRESS_CHECK_EQ(decoder.Next(&out), FrameStatus::kOk);
        STRESS_CHECK_EQ(decoder.Next(&out), FrameStatus::kBadMagic);
        STRESS_CHECK_EQ(decoder.PendingBytes(), 0U);
    }
    {
        Frame out;
        STRESS_CHECK_EQ(Decode(nullptr, 0, &out), FrameStatus::kNeedMoreData);
        STRESS_CHECK_EQ(Decode(good.data(), good.size(), nullptr), FrameStatus::kInvalidArgument);
        STRESS_CHECK_EQ(Decode(nullptr, 1, &out), FrameStatus::kInvalidArgument);
        const FrameFormat zero_magic{0, 1};
        STRESS_CHECK_EQ(Decode(zero_magic, good.data(), good.size(), &out),
                        FrameStatus::kInvalidArgument);
        FrameDecoder decoder(0);
        STRESS_CHECK_EQ(decoder.MaxPayload(), 0U);
        STRESS_CHECK_EQ(decoder.Next(nullptr), FrameStatus::kInvalidArgument);
    }
    {
        FrameDecoder decoder(0);
        const std::vector<std::uint8_t> empty_frame = Encode(MakeFrame(1, 0, 0, 0));
        const std::vector<std::uint8_t> one_byte = Encode(MakeFrame(1, 0, 1, 0));
        decoder.Append(empty_frame.data(), empty_frame.size());
        Frame out;
        STRESS_CHECK_EQ(decoder.Next(&out), FrameStatus::kOk);
        decoder.Append(one_byte.data(), one_byte.size());
        STRESS_CHECK_EQ(decoder.Next(&out), FrameStatus::kLengthExceeded);
        STRESS_CHECK_EQ(decoder.PendingBytes(), 0U);
    }
}

// ---------------------------------------------------------------------------
// 场景 4：并发编解码（每线程独立 FrameDecoder / Frame）。
// ---------------------------------------------------------------------------
void ScenarioConcurrent(int threads, std::uint64_t frames_per_thread) {
    std::printf("\n[scenario] concurrent encode/decode threads=%d frames/thread=%llu\n", threads,
                static_cast<unsigned long long>(frames_per_thread));
    constexpr std::size_t kPayload = 512;
    std::vector<Frame> frames(64, MakeFrame(0x5151, 0x0003, kPayload, 17));
    const std::vector<std::uint8_t> stream = MakeStream(frames);

    std::atomic<std::uint64_t> total{0};
    std::atomic<int> failures{0};
    const auto begin = Clock::now();
    std::vector<std::thread> workers;
    workers.reserve(static_cast<std::size_t>(threads));
    for (int t = 0; t < threads; ++t) {
        workers.emplace_back([&, t]() {
            FrameDecoder decoder;
            Frame out;
            std::uint64_t sum = 0;
            std::uint64_t produced = 0;
            std::size_t position = 0;
            std::uint64_t rng = 0x1234u + static_cast<std::uint64_t>(t) * 0x9e3779b9u;
            while (produced < frames_per_thread) {
                if (position >= stream.size()) {
                    position = 0;
                }
                const std::size_t chunk = std::min<std::size_t>(
                    1 + static_cast<std::size_t>(rng % 97u), stream.size() - position);
                decoder.Append(stream.data() + position, chunk);
                position += chunk;
                FrameStatus status = FrameStatus::kNeedMoreData;
                while ((status = decoder.Next(&out)) == FrameStatus::kOk) {
                    if (out.payload.size() != kPayload) {
                        failures.fetch_add(1);
                    } else {
                        sum += out.payload[0];
                        sum += out.payload[kPayload - 1];
                    }
                    ++produced;
                    if (produced >= frames_per_thread) {
                        break;
                    }
                }
                if (status != FrameStatus::kOk && status != FrameStatus::kNeedMoreData) {
                    failures.fetch_add(1);
                    break;
                }
            }
            total.fetch_add(sum, std::memory_order_relaxed);
        });
    }
    for (std::thread& worker : workers) {
        worker.join();
    }
    const double seconds = SecondsSince(begin);
    STRESS_CHECK_EQ(failures.load(), 0);
    const std::uint64_t frames_total = static_cast<std::uint64_t>(threads) * frames_per_thread;
    char name[96];
    std::snprintf(name, sizeof(name), "concurrent threads=%d payload=%zu", threads, kPayload);
    Report(MakeMeasurement(name, frames_total,
                           frames_total * (kGenericFrameHeaderBytes + kPayload), seconds, 0, 0));
    std::printf("  concurrent payload sum=%llu\n",
                static_cast<unsigned long long>(total.load()));
    STRESS_CHECK(total.load() > 0);
}

// ---------------------------------------------------------------------------
// 场景 5：长稳（连续 N 帧，RSS / 净分配不增长）。
// ---------------------------------------------------------------------------
void ScenarioLongRun(std::uint64_t frames) {
    std::printf("\n[scenario] long run frames=%llu\n", static_cast<unsigned long long>(frames));
    constexpr std::size_t kPayload = 1400;
    const std::vector<std::uint8_t> bytes = Encode(MakeFrame(0x6161, 0x0001, kPayload, 23));
    const std::uint64_t rss_begin = RssKb();
    const std::uint64_t allocs_begin = AllocCount();
    const std::uint64_t alloc_bytes_begin = AllocBytes();
    FrameDecoder decoder;
    Frame out;
    std::uint64_t received = 0;
    std::uint64_t sum = 0;
    std::uint64_t rss_peak = rss_begin;
    const auto begin = Clock::now();
    for (std::uint64_t i = 0; i < frames; ++i) {
        decoder.Append(bytes.data(), bytes.size());
        if (decoder.Next(&out) != FrameStatus::kOk) {
            ++g_failures;
            std::fprintf(stderr, "[FAIL] long run: frame %llu did not decode\n",
                         static_cast<unsigned long long>(i));
            break;
        }
        sum += out.payload[kPayload / 2];
        ++received;
        if ((i % 100000u) == 0) {
            const std::uint64_t rss = RssKb();
            rss_peak = std::max(rss_peak, rss);
            std::printf("  progress=%llu rss=%llu KiB\n",
                        static_cast<unsigned long long>(i),
                        static_cast<unsigned long long>(rss));
        }
    }
    const double seconds = SecondsSince(begin);
    const std::uint64_t rx_bytes = received * (kGenericFrameHeaderBytes + kPayload);
    char name[96];
    std::snprintf(name, sizeof(name), "long-run payload=%zu", kPayload);
    Report(MakeMeasurement(name, received, rx_bytes, seconds, AllocCount() - allocs_begin,
                           AllocBytes() - alloc_bytes_begin));
    STRESS_CHECK_EQ(received, frames);
    STRESS_CHECK_EQ(decoder.PendingBytes(), 0U);
    const std::uint64_t rss_end = RssKb();
    const std::uint64_t rss_growth = rss_end > rss_begin ? rss_end - rss_begin : 0;
    std::printf(
        "  rss begin=%llu KiB end=%llu KiB peak=%llu KiB growth=%llu KiB allocs_delta=%llu\n",
        static_cast<unsigned long long>(rss_begin), static_cast<unsigned long long>(rss_end),
        static_cast<unsigned long long>(rss_peak), static_cast<unsigned long long>(rss_growth),
        static_cast<unsigned long long>(AllocCount() - allocs_begin));
    STRESS_CHECK(rss_growth < 8u * 1024u * 1024u);
    STRESS_CHECK(sum > 0);
}

// ---------------------------------------------------------------------------
// 随机测试：合法/非法混合，随机块切分，与参考模型对拍。
// ---------------------------------------------------------------------------
struct RandomCaseResult {
    std::uint64_t ok_frames = 0;
    FrameStatus final_status = FrameStatus::kNeedMoreData;
    std::uint64_t expected_ok_frames = 0;
    FrameStatus expected_status = FrameStatus::kNeedMoreData;
};

RandomCaseResult RunRandomCase(std::uint64_t seed, bool* stream_truncated) {
    ResetRandom(seed);
    RandomCaseResult result;
    const int frame_count = 1 + static_cast<int>(RandomNext() % 12);
    std::vector<Frame> frames;
    std::vector<std::uint8_t> stream;
    for (int i = 0; i < frame_count; ++i) {
        Frame frame = MakeFrame(static_cast<std::uint16_t>(RandomNext()),
                                static_cast<std::uint16_t>(RandomNext()),
                                static_cast<std::size_t>(RandomNext() % 2048),
                                static_cast<std::uint8_t>(RandomNext()));
        const std::vector<std::uint8_t> bytes = Encode(frame);
        stream.insert(stream.end(), bytes.begin(), bytes.end());
        frames.push_back(std::move(frame));
    }
    const bool inject_bad = (RandomNext() % 100u) < 30u;
    if (inject_bad) {
        std::vector<std::uint8_t> bad = Encode(MakeFrame(1, 0, 16, 1));
        switch (RandomNext() % 5u) {
            case 0:
                bad[0] ^= 0xffu;
                break;
            case 1:
                WriteU16(bad.data() + 4, static_cast<std::uint16_t>(kFrameVersion + 1));
                break;
            case 2:
                WriteU16(bad.data() + 10, 1);
                break;
            case 3:
                WriteU32(bad.data() + 12, kFrameMaxPayload + 1);
                break;
            default:
                bad.back() ^= 0x5au;
                break;
        }
        stream.insert(stream.end(), bad.begin(), bad.end());
    }
    *stream_truncated = false;
    if ((RandomNext() % 100u) < 35u && !stream.empty()) {
        const std::size_t cut = 1 + static_cast<std::size_t>(RandomNext() % 30u);
        const std::size_t keep = stream.size() > cut ? stream.size() - cut : 1;
        stream.resize(keep);
        *stream_truncated = true;
    }

    {
        std::size_t offset = 0;
        for (;;) {
            std::size_t consumed = 0;
            const ModelStatus status =
                ModelNext(stream.data() + offset, stream.size() - offset, &consumed);
            if (status == ModelStatus::kOk) {
                ++result.expected_ok_frames;
                offset += consumed;
                continue;
            }
            result.expected_status = ToFrameStatus(status);
            break;
        }
    }

    FrameDecoder decoder;
    Frame out;
    std::size_t position = 0;
    bool stopped = false;
    while (position < stream.size() && !stopped) {
        const std::size_t chunk =
            std::min<std::size_t>(1 + RandomNext() % 113u, stream.size() - position);
        decoder.Append(stream.data() + position, chunk);
        position += chunk;
        for (;;) {
            const FrameStatus status = decoder.Next(&out);
            if (status == FrameStatus::kOk) {
                const std::size_t index = static_cast<std::size_t>(result.ok_frames);
                STRESS_CHECK(index < frames.size());
                if (index < frames.size()) {
                    STRESS_CHECK_EQ(out.type, frames[index].type);
                    STRESS_CHECK_EQ(out.flags, frames[index].flags);
                    STRESS_CHECK(out.payload == frames[index].payload);
                }
                ++result.ok_frames;
                continue;
            }
            if (status == FrameStatus::kNeedMoreData) {
                result.final_status = status;
                break;
            }
            result.final_status = status;
            stopped = true;
            break;
        }
    }
    STRESS_CHECK_EQ(result.ok_frames, result.expected_ok_frames);
    STRESS_CHECK_EQ(result.final_status, result.expected_status);
    if (stopped) {
        STRESS_CHECK_EQ(decoder.PendingBytes(), 0U);
    } else if (result.final_status == FrameStatus::kNeedMoreData) {
        std::size_t offset = 0;
        for (std::uint64_t i = 0; i < result.expected_ok_frames; ++i) {
            std::size_t consumed = 0;
            (void)ModelNext(stream.data() + offset, stream.size() - offset, &consumed);
            offset += consumed;
        }
        STRESS_CHECK_EQ(decoder.PendingBytes(), stream.size() - offset);
    }
    return result;
}

void ScenarioRandom(std::uint64_t cases) {
    std::printf("\n[scenario] random adversarial cases=%llu\n",
                static_cast<unsigned long long>(cases));
    std::uint64_t truncated = 0;
    std::uint64_t errors = 0;
    std::uint64_t ok_frames = 0;
    const auto begin = Clock::now();
    for (std::uint64_t i = 0; i < cases; ++i) {
        bool stream_truncated = false;
        const RandomCaseResult result = RunRandomCase(0xabc000ull + i * 7919ull, &stream_truncated);
        truncated += stream_truncated ? 1u : 0u;
        errors += (result.final_status != FrameStatus::kNeedMoreData) ? 1u : 0u;
        ok_frames += result.ok_frames;
    }
    const double seconds = SecondsSince(begin);
    std::printf("  cases=%llu ok_frames=%llu truncated_streams=%llu error_streams=%llu sec=%.3f\n",
                static_cast<unsigned long long>(cases),
                static_cast<unsigned long long>(ok_frames),
                static_cast<unsigned long long>(truncated),
                static_cast<unsigned long long>(errors), seconds);
}

// ---------------------------------------------------------------------------
// TSV 输出（供审查脚本与优化前后对比）。
// ---------------------------------------------------------------------------
void WriteTsv(const char* path) {
    std::FILE* file = std::fopen(path, "w");
    if (file == nullptr) {
        std::fprintf(stderr, "cannot write %s\n", path);
        return;
    }
    std::fprintf(file,
                 "name\tframes\tbytes\tseconds\tns_per_frame\tfps\tmib_per_sec\tallocs\t"
                 "alloc_bytes\n");
    for (const Measurement& m : g_results) {
        std::fprintf(file, "%s\t%llu\t%llu\t%.6f\t%.2f\t%.1f\t%.3f\t%llu\t%llu\n", m.name.c_str(),
                     static_cast<unsigned long long>(m.frames),
                     static_cast<unsigned long long>(m.bytes), m.seconds, m.ns_per_frame, m.fps,
                     m.mib_per_sec, static_cast<unsigned long long>(m.allocs),
                     static_cast<unsigned long long>(m.alloc_bytes));
    }
    std::fclose(file);
}

}  // namespace

// ---------------------------------------------------------------------------
// 全局分配计数（替换 operator new/delete）。
// ---------------------------------------------------------------------------
void* operator new(std::size_t size) {
    g_alloc_count.fetch_add(1, std::memory_order_relaxed);
    g_alloc_bytes.fetch_add(size, std::memory_order_relaxed);
    void* pointer = std::malloc(size == 0 ? 1 : size);
    if (pointer == nullptr) {
        throw std::bad_alloc();
    }
    return pointer;
}

void* operator new[](std::size_t size) { return ::operator new(size); }

void operator delete(void* pointer) noexcept { std::free(pointer); }
void operator delete[](void* pointer) noexcept { std::free(pointer); }
void operator delete(void* pointer, std::size_t) noexcept { std::free(pointer); }
void operator delete[](void* pointer, std::size_t) noexcept { std::free(pointer); }

int main(int argc, char** argv) {
    bool quick = false;
    const char* tsv_path = "/tmp/frame_bench.tsv";
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--quick") == 0) {
            quick = true;
        } else if (std::strncmp(argv[i], "--tsv=", 6) == 0) {
            tsv_path = argv[i] + 6;
        }
    }
    std::printf("=== frame stress (quick=%d) ===\n", quick ? 1 : 0);

    const std::uint64_t small_iters = quick ? 200000ull : 1000000ull;
    const std::uint64_t large_iters = quick ? 64ull : 256ull;

    BenchEncode(0, small_iters);
    BenchEncode(64, small_iters);
    BenchEncode(512, small_iters);
    BenchEncode(1400, small_iters);
    BenchEncode(8192, quick ? 100000ull : 500000ull);
    BenchEncode(65536, quick ? 4096ull : 16384ull);
    BenchEncode(kFrameMaxPayload, large_iters);

    BenchDecode(64, small_iters, true);
    BenchDecode(64, small_iters, false);
    BenchDecode(1400, small_iters, true);
    BenchDecode(1400, small_iters, false);
    BenchDecode(kFrameMaxPayload, large_iters, true);
    BenchDecode(kFrameMaxPayload, large_iters, false);

    BenchDecodeSticky(64, quick ? 100000ull : 500000ull);
    BenchDecodeSticky(1400, quick ? 50000ull : 250000ull);
    BenchDecodeBytewise(1400, quick ? 2000ull : 10000ull);

    BenchChecksum(64, small_iters);
    BenchChecksum(1400, small_iters);
    BenchChecksum(65536, quick ? 4096ull : 16384ull);
    BenchChecksum(kFrameMaxPayload, large_iters);

    ScenarioStickyAndPartial();
    BenchCompactionCliff(quick ? 8 : 64);
    ScenarioMaxPayload();
    ScenarioInvalidFrames();
    ScenarioConcurrent(quick ? 2 : 4, quick ? 20000ull : 100000ull);
    ScenarioConcurrent(quick ? 4 : 12, quick ? 10000ull : 50000ull);
    ScenarioLongRun(quick ? 100000ull : 1000000ull);

    ScenarioRandom(quick ? 200ull : 1500ull);

    WriteTsv(tsv_path);
    std::printf("\n=== summary: checks=%d failures=%d ===\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
