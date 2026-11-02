/**
 * @file test_render.cpp
 * @brief 渲染层：无头渲染器（BGRA/I420/校验和/计数/非法帧）+ SDL2 渲染器
 *        （dummy 驱动下的纹理上传、尺寸变化重建、可见性开关）
 *        + BGRA 纹理直传（借用视图直传 / 无中间拷贝 / 回读一致性）。
 * @author go2cpp contributors
 * @date 2026-11-01
 * @copyright MIT License
 */

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "go2cpp/media/pixel.hpp"
#include "go2cpp/media/render.hpp"
#include "media_test_util.hpp"

namespace {

using namespace go2cpp::media;

/// @brief 构造 BGRA 测试帧。
VideoFrame MakeBgra(int width, int height, std::uint8_t seed) {
    VideoFrame frame;
    frame.width = width;
    frame.height = height;
    frame.format = PixelFormat::kBGRA;
    frame.stride = width * 4;
    frame.data.resize(BgraFrameBytes(width, height));
    for (std::size_t i = 0; i < frame.data.size(); i += 4) {
        frame.data[i + 0] = static_cast<std::uint8_t>(i * 3U + seed);
        frame.data[i + 1] = static_cast<std::uint8_t>(i * 5U + seed);
        frame.data[i + 2] = static_cast<std::uint8_t>(i * 7U + seed);
        frame.data[i + 3] = 255;
    }
    return frame;
}

void TestHeadless() {
    auto renderer = CreateHeadlessRenderer();
    MEDIA_CHECK(renderer != nullptr);
    MEDIA_CHECK_EQ(std::string(renderer->name()), std::string("headless"));
    MEDIA_CHECK(!renderer->visible());
    MEDIA_CHECK_EQ(renderer->presented(), 0U);

    const VideoFrame frame = MakeBgra(64, 48, 7);
    MEDIA_CHECK(renderer->Present(frame));
    MEDIA_CHECK_EQ(renderer->presented(), 1U);
    MEDIA_CHECK_EQ(renderer->last_checksum(), frame.Checksum());

    // I420 帧同样接受。
    VideoFrame i420;
    i420.width = 64;
    i420.height = 48;
    i420.format = PixelFormat::kI420;
    i420.data.assign(I420FrameBytes(64, 48), 128);
    MEDIA_CHECK(renderer->Present(i420));
    MEDIA_CHECK_EQ(renderer->presented(), 2U);

    // 非法帧拒绝。
    VideoFrame bad;
    MEDIA_CHECK(!renderer->Present(bad));
    MEDIA_CHECK_EQ(renderer->presented(), 2U);
}

void TestSdl() {
    // 用 dummy 驱动保证无显示环境也能跑；有真实显示时 dummy 依然可用。
    ::setenv("SDL_VIDEODRIVER", "dummy", 1);
    if (!SdlAvailable()) {
        media_test::Skip("SDL2 video subsystem unavailable");
        return;
    }
    std::string error;
    auto renderer = CreateSdlRenderer(64, 48, true, &error);
    if (renderer == nullptr) {
        media_test::Skip("SDL2 headless renderer unavailable");
        return;
    }
    MEDIA_CHECK_EQ(std::string(renderer->name()), std::string("sdl-headless"));
    MEDIA_CHECK(!renderer->visible());

    const VideoFrame frame = MakeBgra(64, 48, 3);
    MEDIA_CHECK(renderer->Present(frame));
    MEDIA_CHECK_EQ(renderer->presented(), 1U);
    MEDIA_CHECK_EQ(renderer->last_checksum(), frame.Checksum());

    // 尺寸变化：应重建纹理并继续工作。
    const VideoFrame bigger = MakeBgra(128, 96, 5);
    MEDIA_CHECK(renderer->Present(bigger));
    MEDIA_CHECK_EQ(renderer->presented(), 2U);
    MEDIA_CHECK_EQ(renderer->last_checksum(), bigger.Checksum());

    // I420 输入：内部转 BGRA 后上传。
    VideoFrame i420;
    i420.width = 128;
    i420.height = 96;
    i420.format = PixelFormat::kI420;
    i420.data.assign(I420FrameBytes(128, 96), 100);
    MEDIA_CHECK(renderer->Present(i420));

    // 真实窗口（dummy 驱动下窗口不可见，但创建路径应成功）。
    auto windowed = CreateSdlRenderer(160, 120, false, &error);
    if (windowed == nullptr) {
        std::printf("[INFO] CreateSdlRenderer(window) failed: %s\n", error.c_str());
        media_test::Skip("SDL window creation unavailable under dummy driver");
    } else {
        MEDIA_CHECK(windowed->visible());
        for (int i = 0; i < 10; ++i) {
            const VideoFrame loop = MakeBgra(160, 120, static_cast<std::uint8_t>(i));
            MEDIA_CHECK(windowed->Present(loop));
            MEDIA_CHECK(windowed->last_present_zero_copy());
        }
        MEDIA_CHECK_EQ(windowed->presented(), 10U);
    }
}

// ---------------------------------------------------------------------------
// 纹理直传：BGRA 直接上传（借用视图也行），I420 才转换
// ---------------------------------------------------------------------------

void TestSdlDirectUpload() {
    ::setenv("SDL_VIDEODRIVER", "dummy", 1);
    if (!SdlAvailable()) {
        media_test::Skip("SDL2 video subsystem unavailable (direct upload)");
        return;
    }
    std::string error;
    auto renderer = CreateSdlRenderer(64, 48, false, &error);
    if (renderer == nullptr) {
        media_test::Skip("SDL window renderer unavailable (direct upload)");
        return;
    }
    MEDIA_CHECK(renderer->visible());

    // 1) 自有 BGRA：直传，不复制中间帧。
    const VideoFrame frame = MakeBgra(64, 48, 11);
    MEDIA_CHECK(renderer->Present(frame));
    MEDIA_CHECK(renderer->last_present_zero_copy());
    MEDIA_CHECK_EQ(renderer->last_checksum(), frame.Checksum());

    // 2) 借用视图（外部缓冲）：直传路径必须读 Bytes()，不能依赖 data。
    std::vector<std::uint8_t> external(BgraFrameBytes(64, 48));
    for (std::size_t i = 0; i < external.size(); i += 4) {
        external[i + 0] = static_cast<std::uint8_t>(i & 0xFFU);
        external[i + 1] = static_cast<std::uint8_t>((i >> 3U) & 0xFFU);
        external[i + 2] = static_cast<std::uint8_t>((i >> 5U) & 0xFFU);
        external[i + 3] = 255;
    }
    VideoFrame borrowed;
    borrowed.width = 64;
    borrowed.height = 48;
    borrowed.format = PixelFormat::kBGRA;
    borrowed.Borrow(external.data(), 64 * 4);
    MEDIA_CHECK(renderer->Present(borrowed));
    MEDIA_CHECK(renderer->last_present_zero_copy());
    MEDIA_CHECK_EQ(renderer->last_checksum(), borrowed.Checksum());

    // 3) I420 输入需要一次格式转换 → 不是直传。
    VideoFrame i420;
    i420.width = 64;
    i420.height = 48;
    i420.format = PixelFormat::kI420;
    i420.data.assign(I420FrameBytes(64, 48), 96);
    MEDIA_CHECK(renderer->Present(i420));
    MEDIA_CHECK(!renderer->last_present_zero_copy());

    // 4) 回读接口：software 后端可用时只验证尺寸/格式与「可调用」，像素比对放到
    //    真实 X11 窗口（test_sdl_render）里做——dummy 视频驱动的窗口表面不会
    //    被合成，回读到的内容不能代表渲染结果。
    VideoFrame readback;
    if (renderer->Readback(&readback)) {
        MEDIA_CHECK_EQ(readback.width, 64);
        MEDIA_CHECK_EQ(readback.height, 48);
        MEDIA_CHECK_EQ(readback.format, PixelFormat::kBGRA);
        MEDIA_CHECK_EQ(readback.data.size(), BgraFrameBytes(64, 48));
        std::printf("[INFO] sdl readback available under dummy driver\n");
    } else {
        std::printf("[INFO] sdl readback unavailable under dummy driver\n");
    }

    // 5) 连续直传 1000 帧：计数正确（每次 Present 都走零拷贝）。
    for (int i = 0; i < 1000; ++i) {
        MEDIA_CHECK(renderer->Present(frame));
        MEDIA_CHECK(renderer->last_present_zero_copy());
    }
    MEDIA_CHECK_EQ(renderer->presented(), 1003U);
}

}  // namespace

int main() {
    TestHeadless();
    TestSdl();
    TestSdlDirectUpload();
    return media_test::Finish("test_render");
}
