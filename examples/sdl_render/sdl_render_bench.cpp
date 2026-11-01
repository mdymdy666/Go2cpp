/**
 * @file sdl_render_bench.cpp
 * @brief 真实 SDL 渲染（纹理上传 + SDL_RenderPresent + 60Hz vsync）的端到端延迟验证。
 *
 * 背景：`docs/reports/vs-webrtc.md` §4.3 指出，媒体链路的 0.16–0.72 ms 是
 *       **headless 校验和渲染**口径，不含 `SDL_RenderPresent` + 60 Hz vsync。
 *       本基准把这条链路补上：
 *
 *       X11 采集（真实窗口 XGetImage）→ delta-I420 编码 → VideoChannel 1200B 分片
 *       → UDP 回环 → 重组 → 解码 → I420→BGRA → SDL_UpdateTexture → SDL_RenderCopy
 *       → SDL_RenderPresent（真实窗口，opengl 渲染器 + PRESENTVSYNC）。
 *
 * 对照场景（同一确定性测试画面、同一 60 Hz 节拍、同一 UDP 回环）：
 *   - `headless-fixed`     ：只做 I420 校验和（vs-webrtc 报告的原始口径）；
 *   - `sdl-novsync-fixed`  ：真实窗口 + 纹理上传 + present，但关闭 vsync；
 *   - `sdl-vsync-fixed`    ：真实窗口 + present + vsync，采集紧接 vblank（最坏相位）；
 *   - `sdl-vsync-random`   ：真实窗口 + present + vsync，采集时刻在帧周期内随机
 *                            （等价于「相机/网络帧到达时刻与显示器 vblank 无关」）；
 *   - `present-only`       ：不含媒体链路，纯 SDL present 循环，量出实际 swap 周期。
 *
 * 每个场景输出：capture_ns → present 返回的 p50/p90/p99/max、逐段耗时、
 * present（含 vsync 等待）占 e2e 的比例、帧间隔与 fps、以及「离 60 Hz 下限」。
 *
 * 产物：
 *   - `/tmp/sdl_render_data.md`：全部数据表（本程序生成）；
 *   - `<artifacts>/sdl_validation_*.ppm`：采集 / I420 参考 / SDL 回读三张图；
 *   - stdout：逐场景摘要。
 *
 * @author go2cpp contributors
 * @date 2026-11-01
 * @copyright MIT License
 */

#include <SDL.h>
#include <SDL_syswm.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "e2e_common.hpp"
#include "go2cpp/bento/net/channel.hpp"
#include "go2cpp/bento/net/protocol.hpp"
#include "go2cpp/media/codec.hpp"
#include "go2cpp/media/frame.hpp"
#include "go2cpp/media/pixel.hpp"
#include "go2cpp/media/transport.hpp"

namespace {

using namespace go2cpp::media;
using namespace go2cpp::media::e2e;

// ---------------------------------------------------------------------------
// 常量
// ---------------------------------------------------------------------------

/// 画面尺寸（与媒体模块文档的默认验证尺寸一致）。
constexpr int kWidth = 320;
constexpr int kHeight = 240;
/// 60 Hz 帧周期（纳秒）。
constexpr std::int64_t kPeriodNs = 16'666'667;
/// 关键帧间隔（帧）。
constexpr int kKeyInterval = 30;
/// 单帧收包超时（毫秒）。
constexpr int kRecvTimeoutMs = 500;

// ---------------------------------------------------------------------------
// 直方图（p50/p90/p99/max/mean）
// ---------------------------------------------------------------------------

class Hist {
   public:
    void Add(double value) {
        m_values.push_back(value);
        m_sorted = false;
    }

    void Finalize() {
        if (!m_sorted) {
            std::sort(m_values.begin(), m_values.end());
            m_sorted = true;
        }
    }

    std::size_t Size() const noexcept {
        return m_values.size();
    }

    bool Empty() const noexcept {
        return m_values.empty();
    }

    /// @brief 分位数（0..1）；调用前必须 Finalize。
    double P(double q) const {
        if (m_values.empty()) {
            return 0.0;
        }
        const std::size_t n = m_values.size();
        if (n == 1) {
            return m_values.front();
        }
        const double pos = q * static_cast<double>(n - 1);
        if (pos <= 0.0) {
            return m_values.front();
        }
        if (pos >= static_cast<double>(n - 1)) {
            return m_values.back();
        }
        const std::size_t lo = static_cast<std::size_t>(pos);
        const double frac = pos - static_cast<double>(lo);
        return m_values[lo] * (1.0 - frac) + m_values[lo + 1] * frac;
    }

    double Mean() const {
        if (m_values.empty()) {
            return 0.0;
        }
        double sum = 0.0;
        for (const double value : m_values) {
            sum += value;
        }
        return sum / static_cast<double>(m_values.size());
    }

    double Min() const {
        return m_values.empty() ? 0.0 : m_values.front();
    }

    double Max() const {
        return m_values.empty() ? 0.0 : m_values.back();
    }

   private:
    std::vector<double> m_values;
    bool m_sorted = false;
};

// ---------------------------------------------------------------------------
// 时间与切片
// ---------------------------------------------------------------------------

/// @brief 睡到绝对时刻（steady_clock 纳秒）。
void SleepUntilNs(std::int64_t deadline_ns) {
    for (;;) {
        const std::int64_t now = NowNs();
        if (now >= deadline_ns) {
            return;
        }
        const std::int64_t remain = deadline_ns - now;
        if (remain > 2'000'000) {
            std::this_thread::sleep_for(std::chrono::nanoseconds(remain - 1'000'000));
        } else {
            std::this_thread::yield();
        }
    }
}

/// @brief BGRA → I420（用于「解码帧 == 采集帧的 I420」的逐帧校验）。
VideoFrame ToI420Frame(const VideoFrame& bgra) {
    VideoFrame out;
    out.width = bgra.width;
    out.height = bgra.height;
    out.format = PixelFormat::kI420;
    out.stride = 0;
    out.data.resize(I420FrameBytes(bgra.width, bgra.height));
    BgraToI420(bgra.data.data(), bgra.width, bgra.height, bgra.EffectiveStride(),
               out.data.data());
    out.frame_id = bgra.frame_id;
    out.capture_ns = bgra.capture_ns;
    return out;
}

// ---------------------------------------------------------------------------
// X11 源窗口：纯 Xlib 绘制 + XGetImage 采集（真实 X11 像素往返）
// ---------------------------------------------------------------------------

/// @brief Xlib 错误计数：默认处理器会直接退出进程，这里改为记录后继续。
int g_x11_errors = 0;

int X11ErrorHandler(Display* /*display*/, XErrorEvent* /*event*/) {
    ++g_x11_errors;
    return 0;
}

/// @brief 一个真实 X11 窗口：XPutImage 绘制测试图案，XGetImage 采集。
/// @details 不依赖 SDL，方便「采集线程」与「渲染线程」完全解耦；
///          窗口内容与 `TestPatternSource` 的 BGRA 帧逐字节一致。
class X11PatternWindow {
   public:
    X11PatternWindow() = default;
    X11PatternWindow(const X11PatternWindow&) = delete;
    X11PatternWindow& operator=(const X11PatternWindow&) = delete;
    ~X11PatternWindow() {
        Shutdown();
    }

    bool Init(int width, int height, std::string* error) {
        XSetErrorHandler(X11ErrorHandler);
        m_display = XOpenDisplay(nullptr);
        if (m_display == nullptr) {
            if (error != nullptr) {
                *error = "XOpenDisplay 失败（DISPLAY 未设置或无 X11）";
            }
            return false;
        }
        const int screen = DefaultScreen(m_display);
        m_window = XCreateSimpleWindow(
            m_display, RootWindow(m_display, screen), 0, 0, static_cast<unsigned>(width),
            static_cast<unsigned>(height), 0, BlackPixel(m_display, screen),
            BlackPixel(m_display, screen));
        if (m_window == 0) {
            if (error != nullptr) {
                *error = "XCreateSimpleWindow 失败";
            }
            return false;
        }
        XStoreName(m_display, m_window, "go2cpp sdl_render source (X11)");
        XSelectInput(m_display, m_window, ExposureMask | StructureNotifyMask);
        XMapWindow(m_display, m_window);
        XSync(m_display, False);

        // 等窗口真正进入 IsViewable（XGetImage 对未映射窗口会返回 BadMatch）。
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
        if (!viewable) {
            if (error != nullptr) {
                *error = "X11 窗口未进入 IsViewable（WSLg/XWayland 未就绪？）";
            }
            return false;
        }
        m_width = attributes.width;
        m_height = attributes.height;
        if (m_width != width || m_height != height) {
            if (error != nullptr) {
                *error = "X11 窗口尺寸被改写：" + std::to_string(m_width) + "x" +
                         std::to_string(m_height);
            }
            return false;
        }
        m_gc = XCreateGC(m_display, m_window, 0, nullptr);
        if (m_gc == nullptr) {
            if (error != nullptr) {
                *error = "XCreateGC 失败";
            }
            return false;
        }
        m_pixels.resize(BgraFrameBytes(m_width, m_height));
        m_image = XCreateImage(m_display, DefaultVisual(m_display, screen),
                               static_cast<unsigned>(DefaultDepth(m_display, screen)), ZPixmap, 0,
                               reinterpret_cast<char*>(m_pixels.data()),
                               static_cast<unsigned>(m_width), static_cast<unsigned>(m_height), 32,
                               0);
        if (m_image == nullptr) {
            if (error != nullptr) {
                *error = "XCreateImage 失败";
            }
            return false;
        }
        m_direct = m_image->bits_per_pixel == 32 && m_image->byte_order == LSBFirst;
        return true;
    }

    /// @brief 把一帧 BGRA 画到窗口（XPutImage + XSync）。
    bool Draw(const VideoFrame& bgra, double* elapsed_ms, std::string* error) {
        if (m_image == nullptr || bgra.width != m_width || bgra.height != m_height ||
            bgra.format != PixelFormat::kBGRA) {
            if (error != nullptr) {
                *error = "Draw 帧尺寸/格式不匹配";
            }
            return false;
        }
        const std::int64_t begin = NowNs();
        if (m_direct) {
            for (int y = 0; y < m_height; ++y) {
                const std::uint8_t* src =
                    bgra.data.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(
                                                                bgra.EffectiveStride());
                std::memcpy(m_image->data + static_cast<std::size_t>(y) *
                                                static_cast<std::size_t>(m_image->bytes_per_line),
                            src, static_cast<std::size_t>(m_width) * 4U);
            }
        } else {
            for (int y = 0; y < m_height; ++y) {
                const std::uint8_t* row =
                    bgra.data.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(
                                                                bgra.EffectiveStride());
                for (int x = 0; x < m_width; ++x) {
                    XPutPixel(m_image, x, y, PixelFromBgra(row + static_cast<std::size_t>(x) * 4U));
                }
            }
        }
        XPutImage(m_display, m_window, m_gc, m_image, 0, 0, 0, 0, static_cast<unsigned>(m_width),
                  static_cast<unsigned>(m_height));
        XSync(m_display, False);
        if (elapsed_ms != nullptr) {
            *elapsed_ms = static_cast<double>(NowNs() - begin) / 1e6;
        }
        return true;
    }

    /// @brief 用 XGetImage 抓窗口像素（真实 X11 采集路径）。
    bool Capture(VideoFrame* out, double* elapsed_ms, std::string* error) {
        const std::int64_t begin = NowNs();
        XImage* image = XGetImage(m_display, m_window, 0, 0, static_cast<unsigned>(m_width),
                                  static_cast<unsigned>(m_height), AllPlanes, ZPixmap);
        if (image == nullptr) {
            if (error != nullptr) {
                *error = "XGetImage 失败";
            }
            return false;
        }
        out->width = m_width;
        out->height = m_height;
        out->format = PixelFormat::kBGRA;
        out->stride = m_width * 4;
        out->data.resize(BgraFrameBytes(m_width, m_height));
        if (image->bits_per_pixel == 32 && image->byte_order == LSBFirst) {
            for (int y = 0; y < m_height; ++y) {
                const std::uint8_t* src =
                    reinterpret_cast<const std::uint8_t*>(image->data) +
                    static_cast<std::size_t>(y) *
                        static_cast<std::size_t>(image->bytes_per_line);
                std::uint8_t* dst = out->data.data() + static_cast<std::size_t>(y) *
                                                          static_cast<std::size_t>(out->stride);
                std::memcpy(dst, src, static_cast<std::size_t>(m_width) * 4U);
                for (int x = 0; x < m_width; ++x) {
                    dst[static_cast<std::size_t>(x) * 4U + 3U] = 255;
                }
            }
        } else {
            for (int y = 0; y < m_height; ++y) {
                std::uint8_t* dst = out->data.data() + static_cast<std::size_t>(y) *
                                                          static_cast<std::size_t>(out->stride);
                for (int x = 0; x < m_width; ++x) {
                    const unsigned long pixel = XGetPixel(image, x, y);
                    dst[static_cast<std::size_t>(x) * 4U + 0U] =
                        static_cast<std::uint8_t>(pixel & 0xFFU);
                    dst[static_cast<std::size_t>(x) * 4U + 1U] =
                        static_cast<std::uint8_t>((pixel >> 8U) & 0xFFU);
                    dst[static_cast<std::size_t>(x) * 4U + 2U] =
                        static_cast<std::uint8_t>((pixel >> 16U) & 0xFFU);
                    dst[static_cast<std::size_t>(x) * 4U + 3U] = 255;
                }
            }
        }
        XDestroyImage(image);
        if (elapsed_ms != nullptr) {
            *elapsed_ms = static_cast<double>(NowNs() - begin) / 1e6;
        }
        return true;
    }

    int width() const noexcept {
        return m_width;
    }
    int height() const noexcept {
        return m_height;
    }
    Display* display() const noexcept {
        return m_display;
    }
    Window xid() const noexcept {
        return m_window;
    }

   private:
    static unsigned long PixelFromBgra(const std::uint8_t* bgra) {
        return static_cast<unsigned long>(bgra[0]) |
               (static_cast<unsigned long>(bgra[1]) << 8U) |
               (static_cast<unsigned long>(bgra[2]) << 16U);
    }

    void Shutdown() {
        if (m_image != nullptr) {
            // XDestroyImage 会释放 image->data；这里的 data 指向 C++ vector，必须先摘掉。
            m_image->data = nullptr;
            XDestroyImage(m_image);
            m_image = nullptr;
        }
        if (m_display != nullptr && m_gc != nullptr) {
            XFreeGC(m_display, m_gc);
            m_gc = nullptr;
        }
        if (m_display != nullptr && m_window != 0) {
            XDestroyWindow(m_display, m_window);
            m_window = 0;
        }
        if (m_display != nullptr) {
            XCloseDisplay(m_display);
            m_display = nullptr;
        }
    }

    Display* m_display = nullptr;
    Window m_window = 0;
    GC m_gc = nullptr;
    XImage* m_image = nullptr;
    std::vector<std::uint8_t> m_pixels;
    int m_width = 0;
    int m_height = 0;
    bool m_direct = false;
};

// ---------------------------------------------------------------------------
// SDL 渲染器：纹理上传 + RenderCopy + RenderPresent（可选 vsync）
// ---------------------------------------------------------------------------

/// @brief 单帧渲染的分段耗时与时间戳。
struct FrameRenderTimes {
    double convert_ms = 0.0;       ///< I420 → BGRA
    double upload_ms = 0.0;        ///< SDL_UpdateTexture
    double copy_ms = 0.0;          ///< SDL_RenderClear + SDL_RenderCopy
    double present_ms = 0.0;       ///< SDL_RenderPresent（含 vsync 等待）
    std::int64_t present_start_ns = 0;
    std::int64_t end_ns = 0;
    bool ok = false;
};

/// @brief SDL2 窗口渲染器。
/// @details `SDL_RENDERER_ACCELERATED` 在 WSLg 下落到 opengl（llvmpipe）；
///          只有 opengl 渲染器的 `PRESENTVSYNC` 会真实阻塞到 vblank，
///          software 渲染器即使请求了 vsync 也立即返回（实测，见报告）。
class SdlWindowRenderer {
   public:
    SdlWindowRenderer() = default;
    SdlWindowRenderer(const SdlWindowRenderer&) = delete;
    SdlWindowRenderer& operator=(const SdlWindowRenderer&) = delete;
    ~SdlWindowRenderer() {
        Shutdown();
    }

    bool Init(int width, int height, bool vsync, const std::string& title, std::string* error) {
        m_width = width;
        m_height = height;
        Uint32 flags = SDL_RENDERER_ACCELERATED;
        if (vsync) {
            flags |= SDL_RENDERER_PRESENTVSYNC;
        }
        m_window = SDL_CreateWindow(title.c_str(), SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                    width, height, SDL_WINDOW_SHOWN);
        if (m_window == nullptr) {
            if (error != nullptr) {
                *error = std::string("SDL_CreateWindow 失败: ") + SDL_GetError();
            }
            return false;
        }
        m_renderer = SDL_CreateRenderer(m_window, -1, flags);
        if (m_renderer == nullptr && vsync) {
            // vsync 请求失败时退到 software（记录在 driver 名里）。
            m_renderer = SDL_CreateRenderer(m_window, -1,
                                            SDL_RENDERER_SOFTWARE | SDL_RENDERER_PRESENTVSYNC);
            m_fallback = true;
        }
        if (m_renderer == nullptr) {
            if (error != nullptr) {
                *error = std::string("SDL_CreateRenderer 失败: ") + SDL_GetError();
            }
            return false;
        }
        SDL_RendererInfo info;
        std::memset(&info, 0, sizeof(info));
        if (SDL_GetRendererInfo(m_renderer, &info) == 0 && info.name != nullptr) {
            m_driver = info.name;
        }
        m_vsync = vsync && !m_fallback;
        m_visible = true;
        if (!EnsureTexture(width, height)) {
            if (error != nullptr) {
                *error = std::string("SDL_CreateTexture 失败: ") + SDL_GetError();
            }
            return false;
        }
        return true;
    }

    /// @brief 上传一帧并 present（完整渲染路径）。
    bool Render(const VideoFrame& frame, FrameRenderTimes* times, std::string* error) {
        if (!Upload(frame, times, error)) {
            return false;
        }
        return Present(times, error);
    }

    /// @brief 只做纹理上传 + RenderClear/Copy（不 present，便于 readPixels 校验）。
    bool Upload(const VideoFrame& frame, FrameRenderTimes* times, std::string* error) {
        if (!frame.Valid()) {
            if (error != nullptr) {
                *error = "上传帧非法";
            }
            return false;
        }
        if (!EnsureTexture(frame.width, frame.height)) {
            if (error != nullptr) {
                *error = std::string("SDL_CreateTexture 失败: ") + SDL_GetError();
            }
            return false;
        }
        FrameRenderTimes local;
        std::int64_t begin = NowNs();
        if (frame.format == PixelFormat::kI420) {
            m_bgra.width = frame.width;
            m_bgra.height = frame.height;
            m_bgra.format = PixelFormat::kBGRA;
            m_bgra.stride = frame.width * 4;
            m_bgra.data.resize(BgraFrameBytes(frame.width, frame.height));
            I420ToBgra(frame.data.data(), frame.width, frame.height, m_bgra.data.data(),
                       m_bgra.stride);
        } else {
            m_bgra.width = frame.width;
            m_bgra.height = frame.height;
            m_bgra.format = PixelFormat::kBGRA;
            m_bgra.stride = frame.EffectiveStride();
            m_bgra.data.assign(frame.data.begin(), frame.data.begin() + frame.PlaneBytes());
        }
        local.convert_ms = static_cast<double>(NowNs() - begin) / 1e6;

        begin = NowNs();
        if (SDL_UpdateTexture(m_texture, nullptr, m_bgra.data.data(), m_bgra.EffectiveStride()) !=
            0) {
            if (error != nullptr) {
                *error = std::string("SDL_UpdateTexture 失败: ") + SDL_GetError();
            }
            return false;
        }
        local.upload_ms = static_cast<double>(NowNs() - begin) / 1e6;

        begin = NowNs();
        SDL_RenderClear(m_renderer);
        SDL_RenderCopy(m_renderer, m_texture, nullptr, nullptr);
        local.copy_ms = static_cast<double>(NowNs() - begin) / 1e6;
        if (times != nullptr) {
            *times = local;
        }
        return true;
    }

    /// @brief present 一次；返回 present 内部耗时（含 vsync 等待）。
    bool Present(FrameRenderTimes* times, std::string* error) {
        FrameRenderTimes local;
        if (times != nullptr) {
            local = *times;
        }
        const std::int64_t begin = NowNs();
        SDL_RenderPresent(m_renderer);
        const std::int64_t end = NowNs();
        local.present_ms = static_cast<double>(end - begin) / 1e6;
        local.present_start_ns = begin;
        local.end_ns = end;
        local.ok = true;
        if (times != nullptr) {
            *times = local;
        }
        PollEvents(error);
        return true;
    }

    /// @brief 从 renderer 的 backbuffer 读回像素（必须在 Present 之前调用）。
    bool ReadBack(VideoFrame* out, std::string* error) {
        out->width = m_width;
        out->height = m_height;
        out->format = PixelFormat::kBGRA;
        out->stride = m_width * 4;
        out->data.resize(BgraFrameBytes(m_width, m_height));
        if (SDL_RenderReadPixels(m_renderer, nullptr, SDL_PIXELFORMAT_ARGB8888, out->data.data(),
                                 out->stride) != 0) {
            if (error != nullptr) {
                *error = std::string("SDL_RenderReadPixels 失败: ") + SDL_GetError();
            }
            return false;
        }
        return true;
    }

    const char* DriverName() const noexcept {
        return m_driver.empty() ? "unknown" : m_driver.c_str();
    }
    bool VsycRequested() const noexcept {
        return m_vsync;
    }
    bool Fallback() const noexcept {
        return m_fallback;
    }
    int width() const noexcept {
        return m_width;
    }
    int height() const noexcept {
        return m_height;
    }
    /// @brief SDL 窗口的 X11 XID（用于可选的屏幕抓取验证）。
    Window xid() const {
        if (m_window == nullptr) {
            return 0;
        }
        SDL_SysWMinfo info;
        SDL_VERSION(&info.version);
        if (!SDL_GetWindowWMInfo(m_window, &info)) {
            return 0;
        }
        if (info.subsystem != SDL_SYSWM_X11) {
            return 0;
        }
        return info.info.x11.window;
    }

   private:
    bool EnsureTexture(int width, int height) {
        if (m_texture != nullptr && m_texture_width == width && m_texture_height == height) {
            return true;
        }
        if (m_texture != nullptr) {
            SDL_DestroyTexture(m_texture);
            m_texture = nullptr;
        }
        m_texture = SDL_CreateTexture(m_renderer, SDL_PIXELFORMAT_ARGB8888,
                                      SDL_TEXTUREACCESS_STREAMING, width, height);
        if (m_texture == nullptr) {
            return false;
        }
        m_texture_width = width;
        m_texture_height = height;
        return true;
    }

    void PollEvents(std::string* error) {
        SDL_Event event;
        while (SDL_PollEvent(&event) != 0) {
            if (event.type == SDL_QUIT) {
                if (error != nullptr) {
                    *error = "SDL 窗口被关闭";
                }
                m_visible = false;
            }
        }
    }

    void Shutdown() {
        if (m_texture != nullptr) {
            SDL_DestroyTexture(m_texture);
            m_texture = nullptr;
        }
        if (m_renderer != nullptr) {
            SDL_DestroyRenderer(m_renderer);
            m_renderer = nullptr;
        }
        if (m_window != nullptr) {
            SDL_DestroyWindow(m_window);
            m_window = nullptr;
        }
    }

    SDL_Window* m_window = nullptr;
    SDL_Renderer* m_renderer = nullptr;
    SDL_Texture* m_texture = nullptr;
    VideoFrame m_bgra;
    int m_width = 0;
    int m_height = 0;
    int m_texture_width = 0;
    int m_texture_height = 0;
    bool m_visible = false;
    bool m_vsync = false;
    bool m_fallback = false;
    std::string m_driver;
};

// ---------------------------------------------------------------------------
// 媒体链路：编码 → 分片 → UDP 回环 → 重组 → 解码
// ---------------------------------------------------------------------------

/// @brief 一条真实的 UDP 回环媒体链路（与 examples/media_e2e 同一套组件）。
class UdpLoop {
   public:
    bool Init(int width, int height, std::string* error) {
        m_encoder = CreateEncoder(CodecKind::kDeltaI420, width, height);
        m_decoder = CreateDecoder(CodecKind::kDeltaI420, width, height);
        if (!m_encoder || !m_decoder) {
            if (error != nullptr) {
                *error = "编码器/解码器创建失败";
            }
            return false;
        }
        if (!m_tx.Open(error) || !m_rx.Open(error)) {
            return false;
        }
        m_tx.SetPeerPort(m_rx.port());
        m_tx_channel = std::make_unique<go2cpp::bento::VideoChannel>(
            [this](const std::uint8_t* data, std::size_t bytes) {
                m_packets.emplace_back(data, data + bytes);
                return true;
            },
            [](std::uint32_t, bool, const std::uint8_t*, std::size_t) {});
        m_rx_channel = std::make_unique<go2cpp::bento::VideoChannel>(
            [](const std::uint8_t*, std::size_t) { return true; },
            [this](std::uint32_t frame_id, bool, const std::uint8_t* data, std::size_t bytes) {
                m_delivered.assign(data, data + bytes);
                m_delivered_frame_id = frame_id;
                m_has_delivery = true;
                ++m_delivered_frames;
            });
        return true;
    }

    /// @brief 编码 + 分片 + 逐包 sendto（分段计时）。
    bool EncodeAndSend(const VideoFrame& capture, bool force_key, double* encode_ms,
                       double* fragment_ms, double* sendto_ms, std::size_t* fragments,
                       std::uint64_t* bytes, std::string* error) {
        std::int64_t begin = NowNs();
        if (!m_encoder->Encode(capture, &m_encoded)) {
            if (error != nullptr) {
                *error = "delta 编码失败";
            }
            return false;
        }
        *encode_ms = static_cast<double>(NowNs() - begin) / 1e6;

        m_packets.clear();
        begin = NowNs();
        const bool key = force_key || (m_encoded.size() > 6 && m_encoded[6] != 0);
        if (!m_tx_channel->SendFrame(capture.frame_id, key, m_encoded.data(), m_encoded.size())) {
            if (error != nullptr) {
                *error = "VideoChannel::SendFrame 失败";
            }
            return false;
        }
        *fragment_ms = static_cast<double>(NowNs() - begin) / 1e6;

        begin = NowNs();
        for (const std::vector<std::uint8_t>& packet : m_packets) {
            m_tx.Send(packet.data(), packet.size());
        }
        *sendto_ms = static_cast<double>(NowNs() - begin) / 1e6;
        *fragments = m_packets.size();
        *bytes = m_encoded.size();
        return true;
    }

    /// @brief 收包直到交付一帧（或超时）。
    bool ReceiveFrame(std::uint32_t frame_id, int timeout_ms, double* recv_ms,
                      std::string* error) {
        m_has_delivery = false;
        m_delivered.clear();
        const std::int64_t begin = NowNs();
        const std::int64_t deadline = begin + static_cast<std::int64_t>(timeout_ms) * 1'000'000;
        std::uint8_t buffer[2048];
        while (NowNs() < deadline) {
            const int received = m_rx.Recv(buffer, sizeof(buffer), 1);
            if (received > 0) {
                go2cpp::bento::FrameView view;
                if (go2cpp::bento::ParseFrame(buffer, static_cast<std::size_t>(received), &view) &&
                    view.header.type == go2cpp::bento::FrameType::kVideo) {
                    m_rx_channel->OnFrame(view.header, view.payload, view.payload_bytes);
                }
            } else {
                m_rx_channel->OnTimer(static_cast<std::uint64_t>(NowNs() / 1000000));
            }
            if (m_has_delivery && m_delivered_frame_id == frame_id) {
                break;
            }
        }
        *recv_ms = static_cast<double>(NowNs() - begin) / 1e6;
        if (!m_has_delivery) {
            if (error != nullptr) {
                *error = "收帧超时（frame_id=" + std::to_string(frame_id) + "）";
            }
            return false;
        }
        if (m_delivered_frame_id != frame_id) {
            if (error != nullptr) {
                *error = "交付帧号不匹配（want=" + std::to_string(frame_id) +
                         " got=" + std::to_string(m_delivered_frame_id) + "）";
            }
            return false;
        }
        return true;
    }

    /// @brief 解码当前交付帧。
    bool Decode(double* decode_ms, VideoFrame* out, std::string* error) {
        const std::int64_t begin = NowNs();
        const bool ok = m_decoder->Decode(m_delivered.data(), m_delivered.size(), out);
        *decode_ms = static_cast<double>(NowNs() - begin) / 1e6;
        if (!ok) {
            if (error != nullptr) {
                *error = "delta 解码失败";
            }
            return false;
        }
        return true;
    }

    std::uint64_t delivered_frames() const noexcept {
        return m_delivered_frames;
    }

   private:
    UdpEndpoint m_tx;
    UdpEndpoint m_rx;
    std::unique_ptr<go2cpp::bento::VideoChannel> m_tx_channel;
    std::unique_ptr<go2cpp::bento::VideoChannel> m_rx_channel;
    std::unique_ptr<VideoEncoder> m_encoder;
    std::unique_ptr<VideoDecoder> m_decoder;
    std::vector<std::vector<std::uint8_t>> m_packets;
    std::vector<std::uint8_t> m_encoded;
    std::vector<std::uint8_t> m_delivered;
    std::uint32_t m_delivered_frame_id = 0;
    bool m_has_delivery = false;
    std::uint64_t m_delivered_frames = 0;
};

// ---------------------------------------------------------------------------
// 场景统计
// ---------------------------------------------------------------------------

enum class RenderMode {
    kHeadless,
    kSdlNoVsync,
    kSdlVsync,
};

enum class PhaseMode {
    kFixed,       ///< 60 Hz 节拍驱动（采集紧接上一帧 present，帧率由节拍锁定）
    kRandom,      ///< 采集时刻在帧周期内随机（等价相机/网络帧与 vblank 无关）
    kContinuous,  ///< 不节流，连续采集 + present（由 vsync 自身背压，最坏 vsync 等待）
};

struct SceneStats {
    std::string name;
    std::string mode;
    std::string phase;
    int frames = 0;
    int ok_frames = 0;
    double wall_ms = 0.0;
    double fps = 0.0;
    std::uint64_t bytes_sent = 0;
    std::uint64_t fragments = 0;
    std::string note;

    Hist draw;
    Hist capture;
    Hist encode;
    Hist fragment;
    Hist sendto;
    Hist recv;
    Hist decode;
    Hist convert;
    Hist upload;
    Hist copy;
    Hist present;
    Hist e2e;
    Hist e2e_pre_present;
    Hist interval;

    std::vector<std::uint64_t> decoded_checksums;

    void Finalize() {
        draw.Finalize();
        capture.Finalize();
        encode.Finalize();
        fragment.Finalize();
        sendto.Finalize();
        recv.Finalize();
        decode.Finalize();
        convert.Finalize();
        upload.Finalize();
        copy.Finalize();
        present.Finalize();
        e2e.Finalize();
        e2e_pre_present.Finalize();
        interval.Finalize();
    }
};

// ---------------------------------------------------------------------------
// 场景运行器：同步（采集 → 编码 → UDP → 解码 → 渲染）
// ---------------------------------------------------------------------------

struct RunOptions {
    int frames = 240;
    std::uint32_t seed = 20261101U;
    std::string artifacts = "artifacts";
};

SceneStats RunSyncScene(const std::string& name, RenderMode mode, PhaseMode phase,
                        const RunOptions& options) {
    SceneStats stats;
    stats.name = name;
    stats.mode = mode == RenderMode::kHeadless
                     ? "headless"
                     : (mode == RenderMode::kSdlVsync ? "sdl-vsync" : "sdl-novsync");
    stats.phase = phase == PhaseMode::kRandom
                      ? "random"
                      : (phase == PhaseMode::kContinuous ? "continuous" : "fixed");

    std::string error;
    X11PatternWindow source;
    if (!source.Init(kWidth, kHeight, &error)) {
        stats.note = "SKIP: " + error;
        return stats;
    }
    std::unique_ptr<SdlWindowRenderer> renderer;
    if (mode != RenderMode::kHeadless) {
        renderer = std::make_unique<SdlWindowRenderer>();
        const std::string title = "go2cpp sdl_render " + stats.mode + " " + stats.phase;
        if (!renderer->Init(kWidth, kHeight, mode == RenderMode::kSdlVsync, title, &error)) {
            stats.note = "SKIP: " + error;
            return stats;
        }
        if (mode == RenderMode::kSdlVsync && renderer->Fallback()) {
            stats.note = "WARN: vsync 请求回退到 software 渲染器（vsync 不生效）";
        }
    }
    UdpLoop loop;
    if (!loop.Init(kWidth, kHeight, &error)) {
        stats.note = "SKIP: " + error;
        return stats;
    }
    TestPatternSource pattern(kWidth, kHeight);
    std::mt19937 rng(options.seed);
    std::uniform_int_distribution<std::int64_t> phase_dist(0, kPeriodNs);

    const std::int64_t start_ns = NowNs();
    std::int64_t next_frame_ns = start_ns;
    std::int64_t previous_end_ns = 0;
    for (int i = 0; i < options.frames; ++i) {
        if (phase == PhaseMode::kRandom) {
            SleepUntilNs(NowNs() + phase_dist(rng));
        } else if (phase == PhaseMode::kContinuous) {
            // 不节流：由 SDL_RenderPresent 的 vsync 背压决定节奏。
        } else {
            if (NowNs() - next_frame_ns > kPeriodNs) {
                next_frame_ns = NowNs();
            }
            SleepUntilNs(next_frame_ns);
            next_frame_ns += kPeriodNs;
        }

        VideoFrame pattern_frame;
        if (!pattern.Capture(&pattern_frame)) {
            stats.note = "SKIP: 测试图案生成失败";
            break;
        }
        double draw_ms = 0.0;
        if (!source.Draw(pattern_frame, &draw_ms, &error)) {
            stats.note = "SKIP: " + error;
            break;
        }
        VideoFrame capture;
        const std::int64_t capture_begin_ns = NowNs();
        double capture_ms = 0.0;
        if (!source.Capture(&capture, &capture_ms, &error)) {
            stats.note = "SKIP: " + error;
            break;
        }
        capture.frame_id = static_cast<std::uint32_t>(i + 1);
        capture.capture_ns = capture_begin_ns;

        const bool force_key = (i % kKeyInterval) == 0;
        double encode_ms = 0.0;
        double fragment_ms = 0.0;
        double sendto_ms = 0.0;
        std::size_t fragments = 0;
        std::uint64_t bytes = 0;
        if (!loop.EncodeAndSend(capture, force_key, &encode_ms, &fragment_ms, &sendto_ms,
                                &fragments, &bytes, &error)) {
            stats.note = "SKIP: " + error;
            break;
        }
        stats.bytes_sent += bytes;
        stats.fragments += fragments;

        double recv_ms = 0.0;
        if (!loop.ReceiveFrame(capture.frame_id, kRecvTimeoutMs, &recv_ms, &error)) {
            stats.note = "SKIP: " + error;
            break;
        }
        double decode_ms = 0.0;
        VideoFrame decoded;
        if (!loop.Decode(&decode_ms, &decoded, &error)) {
            stats.note = "SKIP: " + error;
            break;
        }
        stats.decoded_checksums.push_back(decoded.Checksum());

        std::int64_t end_ns = 0;
        if (mode == RenderMode::kHeadless) {
            const std::int64_t render_begin = NowNs();
            const std::uint64_t checksum = decoded.Checksum();
            end_ns = NowNs();
            (void)checksum;
            stats.convert.Add(0.0);
            stats.upload.Add(0.0);
            stats.copy.Add(0.0);
            stats.present.Add(0.0);
            stats.e2e_pre_present.Add(static_cast<double>(render_begin - capture_begin_ns) / 1e6);
        } else {
            FrameRenderTimes times;
            if (!renderer->Render(decoded, &times, &error)) {
                stats.note = "SKIP: " + error;
                break;
            }
            end_ns = times.end_ns;
            stats.convert.Add(times.convert_ms);
            stats.upload.Add(times.upload_ms);
            stats.copy.Add(times.copy_ms);
            stats.present.Add(times.present_ms);
            stats.e2e_pre_present.Add(
                static_cast<double>(times.present_start_ns - capture_begin_ns) / 1e6);
        }
        stats.draw.Add(draw_ms);
        stats.capture.Add(capture_ms);
        stats.encode.Add(encode_ms);
        stats.fragment.Add(fragment_ms);
        stats.sendto.Add(sendto_ms);
        stats.recv.Add(recv_ms);
        stats.decode.Add(decode_ms);
        stats.e2e.Add(static_cast<double>(end_ns - capture_begin_ns) / 1e6);
        if (previous_end_ns != 0) {
            stats.interval.Add(static_cast<double>(end_ns - previous_end_ns) / 1e6);
        }
        previous_end_ns = end_ns;
        ++stats.ok_frames;
    }
    const std::int64_t end_ns = NowNs();
    stats.frames = options.frames;
    stats.wall_ms = static_cast<double>(end_ns - start_ns) / 1e6;
    stats.fps = stats.wall_ms > 0.0 ? static_cast<double>(stats.ok_frames) * 1000.0 / stats.wall_ms
                                    : 0.0;
    stats.Finalize();
    if (renderer != nullptr) {
        stats.note += stats.note.empty() ? "" : " | ";
        stats.note += std::string("sdl_driver=") + renderer->DriverName() +
                      (renderer->VsycRequested() ? " vsync=on" : " vsync=off");
    }
    return stats;
}

// ---------------------------------------------------------------------------
// 纯 present 基准：量实际 swap 周期（无媒体链路）
// ---------------------------------------------------------------------------

SceneStats RunPresentBench(int frames, bool random_phase, const RunOptions& options) {
    SceneStats stats;
    stats.name = random_phase ? "present-phase" : "present-only";
    stats.mode = "sdl-vsync";
    stats.phase = random_phase ? "random" : "continuous";
    std::string error;
    SdlWindowRenderer renderer;
    if (!renderer.Init(kWidth, kHeight, true, "go2cpp sdl_render " + stats.name, &error)) {
        stats.note = "SKIP: " + error;
        return stats;
    }
    VideoFrame frame;
    frame.Reset(kWidth, kHeight, PixelFormat::kBGRA);
    TestPatternSource pattern(kWidth, kHeight);
    VideoFrame pattern_frame;
    pattern.Capture(&pattern_frame);
    frame = pattern_frame;

    std::mt19937 rng(options.seed ^ (random_phase ? 0x5bd1e995U : 0U));
    std::uniform_int_distribution<std::int64_t> phase_dist(0, kPeriodNs);

    const std::int64_t start_ns = NowNs();
    std::int64_t previous_end_ns = 0;
    for (int i = 0; i < frames; ++i) {
        if (random_phase) {
            SleepUntilNs(NowNs() + phase_dist(rng));
        }
        // 每帧轻微改一个像素，确保上传的是新内容（与真实渲染一致）。
        frame.data[0] = static_cast<std::uint8_t>(i);
        FrameRenderTimes times;
        if (!renderer.Render(frame, &times, &error)) {
            stats.note = "SKIP: " + error;
            break;
        }
        stats.convert.Add(times.convert_ms);
        stats.upload.Add(times.upload_ms);
        stats.copy.Add(times.copy_ms);
        stats.present.Add(times.present_ms);
        stats.e2e.Add(static_cast<double>(times.end_ns - times.present_start_ns) / 1e6 +
                      times.upload_ms + times.copy_ms);
        if (previous_end_ns != 0) {
            stats.interval.Add(static_cast<double>(times.end_ns - previous_end_ns) / 1e6);
        }
        previous_end_ns = times.end_ns;
        ++stats.ok_frames;
    }
    const std::int64_t end_ns = NowNs();
    stats.frames = frames;
    stats.wall_ms = static_cast<double>(end_ns - start_ns) / 1e6;
    stats.fps = stats.wall_ms > 0.0 ? static_cast<double>(stats.ok_frames) * 1000.0 / stats.wall_ms
                                    : 0.0;
    stats.Finalize();
    stats.note = std::string("sdl_driver=") + renderer.DriverName() +
                 (random_phase ? " vsync=on（随机相位提交）" : " vsync=on（连续提交，背压最坏）");
    return stats;
}

// ---------------------------------------------------------------------------
// 像素验证：SDL 回读 vs I420→BGRA 参考
// ---------------------------------------------------------------------------

struct ValidationResult {
    bool ok = false;
    bool x11_grab_ok = false;
    std::uint64_t diff_bytes = 0;
    std::uint64_t total_bytes = 0;
    std::string capture_sha;
    std::string reference_sha;
    std::string readback_sha;
    std::string note;
};

ValidationResult ValidateSdlPixels(const RunOptions& options) {
    ValidationResult result;
    std::string error;
    X11PatternWindow source;
    if (!source.Init(kWidth, kHeight, &error)) {
        result.note = "SKIP: " + error;
        return result;
    }
    SdlWindowRenderer renderer;
    if (!renderer.Init(kWidth, kHeight, true, "go2cpp sdl_render validation", &error)) {
        result.note = "SKIP: " + error;
        return result;
    }
    TestPatternSource pattern(kWidth, kHeight);
    VideoFrame pattern_frame;
    pattern.Capture(&pattern_frame);
    // 跳过几帧，让图案里的移动块/帧号有内容（frame 1 的帧号位图接近全 0）。
    for (int i = 0; i < 7; ++i) {
        pattern.Capture(&pattern_frame);
    }
    if (!source.Draw(pattern_frame, nullptr, &error)) {
        result.note = "SKIP: " + error;
        return result;
    }
    VideoFrame capture;
    if (!source.Capture(&capture, nullptr, &error)) {
        result.note = "SKIP: " + error;
        return result;
    }
    capture.frame_id = 8;
    capture.capture_ns = NowNs();
    result.capture_sha = FrameSha256(capture);

    const VideoFrame i420 = ToI420Frame(capture);
    VideoFrame reference;
    reference.Reset(kWidth, kHeight, PixelFormat::kBGRA);
    I420ToBgra(i420.data.data(), i420.width, i420.height, reference.data.data(), reference.stride);
    result.reference_sha = FrameSha256(reference);

    FrameRenderTimes times;
    if (!renderer.Upload(i420, &times, &error)) {
        result.note = "SKIP: " + error;
        return result;
    }
    VideoFrame readback;
    if (!renderer.ReadBack(&readback, &error)) {
        result.note = "SKIP: " + error;
        return result;
    }
    if (!renderer.Present(&times, &error)) {
        result.note = "SKIP: " + error;
        return result;
    }
    result.readback_sha = FrameSha256(readback);

    result.total_bytes = std::min(reference.data.size(), readback.data.size());
    for (std::size_t i = 0; i < result.total_bytes; ++i) {
        if (reference.data[i] != readback.data[i]) {
            ++result.diff_bytes;
        }
    }
    // 可选：抓 SDL 窗口的 X11 像素（GL 直绘窗口在 X pixmap 里可能是黑的，仅作记录）。
    const Window xid = renderer.xid();
    if (xid != 0 && source.display() != nullptr) {
        XImage* image = XGetImage(source.display(), xid, 0, 0, static_cast<unsigned>(kWidth),
                                  static_cast<unsigned>(kHeight), AllPlanes, ZPixmap);
        if (image != nullptr) {
            result.x11_grab_ok = true;
            XDestroyImage(image);
        }
    }

    // 落盘证据：采集 / I420 参考 / SDL 回读。
    WritePpmFrame(options.artifacts + "/sdl_validation_capture.ppm", capture);
    WritePpmFrame(options.artifacts + "/sdl_validation_reference.ppm", reference);
    WritePpmFrame(options.artifacts + "/sdl_validation_readback.ppm", readback);

    result.ok = result.total_bytes > 0 && result.diff_bytes == 0;
    result.note = "readPixels vs I420→BGRA: " + std::to_string(result.diff_bytes) + "/" +
                  std::to_string(result.total_bytes) + " 字节不同";
    return result;
}

// ---------------------------------------------------------------------------
// 报告
// ---------------------------------------------------------------------------

std::string Format(double value, int precision = 3) {
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "%.*f", precision, value);
    return std::string(buffer);
}

void WriteSceneTable(std::FILE* file, const std::vector<SceneStats>& scenes) {
    std::fprintf(file,
                 "| 场景 | 帧 | e2e p50 | e2e p90 | e2e p99 | e2e max | fps | 帧间隔 p50 |\n"
                 "|---|---:|---:|---:|---:|---:|---:|---:|\n");
    for (const SceneStats& s : scenes) {
        if (s.e2e.Empty()) {
            std::fprintf(file, "| %s | %d | SKIP | | | | | |\n", s.name.c_str(), s.ok_frames);
            continue;
        }
        std::fprintf(file, "| %s | %d | %s | %s | %s | %s | %s | %s |\n", s.name.c_str(),
                     s.ok_frames, Format(s.e2e.P(0.50)).c_str(), Format(s.e2e.P(0.90)).c_str(),
                     Format(s.e2e.P(0.99)).c_str(), Format(s.e2e.Max()).c_str(),
                     Format(s.fps, 2).c_str(), Format(s.interval.P(0.50)).c_str());
    }
}

void WriteStageTable(std::FILE* file, const std::vector<SceneStats>& scenes) {
    std::fprintf(file,
                 "| 场景 | 采集 | 编码 | 分片 | sendto | 收+重组 | 解码 | I420→BGRA | 上传 | "
                 "Clear+Copy | Present |\n"
                 "|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|\n");
    for (const SceneStats& s : scenes) {
        if (s.e2e.Empty()) {
            continue;
        }
        std::fprintf(file, "| %s | %s | %s | %s | %s | %s | %s | %s | %s | %s | %s |\n",
                     s.name.c_str(), Format(s.capture.P(0.50)).c_str(),
                     Format(s.encode.P(0.50)).c_str(), Format(s.fragment.P(0.50)).c_str(),
                     Format(s.sendto.P(0.50)).c_str(), Format(s.recv.P(0.50)).c_str(),
                     Format(s.decode.P(0.50)).c_str(), Format(s.convert.P(0.50)).c_str(),
                     Format(s.upload.P(0.50)).c_str(), Format(s.copy.P(0.50)).c_str(),
                     Format(s.present.P(0.50)).c_str());
    }
}

void WriteDataReport(const std::vector<SceneStats>& scenes, const ValidationResult& validation,
                     const std::string& path) {
    std::FILE* file = std::fopen(path.c_str(), "w");
    if (file == nullptr) {
        std::fprintf(stderr, "[sdl_render] 无法写 %s\n", path.c_str());
        return;
    }
    std::fprintf(file, "# SDL 真实渲染延迟数据（由 sdl_render_bench 生成）\n\n");
    std::fprintf(file, "- 画面：%dx%d，delta-I420，VideoChannel 1200B 分片，UDP 127.0.0.1 回环\n",
                 kWidth, kHeight);
    std::fprintf(file, "- 口径：capture_ns（XGetImage 调用前）→ SDL_RenderPresent 返回\n");
    std::fprintf(file, "- 时间单位：ms；除特别注明外为 p50\n\n");

    std::fprintf(file, "## 1. 端到端延迟\n\n");
    WriteSceneTable(file, scenes);
    std::fprintf(file, "\n## 2. 逐段耗时（p50）\n\n");
    WriteStageTable(file, scenes);

    std::fprintf(file, "\n## 3. vsync 等待与离下限\n\n");
    std::fprintf(file,
                 "| 场景 | Present p50 | Present p99 | e2e p50 | present/e2e | e2e-16.667 | "
                 "e2e-实测周期 |\n"
                 "|---|---:|---:|---:|---:|---:|---:|\n");
    double measured_period = 0.0;
    for (const SceneStats& s : scenes) {
        if (s.name == "present-only" && !s.interval.Empty()) {
            measured_period = s.interval.P(0.50);
        }
    }
    for (const SceneStats& s : scenes) {
        if (s.e2e.Empty()) {
            continue;
        }
        const double present_p50 = s.present.P(0.50);
        const double e2e_p50 = s.e2e.P(0.50);
        const double ratio = e2e_p50 > 0.0 ? present_p50 / e2e_p50 : 0.0;
        std::fprintf(file, "| %s | %s | %s | %s | %s%% | %s | %s |\n", s.name.c_str(),
                     Format(present_p50).c_str(), Format(s.present.P(0.99)).c_str(),
                     Format(e2e_p50).c_str(), Format(ratio * 100.0, 1).c_str(),
                     Format(e2e_p50 - 16.667).c_str(),
                     Format(measured_period > 0.0 ? e2e_p50 - measured_period : 0.0).c_str());
    }
    std::fprintf(file, "\n实测 swap 周期（present-only 帧间隔 p50）= %s ms\n",
                 Format(measured_period).c_str());

    std::fprintf(file, "\n## 4. 场景备注\n\n");
    for (const SceneStats& s : scenes) {
        std::fprintf(file, "- `%s`：%s\n", s.name.c_str(),
                     s.note.empty() ? "OK" : s.note.c_str());
    }

    std::fprintf(file, "\n## 5. 逐帧校验（跨场景一致性）\n\n");
    std::fprintf(file, "同一 frame_id 的 I420 校验和在 headless 与 SDL 场景应完全相同。\n\n");
    std::fprintf(file, "| 场景 A | 场景 B | 比较帧数 | 相同 |\n|---|---|---:|---:|\n");
    for (std::size_t i = 0; i < scenes.size(); ++i) {
        for (std::size_t j = i + 1; j < scenes.size(); ++j) {
            const std::size_t count =
                std::min(scenes[i].decoded_checksums.size(), scenes[j].decoded_checksums.size());
            if (count == 0) {
                continue;
            }
            std::size_t same = 0;
            for (std::size_t k = 0; k < count; ++k) {
                if (scenes[i].decoded_checksums[k] == scenes[j].decoded_checksums[k]) {
                    ++same;
                }
            }
            std::fprintf(file, "| %s | %s | %zu | %zu |\n", scenes[i].name.c_str(),
                         scenes[j].name.c_str(), count, same);
        }
    }

    std::fprintf(file, "\n## 6. 像素验证（SDL 回读）\n\n");
    std::fprintf(file, "- 采集帧 SHA-256：`%s`\n", validation.capture_sha.c_str());
    std::fprintf(file, "- I420→BGRA 参考 SHA-256：`%s`\n", validation.reference_sha.c_str());
    std::fprintf(file, "- SDL readPixels SHA-256：`%s`\n", validation.readback_sha.c_str());
    std::fprintf(file, "- memcmp：%llu/%llu 字节不同\n",
                 static_cast<unsigned long long>(validation.diff_bytes),
                 static_cast<unsigned long long>(validation.total_bytes));
    std::fprintf(file, "- 结果：%s（%s）\n", validation.ok ? "PASS" : "FAIL",
                 validation.note.c_str());
    std::fprintf(file, "- X11 抓取 SDL 窗口：%s\n",
                 validation.x11_grab_ok ? "成功（GL 直绘内容可能不在 X pixmap 中）" : "失败/不支持");

    std::fclose(file);
}

// ---------------------------------------------------------------------------
// 参数解析
// ---------------------------------------------------------------------------

int ParseArgs(int argc, char** argv, RunOptions* options, bool* quick) {
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--quick") {
            *quick = true;
        } else if (arg == "--frames" && i + 1 < argc) {
            options->frames = std::atoi(argv[++i]);
        } else if (arg == "--artifacts" && i + 1 < argc) {
            options->artifacts = argv[++i];
        } else if (arg == "--help" || arg == "-h") {
            std::printf("用法: %s [--quick] [--frames N] [--artifacts DIR]\n", argv[0]);
            return 1;
        } else {
            std::fprintf(stderr, "未知参数: %s\n", arg.c_str());
            return 1;
        }
    }
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    RunOptions options;
    bool quick = false;
    if (ParseArgs(argc, argv, &options, &quick) != 0) {
        return 2;
    }
    if (quick) {
        options.frames = 120;
    }
    if (!options.artifacts.empty()) {
        std::error_code directory_error;
        std::filesystem::create_directories(options.artifacts, directory_error);
    }
    const bool sdl_ok = SDL_Init(SDL_INIT_VIDEO) == 0;
    if (!sdl_ok) {
        std::fprintf(stderr, "[sdl_render] SDL_Init 失败: %s（只跑 headless 场景）\n",
                     SDL_GetError());
    } else {
        std::printf("[sdl_render] SDL video driver = %s\n", SDL_GetCurrentVideoDriver());
    }

    std::vector<SceneStats> scenes;
    scenes.push_back(RunSyncScene("headless-fixed", RenderMode::kHeadless, PhaseMode::kFixed, options));
    if (sdl_ok) {
        scenes.push_back(
            RunSyncScene("sdl-novsync-fixed", RenderMode::kSdlNoVsync, PhaseMode::kFixed, options));
        scenes.push_back(
            RunSyncScene("sdl-vsync-throttled", RenderMode::kSdlVsync, PhaseMode::kFixed, options));
        scenes.push_back(RunSyncScene("sdl-vsync-continuous", RenderMode::kSdlVsync,
                                      PhaseMode::kContinuous, options));
        scenes.push_back(
            RunSyncScene("sdl-vsync-random", RenderMode::kSdlVsync, PhaseMode::kRandom, options));
        scenes.push_back(RunPresentBench(options.frames, false, options));
        scenes.push_back(RunPresentBench(options.frames, true, options));
    }

    ValidationResult validation;
    if (sdl_ok) {
        validation = ValidateSdlPixels(options);
    }

    for (const SceneStats& s : scenes) {
        if (s.e2e.Empty()) {
            std::printf("[scene] %-18s SKIP %s\n", s.name.c_str(), s.note.c_str());
            continue;
        }
        std::printf(
            "[scene] %-18s e2e p50=%7.3f p99=%7.3f max=%7.3f ms | present p50=%7.3f ms | "
            "fps=%6.2f | %s\n",
            s.name.c_str(), s.e2e.P(0.50), s.e2e.P(0.99), s.e2e.Max(), s.present.P(0.50), s.fps,
            s.note.c_str());
    }
    if (sdl_ok) {
        std::printf("[validate] %s | %s\n", validation.ok ? "PASS" : "FAIL",
                    validation.note.c_str());
    }

    WriteDataReport(scenes, validation, "/tmp/sdl_render_data.md");
    std::printf("[sdl_render] data = /tmp/sdl_render_data.md\n");

    bool all_ok = sdl_ok;
    for (const SceneStats& s : scenes) {
        all_ok = all_ok && s.e2e.Empty() == false;
    }
    all_ok = all_ok && validation.ok;
    if (SDL_WasInit(SDL_INIT_VIDEO) != 0) {
        SDL_Quit();
    }
    return all_ok ? 0 : 1;
}
