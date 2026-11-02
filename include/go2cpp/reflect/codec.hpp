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

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <system_error>
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

/// @brief 二进制载荷的字段描述方式。
enum class WireFormat : std::uint8_t {
    /// 紧凑格式（默认）：只写字段号 + 值，字段名/类型由本地 schema 还原。
    /// 比自描述格式每字段省「名字长度 + 名字 + TypeId」，但要求收发两端
    /// 的字段表一致；嵌套结构体同样使用紧凑格式。
    kCompact = 0,
    /// 自描述格式（向后兼容）：每条消息带字段表（名字 + TypeId），
    /// 可跨版本/跨语言解码，旧数据与旧实现都走这条。
    kSelfDescribed = 1,
};

/// @brief 二进制载荷 flags：bit0 = RLE 压缩。
inline constexpr std::uint8_t kFlagCompressedPayload = 0x01u;
/// @brief 二进制载荷 flags：bit1 = 紧凑载荷（无字段表）。
inline constexpr std::uint8_t kFlagCompactPayload = 0x02u;

/// @brief 消息外层（类型信息 + 已解压载荷）。
struct Envelope {
    Encoding encoding = Encoding::kBinary;  ///< 编码模式。
    bool compressed = false;                ///< 载荷是否经过 RLE。
    bool compact = false;                   ///< 二进制载荷是否紧凑格式。
    TypeId type_id = kUnknownType;          ///< 线上类型 id。
    std::string type_name;                  ///< 线上类型名。
    Buffer payload;                         ///< 二进制载荷或文本字段 JSON。
};

namespace detail {

/// @brief 编码消息外层；`payload` 是已序列化的载荷。
CodecStatus EncodeEnvelope(TypeId type_id, std::string_view type_name, Encoding encoding,
                           Compression compression, const Buffer& payload,
                           std::vector<std::uint8_t>& out, bool compact = false);

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

/// @brief 二进制紧凑消息的零拷贝快路径。
/// @details 不构造 Envelope、不复制载荷、不做字段名比较：直接按本地 schema
///          把 tag/值读进目标对象。只在「无压缩 + 紧凑 + 类型 id 命中」时接手；
///          其余情况（含需要精确错误码的坏数据）返回 `kUnsupportedEncoding`，
///          由通用路径给出与旧实现一致的错误语义。
template <typename T>
CodecStatus DecodeBinaryCompact(const std::uint8_t* data, std::size_t size, T& out,
                                std::size_t* consumed) {
    using Value = std::remove_cv_t<std::remove_reference_t<T>>;
    if (data == nullptr || size < 6) {
        return CodecStatus::kUnsupportedEncoding;
    }
    if (data[0] != 'G' || data[1] != '2' || data[2] != 'R' || data[3] != 'F') {
        return CodecStatus::kUnsupportedEncoding;
    }
    if (data[4] != 1) {
        return CodecStatus::kUnsupportedEncoding;
    }
    const std::uint8_t flags = data[5];
    // 只接手「紧凑且未压缩」的消息；压缩/自描述走通用路径。
    if ((flags & kFlagCompactPayload) == 0 || (flags & kFlagCompressedPayload) != 0) {
        return CodecStatus::kUnsupportedEncoding;
    }
    const std::uint8_t* cursor = data + 6;
    const std::uint8_t* const end = data + size;
    std::uint64_t type_id = 0;
    if (!ReadVarintPtr(cursor, end, type_id)) {
        return CodecStatus::kUnsupportedEncoding;
    }
    if (type_id > static_cast<std::uint64_t>(std::numeric_limits<TypeId>::max()) ||
        static_cast<TypeId>(type_id) != TypeIdOf<Value>()) {
        return CodecStatus::kUnsupportedEncoding;
    }
    std::uint64_t name_length = 0;
    if (!ReadVarintPtr(cursor, end, name_length)) {
        return CodecStatus::kUnsupportedEncoding;
    }
    if (name_length > kMaxTypeNameBytes ||
        name_length > static_cast<std::uint64_t>(end - cursor)) {
        return CodecStatus::kUnsupportedEncoding;
    }
    cursor += static_cast<std::size_t>(name_length);
    std::uint64_t payload_length = 0;
    if (!ReadVarintPtr(cursor, end, payload_length)) {
        return CodecStatus::kUnsupportedEncoding;
    }
    if (payload_length > kMaxBufferBytes ||
        payload_length > static_cast<std::uint64_t>(end - cursor)) {
        return CodecStatus::kUnsupportedEncoding;
    }
    const std::uint8_t* const payload_end = cursor + static_cast<std::size_t>(payload_length);
    Value temp{};
    if (!DeserializeCompact(cursor, payload_end, temp) || cursor != payload_end) {
        // 载荷坏/截断：交给通用路径产生精确错误码。
        return CodecStatus::kUnsupportedEncoding;
    }
    out = std::move(temp);
    if (consumed != nullptr) {
        *consumed = static_cast<std::size_t>(payload_end - data);
    }
    return CodecStatus::kOk;
}

/// @brief 文本消息直写：先按上界 resize，再逐字段写（无中间 payload 串）。
/// @return 只有 `kOk`（已写完）与 `kUnsupportedEncoding`（不适用，调用方回退）。
template <typename T>
CodecStatus EncodeTextFast(const T& value, std::vector<std::uint8_t>& out) {
    using Value = std::remove_cv_t<std::remove_reference_t<T>>;
    if constexpr (!HasFieldList<Value>::value) {
        (void)value;
        (void)out;
        return CodecStatus::kUnsupportedEncoding;
    } else {
        const std::size_t payload_estimate = EstimateJson(value);
        if (payload_estimate == std::numeric_limits<std::size_t>::max()) {
            return CodecStatus::kUnsupportedEncoding;
        }
        const char* const type_name = TypeNameOf<Value>();
        const std::size_t type_name_size = std::strlen(type_name);
        // 固定部分：{"v":1,"id": + id(<=10) + ,"type":" + name + ","payload": + } = 34 字节
        constexpr std::size_t kFixedBytes = 34 + 10;
        if (payload_estimate > kMaxBufferBytes ||
            type_name_size > kMaxBufferBytes - payload_estimate - kFixedBytes) {
            return CodecStatus::kUnsupportedEncoding;
        }
        const std::size_t total = kFixedBytes + type_name_size + payload_estimate;
        try {
            out.clear();
            out.resize(total);
        } catch (const std::bad_alloc&) {
            return CodecStatus::kUnsupportedEncoding;
        }
        JsonByteWriter writer(out);
        writer.Put("{\"v\":1,\"id\":", 12);
        char id_buffer[16] = {};
        const auto id_result = std::to_chars(id_buffer, id_buffer + sizeof(id_buffer),
                                             TypeIdOf<Value>());
        if (id_result.ec != std::errc()) {
            return CodecStatus::kUnsupportedEncoding;
        }
        writer.Put(id_buffer, static_cast<std::size_t>(id_result.ptr - id_buffer));
        writer.Put(",\"type\":\"", 9);
        writer.Put(type_name, type_name_size);
        writer.Put("\",\"payload\":", 12);
        AppendJson(value, writer);
        writer.Put('}');
        if (!writer.ok() || writer.size() > kMaxBufferBytes) {
            return CodecStatus::kUnsupportedEncoding;
        }
        out.resize(writer.size());
        return CodecStatus::kOk;
    }
}

/// @brief 文本消息直读：单遍扫描 envelope + payload，不建 JSON 树。
/// @return `kOk` 表示已解码；`kBadPayload` 表示载荷字段不合法；
///         `kUnsupportedEncoding` 表示不适用（调用方回退建树路径拿准确错误码）。
template <typename T>
CodecStatus DecodeTextDirect(const std::uint8_t* data, std::size_t size, T& out,
                             std::size_t* consumed) {
    using Value = std::remove_cv_t<std::remove_reference_t<T>>;
    if constexpr (!HasFieldList<Value>::value) {
        (void)data;
        (void)size;
        (void)out;
        (void)consumed;
        return CodecStatus::kUnsupportedEncoding;
    } else {
        std::string_view text(reinterpret_cast<const char*>(data), size);
        DirectReader reader(text);
        reader.SkipWhitespace();
        if (!reader.Consume('{')) {
            return CodecStatus::kUnsupportedEncoding;
        }
        std::int64_t version = 0;
        std::uint64_t type_id = 0;
        std::string_view type_name;
        bool has_version = false;
        bool has_id = false;
        bool has_type = false;
        bool type_escaped = false;
        bool has_payload = false;
        Value temp{};
        reader.SkipWhitespace();
        if (reader.Consume('}')) {
            return CodecStatus::kUnsupportedEncoding;
        }
        for (;;) {
            reader.SkipWhitespace();
            std::string_view key;
            bool key_escaped = false;
            if (!reader.ScanString(key, key_escaped)) {
                return CodecStatus::kUnsupportedEncoding;
            }
            reader.SkipWhitespace();
            if (!reader.Consume(':')) {
                return CodecStatus::kUnsupportedEncoding;
            }
            reader.SkipWhitespace();
            if (!key_escaped && key == "v") {
                if (!reader.ReadInt64(version)) {
                    return CodecStatus::kUnsupportedEncoding;
                }
                has_version = true;
            } else if (!key_escaped && key == "id") {
                if (!reader.ReadUint64(type_id)) {
                    return CodecStatus::kUnsupportedEncoding;
                }
                has_id = true;
            } else if (!key_escaped && key == "type") {
                if (!reader.ScanString(type_name, type_escaped)) {
                    return CodecStatus::kUnsupportedEncoding;
                }
                has_type = true;
            } else if (!key_escaped && key == "payload") {
                if (!ReadJsonDirect(reader, temp)) {
                    // 类型不匹配（名字与 id 都不对）时优先交给通用路径给出
                    // kTypeMismatch/kUnknownType——旧实现先校验类型再读载荷。
                    const bool current_name_ok =
                        has_type && !type_escaped &&
                        type_name == std::string_view(TypeNameOf<Value>());
                    const bool current_id_ok =
                        has_id && static_cast<TypeId>(type_id) == TypeIdOf<Value>();
                    if (!current_name_ok && !current_id_ok) {
                        return CodecStatus::kUnsupportedEncoding;
                    }
                    return reader.status() == CodecStatus::kBadPayload
                               ? CodecStatus::kBadPayload
                               : CodecStatus::kUnsupportedEncoding;
                }
                has_payload = true;
            } else if (!reader.SkipValue()) {
                return CodecStatus::kUnsupportedEncoding;
            }
            reader.SkipWhitespace();
            if (reader.Consume('}')) {
                break;
            }
            if (!reader.Consume(',')) {
                return CodecStatus::kUnsupportedEncoding;
            }
        }
        reader.SkipWhitespace();
        if (!reader.AtEnd()) {
            return CodecStatus::kUnsupportedEncoding;
        }
        if (!has_version || !has_id || !has_payload) {
            return CodecStatus::kUnsupportedEncoding;
        }
        constexpr std::int64_t kTextVersionValue = 1;
        if (version != kTextVersionValue) {
            return CodecStatus::kUnsupportedEncoding;
        }
        if (type_id > static_cast<std::uint64_t>(std::numeric_limits<TypeId>::max())) {
            return CodecStatus::kUnsupportedEncoding;
        }
        const bool name_ok =
            has_type && !type_escaped && type_name == std::string_view(TypeNameOf<Value>());
        const bool id_ok = static_cast<TypeId>(type_id) == TypeIdOf<Value>();
        if (!name_ok && !id_ok) {
            // 类型不匹配/未知类型：交给建树路径给出 kTypeMismatch/kUnknownType。
            return CodecStatus::kUnsupportedEncoding;
        }
        out = std::move(temp);
        if (consumed != nullptr) {
            *consumed = reader.position();
        }
        return CodecStatus::kOk;
    }
}

/// @brief 把已解析的外层载荷解码为目标类型。
/// @note `envelope` 按非 const 引用传入：二进制模式下 payload 会被 move 进
///       解码 Buffer（旧实现是整段复制一次），调用方传入局部 Envelope 即可。
template <typename T>
CodecStatus DecodeEnvelopeValue(Envelope& envelope, T& out) {
    using Value = std::remove_cv_t<std::remove_reference_t<T>>;
    const CodecStatus type_status = CheckWireType<Value>(envelope);
    if (type_status != CodecStatus::kOk) {
        return type_status;
    }
    if (envelope.encoding == Encoding::kBinary) {
        Buffer in = std::move(envelope.payload);
        Value temp{};
        CodecStatus status = CodecStatus::kOk;
        if (envelope.compact) {
            // 兼容路径里的紧凑载荷：仍是零拷贝指针解码，只是载荷已经落进
            // Envelope::payload（通用入口需要精确错误码）。
            const std::uint8_t* cursor = in.size() == 0 ? nullptr : in.data().data();
            const std::uint8_t* end = in.size() == 0 ? nullptr : in.data().data() + in.size();
            if (!DeserializeCompact(cursor, end, temp)) {
                return CodecStatus::kBadPayload;
            }
            if (cursor != end) {
                return CodecStatus::kBadPayload;
            }
            out = std::move(temp);
            return CodecStatus::kOk;
        }
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
                              Compression compression = Compression::kNone,
                              WireFormat wire_format = WireFormat::kCompact);

    /// @brief 编码为完整消息字节；失败返回空 vector。
    template <typename T>
    static std::vector<std::uint8_t> Encode(const T& value, Encoding encoding = Encoding::kBinary,
                                            Compression compression = Compression::kNone,
                                            WireFormat wire_format = WireFormat::kCompact);

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
                                 Compression compression, WireFormat wire_format) {
    using Value = std::remove_cv_t<std::remove_reference_t<T>>;
    out.clear();
    Buffer payload;
    const bool compact = encoding == Encoding::kBinary && wire_format == WireFormat::kCompact;
    if (encoding == Encoding::kBinary) {
        const bool ok = compact ? SerializeCompact(value, payload) : Serialize(value, payload);
        if (!ok) {
            return CodecStatus::kBadPayload;
        }
    } else {
        // 快速直写：一次 resize + 裸指针写，成功就不再有中间 payload 串。
        const CodecStatus fast = detail::EncodeTextFast(value, out);
        if (fast == CodecStatus::kOk) {
            return fast;
        }
        out.clear();
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
                                  payload, out, compact);
}

template <typename T>
std::vector<std::uint8_t> ReflectCodec::Encode(const T& value, Encoding encoding,
                                               Compression compression, WireFormat wire_format) {
    std::vector<std::uint8_t> out;
    (void)Encode(value, out, encoding, compression, wire_format);
    return out;
}

template <typename T>
CodecStatus ReflectCodec::Decode(const std::uint8_t* data, std::size_t size, T& out,
                                 std::size_t* consumed) {
    if (data == nullptr) {
        return CodecStatus::kInvalidArgument;
    }
    // 快路径：紧凑 + 未压缩 + 类型 id 命中时，不构造 Envelope、不复制载荷。
    const CodecStatus fast = detail::DecodeBinaryCompact<T>(data, size, out, consumed);
    if (fast != CodecStatus::kUnsupportedEncoding) {
        return fast;
    }
    // 文本快路径：单遍直读，不建 JSON 树。
    const CodecStatus text_fast = detail::DecodeTextDirect<T>(data, size, out, consumed);
    if (text_fast != CodecStatus::kUnsupportedEncoding) {
        return text_fast;
    }
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
