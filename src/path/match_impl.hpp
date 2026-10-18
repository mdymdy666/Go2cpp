/**
 * @file match_impl.hpp
 * @brief `path::Match` / `Filepath::Match` 的共享实现（内部头，不对外安装）。
 *
 * @author DeepSeek Harness
 * @date 2026-10-18
 *
 * @details 移植 Go 1.27.1 `src/path/match.go` 与
 *          `src/path/filepath/match.go`（BSD-3-Clause）。两个包的唯一差别是：
 *          - 分隔符字符（`path` 用 `/`，Windows `filepath` 用 `\`）；
 *          - Windows 上 `\` 不启用转义（`escape_enabled == false`）。
 *
 *          因此把算法抽成按分隔符参数化的实现，避免两份几乎相同的代码漂移。
 *          逐字节对照上游，未删边界检查：模式非法时仍会继续把剩余模式解析完，
 *          以区分「不匹配」与「模式错误」（Go 的 `failed` 标志逻辑）。
 */

#pragma once

#include "go2cpp/path/path.hpp"

#include <cstddef>
#include <string_view>

namespace go2cpp::path::detail {

namespace match_internal {

/// UTF-8 解码结果：rune + 消耗字节数；非法字节返回 U+FFFD + 1（同 Go）。
struct RuneDecode final {
    char32_t rune{0xFFFD};
    std::size_t size{1};
};

inline RuneDecode DecodeRune(std::string_view s) noexcept {
    if (s.empty()) {
        return {};
    }
    const auto first = static_cast<unsigned char>(s[0]);
    if (first < 0x80) {
        return {static_cast<char32_t>(first), 1};
    }
    std::size_t need = 0;
    char32_t rune = 0;
    char32_t minimum = 0;
    if ((first & 0xE0) == 0xC0) {
        need = 1;
        rune = first & 0x1F;
        minimum = 0x80;
    } else if ((first & 0xF0) == 0xE0) {
        need = 2;
        rune = first & 0x0F;
        minimum = 0x800;
    } else if ((first & 0xF8) == 0xF0) {
        need = 3;
        rune = first & 0x07;
        minimum = 0x10000;
    } else {
        return {};
    }
    if (s.size() < need + 1) {
        return {};
    }
    for (std::size_t i = 1; i <= need; ++i) {
        const auto ch = static_cast<unsigned char>(s[i]);
        if ((ch & 0xC0) != 0x80) {
            return {};
        }
        rune = (rune << 6) | (ch & 0x3F);
    }
    // 过长编码、代理区、超出 U+10FFFF 一律按 Go 的 RuneError 处理。
    if (rune < minimum || rune > 0x10FFFF ||
        (rune >= 0xD800 && rune <= 0xDFFF)) {
        return {};
    }
    return {rune, need + 1};
}

/// 取下一个模式 chunk：可能带前导 `*`，`chunk` 是不含 `*` 的单字符算子串。
inline void ScanChunk(std::string_view pattern, bool* star,
                      std::string_view* chunk, std::string_view* rest) {
    *star = false;
    while (!pattern.empty() && pattern.front() == '*') {
        pattern.remove_prefix(1);
        *star = true;
    }
    bool in_range = false;
    for (std::size_t i = 0; i < pattern.size(); ++i) {
        const char c = pattern[i];
        if (c == '\\') {
            // 转义是否合法交给 MatchChunk 判断。
            if (i + 1 < pattern.size()) {
                ++i;
            }
        } else if (c == '[') {
            in_range = true;
        } else if (c == ']') {
            in_range = false;
        } else if (c == '*' && !in_range) {
            *chunk = pattern.substr(0, i);
            *rest = pattern.substr(i);
            return;
        }
    }
    *chunk = pattern;
    *rest = std::string_view{};
}

/**
 * @brief 读取字符类里的一个「可能带转义」的字符。
 * @return 成功返回 true 并写入 rune、剩余 chunk；失败（模式非法）返回 false。
 */
inline bool GetEsc(std::string_view chunk, char32_t* rune,
                   std::string_view* next, bool escape_enabled) {
    if (chunk.empty() || chunk.front() == '-' || chunk.front() == ']') {
        return false;
    }
    if (escape_enabled && chunk.front() == '\\') {
        chunk.remove_prefix(1);
        if (chunk.empty()) {
            return false;
        }
    }
    const RuneDecode decoded = DecodeRune(chunk);
    if (decoded.rune == 0xFFFD && decoded.size == 1) {
        // Go 的 getEsc 把非法 UTF-8 视为模式错误；但真正的 U+FFFD 字节序列
        // 解码为 size>1，不会被误判。
        return false;
    }
    chunk.remove_prefix(decoded.size);
    if (chunk.empty()) {
        return false;
    }
    *rune = decoded.rune;
    *next = chunk;
    return true;
}

/**
 * @brief 检查 chunk（字面量/`?`/字符类）是否匹配 s 的前缀。
 * @return 匹配时写入剩余 s 并返回 true；同时通过 `bad_pattern` 报语法错误。
 */
inline bool MatchChunk(std::string_view chunk, std::string_view s,
                       char separator, bool escape_enabled,
                       std::string_view* rest, bool* bad_pattern) {
    *bad_pattern = false;
    bool failed = false;
    while (!chunk.empty()) {
        failed = failed || s.empty();
        const char op = chunk.front();
        if (op == '[') {
            std::size_t count = 0;
            char32_t rune = 0;
            if (!failed) {
                const RuneDecode decoded = DecodeRune(s);
                rune = decoded.rune;
                s.remove_prefix(decoded.size);
            }
            chunk.remove_prefix(1);
            bool negated = false;
            if (!chunk.empty() && chunk.front() == '^') {
                negated = true;
                chunk.remove_prefix(1);
            }
            bool matched = false;
            for (;;) {
                if (!chunk.empty() && chunk.front() == ']' && count > 0) {
                    chunk.remove_prefix(1);
                    break;
                }
                char32_t lo = 0;
                if (!GetEsc(chunk, &lo, &chunk, escape_enabled)) {
                    *bad_pattern = true;
                    return false;
                }
                char32_t hi = lo;
                if (!chunk.empty() && chunk.front() == '-') {
                    if (!GetEsc(chunk.substr(1), &hi, &chunk, escape_enabled)) {
                        *bad_pattern = true;
                        return false;
                    }
                }
                matched = matched || (lo <= rune && rune <= hi);
                ++count;
            }
            failed = failed || (matched == negated);
        } else if (op == '?') {
            if (!failed) {
                failed = s.front() == separator;
                const RuneDecode decoded = DecodeRune(s);
                s.remove_prefix(decoded.size);
            }
            chunk.remove_prefix(1);
        } else if (op == '\\' && escape_enabled) {
            chunk.remove_prefix(1);
            if (chunk.empty()) {
                *bad_pattern = true;
                return false;
            }
            // fallthrough：按字面量比较被转义的字符。
            if (!failed) {
                failed = chunk.front() != s.front();
                s.remove_prefix(1);
            }
            chunk.remove_prefix(1);
        } else {
            if (!failed) {
                failed = op != s.front();
                s.remove_prefix(1);
            }
            chunk.remove_prefix(1);
        }
    }
    if (failed) {
        return false;
    }
    *rest = s;
    return true;
}

}  // namespace match_internal

/**
 * @brief 按分隔符参数化的 shell 匹配（`path.Match` / `filepath.Match` 的合体）。
 *
 * @param pattern 模式。
 * @param name 名字。
 * @param separator 分隔符（`/` 或 `\`），`*`/`?` 不跨过它。
 * @param escape_enabled `\` 是否作为转义符（Windows filepath 为 false）。
 */
inline MatchResult MatchWithSeparator(std::string_view pattern,
                                      std::string_view name, char separator,
                                      bool escape_enabled) {
    using namespace match_internal;
    MatchResult result{};
    while (!pattern.empty()) {
        bool star = false;
        std::string_view chunk;
        std::string_view rest;
        ScanChunk(pattern, &star, &chunk, &rest);
        pattern = rest;
        if (star && chunk.empty()) {
            // 尾随 *：匹配剩余全部内容，但不能含分隔符。
            result.matched = name.find(separator) == std::string_view::npos;
            return result;
        }
        std::string_view tail;
        bool bad = false;
        const bool ok =
            MatchChunk(chunk, name, separator, escape_enabled, &tail, &bad);
        if (ok && (tail.empty() || !pattern.empty())) {
            name = tail;
            continue;
        }
        if (bad) {
            result.matched = false;
            result.bad_pattern = true;
            return result;
        }
        if (star) {
            // 尝试让 * 吞掉 1..i 个非分隔符字节。
            for (std::size_t i = 0; i < name.size() && name[i] != separator;
                 ++i) {
                bool bad2 = false;
                std::string_view tail2;
                if (MatchChunk(chunk, name.substr(i + 1), separator,
                               escape_enabled, &tail2, &bad2)) {
                    if (pattern.empty() && !tail2.empty()) {
                        continue;
                    }
                    name = tail2;
                    goto next_chunk;
                }
                if (bad2) {
                    result.matched = false;
                    result.bad_pattern = true;
                    return result;
                }
            }
        }
        // 不匹配：仍需把剩余模式解析完，以发现语法错误（Go 的行为）。
        while (!pattern.empty()) {
            bool star2 = false;
            std::string_view chunk2;
            std::string_view rest2;
            ScanChunk(pattern, &star2, &chunk2, &rest2);
            (void)star2;
            pattern = rest2;
            bool bad2 = false;
            std::string_view tail2;
            (void)MatchChunk(chunk2, std::string_view{}, separator,
                             escape_enabled, &tail2, &bad2);
            if (bad2) {
                result.matched = false;
                result.bad_pattern = true;
                return result;
            }
        }
        result.matched = false;
        return result;
    next_chunk:;
    }
    result.matched = name.empty();
    return result;
}

}  // namespace go2cpp::path::detail
