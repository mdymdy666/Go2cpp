#pragma once

/**
 * @file status.hpp
 * @brief Reflect 编解码的状态码。
 *
 * @details 错误码是**可恢复业务错误**的统一表示：解码路径不抛异常，所有失败
 *          都通过 `CodecStatus` 返回，调用方可以按错误类型决定重连、丢弃还是
 *          记录日志。编码路径的失败（类型未提供序列化器、内存超限）同样是
 *          编译期/运行期可判定的错误，不依赖异常。
 *
 *          状态码的取值刻意保持稳定（显式数字），因为网络层可能把它写进日志
 *          或监控指标；重排枚举值会让历史日志失去可比性。
 *
 * @author go2cpp contributors
 * @date 2026-10-30
 * @copyright MIT License
 */

#include <cstdint>

namespace go2cpp::reflect {

/// @brief 编解码结果。
enum class CodecStatus : std::uint8_t {
    kOk = 0,               ///< 成功。
    kInvalidArgument = 1,  ///< 调用方传了空指针/非法参数。
    kTruncated = 2,        ///< 字节数不足（半包或消息被截断）。
    kBadMagic = 3,         ///< 消息头魔数不匹配（不是 Reflect 消息）。
    kBadVersion = 4,       ///< 消息格式版本不支持。
    kUnknownType = 5,  ///< 解码端不认识消息类型（未注册且与目标类型不符）。
    kTypeMismatch = 6,  ///< 消息类型与目标 C++ 类型不一致。
    kUnknownField = 7,  ///< 二进制字段表里出现本地结构体没有的字段。
    kBadPayload = 8,    ///< 载荷内容非法（字段类型/长度/取值越界）。
    kBadText = 9,       ///< 文本模式 JSON 解析失败。
    kUnsupportedEncoding = 10,  ///< 该类型不支持所选编码模式（如自定义类型无文本模式）。
    kCompressionError = 11,  ///< 压缩流损坏（长度/控制字节非法）。
    kTooLarge = 12,          ///< 消息或字段超过大小上限。
    kFrameError = 13,        ///< Bento 帧层错误（CRC/长度/magic），见 FrameStatus。
};

/// @brief 状态名（日志/测试失败信息用），永不返回 nullptr。
const char* CodecStatusName(CodecStatus status) noexcept;

}  // namespace go2cpp::reflect
