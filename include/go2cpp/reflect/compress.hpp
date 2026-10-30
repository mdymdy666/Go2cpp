#pragma once

/**
 * @file compress.hpp
 * @brief 轻量 RLE 压缩：不引入第三方库的字节级行程编码。
 *
 * @details 为什么不直接用项目里的 `go2cpp::compress`（DEFLATE）：Reflect 是
 *          独立模块，网络消息通常只有几十到几百字节，DEFLATE 的头开销和压缩
 *          表构建成本在这个量级反而可能得不偿失；而 varint + 字段名去重已经
 *          把结构体消息压掉了大部分冗余。RLE 作为最后一道可选压缩，对
 *          「大量重复字节」的载荷（长文本、填充、重复标记）效果明确，代码量
 *          和可验证性也远好于一个完整 deflate 封装。
 *
 *          压缩流格式（`RleCompress` 的输出）：
 *
 *          | 字段        | 编码                                        |
 *          |-------------|---------------------------------------------|
 *          | 原始长度    | varint（LEB128）                             |
 *          | 段...       | 见下                                        |
 *
 *          每个段首字节 `ctrl`：
 *          - `ctrl & 0x80 == 0`：字面量段，长度 `(ctrl & 0x7f) + 1`（1..128），
 *            后跟对应字节；
 *          - `ctrl & 0x80 != 0`：重复段，长度同样是 `(ctrl & 0x7f) + 1`，
 *            后跟 1 字节重复值。
 *
 *          编码器只对长度 >= 3 的连续相同字节使用重复段；短游程进入字面量段。
 *          解码器会校验「解压结果恰好等于原始长度」，任何越界、截断或尾随垃圾
 *          都判为 `false` 并清空输出，避免把损坏数据交给字段解析器。
 *
 * @author go2cpp contributors
 * @date 2026-10-30
 * @copyright MIT License
 */

#include <cstddef>
#include <cstdint>
#include <vector>

#include "go2cpp/reflect/buffer.hpp"

namespace go2cpp::reflect {

/// @brief 使用重复段的最短游程。
inline constexpr std::size_t kMinRleRun = 3;

/// @brief 单段最大长度（受 ctrl 的 7 位计数限制）。
inline constexpr std::size_t kMaxRleRun = 128;

/**
 * @brief 压缩一段字节。
 * @param data 输入；`size == 0` 时可为 nullptr。
 * @param size 输入长度。
 * @param out 输出（含原始长度前缀）；失败时内容未定义。
 * @return 是否成功（输入过大/内存不足时失败）。
 *
 * @note 不保证一定变小：调用方应比较 `out.size()` 与输入长度，只有更小才
 *       采用压缩并设置压缩标志。
 */
bool RleCompress(const std::uint8_t* data, std::size_t size, std::vector<std::uint8_t>& out);

/**
 * @brief 解压 RLE 流。
 * @param data 压缩流。
 * @param size 压缩流长度。
 * @param out 解压结果；失败时清空。
 * @param max_output 允许的最大解压长度（防压缩炸弹）。
 * @return 是否成功；失败时 `out` 为空。
 */
bool RleDecompress(const std::uint8_t* data, std::size_t size, std::vector<std::uint8_t>& out,
                   std::size_t max_output = kMaxBufferBytes);

}  // namespace go2cpp::reflect
