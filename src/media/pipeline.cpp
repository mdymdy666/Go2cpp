/**
 * @file pipeline.cpp
 * @brief 端到端管线实现：采集 → 编码 → 分片 → UDP → 重组 → 解码 → 渲染。
 * @author go2cpp contributors
 * @date 2026-11-01
 * @copyright MIT License
 */

#include "go2cpp/media/pipeline.hpp"

#include <chrono>
#include <cstring>

namespace go2cpp::media {
namespace {

/// @brief 单调纳秒时钟（与采集帧同一时基，用于端到端延迟）。
std::int64_t NowNs() noexcept {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

/// @brief 单调毫秒时钟（驱动重组超时）。
std::uint64_t NowMs() noexcept {
    return static_cast<std::uint64_t>(NowNs() / 1000000);
}

}  // namespace

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
    if (m_config.keyframe_interval > 0 && m_frame_id > 1 &&
        (m_frame_id % static_cast<std::uint32_t>(m_config.keyframe_interval)) == 0) {
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

std::uint16_t MediaSender::local_port() const noexcept {
    return m_transport ? m_transport->local_port() : 0;
}

const char* MediaSender::source_name() const noexcept {
    return m_source ? m_source->name() : "none";
}

const char* MediaSender::codec_name() const noexcept {
    return m_encoder ? m_encoder->Name() : "none";
}

MediaReceiver::~MediaReceiver() = default;

std::unique_ptr<MediaReceiver> MediaReceiver::Create(const MediaConfig& config, std::string* error) {
    auto receiver = std::unique_ptr<MediaReceiver>(new MediaReceiver());
    receiver->m_config = config;
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
    return m_transport->Pump(timeout_ms, 128);
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
    (void)frame_id;
    (void)key;
    m_bytes_received += bytes;
    if (!m_decoder->Decode(data, bytes, &m_last_frame)) {
        ++m_dropped;
        return;
    }
    const std::int64_t now = NowNs();
    const std::int64_t captured = m_last_frame.capture_ns;
    if (captured > 0 && now >= captured) {
        m_latency.Add(static_cast<double>(now - captured) / 1000000.0);
    }
    if (m_renderer && !m_renderer->Present(m_last_frame)) {
        ++m_dropped;
        return;
    }
    ++m_delivered;
    m_transport->OnTimer(NowMs());
}

}  // namespace go2cpp::media
