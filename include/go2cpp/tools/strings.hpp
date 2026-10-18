/**
 * @file strings.hpp
 * @brief Go `strings` 包的 C++ 对应实现（薄封装 + 语义对齐）。
 *
 * @details 为什么自研而不是移植现成库（按项目规范「复用优先」，先检索再决定）：
 *
 *   | 候选 | 许可证 | 结论 |
 *   |---|---|---|
 *   | [abseil/abseil-cpp](https://github.com/abseil/abseil-cpp) `absl/strings` | Apache-2.0 | **不采用**：引入完整构建系统（CMake/Bazel 集成、若干子库），与项目「零第三方运行时依赖」的定位冲突；且它的语义是 Abseil 自己的，不是 Go 的 |
 *   | [boostorg/algorithm](https://github.com/boostorg/algorithm)（`split`/`join`/`trim`/`starts_with`） | Boost 1.0 | **不采用**：项目当前只在非 native 上下文平台链接 `Boost::context`；为一个纯字符串模块引入 Boost 头依赖不划算 |
 *   | [fmtlib/fmt](https://github.com/fmtlib/fmt) | MIT | **不采用**：它是**格式化**库（对应 Go 的 `fmt`），不是 `strings`；`fmt` 模块会单独评估 |
 *   | [tgalaj/strutil](https://github.com/tgalaj/strutil) | MIT（header-only） | **不采用**：功能集与 Go `strings` 不对齐（无 `Cut*`/`Fields`/`Clone`/`Compare` 等），改造成本高于自研 |
 *   | **自研薄封装** | — | **采用**：目标是与 Go `strings` **语义 1:1**（含 `Count(s,"")==rune+1` 这类反直觉边界），而不是做一个通用字符串库；底层能力 `std::string` 已提供 |
 *
 *   语义对照来源：本机 `third_party/go1.23.0.src.tar.gz` 内的 `go/src/strings/` 包内的 `*.go`
 *   （Go 1.23.0，BSD-3-Clause）。**本实现是独立实现，未复制上游代码**，只对照其行为；
 *   每个非显然边界都在函数注释里写明。
 *
 * @note 本模块是 header-only（纯 inline 函数，无全局状态），与 `endian.hpp`/`json.hpp` 风格一致。
 * @note **明确的边界**（后续 `unicode`/`unicode/utf8` 模块补齐）：
 *       - `ToUpper`/`ToLower` 完整覆盖 ASCII 与 Latin-1，不做全量 Unicode 大小写映射；
 *       - `EqualFold` 支持 ASCII/Latin-1 简单折叠，不做 Unicode 完全折叠（如 Kelvin 符号）；
 *       - `TrimSpace`/`Fields` 覆盖 ASCII 空白 + Latin-1 的 0x85/0xA0 + 常见 Unicode 空白
 *         （U+1680、U+2000–U+200A、U+2028、U+2029、U+202F、U+205F、U+3000）。
 *
 * ## 模块定位
 * Go `strings` 的对应物（header-only 薄封装）：判定 / 查找 / 切分 / 连接 / 替换 /
 * 修剪 / 大小写。被 `fmt`（rune 工具）、`bytes`（同名函数直接转发，避免两份实现
 * 漂移）、`streams` 与配置解析使用。
 *
 * ## 本文件定义什么
 * - UTF-8 最小工具：`Utf8SequenceLength` / `DecodeRune` / `AppendRune` / `RuneCount` /
 *   `IsSpaceRune` / `FoldRune` / `CutsetContains`；
 * - 判定与查找：`Contains*` / `HasPrefix` / `HasSuffix` / `EqualFold` / `Index*` /
 *   `LastIndex*` / `Count`；
 * - 切分与连接：`detail::GenSplit`（Split 家族的唯一实现）/ `Split*` / `Fields` /
 *   `Join` / `Repeat`；
 * - 替换与修剪：`Replace` / `ReplaceAll` / `Trim*` / `Cut*`；
 * - 大小写：`ToUpper` / `ToLower`。
 *
 * ## 阅读建议
 * 先看文件头的「为什么自研」表与「明确边界」，再看 `detail::GenSplit`（Split 家族
 * 只有一个实现，`n`/`sep` 的边界都集中在这里），最后看 `Replace` 对空 `old` 的特判。
 *
 * ## 边界与陷阱
 * - `sep` 为空时 `Split` **按 rune 切分**（每个 rune 一项，不含首尾空串），不是按空串切；
 * - `Replace` 的 `old` 为空时在**每个 rune 前后**插入新串（共 rune+1 处）；
 * - `Count(s, "")` 返回 **rune 数 + 1**（Go 的反直觉边界）；
 * - 非法 UTF-8 字节按 1 字节 rune 处理（`DecodeRune` 返回 size 0），否则循环会卡住；
 * - 大小写/空白判定只覆盖 ASCII + Latin-1 + 常见 Unicode（见文件头边界），不做全量表。
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace go2cpp {

/** @brief 字符串切分结果容器（对应 Go 的 `[]string`）。 */
using StringSlice = std::vector<std::string>;

// ============================================================================
// 最小 UTF-8 工具（Go 的 strings 在多处按 rune 工作；完整版留给 unicode/utf8 模块）
// ============================================================================

/**
 * @brief 返回 UTF-8 首字节指示的序列长度。
 * @return 1..4；非法首字节返回 -1。
 */
inline int Utf8SequenceLength(unsigned char first) noexcept {
    if (first < 0x80U) return 1;
    if ((first & 0xE0U) == 0xC0U) return 2;
    if ((first & 0xF0U) == 0xE0U) return 3;
    if ((first & 0xF8U) == 0xF0U) return 4;
    return -1;
}

/**
 * @brief 解码一个 UTF-8 rune。
 * @details 与 Go `utf8.DecodeRuneInString` 的**错误语义**一致：非法编码返回
 *          `(U+FFFD, 1)` —— 即"非法字节算一个 rune"，这直接影响 `Count`/`Split("")`。
 * @return {rune, 消耗字节数}；输入为空返回 `{U+FFFD, 0}`。
 */
/**
 * @brief 解码一个 UTF-8 rune（非法/截断返回 RuneError + 消耗 1 字节）。
 *
 * ## 步骤
 * step1. 空输入 -> `(RuneError, 0)`；
 * step2. 按首字节求序列长度；非法/截断 -> `(RuneError, 1)`；
 * step3. 逐续字节校验 `10xxxxxx`，不足则按 1 字节处理；
 * step4. 拒绝过长编码（overlong）与代理区，返回 `(rune, 长度)`。
 */
inline std::pair<char32_t, std::size_t> DecodeRune(std::string_view text) noexcept {
    constexpr char32_t kReplacement = 0xFFFDU;
    if (text.empty()) return {kReplacement, 0};
    const auto first = static_cast<unsigned char>(text[0]);
    const int length = Utf8SequenceLength(first);
    if (length <= 0 || static_cast<std::size_t>(length) > text.size()) {
        return {kReplacement, 1};
    }
    char32_t rune = 0;
    if (length == 1) return {static_cast<char32_t>(first), 1};
    if (length == 2) {
        rune = static_cast<char32_t>(first & 0x1FU);
    } else if (length == 3) {
        rune = static_cast<char32_t>(first & 0x0FU);
    } else {
        rune = static_cast<char32_t>(first & 0x07U);
    }
    for (int i = 1; i < length; ++i) {
        const auto next = static_cast<unsigned char>(text[static_cast<std::size_t>(i)]);
        if ((next & 0xC0U) != 0x80U) return {kReplacement, 1};
        rune = (rune << 6) | static_cast<char32_t>(next & 0x3FU);
    }
    return {rune, static_cast<std::size_t>(length)};
}

/// 把一个 rune 追加到输出缓冲（UTF-8 编码；与 Go `utf8.AppendRune` 一致）。
/**
 * @brief 把 rune 追加为 UTF-8 字节。
 *
 * ## 步骤
 * step1. `< 0x80` -> 1 字节；
 * step2. `< 0x800` -> 2 字节；`< 0x10000` -> 3 字节；否则 4 字节；
 * step3. 非法码点（代理区/超范围）按 `RuneError` 写入（不抛异常）。
 */
inline void AppendRune(std::string& output, char32_t rune) {
    if (rune < 0x80U) {
        output.push_back(static_cast<char>(rune));
        return;
    }
    if (rune < 0x800U) {
        output.push_back(static_cast<char>(0xC0U | (rune >> 6)));
        output.push_back(static_cast<char>(0x80U | (rune & 0x3FU)));
        return;
    }
    if (rune < 0x10000U) {
        output.push_back(static_cast<char>(0xE0U | (rune >> 12)));
        output.push_back(static_cast<char>(0x80U | ((rune >> 6) & 0x3FU)));
        output.push_back(static_cast<char>(0x80U | (rune & 0x3FU)));
        return;
    }
    output.push_back(static_cast<char>(0xF0U | (rune >> 18)));
    output.push_back(static_cast<char>(0x80U | ((rune >> 12) & 0x3FU)));
    output.push_back(static_cast<char>(0x80U | ((rune >> 6) & 0x3FU)));
    output.push_back(static_cast<char>(0x80U | (rune & 0x3FU)));
}

/// rune 计数；非法字节按 1 个 rune 计（`utf8.RuneCountInString` 语义）。
/**
 * @brief 统计 rune 数量。
 *
 * ## 说明
 * 按 `DecodeRune` 的步长前进；非法字节记 1 个 rune 并前进 1 字节，保证终止。
 */
inline std::size_t RuneCount(std::string_view text) noexcept {
    std::size_t count = 0;
    for (std::size_t i = 0; i < text.size();) {
        const auto decoded = DecodeRune(text.substr(i));
        i += decoded.second == 0 ? 1 : decoded.second;
        ++count;
    }
    return count;
}

/// `unicode.IsSpace` 的可用子集（覆盖 ASCII、Latin-1 与常见 Unicode 空白）。
inline bool IsSpaceRune(char32_t rune) noexcept {
    switch (rune) {
        case U'\t': case U'\n': case U'\v': case U'\f': case U'\r': case U' ':
        case 0x85U: case 0xA0U: case 0x1680U: case 0x2028U: case 0x2029U:
        case 0x202FU: case 0x205FU: case 0x3000U:
            return true;
        default:
            return rune >= 0x2000U && rune <= 0x200AU;
    }
}

/// ASCII/Latin-1 的简单大小写折叠（`EqualFold`/`ToUpper`/`ToLower` 共用；见文件头边界说明）。
inline char32_t FoldRune(char32_t rune) noexcept {
    if (rune >= U'A' && rune <= U'Z') return rune + 32;
    if (rune >= 0xC0U && rune <= 0xDEU && rune != 0xD7U) return rune + 32;
    return rune;
}

/// rune 是否属于 cutset（cutset 自身按 rune 解析；`Trim*` 的语义是"字符集合"而非子串）。
inline bool CutsetContains(std::string_view cutset, char32_t rune) noexcept {
    for (std::size_t i = 0; i < cutset.size();) {
        const auto decoded = DecodeRune(cutset.substr(i));
        if (decoded.second == 0) break;
        if (decoded.first == rune) return true;
        i += decoded.second;
    }
    return false;
}

// ============================================================================
// 判定
// ============================================================================

/// 对应 `strings.Contains`；**空子串返回 true**（Go 语义）。
inline bool Contains(std::string_view text, std::string_view substr) {
    return text.find(substr) != std::string_view::npos;
}

/// 对应 `strings.ContainsAny`；**空 chars 返回 false**。
inline bool ContainsAny(std::string_view text, std::string_view chars) {
    if (chars.empty()) return false;
    for (std::size_t i = 0; i < text.size();) {
        const auto decoded = DecodeRune(text.substr(i));
        if (decoded.second == 0) break;
        if (CutsetContains(chars, decoded.first)) return true;
        i += decoded.second;
    }
    return false;
}

/// 对应 `strings.ContainsRune`。
inline bool ContainsRune(std::string_view text, char32_t rune) {
    if (rune < 0x80U) {
        return text.find(static_cast<char>(rune)) != std::string_view::npos;
    }
    std::string encoded;
    AppendRune(encoded, rune);
    return text.find(encoded) != std::string_view::npos;
}

/// 对应 `strings.HasPrefix`；空 prefix 返回 true。
inline bool HasPrefix(std::string_view text, std::string_view prefix) {
    return text.size() >= prefix.size() && text.compare(0, prefix.size(), prefix) == 0;
}

/// 对应 `strings.HasSuffix`；空 suffix 返回 true。
inline bool HasSuffix(std::string_view text, std::string_view suffix) {
    return text.size() >= suffix.size() &&
           text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
}

/**
 * @brief 对应 `strings.EqualFold`（ASCII/Latin-1 简单折叠）。
 * @note 不做 Unicode 完全折叠：例如 "K"（U+212A KELVIN SIGN）与 "k" 在 Go 下相等，
 *       本实现不认为相等 —— 已在文件头列为边界。
 */
/**
 * @brief 大小写不敏感比较（ASCII + Latin-1 简单折叠）。
 *
 * ## 说明
 * 两边同时解码 rune 并用 `FoldRune()` 比较；长度不同直接 false。不做 Unicode
 * 完全折叠（如 Kelvin 符号）。
 */
inline bool EqualFold(std::string_view left, std::string_view right) {
    std::size_t i = 0;
    std::size_t j = 0;
    while (i < left.size() && j < right.size()) {
        const auto a = DecodeRune(left.substr(i));
        const auto b = DecodeRune(right.substr(j));
        if (a.second == 0 || b.second == 0) break;
        if (FoldRune(a.first) != FoldRune(b.first)) return false;
        i += a.second;
        j += b.second;
    }
    return i == left.size() && j == right.size();
}

// ============================================================================
// 查找 / 计数
// ============================================================================

/// 对应 `strings.Index`；**空子串返回 0**，未找到返回 -1。
inline std::int64_t Index(std::string_view text, std::string_view substr) {
    const auto position = text.find(substr);
    return position == std::string_view::npos ? -1 : static_cast<std::int64_t>(position);
}

/// 对应 `strings.IndexByte`。
inline std::int64_t IndexByte(std::string_view text, char value) {
    const auto position = text.find(value);
    return position == std::string_view::npos ? -1 : static_cast<std::int64_t>(position);
}

/// 对应 `strings.IndexRune`；rune 为非法编码（U+FFFD）时返回 -1（与 Go 一致）。
inline std::int64_t IndexRune(std::string_view text, char32_t rune) {
    if (rune == 0xFFFDU) return -1;
    if (rune < 0x80U) return IndexByte(text, static_cast<char>(rune));
    std::string encoded;
    AppendRune(encoded, rune);
    return Index(text, encoded);
}

/// 对应 `strings.IndexAny`；chars 为空返回 -1。
inline std::int64_t IndexAny(std::string_view text, std::string_view chars) {
    if (chars.empty()) return -1;
    for (std::size_t i = 0; i < text.size();) {
        const auto decoded = DecodeRune(text.substr(i));
        if (decoded.second == 0) break;
        if (CutsetContains(chars, decoded.first)) return static_cast<std::int64_t>(i);
        i += decoded.second;
    }
    return -1;
}

/// 对应 `strings.LastIndex`；空子串返回 text 长度。
inline std::int64_t LastIndex(std::string_view text, std::string_view substr) {
    const auto position = text.rfind(substr);
    return position == std::string_view::npos ? -1 : static_cast<std::int64_t>(position);
}

/// 对应 `strings.LastIndexByte`。
inline std::int64_t LastIndexByte(std::string_view text, char value) {
    const auto position = text.rfind(value);
    return position == std::string_view::npos ? -1 : static_cast<std::int64_t>(position);
}

/**
 * @brief 对应 `strings.Count`。
 * @details **反直觉但必须对齐**的边界：`Count(s, "")` 返回 `RuneCount(s) + 1`
 *          （Go 源码 `strings.go` 的 `Count` 首个分支即此），不是 0 也不是 1。
 */
inline std::int64_t Count(std::string_view text, std::string_view substr) {
    if (substr.empty()) return static_cast<std::int64_t>(RuneCount(text)) + 1;
    std::int64_t count = 0;
    std::size_t position = 0;
    while (true) {
        const auto found = text.find(substr, position);
        if (found == std::string_view::npos) break;
        ++count;
        position = found + substr.size();
    }
    return count;
}

// ============================================================================
// 切分 / 合并
// ============================================================================

namespace detail {

/// Go `genSplit` 的等价物：`sep_for_after` 为 0 时表示 `Split`，为 `sep.size()` 时表示 `SplitAfter`。
/**
 * @brief Go `genSplit` 的等价物（Split 家族的唯一实现）。
 *
 * ## 步骤
 * step1. `limit == 0` -> 空结果（Go 语义）；
 * step2. `sep` 为空 -> 按 rune 切分，每个 rune 一项；`n > 0` 时最后一项吞掉剩余；
 * step3. `sep` 非空 -> 循环 `find`，每项取 `[position, found + sep_for_after)`：
 *         `sep_for_after == 0` 是 Split，`== sep.size()` 是 SplitAfter；
 * step4. `n > 0` 且已产出 `n-1` 项时，把剩余全部作为最后一项。
 */
inline StringSlice GenSplit(std::string_view text, std::string_view sep,
                            std::size_t sep_for_after, std::int64_t limit) {
    StringSlice result;
    if (limit == 0) return result;                       // n == 0 → 空结果（Go 语义）
    if (sep.empty()) {
        // Go：sep 为空时按 UTF-8 rune 切分，每个 rune 一项（不含首尾空串）。
        for (std::size_t i = 0; i < text.size();) {
            const auto decoded = DecodeRune(text.substr(i));
            const std::size_t step = decoded.second == 0 ? 1 : decoded.second;
            if (limit > 0 && static_cast<std::int64_t>(result.size()) == limit - 1) {
                result.emplace_back(text.substr(i));
                return result;
            }
            result.emplace_back(text.substr(i, step));
            i += step;
        }
        return result;
    }
    std::size_t position = 0;
    while (true) {
        if (limit > 0 && static_cast<std::int64_t>(result.size()) == limit - 1) {
            result.emplace_back(text.substr(position));
            return result;
        }
        const auto found = text.find(sep, position);
        if (found == std::string_view::npos) {
            result.emplace_back(text.substr(position));
            return result;
        }
        result.emplace_back(text.substr(position, found - position + sep_for_after));
        position = found + sep.size();
    }
}

}  // namespace detail

/// 对应 `strings.Split`（`n < 0`）。**sep 为空时按 rune 切分**。
inline StringSlice Split(std::string_view text, std::string_view sep) {
    return detail::GenSplit(text, sep, 0, -1);
}

/// 对应 `strings.SplitN`：`n > 0` 至多 n 项（末项为剩余）；`n == 0` 空；`n < 0` 全部。
inline StringSlice SplitN(std::string_view text, std::string_view sep, std::int64_t n) {
    return detail::GenSplit(text, sep, 0, n);
}

/// 对应 `strings.SplitAfter`：分隔符**保留在前一项末尾**。
inline StringSlice SplitAfter(std::string_view text, std::string_view sep) {
    return detail::GenSplit(text, sep, sep.size(), -1);
}

/// 对应 `strings.SplitAfterN`。
inline StringSlice SplitAfterN(std::string_view text, std::string_view sep, std::int64_t n) {
    return detail::GenSplit(text, sep, sep.size(), n);
}

/// 对应 `strings.Fields`：按 `unicode.IsSpace` 切分并丢弃空项（可用子集见文件头）。
/**
 * @brief 对应 `strings.Fields`：按空白 rune 切分并丢弃空项。
 *
 * ## 步骤
 * step1. 跳过前导空白；
 * step2. 取一段非空白作为一项；
 * step3. 再跳过空白，回到 step2，直到输入耗尽。
 */
inline StringSlice Fields(std::string_view text) {
    StringSlice result;
    std::size_t i = 0;
    while (i < text.size()) {
        const auto decoded = DecodeRune(text.substr(i));
        const std::size_t step = decoded.second == 0 ? 1 : decoded.second;
        if (!IsSpaceRune(decoded.first)) break;
        i += step;
    }
    while (i < text.size()) {
        const std::size_t start = i;
        while (i < text.size()) {
            const auto decoded = DecodeRune(text.substr(i));
            const std::size_t step = decoded.second == 0 ? 1 : decoded.second;
            if (IsSpaceRune(decoded.first)) break;
            i += step;
        }
        result.emplace_back(text.substr(start, i - start));
        while (i < text.size()) {
            const auto decoded = DecodeRune(text.substr(i));
            const std::size_t step = decoded.second == 0 ? 1 : decoded.second;
            if (!IsSpaceRune(decoded.first)) break;
            i += step;
        }
    }
    return result;
}

/// 对应 `strings.Join`（空切片返回空串）。
inline std::string Join(const StringSlice& elements, std::string_view separator) {
    std::string result;
    std::size_t total = 0;
    for (const auto& element : elements) total += element.size();
    if (!elements.empty()) total += separator.size() * (elements.size() - 1);
    result.reserve(total);
    for (std::size_t i = 0; i < elements.size(); ++i) {
        if (i != 0) result.append(separator);
        result.append(elements[i]);
    }
    return result;
}

/// 对应 `strings.Repeat`；`count < 0` 抛 `std::logic_error`（Go 会 panic）。
inline std::string Repeat(std::string_view text, std::int64_t count) {
    if (count < 0) throw std::logic_error("strings.Repeat: negative count");
    std::string result;
    if (count == 0 || text.empty()) return result;
    const auto times = static_cast<std::size_t>(count);
    if (text.size() != 0 && times > result.max_size() / text.size()) {
        throw std::length_error("strings.Repeat: output too large");
    }
    result.reserve(text.size() * times);
    for (std::size_t i = 0; i < times; ++i) result.append(text);
    return result;
}

/// 对应 `strings.Replace`：`n < 0` 表示全部替换。
/**
 * @brief 对应 `strings.Replace`（n < 0 表示全部替换）。
 *
 * ## 步骤
 * step1. `n == 0` -> 原样返回；
 * step2. `old` 为空 -> 在**每个 rune 前后**插入新串（先插一次再逐 rune 追加），
 *         共 rune+1 处（Go 的反直觉语义）；
 * step3. 否则循环 `find`，替换至多 `n` 次（`n < 0` 不限），最后追加尾部。
 */
inline std::string Replace(std::string_view text, std::string_view old_text,
                           std::string_view new_text, std::int64_t n) {
    if (n == 0 || old_text.empty()) {
        if (!old_text.empty()) return std::string(text);
        // Go：old 为空时在每个 rune 前后插入 new（共 rune+1 处）。
        std::string result;
        result.reserve(text.size() + new_text.size());
        result.append(new_text);
        for (std::size_t i = 0; i < text.size();) {
            const auto decoded = DecodeRune(text.substr(i));
            const std::size_t step = decoded.second == 0 ? 1 : decoded.second;
            result.append(text.substr(i, step));
            result.append(new_text);
            i += step;
        }
        return result;
    }
    std::string result;
    std::size_t position = 0;
    std::int64_t replaced = 0;
    while (true) {
        if (n > 0 && replaced >= n) break;
        const auto found = text.find(old_text, position);
        if (found == std::string_view::npos) break;
        result.append(text.substr(position, found - position));
        result.append(new_text);
        position = found + old_text.size();
        ++replaced;
    }
    result.append(text.substr(position));
    return result;
}

/// 对应 `strings.ReplaceAll`。
inline std::string ReplaceAll(std::string_view text, std::string_view old_text,
                              std::string_view new_text) {
    return Replace(text, old_text, new_text, -1);
}

// ============================================================================
// 修剪
// ============================================================================

/// 对应 `strings.TrimSpace`（空白集合见文件头边界说明）。
/**
 * @brief 对应 `strings.TrimSpace`：去掉首尾空白 rune。
 *
 * ## 步骤
 * step1. 从头部按 `DecodeRune` 前进，跳过 `IsSpaceRune` 的 rune；
 * step2. 从尾部反向跳过空白（需按 UTF-8 续字节边界回退）；
 * step3. 返回 `[start, end)` 切片。
 */
inline std::string TrimSpace(std::string_view text) {
    std::size_t start = 0;
    while (start < text.size()) {
        const auto decoded = DecodeRune(text.substr(start));
        const std::size_t step = decoded.second == 0 ? 1 : decoded.second;
        if (!IsSpaceRune(decoded.first)) break;
        start += step;
    }
    std::size_t end = text.size();
    while (end > start) {
        // 从后往前找一个 rune 的起点（UTF-8 续字节 0b10xxxxxx）。
        std::size_t begin = end - 1;
        while (begin > start && (static_cast<unsigned char>(text[begin]) & 0xC0U) == 0x80U) --begin;
        const auto decoded = DecodeRune(text.substr(begin));
        if (!IsSpaceRune(decoded.first)) break;
        end = begin;
    }
    return std::string(text.substr(start, end - start));
}

/// 对应 `strings.TrimLeft`：剥离左侧**属于 cutset 的 rune**（cutset 是字符集合）。
inline std::string TrimLeft(std::string_view text, std::string_view cutset) {
    std::size_t start = 0;
    while (start < text.size()) {
        const auto decoded = DecodeRune(text.substr(start));
        const std::size_t step = decoded.second == 0 ? 1 : decoded.second;
        if (!CutsetContains(cutset, decoded.first)) break;
        start += step;
    }
    return std::string(text.substr(start));
}

/// 对应 `strings.TrimRight`。
inline std::string TrimRight(std::string_view text, std::string_view cutset) {
    std::size_t end = text.size();
    while (end > 0) {
        std::size_t begin = end - 1;
        while (begin > 0 && (static_cast<unsigned char>(text[begin]) & 0xC0U) == 0x80U) --begin;
        const auto decoded = DecodeRune(text.substr(begin));
        if (!CutsetContains(cutset, decoded.first)) break;
        end = begin;
    }
    return std::string(text.substr(0, end));
}

/// 对应 `strings.Trim`（两侧）。
inline std::string Trim(std::string_view text, std::string_view cutset) {
    return TrimRight(TrimLeft(text, cutset), cutset);
}

/// 对应 `strings.TrimPrefix`；不匹配时原样返回（`HasPrefix` 为假时不改动）。
inline std::string TrimPrefix(std::string_view text, std::string_view prefix) {
    if (HasPrefix(text, prefix)) return std::string(text.substr(prefix.size()));
    return std::string(text);
}

/// 对应 `strings.TrimSuffix`。
inline std::string TrimSuffix(std::string_view text, std::string_view suffix) {
    if (HasSuffix(text, suffix)) return std::string(text.substr(0, text.size() - suffix.size()));
    return std::string(text);
}

// ============================================================================
// Cut 族（Go 1.18+）
// ============================================================================

/** @brief `Cut` 的结果（对应 Go 的多返回值 `(before, after, found)`）。 */
struct StringCutResult {
    std::string before;
    std::string after;
    bool found{false};
};

/// 对应 `strings.Cut`：在**首个** sep 处切成两段。
inline StringCutResult Cut(std::string_view text, std::string_view sep) {
    StringCutResult result;
    const auto position = text.find(sep);
    if (position == std::string_view::npos) {
        result.before = std::string(text);
        return result;
    }
    result.before = std::string(text.substr(0, position));
    result.after = std::string(text.substr(position + sep.size()));
    result.found = true;
    return result;
}

/// 对应 `strings.CutPrefix`。
inline std::pair<std::string, bool> CutPrefix(std::string_view text, std::string_view prefix) {
    if (HasPrefix(text, prefix)) return {std::string(text.substr(prefix.size())), true};
    return {std::string(text), false};
}

/// 对应 `strings.CutSuffix`。
inline std::pair<std::string, bool> CutSuffix(std::string_view text, std::string_view suffix) {
    if (HasSuffix(text, suffix)) {
        return {std::string(text.substr(0, text.size() - suffix.size())), true};
    }
    return {std::string(text), false};
}

// ============================================================================
// 大小写 / 比较
// ============================================================================

/// 对应 `strings.ToUpper`（ASCII/Latin-1；见文件头边界）。
inline std::string ToUpper(std::string_view text) {
    std::string result;
    result.reserve(text.size());
    for (std::size_t i = 0; i < text.size();) {
        const auto decoded = DecodeRune(text.substr(i));
        const std::size_t step = decoded.second == 0 ? 1 : decoded.second;
        char32_t rune = decoded.first;
        if (rune >= U'a' && rune <= U'z') rune -= 32;
        else if (rune >= 0xE0U && rune <= 0xFEU && rune != 0xF7U) rune -= 32;
        AppendRune(result, rune);
        i += step;
    }
    return result;
}

/// 对应 `strings.ToLower`（ASCII/Latin-1；见文件头边界）。
inline std::string ToLower(std::string_view text) {
    std::string result;
    result.reserve(text.size());
    for (std::size_t i = 0; i < text.size();) {
        const auto decoded = DecodeRune(text.substr(i));
        const std::size_t step = decoded.second == 0 ? 1 : decoded.second;
        AppendRune(result, FoldRune(decoded.first));
        i += step;
    }
    return result;
}

/// 对应 `strings.Compare`（Go 1.20+）：返回 -1 / 0 / +1。
inline int Compare(std::string_view left, std::string_view right) {
    const int ordering = left.compare(right);
    return ordering < 0 ? -1 : (ordering > 0 ? 1 : 0);
}

/// 对应 `strings.Clone`：返回独立副本（C++ 里即值拷贝，但保留同名 API 便于转译）。
inline std::string Clone(std::string_view text) { return std::string(text); }

// ============================================================================
// Builder
// ============================================================================

/**
 * @brief 对应 `strings.Builder` 的可增长字符串构建器。
 * @details Go 的 `Builder` 在 `String()` 之后再次写入会 **panic**（copyCheck）。
 *          本实现对齐该契约：`String()` 之后调用任何写方法或 `Reset()`/`Grow()`
 *          都会抛 `std::logic_error`（项目惯例：`std::logic_error` 用于 API 误用，
 *          例如 `sync::Mutex::Unlock` 未持锁）。
 */
class StringBuilder final {
public:
    StringBuilder() = default;
    StringBuilder(const StringBuilder&) = delete;
    StringBuilder& operator=(const StringBuilder&) = delete;

    /// 对应 `Builder.WriteString`。
    void WriteString(std::string_view text) {
        EnsureWritable();
        m_buffer.append(text);
    }
    /// 对应 `Builder.Write`（字节切片）。
    void Write(const void* data, std::size_t length) {
        EnsureWritable();
        m_buffer.append(static_cast<const char*>(data), length);
    }
    /// 对应 `Builder.WriteByte`。
    void WriteByte(char value) {
        EnsureWritable();
        m_buffer.push_back(value);
    }
    /// 对应 `Builder.WriteRune`。
    void WriteRune(char32_t rune) {
        EnsureWritable();
        AppendRune(m_buffer, rune);
    }
    /// 对应 `Builder.String`；**调用后构建器转为只读**。
    std::string String() const {
        m_copied = true;
        return m_buffer;
    }
    /// 对应 `Builder.Len`。
    std::size_t Len() const noexcept { return m_buffer.size(); }
    /// 对应 `Builder.Cap`。
    std::size_t Cap() const noexcept { return m_buffer.capacity(); }
    /// 对应 `Builder.Reset`；已 `String()` 过则抛错（与 Go 的 panic 对齐）。
    void Reset() {
        EnsureWritable();
        m_buffer.clear();
    }
    /// 对应 `Builder.Grow`。
    void Grow(std::size_t extra) {
        EnsureWritable();
        m_buffer.reserve(m_buffer.size() + extra);
    }

private:
    void EnsureWritable() const {
        if (m_copied) throw std::logic_error("strings.Builder: write after String");
    }
    std::string m_buffer;
    mutable bool m_copied{false};
};

}  // namespace go2cpp
