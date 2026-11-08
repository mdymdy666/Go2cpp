#pragma once

/**
 * @file value.hpp
 * @brief JSON 值句柄：`bgo2cpp::json::Value`（文档指针 + 节点索引，16 B）。
 *
 * ## 值模型（P6b：Document + 扁平池）
 * 解析结果不再由「每个成员各持一个容器」的递归 `Value` 表示，而是全部写进
 * `bgo2cpp::json::Document` 的四个扁平池（节点 / 成员 / 数组索引 / 字符串 /
 * Decimal）。`Value` 本身退化成 **16 B 的句柄**：
 *
 * @code
 * class Value {
 *   const Document* m_doc;    // 8 B：所属文档
 *   std::uint32_t m_index;    // 4 B：节点池下标（+4 B 对齐填充）
 * };
 * @endcode
 *
 * 所有类型判定与取值都经 `m_doc` 的池完成（实现见 `document.hpp`）：
 * - 节点池：`std::vector<detail::Node>`，每个节点 1 B 标签 + payload；
 * - 成员池：对象字段按插入顺序连续存放（`{key, value_index}`）；
 * - 数组索引池：数组元素头节点的下标序列（4 B/元素）；
 * - 字符串池 + Decimal 池：转义解码结果与超出 int64/double 的数值。
 *
 * ## 为什么这样更快
 * - 解析一次默认**零堆分配**（五个池共用 ≤4 KiB 内联 arena，超出才 `new[]`）；
 * - 节点写入是「定长槽 push_back」，没有 variant 的类型切换与迭代析构；
 * - `Document` 析构一次性释放池，**没有递归、没有迭代工作栈**——
 *   10 万层嵌套的解析与析构都不会爆栈。
 *
 * ## 生命周期契约
 * `Value` 只是句柄：**`Document` 必须比它产出的所有 `Value` 活得久**。
 * - `Document` 被禁用拷贝/移动（移动会让句柄里的文档指针悬垂）；
 * - `Document::Clear()` 或重新 `Parse` 之后，旧句柄失效；
 * - 输入缓冲仍需比 `Document` 活得久：无转义字符串借用输入，不拷贝。
 *
 * ## 只读
 * 解析产物不可原地修改；需要构造 JSON 值时用解析器（或后续版本提供的
 * builder API）。本版本不提供 `Value(Array)` / `Value(Object)` 等独立构造器，
 * 因为任何节点都必须落在某个 `Document` 的池里。
 *
 * 实现（访问器与 `As<T>`）定义在 `document.hpp`，本文件末尾会自动包含它，
 * 因此 `#include "go2cpp/json/value.hpp"` 即可拿到完整类型。
 */

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

#include "go2cpp/json/core/conversion.hpp"
#include "go2cpp/json/core/decimal.hpp"
#include "go2cpp/json/core/text.hpp"
#include "go2cpp/tools/error.hpp"
#include "go2cpp/reflect/type.hpp"

namespace go2cpp::json {

class Document;
struct Member;

namespace detail {

/// @brief 值节点（定义在 `document.hpp`）。
struct Node;

}  // namespace detail

/**
 * @brief 一个 JSON 值的 16 B 句柄。
 *
 * @details 默认构造为 `null`（不绑定任何文档）。所有 `Is*` 与访问器都是 const、
 *          不抛异常；`As<T>()` 失败返回 false 且不修改输出参数。越界 / 类型
 *          不符的访问返回 `NullValue()`（一个静态 null 句柄）的副本。
 */
class Value {
 public:
  /// @brief 构造 `null` 句柄（不绑定文档）。
  Value() noexcept = default;

  /// @brief 是否为 `null`（未绑定文档或节点标签为 null）。
  bool IsNull() const noexcept;
  /// @brief 是否为布尔。
  bool IsBool() const noexcept;
  /// @brief 是否为整数（`std::int64_t` 节点）。
  bool IsInt() const noexcept;
  /// @brief 是否为 double。
  bool IsDouble() const noexcept;
  /// @brief 是否为 `Decimal`（池化节点）。
  bool IsDecimal() const noexcept;
  /// @brief 是否为数值（int64 / double / Decimal 任一）。
  bool IsNumber() const noexcept;
  /// @brief 是否为字符串。
  bool IsString() const noexcept;
  /// @brief 是否为数组。
  bool IsArray() const noexcept;
  /// @brief 是否为对象。
  bool IsObject() const noexcept;

  /**
   * @brief 返回元素个数。
   * @return 数组/对象的元素数；标量、字符串与 null 返回 0。
   */
  std::size_t Size() const noexcept;

  /**
   * @brief 按下标取元素（数组按位置；对象按插入顺序的第 N 个成员值）。
   * @param index [in] 0 起下标。
   * @return 有效下标返回元素句柄；越界、null 或本值不是容器时返回 null 句柄。
   * @note 返回值按值传递（16 B 句柄），不依赖任何临时容器。
   */
  Value At(std::size_t index) const;

  /**
   * @brief 按 key 查对象成员（线性查找；小对象比哈希表更省）。
   * @param key [in] 成员名；按字节精确比较。
   * @return 找到返回成员值句柄；否则返回 null 句柄。
   */
  Value Find(std::string_view key) const noexcept;

  /**
   * @brief `Find` 的引用风格版本。
   * @param key [in] 成员名。
   * @return 找到返回成员值；否则返回 null 句柄。
   */
  Value operator[](std::string_view key) const noexcept;

  /**
   * @brief 对象第 `index` 个成员的键。
   * @param index [in] 0 起成员下标（按插入顺序）。
   * @return 有效下标且本值是对象时返回键视图；否则返回空视图。
   */
  std::string_view MemberName(std::size_t index) const noexcept;

  /**
   * @brief 字符串内容视图。
   * @return 字符串节点的视图；无转义时指向输入缓冲，有转义时指向文档字符串池。
   *         非字符串返回空视图。
   */
  std::string_view AsText() const noexcept;

  /**
   * @brief 按类型读取：失败返回 false 且不修改 `out`。
   *
   * @tparam T 目标类型；支持 `nullptr_t`、`bool`、`std::string`、`std::string_view`、
   *           `core::Text`、`core::Decimal`、所有整型与浮点型，以及通过
   *           `go2cpp::json::core::conversion` 注册的自定义类型。
   * @param out [out] 成功时写入；失败时保持原值。
   * @return 成功 true。
   *
   * @details 取值规则：
   * - 字符串来源的内置类型走 `go2cpp/json/core/conversion.hpp` 的 `StringTo*`；
   * - 数字来源直接按数值转换（整型做范围检查，double→整型要求无小数部分）；
   * - 自定义类型走 `core::Convert(TypeIdOf<T>())` 注册表。
   */
  template <typename T>
  bool As(T& out) const;

  /**
   * @brief `As(T&)` 的便捷取回版本。
   * @tparam T 目标类型。
   * @return 成功时返回值；失败时返回 `T{}`（调用方无法区分失败，需要区分请用
   *         `As(T&)`）。
   */
  template <typename T>
  T As() const;

  /// @brief 返回所属文档；null 句柄返回 nullptr。
  const Document* Owner() const noexcept { return m_doc; }

  /// @brief 返回节点池下标；null 句柄返回 0（无意义）。
  std::uint32_t Index() const noexcept { return m_index; }

 private:
  friend class Document;

  /// @brief 由 `Document` 构造句柄；`doc` 不可为 nullptr（null 用默认构造）。
  Value(const Document* doc, std::uint32_t index) noexcept : m_doc(doc), m_index(index) {}

  /// @brief 节点指针；null 句柄或索引失效时返回 nullptr。
  const detail::Node* NodePtr() const noexcept;

  const Document* m_doc = nullptr;  ///< 所属文档；null 表示空句柄。
  std::uint32_t m_index = 0;        ///< 节点池下标。
};

/// @brief 全局静态 `null` 句柄，供越界/查不到时返回引用。
inline const Value& NullValue() {
  static const Value value;
  return value;
}

}  // namespace go2cpp::json

// 实现需要 Document 的完整定义；放在末尾让 `#include "value.hpp"` 也能拿到全部。
#include "go2cpp/json/document.hpp"
