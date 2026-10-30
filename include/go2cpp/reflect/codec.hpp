#pragma once

/**
 * @file codec.hpp
 * @brief `ReflectCodec`：把任意可序列化 C++ 值编码为自描述消息，或反向解码。
 *
 * @details 消息外层同时解决三件事：
 *
 *          1. **类型路由**：线上携带 `TypeId` + 类型名。类型名是跨进程的真正
 *             身份，id 只做同进程快速校验——这样服务端/客户端动态注册顺序
 *             不同也不会把消息解成错误类型；
 *          2. **编码模式**：二进制（传输用）或 JSON-like 文本（调试用），
 *             由魔数/首字符自动识别，解码端不需要额外协商；
 *          3. **可选压缩**：二进制载荷可套 RLE，压缩标志写在消息头；压缩后
 *             不比原文小则不采用（不设置标志），避免「压缩反而变大」。
 *
 *          二进制消息布局（多字节整数为 varint 或显式小端，不依赖主机字节序）：
 *
 *          | 偏移 | 字段       | 编码                              |
 *          |------|------------|-----------------------------------|
 *          | 0    | magic      | 4B `"G2RF"`                       |
 *          | 4    | version    | u8，当前 1                        |
 *          | 5    | flags      | u8，bit0 = 载荷已 RLE 压缩        |
 *          | 6    | type_id    | varint                            |
 *          | ...  | type_name  | varint 长度 + UTF-8              |
 *          | ...  | payload    | varint 长度 + 载荷（可能压缩）    |
 *
 *          文本消息布局：
 *          `{"v":1,"id":7,"type":"LoginRequest","payload":{...}}`
 *
 * @note 解码是**全量校验**的：类型不匹配、未知字段、载荷尾随字节都会返回
 *       错误，不会「尽量解析」出半个对象。强保证：失败时目标对象不变。
 *
 * @author go2cpp contributors
 * @date 2026-10-30
 * @copyright MIT License
 */

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "go2cpp/reflect/buffer.hpp"
#include "go2cpp/reflect/json.hpp"
#include "go2cpp/reflect/serializer_impl.hpp"
#include "go2cpp/reflect/status.hpp"
#include "go2cpp/reflect/type.hpp"

namespace go2cpp::reflect {

/// @brief 编码模式。
enum class Encoding : std::uint8_t {
    kBinary = 0,  ///< 二进制紧凑格式（传输）。
    kText = 1,    ///< JSON-like 文本（调试）。
};

/// @brief 压缩算法。
enum class Compression : std::uint8_t {
    kNone = 0,  ///< 不压缩。
    kRle = 1,   ///< 字节级 RLE（仅二进制模式生效）。
};

/// @brief 消息外层（类型信息 + 已解压载荷）。
struct Envelope {
    Encoding encoding = Encoding::kBinary;  ///< 编码模式。
    bool compressed = false;                ///< 载荷是否经过 RLE。
    TypeId type_id = kUnknownType;          ///< 线上类型 id。
    std::string type_name;                  ///< 线上类型名。
    Buffer payload;                         ///< 二进制载荷或文本字段 JSON。
};

namespace detail {

/// @brief 编码消息外层；`payload` 是已序列化的载荷。
CodecStatus EncodeEnvelope(TypeId type_id, std::string_view type_name, Encoding encoding,
                           Compression compression, const Buffer& payload,
                           std::vector<std::uint8_t>& out);

/// @brief 解析消息外层（含解压），不解释载荷。
CodecStatus DecodeEnvelope(const std::uint8_t* data, std::size_t size, Envelope& out,
                           std::size_t* consumed);

/// @brief 校验线上类型与目标类型是否兼容。
template <typename T>
CodecStatus CheckWireType(const Envelope& envelope) {
    using Value = std::remove_cv_t<std::remove_reference_t<T>>;
    const TypeId local_id = TypeIdOf<Value>();
    const char* local_name = TypeNameOf<Value>();
    if (envelope.type_name == local_name) {
        return CodecStatus::kOk;
    }
    if (envelope.type_id != kUnknownType && envelope.type_id == local_id) {
        return CodecStatus::kOk;
    }
    // 名字不同：若线上 id 是本地已注册的另一个类型，属于明确的类型不匹配；
    // 若 id 完全未知，说明解码端不认识这条消息（可能是新版本服务端）。
    if (envelope.type_id != kUnknownType && TypeRegistry::Instance().Contains(envelope.type_id)) {
        return CodecStatus::kTypeMismatch;
    }
    return CodecStatus::kUnknownType;
}

/// @brief 把已解析的外层载荷解码为目标类型。
template <typename T>
CodecStatus DecodeEnvelopeValue(const Envelope& envelope, T& out) {
    using Value = std::remove_cv_t<std::remove_reference_t<T>>;
    const CodecStatus type_status = CheckWireType<Value>(envelope);
    if (type_status != CodecStatus::kOk) {
        return type_status;
    }
    if (envelope.encoding == Encoding::kBinary) {
        Buffer in = envelope.payload;
        Value temp{};
        CodecStatus status = CodecStatus::kOk;
        if constexpr (HasFieldList<Value>::value) {
            status = DeserializeReflect(in, temp);
        } else if constexpr (TypeTraits<Value>::kBuiltin) {
            status = Serializer<Value>::Deserialize(in, temp) ? CodecStatus::kOk
                                                              : CodecStatus::kBadPayload;
        } else {
            status = CodecStatus::kUnsupportedEncoding;
        }
        if (status != CodecStatus::kOk) {
            return status;
        }
        if (in.Remaining() != 0) {
            return CodecStatus::kBadPayload;
        }
        out = std::move(temp);
        return CodecStatus::kOk;
    }

    std::string_view text;
    if (!envelope.payload.data().empty()) {
        text = std::string_view(reinterpret_cast<const char*>(envelope.payload.data().data()),
                                envelope.payload.data().size());
    }
    JsonValue json;
    if (!JsonParser::Parse(text, json)) {
        return CodecStatus::kBadText;
    }
    if constexpr (IsJsonWritable<Value>()) {
        Value temp{};
        if (!ReadJson(json, temp)) {
            return CodecStatus::kBadPayload;
        }
        out = std::move(temp);
        return CodecStatus::kOk;
    } else {
        return CodecStatus::kUnsupportedEncoding;
    }
}

}  // namespace detail

/**
 * @brief Reflect 编解码统一入口。
 *
 * @details 所有方法都是静态的、无状态；线程安全性取决于 `TypeRegistry`
 *          （内部加锁）与调用方自己的对象。
 */
class ReflectCodec {
   public:
    /**
     * @brief 编码为完整消息字节。
     * @param value 待编码值。
     * @param out 输出字节；失败时清空。
     * @param encoding 编码模式。
     * @param compression 压缩算法（仅二进制模式生效）。
     * @return 状态码。
     */
    template <typename T>
    static CodecStatus Encode(const T& value, std::vector<std::uint8_t>& out,
                              Encoding encoding = Encoding::kBinary,
                              Compression compression = Compression::kNone);

    /// @brief 编码为完整消息字节；失败返回空 vector。
    template <typename T>
    static std::vector<std::uint8_t> Encode(const T& value, Encoding encoding = Encoding::kBinary,
                                            Compression compression = Compression::kNone);

    /**
     * @brief 从字节流解码。
     * @param data 输入字节。
     * @param size 输入长度。
     * @param out 目标值；失败时保持不变。
     * @param consumed 非空时写入本消息消费的字节数（支持串接多条消息）。
     * @return 状态码。
     */
    template <typename T>
    static CodecStatus Decode(const std::uint8_t* data, std::size_t size, T& out,
                              std::size_t* consumed = nullptr);

    /// @brief 从完整字节数组解码（不允许尾随字节）。
    template <typename T>
    static CodecStatus Decode(const std::vector<std::uint8_t>& bytes, T& out);

    /// @brief 只解析消息外层（类型路由/调试）；不要求调用方知道 T。
    static CodecStatus Peek(const std::uint8_t* data, std::size_t size, Envelope& out,
                            std::size_t* consumed = nullptr) {
        return detail::DecodeEnvelope(data, size, out, consumed);
    }

    /// @brief 只取类型 id 与名字（未知消息路由用）。
    static CodecStatus PeekType(const std::uint8_t* data, std::size_t size, TypeId& type_id,
                                std::string& type_name);
};

template <typename T>
CodecStatus ReflectCodec::Encode(const T& value, std::vector<std::uint8_t>& out, Encoding encoding,
                                 Compression compression) {
    using Value = std::remove_cv_t<std::remove_reference_t<T>>;
    out.clear();
    Buffer payload;
    if (encoding == Encoding::kBinary) {
        if (!Serialize(value, payload)) {
            return CodecStatus::kBadPayload;
        }
    } else {
        std::string text;
        JsonWriter writer(text);
        if (!WriteJson(value, writer) || !writer.ok()) {
            return CodecStatus::kUnsupportedEncoding;
        }
        if (!payload.PutBytes(text.data(), text.size())) {
            return CodecStatus::kTooLarge;
        }
    }
    return detail::EncodeEnvelope(TypeIdOf<Value>(), TypeNameOf<Value>(), encoding, compression,
                                  payload, out);
}

template <typename T>
std::vector<std::uint8_t> ReflectCodec::Encode(const T& value, Encoding encoding,
                                               Compression compression) {
    std::vector<std::uint8_t> out;
    (void)Encode(value, out, encoding, compression);
    return out;
}

template <typename T>
CodecStatus ReflectCodec::Decode(const std::uint8_t* data, std::size_t size, T& out,
                                 std::size_t* consumed) {
    Envelope envelope;
    const CodecStatus status = detail::DecodeEnvelope(data, size, envelope, consumed);
    if (status != CodecStatus::kOk) {
        return status;
    }
    return detail::DecodeEnvelopeValue<T>(envelope, out);
}

template <typename T>
CodecStatus ReflectCodec::Decode(const std::vector<std::uint8_t>& bytes, T& out) {
    if (bytes.empty()) {
        return CodecStatus::kTruncated;
    }
    return Decode(bytes.data(), bytes.size(), out);
}

}  // namespace go2cpp::reflect
