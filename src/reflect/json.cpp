/**
 * @file json.cpp
 * @brief JSON-like 文本写入/解析实现。
 * @author go2cpp contributors
 * @date 2026-10-30
 * @copyright MIT License
 */

#include "go2cpp/reflect/json.hpp"

#include <charconv>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace go2cpp::reflect {
namespace {

constexpr char kHexDigits[] = "0123456789abcdef";

bool IsHexDigit(char value) noexcept {
    return (value >= '0' && value <= '9') || (value >= 'a' && value <= 'f') ||
           (value >= 'A' && value <= 'F');
}

int HexValue(char value) noexcept {
    if (value >= '0' && value <= '9') {
        return value - '0';
    }
    if (value >= 'a' && value <= 'f') {
        return value - 'a' + 10;
    }
    return value - 'A' + 10;
}

bool AppendUtf8(std::uint32_t code_point, std::string& out) {
    if (code_point <= 0x7fu) {
        out.push_back(static_cast<char>(code_point));
        return true;
    }
    if (code_point <= 0x7ffu) {
        out.push_back(static_cast<char>(0xc0u | (code_point >> 6)));
        out.push_back(static_cast<char>(0x80u | (code_point & 0x3fu)));
        return true;
    }
    if (code_point <= 0xffffu) {
        out.push_back(static_cast<char>(0xe0u | (code_point >> 12)));
        out.push_back(static_cast<char>(0x80u | ((code_point >> 6) & 0x3fu)));
        out.push_back(static_cast<char>(0x80u | (code_point & 0x3fu)));
        return true;
    }
    if (code_point <= 0x10ffffu) {
        out.push_back(static_cast<char>(0xf0u | (code_point >> 18)));
        out.push_back(static_cast<char>(0x80u | ((code_point >> 12) & 0x3fu)));
        out.push_back(static_cast<char>(0x80u | ((code_point >> 6) & 0x3fu)));
        out.push_back(static_cast<char>(0x80u | (code_point & 0x3fu)));
        return true;
    }
    return false;
}

/// @brief 递归下降解析器；错误信息只保留首个失败点，避免层层包装。
class Parser {
   public:
    explicit Parser(std::string_view text) noexcept : m_text(text) {}

    bool ParseDocument(JsonValue& out) {
        SkipWhitespace();
        if (!ParseValue(out, 0)) {
            return false;
        }
        SkipWhitespace();
        if (m_pos != m_text.size()) {
            return Fail("trailing characters");
        }
        return true;
    }

    const std::string& error() const noexcept {
        return m_error;
    }
    std::size_t position() const noexcept {
        return m_pos;
    }

   private:
    bool Fail(const char* message) {
        if (m_error.empty()) {
            m_error = std::string(message) + " at offset " + std::to_string(m_pos);
        }
        return false;
    }

    void SkipWhitespace() noexcept {
        while (m_pos < m_text.size()) {
            const char value = m_text[m_pos];
            if (value != ' ' && value != '\t' && value != '\n' && value != '\r') {
                break;
            }
            ++m_pos;
        }
    }

    bool Consume(char expected) noexcept {
        if (m_pos >= m_text.size() || m_text[m_pos] != expected) {
            return false;
        }
        ++m_pos;
        return true;
    }

    bool ParseValue(JsonValue& out, std::size_t depth) {
        if (depth > kMaxJsonDepth) {
            return Fail("nesting too deep");
        }
        if (m_pos >= m_text.size()) {
            return Fail("unexpected end of input");
        }
        switch (m_text[m_pos]) {
            case '{':
                return ParseObject(out, depth);
            case '[':
                return ParseArray(out, depth);
            case '"': {
                std::string text;
                if (!ParseString(text)) {
                    return false;
                }
                out.kind = JsonValue::Kind::kString;
                out.text = std::move(text);
                return true;
            }
            case 't':
                return ParseLiteral("true", JsonValue::Kind::kBool, true, out);
            case 'f':
                return ParseLiteral("false", JsonValue::Kind::kBool, false, out);
            case 'n':
                return ParseLiteral("null", JsonValue::Kind::kNull, false, out);
            default:
                return ParseNumber(out);
        }
    }

    bool ParseLiteral(std::string_view literal, JsonValue::Kind kind, bool boolean,
                      JsonValue& out) {
        if (m_text.substr(m_pos, literal.size()) != literal) {
            return Fail("invalid literal");
        }
        m_pos += literal.size();
        out.kind = kind;
        out.boolean = boolean;
        return true;
    }

    bool ParseObject(JsonValue& out, std::size_t depth) {
        if (!Consume('{')) {
            return Fail("expected '{'");
        }
        out.kind = JsonValue::Kind::kObject;
        out.members.clear();
        SkipWhitespace();
        if (Consume('}')) {
            return true;
        }
        for (;;) {
            if (out.members.size() >= kMaxJsonElements) {
                return Fail("object too large");
            }
            std::string key;
            if (!ParseString(key)) {
                return false;
            }
            for (const auto& member : out.members) {
                if (member.first == key) {
                    return Fail("duplicate object key");
                }
            }
            SkipWhitespace();
            if (!Consume(':')) {
                return Fail("expected ':'");
            }
            JsonValue value;
            if (!ParseValue(value, depth + 1)) {
                return false;
            }
            out.members.emplace_back(std::move(key), std::move(value));
            SkipWhitespace();
            if (Consume('}')) {
                return true;
            }
            if (!Consume(',')) {
                return Fail("expected ',' or '}'");
            }
            SkipWhitespace();
        }
    }

    bool ParseArray(JsonValue& out, std::size_t depth) {
        if (!Consume('[')) {
            return Fail("expected '['");
        }
        out.kind = JsonValue::Kind::kArray;
        out.elements.clear();
        SkipWhitespace();
        if (Consume(']')) {
            return true;
        }
        for (;;) {
            if (out.elements.size() >= kMaxJsonElements) {
                return Fail("array too large");
            }
            JsonValue value;
            if (!ParseValue(value, depth + 1)) {
                return false;
            }
            out.elements.push_back(std::move(value));
            SkipWhitespace();
            if (Consume(']')) {
                return true;
            }
            if (!Consume(',')) {
                return Fail("expected ',' or ']'");
            }
            SkipWhitespace();
        }
    }

    bool ParseString(std::string& out) {
        if (!Consume('"')) {
            return Fail("expected string");
        }
        out.clear();
        while (m_pos < m_text.size()) {
            const char value = m_text[m_pos++];
            if (value == '"') {
                return true;
            }
            if (value == '\\') {
                if (!ParseEscape(out)) {
                    return false;
                }
                continue;
            }
            if (static_cast<unsigned char>(value) < 0x20u) {
                return Fail("control character in string");
            }
            if (out.size() >= kMaxJsonStringBytes) {
                return Fail("string too large");
            }
            out.push_back(value);
        }
        return Fail("unterminated string");
    }

    bool ParseEscape(std::string& out) {
        if (m_pos >= m_text.size()) {
            return Fail("unterminated escape");
        }
        const char value = m_text[m_pos++];
        switch (value) {
            case '"':
                out.push_back('"');
                return true;
            case '\\':
                out.push_back('\\');
                return true;
            case '/':
                out.push_back('/');
                return true;
            case 'b':
                out.push_back('\b');
                return true;
            case 'f':
                out.push_back('\f');
                return true;
            case 'n':
                out.push_back('\n');
                return true;
            case 'r':
                out.push_back('\r');
                return true;
            case 't':
                out.push_back('\t');
                return true;
            case 'u':
                return ParseUnicodeEscape(out);
            default:
                return Fail("invalid escape");
        }
    }

    bool ParseHex4(std::uint32_t& out) noexcept {
        if (m_pos + 4 > m_text.size()) {
            return false;
        }
        std::uint32_t value = 0;
        for (std::size_t index = 0; index < 4; ++index) {
            const char digit = m_text[m_pos + index];
            if (!IsHexDigit(digit)) {
                return false;
            }
            value = (value << 4) | static_cast<std::uint32_t>(HexValue(digit));
        }
        m_pos += 4;
        out = value;
        return true;
    }

    bool ParseUnicodeEscape(std::string& out) {
        std::uint32_t code_point = 0;
        if (!ParseHex4(code_point)) {
            return Fail("invalid \\u escape");
        }
        if (code_point >= 0xd800u && code_point <= 0xdbffu) {
            // UTF-16 代理对：必须紧跟一个低代理。
            if (m_pos + 2 > m_text.size() || m_text[m_pos] != '\\' || m_text[m_pos + 1] != 'u') {
                return Fail("missing low surrogate");
            }
            m_pos += 2;
            std::uint32_t low = 0;
            if (!ParseHex4(low) || low < 0xdc00u || low > 0xdfffu) {
                return Fail("invalid low surrogate");
            }
            code_point = 0x10000u + ((code_point - 0xd800u) << 10) + (low - 0xdc00u);
        } else if (code_point >= 0xdc00u && code_point <= 0xdfffu) {
            return Fail("unexpected low surrogate");
        }
        if (!AppendUtf8(code_point, out)) {
            return Fail("code point out of range");
        }
        return true;
    }

    bool ParseNumber(JsonValue& out) {
        const std::size_t start = m_pos;
        if (m_pos < m_text.size() && m_text[m_pos] == '-') {
            ++m_pos;
        }
        if (m_pos >= m_text.size()) {
            return Fail("invalid number");
        }
        if (m_text[m_pos] == '0') {
            ++m_pos;
        } else if (m_text[m_pos] >= '1' && m_text[m_pos] <= '9') {
            while (m_pos < m_text.size() && m_text[m_pos] >= '0' && m_text[m_pos] <= '9') {
                ++m_pos;
            }
        } else {
            return Fail("invalid number");
        }
        if (m_pos < m_text.size() && m_text[m_pos] == '.') {
            ++m_pos;
            const std::size_t digits = m_pos;
            while (m_pos < m_text.size() && m_text[m_pos] >= '0' && m_text[m_pos] <= '9') {
                ++m_pos;
            }
            if (m_pos == digits) {
                return Fail("invalid fraction");
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
                return Fail("invalid exponent");
            }
        }
        out.kind = JsonValue::Kind::kNumber;
        out.text.assign(m_text.data() + start, m_pos - start);
        return true;
    }

    std::string_view m_text;  ///< 输入文本。
    std::size_t m_pos = 0;    ///< 当前解析位置。
    std::string m_error;      ///< 首个错误。
};

template <typename T>
bool ParseNumberText(std::string_view text, T& out) noexcept {
    if (text.empty() || text.front() == '+') {
        return false;
    }
    T value{};
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    if (result.ec != std::errc() || result.ptr != text.data() + text.size()) {
        return false;
    }
    out = value;
    return true;
}

}  // namespace

std::string EscapeJsonString(std::string_view text) {
    std::string out;
    out.reserve(text.size() + 8);
    for (const unsigned char value : text) {
        switch (value) {
            case '"':
                out += "\\\"";
                break;
            case '\\':
                out += "\\\\";
                break;
            case '\b':
                out += "\\b";
                break;
            case '\f':
                out += "\\f";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\r':
                out += "\\r";
                break;
            case '\t':
                out += "\\t";
                break;
            default:
                if (value < 0x20u) {
                    out += "\\u00";
                    out.push_back(kHexDigits[(value >> 4) & 0x0fu]);
                    out.push_back(kHexDigits[value & 0x0fu]);
                } else {
                    out.push_back(static_cast<char>(value));
                }
                break;
        }
    }
    return out;
}

const JsonValue* JsonValue::Find(std::string_view key) const noexcept {
    if (kind != Kind::kObject) {
        return nullptr;
    }
    for (const auto& member : members) {
        if (member.first == key) {
            return &member.second;
        }
    }
    return nullptr;
}

JsonValue JsonValue::MakeObject() {
    JsonValue value;
    value.kind = Kind::kObject;
    return value;
}

JsonValue JsonValue::MakeArray() {
    JsonValue value;
    value.kind = Kind::kArray;
    return value;
}

void JsonWriter::BeforeValue() {
    if (!m_ok) {
        return;
    }
    if (m_after_key) {
        m_after_key = false;
        return;
    }
    if (m_scopes.empty()) {
        return;
    }
    if (m_scopes.back().object) {
        // 对象内必须先写 Key 再写值，否则不是合法 JSON。
        Fail();
        return;
    }
    if (!m_scopes.back().first) {
        m_out += ',';
    }
    m_scopes.back().first = false;
}

void JsonWriter::AppendEscaped(std::string_view text) {
    m_out += EscapeJsonString(text);
}

void JsonWriter::BeginObject() {
    if (!m_ok) {
        return;
    }
    BeforeValue();
    if (!m_ok) {
        return;
    }
    m_out += '{';
    m_scopes.push_back(Scope{true, true});
}

void JsonWriter::EndObject() {
    if (!m_ok || m_scopes.empty() || !m_scopes.back().object || m_after_key) {
        Fail();
        return;
    }
    m_out += '}';
    m_scopes.pop_back();
}

void JsonWriter::BeginArray() {
    if (!m_ok) {
        return;
    }
    BeforeValue();
    if (!m_ok) {
        return;
    }
    m_out += '[';
    m_scopes.push_back(Scope{true, false});
}

void JsonWriter::EndArray() {
    if (!m_ok || m_scopes.empty() || m_scopes.back().object) {
        Fail();
        return;
    }
    m_out += ']';
    m_scopes.pop_back();
}

void JsonWriter::Key(std::string_view key) {
    if (!m_ok || m_scopes.empty() || !m_scopes.back().object || m_after_key) {
        Fail();
        return;
    }
    if (!m_scopes.back().first) {
        m_out += ',';
    }
    m_scopes.back().first = false;
    m_out += '"';
    AppendEscaped(key);
    m_out += "\":";
    m_after_key = true;
}

void JsonWriter::String(std::string_view value) {
    if (!m_ok) {
        return;
    }
    BeforeValue();
    if (!m_ok) {
        return;
    }
    m_out += '"';
    AppendEscaped(value);
    m_out += '"';
}

void JsonWriter::Int(std::int64_t value) {
    if (!m_ok) {
        return;
    }
    char buffer[32] = {};
    const auto result = std::to_chars(buffer, buffer + sizeof(buffer), value);
    if (result.ec != std::errc()) {
        Fail();
        return;
    }
    BeforeValue();
    if (!m_ok) {
        return;
    }
    m_out.append(buffer, result.ptr);
}

void JsonWriter::Uint(std::uint64_t value) {
    if (!m_ok) {
        return;
    }
    char buffer[32] = {};
    const auto result = std::to_chars(buffer, buffer + sizeof(buffer), value);
    if (result.ec != std::errc()) {
        Fail();
        return;
    }
    BeforeValue();
    if (!m_ok) {
        return;
    }
    m_out.append(buffer, result.ptr);
}

void JsonWriter::Double(double value) {
    if (!m_ok) {
        return;
    }
    if (!std::isfinite(value)) {
        Fail();
        return;
    }
    char buffer[64] = {};
    const auto result =
        std::to_chars(buffer, buffer + sizeof(buffer), value, std::chars_format::general);
    if (result.ec != std::errc()) {
        Fail();
        return;
    }
    BeforeValue();
    if (!m_ok) {
        return;
    }
    m_out.append(buffer, result.ptr);
}

void JsonWriter::NumberLiteral(std::string_view literal) {
    if (!m_ok || literal.empty()) {
        Fail();
        return;
    }
    for (const char value : literal) {
        const bool allowed = (value >= '0' && value <= '9') || value == '-' || value == '+' ||
                             value == '.' || value == 'e' || value == 'E';
        if (!allowed) {
            Fail();
            return;
        }
    }
    BeforeValue();
    if (!m_ok) {
        return;
    }
    m_out.append(literal.data(), literal.size());
}

void JsonWriter::Bool(bool value) {
    if (!m_ok) {
        return;
    }
    BeforeValue();
    if (!m_ok) {
        return;
    }
    m_out += value ? "true" : "false";
}

void JsonWriter::Null() {
    if (!m_ok) {
        return;
    }
    BeforeValue();
    if (!m_ok) {
        return;
    }
    m_out += "null";
}

bool WriteJsonValue(const JsonValue& value, JsonWriter& out) {
    switch (value.kind) {
        case JsonValue::Kind::kNull:
            out.Null();
            break;
        case JsonValue::Kind::kBool:
            out.Bool(value.boolean);
            break;
        case JsonValue::Kind::kNumber:
            out.NumberLiteral(value.text);
            break;
        case JsonValue::Kind::kString:
            out.String(value.text);
            break;
        case JsonValue::Kind::kArray: {
            out.BeginArray();
            for (const auto& element : value.elements) {
                if (!WriteJsonValue(element, out)) {
                    return false;
                }
            }
            out.EndArray();
            break;
        }
        case JsonValue::Kind::kObject: {
            out.BeginObject();
            for (const auto& member : value.members) {
                out.Key(member.first);
                if (!WriteJsonValue(member.second, out)) {
                    return false;
                }
            }
            out.EndObject();
            break;
        }
    }
    return out.ok();
}

bool JsonParser::Parse(std::string_view text, JsonValue& out, std::size_t* consumed,
                       std::string* error) {
    Parser parser(text);
    JsonValue value;
    if (!parser.ParseDocument(value)) {
        if (error != nullptr) {
            *error = parser.error();
        }
        return false;
    }
    out = std::move(value);
    if (consumed != nullptr) {
        *consumed = parser.position();
    }
    return true;
}

bool JsonNumberToInt64(const JsonValue& value, std::int64_t& out) noexcept {
    if (value.kind != JsonValue::Kind::kNumber) {
        return false;
    }
    return ParseNumberText<std::int64_t>(value.text, out);
}

bool JsonNumberToUint64(const JsonValue& value, std::uint64_t& out) noexcept {
    if (value.kind != JsonValue::Kind::kNumber) {
        return false;
    }
    return ParseNumberText<std::uint64_t>(value.text, out);
}

bool JsonNumberToDouble(const JsonValue& value, double& out) noexcept {
    if (value.kind != JsonValue::Kind::kNumber) {
        return false;
    }
    return ParseNumberText<double>(value.text, out);
}

}  // namespace go2cpp::reflect
