/**
 * @file hex.cpp
 * @brief `go2cpp/encoding/hex.hpp` 的实现：小写编码 + 大小写不敏感解码。
 *
 * @author DeepSeek Harness
 * @date 2026-10-18
 * @copyright BSD-3-Clause
 *
 * ## 模块定位
 *
 * `hex.hpp` 的实现层：`Hex` 的四个静态函数与两个错误锚点都在这里落地。
 * 上游是日志/调试输出与十六进制协议解析，下游只有 `go2cpp::tools::error`。
 *
 * ## 本文件定义什么
 *
 * - `Hex::Encode`/`EncodeToString`：字节 -> 小写 hex 文本；
 * - `Hex::Decode`/`DecodeString`：hex 文本 -> 字节（大小写不敏感）；
 * - 内部工具：`HexValue`（单字符转值）、`DescribeByte`（Go 风格错误文本）。
 *
 * ## 阅读建议
 *
 * 先看 `Decode` 的奇数长度与非法字符两个失败分支，再看 `DecodeString`
 * 如何把失败结果清空。编码侧只需确认「始终小写」。
 *
 * ## 边界与陷阱
 *
 * `Decode` 先解码到临时 `out`，成功后才 `dst += out`；因此失败时 `dst`
 * 不会被部分写入。`DecodeString` 失败时会清空 `result.data`。
 */

#include "go2cpp/encoding/hex.hpp"

#include "go2cpp/encoding/errors.hpp"
#include "go2cpp/encoding/hex_core.hpp"

#include <cstdint>
#include <string>
#include <string_view>

namespace go2cpp::encoding {

namespace {

inline unsigned char ByteAt(std::string_view src, std::size_t index) noexcept {
    return static_cast<unsigned char>(src[index]);
}

/// 把非法字节格式化成 Go 风格的可读形式（`U+0067 'g'`）。
std::string DescribeByte(unsigned char c) {
    static const char kHex[] = "0123456789ABCDEF";
    std::string text = "encoding/hex: invalid byte: U+00";
    text.push_back(kHex[(c >> 4) & 0x0f]);
    text.push_back(kHex[c & 0x0f]);
    if (c >= 0x20 && c < 0x7f) {
        text += " '";
        text.push_back(static_cast<char>(c));
        text.push_back('\'');
    }
    return text;
}

}  // namespace

/**
 * @brief 把 `src` 编码成小写 hex 并追加到 `dst`。
 *
 * ## 步骤
 * step1. 记录追加起点并按 `2 * src.size()` 预留；
 * step2. 每个字节拆成高/低 4 位，各查一次小写字母表；
 * step3. 返回追加字节数。
 */
std::size_t Hex::Encode(std::string& dst, std::string_view src) {
    // 编码循环只有一份：encoding/encoding::hexcore::AppendEncoded（各模块也复用它）。
    return encoding::hexcore::AppendEncoded(dst, src);
}

std::string Hex::EncodeToString(std::string_view src) {
    std::string out;
    Encode(out, src);
    return out;
}

/**
 * @brief 解码 hex 文本并追加到 `dst`。
 *
 * ## 步骤
 * step1. 奇数长度直接返回 `HexOddLength()`；
 * step2. 每两个字符查一次值，任一字符非法就返回带下标的 `HexInvalidByte()`；
 * step3. 全部成功后才把临时 `out` 追加到 `dst`。
 *
 * ## 失败语义
 * 失败时 `dst` 保持不变；错误位置指向第一个非法字符。
 */
ErrorPtr Hex::Decode(std::string& dst, std::string_view src) {
    // 解码循环与「失败时不污染 dst」的语义都在 hexcore 里，这里只负责错误文本。
    encoding::hexcore::DecodeFailure failure;
    if (!encoding::hexcore::Decode(dst, src, &failure)) {
        if (failure.odd_length) {
            return detail::Tagged("encoding/hex: odd length hex string",
                                  HexOddLength());
        }
        return detail::Tagged(DescribeByte(ByteAt(src, failure.index)),
                              HexInvalidByte());
    }
    return nullptr;
}

HexDecodeResult Hex::DecodeString(std::string_view src) {
    HexDecodeResult result;
    result.error = Decode(result.data, src);
    if (result.error) {
        result.data.clear();
    }
    return result;
}

}  // namespace go2cpp::encoding
