/**
 * @file hex.hpp
 * @brief Go `encoding/hex` 的 C++ 对应实现。
 *
 * @author DeepSeek Harness
 * @date 2026-10-18
 * @copyright BSD-3-Clause（语义对齐 Go 标准库；本文件为独立实现）
 *
 * @details 复用优先的结论（检索日期 2026-10-18）：
 *   | 候选 | 结论 |
 *   |---|---|
 *   | Go 标准库 `encoding/hex`（Go 1.23.0，BSD-3-Clause） | **语义参照**：输出小写、
 *     解码接受大小写、奇数长度与非法字节分别报 `ErrLength` / `InvalidByteError` |
 *   | [cppcodec](https://github.com/tplgy/cppcodec)（BSD-2，header-only） | **不采用**：
 *     它的错误模型与 Go 的两类错误锚点不同，且会为 40 行逻辑引入整棵头文件树 |
 *   | 自研 | **采用**：十六进制编解码足够小，严格对齐错误语义比适配第三方更省事 |
 *
 * ## 关键语义
 *
 * - `Encode` **输出小写**（与 Go 一致：`hex.EncodeToString([]byte{0xAB}) == "ab"`）。
 * - `Decode` **同时接受大写与小写**，混合大小写也接受（Go 的 `fromHexChar` 行为）。
 * - `Decode` 遇到奇数长度返回 `HexOddLength()` 锚点错误；遇非法字符返回
 *   `HexInvalidByte()` 锚点错误，文本包含字符与下标（对齐 Go `InvalidByteError`）。
 * - `Decode` 失败时不修改输出串（强异常安全）；成功时**追加**到 `dst`。
 * - 二进制数据用 `std::string` 承载（`char` 按无符号字节解释），与项目其他模块一致。
 *
 * ## 模块定位
 *
 * `encoding` 的十六进制工具：把字节编码成小写 hex 文本，或把 hex 文本解码回
 * 字节。上游是需要可读性/调试输出或十六进制协议的模块，下游只有
 * `go2cpp::tools::error`。实现全部在 `src/encoding/hex.cpp`。
 *
 * ## 本文件定义什么
 *
 * - `HexOddLength()` / `HexInvalidByte()`：两个错误身份锚点；
 * - `HexDecodeResult`：`Hex::DecodeString` 的 `(data, error)` 返回；
 * - `Hex`：纯静态函数集合（长度换算、编码、解码）。
 *
 * ## 阅读建议
 *
 * 先读「关键语义」的「输出小写、解码大小写不敏感、失败不改 dst」三点，
 * 其余 API 都是围绕这三点的薄封装。
 *
 * ## 边界与陷阱
 *
 * - `DecodedLen` 只做 `src_len / 2`，不校验奇数长度；真正校验在 `Decode`；
 * - `Decode` 失败时 `dst` 不变，错误文本包含字符与下标；
 * - 输入按 `unsigned char` 解释，高位字节不会被符号扩展。
 */

#pragma once

#include "go2cpp/tools/error.hpp"

#include <cstddef>
#include <string>
#include <string_view>

namespace go2cpp::encoding {

/// 对应 Go `hex.ErrLength` 的进程级身份锚点（奇数长度）。
inline ErrorPtr HexOddLength() {
    static const ErrorPtr* const kError =
        new ErrorPtr(NewError("encoding/hex: odd length hex string"));
    return *kError;
}

/**
 * @brief 对应 Go `hex.InvalidByteError` 的身份锚点。
 * @note 具体错误文本通过 `Wrap` 携带字符与下标；身份判断用 `Is(err, HexInvalidByte())`。
 */
inline ErrorPtr HexInvalidByte() {
    static const ErrorPtr* const kError =
        new ErrorPtr(NewError("encoding/hex: invalid byte"));
    return *kError;
}

/**
 * @brief `Hex::DecodeString` 的返回值：对应 Go 的 `([]byte, error)`。
 *
 * ## 定位
 * `Hex::DecodeString()` 的返回类型；调用方用 `operator bool` 判成功，
 * 失败时 `error` 是 `HexOddLength()` 或带下标的 `HexInvalidByte()`。
 */
struct HexDecodeResult {
    /// 解码结果；`error` 非空时为空串。
    std::string data;
    /// 成功为 `nullptr`。
    ErrorPtr error;

    /// 便于直接判断成功。
    explicit operator bool() const noexcept { return error == nullptr; }
};

/**
 * @brief Go `encoding/hex` 的静态函数集合。
 *
 * @details 用 `struct Hex` 而非命名空间，是为了让调用点与需求中的
 *          `encoding::Hex::EncodeToString` 完全对应，同时保持头文件可整体包含。
 *
 * ## 定位
 * `encoding` 的十六进制工具集，无状态、纯静态；被需要 hex 文本的日志、
 * 调试输出、协议字段解析调用。
 *
 * ## 不变量
 * - `Encode` 输出恒为小写；`Decode` 同时接受大小写；
 * - `EncodedLen(n) == n * 2`；`DecodedLen(n) == n / 2`（不校验合法性）；
 * - `Decode` 失败时 `dst` 保持不变，成功时追加而不是覆盖。
 */
struct Hex {
    /// 编码后长度：`src_len * 2`。
    static std::size_t EncodedLen(std::size_t src_len) noexcept { return src_len * 2; }

    /// 解码后长度：`src_len / 2`（不校验奇数长度）。
    static std::size_t DecodedLen(std::size_t src_len) noexcept { return src_len / 2; }

    /// 追加小写十六进制到 `dst`，返回追加字节数。
    static std::size_t Encode(std::string& dst, std::string_view src);

    /// 对应 Go `hex.EncodeToString`。
    static std::string EncodeToString(std::string_view src);

    /// 追加解码结果到 `dst`；失败返回错误且 `dst` 不变。
    static ErrorPtr Decode(std::string& dst, std::string_view src);

    /// 对应 Go `hex.DecodeString`：返回 `(data, error)`。
    static HexDecodeResult DecodeString(std::string_view src);
};

}  // namespace go2cpp::encoding
