/**
 * @file test_frame.cpp
 * @brief MOD-10 通用帧编解码：正常/粘包/半包/非法/超长/校验失败/空帧 + 随机 fuzz。
 * @author go2cpp contributors
 * @date 2026-10-28
 * @copyright MIT License
 */

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <thread>
#include <utility>
#include <vector>

#include "go2cpp/bento/net/frame.hpp"
#include "test_util.hpp"

namespace {

using namespace go2cpp::bento::net;

/// 确定性 xorshift：随机测试不依赖系统熵，失败可复现。
std::uint64_t g_rng = 0x123456789abcdefull;

std::uint32_t RandomNext() {
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 7;
    g_rng ^= g_rng << 17;
    return static_cast<std::uint32_t>(g_rng >> 32);
}

Frame MakeFrame(std::uint16_t type, std::uint16_t flags, std::size_t payload_bytes,
                std::uint8_t salt) {
    Frame frame;
    frame.type = type;
    frame.flags = flags;
    frame.payload.resize(payload_bytes);
    for (std::size_t i = 0; i < payload_bytes; ++i) {
        frame.payload[i] = static_cast<std::uint8_t>(salt + i * 31u);
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

/// 手工构造 20B 帧头（校验和填 0），用于制造异常输入。
std::vector<std::uint8_t> MakeRawHeader(std::uint32_t magic, std::uint16_t version,
                                        std::uint16_t reserved, std::uint32_t length) {
    std::vector<std::uint8_t> header(kGenericFrameHeaderBytes, 0);
    WriteU32(header.data(), magic);
    WriteU16(header.data() + 4, version);
    WriteU16(header.data() + 6, 0x0001);
    WriteU16(header.data() + 8, 0);
    WriteU16(header.data() + 10, reserved);
    WriteU32(header.data() + 12, length);
    return header;
}

void TestRoundtrip() {
    const std::size_t sizes[] = {0, 1, 17, 1300, 65535, 1024 * 1024};
    for (const std::size_t size : sizes) {
        const Frame in = MakeFrame(0x1234, 0x00f0, size, 7);
        const std::vector<std::uint8_t> bytes = Encode(in);
        BENTO_CHECK_EQ(bytes.size(), kGenericFrameHeaderBytes + size);
        if (bytes.empty()) {
            continue;
        }
        Frame out;
        std::size_t consumed = 0;
        BENTO_CHECK(Decode(bytes.data(), bytes.size(), &out, &consumed) == FrameStatus::kOk);
        BENTO_CHECK_EQ(consumed, kGenericFrameHeaderBytes + size);
        BENTO_CHECK_EQ(out.magic, kFrameMagic);
        BENTO_CHECK_EQ(out.version, kFrameVersion);
        BENTO_CHECK_EQ(out.type, in.type);
        BENTO_CHECK_EQ(out.flags, in.flags);
        BENTO_CHECK_EQ(out.reserved, 0);
        BENTO_CHECK_EQ(out.Length(), static_cast<std::uint32_t>(size));
        BENTO_CHECK(out.payload == in.payload);
        // 线上 checksum 必须与独立重算一致（覆盖头前 16B + 载荷）。
        BENTO_CHECK_EQ(out.checksum, ComputeFrameChecksum(bytes.data(),
                                                          bytes.data() + kGenericFrameHeaderBytes,
                                                          size));
    }
}

void TestStickyPackets() {
    std::vector<std::uint8_t> stream;
    constexpr int kCount = 6;
    for (int i = 0; i < kCount; ++i) {
        const std::vector<std::uint8_t> bytes =
            Encode(MakeFrame(static_cast<std::uint16_t>(100 + i), static_cast<std::uint16_t>(i),
                             10 + static_cast<std::size_t>(i) * 7, static_cast<std::uint8_t>(i)));
        stream.insert(stream.end(), bytes.begin(), bytes.end());
    }
    FrameDecoder decoder;
    decoder.Append(stream.data(), stream.size());
    for (int i = 0; i < kCount; ++i) {
        Frame frame;
        BENTO_CHECK(decoder.Next(&frame) == FrameStatus::kOk);
        BENTO_CHECK_EQ(frame.type, static_cast<std::uint16_t>(100 + i));
        BENTO_CHECK_EQ(frame.flags, static_cast<std::uint16_t>(i));
        BENTO_CHECK_EQ(frame.payload.size(), 10U + static_cast<std::size_t>(i) * 7);
    }
    Frame tail;
    BENTO_CHECK(decoder.Next(&tail) == FrameStatus::kNeedMoreData);
    BENTO_CHECK_EQ(decoder.PendingBytes(), 0U);
}

void TestPartialPackets() {
    const std::vector<std::uint8_t> bytes = Encode(MakeFrame(7, 0, 200, 3));
    FrameDecoder decoder;
    for (std::size_t i = 0; i + 1 < bytes.size(); ++i) {
        decoder.Append(&bytes[i], 1);
        Frame frame;
        BENTO_CHECK(decoder.Next(&frame) == FrameStatus::kNeedMoreData);
        BENTO_CHECK_EQ(decoder.PendingBytes(), i + 1);
    }
    decoder.Append(&bytes.back(), 1);
    Frame frame;
    BENTO_CHECK(decoder.Next(&frame) == FrameStatus::kOk);
    BENTO_CHECK_EQ(frame.payload.size(), 200U);
    BENTO_CHECK_EQ(decoder.PendingBytes(), 0U);
}

void TestBadMagic() {
    std::vector<std::uint8_t> bytes = Encode(MakeFrame(1, 0, 32, 1));
    bytes[0] = 0x00;
    Frame frame;
    BENTO_CHECK(Decode(bytes.data(), bytes.size(), &frame) == FrameStatus::kBadMagic);

    FrameDecoder decoder;
    decoder.Append(bytes.data(), bytes.size());
    BENTO_CHECK(decoder.Next(&frame) == FrameStatus::kBadMagic);
    // 错误后帧边界不可信：缓冲必须被清空，不能把残留字节当成新帧开头。
    BENTO_CHECK_EQ(decoder.PendingBytes(), 0U);
}

void TestBadVersionAndReserved() {
    std::vector<std::uint8_t> header = MakeRawHeader(kFrameMagic, 2, 0, 0);
    Frame frame;
    BENTO_CHECK(Decode(header.data(), header.size(), &frame) == FrameStatus::kBadVersion);

    header = MakeRawHeader(kFrameMagic, kFrameVersion, 1, 0);
    BENTO_CHECK(Decode(header.data(), header.size(), &frame) == FrameStatus::kBadReserved);

    Frame bad;
    bad.reserved = 1;
    BENTO_CHECK(Encode(bad).empty());
}

void TestLengthExceeded() {
    // 只给 20B 头、声明 1MiB+1：必须在分配载荷之前拒绝。
    const std::vector<std::uint8_t> header =
        MakeRawHeader(kFrameMagic, kFrameVersion, 0, kFrameMaxPayload + 1);
    Frame frame;
    BENTO_CHECK(Decode(header.data(), header.size(), &frame) == FrameStatus::kLengthExceeded);

    FrameDecoder decoder;
    decoder.Append(header.data(), header.size());
    BENTO_CHECK(decoder.Next(&frame) == FrameStatus::kLengthExceeded);
    BENTO_CHECK_EQ(decoder.PendingBytes(), 0U);

    // 发送端同样拒绝超过 1MiB 的载荷。
    Frame oversize;
    oversize.payload.resize(kFrameMaxPayload + 1);
    BENTO_CHECK(Encode(oversize).empty());

    // 恰好 1MiB 是合法的边界值。
    const std::vector<std::uint8_t> max_bytes =
        Encode(MakeFrame(9, 0, kFrameMaxPayload, 5));
    BENTO_CHECK_EQ(max_bytes.size(), kGenericFrameHeaderBytes + kFrameMaxPayload);
}

void TestChecksumMismatch() {
    std::vector<std::uint8_t> bytes = Encode(MakeFrame(2, 0, 64, 4));
    bytes.back() ^= 0x01u;
    Frame frame;
    BENTO_CHECK(Decode(bytes.data(), bytes.size(), &frame) == FrameStatus::kChecksumMismatch);

    FrameDecoder decoder;
    decoder.Append(bytes.data(), bytes.size());
    BENTO_CHECK(decoder.Next(&frame) == FrameStatus::kChecksumMismatch);
    BENTO_CHECK_EQ(decoder.PendingBytes(), 0U);
}

void TestEmptyPayloadFrame() {
    const Frame in = MakeFrame(0xabcd, 0x0001, 0, 0);
    const std::vector<std::uint8_t> bytes = Encode(in);
    BENTO_CHECK_EQ(bytes.size(), kGenericFrameHeaderBytes);
    Frame out;
    std::size_t consumed = 0;
    BENTO_CHECK(Decode(bytes.data(), bytes.size(), &out, &consumed) == FrameStatus::kOk);
    BENTO_CHECK_EQ(consumed, kGenericFrameHeaderBytes);
    BENTO_CHECK(out.payload.empty());
    BENTO_CHECK_EQ(out.type, static_cast<std::uint16_t>(0xabcd));

    FrameDecoder decoder;
    decoder.Append(bytes.data(), bytes.size());
    Frame streamed;
    BENTO_CHECK(decoder.Next(&streamed) == FrameStatus::kOk);
    BENTO_CHECK_EQ(decoder.PendingBytes(), 0U);
}

void TestTruncatedHeaderAndArguments() {
    const std::vector<std::uint8_t> bytes = Encode(MakeFrame(3, 0, 4, 2));
    Frame frame;
    BENTO_CHECK(Decode(bytes.data(), kGenericFrameHeaderBytes - 1, &frame) ==
                FrameStatus::kNeedMoreData);
    BENTO_CHECK(Decode(nullptr, 0, &frame) == FrameStatus::kNeedMoreData);
    BENTO_CHECK(Decode(bytes.data(), bytes.size(), nullptr) == FrameStatus::kInvalidArgument);

    FrameDecoder decoder;
    decoder.Append(bytes.data(), kGenericFrameHeaderBytes - 1);
    BENTO_CHECK(decoder.Next(&frame) == FrameStatus::kNeedMoreData);
    decoder.Append(nullptr, 0);
    BENTO_CHECK_EQ(decoder.PendingBytes(), kGenericFrameHeaderBytes - 1);
    decoder.Clear();
    BENTO_CHECK_EQ(decoder.PendingBytes(), 0U);

    // 自定义上限：payload(20) > 16 的合法帧在更小上限的解码器里被判超限。
    const std::vector<std::uint8_t> oversized = Encode(MakeFrame(3, 0, 20, 2));
    FrameDecoder strict(16);
    strict.Append(oversized.data(), oversized.size());
    BENTO_CHECK(strict.Next(&frame) == FrameStatus::kLengthExceeded);
}

/// 随机测试：随机帧数/载荷/内容，按随机块切分喂入，要求逐帧完全还原。
void TestRandomStreamFuzz() {
    for (int round = 0; round < 200; ++round) {
        const int frame_count = 1 + static_cast<int>(RandomNext() % 8);
        std::vector<Frame> expected;
        std::vector<std::uint8_t> stream;
        for (int i = 0; i < frame_count; ++i) {
            Frame frame;
            frame.type = static_cast<std::uint16_t>(RandomNext());
            frame.flags = static_cast<std::uint16_t>(RandomNext());
            frame.payload.resize(RandomNext() % 3000);
            for (auto& byte : frame.payload) {
                byte = static_cast<std::uint8_t>(RandomNext());
            }
            const std::vector<std::uint8_t> bytes = Encode(frame);
            BENTO_CHECK(!bytes.empty());
            stream.insert(stream.end(), bytes.begin(), bytes.end());
            expected.push_back(std::move(frame));
        }

        FrameDecoder decoder;
        std::size_t position = 0;
        std::size_t received = 0;
        while (position < stream.size()) {
            const std::size_t chunk =
                std::min<std::size_t>(1 + RandomNext() % 97, stream.size() - position);
            decoder.Append(stream.data() + position, chunk);
            position += chunk;
            Frame frame;
            while (decoder.Next(&frame) == FrameStatus::kOk) {
                BENTO_CHECK(received < expected.size());
                if (received < expected.size()) {
                    BENTO_CHECK_EQ(frame.type, expected[received].type);
                    BENTO_CHECK_EQ(frame.flags, expected[received].flags);
                    BENTO_CHECK(frame.payload == expected[received].payload);
                }
                ++received;
            }
        }
        BENTO_CHECK_EQ(received, expected.size());
        BENTO_CHECK_EQ(decoder.PendingBytes(), 0U);
    }
}


// ---------------------------------------------------------------------------
// 审查补齐的覆盖：协议格式、头字段篡改、错误恢复、零上限、1 MiB 边界、
// 参数矩阵、并发、随机对抗（与独立参考模型对拍）。
// ---------------------------------------------------------------------------

/// 参考模型：逐位 CRC（与实现的 slicing-by-8 表法互为独立路径）。
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

/// 参考模型的标准 CRC：物理拼接「头前 16B + 载荷」后一次算完。
std::uint32_t RefFrameCrc(const std::uint8_t* frame_start, std::size_t payload_bytes) {
    std::vector<std::uint8_t> material;
    material.reserve(16 + payload_bytes);
    material.insert(material.end(), frame_start, frame_start + 16);
    material.insert(material.end(), frame_start + kGenericFrameHeaderBytes,
                    frame_start + kGenericFrameHeaderBytes + payload_bytes);
    return RefCrc32(material.data(), material.size());
}

std::uint16_t RefReadU16(const std::uint8_t* in) {
    return static_cast<std::uint16_t>((static_cast<std::uint16_t>(in[0]) << 8) | in[1]);
}

std::uint32_t RefReadU32(const std::uint8_t* in) {
    return (static_cast<std::uint32_t>(in[0]) << 24) |
           (static_cast<std::uint32_t>(in[1]) << 16) |
           (static_cast<std::uint32_t>(in[2]) << 8) | static_cast<std::uint32_t>(in[3]);
}

/// 参考模型的状态机：只依据协议文档，不复用实现的任何解析函数。
FrameStatus RefNext(const std::uint8_t* data, std::size_t size, std::size_t* consumed) {
    if (size < kGenericFrameHeaderBytes) {
        return FrameStatus::kNeedMoreData;
    }
    if (RefReadU32(data) != kFrameMagic) {
        return FrameStatus::kBadMagic;
    }
    if (RefReadU16(data + 4) != kFrameVersion) {
        return FrameStatus::kBadVersion;
    }
    if (RefReadU16(data + 10) != 0) {
        return FrameStatus::kBadReserved;
    }
    const std::uint32_t length = RefReadU32(data + 12);
    if (length > kFrameMaxPayload) {
        return FrameStatus::kLengthExceeded;
    }
    const std::size_t total = kGenericFrameHeaderBytes + static_cast<std::size_t>(length);
    if (size < total) {
        return FrameStatus::kNeedMoreData;
    }
    if (RefReadU32(data + 16) != RefFrameCrc(data, length)) {
        return FrameStatus::kChecksumMismatch;
    }
    *consumed = total;
    return FrameStatus::kOk;
}

/// 自定义 protocol magic/version：两种协议共用帧布局与 CRC，但互不识别。
void TestCustomFrameFormat() {
    constexpr std::uint32_t kSessionMagic = 0x53455353u;  // "SESS"
    constexpr std::uint16_t kSessionVersion = 7;
    const FrameFormat format{kSessionMagic, kSessionVersion};
    const Frame in = MakeFrame(0x1111, 0x2222, 100, 9);
    const std::vector<std::uint8_t> bytes = Encode(in, format);
    BENTO_CHECK_EQ(bytes.size(), kGenericFrameHeaderBytes + 100U);
    BENTO_CHECK_EQ(RefReadU32(bytes.data()), kSessionMagic);
    BENTO_CHECK_EQ(RefReadU16(bytes.data() + 4), kSessionVersion);

    Frame out;
    // Link 默认解码器不认自定义协议。
    BENTO_CHECK(Decode(bytes.data(), bytes.size(), &out) == FrameStatus::kBadMagic);
    // 自定义协议解出完整帧。
    BENTO_CHECK(Decode(format, bytes.data(), bytes.size(), &out) == FrameStatus::kOk);
    BENTO_CHECK_EQ(out.type, in.type);
    BENTO_CHECK(out.payload == in.payload);
    // 反向：默认帧被自定义解码器拒绝。
    const std::vector<std::uint8_t> default_bytes = Encode(in);
    BENTO_CHECK(Decode(format, default_bytes.data(), default_bytes.size(), &out) ==
                FrameStatus::kBadMagic);
    // 同 magic、不同 version → kBadVersion（不是 kBadMagic）。
    const FrameFormat wrong_version{kSessionMagic,
                                    static_cast<std::uint16_t>(kSessionVersion + 1)};
    BENTO_CHECK(Decode(wrong_version, bytes.data(), bytes.size(), &out) ==
                FrameStatus::kBadVersion);
    // magic == 0 在编码与解码两端都拒绝。
    const FrameFormat zero_magic{0, kSessionVersion};
    BENTO_CHECK(Encode(in, zero_magic).empty());
    BENTO_CHECK(Decode(zero_magic, default_bytes.data(), default_bytes.size(), &out) ==
                FrameStatus::kInvalidArgument);
    // FrameDecoder 携带自定义格式。
    FrameDecoder decoder(kFrameMaxPayload, format);
    BENTO_CHECK_EQ(decoder.format().magic, kSessionMagic);
    BENTO_CHECK_EQ(decoder.format().version, kSessionVersion);
    decoder.Append(bytes.data(), bytes.size());
    BENTO_CHECK(decoder.Next(&out) == FrameStatus::kOk);
    BENTO_CHECK(out.payload == in.payload);
}

/// 头字段篡改：CRC 覆盖头前 16B，因此 type/flags/length 被改都必须报校验失败；
/// 落在 CRC 覆盖范围之前的字段（magic/version/reserved）按各自的错误码拒绝。
void TestHeaderFieldTamper() {
    const std::vector<std::uint8_t> good = Encode(MakeFrame(0x1234, 0x0001, 64, 4));
    auto expect = [&](const std::vector<std::uint8_t>& bytes, FrameStatus want) {
        Frame out;
        BENTO_CHECK(Decode(bytes.data(), bytes.size(), &out) == want);
    };
    {
        auto bytes = good;
        WriteU16(bytes.data() + 6, 0x9999);
        expect(bytes, FrameStatus::kChecksumMismatch);
    }
    {
        auto bytes = good;
        WriteU16(bytes.data() + 8, 0x9999);
        expect(bytes, FrameStatus::kChecksumMismatch);
    }
    {
        // length 改小：帧头声明的边界被篡改，必须被 CRC 发现，不能悄悄缩短帧。
        auto bytes = good;
        WriteU32(bytes.data() + 12, 32);
        expect(bytes, FrameStatus::kChecksumMismatch);
    }
    {
        // length 改大但未超限：数据不足 → 半包，而不是越界读或错误码。
        auto bytes = good;
        WriteU32(bytes.data() + 12, 65);
        Frame out;
        BENTO_CHECK(Decode(bytes.data(), bytes.size(), &out) == FrameStatus::kNeedMoreData);
    }
    {
        // checksum 字段自身被篡改。
        auto bytes = good;
        bytes[16] ^= 0x80u;
        expect(bytes, FrameStatus::kChecksumMismatch);
    }
    {
        auto bytes = good;
        WriteU16(bytes.data() + 10, 1);
        expect(bytes, FrameStatus::kBadReserved);
    }
    {
        auto bytes = good;
        bytes[3] ^= 0x01u;
        expect(bytes, FrameStatus::kBadMagic);
    }
}

/// 错误恢复：错误后缓冲已清空，同一解码器可直接继续解合法帧。
void TestErrorRecovery() {
    const std::vector<std::uint8_t> good = Encode(MakeFrame(7, 0, 40, 1));
    std::vector<std::uint8_t> bad = good;
    bad[0] ^= 0xffu;
    FrameDecoder decoder;
    Frame out;
    decoder.Append(bad.data(), bad.size());
    BENTO_CHECK(decoder.Next(&out) == FrameStatus::kBadMagic);
    BENTO_CHECK_EQ(decoder.PendingBytes(), 0U);
    decoder.Append(good.data(), good.size());
    BENTO_CHECK(decoder.Next(&out) == FrameStatus::kOk);
    BENTO_CHECK_EQ(out.payload.size(), 40U);
    // 连续两次错误后仍能恢复。
    decoder.Append(bad.data(), bad.size());
    BENTO_CHECK(decoder.Next(&out) == FrameStatus::kBadMagic);
    decoder.Append(bad.data(), bad.size());
    BENTO_CHECK(decoder.Next(&out) == FrameStatus::kBadMagic);
    decoder.Append(good.data(), good.size());
    BENTO_CHECK(decoder.Next(&out) == FrameStatus::kOk);
    // 状态名覆盖全部枚举（含审查新增的 out_of_memory）。
    BENTO_CHECK_EQ(std::strcmp(FrameStatusName(FrameStatus::kOutOfMemory), "out_of_memory"), 0);
    BENTO_CHECK_EQ(std::strcmp(FrameStatusName(FrameStatus::kOk), "ok"), 0);
    BENTO_CHECK_EQ(std::strcmp(FrameStatusName(FrameStatus::kBadMagic), "bad_magic"), 0);
}

/// max_payload = 0：只接受空载荷帧，一次性 Decode 与流式解码器语义一致。
void TestZeroPayloadLimit() {
    FrameDecoder decoder(0);
    BENTO_CHECK_EQ(decoder.MaxPayload(), 0U);
    const std::vector<std::uint8_t> empty_frame = Encode(MakeFrame(1, 0, 0, 0));
    const std::vector<std::uint8_t> one_byte = Encode(MakeFrame(1, 0, 1, 0));
    Frame out;
    decoder.Append(empty_frame.data(), empty_frame.size());
    BENTO_CHECK(decoder.Next(&out) == FrameStatus::kOk);
    BENTO_CHECK(out.payload.empty());
    decoder.Append(one_byte.data(), one_byte.size());
    BENTO_CHECK(decoder.Next(&out) == FrameStatus::kLengthExceeded);
    BENTO_CHECK_EQ(decoder.PendingBytes(), 0U);
    BENTO_CHECK(Decode(kDefaultFrameFormat, 0, one_byte.data(), one_byte.size(), &out) ==
                FrameStatus::kLengthExceeded);
    BENTO_CHECK(Decode(kDefaultFrameFormat, 0, empty_frame.data(), empty_frame.size(), &out) ==
                FrameStatus::kOk);
}

/// 粘包 + 半包交错：已消费前缀与半包残留同时存在时的缓冲压缩路径。
void TestStickyPartialCompaction() {
    const Frame frame_a = MakeFrame(1, 0, 8, 1);
    const Frame frame_b = MakeFrame(2, 0, 4096, 2);
    const Frame frame_c = MakeFrame(3, 0, 3, 3);
    const std::vector<std::uint8_t> bytes_a = Encode(frame_a);
    const std::vector<std::uint8_t> bytes_b = Encode(frame_b);
    const std::vector<std::uint8_t> bytes_c = Encode(frame_c);
    FrameDecoder decoder;
    Frame out;
    decoder.Append(bytes_a.data(), bytes_a.size());
    decoder.Append(bytes_b.data(), 7);
    BENTO_CHECK(decoder.Next(&out) == FrameStatus::kOk);
    BENTO_CHECK_EQ(out.type, frame_a.type);
    BENTO_CHECK_EQ(decoder.PendingBytes(), 7U);
    BENTO_CHECK(decoder.Next(&out) == FrameStatus::kNeedMoreData);
    BENTO_CHECK_EQ(decoder.PendingBytes(), 7U);
    // 补上 B 的其余部分，再一次性喂 C：压缩 + 粘包连续发生。
    decoder.Append(bytes_b.data() + 7, bytes_b.size() - 7);
    decoder.Append(bytes_c.data(), bytes_c.size());
    BENTO_CHECK(decoder.Next(&out) == FrameStatus::kOk);
    BENTO_CHECK_EQ(out.type, frame_b.type);
    BENTO_CHECK(out.payload == frame_b.payload);
    BENTO_CHECK(decoder.Next(&out) == FrameStatus::kOk);
    BENTO_CHECK_EQ(out.type, frame_c.type);
    BENTO_CHECK(decoder.Next(&out) == FrameStatus::kNeedMoreData);
    BENTO_CHECK_EQ(decoder.PendingBytes(), 0U);
}

/// 1 MiB 边界帧：完整往返、三段半包到达、1 MiB+1 的发送/接收双端拒绝。
void TestMaxPayloadFrame() {
    const Frame max_frame = MakeFrame(9, 0, kFrameMaxPayload, 5);
    const std::vector<std::uint8_t> bytes = Encode(max_frame);
    BENTO_CHECK_EQ(bytes.size(), kGenericFrameHeaderBytes + kFrameMaxPayload);
    Frame out;
    std::size_t consumed = 0;
    BENTO_CHECK(Decode(bytes.data(), bytes.size(), &out, &consumed) == FrameStatus::kOk);
    BENTO_CHECK_EQ(consumed, bytes.size());
    BENTO_CHECK(out.payload == max_frame.payload);

    FrameDecoder decoder;
    constexpr std::size_t kThird = kFrameMaxPayload / 3;
    decoder.Append(bytes.data(), kGenericFrameHeaderBytes + kThird);
    BENTO_CHECK(decoder.Next(&out) == FrameStatus::kNeedMoreData);
    decoder.Append(bytes.data() + kGenericFrameHeaderBytes + kThird, kThird);
    BENTO_CHECK(decoder.Next(&out) == FrameStatus::kNeedMoreData);
    decoder.Append(bytes.data() + kGenericFrameHeaderBytes + 2 * kThird,
                   kFrameMaxPayload - 2 * kThird);
    BENTO_CHECK(decoder.Next(&out) == FrameStatus::kOk);
    BENTO_CHECK(out.payload == max_frame.payload);
    BENTO_CHECK_EQ(decoder.PendingBytes(), 0U);

    Frame oversize = max_frame;
    oversize.payload.resize(kFrameMaxPayload + 1);
    BENTO_CHECK(Encode(oversize).empty());
    std::vector<std::uint8_t> bad = bytes;
    WriteU32(bad.data() + 12, kFrameMaxPayload + 1);
    BENTO_CHECK(Decode(bad.data(), bad.size(), &out) == FrameStatus::kLengthExceeded);
    // 声明 1MiB 但数据不足：必须按半包等待，不能因为「声明很大」就报超限。
    std::vector<std::uint8_t> truncated = bytes;
    truncated.resize(kGenericFrameHeaderBytes + 10);
    BENTO_CHECK(Decode(truncated.data(), truncated.size(), &out) == FrameStatus::kNeedMoreData);
}

/// 参数矩阵：nullptr、consumed 写入门槛、max_payload 重载、Next(nullptr)。
void TestDecodeArgumentMatrix() {
    const std::vector<std::uint8_t> bytes = Encode(MakeFrame(3, 0, 16, 1));
    Frame out;
    std::size_t consumed = 12345;
    BENTO_CHECK(Decode(bytes.data(), 10, &out, &consumed) == FrameStatus::kNeedMoreData);
    BENTO_CHECK_EQ(consumed, 12345U);
    std::vector<std::uint8_t> bad = bytes;
    bad[0] ^= 0xffu;
    BENTO_CHECK(Decode(bad.data(), bad.size(), &out, &consumed) == FrameStatus::kBadMagic);
    BENTO_CHECK_EQ(consumed, 12345U);
    BENTO_CHECK(Decode(nullptr, 0, &out) == FrameStatus::kNeedMoreData);
    BENTO_CHECK(Decode(nullptr, 1, &out) == FrameStatus::kInvalidArgument);
    BENTO_CHECK(Decode(bytes.data(), bytes.size(), nullptr) == FrameStatus::kInvalidArgument);
    BENTO_CHECK(Decode(kDefaultFrameFormat, nullptr, 0, &out) == FrameStatus::kNeedMoreData);
    const FrameFormat zero_magic{0, kFrameVersion};
    BENTO_CHECK(Decode(zero_magic, kFrameMaxPayload, bytes.data(), bytes.size(), &out) ==
                FrameStatus::kInvalidArgument);
    // 自定义上限：16B 载荷的合法帧在 15 上限下拒绝、在 16 上限下通过。
    BENTO_CHECK(Decode(kDefaultFrameFormat, 15, bytes.data(), bytes.size(), &out) ==
                FrameStatus::kLengthExceeded);
    BENTO_CHECK(Decode(kDefaultFrameFormat, 16, bytes.data(), bytes.size(), &out) ==
                FrameStatus::kOk);
    FrameDecoder decoder;
    BENTO_CHECK(decoder.Next(nullptr) == FrameStatus::kInvalidArgument);
    decoder.Append(nullptr, 0);
    BENTO_CHECK_EQ(decoder.PendingBytes(), 0U);
}

/// 并发：每线程独立解码器，用不同切分喂同一条流，结果必须逐帧一致。
void TestConcurrentDecoders() {
    constexpr int kThreads = 4;
    constexpr int kFrameCount = 300;
    std::vector<Frame> frames;
    frames.reserve(kFrameCount);
    std::vector<std::uint8_t> stream;
    for (int i = 0; i < kFrameCount; ++i) {
        Frame frame = MakeFrame(static_cast<std::uint16_t>(100 + i),
                                static_cast<std::uint16_t>(i),
                                17 + static_cast<std::size_t>(i * 37) % 400,
                                static_cast<std::uint8_t>(i));
        const std::vector<std::uint8_t> bytes = Encode(frame);
        stream.insert(stream.end(), bytes.begin(), bytes.end());
        frames.push_back(std::move(frame));
    }
    std::atomic<int> failures{0};
    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&, t]() {
            FrameDecoder decoder;
            Frame out;
            std::size_t position = 0;
            int received = 0;
            while (position < stream.size()) {
                const std::size_t chunk = std::min<std::size_t>(
                    1 + static_cast<std::size_t>(t * 17 + position) % 61,
                    stream.size() - position);
                decoder.Append(stream.data() + position, chunk);
                position += chunk;
                FrameStatus status = FrameStatus::kNeedMoreData;
                while ((status = decoder.Next(&out)) == FrameStatus::kOk) {
                    if (received >= kFrameCount ||
                        out.type != frames[static_cast<std::size_t>(received)].type ||
                        out.payload != frames[static_cast<std::size_t>(received)].payload) {
                        failures.fetch_add(1);
                    }
                    ++received;
                }
                if (status != FrameStatus::kNeedMoreData) {
                    failures.fetch_add(1);
                }
            }
            if (received != kFrameCount) {
                failures.fetch_add(1);
            }
        });
    }
    for (std::thread& thread : threads) {
        thread.join();
    }
    BENTO_CHECK_EQ(failures.load(), 0);
}

/// 随机对抗：合法帧 + 随机坏块 + 随机截断，随机块切分喂入，与参考模型对拍。
void TestRandomizedAdversarial() {
    for (int round = 0; round < 400; ++round) {
        std::vector<Frame> frames;
        std::vector<std::uint8_t> stream;
        const int frame_count = 1 + static_cast<int>(RandomNext() % 10);
        for (int i = 0; i < frame_count; ++i) {
            Frame frame;
            frame.type = static_cast<std::uint16_t>(RandomNext());
            frame.flags = static_cast<std::uint16_t>(RandomNext());
            frame.payload.resize(RandomNext() % 1024);
            for (auto& byte : frame.payload) {
                byte = static_cast<std::uint8_t>(RandomNext());
            }
            const std::vector<std::uint8_t> bytes = Encode(frame);
            stream.insert(stream.end(), bytes.begin(), bytes.end());
            frames.push_back(std::move(frame));
        }
        if ((RandomNext() % 100u) < 40u) {
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
        if ((RandomNext() % 100u) < 30u && !stream.empty()) {
            const std::size_t cut = 1 + static_cast<std::size_t>(RandomNext() % 30u);
            const std::size_t keep = stream.size() > cut ? stream.size() - cut : 1;
            stream.resize(keep);
        }

        std::size_t offset = 0;
        std::size_t expected_ok = 0;
        FrameStatus expected_status = FrameStatus::kNeedMoreData;
        for (;;) {
            std::size_t consumed = 0;
            const FrameStatus status =
                RefNext(stream.data() + offset, stream.size() - offset, &consumed);
            if (status == FrameStatus::kOk) {
                ++expected_ok;
                offset += consumed;
                continue;
            }
            expected_status = status;
            break;
        }

        FrameDecoder decoder;
        Frame out;
        std::size_t position = 0;
        std::size_t received = 0;
        FrameStatus final_status = FrameStatus::kNeedMoreData;
        bool stopped = false;
        while (position < stream.size() && !stopped) {
            const std::size_t chunk =
                std::min<std::size_t>(1 + RandomNext() % 96u, stream.size() - position);
            decoder.Append(stream.data() + position, chunk);
            position += chunk;
            for (;;) {
                const FrameStatus status = decoder.Next(&out);
                if (status == FrameStatus::kOk) {
                    BENTO_CHECK(received < frames.size());
                    if (received < frames.size()) {
                        BENTO_CHECK_EQ(out.type, frames[received].type);
                        BENTO_CHECK(out.payload == frames[received].payload);
                    }
                    ++received;
                    continue;
                }
                final_status = status;
                if (status != FrameStatus::kNeedMoreData) {
                    stopped = true;
                }
                break;
            }
        }
        BENTO_CHECK_EQ(received, expected_ok);
        BENTO_CHECK_EQ(final_status, expected_status);
        if (stopped) {
            BENTO_CHECK_EQ(decoder.PendingBytes(), 0U);
        }
    }
}

}  // namespace

int main() {
    TestRoundtrip();
    TestStickyPackets();
    TestPartialPackets();
    TestBadMagic();
    TestBadVersionAndReserved();
    TestLengthExceeded();
    TestChecksumMismatch();
    TestEmptyPayloadFrame();
    TestTruncatedHeaderAndArguments();
    TestRandomStreamFuzz();
    // 审查补齐的覆盖（见各函数注释）。
    TestCustomFrameFormat();
    TestHeaderFieldTamper();
    TestErrorRecovery();
    TestZeroPayloadLimit();
    TestStickyPartialCompaction();
    TestMaxPayloadFrame();
    TestDecodeArgumentMatrix();
    TestConcurrentDecoders();
    TestRandomizedAdversarial();
    return bento_test::Finish("test_frame");
}
