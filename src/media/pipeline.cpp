/**
 * @file pipeline.cpp
 * @brief 端到端管线实现：采集 → 编码 → 分片 → UDP → 重组 → 解码 → 渲染。
 * @details 弱网收尾按 `docs/modules/jitter-degradation.md` §5 接线：
 *
 *          ```
 *          UdpVideoReceiver 重组出整帧
 *             └─ PushToJitter（整帧切 8KB 块 + 12B 块头 → JitterBuffer.Push）
 *                  └─ DrainJitter（Pop 按块序 → 拼回整帧 → DecodeAndPresent）
 *          NetworkSample ─► DegradationController.Update
 *                  ├─ 发送端：档位变化 → 重建采集源/编码器（尺寸/帧率）
 *                  └─ 收端：ShouldDeliverFrame 做帧率门控
 *          ```
 *
 *          默认关闭（`MediaConfig::jitter/degradation`），既有调用与测试的行为不变。
 * @author go2cpp contributors
 * @date 2026-11-01
 * @copyright MIT License
 */

#include "go2cpp/media/pipeline.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <vector>

namespace go2cpp::media {
namespace {

/// @brief 单调纳秒时钟（与采集帧同一时基，用于端到端延迟）。
std::int64_t NowNs() noexcept {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

/// @brief 单调毫秒时钟（驱动重组超时与降级滞回）。
std::uint64_t NowMs() noexcept {
    return static_cast<std::uint64_t>(NowNs() / 1000000);
}

/// @brief jitter 块头字节数：frame_id u32 | chunk_index u16 | chunk_count u16 | total u32。
constexpr std::size_t kJitterChunkHeaderBytes = 12;

/// @brief 块大小下限/上限（上限受 JitterBuffer 64KB 的单包上限约束）。
constexpr int kJitterChunkMinBytes = 256;
constexpr int kJitterChunkMaxBytes = 60000;

/// @brief 默认 jitter 环容量：8KB 块 × 1024 = 8MB，约可容纳 68 个 320x240 关键帧。
constexpr std::uint32_t kJitterMinCapacity = 1024;

void PutU16(std::uint8_t* out, std::uint16_t value) noexcept {
    out[0] = static_cast<std::uint8_t>(value >> 8);
    out[1] = static_cast<std::uint8_t>(value);
}

void PutU32(std::uint8_t* out, std::uint32_t value) noexcept {
    out[0] = static_cast<std::uint8_t>(value >> 24);
    out[1] = static_cast<std::uint8_t>(value >> 16);
    out[2] = static_cast<std::uint8_t>(value >> 8);
    out[3] = static_cast<std::uint8_t>(value);
}

std::uint16_t GetU16(const std::uint8_t* in) noexcept {
    return static_cast<std::uint16_t>((static_cast<std::uint16_t>(in[0]) << 8) |
                                      static_cast<std::uint16_t>(in[1]));
}

std::uint32_t GetU32(const std::uint8_t* in) noexcept {
    return (static_cast<std::uint32_t>(in[0]) << 24) |
           (static_cast<std::uint32_t>(in[1]) << 16) |
           (static_cast<std::uint32_t>(in[2]) << 8) | static_cast<std::uint32_t>(in[3]);
}

/// @brief 纳秒 → 90kHz 媒体时间戳（32 位回绕由 int32 差值处理）。
std::uint32_t CaptureNsToTimestamp(std::int64_t capture_ns) noexcept {
    if (capture_ns <= 0) {
        return 0;
    }
    return static_cast<std::uint32_t>((capture_ns / 1000) * 9 / 100);
}

/// @brief 把用户配置的块大小 clamp 到 JitterBuffer 单包上限以内。
int NormalizeChunkBytes(int value) noexcept {
    if (value < kJitterChunkMinBytes) {
        return kJitterChunkMinBytes;
    }
    if (value > kJitterChunkMaxBytes) {
        return kJitterChunkMaxBytes;
    }
    return value;
}

}  // namespace

// ===========================================================================
// MediaSender
// ===========================================================================

MediaSender::~MediaSender() = default;

std::unique_ptr<MediaSender> MediaSender::Create(const MediaConfig& config,
                                                 std::uint16_t peer_port, std::string* error) {
    auto sender = std::unique_ptr<MediaSender>(new MediaSender());
    sender->m_config = config;
    if (config.use_x11) {
        X11CaptureConfig x11 = config.x11;
        if (x11.width <= 0) {
            x11.width = config.width;
        }
        if (x11.height <= 0) {
            x11.height = config.height;
        }
        sender->m_source = CreateX11Capture(x11, error);
    } else {
        sender->m_source = std::make_unique<SyntheticCapture>(config.width, config.height,
                                                              config.pattern);
    }
    if (!sender->m_source) {
        if (error != nullptr && error->empty()) {
            *error = "no capture source available";
        }
        return nullptr;
    }
    sender->m_encoder = CreateEncoder(config.codec, sender->m_source->width(),
                                      sender->m_source->height());
    if (!sender->m_encoder) {
        if (error != nullptr) {
            *error = std::string("no encoder available for codec ") + CodecKindName(config.codec);
        }
        return nullptr;
    }
    sender->m_transport = std::make_unique<UdpVideoSender>();
    if (!sender->m_transport->Open(peer_port, error)) {
        return nullptr;
    }
    sender->m_capture = std::make_unique<VideoFrame>();
    if (config.degradation) {
        sender->m_degradation = std::make_unique<DegradationController>(config.degradation_config);
    }
    return sender;
}

bool MediaSender::SendNext(VideoFrame* captured, std::size_t* encoded_bytes) {
    if (!m_source || !m_encoder || !m_transport) {
        return false;
    }
    if (!m_source->Capture(m_capture.get())) {
        return false;
    }
    // VideoChannel 把 frame_id <= 已交付帧号视为 stale（初始已交付 = 0），
    // 因此首帧必须从 1 开始，不能用 0。
    m_capture->frame_id = ++m_frame_id;
    m_capture->capture_ns = NowNs();
    // 降级帧率门控：采集照常推进（画面/时间戳连续），但不编码、不发送。
    if (m_degradation != nullptr && !m_degradation->ShouldDeliverFrame(static_cast<std::int64_t>(NowMs()))) {
        ++m_frames_skipped;
        if (captured != nullptr) {
            *captured = *m_capture;
        }
        if (encoded_bytes != nullptr) {
            *encoded_bytes = 0;
        }
        return true;
    }
    if (m_config.keyframe_interval > 0 && m_frame_id > 1 &&
        (m_frame_id % static_cast<std::uint32_t>(m_config.keyframe_interval)) == 0) {
        m_encoder->ForceKeyframe();
    }
    if (m_degradation != nullptr && m_degradation->ConsumeKeyframeRequest()) {
        // 重连/切档后重建参考链：让收端从下一个关键帧开始恢复。
        m_encoder->ForceKeyframe();
    }
    if (!m_encoder->Encode(*m_capture, &m_encoded)) {
        return false;
    }
    if (m_encoded.size() < MediaHeader::kBytes) {
        return false;
    }
    const bool key = m_encoded[6] != 0;
    if (!m_transport->SendFrame(m_capture->frame_id, key, m_encoded.data(), m_encoded.size())) {
        return false;
    }
    ++m_frames_sent;
    m_bytes_sent += m_encoded.size();
    if (captured != nullptr) {
        *captured = *m_capture;
    }
    if (encoded_bytes != nullptr) {
        *encoded_bytes = m_encoded.size();
    }
    return true;
}

DegradationEvent MediaSender::OnNetworkSample(const NetworkSample& sample) {
    if (m_degradation == nullptr) {
        return DegradationEvent{};
    }
    const DegradationEvent event =
        m_degradation->Update(static_cast<std::int64_t>(NowMs()), sample);
    if (event.changed) {
        RebuildForLevel();
    }
    return event;
}

void MediaSender::RebuildForLevel() {
    if (m_degradation == nullptr) {
        return;
    }
    const int width = m_degradation->target_width();
    const int height = m_degradation->target_height();
    if (width == m_source->width() && height == m_source->height()) {
        return;
    }
    std::string error;
    std::unique_ptr<VideoSource> source;
    if (m_config.use_x11) {
        X11CaptureConfig x11 = m_config.x11;
        x11.width = width;
        x11.height = height;
        source = CreateX11Capture(x11, &error);
    } else {
        source = std::make_unique<SyntheticCapture>(width, height, m_config.pattern);
    }
    if (!source) {
        return;  // 重建失败：保留旧源，宁可尺寸不变也不中断。
    }
    auto encoder = CreateEncoder(m_config.codec, width, height);
    if (!encoder) {
        return;
    }
    encoder->ForceKeyframe();  // 新尺寸的第一帧必须是关键帧，收端参考链才能重建。
    m_source = std::move(source);
    m_encoder = std::move(encoder);
    m_config.width = width;
    m_config.height = height;
    m_config.x11.width = width;
    m_config.x11.height = height;
}

int MediaSender::current_width() const noexcept {
    return m_source ? m_source->width() : 0;
}

int MediaSender::current_height() const noexcept {
    return m_source ? m_source->height() : 0;
}

std::uint16_t MediaSender::local_port() const noexcept {
    return m_transport ? m_transport->local_port() : 0;
}

const char* MediaSender::source_name() const noexcept {
    return m_source ? m_source->name() : "none";
}

const char* MediaSender::codec_name() const noexcept {
    return m_encoder ? m_encoder->Name() : "none";
}

// ===========================================================================
// MediaReceiver
// ===========================================================================

MediaReceiver::~MediaReceiver() = default;

std::unique_ptr<MediaReceiver> MediaReceiver::Create(const MediaConfig& config, std::string* error) {
    auto receiver = std::unique_ptr<MediaReceiver>(new MediaReceiver());
    receiver->m_config = config;
    if (config.jitter) {
        const int chunk = NormalizeChunkBytes(config.jitter_chunk_bytes);
        receiver->m_config.jitter_chunk_bytes = chunk;
        JitterBufferConfig jitter_config = config.jitter_config;
        jitter_config.max_payload_bytes =
            std::max<std::size_t>(jitter_config.max_payload_bytes,
                                  static_cast<std::size_t>(chunk) + kJitterChunkHeaderBytes);
        jitter_config.timestamp_hz = 90000;
        // 首帧不额外攒满一个 target_delay：媒体链路首帧延迟已经由 jitter 深度承担，
        // 启动期再等一轮会让「首帧延迟」翻倍，测试里也不好区分。
        jitter_config.startup_buffering = false;
        jitter_config.capacity = std::max<std::uint32_t>(jitter_config.capacity, kJitterMinCapacity);
        receiver->m_jitter = std::make_unique<JitterBuffer>(jitter_config);
        receiver->m_jitter_packet.resize(static_cast<std::size_t>(chunk) + kJitterChunkHeaderBytes);
    }
    if (config.degradation) {
        receiver->m_degradation = std::make_unique<DegradationController>(config.degradation_config);
    }
    receiver->m_transport = std::make_unique<UdpVideoReceiver>(
        [raw = receiver.get()](std::uint32_t frame_id, bool key, const std::uint8_t* data,
                               std::size_t bytes) {
            raw->OnEncodedFrame(frame_id, key, data, bytes);
        });
    if (!receiver->m_transport->Open(error)) {
        return nullptr;
    }
    receiver->m_decoder = CreateDecoder(config.codec, config.width, config.height);
    if (!receiver->m_decoder) {
        if (error != nullptr) {
            *error = std::string("no decoder available for codec ") + CodecKindName(config.codec);
        }
        return nullptr;
    }
    if (config.render) {
        receiver->m_renderer = CreateSdlRenderer(config.width, config.height, false, error);
    }
    if (!receiver->m_renderer) {
        // 窗口不可用（无 DISPLAY / SDL 初始化失败）不静默：错误已写出，
        // 但保留无头渲染器让管线仍可验证像素正确性。
        receiver->m_renderer = CreateHeadlessRenderer();
    }
    return receiver;
}

int MediaReceiver::Pump(int timeout_ms) {
    if (!m_transport) {
        return 0;
    }
    const int delivered = m_transport->Pump(timeout_ms, 128);
    if (m_jitter != nullptr) {
        DrainJitter(NowNs());
    }
    m_transport->OnTimer(NowMs());
    return delivered;
}

DegradationEvent MediaReceiver::OnNetworkSample(const NetworkSample& sample) {
    if (m_degradation == nullptr) {
        return DegradationEvent{};
    }
    return m_degradation->Update(static_cast<std::int64_t>(NowMs()), sample);
}

JitterStats MediaReceiver::jitter_stats() const noexcept {
    return m_jitter ? m_jitter->Snapshot() : JitterStats{};
}

std::uint16_t MediaReceiver::local_port() const noexcept {
    return m_transport ? m_transport->local_port() : 0;
}

const char* MediaReceiver::renderer_name() const noexcept {
    return m_renderer ? m_renderer->name() : "none";
}

const char* MediaReceiver::codec_name() const noexcept {
    return m_decoder ? CodecKindName(m_decoder->kind()) : "none";
}

const go2cpp::bento::VideoChannel::Stats& MediaReceiver::transport_stats() const noexcept {
    return m_transport->stats();
}

void MediaReceiver::OnEncodedFrame(std::uint32_t frame_id, bool key, const std::uint8_t* data,
                                   std::size_t bytes) {
    (void)key;
    m_bytes_received += bytes;
    if (m_jitter != nullptr) {
        PushToJitter(frame_id, data, bytes);
        DrainJitter(NowNs());
        return;
    }
    DecodeAndPresent(data, bytes);
}

void MediaReceiver::DecodeAndPresent(const std::uint8_t* data, std::size_t bytes) {
    if (!m_decoder || !m_renderer) {
        return;
    }
    if (!m_decoder->Decode(data, bytes, &m_last_frame)) {
        ++m_dropped;
        return;
    }
    if (m_degradation != nullptr &&
        !m_degradation->ShouldDeliverFrame(static_cast<std::int64_t>(NowMs()))) {
        // 降帧率档：解码照常（保持参考链），但不渲染、不记端到端延迟。
        ++m_frames_skipped;
        return;
    }
    const std::int64_t now = NowNs();
    const std::int64_t captured = m_last_frame.capture_ns;
    if (captured > 0 && now >= captured) {
        m_latency.Add(static_cast<double>(now - captured) / 1000000.0);
    }
    if (!m_renderer->Present(m_last_frame)) {
        ++m_dropped;
        return;
    }
    ++m_delivered;
}

void MediaReceiver::PushToJitter(std::uint32_t frame_id, const std::uint8_t* data,
                                 std::size_t bytes) {
    if (m_jitter == nullptr || data == nullptr || bytes == 0) {
        ++m_dropped;
        return;
    }
    const std::size_t chunk = static_cast<std::size_t>(m_config.jitter_chunk_bytes);
    const std::size_t chunk_count = (bytes + chunk - 1) / chunk;
    if (chunk_count == 0 || chunk_count > 0xFFFFU) {
        ++m_dropped;
        return;
    }
    // 时间戳取采集时刻（90kHz）：同一帧所有块共享，帧间差值才是媒体时间差；
    // 若用块序号当时间戳，帧间 33ms 的正常间隔会被误判成抖动。
    std::uint32_t timestamp = CaptureNsToTimestamp(NowNs());
    MediaHeader header;
    if (bytes >= MediaHeader::kBytes && DecodeMediaHeader(data, bytes, &header)) {
        if (header.capture_ns != 0) {
            timestamp = CaptureNsToTimestamp(static_cast<std::int64_t>(header.capture_ns));
        }
    }
    const std::int64_t arrival = NowNs();
    for (std::size_t index = 0; index < chunk_count; ++index) {
        const std::size_t offset = index * chunk;
        const std::size_t length = std::min(chunk, bytes - offset);
        PutU32(m_jitter_packet.data(), frame_id);
        PutU16(m_jitter_packet.data() + 4, static_cast<std::uint16_t>(index));
        PutU16(m_jitter_packet.data() + 6, static_cast<std::uint16_t>(chunk_count));
        PutU32(m_jitter_packet.data() + 8, static_cast<std::uint32_t>(bytes));
        std::memcpy(m_jitter_packet.data() + kJitterChunkHeaderBytes, data + offset, length);
        ++m_jitter_seq;
        const PushResult result =
            m_jitter->Push(m_jitter_seq, timestamp, m_jitter_packet.data(),
                           kJitterChunkHeaderBytes + length, arrival);
        if (result == PushResult::kDroppedInvalid) {
            ++m_jitter_frames_dropped;
        }
    }
}

void MediaReceiver::DrainJitter(std::int64_t now_ns) {
    if (m_jitter == nullptr) {
        return;
    }
    // 一次最多吐 4096 个块（64MB 帧上限的 4 倍）：防止异常输入把 Pump 卡死。
    for (std::uint32_t spins = 0; spins < 4096; ++spins) {
        JitterPacketView view;
        const PopResult result = m_jitter->Pop(&view, now_ns);
        if (result == PopResult::kOk || result == PopResult::kLateDelivered) {
            OnJitterChunk(view);
            continue;
        }
        if (result == PopResult::kGapSkipped) {
            OnJitterGap();
            continue;
        }
        break;
    }
}

void MediaReceiver::OnJitterChunk(const JitterPacketView& view) {
    if (view.bytes < kJitterChunkHeaderBytes) {
        ++m_jitter_frames_dropped;
        return;
    }
    const std::uint32_t frame_id = GetU32(view.data);
    const std::uint16_t index = GetU16(view.data + 4);
    const std::uint16_t count = GetU16(view.data + 6);
    const std::uint32_t total = GetU32(view.data + 8);
    if (count == 0 || index >= count || total == 0) {
        ++m_jitter_frames_dropped;
        return;
    }
    if (index == 0) {
        if (!m_jitter_frame.empty() && m_jitter_next_chunk != 0) {
            ++m_jitter_frames_dropped;  // 上一帧没拼完就来了新帧头。
        }
        m_jitter_frame.clear();
        m_jitter_frame_id = frame_id;
        m_jitter_frame_bytes = total;
        m_jitter_chunk_count = count;
        m_jitter_next_chunk = 0;
        m_jitter_frame.reserve(total);
    } else if (m_jitter_frame.empty() || frame_id != m_jitter_frame_id ||
               index != m_jitter_next_chunk) {
        // 缺块/乱序跨帧：这一帧已无法还原，丢弃并等下一个帧头（index == 0）。
        if (!m_jitter_frame.empty()) {
            ++m_jitter_frames_dropped;
        }
        m_jitter_frame.clear();
        m_jitter_frame_id = 0;
        m_jitter_next_chunk = 0;
        m_jitter_chunk_count = 0;
        m_jitter_frame_bytes = 0;
        return;
    }
    m_jitter_frame.insert(m_jitter_frame.end(), view.data + kJitterChunkHeaderBytes,
                          view.data + view.bytes);
    ++m_jitter_next_chunk;
    if (m_jitter_next_chunk == m_jitter_chunk_count) {
        if (m_jitter_frame.size() == m_jitter_frame_bytes) {
            DecodeAndPresent(m_jitter_frame.data(), m_jitter_frame.size());
        } else {
            ++m_jitter_frames_dropped;
        }
        m_jitter_frame.clear();
        m_jitter_frame_id = 0;
        m_jitter_next_chunk = 0;
        m_jitter_chunk_count = 0;
        m_jitter_frame_bytes = 0;
    }
}

void MediaReceiver::OnJitterGap() {
    if (!m_jitter_frame.empty()) {
        ++m_jitter_frames_dropped;
    }
    m_jitter_frame.clear();
    m_jitter_frame_id = 0;
    m_jitter_next_chunk = 0;
    m_jitter_chunk_count = 0;
    m_jitter_frame_bytes = 0;
}

}  // namespace go2cpp::media
