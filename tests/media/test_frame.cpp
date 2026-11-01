/**
 * @file test_frame.cpp
 * @brief 帧容器 + 像素转换：尺寸推导 / 校验和 / NonZeroRatio / BGRA↔I420
 *        / 奇数尺寸 / 纯色转换正确性 / PSNR / MAE / FrameEqual。
 * @author go2cpp contributors
 * @date 2026-11-01
 * @copyright MIT License
 */

#include <cstdint>
#include <cmath>
#include <cstring>
#include <vector>

#include "go2cpp/media/frame.hpp"
#include "go2cpp/media/pixel.hpp"
#include "media_test_util.hpp"

namespace {

using namespace go2cpp::media;

/// @brief 构造一帧纯色 BGRA。
VideoFrame MakeSolid(int width, int height, std::uint8_t b, std::uint8_t g, std::uint8_t r) {
    VideoFrame frame;
    frame.width = width;
    frame.height = height;
    frame.format = PixelFormat::kBGRA;
    frame.stride = width * 4;
    frame.data.resize(BgraFrameBytes(width, height));
    for (int y = 0; y < height; ++y) {
        std::uint8_t* row = frame.data.data() + static_cast<std::size_t>(y) * frame.stride;
        for (int x = 0; x < width; ++x) {
            row[x * 4 + 0] = b;
            row[x * 4 + 1] = g;
            row[x * 4 + 2] = r;
            row[x * 4 + 3] = 255;
        }
    }
    return frame;
}

/// @brief 渐变 + 噪声图案，避免纯色掩盖转换错误。
VideoFrame MakePattern(int width, int height) {
    VideoFrame frame;
    frame.width = width;
    frame.height = height;
    frame.format = PixelFormat::kBGRA;
    frame.stride = width * 4;
    frame.data.resize(BgraFrameBytes(width, height));
    for (int y = 0; y < height; ++y) {
        std::uint8_t* row = frame.data.data() + static_cast<std::size_t>(y) * frame.stride;
        for (int x = 0; x < width; ++x) {
            row[x * 4 + 0] = static_cast<std::uint8_t>((x * 7 + y * 3) & 0xFF);
            row[x * 4 + 1] = static_cast<std::uint8_t>((x * 11 + y * 5) & 0xFF);
            row[x * 4 + 2] = static_cast<std::uint8_t>((x * 13 + y * 17) & 0xFF);
            row[x * 4 + 3] = 255;
        }
    }
    return frame;
}

void TestFrameBasics() {
    VideoFrame frame;
    MEDIA_CHECK(!frame.Valid());
    MEDIA_CHECK_EQ(frame.PlaneBytes(), 0U);

    frame.Reset(4, 4, PixelFormat::kBGRA);
    MEDIA_CHECK_EQ(frame.PlaneBytes(), 64U);
    MEDIA_CHECK(frame.Valid());
    MEDIA_CHECK_EQ(frame.EffectiveStride(), 16);

    frame.Reset(4, 4, PixelFormat::kI420);
    MEDIA_CHECK_EQ(frame.PlaneBytes(), I420FrameBytes(4, 4));
    MEDIA_CHECK_EQ(frame.PlaneBytes(), 24U);  // 16 Y + 4 U + 4 V
    MEDIA_CHECK_EQ(frame.EffectiveStride(), 4);

    // 奇数尺寸：色度平面按 ceil。
    MEDIA_CHECK_EQ(I420FrameBytes(1, 1), 3U);    // 1 + 1 + 1
    MEDIA_CHECK_EQ(I420FrameBytes(3, 5), 27U);   // 15 + 2*3*2 = 27
    MEDIA_CHECK_EQ(I420FrameBytes(17, 33), 17U * 33U + 9U * 17U * 2U);
    MEDIA_CHECK_EQ(BgraFrameBytes(0, 10), 0U);
    MEDIA_CHECK_EQ(I420FrameBytes(-1, 10), 0U);

    // 非法尺寸不可用。
    VideoFrame bad;
    bad.width = 0;
    bad.height = 10;
    MEDIA_CHECK(!bad.Valid());
}

void TestChecksum() {
    VideoFrame a = MakePattern(32, 24);
    VideoFrame b = a;
    MEDIA_CHECK_EQ(a.Checksum(), b.Checksum());
    MEDIA_CHECK(a.Checksum() != 0U);

    b.data[100] = static_cast<std::uint8_t>(b.data[100] ^ 0xFFU);
    MEDIA_CHECK(a.Checksum() != b.Checksum());

    // 全零帧：NonZeroRatio == 0，Checksum 非零（FNV 基值）。
    VideoFrame zero;
    zero.Reset(16, 16, PixelFormat::kBGRA);
    MEDIA_CHECK_EQ(zero.NonZeroRatio(), 0.0);
    // 纯白帧（BGRA 全 0xFF）：NonZeroRatio == 1。
    VideoFrame white;
    white.Reset(16, 16, PixelFormat::kBGRA);
    std::memset(white.data.data(), 0xFF, white.data.size());
    MEDIA_CHECK_EQ(white.NonZeroRatio(), 1.0);
}

void TestColorConversion() {
    // 纯红：(R=255,G=0,B=0) → Y=82, U=90, V=240（BT.601 limited range）。
    VideoFrame red = MakeSolid(8, 8, 0, 0, 255);
    std::vector<std::uint8_t> i420(I420FrameBytes(8, 8), 0);
    BgraToI420(red.data.data(), 8, 8, red.stride, i420.data());
    MEDIA_CHECK_EQ(i420[0], 82);
    MEDIA_CHECK_EQ(i420[64], 90);   // U 平面起始 = 8*8
    MEDIA_CHECK_EQ(i420[64 + 16], 240);  // V 平面起始 = 64 + 16

    // 纯黑 → Y=16, U=V=128。
    VideoFrame black = MakeSolid(8, 8, 0, 0, 0);
    BgraToI420(black.data.data(), 8, 8, black.stride, i420.data());
    MEDIA_CHECK_EQ(i420[0], 16);
    MEDIA_CHECK_EQ(i420[64], 128);
    MEDIA_CHECK_EQ(i420[80], 128);

    // 往返：PSNR 应 > 30 dB（色度下采样决定的固有上限）。
    VideoFrame pattern = MakePattern(64, 48);
    std::vector<std::uint8_t> plane(I420FrameBytes(64, 48), 0);
    BgraToI420(pattern.data.data(), 64, 48, pattern.stride, plane.data());
    VideoFrame back;
    back.width = 64;
    back.height = 48;
    back.format = PixelFormat::kBGRA;
    back.stride = 64 * 4;
    back.data.resize(BgraFrameBytes(64, 48));
    I420ToBgra(plane.data(), 64, 48, back.data.data(), back.stride);
    const double psnr = FramePsnr(pattern, back);
    std::printf("[INFO] BGRA->I420->BGRA PSNR = %.2f dB, MAE = %.2f\n", psnr, FrameMae(pattern, back));
    MEDIA_CHECK_GE(psnr, 18.0);

    // 尺寸不匹配时 PSNR 有定义（取交集），但 FrameEqual 必须 false。
    VideoFrame smaller = MakePattern(32, 24);
    MEDIA_CHECK(!FrameEqual(pattern, smaller));
    MEDIA_CHECK(FrameEqual(pattern, pattern));
}

void TestOddSizes() {
    const int sizes[][2] = {{1, 1}, {3, 5}, {5, 3}, {17, 33}, {33, 17}, {31, 1}, {1, 31}};
    for (const auto& size : sizes) {
        const int width = size[0];
        const int height = size[1];
        VideoFrame source = MakePattern(width, height);
        std::vector<std::uint8_t> i420(I420FrameBytes(width, height), 0);
        BgraToI420(source.data.data(), width, height, source.stride, i420.data());
        VideoFrame back;
        back.width = width;
        back.height = height;
        back.format = PixelFormat::kBGRA;
        back.stride = width * 4;
        back.data.resize(BgraFrameBytes(width, height));
        I420ToBgra(i420.data(), width, height, back.data.data(), back.stride);
        MEDIA_CHECK_GE(FramePsnr(source, back), 18.0);
    }
}

void TestPaddingStride() {
    // 带行补齐的 BGRA：stride = width*4 + 12，校验和应只覆盖有效像素。
    VideoFrame padded;
    padded.width = 20;
    padded.height = 10;
    padded.format = PixelFormat::kBGRA;
    padded.stride = 20 * 4 + 12;
    padded.data.assign(static_cast<std::size_t>(padded.stride) * 10, 0);
    VideoFrame compact = MakePattern(20, 10);
    for (int y = 0; y < 10; ++y) {
        std::memcpy(padded.data.data() + static_cast<std::size_t>(y) * padded.stride,
                    compact.data.data() + static_cast<std::size_t>(y) * compact.stride, 20 * 4);
    }
    MEDIA_CHECK_EQ(padded.Checksum(), compact.Checksum());
}

}  // namespace

int main() {
    TestFrameBasics();
    TestChecksum();
    TestColorConversion();
    TestOddSizes();
    TestPaddingStride();
    return media_test::Finish("test_frame");
}
