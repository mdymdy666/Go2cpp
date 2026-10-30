#pragma once

/**
 * @file frame_codec.hpp
 * @brief Reflect 消息与 Bento 通用帧（`bento::net::Frame`）的对接。
 *
 * @details Bento 帧负责「字节流定界 + CRC32 校验」，Reflect 负责「结构体
 *          编解码 + 类型路由」。两者组合后的发送路径：
 *
 * @code
 *   const auto frame = ReflectFrameCodec::MakeFrame(msg, kMsgLogin);
 *   const auto bytes = net::Encode(frame);   // 20B 帧头 + CRC + payload
 *   transport.Send(bytes);
 * @endcode
 *
 * 接收路径：
 * @code
 *   FrameDecoder decoder;
 *   decoder.Append(received, n);
 *   net::Frame frame;
 *   while (decoder.Next(&frame) == net::FrameStatus::kOk) {
 *       LoginRequest request;
 *       if (ReflectFrameCodec::DecodeFrame(frame, request) == CodecStatus::kOk) {
 *           Handle(request);
 *       }
 *   }
 * @endcode
 *
 * @note 本头文件依赖 `go2cpp/bento/net/frame.hpp`；不把帧对接放进 codec.hpp，
 *       是为了让 Reflect 核心库能在没有 Bento 的构建里独立使用（只需要字符串
 *       和结构体序列化时不必拉入网络帧层）。
 *
 * @author go2cpp contributors
 * @date 2026-10-30
 * @copyright MIT License
 */

#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

#include "go2cpp/bento/net/frame.hpp"
#include "go2cpp/reflect/codec.hpp"

namespace go2cpp::reflect {

namespace net = ::go2cpp::bento::net;

/**
 * @brief 把 Reflect 消息装进 Bento 帧 / 从帧里解出消息。
 *
 * @details 帧头字段 `type` / `flags` 由调用方决定语义（Reflect 不解释），
 *          便于上层把「消息类型」「压缩标志」等路由信息放在帧层。
 */
class ReflectFrameCodec {
   public:
    /**
     * @brief 编码消息并放入帧载荷（不计算 CRC，调用 `net::Encode` 生成线上字节）。
     * @param value 待编码值。
     * @param type 帧类型字段（上层消息类型）。
     * @param encoding 编码模式。
     * @param compression 压缩算法。
     * @param flags 帧标志位。
     * @return 帧；编码失败时 payload 为空（`net::Encode` 仍会生成帧，解码端会拒绝）。
     */
    template <typename T>
    static net::Frame MakeFrame(const T& value, std::uint16_t type,
                                Encoding encoding = Encoding::kBinary,
                                Compression compression = Compression::kNone,
                                std::uint16_t flags = 0) {
        net::Frame frame;
        frame.type = type;
        frame.flags = flags;
        std::vector<std::uint8_t> payload;
        if (ReflectCodec::Encode(value, payload, encoding, compression) == CodecStatus::kOk) {
            frame.payload = std::move(payload);
        }
        return frame;
    }

    /// @brief 编码消息并直接生成完整线上帧字节（含 20B 头与 CRC）。
    template <typename T>
    static std::vector<std::uint8_t> EncodeFrame(const T& value, std::uint16_t type,
                                                 Encoding encoding = Encoding::kBinary,
                                                 Compression compression = Compression::kNone,
                                                 std::uint16_t flags = 0) {
        return net::Encode(MakeFrame(value, type, encoding, compression, flags));
    }

    /// @brief 从已解析的帧解出消息（帧头已经过 CRC/长度校验）。
    template <typename T>
    static CodecStatus DecodeFrame(const net::Frame& frame, T& out) {
        if (frame.payload.empty()) {
            return CodecStatus::kTruncated;
        }
        return ReflectCodec::Decode(frame.payload.data(), frame.payload.size(), out);
    }

    /**
     * @brief 从字节流解帧 + 解消息。
     * @param data 线上字节。
     * @param size 字节数。
     * @param out 目标消息；失败时不修改。
     * @param frame_status 非空时写入帧层状态（用于区分 CRC 错与消息格式错）。
     * @param consumed 非空时写入本帧消费的字节数。
     * @return 帧层错误返回 `kFrameError`（细节见 `frame_status`）。
     */
    template <typename T>
    static CodecStatus DecodeFrame(const std::uint8_t* data, std::size_t size, T& out,
                                   net::FrameStatus* frame_status = nullptr,
                                   std::size_t* consumed = nullptr) {
        net::Frame frame;
        const net::FrameStatus status = net::Decode(data, size, &frame, consumed);
        if (frame_status != nullptr) {
            *frame_status = status;
        }
        if (status != net::FrameStatus::kOk) {
            return CodecStatus::kFrameError;
        }
        return DecodeFrame(frame, out);
    }
};

}  // namespace go2cpp::reflect
