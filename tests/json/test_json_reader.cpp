/**
 * @file test_json_reader.cpp
 * @brief RowView 与 Reader 回归测试：零拷贝视图、规则校验、转换、错误累积、随机对照。
 */
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

#include "go2cpp/json/reader.hpp"

namespace {

int g_failures = 0;

#define CHECK(condition)                                                        \
  do {                                                                          \
    if (!(condition)) {                                                         \
      std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
      ++g_failures;                                                             \
    }                                                                           \
  } while (false)

using go2cpp::json::core::ItemValue;
using go2cpp::json::core::Text;
using go2cpp::json::FieldError;
using go2cpp::json::FieldRule;
using go2cpp::json::Reader;
using go2cpp::json::RowView;
using go2cpp::reflect::kBoolType;
using go2cpp::reflect::kDecimalType;
using go2cpp::reflect::kDoubleType;
using go2cpp::reflect::kInt32Type;
using go2cpp::reflect::kStringType;

/// @brief 1. RowView：视图不拷贝、内联、清空、越界。
void TestRowView() {
  std::printf("\n[1] RowView...\n");
  const std::string buffer = "alpha,beta,gamma";
  RowView<> row;
  CHECK(row.empty());
  CHECK(!row.using_heap());
  row.Append(std::string_view(buffer.data(), 5));
  row.Append(std::string_view(buffer.data() + 6, 4));
  row.Append(std::string_view(buffer.data() + 11, 5));
  CHECK(row.size() == 3u);
  CHECK(!row.using_heap());
  CHECK(row[0] == "alpha");
  CHECK(row[1] == "beta");
  CHECK(row[2] == "gamma");
  // 视图指向原缓冲，不拥有数据。
  CHECK(row[0].data() == buffer.data());

  row.Clear();
  CHECK(row.empty());
  row.Append("x");
  CHECK(row.size() == 1u);
  CHECK(row[0] == "x");

  CHECK(row.At(0) == "x");
  bool threw = false;
  try {
    (void)row.At(1);
  } catch (const std::out_of_range&) {
    threw = true;
  }
  CHECK(threw);

  // N=1 边界：第 2 个字段切换堆。
  RowView<1> narrow;
  narrow.Append("a");
  CHECK(!narrow.using_heap());
  narrow.Append("b");
  CHECK(narrow.using_heap());
  CHECK(narrow.size() == 2u);
}

/// @brief 2. 列数不匹配。
void TestColumnCountMismatch() {
  std::printf("\n[2] 列数不匹配...\n");
  Reader reader;
  reader.SetRules({{"a", kStringType, false, ""}, {"b", kStringType, false, ""}});
  CHECK(reader.ColumnCount() == 2u);

  RowView<> row;
  row.Append("x");
  std::vector<FieldError> errors;
  CHECK(!reader.Validate(row, errors));
  CHECK(errors.size() == 1u);
  CHECK(errors[0].column_index == 2u);  // 期望列数
  CHECK(errors[0].cause != nullptr);
}

/// @brief 3. 必填与默认值。
void TestRequiredAndDefault() {
  std::printf("\n[3] 必填与默认值...\n");
  Reader reader;
  reader.SetRules({
      {"name", kStringType, true, ""},
      {"city", kStringType, false, "Unknown"},
      {"age", kInt32Type, false, ""},
  });

  RowView<> row;
  row.Append("Tom");
  row.Append("");
  row.Append("");
  std::vector<ItemValue> values;
  std::vector<FieldError> errors;
  CHECK(reader.Read(row, values, errors));
  CHECK(values.size() == 3u);
  CHECK(errors.empty());
  CHECK(std::get<Text>(values[1]).view() == "Unknown");
  CHECK(std::holds_alternative<std::monostate>(values[2]));

  // 必填缺失：只报这一条。
  RowView<> missing;
  missing.Append("");
  missing.Append("X");
  missing.Append("1");
  errors.clear();
  CHECK(!reader.Validate(missing, errors));
  CHECK(errors.size() == 1u);
  CHECK(errors[0].column_index == 0u);
  CHECK(errors[0].column_name == "name");
}

/// @brief 4. 转换失败：带列名/原始值/cause。
void TestConvertFailure() {
  std::printf("\n[4] 转换失败...\n");
  Reader reader;
  reader.SetRules({{"age", kInt32Type, false, ""}});

  RowView<> row;
  row.Append("not_a_number");
  std::vector<ItemValue> values;
  std::vector<FieldError> errors;
  CHECK(!reader.Read(row, values, errors));
  CHECK(values.empty());
  CHECK(errors.size() == 1u);
  CHECK(errors[0].column_index == 0u);
  CHECK(errors[0].column_name == "age");
  CHECK(errors[0].raw_value == "not_a_number");
  CHECK(errors[0].cause != nullptr);
  CHECK(!errors[0].cause->Message().empty());
}

/// @brief 5. 成功路径与字符串列零拷贝。
void TestSuccessAndZeroCopy() {
  std::printf("\n[5] 成功路径与零拷贝...\n");
  Reader reader;
  reader.SetRules({
      {"name", kStringType, false, ""},
      {"age", kInt32Type, false, ""},
      {"score", kDoubleType, false, ""},
      {"active", kBoolType, false, ""},
      {"price", kDecimalType, false, ""},
  });

  const std::string buffer = "Tom,25,3.5,true,9.99";
  RowView<> row;
  row.Append(std::string_view(buffer.data(), 3));
  row.Append(std::string_view(buffer.data() + 4, 2));
  row.Append(std::string_view(buffer.data() + 7, 3));
  row.Append(std::string_view(buffer.data() + 11, 4));
  row.Append(std::string_view(buffer.data() + 16, 4));

  std::vector<ItemValue> values;
  std::vector<FieldError> errors;
  CHECK(reader.Read(row, values, errors));
  CHECK(errors.empty());
  CHECK(values.size() == 5u);

  const Text& name = std::get<Text>(values[0]);
  CHECK(name.borrowed());                       // 字符串列借用输入
  CHECK(name.view() == "Tom");
  CHECK(name.view().data() == buffer.data());   // 指向输入，不是拷贝

  CHECK(std::get<std::int64_t>(values[1]) == 25);
  CHECK(std::get<double>(values[2]) == 3.5);
  CHECK(std::get<bool>(values[3]) == true);
  CHECK(std::holds_alternative<go2cpp::json::core::Decimal>(values[4]));
}

/// @brief 6. 错误累积：多列同时错。
void TestErrorAccumulation() {
  std::printf("\n[6] 错误累积...\n");
  Reader reader;
  reader.SetRules({
      {"a", kInt32Type, false, ""},
      {"b", kStringType, true, ""},
      {"c", kDoubleType, false, ""},
  });

  RowView<> row;
  row.Append("x");
  row.Append("");
  row.Append("y");
  std::vector<FieldError> errors;
  CHECK(!reader.Validate(row, errors));
  CHECK(errors.size() == 3u);  // a 转换失败 + b 必填缺失 + c 转换失败
  CHECK(errors[0].column_index == 0u);
  CHECK(errors[1].column_index == 1u);
  CHECK(errors[2].column_index == 2u);
}

/// @brief 7. 随机对照：Validate 与 Read 的返回一致，错误数一致。
void TestRandom() {
  std::printf("\n[7] 随机对照...\n");
  constexpr int kRows = 5000;
  unsigned seed = 20261106u;
  std::size_t mismatches = 0;

  for (int iteration = 0; iteration < kRows; ++iteration) {
    seed = seed * 1103515245u + 12345u;
    const std::size_t columns = 1 + (seed >> 16) % 4;
    Reader reader;
    std::vector<FieldRule> rules;
    for (std::size_t index = 0; index < columns; ++index) {
      seed = seed * 1103515245u + 12345u;
      const int kind = static_cast<int>((seed >> 16) % 3);
      FieldRule rule;
      rule.name = "c" + std::to_string(index);
      rule.type_id = kind == 0 ? kStringType : (kind == 1 ? kInt32Type : kDoubleType);
      rule.required = ((seed >> 8) % 2) != 0;
      rules.push_back(rule);
    }
    reader.SetRules(std::move(rules));

    RowView<> row;
    std::vector<std::string> storage;  // 保活：RowView 只存视图，源串必须活过校验
    storage.reserve(columns);
    for (std::size_t index = 0; index < columns; ++index) {
      seed = seed * 1103515245u + 12345u;
      storage.push_back(((seed >> 16) % 3 == 0) ? "" : std::to_string((seed >> 8) % 100));
      row.Append(storage.back());
    }

    std::vector<FieldError> validate_errors;
    std::vector<FieldError> read_errors;
    std::vector<ItemValue> values;
    const bool validate_ok = reader.Validate(row, validate_errors);
    const bool read_ok = reader.Read(row, values, read_errors);
    if (validate_ok != read_ok || validate_errors.size() != read_errors.size()) {
      ++mismatches;
    }
    if (read_ok && values.size() != columns) {
      ++mismatches;
    }
  }
  CHECK(mismatches == 0u);
  std::printf("  %d 行随机数据，不一致 %zu\n", kRows, mismatches);
}

}  // namespace

int main() {
  std::printf("=== Reader 测试开始 ===\n");
  TestRowView();
  TestColumnCountMismatch();
  TestRequiredAndDefault();
  TestConvertFailure();
  TestSuccessAndZeroCopy();
  TestErrorAccumulation();
  TestRandom();

  if (g_failures == 0) {
    std::printf("\nALL TESTS PASSED\n");
    return 0;
  }
  std::fprintf(stderr, "\n%d check(s) failed\n", g_failures);
  return 1;
}
