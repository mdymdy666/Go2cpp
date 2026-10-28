#pragma once

/**
 * @file caster.hpp
 * @brief 字符串到标量的严格解析器（int/double/bool/string）。
 *
 * @details “严格”指：
 * - 必须整串匹配，不做前后空白裁剪，不忽略尾随垃圾；
 * - 拒绝 `inf`/`nan`/`0x10` 等非十进制普通字面量；
 * - 整数溢出、浮点上溢/下溢返回 false，不返回截断值；
 * - 布尔只接受 `true/false/yes/no/1/0`（ASCII 大小写不敏感）。
 *
 *          解析不读进程 locale（不调用 `strtod`），因此 "1,5" 在德语环境
 *          下也不会被误判为 1.5。
 *
 * @author go2cpp contributors
 * @date 2026-10-28
 * @copyright MIT License
 */

#include <charconv>
#include <cmath>
#include <cstddef>
#include <limits>
#include <string>
#include <string_view>
#include <system_error>

namespace go2cpp::bento::reflect::caster {

namespace detail {

/// @brief 严格十进制整数解析：`[+-]?[0-9]+`，范围必须落在 int 内。
inline bool ParseInt(std::string_view text, int& out) noexcept {
    if (text.empty()) {
        return false;
    }
    std::size_t pos = 0;
    bool negative = false;
    if (text[pos] == '+' || text[pos] == '-') {
        negative = text[pos] == '-';
        ++pos;
    }
    if (pos >= text.size()) {
        return false;
    }

    const unsigned long long positive_limit =
        static_cast<unsigned long long>(std::numeric_limits<int>::max());
    const unsigned long long negative_limit = positive_limit + 1ULL;
    const unsigned long long limit = negative ? negative_limit : positive_limit;
    unsigned long long magnitude = 0;
    for (; pos < text.size(); ++pos) {
        const char raw = text[pos];
        if (raw < '0' || raw > '9') {
            return false;
        }
        const auto digit = static_cast<unsigned long long>(raw - '0');
        if (magnitude > (limit - digit) / 10ULL) {
            return false;
        }
        magnitude = magnitude * 10ULL + digit;
    }

    if (negative) {
        out = magnitude == negative_limit ? std::numeric_limits<int>::min()
                                          : -static_cast<int>(magnitude);
    } else {
        out = static_cast<int>(magnitude);
    }
    return true;
}

/// @brief 浮点字面量语法检查：拒绝 inf/nan/hex/空白与空串。
inline bool IsValidDoubleSyntax(std::string_view text) noexcept {
    if (text.empty()) {
        return false;
    }
    std::size_t pos = 0;
    if (text[pos] == '+' || text[pos] == '-') {
        ++pos;
    }
    std::size_t digit_count = 0;
    while (pos < text.size() && text[pos] >= '0' && text[pos] <= '9') {
        ++pos;
        ++digit_count;
    }
    if (pos < text.size() && text[pos] == '.') {
        ++pos;
        while (pos < text.size() && text[pos] >= '0' && text[pos] <= '9') {
            ++pos;
            ++digit_count;
        }
    }
    if (digit_count == 0) {
        return false;
    }
    if (pos < text.size() && (text[pos] == 'e' || text[pos] == 'E')) {
        ++pos;
        if (pos < text.size() && (text[pos] == '+' || text[pos] == '-')) {
            ++pos;
        }
        std::size_t exponent_digits = 0;
        while (pos < text.size() && text[pos] >= '0' && text[pos] <= '9') {
            ++pos;
            ++exponent_digits;
        }
        if (exponent_digits == 0) {
            return false;
        }
    }
    return pos == text.size();
}

/// @brief 严格十进制浮点解析；拒绝整串未消费、上溢/下溢与非有限值。
inline bool ParseDouble(std::string_view text, double& out) noexcept {
    if (!IsValidDoubleSyntax(text)) {
        return false;
    }
    const char* begin = text.data();
    const char* end = begin + text.size();
    // from_chars 的浮点实现不保证接受前导 '+'；语法检查已保证后面还有字符。
    if (*begin == '+') {
        ++begin;
    }
    double value = 0.0;
    const std::from_chars_result result =
        std::from_chars(begin, end, value, std::chars_format::general);
    if (result.ec != std::errc() || result.ptr != end) {
        return false;
    }
    if (!std::isfinite(value)) {
        return false;
    }
    out = value;
    return true;
}

/// @brief ASCII 小写化；不依赖 locale。
inline char ToLowerAscii(char value) noexcept {
    return value >= 'A' && value <= 'Z' ? static_cast<char>(value - 'A' + 'a') : value;
}

/// @brief 忽略 ASCII 大小写的整串比较。
inline bool EqualsIgnoreCase(std::string_view text, std::string_view lower) noexcept {
    if (text.size() != lower.size()) {
        return false;
    }
    for (std::size_t index = 0; index < text.size(); ++index) {
        if (ToLowerAscii(text[index]) != lower[index]) {
            return false;
        }
    }
    return true;
}

/// @brief 严格布尔解析：true/false/yes/no/1/0，大小写不敏感。
inline bool ParseBool(std::string_view text, bool& out) noexcept {
    if (EqualsIgnoreCase(text, "true") || EqualsIgnoreCase(text, "yes") || text == "1") {
        out = true;
        return true;
    }
    if (EqualsIgnoreCase(text, "false") || EqualsIgnoreCase(text, "no") || text == "0") {
        out = false;
        return true;
    }
    return false;
}

} // namespace detail

/// @brief `std::string_view -> int` 的严格转换器。
struct StringToInt {
    bool operator()(std::string_view from, int& to) const noexcept {
        return detail::ParseInt(from, to);
    }
};

/// @brief `std::string_view -> double` 的严格转换器。
struct StringToDouble {
    bool operator()(std::string_view from, double& to) const noexcept {
        return detail::ParseDouble(from, to);
    }
};

/// @brief `std::string_view -> bool` 的严格转换器。
struct StringToBool {
    bool operator()(std::string_view from, bool& to) const noexcept {
        return detail::ParseBool(from, to);
    }
};

/// @brief `std::string_view -> std::string`：原样复制（空串合法）。
struct StringToString {
    bool operator()(std::string_view from, std::string& to) const {
        to.assign(from);
        return true;
    }
};

} // namespace go2cpp::bento::reflect::caster
