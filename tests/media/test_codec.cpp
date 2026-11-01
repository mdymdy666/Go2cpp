/**
 * @file test_codec.cpp
 * @brief 编解码：MediaHeader 往返/拒绝、raw-bgra/raw-i420 逐字节往返、
 *        delta-i420 逐帧无损校验、静止帧收敛、单块变化、噪声、奇数尺寸、
 *        关键帧强制、缺参考帧/截断/非法 mode 的拒绝路径、压缩率统计。
 * @author go2cpp contributors
 * @date 2026-11-01
 * @copyright MIT License
 */

#include <cstdint>
#include <cstring>
#include <memory>
#include <vector>

#include "go2cpp/media/codec.hpp"
#include "go2cpp/media/pixel.hpp"
#include "media_test_util.hpp"

namespace {

using namespace go2cpp::media;

/// @brief 生成确定性图案（可按帧号轻微变化，模拟真实画面）。
VideoFrame MakeFrame(int width, int height, std::uint32_t frame_id, bool animate) {
    VideoFrame frame;
    frame.width = width;
    frame.height = height;
    frame.format = PixelFormat::kBGRA;
    frame.stride = width * 4;
    frame.frame_id = frame_id;
    frame.data.resize(BgraFrameBytes(width, height));
    for (int y = 0; y < height; ++y) {
        std::uint8_t* row = frame.data.data() + static_cast<std::size_t>(y) * frame.stride;
        for (int x = 0; x < width; ++x) {
            std::uint8_t b = static_cast<std::uint8_t>((x * 3 + y * 5) & 0xFF);
            std::uint8_t g = static_cast<std::uint8_t>((x * 7 + y * 11) & 0xFF);
            std::uint8_t r = static_cast<std::uint8_t>((x * 13 + y * 17) & 0xFF);
            if (animate) {
                // 一个 24x24 的方块随帧号移动：每帧只脏少数宏块。
                const int px = static_cast<int>((frame_id * 7U) % static_cast<std::uint32_t>(width));
                const int py = static_cast<int>((frame_id * 5U) % static_cast<std::uint32_t>(height));
                if (x >= px && x < px + 24 && y >= py && y < py + 24) {
                    b = 0xFF;
                    g = 0x00;
                    r = 0xFF;
                }
            }
            row[x * 4 + 0] = b;
            row[x * 4 + 1] = g;
            row[x * 4 + 2] = r;
            row[x * 4 + 3] = 255;
        }
    }
    return frame;
}

/// @brief 把 BGRA 帧转成紧凑 I420（作为解码结果的期望值）。
VideoFrame ToI420(const VideoFrame& bgra) {
    VideoFrame out;
    out.width = bgra.width;
    out.height = bgra.height;
    out.format = PixelFormat::kI420;
    out.data.assign(I420FrameBytes(bgra.width, bgra.height), 0);
    BgraToI420(bgra.data.data(), bgra.width, bgra.height, bgra.stride, out.data.data());
    return out;
}

void TestMediaHeader() {
    MediaHeader header;
    header.codec = CodecKind::kDeltaI420;
    header.keyframe = true;
    header.width = 641;
    header.height = 481;
    header.frame_id = 0xDEADBEEFU;
    header.payload_bytes = 1234;
    header.capture_ns = 0x0102030405060708ULL;
    std::uint8_t buffer[MediaHeader::kBytes] = {0};
    EncodeMediaHeader(header, buffer);

    MEDIA_CHECK_EQ(buffer[0], 0x47);  // 'G'
    MEDIA_CHECK_EQ(buffer[1], 0x32);  // '2'
    MEDIA_CHECK_EQ(buffer[2], 0x4D);  // 'M'
    MEDIA_CHECK_EQ(buffer[3], 0x46);  // 'F'

    MediaHeader decoded;
    MEDIA_CHECK(DecodeMediaHeader(buffer, sizeof(buffer) + 1234, &decoded));
    MEDIA_CHECK_EQ(decoded.codec, CodecKind::kDeltaI420);
    MEDIA_CHECK(decoded.keyframe);
    MEDIA_CHECK_EQ(decoded.width, 641);
    MEDIA_CHECK_EQ(decoded.height, 481);
    MEDIA_CHECK_EQ(decoded.frame_id, 0xDEADBEEFU);
    MEDIA_CHECK_EQ(decoded.payload_bytes, 1234U);
    MEDIA_CHECK_EQ(decoded.capture_ns, 0x0102030405060708ULL);

    // 拒绝路径：magic 错 / 版本错 / 长度不足。
    std::uint8_t bad[MediaHeader::kBytes] = {0};
    EncodeMediaHeader(header, bad);
    bad[0] = 0;
    MEDIA_CHECK(!DecodeMediaHeader(bad, sizeof(bad) + 1234, &decoded));
    EncodeMediaHeader(header, bad);
    bad[4] = 2;
    MEDIA_CHECK(!DecodeMediaHeader(bad, sizeof(bad) + 1234, &decoded));
    EncodeMediaHeader(header, bad);
    MEDIA_CHECK(!DecodeMediaHeader(bad, MediaHeader::kBytes, &decoded));  // payload 不够
}

void TestRawRoundTrip() {
    const VideoFrame source = MakeFrame(64, 48, 1, false);

    // raw-bgra：逐字节往返。
    {
        auto encoder = CreateEncoder(CodecKind::kRawBgra, 64, 48);
        auto decoder = CreateDecoder(CodecKind::kRawBgra, 64, 48);
        MEDIA_CHECK(encoder != nullptr);
        MEDIA_CHECK(decoder != nullptr);
        std::vector<std::uint8_t> encoded;
        MEDIA_CHECK(encoder->Encode(source, &encoded));
        VideoFrame decoded;
        MEDIA_CHECK(decoder->Decode(encoded.data(), encoded.size(), &decoded));
        MEDIA_CHECK_EQ(decoded.format, PixelFormat::kBGRA);
        MEDIA_CHECK(FrameEqual(source, decoded));
    }
    // raw-i420：与转换后的期望逐字节相等。
    {
        auto encoder = CreateEncoder(CodecKind::kRawI420, 64, 48);
        auto decoder = CreateDecoder(CodecKind::kRawI420, 64, 48);
        std::vector<std::uint8_t> encoded;
        MEDIA_CHECK(encoder->Encode(source, &encoded));
        VideoFrame decoded;
        MEDIA_CHECK(decoder->Decode(encoded.data(), encoded.size(), &decoded));
        MEDIA_CHECK_EQ(decoded.format, PixelFormat::kI420);
        MEDIA_CHECK(FrameEqual(ToI420(source), decoded));
    }
}

/// @brief delta 编解码一轮：编码→解码→与期望 I420 比对。
bool DeltaRoundTrip(VideoEncoder& encoder, VideoDecoder& decoder, const VideoFrame& source,
                    std::size_t* encoded_bytes, VideoFrame* decoded_out) {
    std::vector<std::uint8_t> encoded;
    if (!encoder.Encode(source, &encoded)) {
        return false;
    }
    if (encoded_bytes != nullptr) {
        *encoded_bytes = encoded.size();
    }
    return decoder.Decode(encoded.data(), encoded.size(), decoded_out);
}

void TestDeltaRawLossless() {
    const int width = 96;
    const int height = 64;
    auto encoder = CreateEncoder(CodecKind::kDeltaI420, width, height);
    auto decoder = CreateDecoder(CodecKind::kDeltaI420, width, height);
    MEDIA_CHECK(encoder != nullptr);
    MEDIA_CHECK(decoder != nullptr);
    MEDIA_CHECK_EQ(encoder->kind(), CodecKind::kDeltaI420);

    std::uint64_t total_bytes = 0;
    std::uint64_t first_bytes = 0;
    std::uint64_t last_bytes = 0;
    const int frames = 100;
    for (int i = 0; i < frames; ++i) {
        const VideoFrame source = MakeFrame(width, height, static_cast<std::uint32_t>(i), true);
        std::size_t encoded_bytes = 0;
        VideoFrame decoded;
        MEDIA_CHECK(DeltaRoundTrip(*encoder, *decoder, source, &encoded_bytes, &decoded));
        MEDIA_CHECK(FrameEqual(ToI420(source), decoded));
        MEDIA_CHECK_EQ(decoded.frame_id, static_cast<std::uint32_t>(i));
        total_bytes += encoded_bytes;
        if (i == 0) {
            first_bytes = encoded_bytes;
        }
        last_bytes = encoded_bytes;
    }
    MEDIA_CHECK_EQ(decoder->decoded_frames(), static_cast<std::uint64_t>(frames));
    MEDIA_CHECK_EQ(decoder->dropped_frames(), 0U);
    MEDIA_CHECK_EQ(encoder->keyframes(), 1U);  // 只有首帧是关键帧
    std::printf("[INFO] delta 96x64: first=%lluB last=%lluB avg=%.1fB total=%lluB\n",
                static_cast<unsigned long long>(first_bytes),
                static_cast<unsigned long long>(last_bytes),
                static_cast<double>(total_bytes) / frames,
                static_cast<unsigned long long>(total_bytes));
    // 首帧必须是「全部宏块 raw/rle」，体积接近 I420；后续帧应远小于首帧。
    MEDIA_CHECK_GE(first_bytes, static_cast<std::size_t>(I420FrameBytes(width, height)) / 2);
    MEDIA_CHECK(last_bytes < first_bytes);
}

void TestDeltaStaticConverges() {
    // 完全静止：第 2 帧起负载 = 4 字节（bitmap_bytes = 0），整帧 36 字节。
    const int width = 128;
    const int height = 96;
    auto encoder = CreateEncoder(CodecKind::kDeltaI420, width, height);
    auto decoder = CreateDecoder(CodecKind::kDeltaI420, width, height);
    const VideoFrame source = MakeFrame(width, height, 0, false);
    std::size_t encoded_bytes = 0;
    VideoFrame decoded;
    MEDIA_CHECK(DeltaRoundTrip(*encoder, *decoder, source, &encoded_bytes, &decoded));
    MEDIA_CHECK(FrameEqual(ToI420(source), decoded));
    const std::size_t keyframe_bytes = encoded_bytes;
    for (int i = 1; i < 20; ++i) {
        VideoFrame repeat = source;
        repeat.frame_id = static_cast<std::uint32_t>(i);
        std::size_t bytes = 0;
        VideoFrame out;
        MEDIA_CHECK(DeltaRoundTrip(*encoder, *decoder, repeat, &bytes, &out));
        MEDIA_CHECK(FrameEqual(ToI420(repeat), out));
        MEDIA_CHECK_EQ(bytes, MediaHeader::kBytes + 4);  // 头 + u32(0)
    }
    std::printf("[INFO] delta static 128x96: keyframe=%lluB delta=%zuB (%.1fx)\n",
                static_cast<unsigned long long>(keyframe_bytes), MediaHeader::kBytes + 4,
                static_cast<double>(keyframe_bytes) / static_cast<double>(MediaHeader::kBytes + 4));
    MEDIA_CHECK(keyframe_bytes > MediaHeader::kBytes + 4);
}

void TestDeltaSingleBlockChange() {
    // 只改一个 16x16 区域：负载应远小于全帧，且解码正确。
    const int width = 160;
    const int height = 128;
    auto encoder = CreateEncoder(CodecKind::kDeltaI420, width, height);
    auto decoder = CreateDecoder(CodecKind::kDeltaI420, width, height);
    VideoFrame source = MakeFrame(width, height, 0, false);
    std::size_t bytes = 0;
    VideoFrame decoded;
    MEDIA_CHECK(DeltaRoundTrip(*encoder, *decoder, source, &bytes, &decoded));

    VideoFrame changed = source;
    changed.frame_id = 1;
    for (int y = 32; y < 48; ++y) {
        std::uint8_t* row = changed.data.data() + static_cast<std::size_t>(y) * changed.stride;
        for (int x = 64; x < 80; ++x) {
            row[x * 4 + 0] = 0x11;
            row[x * 4 + 1] = 0x22;
            row[x * 4 + 2] = 0x33;
        }
    }
    std::size_t changed_bytes = 0;
    MEDIA_CHECK(DeltaRoundTrip(*encoder, *decoder, changed, &changed_bytes, &decoded));
    MEDIA_CHECK(FrameEqual(ToI420(changed), decoded));
    std::printf("[INFO] delta single block: keyframe=%lluB one-block=%lluB\n",
                static_cast<unsigned long long>(bytes),
                static_cast<unsigned long long>(changed_bytes));
    MEDIA_CHECK(changed_bytes < bytes);
    // 一个宏块（Y 384 + U/V 128 字节原始）+ bitmap + 头，远小于整帧。
    MEDIA_CHECK(changed_bytes < static_cast<std::size_t>(I420FrameBytes(width, height)) / 4);
}

void TestDeltaNoise() {
    // 不可压缩输入：每块都脏，负载接近 I420 + 每块 1B mode。
    const int width = 64;
    const int height = 48;
    auto encoder = CreateEncoder(CodecKind::kDeltaI420, width, height);
    auto decoder = CreateDecoder(CodecKind::kDeltaI420, width, height);
    std::uint32_t state = 12345;
    for (int i = 0; i < 8; ++i) {
        VideoFrame source;
        source.width = width;
        source.height = height;
        source.format = PixelFormat::kBGRA;
        source.stride = width * 4;
        source.frame_id = static_cast<std::uint32_t>(i);
        source.data.resize(BgraFrameBytes(width, height));
        for (std::size_t p = 0; p < source.data.size(); p += 4) {
            state = state * 1664525U + 1013904223U;
            source.data[p + 0] = static_cast<std::uint8_t>(state);
            source.data[p + 1] = static_cast<std::uint8_t>(state >> 8);
            source.data[p + 2] = static_cast<std::uint8_t>(state >> 16);
            source.data[p + 3] = 255;
        }
        std::size_t bytes = 0;
        VideoFrame decoded;
        MEDIA_CHECK(DeltaRoundTrip(*encoder, *decoder, source, &bytes, &decoded));
        MEDIA_CHECK(FrameEqual(ToI420(source), decoded));
        // 噪声帧：不超过 I420 + 每宏块 1B mode + bitmap + 头。
        const std::size_t mb = static_cast<std::size_t>((width + 15) / 16) *
                               static_cast<std::size_t>((height + 15) / 16);
        MEDIA_CHECK_GE(static_cast<std::size_t>(MediaHeader::kBytes + I420FrameBytes(width, height) +
                                                mb + (mb + 7) / 8 + 4),
                       bytes);
    }
}

void TestDeltaOddSizes() {
    const int sizes[][2] = {{1, 1}, {3, 3}, {17, 33}, {33, 17}, {31, 31}, {213, 157}};
    for (const auto& size : sizes) {
        const int width = size[0];
        const int height = size[1];
        auto encoder = CreateEncoder(CodecKind::kDeltaI420, width, height);
        auto decoder = CreateDecoder(CodecKind::kDeltaI420, width, height);
        for (int i = 0; i < 4; ++i) {
            const VideoFrame source = MakeFrame(width, height, static_cast<std::uint32_t>(i), true);
            std::size_t bytes = 0;
            VideoFrame decoded;
            MEDIA_CHECK(DeltaRoundTrip(*encoder, *decoder, source, &bytes, &decoded));
            MEDIA_CHECK(FrameEqual(ToI420(source), decoded));
        }
    }
}

void TestDeltaKeyframeRecovery() {
    // 解码器丢失后重新创建一个：只有关键帧能立即恢复。
    const int width = 64;
    const int height = 48;
    auto encoder = CreateEncoder(CodecKind::kDeltaI420, width, height);
    auto decoder = CreateDecoder(CodecKind::kDeltaI420, width, height);
    VideoFrame source = MakeFrame(width, height, 0, true);
    std::vector<std::uint8_t> encoded;
    MEDIA_CHECK(encoder->Encode(source, &encoded));
    VideoFrame decoded;
    MEDIA_CHECK(decoder->Decode(encoded.data(), encoded.size(), &decoded));

    // 新解码器收到差分帧：缺参考帧 → 拒绝。
    auto fresh = CreateDecoder(CodecKind::kDeltaI420, width, height);
    source = MakeFrame(width, height, 1, true);
    MEDIA_CHECK(encoder->Encode(source, &encoded));
    MEDIA_CHECK(!fresh->Decode(encoded.data(), encoded.size(), &decoded));
    MEDIA_CHECK_EQ(fresh->dropped_frames(), 1U);

    // 强制关键帧后新解码器可以恢复。
    encoder->ForceKeyframe();
    source.frame_id = 2;
    MEDIA_CHECK(encoder->Encode(source, &encoded));
    MEDIA_CHECK(fresh->Decode(encoded.data(), encoded.size(), &decoded));
    MEDIA_CHECK(FrameEqual(ToI420(source), decoded));
    MEDIA_CHECK_EQ(fresh->decoded_frames(), 1U);
}

void TestDeltaCorruptionRejected() {
    const int width = 64;
    const int height = 48;
    auto encoder = CreateEncoder(CodecKind::kDeltaI420, width, height);
    auto decoder = CreateDecoder(CodecKind::kDeltaI420, width, height);
    VideoFrame source = MakeFrame(width, height, 0, true);
    std::vector<std::uint8_t> encoded;
    MEDIA_CHECK(encoder->Encode(source, &encoded));
    VideoFrame decoded;
    MEDIA_CHECK(decoder->Decode(encoded.data(), encoded.size(), &decoded));

    // 再编一帧差分，然后逐类破坏。
    auto fresh = CreateDecoder(CodecKind::kDeltaI420, width, height);
    std::vector<std::uint8_t> key = encoded;
    MEDIA_CHECK(fresh->Decode(key.data(), key.size(), &decoded));

    source = MakeFrame(width, height, 1, true);
    MEDIA_CHECK(encoder->Encode(source, &encoded));
    MEDIA_CHECK(fresh->Decode(encoded.data(), encoded.size(), &decoded));
    const std::vector<std::uint8_t> good = encoded;

    // 1) 截断 8 字节：payload_len 大于实到字节数 → 头部解码失败。
    auto decoder_a = CreateDecoder(CodecKind::kDeltaI420, width, height);
    MEDIA_CHECK(decoder_a->Decode(good.data(), good.size() - 8, &decoded) == false);

    // 2) 破坏 bitmap：把 bitmap_bytes 改成一个超大值。
    std::vector<std::uint8_t> modified = good;
    modified[MediaHeader::kBytes + 0] = 0x7F;
    auto decoder_b = CreateDecoder(CodecKind::kDeltaI420, width, height);
    MEDIA_CHECK(decoder_b->Decode(key.data(), key.size(), &decoded));       // 建立参考帧
    MEDIA_CHECK(!decoder_b->Decode(modified.data(), modified.size(), &decoded));

    // 3) 非法 mode：定位到 bitmap 之后的第一个块 mode，改成 9。
    std::vector<std::uint8_t> bad_mode = good;
    const std::size_t bitmap_bytes =
        (static_cast<std::size_t>(good[MediaHeader::kBytes + 0]) << 24) |
        (static_cast<std::size_t>(good[MediaHeader::kBytes + 1]) << 16) |
        (static_cast<std::size_t>(good[MediaHeader::kBytes + 2]) << 8) |
        static_cast<std::size_t>(good[MediaHeader::kBytes + 3]);
    const std::size_t first_block = MediaHeader::kBytes + 4 + bitmap_bytes;
    MEDIA_CHECK(bitmap_bytes > 0);
    MEDIA_CHECK(first_block < bad_mode.size());
    bad_mode[first_block] = 9;
    auto decoder_c = CreateDecoder(CodecKind::kDeltaI420, width, height);
    MEDIA_CHECK(decoder_c->Decode(key.data(), key.size(), &decoded));
    MEDIA_CHECK(!decoder_c->Decode(bad_mode.data(), bad_mode.size(), &decoded));
}

}  // namespace

int main() {
    TestMediaHeader();
    TestRawRoundTrip();
    TestDeltaRawLossless();
    TestDeltaStaticConverges();
    TestDeltaSingleBlockChange();
    TestDeltaNoise();
    TestDeltaOddSizes();
    TestDeltaKeyframeRecovery();
    TestDeltaCorruptionRejected();
    return media_test::Finish("test_codec");
}
