/**
 * @file bento_bridge.cpp
 * @brief App 帧 ↔ Bento 通用帧桥接实现。
 *
 * @author go2cpp contributors
 * @date 2026-10-31
 * @copyright MIT License
 */

#include "go2cpp/app/bento_bridge.hpp"

#include "go2cpp/bento/net/frame.hpp"

namespace go2cpp::app::bento_bridge {
namespace {

using go2cpp::bento::net::FrameDecoder;
using go2cpp::bento::net::FrameFormat;
using BentoFrame = go2cpp::bento::net::Frame;

/// Bento 通用帧的默认线上标识（"BENT"/1）；应用层帧靠 type 字段区分。
FrameFormat Format() { return go2cpp::bento::net::kDefaultFrameFormat; }

}  // namespace

const char* StatusName(Status status) noexcept {
    switch (status) {
        case Status::kOk:
            return "OK";
        case Status::kNeedMoreData:
            return "NEED_MORE_DATA";
        case Status::kBadFrame:
            return "BAD_FRAME";
        case Status::kBadFrameType:
            return "BAD_FRAME_TYPE";
        case Status::kBadPayload:
            return "BAD_PAYLOAD";
    }
    return "UNKNOWN";
}

std::vector<std::uint8_t> Encode(const Frame& frame, const Config& config) {
    std::vector<std::uint8_t> app_bytes = EncodeFrame(frame);
    if (app_bytes.empty()) {
        return {};
    }
    BentoFrame bento;
    bento.type = config.frame_type;
    bento.flags = 0;
    bento.payload = std::move(app_bytes);
    return go2cpp::bento::net::Encode(bento, Format());
}

Status Decode(const std::uint8_t* data, std::size_t size, Frame* out,
              std::size_t* consumed, const Config& config) {
    if (out == nullptr || (data == nullptr && size != 0)) {
        return Status::kBadPayload;
    }
    BentoFrame bento;
    std::size_t bento_consumed = 0;
    const auto status =
        go2cpp::bento::net::Decode(Format(), data, size, &bento, &bento_consumed);
    if (status == go2cpp::bento::net::FrameStatus::kNeedMoreData) {
        return Status::kNeedMoreData;
    }
    if (status != go2cpp::bento::net::FrameStatus::kOk) {
        return Status::kBadFrame;
    }
    if (bento.type != config.frame_type) {
        return Status::kBadFrameType;
    }
    std::size_t app_consumed = 0;
    const FrameStatus app_status = DecodeFrame(bento.payload.data(), bento.payload.size(),
                                               config.max_app_payload, out, &app_consumed);
    if (app_status != FrameStatus::kOk) {
        return Status::kBadPayload;
    }
    if (consumed != nullptr) {
        *consumed = bento_consumed;
    }
    return Status::kOk;
}

struct Decoder::Impl {
    explicit Impl(const Config& cfg)
        : config(cfg), decoder(go2cpp::bento::net::kFrameMaxPayload, Format()) {}

    Config config;         ///< 配置。
    FrameDecoder decoder;  ///< Bento 帧解码器。
};

Decoder::Decoder(const Config& config) : m_impl(std::make_unique<Impl>(config)) {}

Decoder::~Decoder() = default;

void Decoder::Append(const std::uint8_t* data, std::size_t size) {
    m_impl->decoder.Append(data, size);
}

Status Decoder::Next(Frame* out) {
    if (out == nullptr) {
        return Status::kBadPayload;
    }
    BentoFrame bento;
    const auto status = m_impl->decoder.Next(&bento);
    if (status == go2cpp::bento::net::FrameStatus::kNeedMoreData) {
        return Status::kNeedMoreData;
    }
    if (status != go2cpp::bento::net::FrameStatus::kOk) {
        return Status::kBadFrame;
    }
    if (bento.type != m_impl->config.frame_type) {
        return Status::kBadFrameType;
    }
    const FrameStatus app_status = DecodeFrame(bento.payload.data(), bento.payload.size(),
                                               m_impl->config.max_app_payload, out);
    if (app_status != FrameStatus::kOk) {
        return Status::kBadPayload;
    }
    return Status::kOk;
}

std::size_t Decoder::PendingBytes() const noexcept {
    return m_impl->decoder.PendingBytes();
}

void Decoder::Clear() noexcept { m_impl->decoder.Clear(); }

}  // namespace go2cpp::app::bento_bridge
