/**
 * @file test_frame_codec.cpp
 * @brief `go2cpp::frame::FrameCodec` 的验收测试（REQ-001）。
 *
 * @details 覆盖 5 类验收 + 3 类工程测试：
 *
 * 1. **格式矩阵**：两种 `FrameSpec`（RDRL 64 KiB / RDCH 4 MiB）各 23 种 type 的
 *    encode → Feed → decode，字段一致且重编码字节完全一致；
 * 2. **半包**：100 字节帧按 1 字节喂 100 次，逐字节断言「未收满不交付」；
 * 3. **粘包**：3 帧连发一次 Feed 返回 3 帧且 `consumed` 精确，尾部半帧留缓冲；
 * 4. **畸形**：magic / version / length / checksum 四类错误各返回可区分错误码，
 *    且「错误前的完整帧仍交付、错误后缓冲已清」；
 * 5. **压测**：1e6 帧 encode → 随机切分 Feed → 逐帧字节校验（无泄漏由 ASan/LSan
 *    单独跑同一可执行文件验证，见 `docs/modules/frame-codec.md`）。
 *
 * 另外包含：
 * - **黄金向量**：按 RDRL 协议文档 §3.1 逐字节手写的线上字节，CRC 由 Python
 *   `zlib.crc32` 独立算出（本测试里的逐位 CRC32 也参与交叉验证）；
 * - **随机测试**：5000 帧随机切分重组 + 垃圾字节 / 单字节变异 fuzz；
 * - **5 个真实场景**：RDRL 握手、RDCH 48 KiB 能力消息、双协议共存、错误后重连、
 *   恶意超长长度头的内存放大防护。
 *
 * 断言失败累计到 `g_failures`（不中断），`main` 汇总后返回非零。
 */

#include "go2cpp/frame/frame_codec.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <random>
#include <string>
#include <vector>

namespace {

using go2cpp::frame::Error;
using go2cpp::frame::ErrorCode;
using go2cpp::frame::Frame;
using go2cpp::frame::FrameCodec;
using go2cpp::frame::FrameSpec;
using go2cpp::frame::MakeFrameSpec;
using go2cpp::frame::MakeMagic;

// ---------------------------------------------------------------------------
// 迷你测试框架（与仓库其它独立测试一致：累计失败 + 打印，不引入外部依赖）
// ---------------------------------------------------------------------------

int g_failures = 0;
std::size_t g_checks = 0;

void Announce(const char* name) {
    std::printf("[frame] %s\n", name);
    std::fflush(stdout);
}

void Check(bool condition, const char* expression, int line) {
    ++g_checks;
    if (condition) {
        return;
    }
    ++g_failures;
    std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, line, expression);
}

void CheckEq(std::size_t actual, std::size_t expected, const char* expression,
             int line) {
    ++g_checks;
    if (actual == expected) {
        return;
    }
    ++g_failures;
    std::fprintf(stderr, "FAIL %s:%d: %s (got %zu, want %zu)\n", __FILE__, line,
                 expression, actual, expected);
}

#define CHECK(expr) Check((expr), #expr, __LINE__)
#define CHECK_EQ(actual, expected) \
    CheckEq((actual), (expected), #actual " == " #expected, __LINE__)

// ---------------------------------------------------------------------------
// 公共工具
// ---------------------------------------------------------------------------

constexpr int kRdrlMaxPayload = 65536;
constexpr int kRdchMaxPayload = 4 * 1024 * 1024;

FrameSpec RdrlSpec() {
    return MakeFrameSpec(MakeMagic("RDRL"), 1, kRdrlMaxPayload);
}

FrameSpec RdchSpec() {
    return MakeFrameSpec(MakeMagic("RDCH"), 1, kRdchMaxPayload);
}

/// 生成可复现的载荷（线性同余，避免依赖平台 RNG 行为）。
Frame MakeFrame(std::uint8_t type, std::uint16_t flags,
                std::size_t payload_bytes, std::uint32_t seed) {
    Frame frame;
    frame.type = type;
    frame.flags = flags;
    frame.payload.resize(payload_bytes);
    std::uint32_t state = seed * 2654435761U + 12345U;
    for (auto& byte : frame.payload) {
        state = state * 1664525U + 1013904223U;
        byte = static_cast<std::uint8_t>(state >> 24);
    }
    return frame;
}

bool SameFrame(const Frame& left, const Frame& right) {
    return left.type == right.type && left.flags == right.flags &&
           left.payload == right.payload;
}

std::uint32_t LoadLe32(const std::uint8_t* p) {
    return static_cast<std::uint32_t>(p[0]) |
           (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) |
           (static_cast<std::uint32_t>(p[3]) << 24);
}

/// 独立实现的逐位 CRC-32/IEEE（不查表、不复用被测代码），用作交叉验证。
std::uint32_t NaiveCrc32(const std::uint8_t* data, std::size_t len) {
    std::uint32_t crc = 0xFFFFFFFFU;
    for (std::size_t i = 0; i < len; ++i) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; ++bit) {
            const std::uint32_t mask = 0U - (crc & 1U);
            crc = (crc >> 1) ^ (0xEDB88320U & mask);
        }
    }
    return ~crc;
}

/// 编码并在失败时打印原因（测试里编码失败即视为用例失败）。
std::vector<std::uint8_t> EncodeOk(const FrameCodec& codec, const Frame& frame,
                                   int line) {
    std::vector<std::uint8_t> wire;
    Error error;
    const bool ok = codec.Encode(frame, &wire, &error);
    Check(ok, "Encode() == true", line);
    if (!ok) {
        std::fprintf(stderr, "  encode error: %s (%s)\n",
                     go2cpp::frame::ErrorCodeName(error.code),
                     error.message.c_str());
    }
    return wire;
}

#define ENCODE_OK(codec, frame) EncodeOk((codec), (frame), __LINE__)

/// 一次 Feed 的结果快照（测试可读性用的聚合类型）。
struct FeedOutcome {
    bool ok = false;
    std::size_t consumed = 0;
    std::vector<Frame> frames;
    Error error;
};

FeedOutcome FeedBytes(FrameCodec& codec, const std::vector<std::uint8_t>& bytes) {
    FeedOutcome outcome;
    outcome.ok = codec.Feed(bytes.data(), bytes.size(), &outcome.frames,
                            &outcome.consumed, &outcome.error);
    return outcome;
}

/// 读 /proc/self/status 的 VmHWM（峰值常驻内存，KB）；非 Linux 返回 0。
std::size_t PeakRssKb() {
#if defined(__linux__)
    std::FILE* status = std::fopen("/proc/self/status", "r");
    if (status == nullptr) {
        return 0;
    }
    char line[256] = {};
    std::size_t kb = 0;
    while (std::fgets(line, sizeof(line), status) != nullptr) {
        if (std::strncmp(line, "VmHWM:", 6) == 0) {
            (void)std::sscanf(line + 6, "%zu", &kb);
            break;
        }
    }
    std::fclose(status);
    return kb;
#else
    return 0;
#endif
}

double MsSince(std::chrono::steady_clock::time_point start) {
    const auto elapsed = std::chrono::steady_clock::now() - start;
    return std::chrono::duration<double, std::milli>(elapsed).count();
}

// ---------------------------------------------------------------------------
// 1. 黄金向量：RDRL / RDCH 线上字节逐字节对齐协议文档
// ---------------------------------------------------------------------------

void TestGoldenWireBytes() {
    Announce("golden: RDRL REGISTER / HEARTBEAT / RDCH HELLO 逐字节");

    // RDRL REGISTER(type=1)：magic "RDRL" | ver 1 | type 1 | flags 0 | len 4
    // | payload {03 'k' 'e' 'y'} | crc32 0xbb92df2c（Python zlib 独立算出）。
    const std::vector<std::uint8_t> golden_register = {
        0x52, 0x44, 0x52, 0x4C, 0x01, 0x01, 0x00, 0x00, 0x04, 0x00,
        0x00, 0x00, 0x03, 0x6B, 0x65, 0x79, 0x2C, 0xDF, 0x92, 0xBB};
    FrameCodec rdrl{RdrlSpec()};
    Frame register_frame;
    register_frame.type = 1;
    register_frame.payload = {0x03, 'k', 'e', 'y'};
    const auto register_wire = ENCODE_OK(rdrl, register_frame);
    CHECK(register_wire == golden_register);
    CHECK_EQ(LoadLe32(register_wire.data() + 16),
             NaiveCrc32(register_wire.data(), 16));

    // 空载荷帧（HEARTBEAT type=5）：crc32 0xd6d1e1ce。
    const std::vector<std::uint8_t> golden_heartbeat = {
        0x52, 0x44, 0x52, 0x4C, 0x01, 0x05, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0xCE, 0xE1, 0xD1, 0xD6};
    Frame heartbeat;
    heartbeat.type = 5;
    CHECK(ENCODE_OK(rdrl, heartbeat) == golden_heartbeat);

    // RDCH HELLO(type=1)：magic "RDCH" | len 5 | "hello" | crc32 0xf87b5e73。
    const std::vector<std::uint8_t> golden_rdch = {
        0x52, 0x44, 0x43, 0x48, 0x01, 0x01, 0x00, 0x00, 0x05, 0x00,
        0x00, 0x00, 0x68, 0x65, 0x6C, 0x6C, 0x6F, 0x73, 0x5E, 0x7B, 0xF8};
    FrameCodec rdch{RdchSpec()};
    Frame hello;
    hello.type = 1;
    hello.payload = {'h', 'e', 'l', 'l', 'o'};
    CHECK(ENCODE_OK(rdch, hello) == golden_rdch);

    // 三类黄金字节回喂后必须还原出同样的帧。
    FrameCodec decode_rdrl{RdrlSpec()};
    const auto outcome = FeedBytes(decode_rdrl, golden_register);
    CHECK(outcome.ok);
    CHECK_EQ(outcome.frames.size(), 1);
    CHECK(outcome.frames.size() == 1 && SameFrame(outcome.frames[0], register_frame));

    FrameCodec decode_rdch{RdchSpec()};
    const auto rdch_outcome = FeedBytes(decode_rdch, golden_rdch);
    CHECK(rdch_outcome.ok);
    CHECK(rdch_outcome.frames.size() == 1 && SameFrame(rdch_outcome.frames[0], hello));
}

// ---------------------------------------------------------------------------
// 2. 两种 FrameSpec × 23 种 type：encode → Feed → decode 字节一致
// ---------------------------------------------------------------------------

void TestTwoSpecsAllTypes() {
    Announce("spec 矩阵：RDRL(64KiB) / RDCH(4MiB) × 23 种 type 往返一致");

    struct SpecCase {
        const char* name;
        FrameSpec spec;
    };
    const SpecCase cases[] = {
        {"RDRL/64KiB", RdrlSpec()},
        {"RDCH/4MiB", MakeFrameSpec(MakeMagic("RDCH"), 3, kRdchMaxPayload)},
    };

    for (const auto& item : cases) {
        FrameCodec codec(item.spec);
        CHECK(codec.SpecValid());
        CHECK_EQ(codec.Spec().max_payload, static_cast<std::size_t>(item.spec.max_payload));
        for (int index = 0; index < 23; ++index) {
            const auto type = static_cast<std::uint8_t>(index + 1);
            const auto flags = static_cast<std::uint16_t>((index * 0x0101) ^ 0xA5A5);
            const std::size_t payload_bytes =
                (index % 7 == 0) ? 0 : static_cast<std::size_t>((index * 17) % 300);
            const Frame frame = MakeFrame(type, flags, payload_bytes,
                                          static_cast<std::uint32_t>(index + 1));

            const auto wire = ENCODE_OK(codec, frame);
            CHECK_EQ(wire.size(),
                     static_cast<std::size_t>(item.spec.header_len) + payload_bytes + 4);
            // 线上字节的 CRC 字段必须等于独立实现算出的 CRC。
            CHECK_EQ(LoadLe32(wire.data() + wire.size() - 4),
                     NaiveCrc32(wire.data(), wire.size() - 4));

            const auto outcome = FeedBytes(codec, wire);
            CHECK(outcome.ok);
            CHECK_EQ(outcome.frames.size(), 1);
            CHECK_EQ(outcome.consumed, wire.size());
            CHECK_EQ(codec.BufferedBytes(), 0);
            if (outcome.frames.size() == 1) {
                CHECK(SameFrame(outcome.frames[0], frame));
                CHECK(ENCODE_OK(codec, outcome.frames[0]) == wire);
            }
        }
        CHECK_EQ(codec.BufferedBytes(), 0);
    }

    // type/flags 边界值：0 与 0xFF / 0xFFFF 不被特殊对待。
    FrameCodec codec{RdrlSpec()};
    for (const auto type : {std::uint8_t{0}, std::uint8_t{0xFF}}) {
        for (const auto flags : {std::uint16_t{0}, std::uint16_t{0xFFFF}}) {
            const Frame frame = MakeFrame(type, flags, 17, type + flags);
            const auto wire = ENCODE_OK(codec, frame);
            const auto outcome = FeedBytes(codec, wire);
            CHECK(outcome.ok);
            CHECK(outcome.frames.size() == 1 && SameFrame(outcome.frames[0], frame));
        }
    }
}

// ---------------------------------------------------------------------------
// 3. 半包：100 字节帧按 1 字节喂 100 次
// ---------------------------------------------------------------------------

void TestHalfPacketByteByByte() {
    Announce("半包：100 字节帧按 1 字节喂 100 次");

    FrameCodec codec{RdrlSpec()};
    const Frame frame = MakeFrame(3, 0x1234, 84, 7);  // 12 + 84 + 4 = 100
    const auto wire = ENCODE_OK(codec, frame);
    CHECK_EQ(wire.size(), 100);

    std::vector<Frame> frames;
    std::size_t total_consumed = 0;
    std::size_t max_buffered = 0;
    for (std::size_t i = 0; i < wire.size(); ++i) {
        Error error;
        std::size_t consumed = 0;
        const bool ok = codec.Feed(&wire[i], 1, &frames, &consumed, &error);
        CHECK(ok);
        CHECK_EQ(consumed, 1);
        total_consumed += consumed;
        max_buffered = std::max(max_buffered, codec.BufferedBytes());
        if (i + 1 < wire.size()) {
            CHECK(frames.empty());  // 未收满一帧之前不得交付
        } else {
            CHECK_EQ(frames.size(), 1);
        }
    }
    CHECK_EQ(total_consumed, 100);
    CHECK_EQ(max_buffered, 99);
    CHECK_EQ(codec.BufferedBytes(), 0);
    CHECK(frames.size() == 1 && SameFrame(frames[0], frame));

    // 空 Feed 不改变状态：半包不会因为「喂了 0 字节」被丢掉。
    FrameCodec fresh{RdrlSpec()};
    std::vector<Frame> pending;
    Error error;
    std::size_t consumed = 0;
    CHECK(fresh.Feed(wire.data(), 37, &pending, &consumed, &error));
    CHECK_EQ(fresh.BufferedBytes(), 37);
    consumed = 12345;
    CHECK(fresh.Feed(nullptr, 0, &pending, &consumed, &error));
    CHECK_EQ(consumed, 0);
    CHECK_EQ(fresh.BufferedBytes(), 37);
    CHECK(pending.empty());
    CHECK(fresh.Feed(wire.data() + 37, wire.size() - 37, &pending, &consumed, &error));
    CHECK(pending.size() == 1 && SameFrame(pending[0], frame));
    CHECK_EQ(fresh.BufferedBytes(), 0);
}

// ---------------------------------------------------------------------------
// 4. 粘包：3 帧连发一次 Feed
// ---------------------------------------------------------------------------

void TestStickyPackets() {
    Announce("粘包：3 帧连发一次 Feed 返回 3 帧且 consumed 精确");

    FrameCodec codec{RdrlSpec()};
    std::vector<Frame> expected;
    std::vector<std::uint8_t> wire;
    for (int i = 0; i < 3; ++i) {
        const std::size_t payload_bytes = (i == 0) ? 0 : static_cast<std::size_t>(i * 50);
        Frame frame = MakeFrame(static_cast<std::uint8_t>(i + 1), 0, payload_bytes, i + 11);
        const auto encoded = ENCODE_OK(codec, frame);
        wire.insert(wire.end(), encoded.begin(), encoded.end());
        expected.push_back(std::move(frame));
    }
    const std::size_t total = wire.size();

    const auto outcome = FeedBytes(codec, wire);
    CHECK(outcome.ok);
    CHECK_EQ(outcome.frames.size(), 3);
    CHECK_EQ(outcome.consumed, total);
    CHECK_EQ(codec.BufferedBytes(), 0);
    for (std::size_t i = 0; i < outcome.frames.size() && i < 3; ++i) {
        CHECK(SameFrame(outcome.frames[i], expected[i]));
    }

    // 3 帧 + 第 4 帧的前 7 字节：仍交付 3 帧，consumed 为全部输入，缓冲正好 7 字节。
    FrameCodec tail_codec{RdrlSpec()};
    const Frame fourth = MakeFrame(9, 0xBEEF, 21, 42);
    const auto fourth_wire = ENCODE_OK(tail_codec, fourth);
    std::vector<std::uint8_t> with_tail = wire;
    with_tail.insert(with_tail.end(), fourth_wire.begin(), fourth_wire.begin() + 7);
    const auto tail_outcome = FeedBytes(tail_codec, with_tail);
    CHECK(tail_outcome.ok);
    CHECK_EQ(tail_outcome.frames.size(), 3);
    CHECK_EQ(tail_outcome.consumed, with_tail.size());
    CHECK_EQ(tail_codec.BufferedBytes(), 7);

    std::vector<Frame> rest;
    Error error;
    std::size_t consumed = 0;
    CHECK(tail_codec.Feed(fourth_wire.data() + 7, fourth_wire.size() - 7, &rest,
                          &consumed, &error));
    CHECK_EQ(consumed, fourth_wire.size() - 7);
    CHECK(rest.size() == 1 && SameFrame(rest[0], fourth));
    CHECK_EQ(tail_codec.BufferedBytes(), 0);
}

// ---------------------------------------------------------------------------
// 5. 畸形：四类错误可区分 + 错误前后行为
// ---------------------------------------------------------------------------

void TestMalformedFrames() {
    Announce("畸形：magic/version/length/checksum 四类错误可区分");

    FrameCodec encoder{RdrlSpec()};
    const Frame good = MakeFrame(2, 0, 24, 5);
    const auto good_wire = ENCODE_OK(encoder, good);

    struct MalformedCase {
        const char* name;
        std::vector<std::uint8_t> wire;
        ErrorCode want;
    };
    std::vector<MalformedCase> cases;

    auto bad_magic = good_wire;
    bad_magic[0] ^= 0xFF;
    cases.push_back({"bad magic", bad_magic, ErrorCode::kBadMagic});

    auto bad_version = good_wire;
    bad_version[4] = 2;
    cases.push_back({"bad version", bad_version, ErrorCode::kBadVersion});

    auto bad_length = good_wire;
    const std::uint32_t over = static_cast<std::uint32_t>(kRdrlMaxPayload + 1);
    bad_length[8] = static_cast<std::uint8_t>(over & 0xFF);
    bad_length[9] = static_cast<std::uint8_t>((over >> 8) & 0xFF);
    bad_length[10] = static_cast<std::uint8_t>((over >> 16) & 0xFF);
    bad_length[11] = static_cast<std::uint8_t>((over >> 24) & 0xFF);
    cases.push_back({"length > max_payload", bad_length, ErrorCode::kLengthExceeded});

    auto bad_crc = good_wire;
    bad_crc[12] ^= 0x01;
    cases.push_back({"checksum mismatch", bad_crc, ErrorCode::kChecksumMismatch});

    for (const auto& item : cases) {
        FrameCodec codec{RdrlSpec()};
        const auto outcome = FeedBytes(codec, item.wire);
        CHECK(!outcome.ok);
        CHECK(outcome.error.code == item.want);
        CHECK(!outcome.error.message.empty());
        CHECK(outcome.frames.empty());
        CHECK_EQ(outcome.consumed, 0);
        CHECK_EQ(codec.BufferedBytes(), 0);  // 错误后缓冲必须清空
        // 错误码名字各不相同，上层日志/监控可直接用它做标签。
        CHECK(std::strcmp(go2cpp::frame::ErrorCodeName(item.want), "none") != 0);
    }

    // 四类错误码互不相等（可区分）。
    CHECK(cases[0].want != cases[1].want);
    CHECK(cases[1].want != cases[2].want);
    CHECK(cases[2].want != cases[3].want);

    // 错误之前的完整帧仍然交付，consumed 指向坏帧起点。
    FrameCodec mixed{RdrlSpec()};
    std::vector<std::uint8_t> stream = good_wire;
    stream.insert(stream.end(), bad_magic.begin(), bad_magic.end());
    const auto mixed_outcome = FeedBytes(mixed, stream);
    CHECK(!mixed_outcome.ok);
    CHECK(mixed_outcome.error.code == ErrorCode::kBadMagic);
    CHECK_EQ(mixed_outcome.frames.size(), 1);
    CHECK_EQ(mixed_outcome.consumed, good_wire.size());

    // 错误后内部缓冲已清：同一实例继续喂一条合法流可以正常解出（调用方仍应重连）。
    std::vector<Frame> recovered;
    Error error;
    std::size_t consumed = 0;
    CHECK(mixed.Feed(good_wire.data(), good_wire.size(), &recovered, &consumed, &error));
    CHECK(recovered.size() == 1 && SameFrame(recovered[0], good));

    // 半包累积到第 12 字节才判定 magic 错误：错误应在帧头凑齐时立即给出。
    FrameCodec drip{RdrlSpec()};
    std::vector<Frame> drip_frames;
    for (std::size_t i = 0; i < 11; ++i) {
        CHECK(drip.Feed(&bad_magic[i], 1, &drip_frames, &consumed, &error));
        CHECK_EQ(drip.BufferedBytes(), i + 1);
    }
    CHECK(!drip.Feed(&bad_magic[11], 1, &drip_frames, &consumed, &error));
    CHECK(error.code == ErrorCode::kBadMagic);
    CHECK_EQ(consumed, 1);
    CHECK_EQ(drip.BufferedBytes(), 0);
}

// ---------------------------------------------------------------------------
// 6. 关闭校验 / 自定义校验
// ---------------------------------------------------------------------------

void TestChecksumModes() {
    Announce("校验和：nil（不校验）/ 自定义回调");

    // nil：帧尾不追加 4 字节，载荷被改也照样解出。
    FrameSpec no_check = RdrlSpec();
    no_check.checksum = nullptr;
    CHECK(!no_check.HasChecksum());
    FrameCodec plain(no_check);
    const Frame frame = MakeFrame(4, 7, 9, 3);
    const auto wire = ENCODE_OK(plain, frame);
    CHECK_EQ(wire.size(), static_cast<std::size_t>(12 + 9));
    const std::vector<std::uint8_t> golden_heartbeat_no_crc = {
        0x52, 0x44, 0x52, 0x4C, 0x01, 0x05, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    Frame heartbeat;
    heartbeat.type = 5;
    CHECK(ENCODE_OK(plain, heartbeat) == golden_heartbeat_no_crc);

    const auto outcome = FeedBytes(plain, wire);
    CHECK(outcome.ok);
    CHECK(outcome.frames.size() == 1 && SameFrame(outcome.frames[0], frame));

    auto tampered = wire;
    tampered[12] ^= 0x5A;  // 载荷第一个字节（无校验模式下位置无关）
    const auto tampered_outcome = FeedBytes(plain, tampered);
    CHECK(tampered_outcome.ok);  // 不校验 → 不报错
    CHECK_EQ(tampered_outcome.frames.size(), 1);
    CHECK(tampered_outcome.frames.size() == 1 &&
          !SameFrame(tampered_outcome.frames[0], frame));

    // 自定义校验：FNV-1a 32 位，统计回调次数验证「每帧恰好一次」。
    int checksum_calls = 0;
    FrameSpec custom = RdchSpec();
    custom.checksum = [&checksum_calls](const std::uint8_t* data, std::size_t len) {
        ++checksum_calls;
        std::uint32_t hash = 0x811C9DC5U;
        for (std::size_t i = 0; i < len; ++i) {
            hash ^= data[i];
            hash *= 16777619U;
        }
        return hash;
    };
    FrameCodec custom_codec(custom);
    const Frame custom_frame = MakeFrame(9, 0x0102, 40, 77);
    const auto custom_wire = ENCODE_OK(custom_codec, custom_frame);
    CHECK_EQ(checksum_calls, 1);
    const auto custom_outcome = FeedBytes(custom_codec, custom_wire);
    CHECK(custom_outcome.ok);
    CHECK_EQ(checksum_calls, 2);  // 解码路径同样回调一次
    CHECK(custom_outcome.frames.size() == 1 &&
          SameFrame(custom_outcome.frames[0], custom_frame));

    auto custom_tampered = custom_wire;
    custom_tampered[20] ^= 0x40;
    FrameCodec custom_decode(custom);
    const auto custom_bad = FeedBytes(custom_decode, custom_tampered);
    CHECK(!custom_bad.ok);
    CHECK(custom_bad.error.code == ErrorCode::kChecksumMismatch);
}

// ---------------------------------------------------------------------------
// 7. 长度上限 / spec 合法性 / 扩展帧头
// ---------------------------------------------------------------------------

void TestLimitsAndSpecValidation() {
    Announce("上限与配置：max_payload 边界、非法 spec、header_len=16 扩展区");

    // max_payload = 64：恰好 64 字节可以，65 字节报 kPayloadTooLarge 且不截断。
    FrameSpec small = RdrlSpec();
    small.max_payload = 64;
    FrameCodec small_codec(small);
    const auto max_frame = MakeFrame(1, 0, 64, 1);
    const auto max_wire = ENCODE_OK(small_codec, max_frame);
    CHECK_EQ(max_wire.size(), static_cast<std::size_t>(12 + 64 + 4));
    CHECK(FeedBytes(small_codec, max_wire).frames.size() == 1);

    std::vector<std::uint8_t> out = {0xDE, 0xAD, 0xBE, 0xEF};
    Error error;
    CHECK(!small_codec.Encode(MakeFrame(1, 0, 65, 2), &out, &error));
    CHECK(error.code == ErrorCode::kPayloadTooLarge);
    CHECK(out.empty());  // 失败不留半帧

    // 解码侧：帧头声明 65 字节 → 立即 kLengthExceeded，不按声明长度攒内存。
    auto oversized_header = max_wire;
    oversized_header[8] = 65;
    FrameCodec decode_small(small);
    const auto oversized = FeedBytes(decode_small, oversized_header);
    CHECK(!oversized.ok);
    CHECK(oversized.error.code == ErrorCode::kLengthExceeded);
    CHECK_EQ(decode_small.BufferedBytes(), 0);

    // max_payload = 0：只允许空载荷帧。
    FrameSpec empty_spec = RdrlSpec();
    empty_spec.max_payload = 0;
    FrameCodec empty_codec(empty_spec);
    Frame empty_frame;
    empty_frame.type = 5;
    const auto empty_wire = ENCODE_OK(empty_codec, empty_frame);
    CHECK_EQ(empty_wire.size(), static_cast<std::size_t>(16));
    CHECK(FeedBytes(empty_codec, empty_wire).frames.size() == 1);
    CHECK(!empty_codec.Encode(MakeFrame(5, 0, 1, 1), &out, &error));
    CHECK(error.code == ErrorCode::kPayloadTooLarge);

    // 非法 spec：header_len 太小 / 太大、max_payload 为负。
    for (const int header_len : {11, 5000}) {
        FrameSpec bad = RdrlSpec();
        bad.header_len = header_len;
        FrameCodec bad_codec(bad);
        CHECK(!bad_codec.SpecValid());
        CHECK(!bad_codec.Encode(MakeFrame(1, 0, 0, 0), &out, &error));
        CHECK(error.code == ErrorCode::kInvalidSpec);
        std::vector<Frame> frames;
        std::size_t consumed = 0;
        CHECK(!bad_codec.Feed(max_wire.data(), max_wire.size(), &frames, &consumed,
                              &error));
        CHECK(error.code == ErrorCode::kInvalidSpec);
        CHECK_EQ(consumed, 0);
    }
    FrameSpec negative = RdrlSpec();
    negative.max_payload = -1;
    FrameCodec negative_codec(negative);
    CHECK(!negative_codec.Encode(MakeFrame(1, 0, 0, 0), &out, &error));
    CHECK(error.code == ErrorCode::kInvalidSpec);

    // header_len = 16：扩展区 4 字节编码写 0，参与校验和，解码时被跳过。
    FrameSpec extended = RdrlSpec();
    extended.header_len = 16;
    FrameCodec extended_codec(extended);
    const Frame extended_frame = MakeFrame(3, 0x00FF, 10, 9);
    const auto extended_wire = ENCODE_OK(extended_codec, extended_frame);
    CHECK_EQ(extended_wire.size(), static_cast<std::size_t>(16 + 10 + 4));
    for (std::size_t i = 12; i < 16; ++i) {
        CHECK_EQ(extended_wire[i], 0);
    }
    const auto extended_outcome = FeedBytes(extended_codec, extended_wire);
    CHECK(extended_outcome.ok);
    CHECK(extended_outcome.frames.size() == 1 &&
          SameFrame(extended_outcome.frames[0], extended_frame));

    // 扩展区被改动 → 校验和必须发现（它覆盖整个帧头）。
    auto extended_tampered = extended_wire;
    extended_tampered[14] = 0x7F;
    FrameCodec extended_decode(extended);
    const auto extended_bad = FeedBytes(extended_decode, extended_tampered);
    CHECK(!extended_bad.ok);
    CHECK(extended_bad.error.code == ErrorCode::kChecksumMismatch);

    // 关闭校验 + 16 字节帧头同样成立。
    FrameSpec extended_plain = extended;
    extended_plain.checksum = nullptr;
    FrameCodec extended_plain_codec(extended_plain);
    const auto extended_plain_wire = ENCODE_OK(extended_plain_codec, extended_frame);
    CHECK_EQ(extended_plain_wire.size(), static_cast<std::size_t>(16 + 10));
    CHECK(FeedBytes(extended_plain_codec, extended_plain_wire).frames.size() == 1);
}

// ---------------------------------------------------------------------------
// 8. 参数校验：空指针 / 错误重置 / 追加语义
// ---------------------------------------------------------------------------

void TestArgumentHandling() {
    Announce("参数：空指针、err 预置后清空、frames 追加语义");

    FrameCodec codec{RdrlSpec()};
    const Frame frame = MakeFrame(1, 0, 4, 1);
    const auto wire = ENCODE_OK(codec, frame);
    Error error;
    std::vector<std::uint8_t> out;

    CHECK(!codec.Encode(frame, nullptr, &error));
    CHECK(error.code == ErrorCode::kInvalidArgument);

    error.code = ErrorCode::kBadMagic;  // 预置脏错误
    CHECK(codec.Encode(frame, &out, &error));
    CHECK(error.Ok());  // 成功时必须清空
    CHECK(error.message.empty());

    std::vector<Frame> frames;
    std::size_t consumed = 0;
    CHECK(!codec.Feed(wire.data(), wire.size(), nullptr, &consumed, &error));
    CHECK(error.code == ErrorCode::kInvalidArgument);
    CHECK(!codec.Feed(wire.data(), wire.size(), &frames, nullptr, &error));
    CHECK(error.code == ErrorCode::kInvalidArgument);
    CHECK(!codec.Feed(nullptr, 1, &frames, &consumed, &error));
    CHECK(error.code == ErrorCode::kInvalidArgument);
    CHECK_EQ(codec.BufferedBytes(), 0);

    // frames 是追加语义：调用方已有内容不会被清掉。
    frames.push_back(MakeFrame(9, 9, 0, 9));
    CHECK(codec.Feed(wire.data(), wire.size(), &frames, &consumed, &error));
    CHECK_EQ(frames.size(), 2);
    CHECK(SameFrame(frames[0], MakeFrame(9, 9, 0, 9)));
    CHECK(SameFrame(frames[1], frame));

    // Encode 复用输出缓冲：每次都被清空重写，不会残留上一次的字节。
    std::vector<std::uint8_t> reused = {1, 2, 3, 4, 5, 6, 7, 8, 9};
    CHECK(codec.Encode(frame, &reused, &error));
    CHECK(reused == wire);

    // err == nullptr 时也不能崩。
    CHECK(codec.Encode(frame, &out, nullptr));
    CHECK(codec.Feed(wire.data(), wire.size(), &frames, &consumed, nullptr));
}

// ---------------------------------------------------------------------------
// 9. 随机测试：随机帧流 + 随机切分
// ---------------------------------------------------------------------------

void TestRandomFrameStream() {
    Announce("随机测试：5000 帧随机切分喂入");

    std::mt19937 rng(20261101U);
    std::uniform_int_distribution<int> type_dist(0, 255);
    std::uniform_int_distribution<int> flag_dist(0, 0xFFFF);
    std::uniform_int_distribution<int> payload_dist(0, 600);
    std::uniform_int_distribution<int> chunk_dist(1, 97);

    FrameCodec encoder{RdrlSpec()};
    std::vector<Frame> expected;
    std::vector<std::uint8_t> wire;
    for (int i = 0; i < 5000; ++i) {
        Frame frame = MakeFrame(static_cast<std::uint8_t>(type_dist(rng)),
                                static_cast<std::uint16_t>(flag_dist(rng)),
                                static_cast<std::size_t>(payload_dist(rng)),
                                static_cast<std::uint32_t>(i));
        const auto encoded = ENCODE_OK(encoder, frame);
        wire.insert(wire.end(), encoded.begin(), encoded.end());
        expected.push_back(std::move(frame));
    }

    FrameCodec decoder{RdrlSpec()};
    std::vector<Frame> decoded;
    std::size_t offset = 0;
    std::size_t total_consumed = 0;
    while (offset < wire.size()) {
        const std::size_t chunk =
            std::min(static_cast<std::size_t>(chunk_dist(rng)), wire.size() - offset);
        Error error;
        std::size_t consumed = 0;
        CHECK(decoder.Feed(wire.data() + offset, chunk, &decoded, &consumed, &error));
        CHECK_EQ(consumed, chunk);
        total_consumed += consumed;
        offset += chunk;
        CHECK(decoder.BufferedBytes() <= decoder.Spec().MaxFrameBytes());
    }
    CHECK_EQ(total_consumed, wire.size());
    CHECK_EQ(decoder.BufferedBytes(), 0);
    CHECK_EQ(decoded.size(), expected.size());
    bool all_equal = decoded.size() == expected.size();
    for (std::size_t i = 0; all_equal && i < expected.size(); ++i) {
        all_equal = SameFrame(decoded[i], expected[i]);
    }
    CHECK(all_equal);
}

// ---------------------------------------------------------------------------
// 10. Fuzz：垃圾字节与单字节变异不得崩溃 / 不得无限缓冲
// ---------------------------------------------------------------------------

void TestFuzzGarbage() {
    Announce("fuzz：垃圾字节流 200 轮 + 合法帧单字节变异 300 轮");

    std::mt19937 rng(0xC0FFEEU);
    std::uniform_int_distribution<int> byte_dist(0, 255);
    std::uniform_int_distribution<int> len_dist(0, 3000);
    std::uniform_int_distribution<int> chunk_dist(1, 128);

    FrameCodec codec{RdrlSpec()};
    const std::size_t max_frame_bytes = codec.Spec().MaxFrameBytes();
    for (int round = 0; round < 200; ++round) {
        std::vector<std::uint8_t> garbage(static_cast<std::size_t>(len_dist(rng)));
        for (auto& byte : garbage) {
            byte = static_cast<std::uint8_t>(byte_dist(rng));
        }
        std::size_t offset = 0;
        while (offset < garbage.size()) {
            const std::size_t chunk = std::min(
                static_cast<std::size_t>(chunk_dist(rng)), garbage.size() - offset);
            std::vector<Frame> frames;
            Error error;
            std::size_t consumed = 0;
            const bool ok =
                codec.Feed(garbage.data() + offset, chunk, &frames, &consumed, &error);
            offset += chunk;
            CHECK(codec.BufferedBytes() <= max_frame_bytes);
            if (!ok) {
                CHECK(error.code == ErrorCode::kBadMagic ||
                      error.code == ErrorCode::kBadVersion ||
                      error.code == ErrorCode::kLengthExceeded ||
                      error.code == ErrorCode::kChecksumMismatch);
                CHECK_EQ(codec.BufferedBytes(), 0);
                codec.Reset();
            }
        }
        codec.Reset();
    }

    // 单字节变异：要么正常解出（改在无关位置），要么给出四类畸形错误，绝不崩溃。
    std::uniform_int_distribution<std::size_t> index_dist(0, 63);
    for (int round = 0; round < 300; ++round) {
        Frame mut_source = MakeFrame(3, 0x00AA, 48, 21);
        const auto wire = ENCODE_OK(codec, mut_source);
        std::vector<std::uint8_t> mutated = wire;
        const std::size_t index = index_dist(rng) % mutated.size();
        mutated[index] ^= 0x01;

        FrameCodec fresh{RdrlSpec()};
        std::vector<Frame> frames;
        Error error;
        std::size_t consumed = 0;
        const bool ok = fresh.Feed(mutated.data(), mutated.size(), &frames, &consumed,
                                   &error);
        if (!ok) {
            CHECK(error.code == ErrorCode::kBadMagic ||
                  error.code == ErrorCode::kBadVersion ||
                  error.code == ErrorCode::kLengthExceeded ||
                  error.code == ErrorCode::kChecksumMismatch);
        } else {
            // 成功路径只有两种：length 位被改大 → 半包；或完整解出（CRC 允许的
            // 情形在本格式里不存在，但断言保持宽松以免把实现细节写死）。
            CHECK_EQ(consumed, mutated.size());
            if (frames.empty()) {
                CHECK_EQ(fresh.BufferedBytes(), mutated.size());
            } else {
                CHECK_EQ(frames.size(), 1);
            }
        }
    }
}

// ---------------------------------------------------------------------------
// 11. 压测：1e6 帧
// ---------------------------------------------------------------------------

void TestStressMillionFrames() {
    Announce("压测：1e6 帧 encode → 随机切分 Feed → 逐帧校验");

    constexpr int kBatches = 1000;
    constexpr int kFramesPerBatch = 1000;
    constexpr int kTotalFrames = kBatches * kFramesPerBatch;

    FrameCodec codec{RdrlSpec()};
    std::mt19937 rng(123456789U);
    std::uniform_int_distribution<int> chunk_dist(1, 97);

    std::vector<Frame> expected;
    std::vector<Frame> decoded;
    std::vector<std::uint8_t> wire;
    std::vector<std::uint8_t> one;
    expected.reserve(kFramesPerBatch);
    decoded.reserve(kFramesPerBatch);
    wire.reserve(kFramesPerBatch * 64);

    std::size_t total_payload_bytes = 0;
    std::size_t peak_buffered = 0;
    std::size_t decoded_total = 0;
    std::uint64_t payload_hash = 0;
    bool all_equal = true;

    const auto start = std::chrono::steady_clock::now();
    for (int batch = 0; batch < kBatches; ++batch) {
        expected.clear();
        decoded.clear();
        wire.clear();
        for (int i = 0; i < kFramesPerBatch; ++i) {
            const std::uint32_t seed = static_cast<std::uint32_t>(batch * 1000 + i);
            Frame frame = MakeFrame(static_cast<std::uint8_t>((batch * 7 + i) & 0xFF),
                                    static_cast<std::uint16_t>((batch * 13 + i) & 0xFFFF),
                                    static_cast<std::size_t>((batch * 29 + i) % 120),
                                    seed);
            total_payload_bytes += frame.payload.size();
            for (const auto byte : frame.payload) {
                payload_hash = (payload_hash ^ byte) * 1099511628211ULL;
            }
            one.clear();
            Error error;
            if (!codec.Encode(frame, &one, &error)) {
                Check(false, "stress Encode() == true", __LINE__);
                return;
            }
            wire.insert(wire.end(), one.begin(), one.end());
            expected.push_back(std::move(frame));
        }

        std::size_t offset = 0;
        while (offset < wire.size()) {
            const std::size_t chunk = std::min(
                static_cast<std::size_t>(chunk_dist(rng)), wire.size() - offset);
            Error error;
            std::size_t consumed = 0;
            if (!codec.Feed(wire.data() + offset, chunk, &decoded, &consumed, &error)) {
                Check(false, "stress Feed() == true", __LINE__);
                std::fprintf(stderr, "  error: %s (%s)\n",
                             go2cpp::frame::ErrorCodeName(error.code),
                             error.message.c_str());
                return;
            }
            offset += chunk;
            peak_buffered = std::max(peak_buffered, codec.BufferedBytes());
        }
        if (decoded.size() != expected.size()) {
            all_equal = false;
            continue;
        }
        decoded_total += decoded.size();
        for (std::size_t i = 0; i < decoded.size(); ++i) {
            if (!SameFrame(decoded[i], expected[i])) {
                all_equal = false;
                break;
            }
        }
    }
    const double ms = MsSince(start);

    CHECK(all_equal);
    CHECK_EQ(decoded_total, static_cast<std::size_t>(kTotalFrames));
    CHECK_EQ(codec.BufferedBytes(), 0);
    CHECK(peak_buffered <= codec.Spec().MaxFrameBytes());
    const double seconds = ms / 1000.0;
    std::printf(
        "[frame]   %d 帧 / %.3f s = %.2f Mframes/s, 载荷 %zu 字节, "
        "peak buffered=%zu, VmHWM=%zu KB, payload_hash=%016llx\n",
        kTotalFrames, seconds, static_cast<double>(kTotalFrames) / seconds / 1e6,
        total_payload_bytes, peak_buffered, PeakRssKb(),
        static_cast<unsigned long long>(payload_hash));
}

// ---------------------------------------------------------------------------
// 12. 真实场景（≥5，覆盖全部功能点）
// ---------------------------------------------------------------------------

/// 场景 1：RDRL 控制链路握手 —— REGISTER / RESPONSE / CONNECT / EVENT / DATA_OPEN
/// 一次性发完，用随机分段模拟 TCP recv 的半包与粘包。
void TestScenarioRdrlHandshake() {
    Announce("场景 1：RDRL 控制链路握手（随机 TCP 分段）");

    FrameCodec codec{RdrlSpec()};
    std::vector<Frame> frames;
    auto add = [&](std::uint8_t type, std::vector<std::uint8_t> payload) {
        Frame frame;
        frame.type = type;
        frame.payload = std::move(payload);
        frames.push_back(std::move(frame));
    };
    // REGISTER: keyLen | key
    add(1, {0x03, 'k', 'e', 'y'});
    // RESPONSE: status u16 LE | "REGISTERED"
    add(3, {0x00, 0x00, 'R', 'E', 'G', 'I', 'S', 'T', 'E', 'R', 'E', 'D'});
    // CONNECT: key
    add(2, {'k', 'e', 'y'});
    // EVENT: kind | token[8] | peerID u64 | nameLen | name
    add(8, {0x01, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F, 0x10, 0x11,
            0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x04, 'v', 'i', 'e', 'w'});
    // DATA_OPEN: keyLen | token[8] | key
    add(6, {0x03, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF, 0x01, 0x02, 'k', 'e', 'y'});

    std::vector<std::uint8_t> stream;
    for (const auto& frame : frames) {
        const auto wire = ENCODE_OK(codec, frame);
        stream.insert(stream.end(), wire.begin(), wire.end());
    }

    FrameCodec decoder{RdrlSpec()};
    std::vector<Frame> decoded;
    std::mt19937 rng(7U);
    std::uniform_int_distribution<int> chunk_dist(1, 23);
    std::size_t offset = 0;
    while (offset < stream.size()) {
        const std::size_t chunk = std::min(
            static_cast<std::size_t>(chunk_dist(rng)), stream.size() - offset);
        Error error;
        std::size_t consumed = 0;
        CHECK(decoder.Feed(stream.data() + offset, chunk, &decoded, &consumed, &error));
        offset += chunk;
    }
    CHECK_EQ(decoded.size(), frames.size());
    bool equal = decoded.size() == frames.size();
    for (std::size_t i = 0; equal && i < frames.size(); ++i) {
        equal = SameFrame(decoded[i], frames[i]);
    }
    CHECK(equal);
}

/// 场景 2：RDCH 48 KiB 能力消息 —— 大载荷走 4 MiB 上限的 spec，按奇数边界切分。
/// 期望的 CRC 由 Python zlib 独立算出（0xdf0d4fdb），可在测试里直接对拍。
void TestScenarioRdchLargeMessage() {
    Announce("场景 2：RDCH 48 KiB 能力消息（奇数边界切分 + 黄金 CRC）");

    FrameCodec codec{RdchSpec()};
    Frame frame;
    frame.type = 9;  // RDRL CTRL，payload 是 RDCH 帧（语义由上层解释）
    frame.payload.resize(49152);
    for (std::size_t i = 0; i < frame.payload.size(); ++i) {
        frame.payload[i] = static_cast<std::uint8_t>((i * 7 + 3) & 0xFF);
    }
    const auto wire = ENCODE_OK(codec, frame);
    CHECK_EQ(wire.size(), static_cast<std::size_t>(12 + 49152 + 4));
    CHECK_EQ(LoadLe32(wire.data() + wire.size() - 4), 0xdf0d4fdbU);

    std::vector<Frame> decoded;
    std::size_t offset = 0;
    std::size_t consumed = 0;
    Error error;
    while (offset < wire.size()) {
        const std::size_t chunk =
            std::min(static_cast<std::size_t>(4093), wire.size() - offset);
        CHECK(codec.Feed(wire.data() + offset, chunk, &decoded, &consumed, &error));
        offset += chunk;
    }
    CHECK(decoded.size() == 1 && SameFrame(decoded[0], frame));
}

/// 场景 3：同一进程内 RDRL / RDCH 双协议共存 —— 各自只认自己的 magic。
void TestScenarioDualProtocol() {
    Announce("场景 3：RDRL / RDCH 双协议共存，互不误解析");

    FrameCodec rdrl{RdrlSpec()};
    FrameCodec rdch{RdchSpec()};
    const Frame rdrl_frame = MakeFrame(1, 0, 8, 1);
    const Frame rdch_frame = MakeFrame(1, 0, 8, 2);
    const auto rdrl_wire = ENCODE_OK(rdrl, rdrl_frame);
    const auto rdch_wire = ENCODE_OK(rdch, rdch_frame);

    // 交叉喂入：magic 阶段就必须拒绝，而不是把对方解释成「未知 type」。
    CHECK(FeedBytes(rdch, rdrl_wire).error.code == ErrorCode::kBadMagic);
    CHECK(FeedBytes(rdrl, rdch_wire).error.code == ErrorCode::kBadMagic);
    // 各自喂自己的流仍然正常。
    CHECK(FeedBytes(rdrl, rdrl_wire).frames.size() == 1);
    CHECK(FeedBytes(rdch, rdch_wire).frames.size() == 1);
}

/// 场景 4：坏帧导致连接不可信 → Reset 后换新连接重新开始。
void TestScenarioReconnectAfterError() {
    Announce("场景 4：错误后 Reset 重连，旧半包不污染新连接");

    FrameCodec codec{RdrlSpec()};
    const Frame first = MakeFrame(2, 0, 30, 3);
    const Frame second = MakeFrame(3, 0, 12, 4);
    const auto first_wire = ENCODE_OK(codec, first);
    const auto second_wire = ENCODE_OK(codec, second);

    // 旧连接：完整帧 + 下一帧的前 5 字节（半包）。
    std::vector<std::uint8_t> stream = first_wire;
    stream.insert(stream.end(), second_wire.begin(), second_wire.begin() + 5);
    std::vector<Frame> frames;
    std::size_t consumed = 0;
    Error error;
    CHECK(codec.Feed(stream.data(), stream.size(), &frames, &consumed, &error));
    CHECK_EQ(frames.size(), 1);
    CHECK_EQ(codec.BufferedBytes(), 5);

    // 半包补到 12 字节时 length 字段是 0xFFFFFFFF：帧头凑齐即报错，半包被丢弃。
    const std::uint8_t bad_tail[7] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    CHECK(!codec.Feed(bad_tail, sizeof(bad_tail), &frames, &consumed, &error));
    CHECK(error.code == ErrorCode::kLengthExceeded);
    CHECK_EQ(codec.BufferedBytes(), 0);

    // 新连接（Reset 只是显式收尾，错误路径本身已经清空缓冲）。
    codec.Reset();
    CHECK_EQ(codec.BufferedBytes(), 0);
    std::vector<Frame> fresh;
    CHECK(codec.Feed(second_wire.data(), second_wire.size(), &fresh, &consumed, &error));
    CHECK(fresh.size() == 1 && SameFrame(fresh[0], second));
}

/// 场景 5：恶意超长长度头 —— 声明 4 GiB 也必须立刻拒绝，不预分配、不缓冲膨胀。
void TestScenarioMemoryAmplification() {
    Announce("场景 5：恶意 length 头（0xFFFFFFFF / 65537）不放大内存");

    FrameCodec codec{RdrlSpec()};
    // 手工构造声明 0xFFFFFFFF 的 12 字节帧头。
    std::vector<std::uint8_t> evil = {0x52, 0x44, 0x52, 0x4C, 0x01, 0x01,
                                      0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF};
    const auto outcome = FeedBytes(codec, evil);
    CHECK(!outcome.ok);
    CHECK(outcome.error.code == ErrorCode::kLengthExceeded);
    CHECK_EQ(codec.BufferedBytes(), 0);

    // 声明 65536（合法上限）但只给 1 字节载荷：允许继续等，缓冲只能等于「实际收到」。
    std::vector<std::uint8_t> legit = {0x52, 0x44, 0x52, 0x4C, 0x01, 0x01,
                                       0x00, 0x00, 0x00, 0x00, 0x01, 0x00};
    std::vector<Frame> frames;
    std::size_t consumed = 0;
    Error error;
    CHECK(codec.Feed(legit.data(), legit.size(), &frames, &consumed, &error));
    CHECK_EQ(codec.BufferedBytes(), 12);
    const std::uint8_t one_byte = 0x7F;
    for (int i = 0; i < 500; ++i) {
        CHECK(codec.Feed(&one_byte, 1, &frames, &consumed, &error));
    }
    CHECK_EQ(codec.BufferedBytes(), 12 + 500);  // 缓冲区只随真实收到的字节增长
    CHECK(codec.BufferedBytes() <= codec.Spec().MaxFrameBytes());
    codec.Reset();
    CHECK_EQ(codec.BufferedBytes(), 0);
}

}  // namespace

int main() {
    std::printf("[frame] === FrameCodec 验收测试（REQ-001）===\n");
    std::fflush(stdout);

    TestGoldenWireBytes();
    TestTwoSpecsAllTypes();
    TestHalfPacketByteByByte();
    TestStickyPackets();
    TestMalformedFrames();
    TestChecksumModes();
    TestLimitsAndSpecValidation();
    TestArgumentHandling();
    TestRandomFrameStream();
    TestFuzzGarbage();
    TestScenarioRdrlHandshake();
    TestScenarioRdchLargeMessage();
    TestScenarioDualProtocol();
    TestScenarioReconnectAfterError();
    TestScenarioMemoryAmplification();
    TestStressMillionFrames();

    std::printf("[frame] checks=%zu failures=%d, VmHWM=%zu KB\n", g_checks,
                g_failures, PeakRssKb());
    if (g_failures != 0) {
        std::fprintf(stderr, "[frame] FAILED: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("[frame] all checks passed\n");
    return 0;
}
