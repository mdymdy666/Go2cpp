/**
 * @file huffman.hpp
 * @brief HPACK 用的静态 Huffman 编解码（RFC 7541 §5.2 + 附录 B）。
 *
 * ## 规格要点
 *
 * - 码表 257 项：字节 0x00..0xFF 共 256 项 + **EOS**（30 位全 1）；
 * - 编码按位拼接，末尾用 **EOS 的最高位（全 1）** 填充到字节边界，
 *   填充长度 **< 8 位**；
 * - 解码时若在非叶节点停下，剩余位必须是 **全 1 且不超过 7 位**，
 *   否则说明填充非法；
 * - **EOS 符号不允许出现在输入中**（出现即为协议错误）。
 *
 * ## 实现选择
 *
 * 解码用**二叉树逐位下降**，而非 8 位一级的查表。理由是正确性优先：
 * 树的语义与 RFC 的"前缀码"定义一一对应，便于对照标准审查；257 个符号的树
 * 只有约 512 个节点，构造成本一次性。若日后成为热点，可再换成多级查表。
 *
 * ## 模块定位
 * HPACK 的字符串压缩后端：`hpack.cpp` 在解析字符串字面量时，按首字节的
 * Huffman 位调用本模块解码；编码器在结果更短时调用它编码。与帧/连接层无
 * 直接依赖，是纯函数式组件。
 *
 * ## 本文件定义什么
 * - `HuffmanStatus`：解码结果码（填充非法 / 出现 EOS / 码表不一致）；
 * - `HuffmanEncode()` / `HuffmanDecode()`：257 项静态码表的编解码；
 * - `HuffmanEncodedBits()`：只算位数，便于测试断言填充长度。
 *
 * ## 阅读建议
 * 先看文件头的「规格要点」（填充必须是全 1 且 < 8 位、输入中禁止 EOS），
 * 再看 `huffman.cpp` 的 `DecodeTree()` 逐位下降，最后看尾部校验。
 *
 * ## 边界与陷阱
 * - 解码用二叉树逐位下降而不是查表：正确性优先，约 512 个节点、一次性构建；
 * - `HuffmanDecode()` 会**先清空** `out`；失败时输出可能已部分写入，调用方应丢弃；
 * - 编码当前不会失败（恒 `kOk`），返回值只为未来扩展保留。
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace go2cpp::net::http2 {

/** @brief Huffman 编解码状态。 */
enum class HuffmanStatus : std::uint8_t {
    kOk,
    /// 输入末尾的填充非法（不是全 1，或超过 7 位）。
    kInvalidPadding,
    /// 输入中出现 EOS 符号（RFC 禁止）。
    kEosInInput,
    /// 码表与输入不一致导致解码走入非法状态。
    kInvalidCode,
};

/** @brief 状态转字符串。 */
const char* ToString(HuffmanStatus status) noexcept;

/**
 * @brief Huffman 编码。
 * @param input 待编码字节。
 * @param out 追加写入的字节（不清空调用方已有内容）。
 * @return 状态。当前实现不会失败，保留返回值以便未来扩展。
 *
 * ## 步骤
 * step1. 逐字节取 `(code, bits)`，按位拼进累加器；
 * step2. 每满 8 位输出一个字节，只保留剩余的低位；
 * step3. 末尾不足 8 位时用 EOS 的最高位（全 1）填充到字节边界。
 */
HuffmanStatus HuffmanEncode(const std::string& input, std::vector<std::uint8_t>* out);

/**
 * @brief Huffman 解码。
 * @param in 输入字节。
 * @param length 字节数。
 * @param out 成功时写入解码结果（**先清空**）。
 * @return 状态。
 *
 * ## 步骤
 * step1. 逐位沿解码树下走，走到叶子就输出符号并回到根；
 * step2. 遇到 EOS 符号立即返回 `kEosInInput`（RFC 7541 §5.2 禁止）；
 * step3. 输入结束若停在内部节点，校验剩余位全 1 且 < 8 位，否则 `kInvalidPadding`。
 */
HuffmanStatus HuffmanDecode(const std::uint8_t* in, std::size_t length, std::string* out);

/** @brief 计算一段输入用 Huffman 编码后需要多少位（便于测试断言）。 */
std::size_t HuffmanEncodedBits(const std::string& input) noexcept;

}  // namespace go2cpp::net::http2
