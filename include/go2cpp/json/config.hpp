#pragma once

/**
 * @file config.hpp
 * @brief JSON 解析架构的「编译期 Policy + 运行期 Config」两轴配置。
 *
 * ## 两轴正交
 * - **Policy**（编译期）：决定嵌套栈用哪种容器。`InlinePolicy` 小深度内联、
 *   零堆分配；`StdPolicy` 全 `std::vector`，行为最容易对齐旧代码；
 *   `CompactPolicy` 更大的内联容量；`AutoPolicy` 按元素大小自动算内联容量。
 * - **Config**（运行期）：决定行为开关（最大深度、是否允许尾逗号），与容器
 *   选型无关。同一份 `ParserConfig` 可以喂给任意 Policy 的解析器。
 *
 * ## 为什么 Value 不跟 Policy 走
 * `Value` 是递归类型（`std::vector<Value>` 内含 `Value`），定义期 `sizeof(Value)`
 * 未完成，`AutoPolicy` 的 `AutoInlineN<T>` 无法实例化；因此值模型固定用
 * `std::vector`，Policy 只作用于解析器的**帧栈**（见 `parser.hpp`）。
 *
 * @note `AutoPolicy` 只能用于非递归类型（如未来的 `RowView`）；对 `Value`
 *       这类递归类型使用会触发不完整类型错误。
 */

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "go2cpp/json/kstd/k_inline_stack.hpp"

namespace go2cpp::json {

/**
 * @brief 对象内出现重复 key 时的处理策略。
 *
 * @details RFC 8259 不要求成员名唯一，但业务配置/API 响应通常假定唯一，
 *          不同上游库的默认行为也不一致（rapidjson 保留全部、首个生效；
 *          多数 JSON 实现（如 Python json）后者覆盖前者）。因此把选择权交给调用方：
 *
 *          - `kFirst`（默认）：**首个生效**。重复成员全部保留在成员表中
 *            （与历史行为逐字节一致），`Find` / `operator[]` 返回第一个匹配。
 *            该策略在解析期**不做任何检测**，因此没有性能代价；
 *          - `kLast`：**最后生效**。同名 key 只保留一项，位置为首次出现处，
 *            值为最后一次出现的值；`Size()` = 不同 key 的个数；
 *          - `kError`：同名 key 第二次出现即失败（`JsonFailure::kDuplicateKey`），
 *            错误位置指向**重复出现的那个 key 的起始引号**。
 *
 * @note `kLast` / `kError` 需要在同一容器的已收集成员里线性查重（O(成员数)），
 *       只在显式配置时启用；默认 `kFirst` 保持零检测开销。三者都按「解码后
 *       的 key 字节」比较，因此 `"\u0061"` 与 `"a"` 视为同一个 key。
 */
enum class DuplicateKeyPolicy : std::uint8_t {
  kFirst,  ///< 保留首个（默认）：首个值生效，重复成员全部保留。
  kLast,   ///< 保留最后：同 key 只留一项，值取最后一次，位置取首次出现。
  kError,  ///< 报错：重复 key 返回 `JsonFailure::kDuplicateKey`。
};

/**
 * @brief 解析错误处理策略（场景 5「错误恢复」）。
 *
 * @details 三档语义由「能否从坏值恢复」决定，互相**不重叠**：
 *
 *          - `kAbort`（默认）：**首个错误即停**。`Parse` 返回 false、
 *            `Document` 被清空、`error` 是首个错误。这是历史行为，
 *            实现上**不进入任何恢复代码路径**，逐字节保持不变；
 *          - `kSkipValue`：坏值位置填 `null` 节点后继续解析同级的后续成员/元素
 *            （`{"a":@,"b":2}` → `{"a":null,"b":2}`）。`Document::Errors()`
 *            只保留**首个**错误（`max_errors == 0` 时一条也不保留），其余同样
 *            跳过但不再记录——调用方要「错误清单完整」请用 `kCollect`；
 *          - `kCollect`：与 `kSkipValue` 同一条恢复路径，区别是把**所有**错误
 *            都收进 `Document::Errors()`，直到 `max_errors` 上限；超过上限后
 *            仍继续恢复，只是不再记录新错误。
 *
 * @note 恢复只覆盖「容器内某个值坏了」这一类错误；结构错误（缺冒号、缺逗号、
 *       键不是字符串、根值本身坏）、`kTimeout` / `kTooLarge` / `kDepthExceeded` /
 *       `kDuplicateKey` / `kTrailingGarbage` 一律按 `kAbort` 处理（返回 false）。
 *       详见 `BasicParser::RecoverValue` 的「已知限制」。
 */
enum class ErrorPolicy : std::uint8_t {
  kAbort,      ///< 默认：首个错误即停，失败时清空文档。
  kSkipValue,  ///< 跳过坏值（填 null）继续；`Errors()` 只保留首个错误。
  kCollect,    ///< 跳过坏值（填 null）继续；`Errors()` 保留全部错误（至多 max_errors）。
};

/**
 * @brief 运行期解析行为配置；与 Policy 正交。
 *
 * @note `max_depth == 0` 表示「按输入长度自动」：上限取
 *       `min(input.size() / 2 + 1, 65536)`，见 `docs/MIGRATION_DESIGN.md` §6.2.1。
 */
struct ParserConfig {
  /// @brief 允许的最大容器嵌套深度（`stack.size()` 达到该值即报错）；0 = 自动。
  std::size_t max_depth = 1024;
  /// @brief 是否允许对象/数组的尾逗号（`{"a":1,}` / `[1,]`）。
  bool allow_trailing_comma = false;
  /// @brief 解析时间预算（毫秒）；0 = 不限。超时返回 `JsonFailure::kTimeout`。
  std::uint32_t timeout_ms = 0;
  /// @brief 是否跳过 UTF-8 BOM（`EF BB BF`）；默认 false（严格）。
  bool allow_bom = false;
  /// @brief 输入字节上限；0 = 不限。超出返回 `JsonFailure::kTooLarge`（GB 级防护）。
  std::size_t max_input_bytes = 0;
  /**
   * @brief 单个字符串 / 数字 token 的内容字节上限；0 = 不限。
   *
   * @details 口径：
   *          - **字符串**：引号之间的**原始字节数**（含转义文本、不含两侧引号），
   *            对象 key 与字符串值同样受限；
   *          - **数字**：字面量字节数（含 `-`、小数部分与指数，无引号）。
   *          超出返回 `JsonFailure::kStringTooLarge`，位置指向该 token 的起点
   *          （字符串为开引号，数字为首字符）。检查发生在 token 解析完成之后，
   *          因此 `Document` 里可能已写入该字符串（失败后由 `Parse` 统一清空）。
   *
   * @note 默认 0 时**不做任何长度比较**：解析器在入口按
   *       「`max != 0 || validate_utf8`」运行期分叉一次，默认配置走编译期
   *       `kChecks == false` 的实例，检查调用点被 `if constexpr` 整体消除，
   *       生成的指令与未加该功能时一致（547 B 小文档实测零回退）。
   */
  std::size_t max_string_bytes = 0;
  /**
   * @brief 是否严格校验字符串（含对象 key）内的 UTF-8 序列；默认 false = 原样透传。
   *
   * @details 开启后，字符串内容里的首字节 / 续字节必须构成合法 UTF-8：
   *          `00-7F`、`C2-DF`+1 续字节、`E0-EF`+2、`F0-F4`+3；拒绝孤立续字节、
   *          超长编码（`C0/C1`、`E0 80-9F`、`F0 80-8F`）、UTF-16 代理区
   *          （`ED A0-BF`）与 `> U+10FFFF`（`F4 90-BF`、`F5-FF`）。非法返回
   *          `JsonFailure::kInvalidUtf8`，位置指向非法序列的**首字节**。
   *
   * @note 只校验原始输入字节：`\uXXXX` 转义由解析器解码，解码结果天然合法；
   *       字符串内的 BOM（`EF BB BF`）按普通 `U+FEFF` 处理（合法三字节序列）。
   */
  bool validate_utf8 = false;
  /// @brief 重复 key 处理策略；默认 `kFirst`（首个生效，与历史行为一致）。
  DuplicateKeyPolicy duplicate_key = DuplicateKeyPolicy::kFirst;
  /// @brief 错误处理策略；默认 `kAbort`（首个错误即停，与历史行为一致）。
  ErrorPolicy error_policy = ErrorPolicy::kAbort;
  /// @brief 错误恢复最多收集多少条错误；默认 16，`0` = 恢复但一条也不记录。
  /// @note 只在 `error_policy != kAbort` 时生效；超出上限后仍继续恢复，只是不再记录。
  /// @note 用 `uint32_t`（而不是 `size_t`）是为了让 `ParserConfig` 的尺寸与改动前
  ///       保持一致（48 B）：默认路径每次 `Parse` 都要在调用点构造这个结构，尺寸不变
  ///       就不会给热路径增加额外写入。
  std::uint32_t max_errors = 16;
};

/// @brief 策略 A：小对象优先（默认）。帧栈内联 8 个，小 JSON 零堆分配。
struct InlinePolicy {
  template <typename T>
  using Stack = go2cpp::KInlineStack<T, 8>;
};

/// @brief 策略 B：保守。帧栈用 `std::vector`，行为与旧代码最容易对齐。
struct StdPolicy {
  template <typename T>
  using Stack = std::vector<T>;
};

/// @brief 策略 C：大容量优先。帧栈内联 16 个，适合深嵌套但单次解析内存敏感的场景。
struct CompactPolicy {
  template <typename T>
  using Stack = go2cpp::KInlineStack<T, 16>;
};

/// @brief 目标内联字节数 320 B 反推的容量；上限 16 防止对象膨胀。
template <typename T>
inline constexpr std::size_t AutoInlineN =
    std::clamp<std::size_t>(320 / sizeof(T), 1, 16);

/// @brief 策略 D：按元素大小自动选内联容量（仅限非递归类型）。
struct AutoPolicy {
  template <typename T>
  using Stack = go2cpp::KInlineStack<T, AutoInlineN<T>>;
};

}  // namespace go2cpp::json
