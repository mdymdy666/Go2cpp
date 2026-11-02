/**
 * @file hex_core.hpp
 * @brief 十六进制编解码的**零依赖核心**（只依赖标准库，不构造 `ErrorPtr`）。
 *
 * @details 为什么和 `encoding/hex.hpp` 分成两层：
 *   `hex.hpp` 的 `Hex::Decode` 返回 `ErrorPtr`，需要 `go2cpp/error.hpp`
 *   （进而链接 `go2cpp_error`）。但仓库里有一批模块刻意保持「独立目标、最小
 *   依赖」——Bento 的 keystore / token_pairing / error mapping，以及
 *   `logging/redact.hpp`（文件头明确写「logging 只依赖 C++17 标准库」）。
 *   这些模块的自写 hex 表此前各有 1~2 处，正是本次要消除的重复。
 *   把纯字节逻辑下沉到本文件后：任何模块只要 include 它就能得到**同一份**
 *   hex 实现，且不引入任何链接依赖。
 *
 * ## 语义（与 `encoding/hex.hpp` 完全一致）
 *
 * - 编码**恒为小写**（对齐 Go `hex.EncodeToString`）；
 * - 解码**同时接受大写与小写**（对齐 Go `fromHexChar`）；
 * - `Decode` 先校验再写入：**失败时 `out` 不变**（强异常安全）；
 * - 失败分两类：奇数长度（`failure->odd_length == true`）与非法字符
 *   （`failure->index` 指向第一个非法字节）。
 *
 * ## 模块定位
 * `encoding::hex` 的底座：`Hex::Encode`/`Hex::Decode` 改为调用本文件；
 * 各业务模块的私有 hex 循环改为直接调用本文件。
 *
 * ## 边界与陷阱
 * - 本文件不生成错误文本（那需要动态分配与错误体系），只报告失败位置；
 * - `DecodedLen` 只做 `src_len / 2`，不校验奇数长度（真正校验在 `Decode`）；
 * - 输入按 `unsigned char` 解释，高位字节不会被符号扩展。
 */

#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace go2cpp::encoding::hexcore {

/// 小写字母表——全仓库唯一一份（编码输出用）。
inline constexpr char kLowerHexDigits[] = "0123456789abcdef";

/// 单字符转十六进制值；非法返回 -1。大小写都接受（对齐 Go `fromHexChar`）。
inline int HexValue(unsigned char c) noexcept {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/// 编码后长度：`src_len * 2`。
inline std::size_t EncodedLen(std::size_t src_len) noexcept { return src_len * 2; }

/// 解码后长度：`src_len / 2`（不校验奇数长度）。
inline std::size_t DecodedLen(std::size_t src_len) noexcept { return src_len / 2; }

/**
 * @brief 把 `src` 编码成小写 hex 并**追加**到 `out`。
 * @return 追加的字节数（`2 * src.size()`）。
 */
inline std::size_t AppendEncoded(std::string& out, std::string_view src) {
    const std::size_t start = out.size();
    out.reserve(start + EncodedLen(src.size()));
    for (const char raw : src) {
        const auto byte = static_cast<unsigned char>(raw);
        out.push_back(kLowerHexDigits[byte >> 4]);
        out.push_back(kLowerHexDigits[byte & 0x0f]);
    }
    return out.size() - start;
}

/// 追加单个字节的两个小写 hex 字符（`\xHH` 转义等单字节场景）。
inline void AppendEncodedByte(std::string& out, unsigned char byte) {
    out.push_back(kLowerHexDigits[byte >> 4]);
    out.push_back(kLowerHexDigits[byte & 0x0f]);
}

/// 编码为新的小写 hex 字符串。
inline std::string Encode(std::string_view src) {
    std::string out;
    AppendEncoded(out, src);
    return out;
}

/**
 * @brief 把字符串当成字节序列转小写 hex（不校验内容）。
 * @note 与 `Encode` 相同；单独保留这个别名是为了让调用点语义更直白
 *       （`std::string` 承载二进制数据是本仓库的既有约定）。
 */
inline std::string EncodeString(std::string_view src) { return Encode(src); }

/// 解码失败的定位信息。
struct DecodeFailure {
    /// 奇数长度（此时 `index` 无意义，恒为 0）。
    bool odd_length{false};
    /// 第一个非法字节的下标（`odd_length == false` 时有效）。
    std::size_t index{0};
};

/**
 * @brief 解码 hex 文本并**追加**到 `out`；失败时 `out` 保持不变。
 * @param failure 非空时写入失败原因与位置。
 * @return 成功返回 true。
 */
inline bool Decode(std::string& out, std::string_view src,
                   DecodeFailure* failure = nullptr) noexcept {
    if ((src.size() % 2U) != 0) {
        if (failure != nullptr) {
            failure->odd_length = true;
            failure->index = 0;
        }
        return false;
    }
    // 先完整校验再写入：失败时 out 不被部分写入（与 Hex::Decode 的强异常安全一致）。
    for (std::size_t i = 0; i < src.size(); i += 2) {
        if (HexValue(static_cast<unsigned char>(src[i])) < 0 ||
            HexValue(static_cast<unsigned char>(src[i + 1])) < 0) {
            if (failure != nullptr) {
                failure->odd_length = false;
                failure->index =
                    HexValue(static_cast<unsigned char>(src[i])) < 0 ? i : i + 1;
            }
            return false;
        }
    }
    out.reserve(out.size() + DecodedLen(src.size()));
    for (std::size_t i = 0; i < src.size(); i += 2) {
        const int high = HexValue(static_cast<unsigned char>(src[i]));
        const int low = HexValue(static_cast<unsigned char>(src[i + 1]));
        out.push_back(static_cast<char>((high << 4) | low));
    }
    return true;
}

/// 解码为新的字符串；失败时返回空串（用 `Decode(out, src, &failure)` 取失败位置）。
inline std::string DecodeString(std::string_view src) {
    std::string out;
    Decode(out, src);
    return out;
}

/// 是否「非空 + 偶数长度 + 全部是 hex 字符」。
inline bool IsHex(std::string_view src) noexcept {
    if (src.empty() || (src.size() % 2U) != 0) {
        return false;
    }
    for (const char raw : src) {
        if (HexValue(static_cast<unsigned char>(raw)) < 0) {
            return false;
        }
    }
    return true;
}

/// 是否「偶数长度 + 全部是 hex 字符」（允许空串）。
inline bool IsHexEven(std::string_view src) noexcept {
    if ((src.size() % 2U) != 0) {
        return false;
    }
    for (const char raw : src) {
        if (HexValue(static_cast<unsigned char>(raw)) < 0) {
            return false;
        }
    }
    return true;
}

}  // namespace go2cpp::encoding::hexcore
