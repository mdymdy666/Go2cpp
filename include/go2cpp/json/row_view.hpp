#pragma once

/**
 * @file row_view.hpp
 * @brief 一行字段的零拷贝视图：≤N 个字段零堆分配。
 *
 * @details
 * 与 week1 的 `Row`（`vector<wstring>`，append 即拷贝）不同：RowView 只存
 * `std::string_view`，字段指向调用方的输入缓冲，不复制任何字节。
 * 生命周期契约：所有视图在源缓冲存活期内有效。
 */

#include <cstddef>
#include <stdexcept>
#include <string_view>

#include "go2cpp/json/kstd/k_inline_vector.hpp"

namespace go2cpp::json {

/**
 * @brief 一行字段的只读视图；内联容量 N。
 * @tparam N 内联字段数；≤N 时零堆分配。
 */
template <std::size_t N = 8>
class RowView {
 public:
  /// @brief 追加一个字段视图（不拷贝内容）。
  void Append(std::string_view field) { m_fields.push_back(field); }

  /// @brief 字段数。
  std::size_t size() const noexcept { return m_fields.size(); }

  /// @brief 是否为空行。
  bool empty() const noexcept { return m_fields.empty(); }

  /// @brief 按下标取字段；越界未定义（与 vector 一致）。
  std::string_view operator[](std::size_t index) const noexcept { return m_fields[index]; }

  /// @brief 按下标取字段；越界抛 `std::out_of_range`。
  std::string_view At(std::size_t index) const {
    if (index >= m_fields.size()) throw std::out_of_range("RowView::At");
    return m_fields[index];
  }

  /// @brief 清空字段（不释放堆容量）。
  void Clear() noexcept { m_fields.clear(); }

  /// @brief 是否已切到堆。
  bool using_heap() const noexcept { return m_fields.using_heap(); }

  /// @brief 当前容量（内联模式为 N）。
  std::size_t capacity() const noexcept { return m_fields.capacity(); }

 private:
  KInlineVector<std::string_view, N> m_fields;
};

}  // namespace go2cpp::json
