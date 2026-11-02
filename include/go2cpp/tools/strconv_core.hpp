/**
 * @file strconv_core.hpp
 * @brief `strconv` 的零依赖解析核心：只返回 bool，不构造 `ErrorPtr`。
 *
 * @details 为什么和 `tools/strconv.hpp` 分成两层：
 *   `strconv.hpp` 完整对齐 Go 的 `(value, error)` 双返回值，错误锚点与错误文本
 *   需要 `go2cpp/error.hpp`（进而链接 `go2cpp_error`）。但项目里有一批模块
 *   （Bento 的 capability / app 的 fetch 等）刻意只依赖标准库与 POSIX，
 *   给它们引入错误体系会破坏「独立目标、最小依赖」的边界。这些模块真正需要的
 *   只是「文本 -> 数值，失败给 false」。因此把纯解析逻辑下沉到本文件：
 *   无 `ErrorPtr`、无动态分派、header-only、只依赖标准库与 `tools/strings.hpp`。
 *
 * ## 语义分层（**不要混用**）
 *
 * | 函数 | 接受前导空白 | 接受符号 | 允许尾随字符 | 溢出 |
 * |---|---|---|---|---|
 * | `ParseU64Decimal` | 否 | 否 | 否 | 返回 false |
 * | `ParseU64DecimalTrimmed` | 两端 `" \t\r\n"` | 否 | 否 | 返回 false |
 * | `ParseU64Prefix` | 是（6 个 ASCII 空白） | 否 | **是**（只取数字前缀） | 返回 false |
 * | `ParseU64Within` | 否 | 否 | 否 | 返回 false |
 * | `ParseU64Argument` / `ParseI64Argument` | 是 | 是（`+`/`-`） | 否 | 返回 false |
 *
 * 这些函数是**既有各模块实现的等价下沉**（行为逐字节一致），不是「更严格的
 * 新版」：例如 `ParseU64Argument` 保留 `strtoull` 对 `"-1"` 的取模语义，
 * 因为替换前后必须完全一致；要收紧语义应作为独立变更单独评审。
 *
 * ## 模块定位
 * `tools/strconv` 的底座：各模块的私有 `ParseU64`/`ParseBool` 改为直接调用本文件。
 * `tools/strconv.hpp` 的公开 API 保持不变（Go 语义 + ErrorPtr），两者不互相替代。
 *
 * ## 边界与陷阱
 * - 一律不做 locale 感知：空白集合固定为 ASCII 6 个字符，大小写折叠只对 `A-Z`；
 * - `Parse*Argument` 系列内部复用 `strtoull`/`strtoll`/`strtod`（与旧实现同一个
 *   标准函数），因此 `" 12"`、`"+12"`、`"-1"`、`"inf"` 的接受集与旧代码完全一致；
 * - `out` 为空指针时一律返回 false（不写、不崩）。
 */

#pragma once

#include "go2cpp/tools/strings.hpp"

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <string>
#include <string_view>
#include <system_error>

namespace go2cpp::strconv_core {

/// ASCII 空白集合：与 C locale 的 `std::isspace` 完全一致（6 个字符）。
inline bool IsAsciiSpace(char c) noexcept {
    return c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '\f' || c == '\r';
}

/**
 * @brief 严格十进制无符号解析：非空、全部 `0-9`、不回收尾、溢出返回 false。
 * @note 对应 `bento/app/fetch.cpp` 旧 `ParseU64Decimal` 与 `media/signaling.cpp`
 *       旧 `ParseUint` 的「全数字」分支；**不接受**前导空白与 `+`/`-`。
 */
inline bool ParseU64Decimal(std::string_view text, std::uint64_t* out) noexcept {
    if (out == nullptr || text.empty()) {
        return false;
    }
    std::uint64_t value = 0;
    for (const char ch : text) {
        if (ch < '0' || ch > '9') {
            return false;
        }
        const std::uint64_t digit = static_cast<std::uint64_t>(ch - '0');
        if (value > (std::numeric_limits<std::uint64_t>::max() - digit) / 10U) {
            return false;
        }
        value = value * 10U + digit;
    }
    *out = value;
    return true;
}

/**
 * @brief 先修剪两端 `" \t\r\n"`，再做严格十进制解析。
 * @details 对应 `config/config.cpp`、`config/so_manifest.cpp` 的
 *          `Trim() + std::from_chars`：空串（含全空白）返回 false；`+` 与
 *          `-` 都不接受（无符号 `from_chars` 的语法）。
 */
inline bool ParseU64DecimalTrimmed(std::string_view text, std::uint64_t* out) noexcept {
    if (out == nullptr) {
        return false;
    }
    constexpr std::string_view kTrimSet = " \t\r\n";
    std::size_t begin = 0;
    std::size_t end = text.size();
    while (begin < end && kTrimSet.find(text[begin]) != std::string_view::npos) ++begin;
    while (end > begin && kTrimSet.find(text[end - 1]) != std::string_view::npos) --end;
    const std::string_view trimmed = text.substr(begin, end - begin);
    if (trimmed.empty()) {
        return false;
    }
    // 与旧实现同一个标准函数，保证对前导 '+'/'-'、前导零与溢出的判定完全一致。
    std::uint64_t parsed = 0;
    const std::from_chars_result result =
        std::from_chars(trimmed.data(), trimmed.data() + trimmed.size(), parsed);
    if (result.ec != std::errc{} || result.ptr != trimmed.data() + trimmed.size()) {
        return false;
    }
    *out = parsed;
    return true;
}

/**
 * @brief 跳过前导 ASCII 空白，解析**最长十进制前缀**；至少一位数字才算成功。
 * @details 对应 `bento/app/capability.cpp` 旧 `ParseU64`：`/proc/meminfo`
 *          的 `"12345 kB"` 与 cgroup 的纯数值都走这里，尾随内容被忽略。
 *          溢出（超出 `uint64_t`）返回 false，`out` 不被写入。
 */
inline bool ParseU64Prefix(std::string_view text, std::uint64_t* out) noexcept {
    if (out == nullptr) {
        return false;
    }
    std::size_t index = 0;
    while (index < text.size() && IsAsciiSpace(text[index])) {
        ++index;
    }
    if (index == text.size() || text[index] < '0' || text[index] > '9') {
        return false;
    }
    std::uint64_t value = 0;
    for (; index < text.size(); ++index) {
        const char ch = text[index];
        if (ch < '0' || ch > '9') {
            break;
        }
        const std::uint64_t digit = static_cast<std::uint64_t>(ch - '0');
        if (value > (std::numeric_limits<std::uint64_t>::max() - digit) / 10U) {
            return false;
        }
        value = value * 10U + digit;
    }
    *out = value;
    return true;
}

/**
 * @brief 全数字十进制 + 上限检查。
 * @details 对应 `media/signaling.cpp` 旧 `ParseUint(text, limit, value)`；
 *          注意旧实现还有 `text.size() > 20` 的前置拒绝，那条前置条件留在调用点
 *          （本函数只负责数值语义，避免把「长度上限」与「数值上限」混为一谈）。
 */
inline bool ParseU64Within(std::string_view text, std::uint64_t limit,
                           std::uint64_t* out) noexcept {
    if (out == nullptr || text.empty()) {
        return false;
    }
    std::uint64_t value = 0;
    for (const char ch : text) {
        if (ch < '0' || ch > '9') {
            return false;
        }
        value = value * 10U + static_cast<std::uint64_t>(ch - '0');
        if (value > limit) {
            return false;
        }
    }
    *out = value;
    return true;
}

/**
 * @brief `strtoull(text, ..., 10)` 的完整消费版本（对应 `app/resource.cpp` 旧实现）。
 * @details 跳过前导 ASCII 空白、接受可选 `+`/`-`、必须整串消费、`ERANGE` 视为失败。
 *          **保留** `strtoull` 对 `"-1"` 的取模语义（→ `UINT64_MAX`），因为这是
 *          替换前的既有行为；收紧它属于语义变更，不在本次「等价替换」范围内。
 */
inline bool ParseU64Argument(std::string_view text, std::uint64_t* out) noexcept {
    if (out == nullptr || text.empty()) {
        return false;
    }
    const std::string buffer(text);
    errno = 0;
    char* end = nullptr;
    const unsigned long long value = std::strtoull(buffer.c_str(), &end, 10);
    if (end == buffer.c_str() || *end != '\0' || errno == ERANGE) {
        return false;
    }
    *out = static_cast<std::uint64_t>(value);
    return true;
}

/**
 * @brief `strtoll(text, ..., 10)` 的完整消费版本（对应 `go/flag.hpp` 旧 `ParseIntText`）。
 * @details 接受前导空白与 `+`/`-`；`ERANGE`（含 `LLONG_MIN` 溢出）视为失败。
 */
inline bool ParseI64Argument(std::string_view text, std::int64_t* out) noexcept {
    if (out == nullptr || text.empty()) {
        return false;
    }
    const std::string buffer(text);
    errno = 0;
    char* end = nullptr;
    const long long value = std::strtoll(buffer.c_str(), &end, 10);
    if (end == buffer.c_str() || *end != '\0' || errno == ERANGE) {
        return false;
    }
    *out = static_cast<std::int64_t>(value);
    return true;
}

/**
 * @brief `strtod` 的完整消费版本（对应 `go/flag.hpp` 旧 `ParseFloatText`）。
 * @details 与 `strconv::ParseFloat` 的 Go 语义**不同**：这里接受 `"inf"`/`"nan"`/
 *          十六进制浮点/前导空白（C 库语法），并且 `ERANGE`（上溢与下溢）都视为失败。
 *          保留该差异是为了让 `go/flag` 的解析行为与替换前逐字节一致。
 */
inline bool ParseDoubleArgument(std::string_view text, double* out) noexcept {
    if (out == nullptr || text.empty()) {
        return false;
    }
    const std::string buffer(text);
    errno = 0;
    char* end = nullptr;
    const double value = std::strtod(buffer.c_str(), &end);
    if (end == buffer.c_str() || *end != '\0' || errno == ERANGE) {
        return false;
    }
    *out = value;
    return true;
}

/**
 * @brief Go `strconv.ParseBool` 接受的 12 个字面量。
 * @details 精确集合：`1 t T true TRUE True` / `0 f F false FALSE False`；
 *          不做 trim、不忽略大小写（`"True"` 与 `"TRUE"` 是白名单里的独立项，
 *          `"tRuE"` 不合法）。
 */
inline bool ParseBoolGoWords(std::string_view text, bool* out) noexcept {
    if (out == nullptr) {
        return false;
    }
    if (text == "1" || text == "t" || text == "T" || text == "TRUE" || text == "true" ||
        text == "True") {
        *out = true;
        return true;
    }
    if (text == "0" || text == "f" || text == "F" || text == "FALSE" || text == "false" ||
        text == "False") {
        *out = false;
        return true;
    }
    return false;
}

/**
 * @brief 宽松布尔词表：大小写不敏感的 `{1,true,yes,on}` / `{0,false,no,off}`。
 * @details 对应 `config/config.cpp` 与 `config/so_manifest.cpp` 的旧 `ParseBool`：
 *          它们都先 `Trim()` 再转小写，因此**本函数不做 trim**，调用方保留原有的
 *          修剪步骤即可保证逐字节一致（`" yes "` 必须先在调用点被 Trim 成 `"yes"`）。
 */
inline bool ParseBoolLooseWords(std::string_view text, bool* out) noexcept {
    if (out == nullptr) {
        return false;
    }
    // 长度先筛掉绝大多数候选，再做逐字节折叠比较。
    switch (text.size()) {
        case 1:  // "1" / "0"
            if (text[0] == '1') { *out = true; return true; }
            if (text[0] == '0') { *out = false; return true; }
            return false;
        case 2:  // "on" / "no"
            if (AsciiEqualIgnoreCase(text, "on")) { *out = true; return true; }
            if (AsciiEqualIgnoreCase(text, "no")) { *out = false; return true; }
            return false;
        case 3:  // "yes" / "off"
            if (AsciiEqualIgnoreCase(text, "yes")) { *out = true; return true; }
            if (AsciiEqualIgnoreCase(text, "off")) { *out = false; return true; }
            return false;
        case 4:  // "true"
            if (AsciiEqualIgnoreCase(text, "true")) { *out = true; return true; }
            return false;
        case 5:  // "false"
            if (AsciiEqualIgnoreCase(text, "false")) { *out = false; return true; }
            return false;
        default:
            return false;
    }
}

}  // namespace go2cpp::strconv_core
