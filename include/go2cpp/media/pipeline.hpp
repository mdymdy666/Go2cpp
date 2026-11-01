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
 *
 *          ## 弱网收尾（jitter / degradation）
 *          默认全部关闭，公开行为与既有一致。打开后按
 *          `docs/modules/jitter-degradation.md` §5 接线：
 *
 *          - **收端 jitter**：`UdpVideoReceiver` 重组出的整帧先按
 *            `jitter_chunk_bytes` 切成小块，带 12B 块头（frame_id/chunk/total）
 *            进入 `JitterBuffer`；`Pop` 按序拼回整帧后再解码渲染。因为
 *            `JitterBuffer` 单包负载上限是 64KB，而关键帧可达 115KB，所以
 *            这里做的是「帧内分块 + 包级重排」，语义上仍是按包序交付。
 *          - **发送端 degradation**：`MediaSender::OnNetworkSample` 喂入带宽/丢包
 *            采样，`DegradationController` 决定档位；档位变化时重建采集源与
 *            编码器（320x240 → 160x120、30fps → 15fps），并对新编码器强制关键帧。
 *          - **收端 degradation**：`ShouldDeliverFrame` 做帧率门控（降帧率档
 *            跳帧渲染），统计通过/跳过帧数。
 * @author go2cpp contributors
 * @date 2026-11-01
 * @copyright MIT License
 */

#include <cstdint>
#include <memory>
#include <string>

#include "go2cpp/media/capture.hpp"
#include "go2cpp/media/codec.hpp"
#include "go2cpp/media/degradation.hpp"
#include "go2cpp/media/jitter_buffer.hpp"
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

    // ---- 弱网收尾：默认关闭，打开后才有额外排队/重建开销 -------------------
    /// @brief true = 收端启用 JitterBuffer（§5 接线）。
    bool jitter = false;
    /// @brief jitter 的帧内分块大小（字节，会被 clamp 到 [256, 60000]）。
    int jitter_chunk_bytes = 8192;
    /// @brief jitter 深度/容量配置（`max_payload_bytes` 会自动放大到块头 + 块大小）。
    JitterBufferConfig jitter_config;

    /// @brief true = 启用降级控制器（发送端可重建尺寸、收端做帧率门控）。
    bool degradation = false;
    /// @brief 降级阶梯与滞回参数。
    DegradationConfig degradation_config;
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
    /// @note 启用降级后，被帧率门控跳过的帧也返回 true，但 `encoded_bytes` 为 0、
    ///       不产生任何 UDP 包（`frames_skipped()` 计数）。
    bool SendNext(VideoFrame* captured, std::size_t* encoded_bytes);

    /// @brief 喂一次网络采样（仅启用 degradation 时生效）。
    /// @details 档位变化会重建采集源/编码器（尺寸随档位），并对新编码器强制关键帧。
    /// @return 档位变化事件；未变化时 `changed == false`。
    DegradationEvent OnNetworkSample(const NetworkSample& sample);

    /// @brief 降级控制器（未启用时为 nullptr）。
    const DegradationController* degradation() const noexcept {
        return m_degradation.get();
    }

    /// @brief 被帧率门控跳过的帧数。
    std::uint64_t frames_skipped() const noexcept {
        return m_frames_skipped;
    }

    /// @brief 当前采集宽/高（降级会改变）。
    int current_width() const noexcept;
    int current_height() const noexcept;

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

    /// @brief 按当前档位重建采集源与编码器（尺寸/帧率变化时）。
    void RebuildForLevel();

    std::unique_ptr<VideoSource> m_source;
    std::unique_ptr<VideoEncoder> m_encoder;
    std::unique_ptr<UdpVideoSender> m_transport;
    std::unique_ptr<VideoFrame> m_capture;
    std::unique_ptr<DegradationController> m_degradation;
    std::vector<std::uint8_t> m_encoded;
    MediaConfig m_config;
    std::uint32_t m_frame_id = 0;
    std::uint64_t m_frames_sent = 0;
    std::uint64_t m_bytes_sent = 0;
    std::uint64_t m_frames_skipped = 0;
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

    /// @brief 喂一次网络采样（仅启用 degradation 时生效），用于帧率门控与统计。
    DegradationEvent OnNetworkSample(const NetworkSample& sample);

    /// @brief 降级控制器（未启用时为 nullptr）。
    const DegradationController* degradation() const noexcept {
        return m_degradation.get();
    }

    /// @brief 收端 jitter 统计快照（未启用时全 0）。
    JitterStats jitter_stats() const noexcept;

    /// @brief jitter 因缺块/乱序无法拼回整帧而丢弃的帧数。
    std::uint64_t jitter_frames_dropped() const noexcept {
        return m_jitter_frames_dropped;
    }

    /// @brief 被收端帧率门控跳过的帧数。
    std::uint64_t frames_skipped() const noexcept {
        return m_frames_skipped;
    }

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

    /// @brief 解码一帧并交付渲染（jitter 拼帧完成后也走这里）。
    void DecodeAndPresent(const std::uint8_t* data, std::size_t bytes);

    /// @brief 把整帧切成小块压入 JitterBuffer。
    void PushToJitter(std::uint32_t frame_id, const std::uint8_t* data, std::size_t bytes);

    /// @brief 从 JitterBuffer 按序取出到期块并拼帧。
    void DrainJitter(std::int64_t now_ns);

    /// @brief 处理一个按序交付的块。
    void OnJitterChunk(const JitterPacketView& view);

    /// @brief JitterBuffer 判定一个块丢失：当前拼帧作废。
    void OnJitterGap();

    std::unique_ptr<UdpVideoReceiver> m_transport;
    std::unique_ptr<VideoDecoder> m_decoder;
    std::unique_ptr<FrameRenderer> m_renderer;
    std::unique_ptr<JitterBuffer> m_jitter;
    std::unique_ptr<DegradationController> m_degradation;
    VideoFrame m_last_frame;
    LatencyRecorder m_latency;
    MediaConfig m_config;
    std::uint64_t m_delivered = 0;
    std::uint64_t m_dropped = 0;
    std::uint64_t m_bytes_received = 0;
    std::uint64_t m_frames_skipped = 0;

    // ---- jitter 分块状态（仅在 m_jitter 非空时使用）----
    std::uint32_t m_jitter_seq = 0;            ///< 包序号（块级，单调递增）。
    std::vector<std::uint8_t> m_jitter_packet; ///< 发送到 JitterBuffer 的块缓冲。
    std::vector<std::uint8_t> m_jitter_frame;  ///< 正在拼接的整帧。
    std::uint32_t m_jitter_frame_id = 0;       ///< 正在拼接的帧号。
    std::uint32_t m_jitter_frame_bytes = 0;    ///< 整帧总字节数。
    std::uint16_t m_jitter_next_chunk = 0;     ///< 期望的下一块下标。
    std::uint16_t m_jitter_chunk_count = 0;    ///< 本帧块数。
    std::uint64_t m_jitter_frames_dropped = 0; ///< 缺块导致的整帧丢弃数。
};

}  // namespace go2cpp::media
