/**
 * @file codec.cpp
 * @brief MediaHeader 编解码、原始帧编解码器、编码器工厂与后端探测。
 * @details 原始帧后端（kRawBgra / kRawI420）是 M0 的兜底路径：即使机器上
 *          没有任何 H.264 编码器也能闭环，代价是带宽。BGRA 与 I420 的
 *          payload 就是紧凑像素数据，解码方按头部宽高重建 `VideoFrame`。
 * @author go2cpp contributors
 * @date 2026-11-01
 * @copyright MIT License
 */

#include "go2cpp/media/codec.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "codec_internal.hpp"
#include "go2cpp/media/capture.hpp"
#include "go2cpp/media/pixel.hpp"
#include "go2cpp/media/render.hpp"

namespace go2cpp::media {
namespace {

void WriteU16(std::uint8_t* out, std::uint16_t value) noexcept {
    out[0] = static_cast<std::uint8_t>(value >> 8);
    out[1] = static_cast<std::uint8_t>(value);
}

void WriteU32(std::uint8_t* out, std::uint32_t value) noexcept {
    out[0] = static_cast<std::uint8_t>(value >> 24);
    out[1] = static_cast<std::uint8_t>(value >> 16);
    out[2] = static_cast<std::uint8_t>(value >> 8);
    out[3] = static_cast<std::uint8_t>(value);
}

void WriteU64(std::uint8_t* out, std::uint64_t value) noexcept {
    for (int i = 0; i < 8; ++i) {
        out[i] = static_cast<std::uint8_t>(value >> (56 - 8 * i));
    }
}

std::uint16_t ReadU16(const std::uint8_t* in) noexcept {
    return static_cast<std::uint16_t>((static_cast<std::uint16_t>(in[0]) << 8) |
                                      static_cast<std::uint16_t>(in[1]));
}

std::uint32_t ReadU32(const std::uint8_t* in) noexcept {
    return (static_cast<std::uint32_t>(in[0]) << 24) | (static_cast<std::uint32_t>(in[1]) << 16) |
           (static_cast<std::uint32_t>(in[2]) << 8) | static_cast<std::uint32_t>(in[3]);
}

std::uint64_t ReadU64(const std::uint8_t* in) noexcept {
    std::uint64_t value = 0;
    for (int i = 0; i < 8; ++i) {
        value = (value << 8) | static_cast<std::uint64_t>(in[i]);
    }
    return value;
}

/// @brief 把任意输入帧转成紧凑 I420（输入已是 I420 时按 stride 去 padding）。
bool ToCompactI420Impl(const VideoFrame& frame, std::vector<std::uint8_t>* out, int* width,
                       int* height) {
    if (frame.width <= 0 || frame.height <= 0) {
        return false;
    }
    const std::size_t bytes = I420FrameBytes(frame.width, frame.height);
    out->resize(bytes);
    if (frame.format == PixelFormat::kI420) {
        const std::size_t stride = static_cast<std::size_t>(frame.EffectiveStride());
        if (frame.data.size() < stride * static_cast<std::size_t>(frame.height)) {
            return false;
        }
        if (stride == static_cast<std::size_t>(frame.width)) {
            std::memcpy(out->data(), frame.data.data(), bytes);
            *width = frame.width;
            *height = frame.height;
            return true;
        }
        const std::size_t chroma_w = (static_cast<std::size_t>(frame.width) + 1) / 2;
        const std::size_t chroma_h = (static_cast<std::size_t>(frame.height) + 1) / 2;
        std::uint8_t* dst_y = out->data();
        std::uint8_t* dst_u = dst_y + static_cast<std::size_t>(frame.width) * frame.height;
        std::uint8_t* dst_v = dst_u + chroma_w * chroma_h;
        for (int y = 0; y < frame.height; ++y) {
            std::memcpy(dst_y + static_cast<std::size_t>(y) * frame.width,
                        frame.data.data() + static_cast<std::size_t>(y) * stride,
                        static_cast<std::size_t>(frame.width));
        }
        const std::size_t uv_stride = (stride + 1) / 2;
        for (int y = 0; y < static_cast<int>(chroma_h); ++y) {
            std::memcpy(dst_u + static_cast<std::size_t>(y) * chroma_w,
                        frame.data.data() + frame.height * stride + static_cast<std::size_t>(y) * uv_stride,
                        chroma_w);
            std::memcpy(dst_v + static_cast<std::size_t>(y) * chroma_w,
                        frame.data.data() + frame.height * stride + chroma_h * uv_stride +
                            static_cast<std::size_t>(y) * uv_stride,
                        chroma_w);
        }
        *width = frame.width;
        *height = frame.height;
        return true;
    }
    if (!frame.Valid()) {
        return false;
    }
    BgraToI420(frame.data.data(), frame.width, frame.height, frame.EffectiveStride(), out->data());
    *width = frame.width;
    *height = frame.height;
    return true;
}

/// @brief 原始 I420 编码器。
class RawI420Encoder final : public VideoEncoder {
   public:
    explicit RawI420Encoder(int width, int height) : m_width(width), m_height(height) {}

    bool Encode(const VideoFrame& frame, std::vector<std::uint8_t>* out) override {
        std::vector<std::uint8_t> i420;
        int width = 0;
        int height = 0;
        if (!ToCompactI420Impl(frame, &i420, &width, &height)) {
            return false;
        }
        out->resize(MediaHeader::kBytes + i420.size());
        MediaHeader header;
        header.codec = CodecKind::kRawI420;
        header.keyframe = true;
        header.width = static_cast<std::uint16_t>(width);
        header.height = static_cast<std::uint16_t>(height);
        header.frame_id = frame.frame_id;
        header.payload_bytes = static_cast<std::uint32_t>(i420.size());
        header.capture_ns = static_cast<std::uint64_t>(frame.capture_ns);
        EncodeMediaHeader(header, out->data());
        std::memcpy(out->data() + MediaHeader::kBytes, i420.data(), i420.size());
        ++m_frames;
        m_bytes += out->size();
        ++m_keyframes;
        m_width = width;
        m_height = height;
        return true;
    }

    CodecKind kind() const noexcept override {
        return CodecKind::kRawI420;
    }
    const char* Name() const noexcept override {
        return "raw-i420";
    }
    void ForceKeyframe() noexcept override {}
    std::uint64_t encoded_frames() const noexcept override {
        return m_frames;
    }
    std::uint64_t encoded_bytes() const noexcept override {
        return m_bytes;
    }
    std::uint64_t keyframes() const noexcept override {
        return m_keyframes;
    }

   private:
    int m_width;
    int m_height;
    std::uint64_t m_frames = 0;
    std::uint64_t m_bytes = 0;
    std::uint64_t m_keyframes = 0;
};

/// @brief 原始 BGRA 编码器（M0 兜底：任何机器都能跑，带宽 2.6x I420）。
class RawBgraEncoder final : public VideoEncoder {
   public:
    explicit RawBgraEncoder(int width, int height) : m_width(width), m_height(height) {}

    bool Encode(const VideoFrame& frame, std::vector<std::uint8_t>* out) override {
        if (!frame.Valid()) {
            return false;
        }
        const std::size_t stride = static_cast<std::size_t>(frame.EffectiveStride());
        const std::size_t row = static_cast<std::size_t>(frame.width) * 4;
        out->resize(MediaHeader::kBytes + row * static_cast<std::size_t>(frame.height));
        MediaHeader header;
        header.codec = CodecKind::kRawBgra;
        header.keyframe = true;
        header.width = static_cast<std::uint16_t>(frame.width);
        header.height = static_cast<std::uint16_t>(frame.height);
        header.frame_id = frame.frame_id;
        header.payload_bytes = static_cast<std::uint32_t>(row * static_cast<std::size_t>(frame.height));
        header.capture_ns = static_cast<std::uint64_t>(frame.capture_ns);
        EncodeMediaHeader(header, out->data());
        std::uint8_t* dst = out->data() + MediaHeader::kBytes;
        if (stride == row) {
            std::memcpy(dst, frame.data.data(), row * static_cast<std::size_t>(frame.height));
        } else {
            for (int y = 0; y < frame.height; ++y) {
                std::memcpy(dst + static_cast<std::size_t>(y) * row,
                            frame.data.data() + static_cast<std::size_t>(y) * stride, row);
            }
        }
        ++m_frames;
        m_bytes += out->size();
        ++m_keyframes;
        m_width = frame.width;
        m_height = frame.height;
        return true;
    }

    CodecKind kind() const noexcept override {
        return CodecKind::kRawBgra;
    }
    const char* Name() const noexcept override {
        return "raw-bgra";
    }
    void ForceKeyframe() noexcept override {}
    std::uint64_t encoded_frames() const noexcept override {
        return m_frames;
    }
    std::uint64_t encoded_bytes() const noexcept override {
        return m_bytes;
    }
    std::uint64_t keyframes() const noexcept override {
        return m_keyframes;
    }

   private:
    int m_width;
    int m_height;
    std::uint64_t m_frames = 0;
    std::uint64_t m_bytes = 0;
    std::uint64_t m_keyframes = 0;
};

/// @brief 按头部重建输出帧的外壳（payload 由调用方填充）。
bool PrepareOutputFrame(const MediaHeader& header, std::size_t payload_bytes, VideoFrame* out) {
    if (header.width == 0 || header.height == 0) {
        return false;
    }
    out->width = header.width;
    out->height = header.height;
    out->stride = 0;
    out->frame_id = header.frame_id;
    out->capture_ns = static_cast<std::int64_t>(header.capture_ns);
    out->keyframe = header.keyframe;
    if (header.codec == CodecKind::kRawBgra) {
        out->format = PixelFormat::kBGRA;
        if (payload_bytes != BgraFrameBytes(header.width, header.height)) {
            return false;
        }
    } else {
        out->format = PixelFormat::kI420;
        if (payload_bytes != I420FrameBytes(header.width, header.height)) {
            return false;
        }
    }
    out->data.assign(payload_bytes, 0);
    return true;
}

/// @brief 原始 I420 解码器。
class RawI420Decoder final : public VideoDecoder {
   public:
    RawI420Decoder(int width, int height) : m_width(width), m_height(height) {}

    bool Decode(const std::uint8_t* data, std::size_t bytes, VideoFrame* out) override {
        MediaHeader header;
        if (!DecodeMediaHeader(data, bytes, &header) || header.codec != CodecKind::kRawI420) {
            ++m_dropped;
            return false;
        }
        if (!PrepareOutputFrame(header, header.payload_bytes, out)) {
            ++m_dropped;
            return false;
        }
        std::memcpy(out->data.data(), data + MediaHeader::kBytes, header.payload_bytes);
        m_width = out->width;
        m_height = out->height;
        ++m_decoded;
        return true;
    }

    CodecKind kind() const noexcept override {
        return CodecKind::kRawI420;
    }
    std::uint64_t decoded_frames() const noexcept override {
        return m_decoded;
    }
    std::uint64_t dropped_frames() const noexcept override {
        return m_dropped;
    }

   private:
    int m_width;
    int m_height;
    std::uint64_t m_decoded = 0;
    std::uint64_t m_dropped = 0;
};

/// @brief 原始 BGRA 解码器。
class RawBgraDecoder final : public VideoDecoder {
   public:
    RawBgraDecoder(int width, int height) : m_width(width), m_height(height) {}

    bool Decode(const std::uint8_t* data, std::size_t bytes, VideoFrame* out) override {
        MediaHeader header;
        if (!DecodeMediaHeader(data, bytes, &header) || header.codec != CodecKind::kRawBgra) {
            ++m_dropped;
            return false;
        }
        if (!PrepareOutputFrame(header, header.payload_bytes, out)) {
            ++m_dropped;
            return false;
        }
        std::memcpy(out->data.data(), data + MediaHeader::kBytes, header.payload_bytes);
        m_width = out->width;
        m_height = out->height;
        ++m_decoded;
        return true;
    }

    CodecKind kind() const noexcept override {
        return CodecKind::kRawBgra;
    }
    std::uint64_t decoded_frames() const noexcept override {
        return m_decoded;
    }
    std::uint64_t dropped_frames() const noexcept override {
        return m_dropped;
    }

   private:
    int m_width;
    int m_height;
    std::uint64_t m_decoded = 0;
    std::uint64_t m_dropped = 0;
};

/// @brief 执行命令并取首行输出（无 shell 注入：命令串是编译期常量）。
std::string RunCommand(const char* command) {
    std::string result;
    FILE* pipe = ::popen(command, "r");
    if (pipe == nullptr) {
        return result;
    }
    char buffer[512];
    while (std::fgets(buffer, sizeof(buffer), pipe) != nullptr) {
        result += buffer;
        if (result.size() > 8192) {
            break;
        }
    }
    ::pclose(pipe);
    return result;
}

}  // namespace

const char* CodecKindName(CodecKind kind) noexcept {
    switch (kind) {
        case CodecKind::kRawBgra:
            return "raw-bgra";
        case CodecKind::kRawI420:
            return "raw-i420";
        case CodecKind::kDeltaI420:
            return "delta-i420";
        case CodecKind::kH264:
            return "h264";
    }
    return "unknown";
}

bool ParseCodecKind(const char* text, CodecKind* out) noexcept {
    if (text == nullptr || out == nullptr) {
        return false;
    }
    if (std::strcmp(text, "delta") == 0 || std::strcmp(text, "delta-i420") == 0) {
        *out = CodecKind::kDeltaI420;
        return true;
    }
    if (std::strcmp(text, "raw") == 0 || std::strcmp(text, "raw-bgra") == 0) {
        *out = CodecKind::kRawBgra;
        return true;
    }
    if (std::strcmp(text, "raw-i420") == 0 || std::strcmp(text, "i420") == 0) {
        *out = CodecKind::kRawI420;
        return true;
    }
    if (std::strcmp(text, "h264") == 0 || std::strcmp(text, "nvenc") == 0) {
        *out = CodecKind::kH264;
        return true;
    }
    return false;
}

void EncodeMediaHeader(const MediaHeader& header, std::uint8_t* out) noexcept {
    WriteU32(out, MediaHeader::kMagic);
    out[4] = header.version;
    out[5] = static_cast<std::uint8_t>(header.codec);
    out[6] = header.keyframe ? 0x01 : 0x00;
    out[7] = 0;
    WriteU16(out + 8, header.width);
    WriteU16(out + 10, header.height);
    WriteU16(out + 12, 0);
    WriteU16(out + 14, 0);
    WriteU32(out + 16, header.frame_id);
    WriteU32(out + 20, header.payload_bytes);
    WriteU64(out + 24, header.capture_ns);
}

bool DecodeMediaHeader(const std::uint8_t* data, std::size_t bytes, MediaHeader* out) noexcept {
    if (data == nullptr || out == nullptr || bytes < MediaHeader::kBytes) {
        return false;
    }
    if (ReadU32(data) != MediaHeader::kMagic || data[4] != 1) {
        return false;
    }
    MediaHeader header;
    header.version = data[4];
    header.codec = static_cast<CodecKind>(data[5]);
    header.keyframe = (data[6] & 0x01U) != 0;
    header.width = ReadU16(data + 8);
    header.height = ReadU16(data + 10);
    header.frame_id = ReadU32(data + 16);
    header.payload_bytes = ReadU32(data + 20);
    header.capture_ns = ReadU64(data + 24);
    if (header.width == 0 || header.height == 0) {
        return false;
    }
    if (bytes < MediaHeader::kBytes + header.payload_bytes) {
        return false;
    }
    *out = header;
    return true;
}

std::unique_ptr<VideoEncoder> CreateEncoder(CodecKind kind, int width, int height) {
    switch (kind) {
        case CodecKind::kRawBgra:
            return detail::CreateRawBgraEncoder(width, height);
        case CodecKind::kRawI420:
            return detail::CreateRawI420Encoder(width, height);
        case CodecKind::kDeltaI420:
            return detail::CreateDeltaEncoder(width, height);
        case CodecKind::kH264:
            return detail::CreateH264Encoder(width, height);
    }
    return nullptr;
}

std::unique_ptr<VideoDecoder> CreateDecoder(CodecKind kind, int width, int height) {
    switch (kind) {
        case CodecKind::kRawBgra:
            return detail::CreateRawBgraDecoder(width, height);
        case CodecKind::kRawI420:
            return detail::CreateRawI420Decoder(width, height);
        case CodecKind::kDeltaI420:
            return detail::CreateDeltaDecoder(width, height);
        case CodecKind::kH264:
            return detail::CreateH264Decoder(width, height);
    }
    return nullptr;
}

namespace detail {

bool ToCompactI420(const VideoFrame& frame, std::vector<std::uint8_t>* out, int* width,
                   int* height) {
    return ToCompactI420Impl(frame, out, width, height);
}

std::unique_ptr<VideoEncoder> CreateRawI420Encoder(int width, int height) {
    return std::make_unique<RawI420Encoder>(width, height);
}

std::unique_ptr<VideoDecoder> CreateRawI420Decoder(int width, int height) {
    return std::make_unique<RawI420Decoder>(width, height);
}

std::unique_ptr<VideoEncoder> CreateRawBgraEncoder(int width, int height) {
    return std::make_unique<RawBgraEncoder>(width, height);
}

std::unique_ptr<VideoDecoder> CreateRawBgraDecoder(int width, int height) {
    return std::make_unique<RawBgraDecoder>(width, height);
}

bool FfmpegAvailable() {
    static const bool available = [] {
        const std::string output = RunCommand("ffmpeg -hide_banner -version 2>/dev/null");
        return output.find("ffmpeg version") != std::string::npos;
    }();
    return available;
}

bool NvencAvailable() {
    static const bool available = [] {
        if (!FfmpegAvailable()) {
            return false;
        }
        const std::string encoders = RunCommand("ffmpeg -hide_banner -encoders 2>/dev/null");
        if (encoders.find("h264_nvenc") == std::string::npos) {
            return false;
        }
        const std::string smi =
            RunCommand("nvidia-smi --query-gpu=name --format=csv,noheader 2>/dev/null");
        return !smi.empty() && smi.find("NVIDIA") != std::string::npos;
    }();
    return available;
}

}  // namespace detail

CodecAvailability ProbeAvailability() {
    static const CodecAvailability availability = [] {
        CodecAvailability result;
        result.h264 = detail::FfmpegAvailable();
        result.h264_nvenc = detail::NvencAvailable();
        result.x11_capture = X11Available();
        result.sdl_render = SdlAvailable();
        return result;
    }();
    return availability;
}

}  // namespace go2cpp::media
