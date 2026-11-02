/**
 * @file media_capture_render_bench.cpp
 * @brief 采集（X11 XShm）与渲染（SDL2）的架构/汇编级基准 + 正确性闭环。
 *
 * 与 `sdl_render_bench.cpp` 的分工：
 *   - `sdl_render_bench` 量的是「采集→编码→1200B 分片→UDP→重组→解码→渲染」的
 *     端到端延迟（含 vsync 相位），本文件把范围收窄到**采集与渲染本身**：
 *     ① XShm 零拷贝采集 vs owned 拷贝采集（同一窗口、同一帧数）；
 *     ② 合成源采集（无 X 依赖的基线，1e5 帧）；
 *     ③ SDL 纹理直传渲染（BGRA 直接上传，无中间帧、无 PPM）与回读校验；
 *     ④ 1e5 帧压测（fps / p50 / p90 / p99 / max / RSS 增长）；
 *     ⑤ 「采集 → 直传 → 回读」逐字节 0 差异。
 *
 * 输出：stdout 摘要 + `/tmp/media_capture_render_data.md` 数据表。
 * 不写 PPM/PNG：渲染路径上不存在文件 IO，验证只做内存回读。
 *
 * 用法：
 *   media_capture_render_bench [--frames N] [--render-frames N] [--quick]
 *                              [--out PATH]
 *
 * @author go2cpp contributors
 * @date 2026-11-02
 * @copyright MIT License
 */

#include <SDL.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include "go2cpp/media/capture.hpp"
#include "go2cpp/media/frame.hpp"
#include "go2cpp/media/pixel.hpp"
#include "go2cpp/media/render.hpp"

namespace {

using namespace go2cpp::media;

// ---------------------------------------------------------------------------
// 小工具
// ---------------------------------------------------------------------------

std::int64_t NowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

/// @brief 进程常驻内存（KB），读 /proc/self/statm。
long RssKb() {
    std::FILE* file = std::fopen("/proc/self/statm", "r");
    if (file == nullptr) {
        return -1;
    }
    long size = 0;
    long resident = 0;
    if (std::fscanf(file, "%ld %ld", &size, &resident) != 2) {
        resident = -1;
    }
    std::fclose(file);
    if (resident < 0) {
        return -1;
    }
    return resident * static_cast<long>(sysconf(_SC_PAGESIZE) / 1024);
}

class Hist {
   public:
    void Reserve(std::size_t count) {
        m_values.reserve(count);
    }
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
    std::size_t size() const noexcept {
        return m_values.size();
    }
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
        return m_values[static_cast<std::size_t>(pos)];
    }
    double mean() const {
        if (m_values.empty()) {
            return 0.0;
        }
        double sum = 0.0;
        for (const double value : m_values) {
            sum += value;
        }
        return sum / static_cast<double>(m_values.size());
    }

   private:
    std::vector<double> m_values;
    bool m_sorted = false;
};

// ---------------------------------------------------------------------------
// X11 图案窗口：XPutImage 画确定性图案，供 XShm 采集
// ---------------------------------------------------------------------------

class X11PatternWindow {
   public:
    ~X11PatternWindow() {
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
        m_window = XCreateSimpleWindow(m_display, RootWindow(m_display, screen), 24, 24,
                                       static_cast<unsigned>(width), static_cast<unsigned>(height),
                                       0, BlackPixel(m_display, screen),
                                       BlackPixel(m_display, screen));
        if (m_window == 0) {
            return false;
        }
        XStoreName(m_display, m_window, "go2cpp media capture/render bench");
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

    /// @brief 等窗口内容在服务器端稳定（重绘/合成期间 XShmGetImage 可能瞬态失败）。
    void Settle() {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    Window window() const noexcept {
        return m_window;
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

// ---------------------------------------------------------------------------
// 结果结构
// ---------------------------------------------------------------------------

struct BenchResult {
    std::string name;
    int frames = 0;
    double total_ms = 0.0;
    double fps = 0.0;
    double p50_ms = 0.0;
    double p90_ms = 0.0;
    double p99_ms = 0.0;
    double max_ms = 0.0;
    long rss_before_kb = 0;
    long rss_after_kb = 0;
    std::uint64_t mismatches = 0;
    std::string note;
};

std::string Format(double value, int precision = 3) {
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "%.*f", precision, value);
    return buffer;
}

// ---------------------------------------------------------------------------
// 采集基准
// ---------------------------------------------------------------------------

/// @brief 采集 N 帧（source）。
/// @param expect_static true 时要求每帧 checksum 与首帧一致（静止窗口）；
///        合成图案/root 画面会变，传 false 只统计采集失败。
BenchResult BenchCapture(const std::string& name, VideoSource* source, int frames,
                         bool expect_static, const std::string& note) {
    BenchResult result;
    result.name = name;
    result.frames = frames;
    result.note = note;
    if (source == nullptr || frames <= 0) {
        result.note += " (skipped)";
        return result;
    }
    Hist hist;
    hist.Reserve(static_cast<std::size_t>(frames));
    std::uint64_t first_checksum = 0;
    std::uint64_t mismatches = 0;
    std::uint64_t borrowed_frames = 0;
    result.rss_before_kb = RssKb();
    const std::int64_t begin = NowNs();
    for (int i = 0; i < frames; ++i) {
        VideoFrame frame;
        const std::int64_t frame_begin = NowNs();
        if (!source->Capture(&frame)) {
            ++mismatches;
            continue;
        }
        hist.Add(static_cast<double>(NowNs() - frame_begin) / 1e6);
        if (source->last_capture_borrowed()) {
            ++borrowed_frames;
        }
        if (!expect_static) {
            continue;
        }
        const std::uint64_t checksum = frame.Checksum();
        if (i == 0) {
            first_checksum = checksum;
        } else if (checksum != first_checksum) {
            ++mismatches;
        }
    }
    const std::int64_t end = NowNs();
    result.total_ms = static_cast<double>(end - begin) / 1e6;
    result.rss_after_kb = RssKb();
    hist.Finalize();
    result.fps = result.total_ms > 0.0 ? static_cast<double>(frames) * 1000.0 / result.total_ms
                                       : 0.0;
    result.p50_ms = hist.P(0.50);
    result.p90_ms = hist.P(0.90);
    result.p99_ms = hist.P(0.99);
    result.max_ms = hist.P(1.0);
    result.mismatches = mismatches;
    result.note += " borrowed=" + std::to_string(borrowed_frames) + "/" +
                   std::to_string(frames);
    return result;
}

// ---------------------------------------------------------------------------
// 渲染基准（headless / dummy 驱动）
// ---------------------------------------------------------------------------

BenchResult BenchRender(const std::string& name, VideoSource* source, FrameRenderer* renderer,
                        int frames, std::uint64_t* zero_copy_presents,
                        std::uint64_t* readback_diffs, const std::string& note) {
    BenchResult result;
    result.name = name;
    result.frames = frames;
    result.note = note;
    if (source == nullptr || renderer == nullptr || frames <= 0) {
        result.note += " (skipped)";
        return result;
    }
    Hist hist;
    hist.Reserve(static_cast<std::size_t>(frames));
    std::uint64_t mismatches = 0;
    result.rss_before_kb = RssKb();
    const std::int64_t begin = NowNs();
    for (int i = 0; i < frames; ++i) {
        VideoFrame frame;
        if (!source->Capture(&frame)) {
            ++mismatches;
            continue;
        }
        const std::int64_t frame_begin = NowNs();
        if (!renderer->Present(frame)) {
            ++mismatches;
            continue;
        }
        hist.Add(static_cast<double>(NowNs() - frame_begin) / 1e6);
        if (renderer->last_present_zero_copy() && zero_copy_presents != nullptr) {
            ++(*zero_copy_presents);
        }
        // 每 1000 帧回读一次，逐字节比对采集帧（静止窗口时内容不变）。
        if (readback_diffs != nullptr && renderer->visible() && (i % 1000) == 0) {
            VideoFrame back;
            if (renderer->Readback(&back) && back.width == frame.width &&
                back.height == frame.height) {
                const std::size_t bytes = std::min(back.PlaneBytes(), frame.PlaneBytes());
                for (std::size_t j = 0; j < bytes; ++j) {
                    if (back.data[j] != frame.Bytes()[j]) {
                        ++(*readback_diffs);
                    }
                }
            }
        }
    }
    const std::int64_t end = NowNs();
    result.total_ms = static_cast<double>(end - begin) / 1e6;
    result.rss_after_kb = RssKb();
    hist.Finalize();
    result.fps = result.total_ms > 0.0 ? static_cast<double>(frames) * 1000.0 / result.total_ms
                                       : 0.0;
    result.p50_ms = hist.P(0.50);
    result.p90_ms = hist.P(0.90);
    result.p99_ms = hist.P(0.99);
    result.max_ms = hist.P(1.0);
    result.mismatches = mismatches;
    return result;
}

/// @brief 渲染对照：每帧先深拷贝一次 BGRA（等价旧实现 `m_bgra = frame`），再上传。
/// @details 用来量化「纹理直传」相对「中间帧拷贝」的收益；Present 内部仍是同一路径。
BenchResult BenchRenderCopy(const std::string& name, VideoSource* source,
                            FrameRenderer* renderer, int frames, const std::string& note) {
    BenchResult result;
    result.name = name;
    result.frames = frames;
    result.note = note;
    if (source == nullptr || renderer == nullptr || frames <= 0) {
        result.note += " (skipped)";
        return result;
    }
    Hist hist;
    hist.Reserve(static_cast<std::size_t>(frames));
    std::uint64_t mismatches = 0;
    result.rss_before_kb = RssKb();
    const std::int64_t begin = NowNs();
    for (int i = 0; i < frames; ++i) {
        VideoFrame frame;
        if (!source->Capture(&frame)) {
            ++mismatches;
            continue;
        }
        const std::int64_t frame_begin = NowNs();
        VideoFrame copy = frame;  // 旧实现的整帧拷贝（vector 深拷贝）
        if (!renderer->Present(copy)) {
            ++mismatches;
            continue;
        }
        hist.Add(static_cast<double>(NowNs() - frame_begin) / 1e6);
    }
    const std::int64_t end = NowNs();
    result.total_ms = static_cast<double>(end - begin) / 1e6;
    result.rss_after_kb = RssKb();
    hist.Finalize();
    result.fps = result.total_ms > 0.0 ? static_cast<double>(frames) * 1000.0 / result.total_ms
                                       : 0.0;
    result.p50_ms = hist.P(0.50);
    result.p90_ms = hist.P(0.90);
    result.p99_ms = hist.P(0.99);
    result.max_ms = hist.P(1.0);
    result.mismatches = mismatches;
    return result;
}

// ---------------------------------------------------------------------------
// 输出
// ---------------------------------------------------------------------------

void PrintResult(const BenchResult& result) {
    std::printf("[%s] frames=%d fps=%.1f p50=%.3fms p90=%.3fms p99=%.3fms max=%.3fms "
                "rss=%ld->%ldKB mismatches=%llu %s\n",
                result.name.c_str(), result.frames, result.fps, result.p50_ms, result.p90_ms,
                result.p99_ms, result.max_ms, result.rss_before_kb, result.rss_after_kb,
                static_cast<unsigned long long>(result.mismatches), result.note.c_str());
}

std::string MarkdownRow(const BenchResult& result) {
    return "| " + result.name + " | " + std::to_string(result.frames) + " | " +
           Format(result.fps, 1) + " | " + Format(result.p50_ms, 3) + " | " +
           Format(result.p90_ms, 3) + " | " + Format(result.p99_ms, 3) + " | " +
           Format(result.max_ms, 3) + " | " + std::to_string(result.rss_before_kb) + "->" +
           std::to_string(result.rss_after_kb) + " | " + std::to_string(result.mismatches) +
           " | " + result.note + " |\n";
}

}  // namespace

int main(int argc, char** argv) {
    int frames = 100000;
    int render_frames = 0;
    bool quick = false;
    std::string out_path = "/tmp/media_capture_render_data.md";
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--frames" && i + 1 < argc) {
            frames = std::atoi(argv[++i]);
        } else if (arg == "--render-frames" && i + 1 < argc) {
            render_frames = std::atoi(argv[++i]);
        } else if (arg == "--out" && i + 1 < argc) {
            out_path = argv[++i];
        } else if (arg == "--quick") {
            quick = true;
        } else {
            std::printf("usage: %s [--frames N] [--render-frames N] [--quick] [--out PATH]\n",
                        argv[0]);
            return 2;
        }
    }
    if (quick) {
        if (frames > 5000) {
            frames = 5000;
        }
        if (render_frames == 0) {
            render_frames = 2000;
        }
    }
    if (render_frames <= 0) {
        render_frames = frames;
    }

    std::printf("== media capture/render bench ==\n");
    std::printf("control: frames=%d render_frames=%d display=%s\n", frames, render_frames,
                std::getenv("DISPLAY") != nullptr ? std::getenv("DISPLAY") : "(unset)");

    std::vector<BenchResult> results;

    // 1) 合成源采集（无 X 依赖基线）。
    {
        SyntheticCapture synthetic(320, 240, SyntheticPattern::kScroll);
        results.push_back(BenchCapture("capture-synthetic", &synthetic, frames, false,
                                       "320x240 synthetic scroll (no X)"));
    }

    // 2) X11 窗口：零拷贝 vs owned。
    X11PatternWindow window;
    bool window_ok = window.Init(320, 240);
    X11CaptureConfig config;
    config.width = 320;
    config.height = 240;
    config.use_shm = true;
    config.target_window = window_ok ? static_cast<unsigned long>(window.window()) : 0;
    std::string error;
    if (window_ok) {
        SyntheticCapture pattern(320, 240, SyntheticPattern::kScroll);
        VideoFrame pattern_frame;
        pattern.Capture(&pattern_frame);
        window.Draw(pattern_frame);
        window.Settle();
    }

    if (window_ok) {
        config.zero_copy = true;
        auto zero = CreateX11Capture(config, &error);
        if (zero != nullptr) {
            results.push_back(BenchCapture("capture-xshm-zerocopy", zero.get(), frames, true,
                                           "XShm borrowed view (no memcpy)"));
        } else {
            std::printf("[SKIP] zero-copy capture: %s\n", error.c_str());
        }
        config.zero_copy = false;
        auto owned = CreateX11Capture(config, &error);
        if (owned != nullptr) {
            results.push_back(BenchCapture("capture-xshm-owned", owned.get(), frames, true,
                                           "XShm + SSE2 row copy to owned frame"));
        } else {
            std::printf("[SKIP] owned capture: %s\n", error.c_str());
        }

        // 3) root 采集（与 FFmpeg x11grab 同区域，供脚本对照）。
        X11CaptureConfig root_config;
        root_config.width = 320;
        root_config.height = 240;
        root_config.zero_copy = true;
        auto root = CreateX11Capture(root_config, &error);
        if (root != nullptr) {
            results.push_back(BenchCapture("capture-xshm-root-zerocopy", root.get(),
                                           std::min(frames, 20000), false,
                                           "root :0.0+0,0 borrowed (ffmpeg-comparable)"));
        }
    } else {
        std::printf("[SKIP] X11 window unavailable (no DISPLAY or window creation failed)\n");
    }

    // 4) 渲染：BGRA 直传（dummy 驱动，避免 vsync 干扰吞吐）。
    ::setenv("SDL_VIDEODRIVER", "dummy", 1);
    std::uint64_t zero_copy_presents = 0;
    std::uint64_t readback_diffs = 0;
    if (SdlAvailable()) {
        SyntheticCapture synthetic(320, 240, SyntheticPattern::kScroll);
        auto renderer = CreateSdlRenderer(320, 240, false, &error);
        if (renderer != nullptr) {
            results.push_back(BenchRender("render-sdl-direct-dummy", &synthetic, renderer.get(),
                                          render_frames, &zero_copy_presents, &readback_diffs,
                                          "SDL_UpdateTexture(BGRA direct) dummy driver"));
            std::printf("[INFO] zero-copy presents=%llu readback_diff_bytes=%llu\n",
                        static_cast<unsigned long long>(zero_copy_presents),
                        static_cast<unsigned long long>(readback_diffs));
        } else {
            std::printf("[SKIP] SDL renderer: %s\n", error.c_str());
        }
    } else {
        std::printf("[SKIP] SDL2 unavailable\n");
    }

    // 4b) 渲染对照：每帧一次整帧深拷贝（等价旧实现），量化「直传」的收益。
    if (SdlAvailable()) {
        SyntheticCapture synthetic(320, 240, SyntheticPattern::kScroll);
        auto renderer = CreateSdlRenderer(320, 240, false, &error);
        if (renderer != nullptr) {
            results.push_back(BenchRenderCopy("render-sdl-copy-baseline", &synthetic,
                                              renderer.get(), render_frames,
                                              "one full-frame copy + SDL upload (old path)"));
        }
    }
    ::unsetenv("SDL_VIDEODRIVER");

    // 5) 真实窗口「采集 → 直传 → 回读」正确性（小帧数，避免 vsync/合成开销）。
    bool validation_ok = false;
    std::uint64_t validation_diff = 0;
    if (window_ok) {
        config.zero_copy = true;
        auto capture = CreateX11Capture(config, &error);
        auto renderer = CreateSdlRenderer(320, 240, false, &error);
        if (capture != nullptr && renderer != nullptr) {
            VideoFrame frame;
            if (capture->Capture(&frame) && renderer->Present(frame)) {
                VideoFrame back;
                if (renderer->Readback(&back) && back.width == frame.width &&
                    back.height == frame.height) {
                    const std::size_t bytes = std::min(back.PlaneBytes(), frame.PlaneBytes());
                    for (std::size_t i = 0; i < bytes; ++i) {
                        if (back.data[i] != frame.Bytes()[i]) {
                            ++validation_diff;
                        }
                    }
                    validation_ok = bytes > 0 && validation_diff == 0;
                }
            }
        }
    }
    std::printf("[INFO] capture->sdl->readback: ok=%d diff_bytes=%llu\n", validation_ok ? 1 : 0,
                static_cast<unsigned long long>(validation_diff));

    // 6) 写数据表。
    {
        std::ofstream out(out_path);
        if (out) {
            out << "# 媒体采集/渲染基准数据（media_capture_render_bench 生成）\n\n";
            out << "| 场景 | 帧数 | fps | p50 ms | p90 ms | p99 ms | max ms | RSS KB | 不符 | 备注 |\n";
            out << "|---|---|---|---|---|---|---|---|---|---|\n";
            for (const auto& result : results) {
                out << MarkdownRow(result);
            }
            out << "\n- 采集→SDL 直传→回读逐字节差异: " << validation_diff
                << " B（ok=" << (validation_ok ? 1 : 0) << "）\n";
            out << "- zero-copy presents: " << zero_copy_presents
                << "，回读抽样差异字节: " << readback_diffs << "\n";
            out << "- DISPLAY=" << (std::getenv("DISPLAY") != nullptr ? std::getenv("DISPLAY")
                                                                     : "(unset)")
                << "\n";
            std::printf("[INFO] data written to %s\n", out_path.c_str());
        } else {
            std::printf("[WARN] cannot write %s\n", out_path.c_str());
        }
    }
    for (const auto& result : results) {
        PrintResult(result);
    }
    return validation_ok || !window_ok ? 0 : 1;
}
