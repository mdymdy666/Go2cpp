/**
 * @file media_e2e_demo.cpp
 * @brief 媒体端到端「采集 → 编码 → 分片 → 传输 → 重组 → 解码 → 渲染」真实验证。
 * @details 每个场景都产出可肉眼检查的 PPM 截图（build_and_run.sh 再转 PNG）与数据：
 *
 *          1. x11-real        WSLg X11(XShm) 真实采集 → delta → UDP 回环（包级 jitter）→ 渲染
 *          2. raw-bgra-strict 合成彩色方块/帧号 → raw-bgra → 包级 jitter → BGRA 逐像素 SHA-256 相同
 *          3. delta-1000      合成 320x240 → delta → pipeline → 1000 帧逐帧 I420 逐字节一致
 *          4. weaknet-delta   ±50ms 抖动 + 5% 丢包 + 乱序，jitter 开/关对照
 *          5. weaknet-raw     ±50ms 抖动 + 5% 丢包，raw-i420（每帧独立）交付帧 100% 正确
 *          6. degradation     带宽/丢包采样驱动 full→reduced→low→minimal，尺寸/帧率真实变化
 *          7. latency         采集/编码/分片/收发/重组/解码/渲染分段耗时 + memcpy/syscall 下限
 *
 *          包级 jitter 的接线位置与文档 §2.1 一致：在 `VideoChannel` 重组**之前**
 *          按 Bento 包序号重排，避免「缺一片就丢整帧」与「帧乱序导致旧帧 stale」。
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
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "go2cpp/bento/net/protocol.hpp"
#include "go2cpp/media/capture.hpp"
#include "go2cpp/media/codec.hpp"
#include "go2cpp/media/degradation.hpp"
#include "go2cpp/media/frame.hpp"
#include "go2cpp/media/jitter_buffer.hpp"
#include "go2cpp/media/pipeline.hpp"
#include "go2cpp/media/pixel.hpp"
#include "go2cpp/media/render.hpp"
#include "go2cpp/media/stats.hpp"
#include "go2cpp/media/transport.hpp"

#include "e2e_common.hpp"

namespace {

using namespace go2cpp::media;
using namespace go2cpp::media::e2e;

// ---------------------------------------------------------------------------
// 场景结果
// ---------------------------------------------------------------------------

struct SceneResult {
    std::string name;
    std::string detail;
    int width = 0;
    int height = 0;
    std::uint64_t sent = 0;
    std::uint64_t delivered = 0;
    std::uint64_t correct = 0;
    std::uint64_t wrong = 0;
    std::uint64_t keyframes = 0;
    std::uint64_t keyframes_correct = 0;
    std::uint64_t bytes_sent = 0;
    std::uint64_t bytes_received = 0;
    std::uint64_t stale = 0;
    std::uint64_t incomplete = 0;
    std::uint64_t reordered = 0;
    std::uint64_t lost = 0;
    std::uint64_t duplicates = 0;
    std::uint64_t jitter_dropped_frames = 0;
    std::int64_t target_delay_ns = 0;
    std::int64_t jitter_ns = 0;
    double p50_ms = 0.0;
    double p99_ms = 0.0;
    double max_ms = 0.0;
    double mean_ms = 0.0;
    double fps = 0.0;
    double elapsed_ms = 0.0;
    bool ok = true;
    std::string fail_reason;
};

std::vector<SceneResult> g_results;
std::string g_artifacts;

void Fail(SceneResult* result, const std::string& reason) {
    result->ok = false;
    if (result->fail_reason.empty()) {
        result->fail_reason = reason;
    } else {
        result->fail_reason += "; " + reason;
    }
}

/// @brief 采集 BGRA → I420 参考帧（逐字节比较用）。
VideoFrame ToI420(const VideoFrame& bgra) {
    VideoFrame frame;
    frame.width = bgra.width;
    frame.height = bgra.height;
    frame.format = PixelFormat::kI420;
    frame.data.assign(I420FrameBytes(bgra.width, bgra.height), 0);
    BgraToI420(bgra.data.data(), bgra.width, bgra.height, bgra.EffectiveStride(),
               frame.data.data());
    return frame;
}

/// @brief 90kHz 时间戳（与 pipeline 的 jitter 口径一致）。
std::uint32_t To90k(std::int64_t capture_ns) {
    if (capture_ns <= 0) {
        return 0;
    }
    return static_cast<std::uint32_t>((capture_ns / 1000) * 9 / 100);
}

// ---------------------------------------------------------------------------
// 包级接收链路：JitterBuffer（VideoChannel 之前）+ 重组 + 解码 + 渲染
// ---------------------------------------------------------------------------

class PacketReceiver {
   public:
    struct Options {
        bool use_jitter = true;
        bool collect_latency = true;
        JitterBufferConfig jitter;
        int width = 320;
        int height = 240;
        CodecKind codec = CodecKind::kDeltaI420;
        PpmRenderer* renderer = nullptr;
        std::function<void(const VideoFrame&)> on_frame;
        /// @brief 当前时刻来源（模拟网络用虚拟时钟，真实链路用 steady_clock）。
        std::function<std::int64_t()> clock;
    };

    bool Init(const Options& options, std::string* error) {
        m_options = options;
        if (options.use_jitter) {
            JitterBufferConfig config = options.jitter;
            config.max_payload_bytes = std::max<std::size_t>(config.max_payload_bytes, 1600);
            config.startup_buffering = false;
            config.capacity = std::max<std::uint32_t>(config.capacity, 4096);
            m_jitter = std::make_unique<JitterBuffer>(config);
        }
        m_decoder = CreateDecoder(options.codec, options.width, options.height);
        if (!m_decoder) {
            if (error != nullptr) {
                *error = "decoder create failed";
            }
            return false;
        }
        m_channel = std::make_unique<go2cpp::bento::VideoChannel>(
            [](const std::uint8_t*, std::size_t) { return true; },
            [this](std::uint32_t, bool, const std::uint8_t* data, std::size_t bytes) {
                OnFrame(data, bytes);
            });
        return true;
    }

    /// @brief 收一个完整数据报（Bento 帧）。
    /// @param arrival_ns 到达时刻（真实或模拟）。
    /// @param now_ns 驱动 jitter 的当前时刻（与 arrival 同一时基）。
    void OnDatagram(const std::uint8_t* data, std::size_t bytes, std::int64_t arrival_ns,
                    std::int64_t now_ns) {
        m_bytes += bytes;
        go2cpp::bento::FrameView view;
        if (!go2cpp::bento::ParseFrame(data, bytes, &view) ||
            view.header.type != go2cpp::bento::FrameType::kVideo) {
            ++m_invalid;
            return;
        }
        if (m_jitter != nullptr) {
            const std::uint32_t timestamp = StampFor(view);
            m_jitter->Push(view.header.seq, timestamp, data, bytes, arrival_ns);
            Drain(now_ns);
            return;
        }
        m_channel->OnFrame(view.header, view.payload, view.payload_bytes);
        m_channel->OnTimer(static_cast<std::uint64_t>(now_ns / 1000000));
    }

    /// @brief 把 jitter 里已到期的包按序喂给重组层。
    void Drain(std::int64_t now_ns) {
        if (m_jitter == nullptr) {
            return;
        }
        for (std::uint32_t spins = 0; spins < 8192; ++spins) {
            JitterPacketView packet;
            const PopResult result = m_jitter->Pop(&packet, now_ns);
            if (result == PopResult::kOk || result == PopResult::kLateDelivered) {
                go2cpp::bento::FrameView view;
                if (go2cpp::bento::ParseFrame(packet.data, packet.bytes, &view)) {
                    m_channel->OnFrame(view.header, view.payload, view.payload_bytes);
                }
                continue;
            }
            if (result == PopResult::kGapSkipped) {
                continue;
            }
            break;
        }
        m_channel->OnTimer(static_cast<std::uint64_t>(now_ns / 1000000));
    }

    /// @brief 收尾：忽略等待窗口把 jitter 全部吐出（尾帧不滞留）。
    void Flush(std::int64_t now_ns) {
        if (m_jitter == nullptr) {
            return;
        }
        for (std::uint32_t spins = 0; spins < 65536; ++spins) {
            JitterPacketView packet;
            const PopResult result = m_jitter->PopFlush(&packet, now_ns);
            if (result == PopResult::kOk || result == PopResult::kLateDelivered) {
                go2cpp::bento::FrameView view;
                if (go2cpp::bento::ParseFrame(packet.data, packet.bytes, &view)) {
                    m_channel->OnFrame(view.header, view.payload, view.payload_bytes);
                }
                continue;
            }
            if (result == PopResult::kGapSkipped) {
                continue;
            }
            break;
        }
        m_channel->OnTimer(static_cast<std::uint64_t>(now_ns / 1000000) + 5000);
    }

    std::uint64_t decoded() const noexcept {
        return m_decoded;
    }
    std::uint64_t dropped() const noexcept {
        return m_dropped;
    }
    std::uint64_t invalid() const noexcept {
        return m_invalid;
    }
    std::uint64_t bytes_received() const noexcept {
        return m_bytes;
    }
    const LatencyRecorder& latency() const noexcept {
        return m_latency;
    }
    const VideoFrame& last_frame() const noexcept {
        return m_last_frame;
    }
    JitterStats jitter_stats() const noexcept {
        return m_jitter ? m_jitter->Snapshot() : JitterStats{};
    }
    const go2cpp::bento::VideoChannel::Stats& channel_stats() const noexcept {
        return m_channel->stats();
    }
    std::uint32_t jitter_next_seq() const noexcept {
        return m_jitter ? m_jitter->next_seq() : 0;
    }
    std::uint32_t jitter_highest_seq() const noexcept {
        return m_jitter ? m_jitter->highest_seq() : 0;
    }
    std::uint32_t jitter_buffered() const noexcept {
        return m_jitter ? m_jitter->buffered() : 0;
    }

   private:
    /// @brief 从分片里取该帧的采集时间戳（90kHz）。
    std::uint32_t StampFor(const go2cpp::bento::FrameView& view) {
        go2cpp::bento::VideoFragmentHeader video;
        if (!go2cpp::bento::DecodeVideoHeader(view.payload, view.payload_bytes, &video)) {
            return To90k(Clock());
        }
        if (video.frag_index == 0 && view.payload_bytes >= go2cpp::bento::kVideoHeaderBytes) {
            MediaHeader header;
            if (DecodeMediaHeader(view.payload + go2cpp::bento::kVideoHeaderBytes,
                                  view.payload_bytes - go2cpp::bento::kVideoHeaderBytes,
                                  &header) &&
                header.capture_ns != 0) {
                m_stamps[video.frame_id] = header.capture_ns;
            }
        }
        const auto it = m_stamps.find(video.frame_id);
        if (it != m_stamps.end()) {
            m_last_timestamp = To90k(it->second);
            return m_last_timestamp;
        }
        // 帧头分片被丢时拿不到采集时刻：沿用上一帧时间戳（帧内差 0），避免用
        // 当前时钟造成几十毫秒的伪抖动，把 jitter 深度推满。
        if (m_last_timestamp != 0) {
            return m_last_timestamp;
        }
        return To90k(Clock());
    }

    std::int64_t Clock() const {
        return m_options.clock ? m_options.clock() : NowNs();
    }

    void OnFrame(const std::uint8_t* data, std::size_t bytes) {
        VideoFrame frame;
        if (!m_decoder->Decode(data, bytes, &frame)) {
            ++m_dropped;
            return;
        }
        ++m_decoded;
        m_last_frame = frame;
        if (m_options.collect_latency) {
            const std::int64_t now = NowNs();
            if (frame.capture_ns > 0 && now >= frame.capture_ns) {
                m_latency.Add(static_cast<double>(now - frame.capture_ns) / 1000000.0);
            }
        }
        if (m_options.renderer != nullptr) {
            m_options.renderer->Present(frame);
        }
        if (m_options.on_frame) {
            m_options.on_frame(frame);
        }
    }

    Options m_options;
    std::unique_ptr<JitterBuffer> m_jitter;
    std::unique_ptr<go2cpp::bento::VideoChannel> m_channel;
    std::unique_ptr<VideoDecoder> m_decoder;
    std::map<std::uint32_t, std::int64_t> m_stamps;
    std::uint32_t m_last_timestamp = 0;
    VideoFrame m_last_frame;
    LatencyRecorder m_latency;
    std::uint64_t m_decoded = 0;
    std::uint64_t m_dropped = 0;
    std::uint64_t m_invalid = 0;
    std::uint64_t m_bytes = 0;
};

/// @brief SDL 窗口画面源：在 (0,0) 开一个真实窗口显示测试图案，供 X11 采集。
/// @details WSLg 默认桌面是纯黑，直接抓 root 会得到全黑截图；这里先创建一个
///          320x240 窗口（软件渲染、无 WM 时位于 0,0），root 采集就能抓到
///          真实应用窗口内容——「采集源」与「渲染结果」两张图因此都有画面。
class SdlPatternWindow {
   public:
    bool Init(int width, int height) {
        if (SDL_Init(SDL_INIT_VIDEO) != 0) {
            return false;
        }
        m_window = SDL_CreateWindow("media_e2e source", 0, 0, width, height, SDL_WINDOW_SHOWN);
        if (m_window == nullptr) {
            return false;
        }
        SDL_SetWindowPosition(m_window, 0, 0);
        m_renderer = SDL_CreateRenderer(m_window, -1, SDL_RENDERER_SOFTWARE);
        if (m_renderer == nullptr) {
            return false;
        }
        m_texture = SDL_CreateTexture(m_renderer, SDL_PIXELFORMAT_ARGB8888,
                                      SDL_TEXTUREACCESS_STREAMING, width, height);
        return m_texture != nullptr;
    }

    void Show(const VideoFrame& bgra) {
        if (m_texture == nullptr) {
            return;
        }
        SDL_UpdateTexture(m_texture, nullptr, bgra.data.data(), bgra.EffectiveStride());
        SDL_RenderClear(m_renderer);
        SDL_RenderCopy(m_renderer, m_texture, nullptr, nullptr);
        SDL_RenderPresent(m_renderer);
        SDL_PumpEvents();
    }

    /// @brief 用 X11 `XGetImage` 抓这个窗口的像素（真实 X11 采集路径）。
    /// @details WSLg 的 root window 是 Weston 合成器的黑底，`XGetImage(root)`
    ///          拿不到 XWayland 窗口内容；抓**具体窗口**可以拿到真实像素。
    bool Capture(VideoFrame* out) {
        if (m_window == nullptr || out == nullptr) {
            return false;
        }
        SDL_SysWMinfo info;
        SDL_VERSION(&info.version);
        if (!SDL_GetWindowWMInfo(m_window, &info) || info.subsystem != SDL_SYSWM_X11) {
            return false;
        }
        Display* display = info.info.x11.display;
        const Window window = info.info.x11.window;
        XWindowAttributes attributes;
        if (XGetWindowAttributes(display, window, &attributes) == 0 || attributes.width <= 0 ||
            attributes.height <= 0) {
            return false;
        }
        XImage* image = XGetImage(display, window, 0, 0, attributes.width, attributes.height,
                                  AllPlanes, ZPixmap);
        if (image == nullptr) {
            return false;
        }
        out->width = attributes.width;
        out->height = attributes.height;
        out->format = PixelFormat::kBGRA;
        out->stride = attributes.width * 4;
        out->data.resize(BgraFrameBytes(attributes.width, attributes.height));
        for (int y = 0; y < attributes.height; ++y) {
            std::uint8_t* row =
                out->data.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(out->stride);
            for (int x = 0; x < attributes.width; ++x) {
                const unsigned long pixel = XGetPixel(image, x, y);
                row[x * 4 + 0] = static_cast<std::uint8_t>(pixel & 0xFF);
                row[x * 4 + 1] = static_cast<std::uint8_t>((pixel >> 8) & 0xFF);
                row[x * 4 + 2] = static_cast<std::uint8_t>((pixel >> 16) & 0xFF);
                row[x * 4 + 3] = 255;
            }
        }
        XDestroyImage(image);
        return true;
    }

    ~SdlPatternWindow() {
        if (m_texture != nullptr) {
            SDL_DestroyTexture(m_texture);
        }
        if (m_renderer != nullptr) {
            SDL_DestroyRenderer(m_renderer);
        }
        if (m_window != nullptr) {
            SDL_DestroyWindow(m_window);
        }
        if (m_window != nullptr) {
            SDL_QuitSubSystem(SDL_INIT_VIDEO);
        }
    }

   private:
    SDL_Window* m_window = nullptr;
    SDL_Renderer* m_renderer = nullptr;
    SDL_Texture* m_texture = nullptr;
};

/// @brief 把 `LatencyRecorder` 填进 `SceneResult`。
void FillLatency(const LatencyRecorder& latency, SceneResult* result) {
    if (latency.count() == 0) {
        return;
    }
    result->p50_ms = latency.p50();
    result->p99_ms = latency.p99();
    result->max_ms = latency.max();
    result->mean_ms = latency.mean();
}

/// @brief 写一张采集/渲染对照截图。
/// @param write_reference true 时额外写「采集→I420→BGRA」参考图：delta 路径的
///        编码本身有损（BT.601 4:2:0 色度下采样），严格逐像素口径是
///        「解码 I420→BGRA == 参考 I420→BGRA」，而不是「== 采集 BGRA」。
void SavePair(const std::string& prefix, const VideoFrame& capture, const VideoFrame& render,
              bool write_reference = false) {
    WritePpmFrame(g_artifacts + "/" + prefix + "_capture.ppm", capture);
    if (render.Valid()) {
        WritePpmFrame(g_artifacts + "/" + prefix + "_render.ppm", render);
    }
    if (write_reference && capture.Valid() && capture.format == PixelFormat::kBGRA) {
        VideoFrame reference = ToI420(capture);
        VideoFrame bgra;
        bgra.width = reference.width;
        bgra.height = reference.height;
        bgra.format = PixelFormat::kBGRA;
        bgra.stride = reference.width * 4;
        bgra.data.resize(BgraFrameBytes(reference.width, reference.height));
        I420ToBgra(reference.data.data(), reference.width, reference.height, bgra.data.data(),
                   bgra.stride);
        WritePpmBgra(g_artifacts + "/" + prefix + "_reference.ppm", bgra.data.data(), bgra.width,
                     bgra.height, bgra.stride);
    }
}

/// @brief 把包级 jitter 统计填进场景结果。
void FillJitter(const JitterStats& stats, SceneResult* result) {
    result->reordered = stats.reordered;
    result->lost = stats.lost_packets;
    result->duplicates = stats.duplicates;
    result->target_delay_ns = stats.target_delay_ns;
    result->jitter_ns = stats.jitter_ns;
}

// ===========================================================================
// 场景 1：真实 X11 采集（WSLg XShm）
// ===========================================================================

SceneResult RunX11Scene() {
    SceneResult result;
    result.name = "x11-real";
    const int width = 320;
    const int height = 240;
    result.width = width;
    result.height = height;

    std::string error;
    // WSLg 的空桌面是纯黑：先开一个真实 SDL 窗口显示测试图案，root 采集才有画面。
    SdlPatternWindow window;
    TestPatternSource window_source(width, height);
    const bool window_ok = window.Init(width, height);
    if (window_ok) {
        VideoFrame first;
        window_source.Capture(&first);
        window.Show(first);
        std::this_thread::sleep_for(std::chrono::milliseconds(120));
    }
    X11CaptureConfig capture_config;
    capture_config.width = width;
    capture_config.height = height;
    capture_config.use_shm = true;
    auto source = CreateX11Capture(capture_config, &error);
    if (!source) {
        result.detail = "SKIP: X11 不可用（" + error + "）";
        g_results.push_back(result);
        return result;
    }
    auto encoder = CreateEncoder(CodecKind::kDeltaI420, width, height);
    auto renderer = std::make_unique<PpmRenderer>(g_artifacts, 0);
    PacketReceiver receiver;
    PacketReceiver::Options options;
    options.use_jitter = true;
    options.width = width;
    options.height = height;
    options.codec = CodecKind::kDeltaI420;
    options.renderer = renderer.get();
    JitterBufferConfig jitter;
    jitter.base_delay_ns = 5'000'000;
    jitter.min_delay_ns = 2'000'000;
    jitter.max_delay_ns = 100'000'000;
    options.jitter = jitter;
    std::vector<std::string> actual;
    options.on_frame = [&actual](const VideoFrame& frame) {
        actual.push_back(FrameSha256(frame));
    };
    if (!receiver.Init(options, &error)) {
        Fail(&result, "receiver init: " + error);
        g_results.push_back(result);
        return result;
    }

    UdpEndpoint tx;
    UdpEndpoint rx;
    if (!tx.Open(&error) || !rx.Open(&error)) {
        Fail(&result, "udp open: " + error);
        g_results.push_back(result);
        return result;
    }
    tx.SetPeerPort(rx.port());
    go2cpp::bento::VideoChannel transmitter(
        [&tx](const std::uint8_t* data, std::size_t bytes) { return tx.Send(data, bytes); },
        [](std::uint32_t, bool, const std::uint8_t*, std::size_t) {});

    std::vector<std::string> expected;
    std::vector<std::uint8_t> encoded;
    std::vector<VideoFrame> saved;
    std::vector<VideoFrame> saved_renders;
    std::uint8_t buffer[2048];
    const int frames = 30;
    const auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < frames; ++i) {
        VideoFrame capture;
        if (window_ok) {
            VideoFrame window_frame;
            window_source.Capture(&window_frame);
            window.Show(window_frame);
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            if (!window.Capture(&capture)) {
                Fail(&result, "window XGetImage failed");
                break;
            }
        } else if (source && source->Capture(&capture)) {
            // 没有 SDL 窗口时退回模块的 X11 root 采集（WSLg 上画面是黑的，
            // 但链路仍然逐帧一致，截图仅作为 fallback 证据）。
        } else {
            Fail(&result, "capture failed");
            break;
        }
        capture.frame_id = static_cast<std::uint32_t>(i + 1);
        capture.capture_ns = NowNs();
        if (i == 0 || (i % 15) == 0) {
            encoder->ForceKeyframe();
        }
        if (!encoder->Encode(capture, &encoded)) {
            Fail(&result, "encode failed");
            break;
        }
        expected.push_back(FrameSha256(ToI420(capture)));
        ++result.sent;
        result.bytes_sent += encoded.size();
        const bool key = encoded[6] != 0;
        if (!transmitter.SendFrame(capture.frame_id, key, encoded.data(), encoded.size())) {
            Fail(&result, "send failed");
            break;
        }
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
        while (receiver.decoded() <= static_cast<std::uint64_t>(i) &&
               std::chrono::steady_clock::now() < deadline) {
            const int received = rx.Recv(buffer, sizeof(buffer), 1);
            if (received > 0) {
                receiver.OnDatagram(buffer, static_cast<std::size_t>(received), NowNs(), NowNs());
            } else {
                receiver.Drain(NowNs());
            }
        }
        if (receiver.decoded() <= static_cast<std::uint64_t>(i)) {
            Fail(&result, "frame timeout");
            break;
        }
        if (i == 0 || i == 15) {
            saved.push_back(capture);
            saved_renders.push_back(renderer->last_bgra());
        }
    }
    receiver.Flush(NowNs());
    const auto end = std::chrono::steady_clock::now();
    result.elapsed_ms = std::chrono::duration<double, std::milli>(end - start).count();
    result.fps = result.elapsed_ms > 0.0
                     ? static_cast<double>(result.sent) * 1000.0 / result.elapsed_ms
                     : 0.0;
    result.delivered = receiver.decoded();
    result.bytes_received = receiver.bytes_received();
    FillLatency(receiver.latency(), &result);
    FillJitter(receiver.jitter_stats(), &result);
    result.stale = receiver.channel_stats().fragments_stale;
    result.incomplete = receiver.channel_stats().frames_incomplete_dropped;

    for (std::size_t i = 0; i < std::min(actual.size(), expected.size()); ++i) {
        if (actual[i] == expected[i]) {
            ++result.correct;
        } else {
            ++result.wrong;
        }
    }
    if (saved.size() >= 2 && saved_renders.size() >= 2) {
        SavePair("x11", saved[0], saved_renders[0], true);
        WritePpmFrame(g_artifacts + "/x11_key_capture.ppm", saved[1]);
        WritePpmFrame(g_artifacts + "/x11_key_render.ppm", saved_renders[1]);
    }
    result.detail = "x11-shm 320x240 delta；逐帧 compare=" +
                    std::to_string(std::min(actual.size(), expected.size())) +
                    " correct=" + std::to_string(result.correct);
    if (result.wrong != 0 || result.delivered != result.sent) {
        Fail(&result, "pixel mismatch or lost frame");
    }
    g_results.push_back(result);
    return result;
}

// ===========================================================================
// 场景 2：raw-bgra 严格逐像素（BGRA SHA-256 相同 + memcmp）
// ===========================================================================

SceneResult RunRawBgraStrictScene() {
    SceneResult result;
    result.name = "raw-bgra-strict";
    const int width = 160;
    const int height = 120;
    result.width = width;
    result.height = height;

    TestPatternSource source(width, height);
    auto encoder = CreateEncoder(CodecKind::kRawBgra, width, height);
    auto renderer = std::make_unique<PpmRenderer>(g_artifacts, 0);
    PacketReceiver receiver;
    PacketReceiver::Options options;
    options.use_jitter = true;
    options.width = width;
    options.height = height;
    options.codec = CodecKind::kRawBgra;
    options.renderer = renderer.get();
    JitterBufferConfig jitter;
    jitter.base_delay_ns = 3'000'000;
    options.jitter = jitter;
    std::vector<VideoFrame> actual_frames;
    std::vector<VideoFrame> rendered_frames;
    options.on_frame = [&actual_frames, &rendered_frames, &renderer](const VideoFrame& frame) {
        actual_frames.push_back(frame);
        rendered_frames.push_back(renderer->last_bgra());
    };
    std::string error;
    if (!receiver.Init(options, &error)) {
        Fail(&result, "receiver init: " + error);
        g_results.push_back(result);
        return result;
    }
    UdpEndpoint tx;
    UdpEndpoint rx;
    if (!tx.Open(&error) || !rx.Open(&error)) {
        Fail(&result, "udp open");
        g_results.push_back(result);
        return result;
    }
    tx.SetPeerPort(rx.port());
    go2cpp::bento::VideoChannel transmitter(
        [&tx](const std::uint8_t* data, std::size_t bytes) { return tx.Send(data, bytes); },
        [](std::uint32_t, bool, const std::uint8_t*, std::size_t) {});

    std::vector<std::string> expected;
    std::vector<VideoFrame> saved_captures;
    std::vector<VideoFrame> saved_renders;
    std::vector<std::uint8_t> encoded;
    std::uint8_t buffer[2048];
    const int frames = 90;
    const auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < frames; ++i) {
        VideoFrame capture;
        if (!source.Capture(&capture)) {
            Fail(&result, "capture failed");
            break;
        }
        capture.frame_id = static_cast<std::uint32_t>(i + 1);
        if (!encoder->Encode(capture, &encoded)) {
            Fail(&result, "encode failed");
            break;
        }
        expected.push_back(FrameSha256(capture));
        ++result.sent;
        result.bytes_sent += encoded.size();
        if (!transmitter.SendFrame(capture.frame_id, true, encoded.data(), encoded.size())) {
            Fail(&result, "send failed");
            break;
        }
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
        while (receiver.decoded() <= static_cast<std::uint64_t>(i) &&
               std::chrono::steady_clock::now() < deadline) {
            const int received = rx.Recv(buffer, sizeof(buffer), 1);
            if (received > 0) {
                receiver.OnDatagram(buffer, static_cast<std::size_t>(received), NowNs(), NowNs());
            } else {
                receiver.Drain(NowNs());
            }
        }
        if (receiver.decoded() <= static_cast<std::uint64_t>(i)) {
            Fail(&result, "frame timeout");
            break;
        }
        if (i == 0 || i == 44 || i == 89) {
            saved_captures.push_back(capture);
            saved_renders.push_back(renderer->last_bgra());
        }
    }
    receiver.Flush(NowNs());
    const auto end = std::chrono::steady_clock::now();
    result.elapsed_ms = std::chrono::duration<double, std::milli>(end - start).count();
    result.fps = result.elapsed_ms > 0.0
                     ? static_cast<double>(result.sent) * 1000.0 / result.elapsed_ms
                     : 0.0;
    result.delivered = receiver.decoded();
    result.bytes_received = receiver.bytes_received();
    FillLatency(receiver.latency(), &result);
    FillJitter(receiver.jitter_stats(), &result);
    result.stale = receiver.channel_stats().fragments_stale;
    result.incomplete = receiver.channel_stats().frames_incomplete_dropped;

    std::uint64_t memcmp_diff_bytes = 0;
    std::uint64_t memcmp_bytes = 0;
    for (std::size_t i = 0; i < std::min(actual_frames.size(), expected.size()); ++i) {
        const std::string decoded_hash = FrameSha256(actual_frames[i]);
        if (decoded_hash == expected[i]) {
            ++result.correct;
        } else {
            ++result.wrong;
        }
    }
    // 渲染结果 vs 采集源：SHA-256 + 逐字节 memcmp（双重证据）。
    for (std::size_t i = 0; i < std::min(rendered_frames.size(), expected.size()); ++i) {
        if (FrameSha256(rendered_frames[i]) != expected[i]) {
            ++result.wrong;
        }
        if (i == 0 && rendered_frames[i].data.size() == saved_captures.front().data.size()) {
            memcmp_bytes = rendered_frames[i].data.size();
            for (std::size_t b = 0; b < memcmp_bytes; ++b) {
                if (rendered_frames[i].data[b] != saved_captures.front().data[b]) {
                    ++memcmp_diff_bytes;
                }
            }
        }
    }
    if (!saved_captures.empty() && !saved_renders.empty()) {
        SavePair("raw_strict", saved_captures.front(), saved_renders.front());
    }
    result.detail = "raw-bgra 160x120 每帧关键帧；BGRA 渲染vs采集 SHA-256 比对 " +
                    std::to_string(std::min(rendered_frames.size(), expected.size())) +
                    " 帧，相同=" + std::to_string(result.correct) +
                    " 不同=" + std::to_string(result.wrong) + " memcmp=" +
                    std::to_string(memcmp_bytes) + "B 差异=" +
                    std::to_string(memcmp_diff_bytes) + "B";
    if (result.wrong != 0 || result.delivered != result.sent || memcmp_diff_bytes != 0) {
        Fail(&result, "pixel mismatch or lost frame");
    }
    g_results.push_back(result);
    return result;
}

// ===========================================================================
// 场景 3：delta 1000 帧（pipeline，真实 UDP 回环）
// ===========================================================================

SceneResult RunDelta1000Scene(int frames) {
    SceneResult result;
    result.name = "delta-1000";
    const int width = 320;
    const int height = 240;
    result.width = width;
    result.height = height;

    MediaConfig config;
    config.width = width;
    config.height = height;
    config.codec = CodecKind::kDeltaI420;
    config.pattern = SyntheticPattern::kPatch;
    config.keyframe_interval = 30;
    std::string error;
    auto receiver = MediaReceiver::Create(config, &error);
    if (!receiver) {
        Fail(&result, "receiver create: " + error);
        g_results.push_back(result);
        return result;
    }
    auto sender = MediaSender::Create(config, receiver->local_port(), &error);
    if (!sender) {
        Fail(&result, "sender create: " + error);
        g_results.push_back(result);
        return result;
    }

    std::uint64_t correct = 0;
    std::uint64_t wrong = 0;
    std::uint64_t order_errors = 0;
    std::vector<VideoFrame> saved_captures;
    std::vector<VideoFrame> saved_renders;

    const auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < frames; ++i) {
        VideoFrame captured;
        std::size_t encoded_bytes = 0;
        if (!sender->SendNext(&captured, &encoded_bytes)) {
            Fail(&result, "SendNext failed at " + std::to_string(i));
            break;
        }
        ++result.sent;
        result.bytes_sent += encoded_bytes;
        const std::string want = FrameSha256(ToI420(captured));
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
        while (receiver->frames_delivered() <= static_cast<std::uint64_t>(i) &&
               std::chrono::steady_clock::now() < deadline) {
            receiver->Pump(1);
        }
        if (receiver->frames_delivered() <= static_cast<std::uint64_t>(i)) {
            Fail(&result, "frame timeout at " + std::to_string(i));
            break;
        }
        const VideoFrame& got = receiver->last_frame();
        if (got.frame_id != static_cast<std::uint32_t>(i + 1)) {
            ++order_errors;
        }
        if (FrameSha256(got) == want) {
            ++correct;
        } else {
            ++wrong;
        }
        if (i == 0 || i == frames / 2 || i == frames - 1) {
            WritePpmFrame(g_artifacts + "/delta1000_capture_" + std::to_string(i + 1) + ".ppm",
                          captured);
            WritePpmFrame(g_artifacts + "/delta1000_render_" + std::to_string(i + 1) + ".ppm", got);
            saved_captures.push_back(captured);
            saved_renders.push_back(got);
        }
    }
    const auto drain_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (receiver->frames_delivered() < result.sent &&
           std::chrono::steady_clock::now() < drain_deadline) {
        receiver->Pump(2);
    }
    const auto end = std::chrono::steady_clock::now();
    result.elapsed_ms = std::chrono::duration<double, std::milli>(end - start).count();
    result.fps = result.elapsed_ms > 0.0
                     ? static_cast<double>(result.sent) * 1000.0 / result.elapsed_ms
                     : 0.0;
    result.delivered = receiver->frames_delivered();
    result.bytes_received = receiver->bytes_received();
    result.correct = correct;
    result.wrong = wrong;
    FillLatency(receiver->latency(), &result);
    result.stale = receiver->transport_stats().fragments_stale;
    result.incomplete = receiver->transport_stats().frames_incomplete_dropped;
    result.duplicates = receiver->transport_stats().fragments_duplicate;
    result.detail = "delta-i420 320x240 patch keyframe=30；帧号错序=" +
                    std::to_string(order_errors) + " bytes_tx=" +
                    std::to_string(result.bytes_sent) + " bytes_rx=" +
                    std::to_string(result.bytes_received);
    if (result.delivered != result.sent || result.wrong != 0 || order_errors != 0 ||
        result.bytes_sent != result.bytes_received || result.incomplete != 0 ||
        result.stale != 0) {
        Fail(&result, "lost/order/mismatch");
    }
    if (!saved_captures.empty()) {
        SavePair("delta1000_mid", saved_captures[saved_captures.size() / 2],
                 saved_renders[saved_renders.size() / 2], true);
    }
    g_results.push_back(result);
    return result;
}

// ===========================================================================
// 场景 4：弱网（±50ms 抖动 + 5% 丢包 + 乱序）delta，jitter 开/关对照
// ===========================================================================

/// @brief 发送端把一帧编码后经模拟网络发出（包级抖动/丢包）。
struct SimulatedSender {
    SimulatedNetwork net;
    std::vector<std::string> expected;  ///< 每帧 I420 SHA-256（按 frame_id-1）。
    std::vector<bool> key_flags;
    std::uint64_t bytes_sent = 0;
    std::vector<VideoFrame> saved_captures;  ///< 前若干帧采集（截图用）。

    explicit SimulatedSender(SimulatedNetwork::Config config) : net(config) {}
};

/// @brief 用模拟网络跑一遍编码 + 分片发送。
void EncodeIntoNetwork(SimulatedSender* sender, VideoSource* source, VideoEncoder* encoder,
                       CodecKind codec, int frames, int key_interval, bool every_key,
                       int save_first) {
    (void)codec;
    std::vector<std::uint8_t> encoded;
    go2cpp::bento::VideoChannel transmitter(
        [sender](const std::uint8_t* data, std::size_t bytes) {
            sender->net.Send(data, bytes);
            return true;
        },
        [](std::uint32_t, bool, const std::uint8_t*, std::size_t) {});
    for (int i = 0; i < frames; ++i) {
        VideoFrame capture;
        if (!source->Capture(&capture)) {
            break;
        }
        capture.frame_id = static_cast<std::uint32_t>(i + 1);
        capture.capture_ns = sender->net.now_ns();
        if (every_key || (key_interval > 0 && (i % key_interval) == 0)) {
            encoder->ForceKeyframe();
        }
        if (!encoder->Encode(capture, &encoded)) {
            break;
        }
        const bool key = encoded[6] != 0;
        sender->expected.push_back(FrameSha256(ToI420(capture)));
        sender->key_flags.push_back(key);
        sender->bytes_sent += encoded.size();
        if (static_cast<int>(sender->saved_captures.size()) < save_first) {
            sender->saved_captures.push_back(capture);
        }
        transmitter.SendFrame(capture.frame_id, key, encoded.data(), encoded.size());
        sender->net.AdvanceFrame();
    }
}

/// @brief 用模拟网络把包投递给接收端，返回场景统计。
SceneResult ReceiveFromNetwork(SimulatedSender* sender, bool use_jitter, CodecKind codec,
                               int width, int height, const std::string& name,
                               const std::string& artifact_prefix, bool save_bad) {
    SceneResult result;
    result.name = name;
    result.width = width;
    result.height = height;
    result.sent = sender->expected.size();
    result.bytes_sent = sender->bytes_sent;

    auto renderer = std::make_unique<PpmRenderer>(g_artifacts, 0);
    PacketReceiver receiver;
    PacketReceiver::Options options;
    options.use_jitter = use_jitter;
    options.collect_latency = false;
    options.width = width;
    options.height = height;
    options.codec = codec;
    options.renderer = renderer.get();
    JitterBufferConfig jitter;
    // 模拟网络抖动跨度 100ms：base 取 150ms，保证 target 覆盖最大帧间延迟差，
    // 否则「迟到但没丢」的包会被误判成丢包（raw-i420 每帧 24 片时尤其明显）。
    jitter.base_delay_ns = 150'000'000;
    jitter.min_delay_ns = 150'000'000;
    jitter.max_delay_ns = 400'000'000;
    jitter.jitter_factor_q8 = 1024;
    options.jitter = jitter;
    options.clock = [sender]() { return sender ? sender->net.now_ns() : NowNs(); };

    std::vector<std::uint32_t> ids;
    std::vector<bool> keys;
    std::vector<std::string> hashes;
    std::vector<VideoFrame> renders;
    VideoFrame first_render;
    std::uint32_t first_render_id = 0;
    bool has_first_render = false;
    std::string first_bad_sha;
    std::uint32_t first_bad_id = 0;
    options.on_frame = [&](const VideoFrame& frame) {
        ids.push_back(frame.frame_id);
        keys.push_back(frame.keyframe);
        const std::string hash = FrameSha256(frame);
        hashes.push_back(hash);
        if (!has_first_render) {
            first_render = renderer->last_bgra();
            first_render_id = frame.frame_id;
            has_first_render = true;
        }
        if (first_bad_id == 0 && !sender->expected.empty() && frame.frame_id >= 1 &&
            frame.frame_id <= sender->expected.size() &&
            hash != sender->expected[frame.frame_id - 1]) {
            first_bad_id = frame.frame_id;
            first_bad_sha = hash;
            renders.push_back(renderer->last_bgra());
        }
    };
    std::string error;
    if (!receiver.Init(options, &error)) {
        Fail(&result, "receiver init: " + error);
        g_results.push_back(result);
        return result;
    }

    sender->net.DeliverDue([&receiver](const std::uint8_t* data, std::size_t bytes,
                                       std::int64_t due_ns) {
        receiver.OnDatagram(data, bytes, due_ns, due_ns);
    });
    receiver.Flush(sender->net.now_ns() + 1'000'000'000LL);

    result.delivered = hashes.size();
    for (std::size_t i = 0; i < hashes.size(); ++i) {
        const std::uint32_t id = ids[i];
        if (id == 0 || id > sender->expected.size()) {
            continue;
        }
        const bool match = hashes[i] == sender->expected[id - 1];
        if (match) {
            ++result.correct;
        } else {
            ++result.wrong;
        }
        if (keys[i]) {
            ++result.keyframes;
            if (match) {
                ++result.keyframes_correct;
            }
        }
    }
    const JitterStats jitter_stats = receiver.jitter_stats();
    result.reordered = jitter_stats.reordered;
    result.lost = jitter_stats.lost_packets;
    result.duplicates = jitter_stats.duplicates;
    result.target_delay_ns = jitter_stats.target_delay_ns;
    result.jitter_ns = jitter_stats.jitter_ns;
    result.stale = receiver.channel_stats().fragments_stale;
    result.incomplete = receiver.channel_stats().frames_incomplete_dropped;
    result.bytes_received = receiver.bytes_received();
    result.duplicates += receiver.channel_stats().fragments_duplicate;

    const VideoFrame* capture_for_render = nullptr;
    if (first_render_id >= 1 && first_render_id <= sender->saved_captures.size()) {
        capture_for_render = &sender->saved_captures[first_render_id - 1];
    } else if (!sender->saved_captures.empty()) {
        capture_for_render = &sender->saved_captures.front();
    }
    if (capture_for_render != nullptr) {
        WritePpmFrame(g_artifacts + "/" + artifact_prefix + "_capture.ppm", *capture_for_render);
    }
    if (!renders.empty() && save_bad) {
        WritePpmFrame(g_artifacts + "/" + artifact_prefix + "_bad.ppm", renders.front());
        if (first_bad_id >= 1 && first_bad_id <= sender->saved_captures.size()) {
            WritePpmFrame(g_artifacts + "/" + artifact_prefix + "_expected.ppm",
                          sender->saved_captures[first_bad_id - 1]);
        }
    } else if (has_first_render) {
        // 第一个交付帧（正常路径见证逐像素一致）。
        WritePpmFrame(g_artifacts + "/" + artifact_prefix + "_render.ppm", first_render);
    }
    result.detail = "sim-net loss=5% jitter=0..100ms; 关键帧正确 " +
                    std::to_string(result.keyframes_correct) + "/" +
                    std::to_string(result.keyframes) + "；错帧代表丢包后 delta 参考断裂" +
                    (first_bad_id != 0 ? "（首个错帧 id=" + std::to_string(first_bad_id) + "）" : "");
    if (use_jitter) {
        if (result.keyframes != 0 && result.keyframes_correct != result.keyframes) {
            Fail(&result, "delivered keyframe mismatch");
        }
    }
    g_results.push_back(result);
    return result;
}

// ===========================================================================
// 场景 5：弱网 raw-i420（每帧独立，交付帧必须 100% 正确）
// ===========================================================================

// ===========================================================================
// 场景 6：降级（带宽/丢包 → 分辨率/帧率真实变化）
// ===========================================================================

SceneResult RunDegradationScene() {
    SceneResult result;
    result.name = "degradation";
    MediaConfig sender_config;
    sender_config.width = 320;
    sender_config.height = 240;
    sender_config.codec = CodecKind::kDeltaI420;
    sender_config.pattern = SyntheticPattern::kPatch;
    sender_config.keyframe_interval = 15;
    sender_config.degradation = true;
    sender_config.degradation_config.downgrade_hold_ms = 50;
    sender_config.degradation_config.upgrade_hold_ms = 60000;
    sender_config.degradation_config.max_downgrade_step = 2;
    MediaConfig receiver_config = sender_config;
    receiver_config.degradation = false;  // 收端不做帧率门控，避免叠加跳帧

    std::string error;
    auto receiver = MediaReceiver::Create(receiver_config, &error);
    if (!receiver) {
        Fail(&result, "receiver create: " + error);
        g_results.push_back(result);
        return result;
    }
    auto sender = MediaSender::Create(sender_config, receiver->local_port(), &error);
    if (!sender) {
        Fail(&result, "sender create: " + error);
        g_results.push_back(result);
        return result;
    }

    struct Phase {
        std::uint64_t bps;
        double loss;
        int frames;
        const char* name;
    };
    const std::vector<Phase> phases = {
        {2'000'000, 0.00, 80, "full"},
        {700'000, 0.00, 80, "reduced"},
        {320'000, 0.00, 80, "low"},
        {120'000, 0.00, 80, "minimal"},
    };
    std::map<std::uint32_t, std::string> expected;
    std::vector<std::string> lines;
    std::vector<VideoFrame> saved_captures;
    std::vector<VideoFrame> saved_renders;
    std::vector<std::string> saved_names;
    std::uint64_t sent = 0;
    std::uint64_t correct = 0;
    std::uint64_t wrong = 0;
    std::uint64_t change_events = 0;
    std::uint64_t decode_failures = 0;

    for (const Phase& phase : phases) {
        int phase_sent = 0;
        int phase_skipped = 0;
        VideoFrame last_capture;
        VideoFrame last_render;
        bool has_last = false;
        for (int i = 0; i < phase.frames; ++i) {
            NetworkSample sample;
            sample.available_bps = phase.bps;
            sample.loss_rate = phase.loss;
            sample.rtt_ms = 0;
            sample.valid = true;
            const DegradationEvent event = sender->OnNetworkSample(sample);
            if (event.changed) {
                ++change_events;
                std::printf("[degradation] %s: %s -> %s (%s)\n", phase.name,
                            QualityLevelName(event.from), QualityLevelName(event.to),
                            DegradationReasonName(event.reason));
            }
            VideoFrame captured;
            std::size_t encoded_bytes = 0;
            if (!sender->SendNext(&captured, &encoded_bytes)) {
                Fail(&result, "SendNext failed");
                break;
            }
            if (encoded_bytes == 0) {
                ++phase_skipped;
                std::this_thread::sleep_for(std::chrono::milliseconds(4));
                continue;
            }
            expected[captured.frame_id] = FrameSha256(ToI420(captured));
            result.bytes_sent += encoded_bytes;
            ++sent;
            ++phase_sent;
            const std::uint64_t before = receiver->frames_delivered();
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(30);
            while (receiver->frames_delivered() == before &&
                   std::chrono::steady_clock::now() < deadline) {
                receiver->Pump(1);
            }
            if (receiver->frames_delivered() > before) {
                const VideoFrame& got = receiver->last_frame();
                const auto it = expected.find(got.frame_id);
                if (it != expected.end()) {
                    if (FrameSha256(got) == it->second) {
                        ++correct;
                    } else {
                        ++wrong;
                    }
                }
            } else {
                ++decode_failures;
            }
            if (receiver->frames_delivered() > before) {
                last_capture = captured;
                last_render = receiver->last_frame();
                has_last = true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(4));
        }
        if (has_last) {
            saved_captures.push_back(last_capture);
            saved_renders.push_back(last_render);
            saved_names.push_back(phase.name);
        }
        const DegradationController* controller = sender->degradation();
        const std::string line =
            std::string("| ") + phase.name + " | " + std::to_string(phase.bps) + " | " +
            QualityLevelName(controller->level()) + " | " + std::to_string(sender->current_width()) +
            "x" + std::to_string(sender->current_height()) + " | " +
            std::to_string(controller->target_fps()) + "fps | " + std::to_string(phase_sent) +
            " | " + std::to_string(phase_skipped) + " |";
        lines.push_back(line);
        std::printf("[degradation] %s\n", line.c_str());
    }

    for (int i = 0; i < 50 && receiver->frames_delivered() < sent; ++i) {
        receiver->Pump(2);
    }
    result.sent = sent;
    result.delivered = receiver->frames_delivered();
    result.bytes_received = receiver->bytes_received();
    result.correct = correct;
    result.wrong = wrong;
    result.width = sender->current_width();
    result.height = sender->current_height();
    result.detail = "档位变化=" + std::to_string(change_events) + " 次；最终 " +
                    QualityLevelName(sender->degradation()->level()) + " " +
                    std::to_string(sender->current_width()) + "x" +
                    std::to_string(sender->current_height()) + " @" +
                    std::to_string(sender->degradation()->target_fps()) + "fps" +
                    "；跳帧=" + std::to_string(sender->frames_skipped()) +
                    "；交付=" + std::to_string(result.delivered) +
                    " 解码失败/超时=" + std::to_string(decode_failures);
    if (change_events < 3) {
        Fail(&result, "档位变化不足 3 次");
    }
    if (sender->degradation()->level() != QualityLevel::kMinimal) {
        Fail(&result, "最终未到 minimal");
    }
    if (sender->current_width() != 160 || sender->current_height() != 120) {
        Fail(&result, "最终分辨率不是 160x120");
    }
    if (wrong != 0) {
        Fail(&result, "降级后交付帧内容错");
    }
    for (std::size_t i = 0; i < saved_captures.size() && i < saved_renders.size(); ++i) {
        if (saved_renders[i].Valid()) {
            SavePair("degradation_" + saved_names[i], saved_captures[i], saved_renders[i], true);
        }
    }
    std::FILE* file = std::fopen("/tmp/media_e2e_degradation.md", "w");
    if (file != nullptr) {
        std::fprintf(file, "# 降级阶梯实测（自动生成）\n\n");
        std::fprintf(file, "| 阶段 | 可用带宽 bps | 档位 | 发送尺寸 | 目标帧率 | 实际发送 | 帧率门控跳过 |\n");
        std::fprintf(file, "|---|---|---|---|---|---|---|\n");
        for (const std::string& line : lines) {
            std::fprintf(file, "%s\n", line.c_str());
        }
        std::fclose(file);
    }
    g_results.push_back(result);
    return result;
}

// ===========================================================================
// 场景 7：延迟分段 + 物理下限
// ===========================================================================

struct LatencyRow {
    std::string stage;
    double p50_us = 0.0;
    double p99_us = 0.0;
    double max_us = 0.0;
};

std::vector<LatencyRow> g_latency_rows;
std::vector<std::string> g_bench_rows;

/// @brief 测量 memcpy 带宽（GB/s）。
double MeasureMemcpyGbps(std::size_t bytes, int iters) {
    std::vector<std::uint8_t> src(bytes, 0x5A);
    std::vector<std::uint8_t> dst(bytes, 0);
    volatile std::uint8_t sink = 0;
    const std::int64_t start = NowNs();
    for (int i = 0; i < iters; ++i) {
        std::memcpy(dst.data(), src.data(), bytes);
        sink = static_cast<std::uint8_t>(sink ^ dst[static_cast<std::size_t>(i) % bytes]);
    }
    const std::int64_t end = NowNs();
    (void)sink;
    const double elapsed_s = static_cast<double>(end - start) / 1e9;
    return static_cast<double>(bytes) * static_cast<double>(iters) / elapsed_s / 1e9;
}

/// @brief 场景 7：逐段计时（真实 UDP 回环）+ memcpy/syscall 下限。
void RunLatencyScene(int frames) {
    const int width = 320;
    const int height = 240;
    TestPatternSource source(width, height);
    auto encoder = CreateEncoder(CodecKind::kDeltaI420, width, height);
    auto decoder = CreateDecoder(CodecKind::kDeltaI420, width, height);

    UdpEndpoint tx;
    UdpEndpoint rx;
    std::string error;
    if (!tx.Open(&error) || !rx.Open(&error)) {
        return;
    }
    tx.SetPeerPort(rx.port());

    std::vector<std::vector<std::uint8_t>> packets;
    go2cpp::bento::VideoChannel slice_channel(
        [&packets](const std::uint8_t* data, std::size_t bytes) {
            packets.emplace_back(data, data + bytes);
            return true;
        },
        [](std::uint32_t, bool, const std::uint8_t*, std::size_t) {});
    std::vector<std::uint8_t> delivered;
    go2cpp::bento::VideoChannel recv_channel(
        [](const std::uint8_t*, std::size_t) { return true; },
        [&delivered](std::uint32_t, bool, const std::uint8_t* data, std::size_t bytes) {
            delivered.assign(data, data + bytes);
        });

    LatencyRecorder capture_time;
    LatencyRecorder convert_time;
    LatencyRecorder encode_time;
    LatencyRecorder slice_time;
    LatencyRecorder send_time;
    LatencyRecorder recv_time;
    LatencyRecorder decode_time;
    LatencyRecorder render_time;
    LatencyRecorder end_to_end;
    std::vector<std::uint8_t> encoded;
    std::vector<std::uint8_t> bgra(BgraFrameBytes(width, height));
    std::uint8_t buffer[2048];
    std::uint32_t frame_id = 0;
    std::uint64_t fragments_total = 0;

    for (int i = 0; i < frames; ++i) {
        const std::int64_t t0 = NowNs();
        VideoFrame capture;
        source.Capture(&capture);
        const std::int64_t t1 = NowNs();
        capture_time.Add(static_cast<double>(t1 - t0) / 1e6);
        capture.frame_id = ++frame_id;
        capture.capture_ns = t0;
        const VideoFrame reference = ToI420(capture);
        const std::int64_t t2 = NowNs();
        convert_time.Add(static_cast<double>(t2 - t1) / 1e6);
        if ((i % 30) == 0) {
            encoder->ForceKeyframe();
        }
        encoder->Encode(capture, &encoded);
        const std::int64_t t3 = NowNs();
        encode_time.Add(static_cast<double>(t3 - t2) / 1e6);
        packets.clear();
        slice_channel.SendFrame(capture.frame_id, encoded[6] != 0, encoded.data(), encoded.size());
        const std::int64_t t4 = NowNs();
        slice_time.Add(static_cast<double>(t4 - t3) / 1e6);
        for (const std::vector<std::uint8_t>& packet : packets) {
            tx.Send(packet.data(), packet.size());
        }
        const std::int64_t t5 = NowNs();
        send_time.Add(static_cast<double>(t5 - t4) / 1e6);
        fragments_total += packets.size();
        delivered.clear();
        while (delivered.empty()) {
            const int received = rx.Recv(buffer, sizeof(buffer), 2);
            if (received <= 0) {
                break;
            }
            go2cpp::bento::FrameView view;
            if (go2cpp::bento::ParseFrame(buffer, static_cast<std::size_t>(received), &view)) {
                recv_channel.OnFrame(view.header, view.payload, view.payload_bytes);
            }
        }
        const std::int64_t t6 = NowNs();
        recv_time.Add(static_cast<double>(t6 - t5) / 1e6);
        VideoFrame decoded;
        if (delivered.empty()) {
            continue;
        }
        decoder->Decode(delivered.data(), delivered.size(), &decoded);
        const std::int64_t t7 = NowNs();
        decode_time.Add(static_cast<double>(t7 - t6) / 1e6);
        I420ToBgra(decoded.data.data(), decoded.width, decoded.height, bgra.data(), width * 4);
        const std::int64_t t8 = NowNs();
        render_time.Add(static_cast<double>(t8 - t7) / 1e6);
        end_to_end.Add(static_cast<double>(t8 - t0) / 1e6);
        (void)reference;
    }

    auto add_row = [](const char* stage, const LatencyRecorder& recorder) {
        if (recorder.count() == 0) {
            return;
        }
        LatencyRow row;
        row.stage = stage;
        row.p50_us = recorder.p50() * 1000.0;
        row.p99_us = recorder.p99() * 1000.0;
        row.max_us = recorder.max() * 1000.0;
        g_latency_rows.push_back(row);
    };
    add_row("采集(TestPattern 生成)", capture_time);
    add_row("BGRA->I420", convert_time);
    add_row("delta 编码", encode_time);
    add_row("VideoChannel 分片", slice_time);
    add_row("sendto x N", send_time);
    add_row("recv+重组", recv_time);
    add_row("delta 解码", decode_time);
    add_row("I420->BGRA 渲染", render_time);
    add_row("端到端(采集->渲染)", end_to_end);

    const double memcpy_1k = MeasureMemcpyGbps(1024, 2000000);
    const double memcpy_64k = MeasureMemcpyGbps(64 * 1024, 20000);
    const double memcpy_256k = MeasureMemcpyGbps(256 * 1024, 5000);

    UdpEndpoint a;
    UdpEndpoint b;
    a.Open(&error);
    b.Open(&error);
    a.SetPeerPort(b.port());
    std::uint8_t probe[1200] = {0};
    const int pairs = 20000;
    const std::int64_t bench_start = NowNs();
    for (int i = 0; i < pairs; ++i) {
        a.Send(probe, sizeof(probe));
        b.Recv(probe, sizeof(probe), 2);
    }
    const std::int64_t bench_end = NowNs();
    const double pair_us = static_cast<double>(bench_end - bench_start) / 1000.0 / pairs;

    char line[256];
    std::snprintf(line, sizeof(line), "memcpy 1KB: %.2f GB/s", memcpy_1k);
    g_bench_rows.push_back(line);
    std::snprintf(line, sizeof(line), "memcpy 64KB: %.2f GB/s", memcpy_64k);
    g_bench_rows.push_back(line);
    std::snprintf(line, sizeof(line), "memcpy 256KB: %.2f GB/s", memcpy_256k);
    g_bench_rows.push_back(line);
    std::snprintf(line, sizeof(line), "UDP 回环 sendto+recv 1 对: %.3f us", pair_us);
    g_bench_rows.push_back(line);

    // 物理下限（每帧，估算）：
    //   采集拷贝 307200B + BGRA->I420(读 307200+写 115200) + delta 编码(读 230400+写 ~115200)
    //   + 分片(读 115200+写 115200) + 内核 send/recv 各 115200 + 重组 115200
    //   + 解码(读 115200+写 115200) + 渲染(读 115200+写 307200) ≈ 2.15MB 数据搬运
    //   + 片数 x (sendto+recv 一对) 的 syscall 成本。
    const double copy_bytes = 2150000.0;
    const double copy_lower_ms = copy_bytes / (memcpy_64k * 1e9) * 1000.0;
    const double fragments = static_cast<double>(fragments_total) / std::max(1, frames);
    const double syscall_lower_ms = fragments * pair_us / 1000.0;
    const double lower_ms = copy_lower_ms + syscall_lower_ms;
    const double measured_ms = end_to_end.p50();
    const double ratio = measured_ms / lower_ms;
    std::printf(
        "[latency] e2e p50=%.3fms p99=%.3fms | copy_lower=%.3fms syscall_lower=%.3fms "
        "total_lower=%.3fms ratio=%.2fx (frags/frame=%.1f memcpy64k=%.1fGB/s)\n",
        measured_ms, end_to_end.p99(), copy_lower_ms, syscall_lower_ms, lower_ms, ratio, fragments,
        memcpy_64k);

    std::FILE* file = std::fopen("/tmp/media_e2e_latency.md", "w");
    if (file != nullptr) {
        std::fprintf(file, "# 分段延迟与物理下限（自动生成）\n\n");
        std::fprintf(file, "| 阶段 | p50 us | p99 us | max us |\n|---|---|---|---|\n");
        for (const LatencyRow& row : g_latency_rows) {
            std::fprintf(file, "| %s | %.1f | %.1f | %.1f |\n", row.stage.c_str(), row.p50_us,
                         row.p99_us, row.max_us);
        }
        std::fprintf(file, "\n## 基准\n\n");
        for (const std::string& bench : g_bench_rows) {
            std::fprintf(file, "- %s\n", bench.c_str());
        }
        std::fprintf(file,
                     "\n## 物理下限（估算，%d 帧，%.1f 片/帧）\n\n"
                     "- 每帧数据搬运约 %.0f B / memcpy 64KB 带宽 %.2f GB/s = **%.3f ms**\n"
                     "- 每帧 %.1f 次 sendto+recv（%.3f us/对）= **%.3f ms**\n"
                     "- 合计下限 **%.3f ms**；实测端到端 p50 **%.3f ms** → **%.2fx 下限**\n",
                     frames, fragments, copy_bytes, memcpy_64k, copy_lower_ms, fragments, pair_us,
                     syscall_lower_ms, lower_ms, measured_ms, ratio);
        std::fclose(file);
    }
}

// ===========================================================================
// 弱网场景入口（场景 4/5）
// ===========================================================================

/// @brief 构造一份「±50ms 抖动 + 5% 丢包」的模拟网络配置。
SimulatedNetwork::Config WeakNetConfig(std::int64_t frame_gap_ns, std::uint32_t seed) {
    SimulatedNetwork::Config config;
    config.loss_permille = 50;            // 5% 丢包
    config.base_delay_ns = 5'000'000;     // 5ms 基础传输延迟
    config.jitter_ns = 100'000'000;       // 0..100ms 随机延迟（均值 50ms，±50ms）
    config.frame_gap_ns = frame_gap_ns;
    config.seed = seed;
    return config;
}

/// @brief 场景 4：delta 1000 帧弱网，jitter 开/关对照。
void RunWeakNetDeltaScene() {
    const int width = 320;
    const int height = 240;
    const int frames = 1000;
    const SimulatedNetwork::Config config = WeakNetConfig(33'000'000, 0x12345678U);

    SimulatedSender jitter_sender(config);
    TestPatternSource jitter_source(width, height);
    auto jitter_encoder = CreateEncoder(CodecKind::kDeltaI420, width, height);
    EncodeIntoNetwork(&jitter_sender, &jitter_source, jitter_encoder.get(), CodecKind::kDeltaI420,
                      frames, 30, false, frames);
    const SceneResult with_jitter =
        ReceiveFromNetwork(&jitter_sender, true, CodecKind::kDeltaI420, width, height,
                           "weaknet-delta-jitter", "weaknet_delta_jitter", false);

    SimulatedSender plain_sender(config);
    TestPatternSource plain_source(width, height);
    auto plain_encoder = CreateEncoder(CodecKind::kDeltaI420, width, height);
    EncodeIntoNetwork(&plain_sender, &plain_source, plain_encoder.get(), CodecKind::kDeltaI420,
                      frames, 30, false, frames);
    const SceneResult without_jitter =
        ReceiveFromNetwork(&plain_sender, false, CodecKind::kDeltaI420, width, height,
                           "weaknet-delta-nojitter", "weaknet_delta_nojitter", true);

    // 对照结论写进两条记录的 detail，并在控制台打印。
    std::printf(
        "[weaknet-delta] jitter: delivered=%llu correct=%llu wrong=%llu key=%llu/%llu "
        "reordered=%llu lost=%llu target=%.1fms\n",
        static_cast<unsigned long long>(with_jitter.delivered),
        static_cast<unsigned long long>(with_jitter.correct),
        static_cast<unsigned long long>(with_jitter.wrong),
        static_cast<unsigned long long>(with_jitter.keyframes_correct),
        static_cast<unsigned long long>(with_jitter.keyframes),
        static_cast<unsigned long long>(with_jitter.reordered),
        static_cast<unsigned long long>(with_jitter.lost),
        static_cast<double>(with_jitter.target_delay_ns) / 1e6);
    std::printf(
        "[weaknet-delta] nojitter: delivered=%llu correct=%llu wrong=%llu key=%llu/%llu "
        "stale=%llu incomplete=%llu\n",
        static_cast<unsigned long long>(without_jitter.delivered),
        static_cast<unsigned long long>(without_jitter.correct),
        static_cast<unsigned long long>(without_jitter.wrong),
        static_cast<unsigned long long>(without_jitter.keyframes_correct),
        static_cast<unsigned long long>(without_jitter.keyframes),
        static_cast<unsigned long long>(without_jitter.stale),
        static_cast<unsigned long long>(without_jitter.incomplete));
    // 给带 jitter 的记录补上对照结论。
    for (SceneResult& r : g_results) {
        if (r.name == "weaknet-delta-jitter") {
            r.detail += "；无 jitter 对照：交付=" + std::to_string(without_jitter.delivered) +
                        " 错帧=" + std::to_string(without_jitter.wrong) +
                        " stale=" + std::to_string(without_jitter.stale);
            if (without_jitter.delivered >= with_jitter.delivered &&
                without_jitter.wrong <= with_jitter.wrong) {
                Fail(&r, "jitter 未改善交付/正确性");
            }
        }
    }
}

/// @brief 场景 5：raw-i420（每帧独立）弱网，交付帧必须 100% 逐字节正确。
void RunWeakNetRawScene() {
    // 80x60 raw-i420 = 4800B/帧 = 4 个 1200B 分片：5% 丢包下每 20 包一个空洞，
    // jitter 每个空洞等待 target_delay（150ms）仍能跟上 30fps（133 包/s > 120 包/s）。
    // 若用 320x240（97 片/帧），jitter 会因每个空洞独立等待而吞吐不足，属于
    // 已知实现行为（见 docs/modules/jitter-degradation.md §7）。
    const int width = 80;
    const int height = 60;
    const int frames = 500;
    const SimulatedNetwork::Config config = WeakNetConfig(33'000'000, 0xCAFEBABEU);

    SimulatedSender jitter_sender(config);
    TestPatternSource jitter_source(width, height);
    auto jitter_encoder = CreateEncoder(CodecKind::kRawI420, width, height);
    EncodeIntoNetwork(&jitter_sender, &jitter_source, jitter_encoder.get(), CodecKind::kRawI420,
                      frames, 0, true, 1);
    const SceneResult with_jitter =
        ReceiveFromNetwork(&jitter_sender, true, CodecKind::kRawI420, width, height,
                           "weaknet-raw-jitter", "weaknet_raw_jitter", false);

    SimulatedSender plain_sender(config);
    TestPatternSource plain_source(width, height);
    auto plain_encoder = CreateEncoder(CodecKind::kRawI420, width, height);
    EncodeIntoNetwork(&plain_sender, &plain_source, plain_encoder.get(), CodecKind::kRawI420,
                      frames, 0, true, 1);
    const SceneResult without_jitter =
        ReceiveFromNetwork(&plain_sender, false, CodecKind::kRawI420, width, height,
                           "weaknet-raw-nojitter", "weaknet_raw_nojitter", false);

    std::printf("[weaknet-raw] jitter: delivered=%llu correct=%llu wrong=%llu reordered=%llu "
                "lost=%llu target=%.1fms\n",
                static_cast<unsigned long long>(with_jitter.delivered),
                static_cast<unsigned long long>(with_jitter.correct),
                static_cast<unsigned long long>(with_jitter.wrong),
                static_cast<unsigned long long>(with_jitter.reordered),
                static_cast<unsigned long long>(with_jitter.lost),
                static_cast<double>(with_jitter.target_delay_ns) / 1e6);
    std::printf("[weaknet-raw] nojitter: delivered=%llu correct=%llu wrong=%llu\n",
                static_cast<unsigned long long>(without_jitter.delivered),
                static_cast<unsigned long long>(without_jitter.correct),
                static_cast<unsigned long long>(without_jitter.wrong));

    for (SceneResult& r : g_results) {
        if (r.name == "weaknet-raw-jitter") {
            r.detail += "；无 jitter 对照：交付=" + std::to_string(without_jitter.delivered) +
                        " 错帧=" + std::to_string(without_jitter.wrong);
            if (r.wrong != 0 || r.correct != r.delivered) {
                Fail(&r, "交付帧存在像素错误（raw 每帧独立，不应错）");
            }
        }
    }
}

// ===========================================================================
// 报告
// ===========================================================================

std::string FormatDouble(double value, int precision = 3) {
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "%.*f", precision, value);
    return buffer;
}

void WriteReport(const std::string& path) {
    std::FILE* file = std::fopen(path.c_str(), "w");
    if (file == nullptr) {
        return;
    }
    std::fprintf(file, "# media_e2e 原始数据（自动生成）\n\n");
    std::fprintf(file,
                 "| 场景 | 尺寸 | 发送 | 交付 | 逐帧正确 | 逐帧错误 | p50 ms | p99 ms | max ms "
                 "| fps | 备注 |\n");
    std::fprintf(file, "|---|---|---|---|---|---|---|---|---|---|---|\n");
    for (const SceneResult& r : g_results) {
        std::fprintf(file,
                     "| %s | %dx%d | %llu | %llu | %llu | %llu | %s | %s | %s | %s | %s%s%s |\n",
                     r.name.c_str(), r.width, r.height,
                     static_cast<unsigned long long>(r.sent),
                     static_cast<unsigned long long>(r.delivered),
                     static_cast<unsigned long long>(r.correct),
                     static_cast<unsigned long long>(r.wrong), FormatDouble(r.p50_ms).c_str(),
                     FormatDouble(r.p99_ms).c_str(), FormatDouble(r.max_ms).c_str(),
                     FormatDouble(r.fps, 1).c_str(), r.detail.c_str(), r.ok ? "" : " / FAIL: ",
                     r.fail_reason.c_str());
    }
    std::fprintf(file, "\n## 传输/抖动细节\n\n");
    std::fprintf(file,
                 "| 场景 | stale | incomplete | duplicate | reordered | lost | target_delay_us | "
                 "jitter_us | bytes_tx | bytes_rx |\n");
    std::fprintf(file, "|---|---|---|---|---|---|---|---|---|---|\n");
    for (const SceneResult& r : g_results) {
        std::fprintf(file,
                     "| %s | %llu | %llu | %llu | %llu | %llu | %lld | %lld | %llu | %llu |\n",
                     r.name.c_str(), static_cast<unsigned long long>(r.stale),
                     static_cast<unsigned long long>(r.incomplete),
                     static_cast<unsigned long long>(r.duplicates),
                     static_cast<unsigned long long>(r.reordered),
                     static_cast<unsigned long long>(r.lost),
                     static_cast<long long>(r.target_delay_ns / 1000),
                     static_cast<long long>(r.jitter_ns / 1000),
                     static_cast<unsigned long long>(r.bytes_sent),
                     static_cast<unsigned long long>(r.bytes_received));
    }
    std::fclose(file);
}

int ParseArgs(int argc, char** argv, bool* quick) {
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--artifacts" && i + 1 < argc) {
            g_artifacts = argv[++i];
        } else if (arg == "--quick") {
            *quick = true;
        } else {
            std::fprintf(stderr, "unknown argument: %s\n", arg.c_str());
            return 1;
        }
    }
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    bool quick = false;
    if (ParseArgs(argc, argv, &quick) != 0) {
        return 1;
    }
    if (g_artifacts.empty()) {
        g_artifacts = "artifacts";
    }
    std::error_code directory_error;
    std::filesystem::create_directories(g_artifacts, directory_error);
    std::printf("[media_e2e] artifacts = %s\n", g_artifacts.c_str());
    const bool sha_ok =
        Sha256Hex("abc", 3) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";
    std::printf("[media_e2e] sha256 self-test = %s\n", sha_ok ? "OK" : "FAIL");

    RunX11Scene();
    RunRawBgraStrictScene();
    RunDelta1000Scene(quick ? 100 : 1000);
    RunWeakNetDeltaScene();
    RunWeakNetRawScene();
    RunDegradationScene();
    RunLatencyScene(quick ? 60 : 200);

    WriteReport("/tmp/media_e2e_data.md");

    bool all_ok = sha_ok;
    for (const SceneResult& r : g_results) {
        std::printf("[scene] %-16s ok=%d sent=%llu delivered=%llu correct=%llu wrong=%llu "
                    "p50=%.3fms p99=%.3fms fps=%.1f %s\n",
                    r.name.c_str(), r.ok ? 1 : 0,
                    static_cast<unsigned long long>(r.sent),
                    static_cast<unsigned long long>(r.delivered),
                    static_cast<unsigned long long>(r.correct),
                    static_cast<unsigned long long>(r.wrong), r.p50_ms, r.p99_ms, r.fps,
                    r.ok ? "" : r.fail_reason.c_str());
        all_ok = all_ok && r.ok;
    }
    std::printf("[media_e2e] %s; data=/tmp/media_e2e_data.md\n", all_ok ? "PASS" : "FAIL");
    return all_ok ? 0 : 1;
}
