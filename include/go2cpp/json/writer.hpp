#pragma once

/**
 * @file writer.hpp
 * @brief 把 `Document` / `Value` 序列化回 JSON 文本：`bgo2cpp::json::Write`。
 *
 * ## 定位
 * 解析器是「文本 → 值模型」的单向入口；本模块补上反方向，使库第一次具备
 * **读写闭环**：`Parse(Write(Parse(text)))` 与 `Parse(text)` 得到同样的值模型。
 * 没有它，库只能消费 JSON，不能生成配置文件、不能回包、不能做 golden 测试。
 *
 * ## 输出约定
 * - **对象 key 保持插入序**（不排序）：解析顺序就是用户写的顺序，重排会让
 *   diff、调试输出与原始文件不可对照；
 * - **数字**：整数按 `int64` 原样输出；浮点用 `std::to_chars` 的
 *   `chars_format::general`（**最短往返**，`double → 文本 → double` 位级一致）；
 *   `Decimal`（超 `int64` 的整数）走 `Decimal::ToString()` 保留全部十进制位；
 * - **浮点类型保持**：最短往返可能把 `1.0` 写成 `1`，重新解析会变成整数节点。
 *   因此当输出不含 `.` / `e` / `E` 时补 `.0`，保证节点类型也往返一致；
 * - **字符串**：`"` `\` 与全部 `< 0x20` 控制字符必须转义（`\b \f \n \r \t`
 *   用短形式，其余 `\u00XX`）；非 ASCII **默认原样输出 UTF-8 字节**
 *   （可读性最好、体积最小），`WriterOptions::escape_non_ascii = true` 时
 *   改写成 `\uXXXX`（补充平面输出代理对，大写 hex）；
 * - **pretty**：`WriterOptions::indent > 0` 时按层缩进、每元素一行；空容器
 *   始终紧凑输出 `{}` / `[]`。
 *
 * ## 失败口径
 * 只有两种失败：`to_chars` 无法表示（缓冲不足，实际不会发生）与
 * **非有限浮点**（`NaN` / `Inf`，JSON 不允许，输出 `null` 并返回 false）。
 * 字符串里的非法 UTF-8 字节**不**导致失败：默认原样输出，转义模式按字节
 * 逐个保留（不让 writer 成为数据丢失点）。
 */

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "go2cpp/json/document.hpp"
#include "go2cpp/json/value.hpp"

namespace go2cpp::json {

/// @brief 序列化选项；默认紧凑单行 + 原样 UTF-8 + 插入序。
struct WriterOptions {
  /// @brief 每层缩进字节数；`0` = 紧凑单行，`>0` = pretty（常用 2 或 4）。
  int indent = 0;

  /// @brief true 时把非 ASCII 码点写成 `\uXXXX`；false（默认）原样输出 UTF-8。
  bool escape_non_ascii = false;
};

/**
 * @brief 把整个文档序列化成 JSON 文本。
 *
 * @param doc [in] 解析结果；借用，调用期间有效（`Root()` 为 null 时输出 `null`）。
 * @param options [in] 序列化选项；见 `WriterOptions`。
 * @return JSON 文本；**不会**为空串（最小输出是 `null`）。失败时返回已写部分
 *         （只有非有限浮点会触发，此时该值写成 `null`）。
 *
 * @note 复杂度 O(节点数 + 输出字节数)；内部用显式栈而非递归，10 万层嵌套也
 *       不会爆栈（与解析器一致）。
 */
std::string Write(const Document& doc, const WriterOptions& options = WriterOptions{});

/**
 * @brief 把任意 `Value` 子树序列化并写入调用方缓冲。
 *
 * @param value [in] 值句柄；借用，必须属于仍存活的 `Document`。默认构造的
 *        null 句柄合法，输出 `null`。
 * @param out [in,out] 目标缓冲；**入口先 clear()**，成功时是完整 JSON。
 *        失败时内容未定义（可能已写入部分字节），调用方不应使用。
 * @param options [in] 序列化选项。
 * @return 成功 true；遇到 `NaN` / `Inf` 返回 false（该值写成 `null` 后继续，
 *         但整体结果不可用于反序列化——调用方应视为失败）。
 *
 * ## 并发/失败语义
 * - 纯函数：只读 `value` 与 `options`，写 `out`；不同线程各用自己的缓冲即可并发；
 * - 不抛异常（除 `std::bad_alloc`）；失败时 `out` 可能已部分写入；
 * - 不递归，栈深度由 `std::vector` 承载，与 JSON 嵌套深度线性相关。
 */
bool Write(const Value& value, std::string* out,
           const WriterOptions& options = WriterOptions{});

}  // namespace go2cpp::json
