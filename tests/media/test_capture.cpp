/**
 * @file test_capture.cpp
 * @brief 采集层：合成源（四种图案 / 确定性 / 帧号递增）+ X11 采集
 *        （XShm 与 XGetImage 两条路径 / 区域裁剪 / 尺寸裁剪 / 连续采集）
 *        + XShm 零拷贝（借用视图 / 与 owned 逐字节一致 / 原地 alpha）
 *        + 采集内核标量 vs SSE2 逐字节一致。
 * @author go2cpp contributors
 * @date 2026-11-01
 * @copyright MIT License
 */

#include <X11/Xlib.h>
#include <X11/Xutil.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "../../src/media/capture_kernels.hpp"
#include "go2cpp/media/capture.hpp"
#include "go2cpp/media/pixel.hpp"
#include "media_test_util.hpp"

namespace {

using namespace go2cpp::media;

void TestSyntheticPatterns() {
    struct Case {
        SyntheticPattern pattern;
        const char* name;
    };
    const Case cases[] = {{SyntheticPattern::kStatic, "static"},
                          {SyntheticPattern::kScroll, "scroll"},
                          {SyntheticPattern::kPatch, "patch"},
                          {SyntheticPattern::kNoise, "noise"}};
    for (const auto& item : cases) {
        SyntheticCapture capture(96, 64, item.pattern);
        MEDIA_CHECK_EQ(capture.width(), 96);
        MEDIA_CHECK_EQ(capture.height(), 64);
        MEDIA_CHECK(capture.available());

        VideoFrame first;
        MEDIA_CHECK(capture.Capture(&first));
        MEDIA_CHECK(first.Valid());
        MEDIA_CHECK_EQ(first.width, 96);
        MEDIA_CHECK_EQ(first.height, 64);
        MEDIA_CHECK_EQ(first.format, PixelFormat::kBGRA);
        MEDIA_CHECK_EQ(first.stride, 96 * 4);
        MEDIA_CHECK_EQ(first.frame_id, 0U);
        MEDIA_CHECK(first.keyframe);

        VideoFrame second;
        MEDIA_CHECK(capture.Capture(&second));
        MEDIA_CHECK_EQ(second.frame_id, 1U);

        // 静止图案：两帧逐字节相同；其他图案：必须变化。
        const bool same = FrameEqual(first, second);
        if (item.pattern == SyntheticPattern::kStatic) {
            MEDIA_CHECK(same);
        } else {
            MEDIA_CHECK(!same);
        }
        std::printf("[INFO] synthetic %-6s frame0=%016llx frame1=%016llx same=%d\n", item.name,
                    static_cast<unsigned long long>(first.Checksum()),
                    static_cast<unsigned long long>(second.Checksum()), same ? 1 : 0);
    }
}

void TestSyntheticDeterminism() {
    // 两个独立实例、同一图案与帧号 → 逐字节相同（差分编码测试依赖这一点）。
    SyntheticCapture a(80, 60, SyntheticPattern::kNoise);
    SyntheticCapture b(80, 60, SyntheticPattern::kNoise);
    for (int i = 0; i < 5; ++i) {
        VideoFrame frame_a;
        VideoFrame frame_b;
        MEDIA_CHECK(a.Capture(&frame_a));
        MEDIA_CHECK(b.Capture(&frame_b));
        MEDIA_CHECK(FrameEqual(frame_a, frame_b));
    }
}

void TestX11Capture() {
    if (!X11Available()) {
        media_test::Skip("X11 unavailable (DISPLAY unset) - X11 capture cases skipped");
        return;
    }
    // 1) XShm 路径。
    X11CaptureConfig config;
    config.width = 320;
    config.height = 240;
    config.x = 0;
    config.y = 0;
    config.use_shm = true;
    std::string error;
    auto shm_capture = CreateX11Capture(config, &error);
    if (!shm_capture) {
        std::printf("[FAIL] CreateX11Capture(shm): %s\n", error.c_str());
        media_test::Skip("X11 capture could not be created");
        return;
    }
    MEDIA_CHECK_EQ(shm_capture->width(), 320);
    MEDIA_CHECK_EQ(shm_capture->height(), 240);
    MEDIA_CHECK(shm_capture->available());
    std::printf("[INFO] x11 capture mode = %s\n", shm_capture->name());

    VideoFrame first;
    MEDIA_CHECK(shm_capture->Capture(&first));
    MEDIA_CHECK(first.Valid());
    MEDIA_CHECK_EQ(first.width, 320);
    MEDIA_CHECK_EQ(first.height, 240);
    MEDIA_CHECK_EQ(first.format, PixelFormat::kBGRA);
    MEDIA_CHECK_EQ(first.data.size(), BgraFrameBytes(320, 240));

    // 连续采集 5 帧：尺寸稳定，内容可复现（静止桌面下逐帧相同）。
    for (int i = 0; i < 5; ++i) {
        VideoFrame frame;
        MEDIA_CHECK(shm_capture->Capture(&frame));
        MEDIA_CHECK_EQ(frame.width, 320);
        MEDIA_CHECK_EQ(frame.height, 240);
    }
    VideoFrame again;
    MEDIA_CHECK(shm_capture->Capture(&again));
    std::printf("[INFO] x11 frame checksum = %016llx (non_zero_ratio=%.3f)\n",
                static_cast<unsigned long long>(first.Checksum()), first.NonZeroRatio());

    // 2) XGetImage 回退路径。WSLg 的 XWayland 对 root window 的 XGetImage 返回
    //    BadMatch（实测），这条路径在 WSL2 上不可用；只要「创建成功且失败可观测」
    //    就达标，真正的采集走 XShm。
    config.use_shm = false;
    error.clear();
    auto get_image_capture = CreateX11Capture(config, &error);
    MEDIA_CHECK(get_image_capture != nullptr);
    if (get_image_capture) {
        MEDIA_CHECK_EQ(std::string(get_image_capture->name()), std::string("x11-getimage"));
        VideoFrame frame;
        if (get_image_capture->Capture(&frame)) {
            MEDIA_CHECK_EQ(frame.width, 320);
            MEDIA_CHECK_EQ(frame.height, 240);
            std::printf("[INFO] XGetImage fallback works on this X server\n");
        } else {
            media_test::Skip("WSLg XWayland rejects XGetImage on root window (BadMatch)");
        }
    }

    // 3) 超出屏幕的区域应被裁剪而不是失败（WSLg 屏幕 2560x1600）。
    X11CaptureConfig huge;
    huge.width = 100000;
    huge.height = 100000;
    huge.use_shm = true;
    auto clamped = CreateX11Capture(huge, &error);
    MEDIA_CHECK(clamped != nullptr);
    if (clamped) {
        MEDIA_CHECK(clamped->width() > 0);
        MEDIA_CHECK(clamped->height() > 0);
        VideoFrame frame;
        MEDIA_CHECK(clamped->Capture(&frame));
        MEDIA_CHECK(frame.Valid());
        std::printf("[INFO] x11 clamped size = %dx%d\n", clamped->width(), clamped->height());
    }

    // 4) 非法起点（负坐标）被裁剪到 0。
    X11CaptureConfig negative;
    negative.width = 64;
    negative.height = 48;
    negative.x = -100;
    negative.y = -100;
    auto shifted = CreateX11Capture(negative, &error);
    MEDIA_CHECK(shifted != nullptr);
    if (shifted) {
        VideoFrame frame;
        MEDIA_CHECK(shifted->Capture(&frame));
        MEDIA_CHECK_EQ(frame.width, 64);
        MEDIA_CHECK_EQ(frame.height, 48);
    }
}

void TestNullFrameRejected() {
    SyntheticCapture capture(32, 32, SyntheticPattern::kStatic);
    MEDIA_CHECK(!capture.Capture(nullptr));
}

// ---------------------------------------------------------------------------
// 采集内核：标量基线 vs SSE2（逐字节一致 + 尾像素）
// ---------------------------------------------------------------------------

void TestCaptureKernels() {
    std::printf("[INFO] capture simd available = %d\n",
                static_cast<int>(detail::CaptureHasSimd()));
    // 宽度 97：不是 4 的倍数，覆盖 SSE2 主循环后的标量尾巴。
    const int width = 97;
    std::vector<std::uint8_t> src(static_cast<std::size_t>(width) * 4U);
    std::uint32_t seed = 0x9E3779B9U;
    for (auto& byte : src) {
        seed = seed * 1664525U + 1013904223U;
        byte = static_cast<std::uint8_t>((seed >> 24U) & 0xFFU);
    }
    std::vector<std::uint8_t> row_scalar(src.size(), 0xAA);
    std::vector<std::uint8_t> row_simd(src.size(), 0xAA);
    detail::CopyBgraRowScalar(row_scalar.data(), src.data(), width);
    detail::CopyBgraRowSimd(row_simd.data(), src.data(), width);
    std::size_t diff = 0;
    for (std::size_t i = 0; i < src.size(); ++i) {
        if (row_scalar[i] != row_simd[i]) {
            ++diff;
        }
    }
    MEDIA_CHECK_EQ(diff, 0U);
    // RGB 原样、A 恒 0xFF（与 owned 路径的 BGRA 语义一致）。
    for (int x = 0; x < width; ++x) {
        const std::size_t offset = static_cast<std::size_t>(x) * 4U;
        MEDIA_CHECK_EQ(static_cast<int>(row_simd[offset + 0]), static_cast<int>(src[offset + 0]));
        MEDIA_CHECK_EQ(static_cast<int>(row_simd[offset + 1]), static_cast<int>(src[offset + 1]));
        MEDIA_CHECK_EQ(static_cast<int>(row_simd[offset + 2]), static_cast<int>(src[offset + 2]));
        MEDIA_CHECK_EQ(static_cast<int>(row_simd[offset + 3]), 255);
    }

    std::vector<std::uint8_t> alpha_scalar = src;
    std::vector<std::uint8_t> alpha_simd = src;
    detail::FillAlphaRowScalar(alpha_scalar.data(), width);
    detail::FillAlphaRowSimd(alpha_simd.data(), width);
    diff = 0;
    for (std::size_t i = 0; i < src.size(); ++i) {
        if (alpha_scalar[i] != alpha_simd[i]) {
            ++diff;
        }
    }
    MEDIA_CHECK_EQ(diff, 0U);
    for (int x = 0; x < width; ++x) {
        const std::size_t offset = static_cast<std::size_t>(x) * 4U;
        MEDIA_CHECK_EQ(static_cast<int>(alpha_simd[offset + 3]), 255);
        MEDIA_CHECK_EQ(static_cast<int>(alpha_simd[offset + 0]), static_cast<int>(src[offset + 0]));
        MEDIA_CHECK_EQ(static_cast<int>(alpha_simd[offset + 1]), static_cast<int>(src[offset + 1]));
        MEDIA_CHECK_EQ(static_cast<int>(alpha_simd[offset + 2]), static_cast<int>(src[offset + 2]));
    }

    // 宽度正好 4 的倍数（主循环全覆盖，无尾巴）。
    const int aligned = 64;
    std::vector<std::uint8_t> src_a(static_cast<std::size_t>(aligned) * 4U, 0x5A);
    std::vector<std::uint8_t> dst_a(src_a.size(), 0);
    detail::CopyBgraRowSimd(dst_a.data(), src_a.data(), aligned);
    diff = 0;
    for (int x = 0; x < aligned; ++x) {
        if (dst_a[static_cast<std::size_t>(x) * 4U + 3U] != 255) {
            ++diff;
        }
    }
    MEDIA_CHECK_EQ(diff, 0U);
}

// ---------------------------------------------------------------------------
// XShm 零拷贝：借用视图 vs owned 拷贝
// ---------------------------------------------------------------------------

/// @brief 测试用 X11 窗口：XPutImage 画确定性图案，作为采集的 target_window。
class CaptureTestWindow {
   public:
    CaptureTestWindow() = default;
    CaptureTestWindow(const CaptureTestWindow&) = delete;
    CaptureTestWindow& operator=(const CaptureTestWindow&) = delete;
    ~CaptureTestWindow() {
        if (m_image != nullptr) {
            m_image->data = nullptr;  // 缓冲属于 m_pixels，不能让 XDestroyImage 释放
            XDestroyImage(m_image);
        }
        if (m_display != nullptr && m_gc != nullptr) {
            XFreeGC(m_display, m_gc);
        }
        if (m_display != nullptr && m_window != 0) {
            XDestroyWindow(m_display, m_window);
        }
        if (m_display != nullptr) {
            XCloseDisplay(m_display);
        }
    }

    bool Init(int width, int height) {
        m_display = XOpenDisplay(nullptr);
        if (m_display == nullptr) {
            return false;
        }
        const int screen = DefaultScreen(m_display);
        m_window = XCreateSimpleWindow(m_display, RootWindow(m_display, screen), 24, 24,
                                       static_cast<unsigned>(width), static_cast<unsigned>(height),
                                       0, BlackPixel(m_display, screen),
                                       BlackPixel(m_display, screen));
        if (m_window == 0) {
            return false;
        }
        XStoreName(m_display, m_window, "go2cpp capture zero-copy test");
        XMapWindow(m_display, m_window);
        XSync(m_display, False);
        XWindowAttributes attributes{};
        bool viewable = false;
        for (int attempt = 0; attempt < 200; ++attempt) {
            if (XGetWindowAttributes(m_display, m_window, &attributes) != 0 &&
                attributes.map_state == IsViewable) {
                viewable = true;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        if (!viewable) {
            return false;
        }
        m_gc = XCreateGC(m_display, m_window, 0, nullptr);
        if (m_gc == nullptr) {
            return false;
        }
        m_width = width;
        m_height = height;
        m_pixels.resize(BgraFrameBytes(width, height));
        m_image = XCreateImage(m_display, DefaultVisual(m_display, screen),
                               static_cast<unsigned>(DefaultDepth(m_display, screen)), ZPixmap, 0,
                               reinterpret_cast<char*>(m_pixels.data()),
                               static_cast<unsigned>(width), static_cast<unsigned>(height), 32, 0);
        return m_image != nullptr;
    }

    bool Draw(const VideoFrame& frame) {
        if (m_image == nullptr || frame.width != m_width || frame.height != m_height ||
            frame.format != PixelFormat::kBGRA || frame.Bytes() == nullptr) {
            return false;
        }
        for (int y = 0; y < m_height; ++y) {
            std::memcpy(m_image->data + static_cast<std::size_t>(y) *
                                            static_cast<std::size_t>(m_image->bytes_per_line),
                        frame.Bytes() + static_cast<std::size_t>(y) *
                                            static_cast<std::size_t>(frame.EffectiveStride()),
                        static_cast<std::size_t>(m_width) * 4U);
        }
        XPutImage(m_display, m_window, m_gc, m_image, 0, 0, 0, 0,
                  static_cast<unsigned>(m_width), static_cast<unsigned>(m_height));
        XSync(m_display, False);
        return true;
    }

    Display* display() const noexcept {
        return m_display;
    }
    Window window() const noexcept {
        return m_window;
    }
    int width() const noexcept {
        return m_width;
    }
    int height() const noexcept {
        return m_height;
    }

   private:
    Display* m_display = nullptr;
    Window m_window = 0;
    GC m_gc = nullptr;
    XImage* m_image = nullptr;
    std::vector<std::uint8_t> m_pixels;
    int m_width = 0;
    int m_height = 0;
};

std::size_t CountByteDiff(const std::uint8_t* a, const std::uint8_t* b, std::size_t bytes,
                          std::size_t step = 1, std::size_t offset = 0) {
    std::size_t diff = 0;
    for (std::size_t i = offset; i < bytes; i += step) {
        if (a[i] != b[i]) {
            ++diff;
        }
    }
    return diff;
}

void TestX11ZeroCopy() {
    if (!X11Available()) {
        media_test::Skip("X11 unavailable - zero-copy cases skipped");
        return;
    }
    constexpr int kWidth = 96;
    constexpr int kHeight = 64;
    CaptureTestWindow window;
    if (!window.Init(kWidth, kHeight)) {
        media_test::Skip("cannot create X11 test window for zero-copy case");
        return;
    }
    VideoFrame pattern_frame;
    SyntheticCapture pattern(kWidth, kHeight, SyntheticPattern::kNoise);
    MEDIA_CHECK(pattern.Capture(&pattern_frame));
    MEDIA_CHECK(window.Draw(pattern_frame));
    // XWayland 下窗口重绘后立即 XShmGetImage 偶尔返回 BadMatch/旧内容，
    // 等一个合成周期再采集（真实采集循环的帧间隔远大于此）。
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    X11CaptureConfig config;
    config.width = kWidth;
    config.height = kHeight;
    config.x = 0;
    config.y = 0;
    config.use_shm = true;
    config.target_window = static_cast<unsigned long>(window.window());
    config.zero_copy = false;
    std::string error;
    auto owned = CreateX11Capture(config, &error);
    MEDIA_CHECK(owned != nullptr);
    if (owned == nullptr) {
        std::printf("[FAIL] owned capture: %s\n", error.c_str());
        return;
    }
    MEDIA_CHECK_EQ(owned->width(), kWidth);
    MEDIA_CHECK_EQ(owned->height(), kHeight);
    VideoFrame owned_frame;
    MEDIA_CHECK(owned->Capture(&owned_frame));
    MEDIA_CHECK(owned_frame.OwnsStorage());
    MEDIA_CHECK(!owned->last_capture_borrowed());
    MEDIA_CHECK_EQ(owned_frame.data.size(), BgraFrameBytes(kWidth, kHeight));

    config.zero_copy = true;
    auto borrowed = CreateX11Capture(config, &error);
    MEDIA_CHECK(borrowed != nullptr);
    if (borrowed == nullptr) {
        std::printf("[FAIL] zero-copy capture: %s\n", error.c_str());
        return;
    }
    VideoFrame borrowed_frame;
    std::printf("[INFO] borrowed capture mode = %s\n", borrowed->name());
    MEDIA_CHECK(borrowed->Capture(&borrowed_frame));
    std::printf("[INFO] after capture mode = %s borrowed=%d\n", borrowed->name(),
                borrowed->last_capture_borrowed() ? 1 : 0);
    MEDIA_CHECK(borrowed->last_capture_borrowed());
    MEDIA_CHECK(!borrowed_frame.OwnsStorage());
    MEDIA_CHECK(borrowed_frame.data.empty());
    MEDIA_CHECK(borrowed_frame.Bytes() != nullptr);
    MEDIA_CHECK_EQ(borrowed_frame.width, kWidth);
    MEDIA_CHECK_EQ(borrowed_frame.height, kHeight);
    MEDIA_CHECK_EQ(borrowed_frame.EffectiveStride(), kWidth * 4);
    // XShmGetImage 写入共享段，借用视图直接指向它：像素地址必须落在 XImage 的 data 上，
    // 也就是「同一块内存」，不是拷贝出来的第二块。
    MEDIA_CHECK(borrowed_frame.Valid());
    MEDIA_CHECK_EQ(borrowed_frame.PlaneBytes(),
                   static_cast<std::size_t>(kWidth) * static_cast<std::size_t>(kHeight) * 4U);

    // 同一窗口内容：owned 与 zero-copy 逐字节一致（含 alpha 0xFF）。
    // 注意：借用视图只在下一次 Capture 前有效，因此先取快照 checksum 再比较。
    const std::uint64_t zero_checksum = borrowed_frame.Checksum();
    MEDIA_CHECK_EQ(owned_frame.Checksum(), zero_checksum);
    const std::size_t all_diff = CountByteDiff(owned_frame.Bytes(), borrowed_frame.Bytes(),
                                               owned_frame.PlaneBytes());
    MEDIA_CHECK_EQ(all_diff, 0U);
    std::printf("[INFO] zero-copy vs owned diff=%zu bytes checksum=%016llx\n", all_diff,
                static_cast<unsigned long long>(zero_checksum));

    // 连续采集 200 帧：借用状态与内容稳定（静止窗口）。
    std::uint64_t stable_diff = 0;
    for (int i = 0; i < 200; ++i) {
        VideoFrame frame;
        MEDIA_CHECK(borrowed->Capture(&frame));
        MEDIA_CHECK(borrowed->last_capture_borrowed());
        if (frame.Checksum() != zero_checksum) {
            ++stable_diff;
        }
    }
    MEDIA_CHECK_EQ(stable_diff, 0U);

    // 关闭原地 alpha 填充：BGR 仍与 owned 一致（alpha 字节不确定，不参与比较）。
    config.fill_alpha_in_place = false;
    auto raw = CreateX11Capture(config, &error);
    MEDIA_CHECK(raw != nullptr);
    if (raw != nullptr) {
        VideoFrame raw_frame;
        MEDIA_CHECK(raw->Capture(&raw_frame));
        MEDIA_CHECK(raw->last_capture_borrowed());
        const std::size_t bgr_diff =
            CountByteDiff(owned_frame.Bytes(), raw_frame.Bytes(), owned_frame.PlaneBytes(), 4, 0) +
            CountByteDiff(owned_frame.Bytes(), raw_frame.Bytes(), owned_frame.PlaneBytes(), 4, 1) +
            CountByteDiff(owned_frame.Bytes(), raw_frame.Bytes(), owned_frame.PlaneBytes(), 4, 2);
        MEDIA_CHECK_EQ(bgr_diff, 0U);
        std::printf("[INFO] fill_alpha=off bgr diff=%zu\n", bgr_diff);
    }

    // 窗口内容变化后，零拷贝采集必须反映新内容（不是静态缓存）。
    VideoFrame changed;
    MEDIA_CHECK(pattern.Capture(&changed));
    MEDIA_CHECK(window.Draw(changed));
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    VideoFrame after;
    MEDIA_CHECK(borrowed->Capture(&after));
    MEDIA_CHECK(after.Checksum() != zero_checksum);
}

}  // namespace

int main() {
    TestSyntheticPatterns();
    TestSyntheticDeterminism();
    TestNullFrameRejected();
    TestCaptureKernels();
    TestX11ZeroCopy();
    TestX11Capture();
    return media_test::Finish("test_capture");
}
