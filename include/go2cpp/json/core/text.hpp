#pragma once

/**
 * @file text.hpp
 * @brief UTF-8 字节视图与「借用 / 池化拥有」双态字符串：`bgo2cpp::json::core::Text`。
 *
 * 迁移自 week1 `csvp/text.h` 的 `String` / `StringView`，但**不迁移编码层**：
 * week1 用 `wchar_t`（Windows 下 16 位）承载文本，因为 CSV 需要 GBK/Big5/
 * Shift-JIS 探测；week2 的 JSON 输入固定是 UTF-8 字节流，用 `std::string_view`
 * 直接切片即可，不需要宽字符中转。`Encoding` / `Decode` / `Encode` / `TextError`
 * 属于 CSV 的职责，留在 week1，本文件不提供。
 *
 * ## 为什么需要 Text
 * JSON 解析中绝大多数字符串是「无转义的一段字节」——它原样躺在输入缓冲里，
 * 复制成 `std::string` 纯属浪费。只有含 `\"` `\\` `\n` 等转义、或需要拼接时
 * 才必须自己持有存储。`Text` 用三个成员同时表达这两种情况：
 *
 * - **borrowed**：只存 `std::string_view`，指向调用方的输入缓冲，**零分配零拷贝**；
 * - **owned（池化）**：视图指向 `json::Document::strings_` 池，`m_owned_offset` /
 *   `m_owned_len` 记录它在池内的区间，供诊断与「是否拥有」判定。
 *
 * ## 大小（P6b S2 的关键约束）
 * `sizeof(Text) == 24`（view 16 + offset 4 + len 4），比改造前的 56 B 少 32 B。
 * `json::Document` 的节点池每个字符串值都要放一个 `Text`，这 32 B 直接决定
 * 节点大小（32 B vs 64 B）。**不再持有 `std::string`**：拥有态内容一律由
 * `Document` 的字符串池承载，避免每个转义字符串一次堆分配。
 *
 * ## 生命周期契约（易错点）
 * borrowed 的视图指向输入缓冲，**输入缓冲必须在 `Text` 及其副本的整个使用期内
 * 保持存活且不被修改**。owned 的视图指向 `Document::strings_`，池在解析期间
 * 不扩容，因此视图在 `Document` 存活期内稳定。
 *
 * @code
 * Text t = Text::Borrow(std::string("abc"));   // 错误：临时串立刻析构，t 悬垂
 * @endcode
 *
 * 需要结果独立存活时，用 `json::Document::AppendString` 把字节复制进文档池。
 */

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace go2cpp::json::core {

/// @brief UTF-8 字节视图；不保证是合法 UTF-8，解析层只把它当字节序列。
using StringView = std::string_view;

/**
 * @brief 借用 / 池化拥有双态字符串（24 B）。
 *
 * @details 两种模式互斥，由 `m_owned_len` 区分：`0` 表示借用态。转义解码结果
 *          至少 1 字节，因此池化拥有态的 `m_owned_len` 恒非零，判定无歧义。
 */
class Text {
 public:
  /// @brief 构造空的借用态文本。
  Text() = default;

  /**
   * @brief 构造借用态文本：不分配、不拷贝。
   * @param view [in] 指向外部缓冲的视图；缓冲区必须比返回的 `Text` 活得久。
   * @return 借用态 `Text`，`borrowed()==true`。
   */
  static Text Borrow(std::string_view view);

  /**
   * @brief 构造池化拥有态文本：内容在外部池里，本对象只持视图与池内区间。
   * @param view [in] 池内视图；池必须比本对象活得久，且视图有效期内不得扩容。
   * @param offset [in] 内容在池内的起始字节偏移。
   * @param length [in] 内容字节数；必须 > 0（借用态用 `Borrow`）。
   * @return 拥有态 `Text`，`borrowed()==false`。
   */
  static Text Owned(std::string_view view, std::uint32_t offset, std::uint32_t length);

  /**
   * @brief 返回统一的只读视图。
   * @return 借用态返回借用的视图；拥有态返回池内视图。
   * @note 视图在底层缓冲（输入或文档池）失效后失效；对外传递时按借用语义处理。
   */
  std::string_view view() const noexcept { return m_view; }

  /// @brief 是否处于借用态（不指向文档池）。
  bool borrowed() const noexcept { return m_owned_len == 0; }

  /// @brief 内容是否为空（两种模式语义一致）。
  bool empty() const noexcept { return m_view.empty(); }

  /// @brief 内容字节数（不含结尾 NUL，`std::string_view` 语义）。
  std::size_t size() const noexcept { return m_view.size(); }

  /// @brief 拥有态在池内的起始偏移；借用态返回 0。
  std::uint32_t owned_offset() const noexcept { return m_owned_offset; }

  /// @brief 拥有态在池内的字节数；借用态返回 0。
  std::uint32_t owned_len() const noexcept { return m_owned_len; }

 private:
  std::string_view m_view;             ///< 借用：输入缓冲；拥有：文档字符串池。
  std::uint32_t m_owned_offset = 0;    ///< 池内偏移；借用态为 0。
  std::uint32_t m_owned_len = 0;       ///< 池内长度；借用态为 0。
};

inline Text Text::Borrow(std::string_view view) {
  Text text;
  text.m_view = view;
  return text;
}

inline Text Text::Owned(std::string_view view, std::uint32_t offset, std::uint32_t length) {
  Text text;
  text.m_view = view;
  text.m_owned_offset = offset;
  text.m_owned_len = length;
  return text;
}

static_assert(sizeof(Text) == 24, "Text 必须是 24 B（view 16 + offset 4 + len 4）");

}  // namespace go2cpp::json::core
