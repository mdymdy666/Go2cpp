/**
 * @file reader.cpp
 * @brief `Reader` 的实现：预编译列计划 + 逐列校验/转换 + 错误累积。
 */

#include "go2cpp/json/reader.hpp"

#include <utility>

namespace go2cpp::json {

namespace {

/// @brief 构造一条结构化错误（`ErrorPtr` 恒非空）。
ErrorPtr MakeError(std::string message) { return NewError(std::move(message)); }

/// @brief 记录一条列错误。
void AddError(std::vector<FieldError>& errors, std::size_t index, const std::string& name,
              std::string_view raw, std::string message) {
  FieldError error;
  error.column_index = index;
  error.column_name = name;
  error.raw_value.assign(raw.data(), raw.size());
  error.cause = MakeError(std::move(message));
  errors.push_back(std::move(error));
}

}  // namespace

void Reader::SetRules(std::vector<FieldRule> rules) {
  m_rules = std::move(rules);
  m_plans.clear();
  m_plans.reserve(m_rules.size());
  for (const FieldRule& rule : m_rules) {
    ColumnPlan plan;
    plan.required = rule.required;
    plan.fill_default = !rule.default_value.empty();
    plan.default_view =
        plan.fill_default ? std::string_view(rule.default_value) : std::string_view{};
    plan.type_id = rule.type_id;
    m_plans.push_back(plan);
  }
}

bool Reader::Validate(const RowView<>& row, std::vector<FieldError>& errors) const {
  return ReadImpl(row, nullptr, errors);
}

bool Reader::Read(const RowView<>& row, std::vector<core::ItemValue>& values,
                  std::vector<FieldError>& errors) const {
  values.clear();
  values.reserve(m_rules.size());
  const bool ok = ReadImpl(row, &values, errors);
  if (!ok) {
    values.clear();
  }
  return ok;
}

bool Reader::ReadImpl(const RowView<>& row, std::vector<core::ItemValue>* values,
                      std::vector<FieldError>& errors) const {
  const std::size_t expected = m_rules.size();
  if (row.size() != expected) {
    FieldError error;
    error.column_index = expected;
    error.cause = MakeError("column count mismatch: expected " + std::to_string(expected) +
                            ", got " + std::to_string(row.size()));
    errors.push_back(std::move(error));
    return false;
  }

  bool ok = true;
  for (std::size_t index = 0; index < expected; ++index) {
    const FieldRule& rule = m_rules[index];
    const ColumnPlan& plan = m_plans[index];
    std::string_view raw = row[index];

    if (raw.empty()) {
      if (plan.fill_default) {
        raw = plan.default_view;
      } else if (plan.required) {
        AddError(errors, index, rule.name, raw, "required field missing: " + rule.name);
        ok = false;
        continue;
      } else {
        if (values != nullptr) {
          values->emplace_back();  // std::monostate：可选且未提供
        }
        continue;
      }
    }

    // 字符串列零拷贝：直接借用原始字段缓冲。
    if (plan.type_id == reflect::kStringType) {
      if (values != nullptr) {
        values->emplace_back(core::Text::Borrow(raw));
      }
      continue;
    }

    core::ItemValue item;
    if (!core::Convert(plan.type_id, raw, item)) {
      AddError(errors, index, rule.name, raw,
               "convert failed: " + rule.name + " = " + std::string(raw));
      ok = false;
      continue;
    }
    if (values != nullptr) {
      values->push_back(std::move(item));
    }
  }
  return ok;
}

}  // namespace go2cpp::json
