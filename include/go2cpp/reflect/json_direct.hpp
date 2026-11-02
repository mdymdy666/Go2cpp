#pragma once

/**
 * @file json_direct.hpp
 * @brief Schema 驱动的 JSON 快速路径：预留缓冲直写 + 不建树直读。
 *
 * @details 文本模式原本走「建树」路径：`JsonParser` 把整份文档解析成
 *          `JsonValue` 树（每个对象/字符串都分配），`WriteJsonValue` 再把
 *          payload 重写成字符串，随后又 `Parse` 一次 payload、`ReadJson`
 *          遍历树把值拷进结构体。一条 150 字节的登录消息因此要做两次完整
 *          解析 + 一次重写。
 *
 *          本文件提供两条对称的快速路径：
 *
 *          - **直写**：`EstimateJson` 先算出字节数上界，输出缓冲一次性
 *            `resize`，`AppendJson` 用裸指针逐字段写（无中间 string、无
 *            反复扩容）。转义、整数格式化仍与 `JsonWriter` 完全一致。
 *          - **直读**：`DirectReader` 在原始文本上单遍前进，按字段名直接
 *            写进目标成员，不构造任何 JSON 树。字段名匹配、未知字段拒绝、
 *            重复键拒绝、转义/Unicode 解码、数字范围校验都与建树路径一致。
 *
 *          两条路径都是「schema 驱动」的：结构体字段通过 `FieldOps` 的
 *          `estimate_text` / `write_text_fast` / `read_text_direct` 回调递归，
 *          内置类型在模板里直接展开。
 *
 * @note 快速路径失败（语法错误/类型不匹配/未知字段）时调用方回退到建树
 *       路径，由后者给出与旧实现完全一致的错误码；直写失败只可能是值超出
 *       容量或非有限浮点，同样回退。
 *
 * @author go2cpp contributors
 * @date 2026-11-02
 * @copyright MIT License
 */

#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <new>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>
#include <vector>

#include "go2cpp/encoding/hex_core.hpp"
#include "go2cpp/reflect/field.hpp"
#include "go2cpp/reflect/json.hpp"
#include "go2cpp/reflect/status.hpp"
#include "go2cpp/reflect/type.hpp"

namespace go2cpp::reflect {

/**
 * @brief 直写 JSON 的字节游标。
 *
 * @details 调用方必须先按 `EstimateJson` 的上界把输出缓冲 `resize` 到位；
 *          写操作只做定长 `memcpy` / 单字节写，不做重新分配。写入内容非法
 *          （非有限浮点）或超出容量时通过 `ok()` 反映，由调用方回退。
 */
class JsonByteWriter {
   public:
    /// @param out 已按上界 resize 的输出缓冲；生命周期必须覆盖写入器。
    explicit JsonByteWriter(std::vector<std::uint8_t>& out) noexcept
        : m_data(out.data()), m_capacity(out.size()) {}

    /// @brief 写一个字符。
    void Put(char value) noexcept {
        if (m_size < m_capacity) {
            m_data[m_size++] = static_cast<std::uint8_t>(value);
        } else {
            m_ok = false;
        }
    }

    /// @brief 写一段字节。
    void Put(const char* text, std::size_t size) noexcept {
        if (size == 0) {
            return;
        }
        if (size > m_capacity - m_size) {
            m_ok = false;
            return;
        }
        std::memcpy(m_data + m_size, text, size);
        m_size += size;
    }

    /// @brief 写一段字节。
    void Put(std::string_view text) noexcept {
        Put(text.data(), text.size());
    }

    /// @brief 已写入字节数。
    std::size_t size() const noexcept {
        return m_size;
    }

    /// @brief 写入是否仍然合法（容量足够且值合法）。
    bool ok() const noexcept {
        return m_ok;
    }

    /// @brief 显式标记失败。
    void Fail() noexcept {
        m_ok = false;
    }

   private:
    std::uint8_t* m_data;    ///< 输出起点。
    std::size_t m_capacity;  ///< 已预留字节数。
    std::size_t m_size = 0;  ///< 已写入字节数。
    bool m_ok = true;        ///< 写入是否合法。
};

/// @brief JSON 字符串内容的转义写入（规则与 `JsonWriter::AppendEscaped` 一致）。
inline void AppendJsonEscaped(std::string_view text, JsonByteWriter& out) noexcept {
    std::size_t plain = 0;
    while (plain < text.size()) {
        const unsigned char value = static_cast<unsigned char>(text[plain]);
        if (value < 0x20u || value == '"' || value == '\\') {
            break;
        }
        ++plain;
    }
    out.Put(text.data(), plain);
    for (std::size_t index = plain; index < text.size(); ++index) {
        const unsigned char value = static_cast<unsigned char>(text[index]);
        switch (value) {
            case '"':
                out.Put("\\\"", 2);
                break;
            case '\\':
                out.Put("\\\\", 2);
                break;
            case '\b':
                out.Put("\\b", 2);
                break;
            case '\f':
                out.Put("\\f", 2);
                break;
            case '\n':
                out.Put("\\n", 2);
                break;
            case '\r':
                out.Put("\\r", 2);
                break;
            case '\t':
                out.Put("\\t", 2);
                break;
            default:
                if (value < 0x20u) {
                    const char* digits = encoding::hexcore::kLowerHexDigits;
                    const char escaped[6] = {'\\', 'u', '0', '0', digits[(value >> 4) & 0x0fu],
                                             digits[value & 0x0fu]};
                    out.Put(escaped, sizeof(escaped));
                } else {
                    out.Put(static_cast<char>(value));
                }
                break;
        }
    }
}

namespace detail {

template <typename T>
inline constexpr bool kDirectAlwaysFalse = false;

/// @brief 内置类型的 JSON 字节数上界。
template <typename T>
std::size_t EstimateJsonBuiltin(const T& value) noexcept {
    if constexpr (std::is_same_v<T, bool>) {
        return 5;
    } else if constexpr (std::is_integral_v<T>) {
        // int64 最长 "-9223372036854775808"（20 字节），uint64 20 字节。
        return 21;
    } else if constexpr (std::is_same_v<T, double>) {
        // to_chars(general) 的最长形式（含 e±xxx 指数）不超过 32 字节。
        return 32;
    } else if constexpr (std::is_same_v<T, std::string>) {
        // 每个字节最坏转义成 \u00XX（6 字节），外加首尾引号。
        if (value.size() > (std::numeric_limits<std::size_t>::max() - 8) / 6) {
            return std::numeric_limits<std::size_t>::max();
        }
        return value.size() * 6 + 2;
    } else if constexpr (std::is_same_v<T, std::vector<std::uint8_t>>) {
        if (value.size() > (std::numeric_limits<std::size_t>::max() - 8) / 4) {
            return std::numeric_limits<std::size_t>::max();
        }
        return value.size() * 4 + 3;
    } else {
        (void)value;
        return std::numeric_limits<std::size_t>::max();
    }
}

/// @brief 内置类型的 JSON 直写。
template <typename T>
void AppendJsonBuiltin(const T& value, JsonByteWriter& out) {
    if constexpr (std::is_same_v<T, bool>) {
        if (value) {
            out.Put("true", 4);
        } else {
            out.Put("false", 5);
        }
    } else if constexpr (std::is_integral_v<T>) {
        char buffer[24] = {};
        const auto result = std::to_chars(buffer, buffer + sizeof(buffer), value);
        if (result.ec != std::errc()) {
            out.Fail();
            return;
        }
        out.Put(buffer, static_cast<std::size_t>(result.ptr - buffer));
    } else if constexpr (std::is_same_v<T, double>) {
        if (!std::isfinite(value)) {
            out.Fail();
            return;
        }
        char buffer[40] = {};
        const auto result =
            std::to_chars(buffer, buffer + sizeof(buffer), value, std::chars_format::general);
        if (result.ec != std::errc()) {
            out.Fail();
            return;
        }
        out.Put(buffer, static_cast<std::size_t>(result.ptr - buffer));
    } else if constexpr (std::is_same_v<T, std::string>) {
        out.Put('"');
        AppendJsonEscaped(value, out);
        out.Put('"');
    } else if constexpr (std::is_same_v<T, std::vector<std::uint8_t>>) {
        out.Put('[');
        for (std::size_t index = 0; index < value.size(); ++index) {
            if (index != 0) {
                out.Put(',');
            }
            char buffer[8] = {};
            const auto result = std::to_chars(buffer, buffer + sizeof(buffer), value[index]);
            if (result.ec != std::errc()) {
                out.Fail();
                return;
            }
            out.Put(buffer, static_cast<std::size_t>(result.ptr - buffer));
        }
        out.Put(']');
    } else {
        static_assert(kDirectAlwaysFalse<T>, "unsupported builtin type for JSON writer");
    }
}

}  // namespace detail

/// @brief 值的 JSON 字节数上界（结构体递归字段表，内置类型查表）。
template <typename T>
std::size_t EstimateJson(const T& value) {
    using Value = std::remove_cv_t<std::remove_reference_t<T>>;
    if constexpr (HasFieldList<Value>::value) {
        std::size_t count = 0;
        const FieldDescriptor* fields = FieldList<Value>::Fields(count);
        std::size_t total = 2;  // '{' + '}'
        for (std::size_t index = 0; index < count; ++index) {
            total += fields[index].name_size + 4;  // '"' + name + '":' + 逗号（多算 1 字节）
            const std::size_t body = fields[index].ops.estimate_text(&value);
            if (body > std::numeric_limits<std::size_t>::max() - total) {
                return std::numeric_limits<std::size_t>::max();
            }
            total += body;
        }
        return total;
    } else if constexpr (TypeTraits<Value>::kBuiltin) {
        return detail::EstimateJsonBuiltin(value);
    } else {
        (void)value;
        return std::numeric_limits<std::size_t>::max();
    }
}

/// @brief 把值按 JSON 语法直写到已预留容量的缓冲。
template <typename T>
void AppendJson(const T& value, JsonByteWriter& out) {
    using Value = std::remove_cv_t<std::remove_reference_t<T>>;
    if constexpr (HasFieldList<Value>::value) {
        std::size_t count = 0;
        const FieldDescriptor* fields = FieldList<Value>::Fields(count);
        out.Put('{');
        for (std::size_t index = 0; index < count; ++index) {
            if (index != 0) {
                out.Put(',');
            }
            out.Put('"');
            out.Put(fields[index].name, fields[index].name_size);
            out.Put("\":", 2);
            fields[index].ops.write_text_fast(&value, out);
        }
        out.Put('}');
    } else if constexpr (TypeTraits<Value>::kBuiltin) {
        detail::AppendJsonBuiltin(value, out);
    } else {
        (void)value;
        (void)out;
    }
}

/**
 * @brief 直读游标：在原始 JSON 文本上单遍前进。
 *
 * @details 只保存位置与首个错误码；不构造任何容器。所有 `Read*` 方法在
 *          失败时把错误码写进 `status()`（区分语法错误 `kBadText` 与
 *          值错误 `kBadPayload`），调用方据此决定是回退还是直接返回。
 */
class DirectReader {
   public:
    /// @param text 完整输入文本。
    explicit DirectReader(std::string_view text) noexcept : m_text(text) {}

    /// @brief 当前位置。
    std::size_t position() const noexcept {
        return m_pos;
    }

    /// @brief 是否已到输入末尾。
    bool AtEnd() const noexcept {
        return m_pos >= m_text.size();
    }

    /// @brief 首个错误码（`kOk` 表示尚无错误）。
    CodecStatus status() const noexcept {
        return m_status;
    }

    /// @brief 显式设置错误码（只保留第一个）。
    void SetStatus(CodecStatus status) noexcept {
        if (m_status == CodecStatus::kOk) {
            m_status = status;
        }
    }

    /// @brief 跳过 JSON 空白。
    void SkipWhitespace() noexcept {
        while (m_pos < m_text.size()) {
            const char value = m_text[m_pos];
            if (value != ' ' && value != '\t' && value != '\n' && value != '\r') {
                break;
            }
            ++m_pos;
        }
    }

    /// @brief 消费一个期望字符；不匹配返回 false。
    bool Consume(char expected) noexcept {
        if (m_pos >= m_text.size() || m_text[m_pos] != expected) {
            return false;
        }
        ++m_pos;
        return true;
    }

    /// @brief 扫描一个 JSON 字符串，返回引号内的原始视图（可能含转义）。
    /// @param raw 原始内容（未解码）。
    /// @param escaped 是否含反斜杠转义。
    /// @return 语法错误时返回 false 并置 `kBadText`。
    bool ScanString(std::string_view& raw, bool& escaped) noexcept {
        escaped = false;
        if (!Consume('"')) {
            SetStatus(CodecStatus::kBadText);
            return false;
        }
        const std::size_t start = m_pos;
        while (m_pos < m_text.size()) {
            const unsigned char value = static_cast<unsigned char>(m_text[m_pos]);
            if (value == '"') {
                raw = m_text.substr(start, m_pos - start);
                ++m_pos;
                return true;
            }
            if (value == '\\') {
                escaped = true;
                if (m_pos + 1 >= m_text.size()) {
                    break;
                }
                m_pos += 2;
                continue;
            }
            if (value < 0x20u) {
                SetStatus(CodecStatus::kBadText);
                return false;
            }
            ++m_pos;
        }
        SetStatus(CodecStatus::kBadText);
        return false;
    }

    /// @brief 把含转义的原始字符串解码成 UTF-8。
    bool Unescape(std::string_view raw, std::string& out) {
        out.clear();
        if (raw.size() > kMaxJsonStringBytes) {
            SetStatus(CodecStatus::kBadText);
            return false;
        }
        out.reserve(raw.size());
        const char* cursor = raw.data();
        const char* const end = raw.data() + raw.size();
        while (cursor < end) {
            const char value = *cursor++;
            if (value != '\\') {
                out.push_back(value);
                continue;
            }
            if (cursor >= end) {
                SetStatus(CodecStatus::kBadText);
                return false;
            }
            switch (*cursor++) {
                case '"':
                    out.push_back('"');
                    break;
                case '\\':
                    out.push_back('\\');
                    break;
                case '/':
                    out.push_back('/');
                    break;
                case 'b':
                    out.push_back('\b');
                    break;
                case 'f':
                    out.push_back('\f');
                    break;
                case 'n':
                    out.push_back('\n');
                    break;
                case 'r':
                    out.push_back('\r');
                    break;
                case 't':
                    out.push_back('\t');
                    break;
                case 'u': {
                    std::uint32_t code_point = 0;
                    if (!ParseHex4(cursor, end, code_point)) {
                        return false;
                    }
                    if (code_point >= 0xd800u && code_point <= 0xdbffu) {
                        if (end - cursor < 6 || cursor[0] != '\\' || cursor[1] != 'u') {
                            SetStatus(CodecStatus::kBadText);
                            return false;
                        }
                        cursor += 2;
                        std::uint32_t low = 0;
                        if (!ParseHex4(cursor, end, low) || low < 0xdc00u || low > 0xdfffu) {
                            SetStatus(CodecStatus::kBadText);
                            return false;
                        }
                        code_point = 0x10000u + ((code_point - 0xd800u) << 10) + (low - 0xdc00u);
                    } else if (code_point >= 0xdc00u && code_point <= 0xdfffu) {
                        SetStatus(CodecStatus::kBadText);
                        return false;
                    }
                    AppendUtf8(code_point, out);
                    break;
                }
                default:
                    SetStatus(CodecStatus::kBadText);
                    return false;
            }
        }
        return true;
    }

    /// @brief 读一个字符串到 `out`（无转义时一次 assign）。
    bool ReadString(std::string& out) {
        std::string_view raw;
        bool escaped = false;
        if (!ScanString(raw, escaped)) {
            return false;
        }
        if (escaped) {
            return Unescape(raw, out);
        }
        if (raw.size() > kMaxJsonStringBytes) {
            SetStatus(CodecStatus::kBadPayload);
            return false;
        }
        try {
            out.assign(raw.data(), raw.size());
        } catch (const std::bad_alloc&) {
            SetStatus(CodecStatus::kBadPayload);
            return false;
        }
        return true;
    }

    /// @brief 读有符号 64 位整数（JSON 数字语法由扫描器校验）。
    bool ReadInt64(std::int64_t& out) {
        std::string_view raw;
        if (!ScanNumber(raw)) {
            return false;
        }
        std::int64_t value = 0;
        const auto result = std::from_chars(raw.data(), raw.data() + raw.size(), value);
        if (result.ec != std::errc() || result.ptr != raw.data() + raw.size()) {
            SetStatus(CodecStatus::kBadPayload);
            return false;
        }
        out = value;
        return true;
    }

    /// @brief 读无符号 64 位整数。
    bool ReadUint64(std::uint64_t& out) {
        std::string_view raw;
        if (!ScanNumber(raw)) {
            return false;
        }
        std::uint64_t value = 0;
        const auto result = std::from_chars(raw.data(), raw.data() + raw.size(), value);
        if (result.ec != std::errc() || result.ptr != raw.data() + raw.size()) {
            SetStatus(CodecStatus::kBadPayload);
            return false;
        }
        out = value;
        return true;
    }

    /// @brief 读双精度浮点。
    bool ReadDouble(double& out) {
        std::string_view raw;
        if (!ScanNumber(raw)) {
            return false;
        }
        double value = 0.0;
        const auto result = std::from_chars(raw.data(), raw.data() + raw.size(), value);
        if (result.ec != std::errc() || result.ptr != raw.data() + raw.size()) {
            SetStatus(CodecStatus::kBadPayload);
            return false;
        }
        out = value;
        return true;
    }

    /// @brief 读布尔。
    bool ReadBool(bool& out) {
        if (m_text.size() - m_pos >= 4 && m_text.substr(m_pos, 4) == "true") {
            m_pos += 4;
            out = true;
            return true;
        }
        if (m_text.size() - m_pos >= 5 && m_text.substr(m_pos, 5) == "false") {
            m_pos += 5;
            out = false;
            return true;
        }
        SetStatus(CodecStatus::kBadPayload);
        return false;
    }

    /// @brief 读字节数组（`[1,2,3]`）。
    bool ReadByteArray(std::vector<std::uint8_t>& out) {
        if (!Consume('[')) {
            SetStatus(CodecStatus::kBadPayload);
            return false;
        }
        std::vector<std::uint8_t> bytes;
        SkipWhitespace();
        if (Consume(']')) {
            out = std::move(bytes);
            return true;
        }
        for (;;) {
            std::uint64_t byte = 0;
            if (!ReadUint64(byte)) {
                return false;
            }
            if (byte > 0xffu || bytes.size() >= kMaxBlobBytes) {
                SetStatus(CodecStatus::kBadPayload);
                return false;
            }
            try {
                bytes.push_back(static_cast<std::uint8_t>(byte));
            } catch (const std::bad_alloc&) {
                SetStatus(CodecStatus::kBadPayload);
                return false;
            }
            SkipWhitespace();
            if (Consume(']')) {
                out = std::move(bytes);
                return true;
            }
            if (!Consume(',')) {
                SetStatus(CodecStatus::kBadText);
                return false;
            }
            SkipWhitespace();
        }
    }

    /// @brief 跳过任意 JSON 值（envelope 的未知键用）。
    bool SkipValue(std::size_t depth = 0) {
        if (depth > kMaxJsonDepth) {
            SetStatus(CodecStatus::kBadText);
            return false;
        }
        SkipWhitespace();
        if (AtEnd()) {
            SetStatus(CodecStatus::kBadText);
            return false;
        }
        switch (m_text[m_pos]) {
            case '"': {
                std::string_view raw;
                bool escaped = false;
                return ScanString(raw, escaped);
            }
            case '{': {
                ++m_pos;
                SkipWhitespace();
                if (Consume('}')) {
                    return true;
                }
                for (;;) {
                    std::string_view key;
                    bool escaped = false;
                    if (!ScanString(key, escaped)) {
                        return false;
                    }
                    SkipWhitespace();
                    if (!Consume(':')) {
                        SetStatus(CodecStatus::kBadText);
                        return false;
                    }
                    if (!SkipValue(depth + 1)) {
                        return false;
                    }
                    SkipWhitespace();
                    if (Consume('}')) {
                        return true;
                    }
                    if (!Consume(',')) {
                        SetStatus(CodecStatus::kBadText);
                        return false;
                    }
                    SkipWhitespace();
                }
            }
            case '[': {
                ++m_pos;
                SkipWhitespace();
                if (Consume(']')) {
                    return true;
                }
                for (;;) {
                    if (!SkipValue(depth + 1)) {
                        return false;
                    }
                    SkipWhitespace();
                    if (Consume(']')) {
                        return true;
                    }
                    if (!Consume(',')) {
                        SetStatus(CodecStatus::kBadText);
                        return false;
                    }
                    SkipWhitespace();
                }
            }
            case 't':
                return ConsumeLiteral("true");
            case 'f':
                return ConsumeLiteral("false");
            case 'n':
                return ConsumeLiteral("null");
            default: {
                std::string_view raw;
                return ScanNumber(raw);
            }
        }
    }

   private:
    static bool IsHexDigit(char value) noexcept {
        return (value >= '0' && value <= '9') || (value >= 'a' && value <= 'f') ||
               (value >= 'A' && value <= 'F');
    }

    static int HexValue(char value) noexcept {
        if (value >= '0' && value <= '9') {
            return value - '0';
        }
        if (value >= 'a' && value <= 'f') {
            return value - 'a' + 10;
        }
        return value - 'A' + 10;
    }

    static void AppendUtf8(std::uint32_t code_point, std::string& out) {
        if (code_point <= 0x7fu) {
            out.push_back(static_cast<char>(code_point));
        } else if (code_point <= 0x7ffu) {
            out.push_back(static_cast<char>(0xc0u | (code_point >> 6)));
            out.push_back(static_cast<char>(0x80u | (code_point & 0x3fu)));
        } else if (code_point <= 0xffffu) {
            out.push_back(static_cast<char>(0xe0u | (code_point >> 12)));
            out.push_back(static_cast<char>(0x80u | ((code_point >> 6) & 0x3fu)));
            out.push_back(static_cast<char>(0x80u | (code_point & 0x3fu)));
        } else {
            out.push_back(static_cast<char>(0xf0u | (code_point >> 18)));
            out.push_back(static_cast<char>(0x80u | ((code_point >> 12) & 0x3fu)));
            out.push_back(static_cast<char>(0x80u | ((code_point >> 6) & 0x3fu)));
            out.push_back(static_cast<char>(0x80u | (code_point & 0x3fu)));
        }
    }

    bool ParseHex4(const char*& cursor, const char* end, std::uint32_t& out) noexcept {
        if (end - cursor < 4) {
            SetStatus(CodecStatus::kBadText);
            return false;
        }
        std::uint32_t value = 0;
        for (std::size_t index = 0; index < 4; ++index) {
            if (!IsHexDigit(cursor[index])) {
                SetStatus(CodecStatus::kBadText);
                return false;
            }
            value = (value << 4) | static_cast<std::uint32_t>(HexValue(cursor[index]));
        }
        cursor += 4;
        out = value;
        return true;
    }

    /// @brief 扫描数字 token（严格 JSON 语法），返回原文视图。
    bool ScanNumber(std::string_view& raw) noexcept {
        const std::size_t start = m_pos;
        if (m_pos < m_text.size() && m_text[m_pos] == '-') {
            ++m_pos;
        }
        if (m_pos >= m_text.size()) {
            SetStatus(CodecStatus::kBadText);
            return false;
        }
        if (m_text[m_pos] == '0') {
            ++m_pos;
        } else if (m_text[m_pos] >= '1' && m_text[m_pos] <= '9') {
            while (m_pos < m_text.size() && m_text[m_pos] >= '0' && m_text[m_pos] <= '9') {
                ++m_pos;
            }
        } else {
            SetStatus(CodecStatus::kBadText);
            return false;
        }
        if (m_pos < m_text.size() && m_text[m_pos] == '.') {
            ++m_pos;
            const std::size_t digits = m_pos;
            while (m_pos < m_text.size() && m_text[m_pos] >= '0' && m_text[m_pos] <= '9') {
                ++m_pos;
            }
            if (m_pos == digits) {
                SetStatus(CodecStatus::kBadText);
                return false;
            }
        }
        if (m_pos < m_text.size() && (m_text[m_pos] == 'e' || m_text[m_pos] == 'E')) {
            ++m_pos;
            if (m_pos < m_text.size() && (m_text[m_pos] == '+' || m_text[m_pos] == '-')) {
                ++m_pos;
            }
            const std::size_t digits = m_pos;
            while (m_pos < m_text.size() && m_text[m_pos] >= '0' && m_text[m_pos] <= '9') {
                ++m_pos;
            }
            if (m_pos == digits) {
                SetStatus(CodecStatus::kBadText);
                return false;
            }
        }
        raw = m_text.substr(start, m_pos - start);
        return true;
    }

    bool ConsumeLiteral(std::string_view literal) noexcept {
        if (m_text.size() - m_pos < literal.size() ||
            m_text.substr(m_pos, literal.size()) != literal) {
            SetStatus(CodecStatus::kBadText);
            return false;
        }
        m_pos += literal.size();
        return true;
    }

    std::string_view m_text;                  ///< 输入文本。
    std::size_t m_pos = 0;                    ///< 当前解析位置。
    CodecStatus m_status = CodecStatus::kOk;  ///< 首个错误码。
};

namespace detail {

/// @brief 内置类型的 JSON 直读。
template <typename T>
bool ReadJsonDirectBuiltin(DirectReader& reader, T& value) {
    if constexpr (std::is_same_v<T, bool>) {
        return reader.ReadBool(value);
    } else if constexpr (std::is_integral_v<T>) {
        if constexpr (std::is_signed_v<T>) {
            std::int64_t raw = 0;
            if (!reader.ReadInt64(raw)) {
                return false;
            }
            if (raw < static_cast<std::int64_t>(std::numeric_limits<T>::min()) ||
                raw > static_cast<std::int64_t>(std::numeric_limits<T>::max())) {
                reader.SetStatus(CodecStatus::kBadPayload);
                return false;
            }
            value = static_cast<T>(raw);
            return true;
        } else {
            std::uint64_t raw = 0;
            if (!reader.ReadUint64(raw)) {
                return false;
            }
            if (raw > static_cast<std::uint64_t>(std::numeric_limits<T>::max())) {
                reader.SetStatus(CodecStatus::kBadPayload);
                return false;
            }
            value = static_cast<T>(raw);
            return true;
        }
    } else if constexpr (std::is_same_v<T, double>) {
        return reader.ReadDouble(value);
    } else if constexpr (std::is_same_v<T, std::string>) {
        return reader.ReadString(value);
    } else if constexpr (std::is_same_v<T, std::vector<std::uint8_t>>) {
        return reader.ReadByteArray(value);
    } else {
        static_assert(kDirectAlwaysFalse<T>, "unsupported builtin type for direct JSON reader");
    }
}

}  // namespace detail

/// @brief 结构体的 JSON 直读（不建树）。
template <typename T>
bool ReadJsonDirectReflect(DirectReader& reader, T& value) {
    if (!reader.Consume('{')) {
        reader.SetStatus(CodecStatus::kBadPayload);
        return false;
    }
    std::size_t count = 0;
    const FieldDescriptor* fields = FieldList<T>::Fields(count);
    std::uint64_t seen = 0;
    reader.SkipWhitespace();
    if (reader.Consume('}')) {
        return true;
    }
    for (;;) {
        std::string_view key;
        bool escaped = false;
        if (!reader.ScanString(key, escaped)) {
            return false;
        }
        std::size_t index = count;
        if (!escaped) {
            for (std::size_t position = 0; position < count; ++position) {
                if (key.size() == fields[position].name_size &&
                    std::memcmp(key.data(), fields[position].name, key.size()) == 0) {
                    index = position;
                    break;
                }
            }
        } else {
            std::string decoded;
            if (!reader.Unescape(key, decoded)) {
                return false;
            }
            for (std::size_t position = 0; position < count; ++position) {
                if (decoded == fields[position].name) {
                    index = position;
                    break;
                }
            }
        }
        if (index == count) {
            // 与建树路径一致：未知字段整条拒绝，而不是静默丢弃。
            reader.SetStatus(CodecStatus::kBadPayload);
            return false;
        }
        if (count <= 64) {
            const std::uint64_t bit = std::uint64_t{1} << index;
            if ((seen & bit) != 0) {
                reader.SetStatus(CodecStatus::kBadText);  // 重复键
                return false;
            }
            seen |= bit;
        }
        reader.SkipWhitespace();
        if (!reader.Consume(':')) {
            reader.SetStatus(CodecStatus::kBadText);
            return false;
        }
        reader.SkipWhitespace();
        if (!fields[index].ops.read_text_direct(reader, &value)) {
            return false;
        }
        reader.SkipWhitespace();
        if (reader.Consume('}')) {
            return true;
        }
        if (!reader.Consume(',')) {
            reader.SetStatus(CodecStatus::kBadText);
            return false;
        }
        reader.SkipWhitespace();
    }
}

/// @brief 按 schema 直接从 JSON 文本读入值（结构体或内置类型）。
template <typename T>
bool ReadJsonDirect(DirectReader& reader, T& value) {
    using Value = std::remove_cv_t<std::remove_reference_t<T>>;
    if constexpr (HasFieldList<Value>::value) {
        return ReadJsonDirectReflect(reader, value);
    } else if constexpr (TypeTraits<Value>::kBuiltin) {
        return detail::ReadJsonDirectBuiltin(reader, value);
    } else {
        reader.SetStatus(CodecStatus::kBadPayload);
        return false;
    }
}

}  // namespace go2cpp::reflect
