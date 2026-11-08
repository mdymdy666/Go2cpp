#pragma once

/**
 * @file pointer.hpp
 * @brief JSON Pointer（RFC 6901）查询：`bgo2cpp::json::Find`。
 *
 * ## 语法
 * ```
 * ""            整个文档（根值）
 * "/foo"        对象成员 foo
 * "/foo/0/bar"  数组 foo 的第 0 个元素的成员 bar
 * "/a~1b"       成员名 "a/b"（~1 → /）
 * "/m~0n"       成员名 "m~n"（~0 → ~）
 * ```
 *
 * 转义顺序：解析时从左到右扫描，`~1` 先于 `~0` 识别（RFC 6901 §4 要求先
 * 替换 `~1` 再替换 `~0`，否则 `~01` 会被误解成 `/`）。本实现逐段扫描，
 * 遇到 `~0` 输出 `~`、`~1` 输出 `/`，因此 `~01` 输出 `~1`，与规范一致。
 *
 * ## 数组下标
 * 只接受 RFC 6901 的 `array-index`：`0` 或 `[1-9][0-9]*`。
 * - 前导零（`/01`）、负号（`/-1`）、空段（`//`）都判为非法；
 * - 下标超出元素数判为 `kIndexOutOfRange`。
 * （`-` 是 JSON Patch 的「追加」语义，不在 Pointer 规范里，这里不支持。）
 *
 * ## 失败
 * 返回 false 并写 `PointerError`，其中 `offset` 指向**指针字符串**里出错字节
 * 的下标。`Find` 的指针版（返回 `const Value*`）失败时返回 `nullptr`。
 */

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "go2cpp/json/document.hpp"
#include "go2cpp/json/value.hpp"

namespace go2cpp::json {

/// @brief JSON Pointer 求值失败的原因。
enum class PointerFailure : std::uint8_t {
  kNone = 0,               ///< 无错误。
  kMissingLeadingSlash,    ///< 非空指针不以 `/` 开头。
  kInvalidEscape,          ///< `~` 后面不是 `0` 或 `1`。
  kInvalidArrayIndex,      ///< 数组段不是合法下标（空串 / 前导零 / 非数字）。
  kIndexOutOfRange,        ///< 数组下标超出元素数。
  kMemberNotFound,         ///< 对象里没有该 key。
  kNotAContainer,          ///< 路径还没走完，但当前值是标量（无法继续下钻）。
};

/// @brief 失败详情：原因 + 指针字符串中的出错字节下标。
struct PointerError {
  PointerFailure failure = PointerFailure::kNone;  ///< 失败原因。
  std::size_t offset = 0;                          ///< 出错字节在 pointer 中的下标。
};

/**
 * @brief 按 JSON Pointer 在文档里查找值。
 *
 * @param doc [in] 文档；借用，调用期间有效。
 * @param pointer [in] 指针字符串（UTF-8 字节；key 按字节精确比较）。
 *        空字符串表示根值。
 * @param out [out] 成功时写入找到的值句柄；失败时**保持原值**。不可为 nullptr。
 * @param error [out] 失败时写入原因与出错下标；成功时写 `kNone`。可为 nullptr
 *        （不需要诊断时）。
 * @return 找到 true；语法错误或路径不存在 false。
 *
 * ## 定位
 * 这是按路径读取配置的**唯一入口**（替代一串手写 `Find("a").Find("b")`）。
 * 失败必须区分「语法坏」与「路径不存在」：前者是调用方的 bug，后者是数据
 * 缺字段，处理策略不同（前者应拒绝请求，后者可回退默认值）。
 *
 * ## 步骤
 * step1. 空指针 → 返回根值。
 * step2. 校验首字符 `/`，否则 `kMissingLeadingSlash`。
 * step3. 按 `/` 切段；逐段解码 `~0`/`~1`（非法转义立即失败）。
 * step4. 当前值是对象 → 按 key 线性查找（保持插入序，语义与 `Value::Find` 一致）；
 *        数组 → 解析十进制下标并做范围检查；标量 → `kNotAContainer`。
 * step5. 全段走完 → 返回当前句柄。
 *
 * ## 并发/失败语义
 * - 纯读，无共享可变状态；可被多线程同时调用；
 * - 不分配（段缓冲复用 `std::string`，实际只在超长段时分配）；
 * - 失败时 `out` 保持原值，调用方可以安全地继续用旧句柄。
 */
bool Find(const Document& doc, std::string_view pointer, Value* out,
          PointerError* error = nullptr);

/// @brief `Find` 的便捷重载：从任意子树出发求值。
/// @param root [in] 起始值；借用，必须属于仍存活的 `Document`。
/// @param pointer [in] 指针字符串（相对 `root`）。
/// @param out [out] 成功时写入；失败时保持原值。
/// @param error [out] 失败详情；可为 nullptr。
/// @return 找到 true。
bool Find(const Value& root, std::string_view pointer, Value* out,
          PointerError* error = nullptr);

/**
 * @brief 返回指向查询结果的指针（任务约定的便捷签名）。
 *
 * @param doc [in] 文档；借用，调用期间有效。
 * @param pointer [in] 指针字符串。
 * @return 找到时返回非空指针；失败返回 `nullptr`。
 *
 * @warning 返回值指向**线程局部缓存**：下一次同线程调用会覆盖它。需要跨调用
 *          保存结果时请用 `Find(doc, pointer, Value* out, ...)` 把句柄拷走。
 *          不要在多线程之间传递这个指针。
 */
const Value* Find(const Document& doc, std::string_view pointer);

/**
 * @brief 返回失败原因的英文短语（日志/诊断用）。
 * @param failure [in] 失败原因。
 * @return 静态存储的短语；调用方不得释放、不得改写。
 */
const char* PointerFailureName(PointerFailure failure) noexcept;

}  // namespace go2cpp::json
