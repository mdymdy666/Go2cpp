/**
 * @file test_frame.cpp
 * @brief MOD-10 通用帧编解码：正常/粘包/半包/非法/超长/校验失败/空帧 + 随机 fuzz。
 * @author go2cpp contributors
 * @date 2026-10-28
 * @copyright MIT License
 */

#include <algorithm>
#include <cstdint>
#include <cstdio>
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
    return bento_test::Finish("test_frame");
}
