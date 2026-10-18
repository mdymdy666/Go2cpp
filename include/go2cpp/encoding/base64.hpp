/**
 * @file base64.hpp
 * @brief Go `encoding/base64` 的 C++ 对应实现（Std / URL / Raw 四种编码）。
 *
 * @author DeepSeek Harness
 * @date 2026-10-18
 * @copyright BSD-3-Clause（语义对齐 Go 标准库；本文件为独立实现）
 *
 * @details 复用优先的结论（检索日期 2026-10-18）：
 *   | 候选 | 结论 |
 *   |---|---|
 *   | Go 标准库 `encoding/base64`（Go 1.23.0，BSD-3-Clause，见
 *     `third_party/go1.23.0.src.tar.gz`） | **语义参照**：本模块按其文档与测试语料
 *     对齐 API、字母表、填充与错误分类，但代码为独立实现，未复制上游 |
 *   | [cppcodec](https://github.com/tplgy/cppcodec)（BSD-2，header-only） | **不采用**：
 *     它的返回类型是 `std::vector<uint8_t>`、错误用异常 / 返回码表达，且没有与
 *     Go 一一对应的 `EncodedLen`/`DecodedLen` 与「输入字节位置」错误语义；为一个
 *     纯函数模块引入第三方头文件树不划算 |
 *   | OpenSSL `EVP_EncodeBlock`/`EVP_DecodeBlock` | **不采用**：引入 libcrypto 依赖；
 *     `EVP_DecodeBlock` 不区分 URL 安全字母表，也不报告非法字节位置 |
 *   | 自研 | **采用**：算法只有几十行，严格校验规则（字母表、长度、填充位置）
 *     必须与 Go 对齐，自研反而最容易验证清楚 |
 *
 * ## 关键语义（容易写错的地方）
 *
 * - 四种预定义编码与 Go 同名：`StdEncoding`（`+/`、`=`）、`URLEncoding`（`-_`、`=`）、
 *   `RawStdEncoding`（`+/`、无填充）、`RawURLEncoding`（`-_`、无填充）。
 * - `Encode(dst, src)` / `Decode(dst, src)` **追加**到 `dst` 末尾并返回追加的字节数，
 *   不覆盖 `dst` 原有内容；这与 Go 的 `AppendEncode`/`AppendDecode` 一致，
 *   但不同于 Go `Encode` 要求 `dst` 预先足够大的写法（C++ 没有 `panic`）。
 * - `Decode` 失败时 **不修改 `dst`**（先解码到临时缓冲，成功才追加）；Go 的
 *   `Decode` 会返回已解出的部分字节，这里选择强异常安全，差异已在测试中固化。
 * - **严格校验**（比 Go 默认更严，与 Go 的 `Encoding.Strict()` 对齐的部分另行标注）：
 *   1. 只接受字母表字符与填充符 `=`，**不接受** `\r`/`\n`/空格（Go 默认会忽略
 *      `\r` 和 `\n`；本模块按需求选择严格模式）；
 *   2. 带填充编码的输入长度必须是 4 的倍数；
 *   3. `=` 只能出现在**最后一个** quantum 的第 3、4 位，且形如 `xx==` / `xxx=`；
 *      数据后继续出现 `=` 或填充后还有数据都报错；
 *   4. 无填充编码（Raw）禁止出现 `=`，且长度 `% 4 == 1` 非法。
 * - **未校验尾部填充位**：`"Zh=="` 会解出 `'f'`（Go 默认 `Decode` 同样接受，
 *   `Strict()` 才会拒绝）。这是有意的互操作取舍，见 `Decode()` 注释。
 * - 错误用 `ErrorPtr` + 身份锚点 `Base64CorruptInput()` 表达，文本形如
 *   `illegal base64 data at input byte 3`（对齐 Go `CorruptInputError`）。
 *
 * ## 示例
 * @code
 * using go2cpp::encoding::RawURLEncoding;
 * std::string out = RawURLEncoding.EncodeToString("hello");  // "aGVsbG8"
 * auto decoded = RawURLEncoding.DecodeString("aGVsbG8");     // decoded.data == "hello"
 * @endcode
 */

#pragma once

#include "go2cpp/tools/error.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace go2cpp::encoding {

/**
 * @brief 对应 Go `base64.CorruptInputError` 的进程级身份锚点。
 * @note 故意泄漏（`new` 后不析构）：避免静态析构顺序让退出阶段的身份比较失效。
 */
inline ErrorPtr Base64CorruptInput() {
    static const ErrorPtr* const kError =
        new ErrorPtr(NewError("illegal base64 data"));
    return *kError;
}

/// `DecodeString` 的返回值：对应 Go 的 `([]byte, error)`。
struct Base64DecodeResult {
    /// 解码结果；`error` 非空时为空串。
    std::string data;
    /// 成功为 `nullptr`；失败为包装了 `Base64CorruptInput()` 的错误。
    ErrorPtr error;

    /// 便于 `if (auto r = ...DecodeString(s))` 直接判断成功。
    explicit operator bool() const noexcept { return error == nullptr; }
};

/**
 * @brief 一种 base64 编码（字母表 + 填充字符），对应 Go 的 `*base64.Encoding`。
 *
 * @details 实例是不可变的：字母表在构造时固定，之后所有操作都是 `const`。
 *          四个预定义实例由本头文件以 `inline constexpr` 提供。
 */
class Base64Encoding {
 public:
    /**
     * @brief 用 64 字符字母表与填充字符构造编码。
     * @param alphabet 必须恰好 64 个互不相同的字符（不做运行期校验）。
     * @param pad 填充字符；传 `'\0'` 表示无填充（Raw 编码）。
     * @warning 若 `alphabet` 指向临时字符串，必须保证其生命周期长于本对象。
     */
    constexpr Base64Encoding(std::string_view alphabet, char pad) noexcept
        : m_alphabet(alphabet), m_pad(pad) {}

    /// 对应 Go `EncodedLen`：`src_len` 字节编码后的长度（含填充）。
    std::size_t EncodedLen(std::size_t src_len) const noexcept;

    /// 对应 Go `DecodedLen`：长度为 `src_len` 的合法输入解码后的长度上界。
    std::size_t DecodedLen(std::size_t src_len) const noexcept;

    /**
     * @brief 追加编码结果到 `dst`。
     * @return 追加的字节数（等于 `EncodedLen(src.size())`）。
     */
    std::size_t Encode(std::string& dst, std::string_view src) const;

    /// 对应 Go `EncodeToString`：返回编码后的新字符串。
    std::string EncodeToString(std::string_view src) const;

    /**
     * @brief 严格解码 `src` 并追加到 `dst`。
     * @return 成功返回 `nullptr`；失败返回包装了 `Base64CorruptInput()` 的错误，
     *         且 `dst` 保持不变。
     *
     * @note 不校验尾部填充位是否为零（`"Zh=="` → `"f"`）。需要该检查时调用方
     *       可在解码后重新编码比对；Go 用 `Strict()` 开关，本模块按需求只保留
     *       严格形态，但把「尾部比特」排除在严格条件之外以保证与常见编码器互操作。
     */
    ErrorPtr Decode(std::string& dst, std::string_view src) const;

    /// 对应 Go `DecodeString`：返回 `(data, error)`。
    Base64DecodeResult DecodeString(std::string_view src) const;

    /// 是否为无填充（Raw）编码。
    bool IsRaw() const noexcept { return m_pad == '\0'; }

    /// 64 字符字母表。
    std::string_view Alphabet() const noexcept { return m_alphabet; }

    /// 填充字符；无填充时为 `'\0'`。
    char Padding() const noexcept { return m_pad; }

 private:
    std::string_view m_alphabet;
    char m_pad;
};

/// Go 包名 `encoding/base64` 的类型别名；四种预定义实例见下方常量。
using Base64 = Base64Encoding;

namespace detail {

inline constexpr std::string_view kStdBase64Alphabet =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
inline constexpr std::string_view kURLBase64Alphabet =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

}  // namespace detail

/// 对应 Go `base64.StdEncoding`：标准字母表 + `=` 填充。
inline constexpr Base64Encoding StdEncoding{detail::kStdBase64Alphabet, '='};
/// 对应 Go `base64.URLEncoding`：URL 安全字母表 + `=` 填充。
inline constexpr Base64Encoding URLEncoding{detail::kURLBase64Alphabet, '='};
/// 对应 Go `base64.RawStdEncoding`：标准字母表、无填充。
inline constexpr Base64Encoding RawStdEncoding{detail::kStdBase64Alphabet, '\0'};
/// 对应 Go `base64.RawURLEncoding`：URL 安全字母表、无填充。
inline constexpr Base64Encoding RawURLEncoding{detail::kURLBase64Alphabet, '\0'};

}  // namespace go2cpp::encoding
