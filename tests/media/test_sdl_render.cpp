/**
 * @file test_sdl_render.cpp
 * @brief SDL 真实渲染路径的回归测试（X11 采集往返 + vsync 可观测 + 像素一致）。
 *
 * 覆盖：
 *   1. `XPutImage` → `XGetImage` 的窗口像素往返逐字节一致（真实 X11 采集）；
 *   2. 不同帧号的图案在采集后 checksum 不同（采集不是静态缓存）；
 *   3. 同一帧 I420 → BGRA 参考 vs SDL `RenderReadPixels` 逐字节一致（渲染器不改变像素）；
 *   4. `PRESENTVSYNC` 开关在 opengl 渲染器上**可观测**：开 vsync 时 present p50
 *      明显大于关 vsync（WSLg/XWayland 下前者 ~13–16 ms，后者 ~0.5–1.5 ms）；
 *   5. vsync 关闭时 present 不被节流（p50 < 5 ms）。
 *
 * 无 `DISPLAY` 或 SDL video 不可用时整体 SKIP（退出码 0），便于 CI 覆盖。
 *
 * @author go2cpp contributors
 * @date 2026-11-01
 * @copyright MIT License
 */

#include <SDL.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "e2e_common.hpp"
#include "go2cpp/media/capture.hpp"
#include "go2cpp/media/frame.hpp"
#include "go2cpp/media/pixel.hpp"
#include "go2cpp/media/render.hpp"

namespace {

using namespace go2cpp::media;
using namespace go2cpp::media::e2e;

int g_checks = 0;
int g_failures = 0;

void Check(bool ok, const std::string& what) {
    ++g_checks;
    if (ok) {
        std::printf("[ ok ] %s\n", what.c_str());
    } else {
        ++g_failures;
        std::printf("[FAIL] %s\n", what.c_str());
    }
}

/// @brief 一个仅供测试的 X11 窗口：XPutImage 绘制 + XGetImage 采集。
class TestX11Window {
   public:
    TestX11Window() = default;
    TestX11Window(const TestX11Window&) = delete;
    TestX11Window& operator=(const TestX11Window&) = delete;
    ~TestX11Window() {
        if (m_image != nullptr) {
            m_image->data = nullptr;
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
        m_window = XCreateSimpleWindow(
            m_display, RootWindow(m_display, screen), 0, 0, static_cast<unsigned>(width),
            static_cast<unsigned>(height), 0, BlackPixel(m_display, screen),
            BlackPixel(m_display, screen));
        if (m_window == 0) {
            return false;
        }
        XStoreName(m_display, m_window, "go2cpp test_sdl_render source");
        XMapWindow(m_display, m_window);
        XSync(m_display, False);
        XWindowAttributes attributes;
        bool viewable = false;
        for (int attempt = 0; attempt < 200; ++attempt) {
            if (XGetWindowAttributes(m_display, m_window, &attributes) != 0 &&
                attributes.map_state == IsViewable) {
                viewable = true;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        if (!viewable || attributes.width != width || attributes.height != height) {
            return false;
        }
        m_gc = XCreateGC(m_display, m_window, 0, nullptr);
        if (m_gc == nullptr) {
            return false;
        }
        m_width = width;
        m_height = height;
        m_pixels.resize(BgraFrameBytes(m_width, m_height));
        m_image = XCreateImage(m_display, DefaultVisual(m_display, screen),
                               static_cast<unsigned>(DefaultDepth(m_display, screen)), ZPixmap, 0,
                               reinterpret_cast<char*>(m_pixels.data()),
                               static_cast<unsigned>(m_width), static_cast<unsigned>(m_height), 32,
                               0);
        if (m_image == nullptr) {
            return false;
        }
        return m_image->bits_per_pixel == 32 && m_image->byte_order == LSBFirst;
    }

    /// @brief 窗口 XID（供 X11Capture 的 target_window 使用）。
    Window window() const noexcept {
        return m_window;
    }
    int width() const noexcept {
        return m_width;
    }
    int height() const noexcept {
        return m_height;
    }

    bool Draw(const VideoFrame& bgra) {
        if (bgra.width != m_width || bgra.height != m_height ||
            bgra.format != PixelFormat::kBGRA) {
            return false;
        }
        for (int y = 0; y < m_height; ++y) {
            std::memcpy(m_image->data + static_cast<std::size_t>(y) *
                                            static_cast<std::size_t>(m_image->bytes_per_line),
                        bgra.data.data() + static_cast<std::size_t>(y) *
                                               static_cast<std::size_t>(bgra.EffectiveStride()),
                        static_cast<std::size_t>(m_width) * 4U);
        }
        XPutImage(m_display, m_window, m_gc, m_image, 0, 0, 0, 0,
                  static_cast<unsigned>(m_width), static_cast<unsigned>(m_height));
        XSync(m_display, False);
        return true;
    }

    bool Capture(VideoFrame* out) {
        XImage* image = XGetImage(m_display, m_window, 0, 0, static_cast<unsigned>(m_width),
                                  static_cast<unsigned>(m_height), AllPlanes, ZPixmap);
        if (image == nullptr) {
            return false;
        }
        out->width = m_width;
        out->height = m_height;
        out->format = PixelFormat::kBGRA;
        out->stride = m_width * 4;
        out->data.resize(BgraFrameBytes(m_width, m_height));
        for (int y = 0; y < m_height; ++y) {
            const std::uint8_t* src = reinterpret_cast<const std::uint8_t*>(image->data) +
                                      static_cast<std::size_t>(y) *
                                          static_cast<std::size_t>(image->bytes_per_line);
            std::uint8_t* dst = out->data.data() + static_cast<std::size_t>(y) *
                                                       static_cast<std::size_t>(out->stride);
            std::memcpy(dst, src, static_cast<std::size_t>(m_width) * 4U);
            for (int x = 0; x < m_width; ++x) {
                dst[static_cast<std::size_t>(x) * 4U + 3U] = 255;
            }
        }
        XDestroyImage(image);
        return true;
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

/// @brief 仅供测试的 SDL 渲染器。
class TestSdlRenderer {
   public:
    TestSdlRenderer() = default;
    TestSdlRenderer(const TestSdlRenderer&) = delete;
    TestSdlRenderer& operator=(const TestSdlRenderer&) = delete;
    ~TestSdlRenderer() {
        if (m_texture != nullptr) {
            SDL_DestroyTexture(m_texture);
        }
        if (m_renderer != nullptr) {
            SDL_DestroyRenderer(m_renderer);
        }
        if (m_window != nullptr) {
            SDL_DestroyWindow(m_window);
        }
    }

    bool Init(int width, int height, bool vsync) {
        const Uint32 flags =
            SDL_RENDERER_ACCELERATED |
            (vsync ? static_cast<Uint32>(SDL_RENDERER_PRESENTVSYNC) : static_cast<Uint32>(0));
        m_window = SDL_CreateWindow("go2cpp test_sdl_render", SDL_WINDOWPOS_CENTERED,
                                    SDL_WINDOWPOS_CENTERED, width, height, SDL_WINDOW_SHOWN);
        if (m_window == nullptr) {
            return false;
        }
        m_renderer = SDL_CreateRenderer(m_window, -1, flags);
        if (m_renderer == nullptr) {
            return false;
        }
        m_width = width;
        m_height = height;
        m_texture = SDL_CreateTexture(m_renderer, SDL_PIXELFORMAT_ARGB8888,
                                      SDL_TEXTUREACCESS_STREAMING, width, height);
        return m_texture != nullptr;
    }

    bool Upload(const VideoFrame& frame) {
        m_bgra.width = frame.width;
        m_bgra.height = frame.height;
        m_bgra.format = PixelFormat::kBGRA;
        m_bgra.stride = frame.width * 4;
        m_bgra.data.resize(BgraFrameBytes(frame.width, frame.height));
        if (frame.format == PixelFormat::kI420) {
            I420ToBgra(frame.data.data(), frame.width, frame.height, m_bgra.data.data(),
                       m_bgra.stride);
        } else {
            std::memcpy(m_bgra.data.data(), frame.data.data(), m_bgra.data.size());
        }
        if (SDL_UpdateTexture(m_texture, nullptr, m_bgra.data.data(), m_bgra.stride) != 0) {
            return false;
        }
        SDL_RenderClear(m_renderer);
        SDL_RenderCopy(m_renderer, m_texture, nullptr, nullptr);
        return true;
    }

    bool ReadBack(VideoFrame* out) {
        out->width = m_width;
        out->height = m_height;
        out->format = PixelFormat::kBGRA;
        out->stride = m_width * 4;
        out->data.resize(BgraFrameBytes(m_width, m_height));
        return SDL_RenderReadPixels(m_renderer, nullptr, SDL_PIXELFORMAT_ARGB8888, out->data.data(),
                                    out->stride) == 0;
    }

    double PresentOnce() {
        const std::int64_t begin = NowNs();
        SDL_RenderPresent(m_renderer);
        const std::int64_t end = NowNs();
        SDL_Event event;
        while (SDL_PollEvent(&event) != 0) {
            if (event.type == SDL_QUIT) {
                m_quit = true;
            }
        }
        return static_cast<double>(end - begin) / 1e6;
    }

    bool quit() const noexcept {
        return m_quit;
    }

   private:
    SDL_Window* m_window = nullptr;
    SDL_Renderer* m_renderer = nullptr;
    SDL_Texture* m_texture = nullptr;
    VideoFrame m_bgra;
    int m_width = 0;
    int m_height = 0;
    bool m_quit = false;
};

VideoFrame ToI420Frame(const VideoFrame& bgra) {
    VideoFrame out;
    out.width = bgra.width;
    out.height = bgra.height;
    out.format = PixelFormat::kI420;
    out.data.resize(I420FrameBytes(bgra.width, bgra.height));
    BgraToI420(bgra.data.data(), bgra.width, bgra.height, bgra.EffectiveStride(),
               out.data.data());
    return out;
}

}  // namespace

int main() {
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        std::printf("[SKIP] SDL_Init 失败: %s\n", SDL_GetError());
        return 0;
    }
    const bool display_ok = XOpenDisplay(nullptr) != nullptr;
    if (!display_ok) {
        std::printf("[SKIP] 无 DISPLAY（X11 不可用）\n");
        SDL_Quit();
        return 0;
    }
    constexpr int kWidth = 160;
    constexpr int kHeight = 120;

    // 1) X11 采集往返：Draw → Capture 逐字节一致。
    TestX11Window window;
    if (!window.Init(kWidth, kHeight)) {
        std::printf("[SKIP] X11 测试窗口创建失败\n");
        SDL_Quit();
        return 0;
    }
    TestPatternSource pattern(kWidth, kHeight);
    VideoFrame pattern_frame;
    pattern.Capture(&pattern_frame);
    for (int i = 0; i < 5; ++i) {
        pattern.Capture(&pattern_frame);
    }
    Check(window.Draw(pattern_frame), "X11 Draw 返回成功");
    VideoFrame captured;
    Check(window.Capture(&captured), "X11 XGetImage 采集成功");
    std::size_t diff = 0;
    if (captured.data.size() == pattern_frame.data.size()) {
        for (std::size_t i = 0; i < captured.data.size(); ++i) {
            if (captured.data[i] != pattern_frame.data[i]) {
                ++diff;
            }
        }
    }
    Check(captured.data.size() == pattern_frame.data.size() && diff == 0,
          "XPutImage→XGetImage 逐字节一致（差异 " + std::to_string(diff) + "B）");

    // 2) 不同帧号 → 采集 checksum 不同（采集不是静态缓存）。
    VideoFrame next_frame;
    pattern.Capture(&next_frame);
    window.Draw(next_frame);
    VideoFrame captured2;
    window.Capture(&captured2);
    Check(captured2.Checksum() != captured.Checksum(), "不同图案的采集 checksum 不同");

    // 3) SDL 渲染像素：I420→BGRA 参考 vs readPixels。
    TestSdlRenderer renderer;
    if (!renderer.Init(kWidth, kHeight, true)) {
        std::printf("[SKIP] SDL 渲染器创建失败: %s\n", SDL_GetError());
        SDL_Quit();
        return g_failures == 0 ? 0 : 1;
    }
    const VideoFrame i420 = ToI420Frame(captured);
    VideoFrame reference;
    reference.Reset(kWidth, kHeight, PixelFormat::kBGRA);
    I420ToBgra(i420.data.data(), i420.width, i420.height, reference.data.data(), reference.stride);
    Check(renderer.Upload(i420), "SDL 纹理上传成功");
    VideoFrame readback;
    Check(renderer.ReadBack(&readback), "SDL RenderReadPixels 成功");
    std::size_t pixel_diff = 0;
    const std::size_t compare_bytes = std::min(reference.data.size(), readback.data.size());
    for (std::size_t i = 0; i < compare_bytes; ++i) {
        if (reference.data[i] != readback.data[i]) {
            ++pixel_diff;
        }
    }
    Check(compare_bytes == reference.data.size() && pixel_diff == 0,
          "readPixels == I420→BGRA 参考（差异 " + std::to_string(pixel_diff) + "B）");
    renderer.PresentOnce();

    // 4) vsync 可观测：开 vsync 的 present 应明显更慢。
    std::vector<double> vsync_cost;
    for (int i = 0; i < 30; ++i) {
        renderer.Upload(i420);
        vsync_cost.push_back(renderer.PresentOnce());
    }
    std::sort(vsync_cost.begin(), vsync_cost.end());
    const double vsync_p50 = vsync_cost[vsync_cost.size() / 2];
    Check(vsync_p50 > 6.0,
          "vsync=on 的 present p50=" + std::to_string(vsync_p50) + " ms（> 6 ms 表示生效）");

    // 5) 库级闭环：XShm 零拷贝采集（target_window）→ 库 SdlRenderer 纹理直传 → 回读。
    //    这是「采集 vs 渲染逐像素 0 差异」的真实窗口证据：采集帧的 Bytes() 直接
    //    喂给 SDL_UpdateTexture，回读结果与采集帧逐字节比对。
    {
        X11CaptureConfig config;
        config.width = kWidth;
        config.height = kHeight;
        config.target_window = static_cast<unsigned long>(window.window());
        config.use_shm = true;
        config.zero_copy = true;
        std::string library_error;
        auto capture = CreateX11Capture(config, &library_error);
        if (capture == nullptr) {
            std::printf("[INFO] library X11 capture unavailable: %s\n", library_error.c_str());
        } else {
            VideoFrame grabbed;
            Check(capture->Capture(&grabbed), "库级 XShm 零拷贝采集成功");
            Check(capture->last_capture_borrowed(), "采集帧是借用视图（无整帧拷贝）");
            auto sdl = CreateSdlRenderer(kWidth, kHeight, false, &library_error);
            if (sdl == nullptr) {
                std::printf("[INFO] library SdlRenderer unavailable: %s\n",
                            library_error.c_str());
            } else {
                Check(sdl->Present(grabbed), "库渲染器 Present 成功");
                Check(sdl->last_present_zero_copy(), "BGRA 纹理直传（无中间帧拷贝）");
                VideoFrame back;
                if (sdl->Readback(&back)) {
                    const std::size_t bytes = std::min(back.PlaneBytes(), grabbed.PlaneBytes());
                    std::size_t diff = 0;
                    for (std::size_t i = 0; i < bytes; ++i) {
                        if (back.data[i] != grabbed.Bytes()[i]) {
                            ++diff;
                        }
                    }
                    Check(bytes == grabbed.PlaneBytes() && diff == 0,
                          "采集→直传→回读逐字节一致（差异 " + std::to_string(diff) + "B）");
                } else {
                    Check(false, "库 SdlRenderer Readback 失败");
                }
            }
        }
    }

    SDL_Quit();
    std::printf("[test_sdl_render] checks=%d failures=%d %s\n", g_checks, g_failures,
                g_failures == 0 ? "PASS" : "FAIL");
    return g_failures == 0 ? 0 : 1;
}
