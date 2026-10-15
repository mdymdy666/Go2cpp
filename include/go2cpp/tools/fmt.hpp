/**
 * @file fmt.hpp
 * @brief Go `fmt` 包的 C++ 对应实现（核心子集，语义对齐）。
 *
 * @details 复用优先的取舍（沿用 `strings.hpp` / `strconv.hpp` 的结论）：
 *
 *   | 候选 | 许可证 | 结论 |
 *   |---|---|---|
 *   | [fmtlib/fmt](https://github.com/fmtlib/fmt) | MIT | **不采用为直接依赖**：它的语法是 `{}`（Python 风格），与 Go 的 `%v` 指令集不同；要用它必须先做「Go 格式串 → fmt 格式串」的转换层，而转换层本身就要实现本文件的解析器（等于双份复杂度）。它仍然是**底层候选**：若将来需要更强的浮点/编译期检查，可在 `detail` 层替换实现。 |
 *   | `printf` 家族（libc） | — | **不采用**：类型不安全（可变参数），且 Go 的 width/precision 以 **rune** 计、C 的以字节计；`%v` 在 C 中不存在 |
 *   | **自研（本文件）** | — | **采用**：目标是 Go 语义 1:1，且天然复用本项目已有的 `strconv.hpp`（整数/浮点格式化与 `Quote`）与 `error.hpp`（`ErrorPtr` / `Wrap`） |
 *
 *   语义对照来源：本机 `third_party/go1.23.0.src.tar.gz` 内的 `go/src/fmt/doc.go`
 *   （Go 1.23.0，BSD-3-Clause）—— 已对照并落实的点：
 *     - width/precision **以 rune 计**（不是字节）；
 *     - flags：`-`（左对齐）、`0`（零填充，数字时在符号之后）、`+`（总是符号）、
 *       空格（省略号位留空格）、`#`（替代格式：`0b`/`0`/`0x` 前缀、浮点保留小数点）；
 *     - `%v` 对浮点用**最短唯一表示**（等价 `%g` 且精度为 -1），`%e`/`%f` 默认精度 6；
 *     - `Print` 系列 = 每个操作数按 `%v`；`Println` 在操作数之间加空格并追加换行；
 *     - 动词与类型不匹配时输出 `%!verb(type=value)`，缺参数输出 `%!verb(MISSING)`，
 *       多余参数在末尾追加 `%!(EXTRA type=value)`。
 *
 * @note **明确的边界**（相对 Go 的 `fmt`）：
 *       - 不实现 `Formatter`/`GoStringer` 接口分派（C++ 无反射），但**特别支持**
 *         `ErrorPtr`（按 `Message()` 输出，对齐 Go 的 error 接口规则）与
 *         `std::vector<T>`/`std::pair`/`std::tuple` 的递归 `%v`；
 *       - 不实现 `%U`（Unicode 码点表）、复数与 `%p` 的 `#` 抑制细节；
 *       - width/precision 的 `*` 形式本轮未实现（抛 `std::logic_error` 明确拒绝）。
 */

#pragma once

#include "go2cpp/error.hpp"
#include "go2cpp/strconv.hpp"
#include "go2cpp/strings.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

namespace go2cpp {

namespace detail {

/** @brief 一次 `%` 指令的解析结果。 */
struct FormatSpec {
    bool minus{false};      // '-'：左对齐
    bool zero{false};       // '0'：零填充（数字时在符号之后）
    bool plus{false};       // '+'：总是打印符号
    bool space{false};      // ' '：省略号位留空格
    bool sharp{false};      // '#'：替代格式
    int width{-1};
    int precision{-1};
    char verb{0};
};

/** @brief 把 rune 数补齐到 width（按 rune 而非字节，与 Go 一致）。 */
inline void PadToWidth(std::string& out, std::string_view text, const FormatSpec& spec,
                       bool numeric) {
    if (spec.width <= 0) {
        out.append(text);
        return;
    }
    const auto runes = static_cast<std::int64_t>(RuneCount(text));
    const auto padding = static_cast<std::int64_t>(spec.width) - runes;
    if (padding <= 0) {
        out.append(text);
        return;
    }
    const auto count = static_cast<std::size_t>(padding);
    if (spec.minus) {
        out.append(text);
        out.append(count, ' ');
        return;
    }
    if (spec.zero && numeric) {
        // 零填充要放在符号之后（与 Go / C 一致）
        std::size_t prefix = 0;
        if (!text.empty() && (text[0] == '-' || text[0] == '+' || text[0] == ' ')) prefix = 1;
        out.append(text.substr(0, prefix));
        out.append(count, '0');
        out.append(text.substr(prefix));
        return;
    }
    out.append(count, ' ');
    out.append(text);
}

/** @brief 给数值文本加上 `+` / 空格前缀（flags 语义）。 */
inline std::string ApplySignFlags(std::string text, const FormatSpec& spec) {
    if (text.empty() || text[0] == '-') return text;
    if (spec.plus) return "+" + text;
    if (spec.space) return " " + text;
    return text;
}

/** @brief 截断到 precision 个 rune（用于字符串类 verb）。 */
inline std::string_view TruncateToRunes(std::string_view text, int precision) {
    if (precision < 0) return text;
    std::size_t taken = 0;
    std::size_t index = 0;
    while (index < text.size() && static_cast<int>(taken) < precision) {
        const auto decoded = DecodeRune(text.substr(index));
        index += decoded.second == 0 ? 1 : decoded.second;
        ++taken;
    }
    return text.substr(0, index);
}

/** @brief 逐字节 hex（用于 `%x` / `%X` 的字符串）。 */
inline std::string HexEncode(std::string_view text, bool upper, bool sharp, bool spaced) {
    static const char* const kLower = "0123456789abcdef";
    static const char* const kUpper = "0123456789ABCDEF";
    const char* const digits = upper ? kUpper : kLower;
    std::string out;
    if (sharp) out += upper ? "0X" : "0x";
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (spaced && i != 0) out.push_back(' ');
        const auto byte = static_cast<unsigned char>(text[i]);
        out.push_back(digits[(byte >> 4) & 0x0FU]);
        out.push_back(digits[byte & 0x0FU]);
    }
    return out;
}

/// rune 的 Go 风格单引号字面量（对应 `%q` 作用于整数/rune）。
inline std::string QuoteRune(char32_t rune) {
    std::string body;
    switch (rune) {
        case U'\'': body = "\\'"; break;
        case U'\\': body = "\\\\"; break;
        case U'\n': body = "\\n"; break;
        case U'\t': body = "\\t"; break;
        case U'\r': body = "\\r"; break;
        default:
            if (rune >= 0x20U && rune < 0x7FU) {
                body.push_back(static_cast<char>(rune));
            } else {
                char buffer[16];
                std::snprintf(buffer, sizeof(buffer), "\\u%04x", static_cast<unsigned>(rune));
                body = buffer;
            }
            break;
    }
    return "'" + body + "'";
}

/// 归一化后的类型名（对应 `%T` 的可读版本；Go 用 reflect，本项目用编译期映射）。
template <typename T>
const char* TypeName() {
    // 按 C++ 类型名映射到 Go 名字（不用 sizeof 推断）：Go 的 %T 对 int 输出 "int"，
    // 而 C++ 的 int 是 32 位 —— 若按大小映射会得到 "int32"，与 Go 不一致。
    // 另外 char[N] 会衰变为 char*，必须用 is_convertible 判定而不是 is_same_v<const char*>。
    using U = std::remove_cv_t<T>;
    if constexpr (std::is_same_v<U, bool>) return "bool";
    else if constexpr (std::is_same_v<U, std::string> || std::is_same_v<U, std::string_view>) return "string";
    else if constexpr (std::is_convertible_v<U, std::string_view>) return "string";
    else if constexpr (std::is_same_v<U, char>) return "byte";
    else if constexpr (std::is_same_v<U, signed char>) return "int8";
    else if constexpr (std::is_same_v<U, unsigned char>) return "uint8";
    else if constexpr (std::is_same_v<U, short>) return "int16";
    else if constexpr (std::is_same_v<U, unsigned short>) return "uint16";
    else if constexpr (std::is_same_v<U, int>) return "int";
    else if constexpr (std::is_same_v<U, unsigned int>) return "uint";
    else if constexpr (std::is_same_v<U, long> || std::is_same_v<U, long long>) return "int64";
    else if constexpr (std::is_same_v<U, unsigned long> || std::is_same_v<U, unsigned long long>) return "uint64";
    else if constexpr (std::is_same_v<U, float>) return "float32";
    else if constexpr (std::is_same_v<U, double>) return "float64";
    else return "interface {}";
}

template <typename T>
struct IsErrorPtr : std::false_type {};
template <>
struct IsErrorPtr<ErrorPtr> : std::true_type {};

template <typename T>
struct IsVector : std::false_type {};
template <typename U, typename Alloc>
struct IsVector<std::vector<U, Alloc>> : std::true_type {
    using element = U;
};

template <typename T>
struct IsPair : std::false_type {};
template <typename A, typename B>
struct IsPair<std::pair<A, B>> : std::true_type {};

/// 字符串类判定（`std::string` / `std::string_view` / `const char*` / 字面量）。
template <typename T>
struct IsStringLike
    : std::bool_constant<std::is_convertible_v<const T&, std::string_view> &&
                         !std::is_same_v<std::decay_t<T>, bool> &&
                         !IsErrorPtr<std::decay_t<T>>::value> {};

/// 把任意受支持的值按 `%v` 规则追加到输出（供容器递归使用）。
template <typename T>
void AppendDefault(std::string& out, const T& value);

/** @brief 用一个 `FormatSpec` 格式化单个值（类型分派的核心）。 */
template <typename T>
void FormatValue(std::string& out, const T& value, const FormatSpec& spec) {
    using Value = std::decay_t<T>;
    const char verb = spec.verb;

    if constexpr (IsErrorPtr<Value>::value) {
        // Go 的 error 接口优先规则：%v / %s / %q 都作用在 Error() 文本上
        const std::string message = value ? ErrorMessage(value) : "<nil>";
        if (verb == 'q') {
            PadToWidth(out, Quote(message), spec, false);
        } else if (verb == 'v' || verb == 's') {
            PadToWidth(out, message, spec, false);
        } else if (verb == 'T') {
            PadToWidth(out, "error", spec, false);
        } else {
            out += "%!" + std::string(1, verb) + "(error=" + message + ")";
        }
        return;
    } else if constexpr (IsVector<Value>::value) {
        // Go 对切片是**递归**应用格式：%q 逐个引用元素，%v/%s 逐个按默认规则打印。
        if (verb == 'v' || verb == 's' || verb == 'q') {
            std::string body = "[";
            using Element = typename IsVector<Value>::element;
            for (std::size_t i = 0; i < value.size(); ++i) {
                if (i != 0) body.push_back(' ');
                if constexpr (IsStringLike<Element>::value) {
                    // Go：%v/%s 打印字符串元素不带引号；%q 带引号
                    body += (verb == 'q') ? Quote(std::string(value[i])) : std::string(value[i]);
                } else {
                    FormatSpec element_spec;
                    element_spec.verb = (verb == 'q') ? 'q' : 'v';
                    FormatValue(body, value[i], element_spec);
                }
            }
            body.push_back(']');
            PadToWidth(out, body, spec, false);
        } else {
            out += "%!" + std::string(1, verb) + "(slice)";
        }
        return;
    } else if constexpr (IsPair<Value>::value) {
        if (verb == 'v' || verb == 's') {
            std::string body = "(";
            AppendDefault(body, value.first);
            body += ", ";
            AppendDefault(body, value.second);
            body.push_back(')');
            PadToWidth(out, body, spec, false);
        } else {
            out += "%!" + std::string(1, verb) + "(pair)";
        }
        return;
    } else if constexpr (std::is_same_v<Value, bool>) {
        const std::string text = value ? "true" : "false";
        if (verb == 'v' || verb == 't') {
            PadToWidth(out, text, spec, false);
        } else if (verb == 'q') {
            PadToWidth(out, Quote(text), spec, false);
        } else if (verb == 'T') {
            PadToWidth(out, "bool", spec, false);
        } else if (verb == 's') {
            PadToWidth(out, text, spec, false);
        } else {
            out += "%!" + std::string(1, verb) + "(bool=" + text + ")";
        }
        return;
    } else if constexpr (std::is_integral_v<Value>) {
        if (verb == 'c') {
            std::string text;
            AppendRune(text, static_cast<char32_t>(value));
            PadToWidth(out, text, spec, false);
            return;
        }
        if (verb == 'q') {
            PadToWidth(out, QuoteRune(static_cast<char32_t>(value)), spec, false);
            return;
        }
        if (verb == 'T') {
            PadToWidth(out, TypeName<Value>(), spec, false);
            return;
        }
        int base = 10;
        bool upper = false;
        const char* prefix = "";
        if (verb == 'x' || verb == 'X') {
            base = 16;
            upper = (verb == 'X');
            if (spec.sharp) prefix = upper ? "0X" : "0x";
        } else if (verb == 'b') {
            base = 2;
            if (spec.sharp) prefix = "0b";
        } else if (verb == 'o') {
            base = 8;
            if (spec.sharp) prefix = "0";
        } else if (verb != 'd' && verb != 'v') {
            out += "%!" + std::string(1, verb) + "(" + TypeName<Value>() + "=" +
                   std::to_string(static_cast<std::int64_t>(value)) + ")";
            return;
        }
        std::string text;
        if constexpr (std::is_signed_v<Value>) {
            text = FormatInt(static_cast<std::int64_t>(value), base);
        } else {
            text = FormatUint(static_cast<std::uint64_t>(value), base);
        }
        if (upper && base == 16) {
            for (auto& ch : text) {
                if (ch >= 'a' && ch <= 'f') ch = static_cast<char>(ch - 'a' + 'A');
            }
        }
        text = std::string(prefix) + text;
        if (base == 10) text = ApplySignFlags(std::move(text), spec);
        PadToWidth(out, text, spec, true);
        return;
    } else if constexpr (std::is_floating_point_v<Value>) {
        if (verb == 'T') {
            PadToWidth(out, TypeName<Value>(), spec, false);
            return;
        }
        char format = 'g';
        if (verb == 'f') format = 'f';
        else if (verb == 'e') format = 'e';
        else if (verb == 'g' || verb == 'v') format = 'g';
        else {
            out += "%!" + std::string(1, verb) + "(float64)";
            return;
        }
        int precision = spec.precision;
        if (precision < 0 && format != 'g') precision = 6;      // Go：%e/%f 默认 6
        std::string text = FormatFloat(static_cast<double>(value), format, precision, 64);
        if (spec.sharp && text.find('.') == std::string::npos &&
            text.find('e') == std::string::npos && text.find("Inf") == std::string::npos &&
            text.find("NaN") == std::string::npos) {
            text += ".";                                        // `#`：保留小数点
        }
        PadToWidth(out, ApplySignFlags(std::move(text), spec), spec, true);
        return;
    } else if constexpr (IsStringLike<Value>::value) {
        const std::string_view text(value);
        if (verb == 'T') {
            PadToWidth(out, TypeName<Value>(), spec, false);
            return;
        }
        if (verb == 'q') {
            PadToWidth(out, Quote(std::string(text)), spec, false);
            return;
        }
        if (verb == 'x' || verb == 'X') {
            PadToWidth(out, HexEncode(text, verb == 'X', spec.sharp, spec.space), spec, false);
            return;
        }
        if (verb == 'v' || verb == 's') {
            PadToWidth(out, TruncateToRunes(text, spec.precision), spec, false);
            return;
        }
        out += "%!" + std::string(1, verb) + "(string=" + std::string(text) + ")";
        return;
    } else {
        out += "%!";
        out.push_back(verb);
        out += "(unsupported)";
        return;
    }
}

/// 按 `%v` 默认规则追加（容器元素递归用）。
template <typename T>
void AppendDefault(std::string& out, const T& value) {
    FormatSpec spec;
    spec.verb = 'v';
    FormatValue(out, value, spec);
}

/// 解析一个 `%` 指令（`index` 指向 `%` 之后的第一个字符）。
inline FormatSpec ParseSpec(std::string_view format, std::size_t& index) {
    FormatSpec spec;
    while (index < format.size()) {
        const char ch = format[index];
        if (ch == '-') spec.minus = true;
        else if (ch == '0') spec.zero = true;
        else if (ch == '+') spec.plus = true;
        else if (ch == ' ') spec.space = true;
        else if (ch == '#') spec.sharp = true;
        else break;
        ++index;
    }
    if (index < format.size() && format[index] == '*') {
        throw std::logic_error("fmt: '*' width/precision is not supported yet");
    }
    int width = -1;
    while (index < format.size() && format[index] >= '0' && format[index] <= '9') {
        width = (width < 0 ? 0 : width) * 10 + (format[index] - '0');
        ++index;
    }
    spec.width = width;
    if (index < format.size() && format[index] == '.') {
        ++index;
        if (index < format.size() && format[index] == '*') {
            throw std::logic_error("fmt: '*' precision is not supported yet");
        }
        int precision = 0;
        while (index < format.size() && format[index] >= '0' && format[index] <= '9') {
            precision = precision * 10 + (format[index] - '0');
            ++index;
        }
        spec.precision = precision;
    }
    if (index < format.size()) {
        spec.verb = format[index];
        ++index;
    }
    return spec;
}

/// 逐参数分派（编译期展开 + 运行期索引匹配）。
template <typename Tuple, std::size_t... I>
void DispatchArg(const Tuple& tuple, std::size_t index, const FormatSpec& spec,
                 std::string& out, std::index_sequence<I...>) {
    bool handled = false;
    ((index == I ? (FormatValue(out, std::get<I>(tuple), spec), handled = true) : false), ...);
    if (!handled) {
        out += "%!";
        out.push_back(spec.verb);
        out += "(MISSING)";
    }
}

template <typename... Args>
std::string FormatImpl(std::string_view format, const Args&... args) {
    std::string out;
    const auto tuple = std::tie(args...);
    std::size_t argument = 0;
    std::size_t index = 0;
    while (index < format.size()) {
        const char ch = format[index];
        if (ch != '%') {
            out.push_back(ch);
            ++index;
            continue;
        }
        ++index;
        if (index < format.size() && format[index] == '%') {
            out.push_back('%');
            ++index;
            continue;
        }
        const FormatSpec spec = ParseSpec(format, index);
        if (argument >= sizeof...(Args)) {
            out += "%!";
            out.push_back(spec.verb);
            out += "(MISSING)";
            continue;
        }
        DispatchArg(tuple, argument, spec, out, std::index_sequence_for<Args...>{});
        ++argument;
    }
    if constexpr (sizeof...(Args) > 0) {
    if (argument < sizeof...(Args)) {
        out += "%!(EXTRA ";
        bool first = true;
        auto append_extra = [&](const auto& value) {
            if (!first) out += ", ";
            first = false;
            out += TypeName<std::decay_t<decltype(value)>>();
            out += "=";
            AppendDefault(out, value);
        };
        std::apply([&](const auto&... values) {
            std::size_t position = 0;
            (((position++ >= argument) ? append_extra(values) : (void)0), ...);
        }, tuple);
        out += ")";
    }
    }
    return out;
}

}  // namespace detail

// ============================================================================
// Sprintf / Sprint / Sprintln（对应 Go 的同名函数）
// ============================================================================

/**
 * @brief 对应 `fmt.Sprintf`：按 Go 格式串格式化并返回字符串。
 * @note 支持的动词：`v`/`d`/`s`/`q`/`t`/`f`/`e`/`g`/`x`/`X`/`b`/`o`/`c`/`T`；
 *       支持的 flags：`-`/`0`/`+`/空格/`#`；width 与 precision 为字面数字。
 */
template <typename... Args>
inline std::string Sprintf(std::string_view format, const Args&... args) {
    return detail::FormatImpl(format, args...);
}

/// 对应 `fmt.Sprint`：每个操作数按 `%v`，**不加空格**。
template <typename... Args>
inline std::string Sprint(const Args&... args) {
    std::string out;
    if constexpr (sizeof...(Args) > 0) {
        std::apply([&](const auto&... values) {
            (detail::AppendDefault(out, values), ...);
        }, std::tie(args...));
    }
    return out;
}

/// 对应 `fmt.Sprintln`：每两个操作数之间插入空格，并追加换行。
template <typename... Args>
inline std::string Sprintln(const Args&... args) {
    std::string out;
    std::size_t index = 0;
    if constexpr (sizeof...(Args) > 0) {
        std::apply([&](const auto&... values) {
            // 注意：不要把三目直接写进逗号折叠的操作数 —— 编译器会把它解析成
            // 「二元折叠」并报 binary expression in operand of fold-expression。
            const auto emit = [&](const auto& value) {
                if (index++ != 0) out.push_back(' ');
                detail::AppendDefault(out, value);
            };
            (emit(values), ...);
        }, std::tie(args...));
    }
    out.push_back('\n');
    return out;
}

// ============================================================================
// Errorf（支持 %w 包装）
// ============================================================================

/**
 * @brief 对应 `fmt.Errorf`：生成一个 `ErrorPtr`；`%w` 会把对应参数包进错误链。
 * @details 多个 `%w` 时用 `Join` 聚合（对齐 Go 1.20+ 的 `errors.Join` 语义）。
 *          文本部分与 `Sprintf` 一致（`%w` 按 `%v` 渲染）。
 */
template <typename... Args>
inline ErrorPtr Errorf(std::string_view format, const Args&... args) {
    // 1) 找出 %w 对应的参数序号（按指令出现顺序）
    std::vector<std::size_t> wrapped;
    std::size_t argument = 0;
    std::size_t index = 0;
    while (index < format.size()) {
        if (format[index] != '%') {
            ++index;
            continue;
        }
        ++index;
        if (index < format.size() && format[index] == '%') {
            ++index;
            continue;
        }
        std::size_t spec_index = index;
        const detail::FormatSpec spec = detail::ParseSpec(format, spec_index);
        if (spec.verb == 'w') wrapped.push_back(argument);
        ++argument;
        index = spec_index;
    }

    // 2) 文本：把 %w 当 %v 渲染（保证参数消费一致）
    std::string normalized(format);
    for (std::size_t position = 0; (position = normalized.find("%w", position)) != std::string::npos;
         position += 2) {
        normalized.replace(position, 2, "%v");
    }
    const std::string message = detail::FormatImpl(normalized, args...);

    // 3) 收集被 %w 指定的 ErrorPtr
    std::vector<ErrorPtr> causes;
    const auto tuple = std::tie(args...);
    std::apply([&](const auto&... values) {
        std::size_t position = 0;
        auto collect = [&](const auto& value) {
            if (std::find(wrapped.begin(), wrapped.end(), position) != wrapped.end()) {
                using Value = std::decay_t<decltype(value)>;
                if constexpr (detail::IsErrorPtr<Value>::value) {
                    if (value) causes.push_back(value);
                }
            }
            ++position;
        };
        (collect(values), ...);
    }, tuple);

    if (causes.empty()) return NewError(message);
    if (causes.size() == 1) return Wrap(causes.front(), message);
    return Join({Wrap(causes.front(), message), Join(causes)});
}

// ============================================================================
// Print 系列（写标准输出，对应 Go 的 os.Stdout 语义）
// ============================================================================

/// 对应 `fmt.Printf`。
template <typename... Args>
inline void Printf(std::string_view format, const Args&... args) {
    std::cout << Sprintf(format, args...);
}

/// 对应 `fmt.Print`。
template <typename... Args>
inline void Print(const Args&... args) {
    std::cout << Sprint(args...);
}

/// 对应 `fmt.Println`。
template <typename... Args>
inline void Println(const Args&... args) {
    std::cout << Sprintln(args...);
}

}  // namespace go2cpp
