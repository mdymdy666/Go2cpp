#pragma once

/**
 * @file pipeline.hpp
 * @brief 端到端管线：采集 → 编码 → VideoChannel 分片 → UDP → 重组 → 解码 → 渲染。
 * @details 这是 M1 的「一条命令跑完」入口，把四个可替换层串起来：
 *
 *          ```
 *          VideoSource ──BGRA──► VideoEncoder ──MediaHeader+payload──►
 *          UdpVideoSender（VideoChannel 1200B 分片，UDP 回环）──►
 *          UdpVideoReceiver（重组）──► VideoDecoder ──I420/BGRA──► FrameRenderer
 *          ```
 *
 *          延迟口径：`now(steady_clock) - frame.capture_ns`，即**采集完成时刻**到
 *          「解码完成、交付渲染」时刻的全链路耗时（含编码、分片、UDP、重组、解码）。
 *          单进程回环下不含网络 RTT 抖动，但包含真实的内核 UDP 收发与 1200B 分片。
 *
 *          关键帧策略：差分编码依赖参考帧，丢包会让后续帧无法解码，因此编码端
 *          每 `keyframe_interval` 帧强制一个关键帧（默认 30，即 1 秒 @30fps），
 *          收端最多错一整个 GOP 即可自愈。
 * @author go2cpp contributors
 * @date 2026-11-01
 * @copyright MIT License
 */

#include <cstdint>
#include <memory>
#include <string>

#include "go2cpp/media/capture.hpp"
#include "go2cpp/media/codec.hpp"
#include "go2cpp/media/render.hpp"
#include "go2cpp/media/stats.hpp"
#include "go2cpp/media/transport.hpp"

namespace go2cpp::media {

/// @brief 管线配置（收发两端共用一个结构，保证尺寸/编码器一致）。
struct MediaConfig {
    /// 采集/编码尺寸。默认 320x240：I420 关键帧 115200B（97 片）能在 WSL2 默认
    /// `net.core.rmem_max = 208KB` 下一次性塞进回环 socket（内核接收队列约
    /// 2 x 208KB，含每个 skb 的 ~768B 开销）。512x384 的关键帧 294912B（247 片）
    /// 会超过该预算而丢包——这是本机的硬限制，不是分片逻辑的问题；需要大尺寸时
    /// 必须由 root 调大 rmem_max，或改成分帧发送（见 docs/modules/webrtc-media.md）。
    int width = 320;
    int height = 240;
    int fps = 30;                        ///< 目标帧率（仅统计与 H.264 后端使用）。
    CodecKind codec = CodecKind::kDeltaI420;  ///< 编码器。
    bool use_x11 = false;                ///< true = 真实 X11 采集；false = 合成画面。
    bool render = false;                 ///< true = 创建 SDL 窗口呈现；false = 无头。
    int keyframe_interval = 30;          ///< 每 N 帧强制关键帧；<=0 表示只在第 0 帧。
    SyntheticPattern pattern = SyntheticPattern::kPatch;  ///< 合成图案。
    X11CaptureConfig x11;                ///< use_x11 时的采集参数。
};

/// @brief 采集 → 编码 → 分片的发送端。
class MediaSender {
   public:
    ~MediaSender();

    MediaSender(const MediaSender&) = delete;
    MediaSender& operator=(const MediaSender&) = delete;

    /// @brief 创建发送端（会打开 UDP socket 与采集源）。
    /// @param peer_port 接收端端口。
    static std::unique_ptr<MediaSender> Create(const MediaConfig& config, std::uint16_t peer_port,
                                               std::string* error);

    /// @brief 采集并发送一帧。
    /// @param captured 若非空，输出采集到的原始帧（供逐帧比对）。
    /// @param encoded_bytes 若非空，输出本帧编码后的字节数（含 32B 头）。
    /// @return 成功返回 true；采集失败返回 false。
    bool SendNext(VideoFrame* captured, std::size_t* encoded_bytes);

    /// @brief 本端 UDP 端口。
    std::uint16_t local_port() const noexcept;

    /// @brief 已发送帧数。
    std::uint64_t frames_sent() const noexcept {
        return m_frames_sent;
    }

    /// @brief 已发送的编码字节数。
    std::uint64_t bytes_sent() const noexcept {
        return m_bytes_sent;
    }

    /// @brief 源名字（x11 / synthetic）。
    const char* source_name() const noexcept;

    /// @brief 编码器名字。
    const char* codec_name() const noexcept;

    /// @brief 访问编码器（测试里可真接调用）。
    VideoEncoder& encoder() noexcept {
        return *m_encoder;
    }

    /// @brief 访问采集源。
    VideoSource& source() noexcept {
        return *m_source;
    }

   private:
    MediaSender() = default;

    std::unique_ptr<VideoSource> m_source;
    std::unique_ptr<VideoEncoder> m_encoder;
    std::unique_ptr<UdpVideoSender> m_transport;
    std::unique_ptr<VideoFrame> m_capture;
    std::vector<std::uint8_t> m_encoded;
    MediaConfig m_config;
    std::uint32_t m_frame_id = 0;
    std::uint64_t m_frames_sent = 0;
    std::uint64_t m_bytes_sent = 0;
};

/// @brief 重组 → 解码 → 渲染的接收端。
class MediaReceiver {
   public:
    ~MediaReceiver();

    MediaReceiver(const MediaReceiver&) = delete;
    MediaReceiver& operator=(const MediaReceiver&) = delete;

    /// @brief 创建接收端（绑定本地 UDP 端口）。
    static std::unique_ptr<MediaReceiver> Create(const MediaConfig& config, std::string* error);

    /// @brief 泵一次：收包 → 重组 → 解码 → 渲染，直到超时或 64 个数据报。
    /// @return 本次解码成功并交付渲染的帧数。
    int Pump(int timeout_ms);

    /// @brief 本地 UDP 端口。
    std::uint16_t local_port() const noexcept;

    /// @brief 解码成功帧数。
    std::uint64_t frames_delivered() const noexcept {
        return m_delivered;
    }

    /// @brief 解码失败/丢参考帧的帧数。
    std::uint64_t frames_dropped() const noexcept {
        return m_dropped;
    }

    /// @brief 端到端延迟统计（毫秒）。
    const LatencyRecorder& latency() const noexcept {
        return m_latency;
    }

    /// @brief 最近一帧的解码结果（供逐帧校验）。
    const VideoFrame& last_frame() const noexcept {
        return m_last_frame;
    }

    /// @brief 渲染器名字（headless / sdl）。
    const char* renderer_name() const noexcept;

    /// @brief 编码器名字。
    const char* codec_name() const noexcept;

    /// @brief 收到的编码字节数。
    std::uint64_t bytes_received() const noexcept {
        return m_bytes_received;
    }

    /// @brief 收端分片/重组统计。
    const go2cpp::bento::VideoChannel::Stats& transport_stats() const noexcept;

   private:
    MediaReceiver() = default;

    /// @brief 一帧重组完成后的处理。
    void OnEncodedFrame(std::uint32_t frame_id, bool key, const std::uint8_t* data,
                        std::size_t bytes);

    std::unique_ptr<UdpVideoReceiver> m_transport;
    std::unique_ptr<VideoDecoder> m_decoder;
    std::unique_ptr<FrameRenderer> m_renderer;
    VideoFrame m_last_frame;
    LatencyRecorder m_latency;
    MediaConfig m_config;
    std::uint64_t m_delivered = 0;
    std::uint64_t m_dropped = 0;
    std::uint64_t m_bytes_received = 0;
};

}  // namespace go2cpp::media
