#pragma once

/**
 * @file reader.hpp
 * @brief 规则驱动的字段校验与转换：预编译列计划，失败返回结构化错误。
 *
 * @details
 * 替代 week1 的 CSV `Validator`，去掉宽字符路径。设计保留「SetRules 时预编译
 * ColumnPlan、校验时无虚调用」的思路：
 * - `RowView` 只提供视图，读取层不拷贝字段；
 * - `kStringType` 列直接 `Text::Borrow`（零拷贝），其它类型走 `core::Convert`；
 * - 错误累积：尽量收集所有列错误，不在第一个错误处中断。
 */

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "go2cpp/json/core/conversion.hpp"
#include "go2cpp/tools/error.hpp"
#include "go2cpp/reflect/type.hpp"
#include "go2cpp/json/row_view.hpp"

namespace go2cpp::json {

/// @brief 一列的规则。
struct FieldRule {
  std::string name;                                    ///< 列名（用于错误信息）。
  reflect::TypeId type_id = reflect::kStringType;      ///< 目标类型。
  bool required = false;                               ///< 是否必填。
  std::string default_value;                           ///< 非空时：空字段用它补位。
};

/// @brief 一列的错误。
struct FieldError {
  std::size_t column_index = 0;  ///< 列下标；列数不匹配时为规则列数。
  std::string column_name;       ///< 列名（列数不匹配时为空）。
  std::string raw_value;         ///< 原始字段（列数不匹配时为空）。
  ErrorPtr cause;                ///< 结构化错误，恒非空。
};

/**
 * @brief 规则驱动的读取器：校验 + 转换，不拥有字段数据。
 * @note 拷贝被禁用：`ColumnPlan` 持有指向 `m_rules` 元素的引用语义。
 */
class Reader {
 public:
  Reader() = default;

  /// @brief 设置规则并预编译列计划；规则数量决定列数。
  void SetRules(std::vector<FieldRule> rules);

  /// @brief 规则列数。
  std::size_t ColumnCount() const noexcept { return m_rules.size(); }

  /// @brief 当前规则（只读）。
  const std::vector<FieldRule>& Rules() const noexcept { return m_rules; }

  /**
   * @brief 只校验，不产出转换结果。
   * @param row [in] 字段视图；大小必须等于规则列数。
   * @param errors [out] 追加错误；返回 true 时为空。
   * @return true 表示全部列通过。
   */
  bool Validate(const RowView<>& row, std::vector<FieldError>& errors) const;

  /**
   * @brief 校验并转换。
   * @param values [out] 成功时按列顺序写入；失败时被清空。
   * @param errors [out] 追加错误。
   * @return true 表示全部列通过且 values 已写入。
   */
  bool Read(const RowView<>& row, std::vector<core::ItemValue>& values,
            std::vector<FieldError>& errors) const;

  Reader(const Reader&) = delete;
  Reader& operator=(const Reader&) = delete;
  Reader(Reader&&) noexcept = default;
  Reader& operator=(Reader&&) noexcept = default;

 private:
  /// @brief 预编译的列执行计划（避免每次校验重查规则）。
  struct ColumnPlan {
    bool required = false;
    bool fill_default = false;
    std::string_view default_view;
    reflect::TypeId type_id = reflect::kStringType;
  };

  /// @brief `values` 为 nullptr 时只校验。
  bool ReadImpl(const RowView<>& row, std::vector<core::ItemValue>* values,
                std::vector<FieldError>& errors) const;

  std::vector<FieldRule> m_rules;
  std::vector<ColumnPlan> m_plans;
};

}  // namespace go2cpp::json
