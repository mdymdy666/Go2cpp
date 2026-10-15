/**
 * @file strconv.hpp
 * @brief Go `strconv` 包的 C++ 对应实现（语义对齐，独立实现）。
 *
 * @details 复用优先的取舍（沿用 `strings.hpp` 的结论）：
 *   C++ 侧没有与 Go `strconv` 语义对齐的现成库（`std::stoi` 不做 base 推断、
 *   不报 errno 风格错误、不返回"范围钳制值"；`std::from_chars` 不做 base=0 推断与下划线），
 *   因此本模块自研，底层复用 `std::from_chars`/`std::to_chars`（C++17）保证 round-trip 精度。
 *
 *   语义对照来源：本机 `third_party/go1.23.0.src.tar.gz` 内的 `go/src/strconv/`
 *   （Go 1.23.0，BSD-3-Clause）。**本实现为独立实现，未复制上游代码**。
 *   已对照并落实的非显然行为：
 *     - `ParseBool` 的接受集**只有** 12 个字符串（`1 t T true TRUE True` / `0 f F false FALSE False`），
 *       连 `"True "`（带空格）都是语法错误；
 *     - `ParseInt/ParseUint` 的 `base == 0` 会**推断进制**：`0b`/`0o`/`0x` 前缀，
 *       以及"`0` 开头但无字母前缀" → **八进制**（`"0755"` → 493）；
 *     - **下划线 `_` 只在 `base == 0` 时被接受**，且必须位于数字之间（`underscoreOK` 规则）；
 *     - 越界返回 `ErrRange` **并给出钳制值**（`ParseInt("999", 10, 8)` → 127 + ErrRange）；
 *     - `ParseFloat` 接受 `inf`/`infinity`/`nan`（大小写不敏感），**不接受前后空格**。
 *
 * @note **边界**（后续可补）：
 *       - `FormatFloat` 只实现 `'f'`/`'e'`/`'g'`（Go 的 `'b'`/`'x'`/`'X'` 抛 `std::logic_error`）；
 *       - `Quote` 覆盖 ASCII 可打印 + 常见转义 + `\xNN`/`\uNNNN`，不做 Go 的完整 Unicode `IsPrint` 表。
 * @note 本模块依赖 `ErrorPtr`（`go2cpp/error.hpp`），错误按 Go 的 `ErrSyntax`/`ErrRange` 两类锚点区分，
 *       判断用身份比较 `Is(err, StrconvErrSyntax())`（与全项目一致，不做文本比较）。
 */

#pragma once

#include "go2cpp/error.hpp"

#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>

namespace go2cpp {

// ============================================================================
// 错误锚点（对应 Go 的 ErrSyntax / ErrRange）
// ============================================================================

/**
 * @brief 对应 `strconv.ErrSyntax` 的进程级身份锚点。
 * @note 故意泄漏（`new` 后不析构）：与 `JsonDepthExceededError` 同理，
 *       避免静态析构顺序让退出阶段的身份比较退化为「与空指针比较」。
 */
inline ErrorPtr StrconvErrSyntax() {
    static const ErrorPtr* const kError = new ErrorPtr(NewError("strconv: invalid syntax"));
    return *kError;
}

/// 对应 `strconv.ErrRange` 的进程级身份锚点。
inline ErrorPtr StrconvErrRange() {
    static const ErrorPtr* const kError = new ErrorPtr(NewError("strconv: value out of range"));
    return *kError;
}

namespace detail {

/// 构造一条 `Wrap(锚点, "strconv.Xxx: parsing \"<num>\"")` 形式的错误（对齐 Go 的 `NumError`）。
inline ErrorPtr StrconvError(const ErrorPtr& kind, const char* function, std::string_view num) {
    std::string message = "strconv.";
    message += function;
    message += ": parsing ";
    message += std::string(num);
    return Wrap(kind, std::move(message));
}

/// `underscoreOK` 的等价实现（Go `atoi.go`）：下划线必须在数字之间，且不能出现在进制前缀前。
inline bool UnderscoreOk(std::string_view text) {
    char saw = '^';                       // '^' 起始 / '0' 数字（含进制前缀）/ '_' 下划线 / '!' 其它
    std::size_t i = 0;
    if (!text.empty() && (text[0] == '-' || text[0] == '+')) text.remove_prefix(1);
    bool hex = false;
    if (text.size() >= 2 && text[0] == '0' &&
        (text[1] == 'b' || text[1] == 'B' || text[1] == 'o' || text[1] == 'O' ||
         text[1] == 'x' || text[1] == 'X')) {
        i = 2;
        saw = '0';
        hex = (text[1] == 'x' || text[1] == 'X');
    }
    for (; i < text.size(); ++i) {
        const char c = text[i];
        const bool digit = (c >= '0' && c <= '9') ||
                           (hex && ((c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')));
        if (digit) {
            saw = '0';
            continue;
        }
        if (c == '_') {
            if (saw != '0') return false;
            saw = '_';
            continue;
        }
        if (saw == '_') return false;
        saw = '!';
    }
    return saw != '_';
}

/// 单个字符到数字值（`base` 进制内合法时返回 0..35，否则 -1）。
inline int DigitValue(char c, int base) noexcept {
    int value = -1;
    if (c >= '0' && c <= '9') value = c - '0';
    else if (c >= 'a' && c <= 'z') value = c - 'a' + 10;
    else if (c >= 'A' && c <= 'Z') value = c - 'A' + 10;
    if (value < 0 || value >= base) return -1;
    return value;
}

}  // namespace detail

// ============================================================================
// 结果类型（对应 Go 的多返回值 `(value, error)`）
// ============================================================================

/** @brief `Atoi`/`ParseInt` 的结果。 */
struct StrconvIntResult {
    std::int64_t value{0};
    ErrorPtr error;
    bool ok() const noexcept { return !error; }
};

/** @brief `ParseUint` 的结果。 */
struct StrconvUintResult {
    std::uint64_t value{0};
    ErrorPtr error;
    bool ok() const noexcept { return !error; }
};

/** @brief `ParseFloat` 的结果。 */
struct StrconvFloatResult {
    double value{0};
    ErrorPtr error;
    bool ok() const noexcept { return !error; }
};

/** @brief `ParseBool` 的结果。 */
struct StrconvBoolResult {
    bool value{false};
    ErrorPtr error;
    bool ok() const noexcept { return !error; }
};

// ============================================================================
// 解析
// ============================================================================

/**
 * @brief 对应 `strconv.ParseUint`。
 * @param text 待解析文本（不允许前后空格）。
 * @param base 2..36，或 0 表示自动推断（`0b`/`0o`/`0x` 前缀，或前导 `0` → 八进制）。
 * @param bit_size 0（视作 64）/8/16/32/64；越界值时返回 `ErrRange` **并给出钳制值**。
 */
inline StrconvUintResult ParseUint(std::string_view text, int base, int bit_size) {
    StrconvUintResult result;
    if (text.empty()) {
        result.error = detail::StrconvError(StrconvErrSyntax(), "ParseUint", text);
        return result;
    }
    const std::string_view original = text;
    const bool base_zero = (base == 0);
    if (base_zero) {
        base = 10;
        if (text[0] == '0') {
            if (text.size() >= 3 && (text[1] == 'b' || text[1] == 'B')) {
                base = 2;
                text.remove_prefix(2);
            } else if (text.size() >= 3 && (text[1] == 'o' || text[1] == 'O')) {
                base = 8;
                text.remove_prefix(2);
            } else if (text.size() >= 3 && (text[1] == 'x' || text[1] == 'X')) {
                base = 16;
                text.remove_prefix(2);
            } else {
                base = 8;                      // ★ Go 语义：前导 0 但无字母前缀 → 八进制
                text.remove_prefix(1);
            }
        }
    } else if (base < 2 || base > 36) {
        result.error = detail::StrconvError(StrconvErrSyntax(), "ParseUint", original);
        return result;
    }
    if (bit_size == 0) bit_size = 64;
    if (bit_size < 0 || bit_size > 64) {
        result.error = detail::StrconvError(StrconvErrRange(), "ParseUint", original);
        return result;
    }
    if (text.empty()) {
        result.error = detail::StrconvError(StrconvErrSyntax(), "ParseUint", original);
        return result;
    }

    bool saw_underscore = false;
    std::string cleaned;
    cleaned.reserve(text.size());
    for (const char c : text) {
        if (c == '_') {
            if (!base_zero) {                  // ★ 只有 base==0 才接受下划线
                result.error = detail::StrconvError(StrconvErrSyntax(), "ParseUint", original);
                return result;
            }
            saw_underscore = true;
            continue;
        }
        cleaned.push_back(c);
    }
    if (cleaned.empty()) {
        result.error = detail::StrconvError(StrconvErrSyntax(), "ParseUint", original);
        return result;
    }
    if (saw_underscore && !detail::UnderscoreOk(original)) {
        result.error = detail::StrconvError(StrconvErrSyntax(), "ParseUint", original);
        return result;
    }

    // 手动累加：既做语法校验，也做"越界时给出钳制值"（Go 行为）。
    const std::uint64_t max_value = bit_size == 64
                                        ? std::numeric_limits<std::uint64_t>::max()
                                        : ((std::uint64_t{1} << bit_size) - 1U);
    const std::uint64_t cutoff = max_value / static_cast<std::uint64_t>(base);
    const int max_digit = static_cast<int>(max_value % static_cast<std::uint64_t>(base));
    std::uint64_t accumulator = 0;
    bool overflow = false;
    for (const char c : cleaned) {
        const int digit = detail::DigitValue(c, base);
        if (digit < 0) {
            result.error = detail::StrconvError(StrconvErrSyntax(), "ParseUint", original);
            return result;
        }
        if (accumulator > cutoff || (accumulator == cutoff && digit > max_digit)) {
            overflow = true;
            break;
        }
        accumulator = accumulator * static_cast<std::uint64_t>(base) + static_cast<std::uint64_t>(digit);
    }
    if (overflow) {
        result.value = max_value;
        result.error = detail::StrconvError(StrconvErrRange(), "ParseUint", original);
        return result;
    }
    result.value = accumulator;
    return result;
}

/**
 * @brief 对应 `strconv.ParseInt`。
 * @details 负号只在 `bitSize` 的**有符号**范围内检查（Go 用 `cutoff = 1 << (bitSize-1)`，
 *          允许 `-128` 但 `+127` 已是 `ErrRange`）。
 */
inline StrconvIntResult ParseInt(std::string_view text, int base, int bit_size) {
    StrconvIntResult result;
    if (text.empty()) {
        result.error = detail::StrconvError(StrconvErrSyntax(), "ParseInt", text);
        return result;
    }
    const std::string_view original = text;
    bool negative = false;
    if (text[0] == '+') {
        text.remove_prefix(1);
    } else if (text[0] == '-') {
        negative = true;
        text.remove_prefix(1);
    }
    const StrconvUintResult magnitude = ParseUint(text, base, bit_size);
    if (!magnitude.ok() && !Is(magnitude.error, StrconvErrRange())) {
        result.error = detail::StrconvError(StrconvErrSyntax(), "ParseInt", original);
        return result;
    }
    const int size = (bit_size == 0) ? 64 : bit_size;
    const std::uint64_t cutoff = std::uint64_t{1} << static_cast<unsigned>(size - 1);
    if (!negative && magnitude.value >= cutoff) {
        result.value = static_cast<std::int64_t>(cutoff - 1);
        result.error = detail::StrconvError(StrconvErrRange(), "ParseInt", original);
        return result;
    }
    if (negative && magnitude.value > cutoff) {
        result.value = -static_cast<std::int64_t>(cutoff);
        result.error = detail::StrconvError(StrconvErrRange(), "ParseInt", original);
        return result;
    }
    if (!magnitude.ok()) {                       // 上游已给出 ErrRange 的钳制值
        result.value = static_cast<std::int64_t>(magnitude.value);
        result.error = magnitude.error;
        return result;
    }
    std::int64_t value = static_cast<std::int64_t>(magnitude.value);
    if (negative) value = -value;
    result.value = value;
    return result;
}

/// 对应 `strconv.Atoi`（等价于 `ParseInt(s, 10, 0)`）。
inline StrconvIntResult Atoi(std::string_view text) { return ParseInt(text, 10, 0); }

/**
 * @brief 对应 `strconv.ParseBool`。
 * @details 接受集**精确**为：`1 t T true TRUE True` / `0 f F false FALSE False`；其余（含带空格）→ `ErrSyntax`。
 */
inline StrconvBoolResult ParseBool(std::string_view text) {
    StrconvBoolResult result;
    if (text == "1" || text == "t" || text == "T" || text == "true" || text == "TRUE" ||
        text == "True") {
        result.value = true;
        return result;
    }
    if (text == "0" || text == "f" || text == "F" || text == "false" || text == "FALSE" ||
        text == "False") {
        result.value = false;
        return result;
    }
    result.error = detail::StrconvError(StrconvErrSyntax(), "ParseBool", text);
    return result;
}

/// 内部：大小写不敏感的前缀比较（供特殊浮点值识别）。
namespace detail {
inline bool EqualIgnoreCase(std::string_view text, std::string_view expected) {
    if (text.size() != expected.size()) return false;
    for (std::size_t i = 0; i < text.size(); ++i) {
        char a = text[i];
        char b = expected[i];
        if (a >= 'A' && a <= 'Z') a = static_cast<char>(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z') b = static_cast<char>(b - 'A' + 'a');
        if (a != b) return false;
    }
    return true;
}
}  // namespace detail

/**
 * @brief 对应 `strconv.ParseFloat`。
 * @details 接受十进制/十六进制浮点、科学计数法、数字间的下划线，以及
 *          `inf`/`infinity`/`nan`（大小写不敏感，可带符号）；**不接受前后空格**。
 * @param bit_size 32 或 64（32 时按 `float` 精度校验，超出返回 `ErrRange`）。
 */
inline StrconvFloatResult ParseFloat(std::string_view text, int bit_size) {
    StrconvFloatResult result;
    if (bit_size != 32 && bit_size != 64) {
        result.error = detail::StrconvError(StrconvErrSyntax(), "ParseFloat", text);
        return result;
    }
    const std::string_view original = text;
    if (text.empty()) {
        result.error = detail::StrconvError(StrconvErrSyntax(), "ParseFloat", text);
        return result;
    }

    // 符号剥离（std::from_chars 不接受前导 '+'）。
    bool negative = false;
    std::string_view body = text;
    if (body[0] == '+' || body[0] == '-') {
        negative = body[0] == '-';
        body.remove_prefix(1);
        if (body.empty()) {
            result.error = detail::StrconvError(StrconvErrSyntax(), "ParseFloat", original);
            return result;
        }
    }

    // 特殊值（Go：inf / infinity / nan，大小写不敏感）。
    if (detail::EqualIgnoreCase(body, "inf") || detail::EqualIgnoreCase(body, "infinity")) {
        result.value = negative ? -std::numeric_limits<double>::infinity()
                                : std::numeric_limits<double>::infinity();
        return result;
    }
    if (detail::EqualIgnoreCase(body, "nan")) {
        result.value = std::numeric_limits<double>::quiet_NaN();
        return result;
    }

    // 下划线：按 Go 规则校验后剥离（from_chars 不认识 '_'）。
    std::string cleaned;
    bool saw_underscore = false;
    for (const char c : body) {
        if (c == '_') {
            saw_underscore = true;
            continue;
        }
        cleaned.push_back(c);
    }
    if (cleaned.empty()) {
        result.error = detail::StrconvError(StrconvErrSyntax(), "ParseFloat", original);
        return result;
    }
    if (saw_underscore && !detail::UnderscoreOk(body)) {
        result.error = detail::StrconvError(StrconvErrSyntax(), "ParseFloat", original);
        return result;
    }

    bool hex = cleaned.size() > 2 && cleaned[0] == '0' &&
               (cleaned[1] == 'x' || cleaned[1] == 'X');
    if (hex) {
        // ★ std::from_chars 的 hex 浮点格式**不含** `0x` 前缀，而 Go 的输入含前缀。
        cleaned.erase(0, 2);
    }
    double value = 0;
    const char* const begin = cleaned.data();
    const char* const end = cleaned.data() + cleaned.size();
    const auto format = hex ? std::chars_format::hex : std::chars_format::general;
    const auto parsed = std::from_chars(begin, end, value, format);
    if (parsed.ec != std::errc() || parsed.ptr != end) {
        result.error = detail::StrconvError(StrconvErrSyntax(), "ParseFloat", original);
        return result;
    }
    if (negative) value = -value;
    if (bit_size == 32) {
        const float narrowed = static_cast<float>(value);
        if (std::isinf(narrowed) && !std::isinf(value)) {
            result.value = narrowed > 0 ? std::numeric_limits<double>::infinity()
                                        : -std::numeric_limits<double>::infinity();
            result.error = detail::StrconvError(StrconvErrRange(), "ParseFloat", original);
            return result;
        }
        value = static_cast<double>(narrowed);
    }
    result.value = value;
    return result;
}

// ============================================================================
// 格式化
// ============================================================================

/// 对应 `strconv.FormatInt`（`base` 2..36，小写字母；非法 base 抛 `std::logic_error`）。
inline std::string FormatInt(std::int64_t value, int base) {
    if (base < 2 || base > 36) throw std::logic_error("strconv.FormatInt: invalid base");
    const bool negative = value < 0;
    std::uint64_t magnitude = negative
                                  ? (~static_cast<std::uint64_t>(value) + 1U)   // 避免 INT64_MIN 取负溢出
                                  : static_cast<std::uint64_t>(value);
    char buffer[70];
    char* cursor = buffer + sizeof(buffer);
    do {
        const int digit = static_cast<int>(magnitude % static_cast<std::uint64_t>(base));
        *--cursor = static_cast<char>(digit < 10 ? ('0' + digit) : ('a' + digit - 10));
        magnitude /= static_cast<std::uint64_t>(base);
    } while (magnitude != 0);
    if (negative) *--cursor = '-';
    return std::string(cursor, buffer + sizeof(buffer));
}

/// 对应 `strconv.FormatUint`。
inline std::string FormatUint(std::uint64_t value, int base) {
    if (base < 2 || base > 36) throw std::logic_error("strconv.FormatUint: invalid base");
    char buffer[70];
    char* cursor = buffer + sizeof(buffer);
    do {
        const int digit = static_cast<int>(value % static_cast<std::uint64_t>(base));
        *--cursor = static_cast<char>(digit < 10 ? ('0' + digit) : ('a' + digit - 10));
        value /= static_cast<std::uint64_t>(base);
    } while (value != 0);
    return std::string(cursor, buffer + sizeof(buffer));
}

/// 对应 `strconv.Itoa`（等价 `FormatInt(i, 10)`）。
inline std::string Itoa(std::int64_t value) { return FormatInt(value, 10); }

/// 对应 `strconv.FormatBool`。
inline std::string FormatBool(bool value) { return value ? "true" : "false"; }

/**
 * @brief 对应 `strconv.FormatFloat`（子集）。
 * @param format `'f'`（定点）、`'e'`（指数）、`'g'`（通用/最短）。
 * @param precision `'f'`/`'e'` 为小数位数；`'g'` 为有效数字位；**-1 表示最短可往返表示**。
 * @param bit_size 32 或 64。
 * @note Go 的 `'b'`/`'x'`/`'X'` 未实现 —— 抛 `std::logic_error`（已在文件头列为边界）。
 */
inline std::string FormatFloat(double value, char format, int precision, int bit_size) {
    if (bit_size != 32 && bit_size != 64) throw std::logic_error("strconv.FormatFloat: bad bitSize");
    if (format != 'f' && format != 'e' && format != 'g') {
        throw std::logic_error("strconv.FormatFloat: only 'f'/'e'/'g' are supported");
    }
    if (std::isnan(value)) return "NaN";
    if (std::isinf(value)) return value < 0 ? "-Inf" : "+Inf";

    const double rounded = (bit_size == 32) ? static_cast<double>(static_cast<float>(value)) : value;
    if (format == 'g' && precision == -1) {
        // 最短可往返表示：C++17 的 to_chars 默认即 shortest round-trip。
        char buffer[64];
        const auto converted = std::to_chars(buffer, buffer + sizeof(buffer), rounded);
        if (converted.ec == std::errc()) return std::string(buffer, converted.ptr);
        throw std::runtime_error("strconv.FormatFloat: to_chars failed");
    }

    // 'f' / 'e' 用定点或科学计数法；'g' 带 precision 时用 to_chars 的 precision 重载。
    char buffer[512];
    const int effective = precision < 0 ? 6 : precision;
    if (format == 'f' || format == 'e') {
        const auto converted = std::to_chars(buffer, buffer + sizeof(buffer), rounded,
                                             format == 'f' ? std::chars_format::fixed
                                                           : std::chars_format::scientific,
                                             effective);
        if (converted.ec == std::errc()) return std::string(buffer, converted.ptr);
    } else {
        const auto converted = std::to_chars(buffer, buffer + sizeof(buffer), rounded,
                                             std::chars_format::general, effective);
        if (converted.ec == std::errc()) return std::string(buffer, converted.ptr);
    }
    throw std::runtime_error("strconv.FormatFloat: to_chars failed");
}

/**
 * @brief 对应 `strconv.Quote`（子集：双引号 + Go 常用转义）。
 * @note ASCII 可打印字符直出；`\a\b\f\n\r\t\v\\\"` 用短转义；
 *       其余字节用 `\xNN`，非 ASCII 的合法 UTF-8 序列用 `\uNNNN`（见文件头边界说明）。
 */
inline std::string Quote(std::string_view text) {
    std::string result;
    result.reserve(text.size() + 2);
    result.push_back('"');
    for (std::size_t i = 0; i < text.size();) {
        const auto byte = static_cast<unsigned char>(text[i]);
        switch (byte) {
            case '\a': result += "\\a"; ++i; continue;
            case '\b': result += "\\b"; ++i; continue;
            case '\f': result += "\\f"; ++i; continue;
            case '\n': result += "\\n"; ++i; continue;
            case '\r': result += "\\r"; ++i; continue;
            case '\t': result += "\\t"; ++i; continue;
            case '\v': result += "\\v"; ++i; continue;
            case '\\': result += "\\\\"; ++i; continue;
            case '"': result += "\\\""; ++i; continue;
            default: break;
        }
        if (byte >= 0x20U && byte < 0x7FU) {
            result.push_back(static_cast<char>(byte));
            ++i;
            continue;
        }
        static const char* const kHexDigits = "0123456789abcdef";
        result += "\\x";
        result.push_back(kHexDigits[(byte >> 4) & 0x0FU]);
        result.push_back(kHexDigits[byte & 0x0FU]);
        ++i;
    }
    result.push_back('"');
    return result;
}

/// 对应 `strconv.AppendInt` 的便捷形式（本项目直接返回字符串，语义等价于 `Itoa` 的 base 版本）。
inline std::string AppendInt(std::int64_t value, int base) { return FormatInt(value, base); }

}  // namespace go2cpp
