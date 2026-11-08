/**
 * @file test_stream.cpp
 * @brief 顶层数组流式分块解析（`go2cpp::json::ParseArrayStream`）的回归测试。
 *
 * 覆盖：
 * - 切分：空数组、单元素、多标量、前导/尾随空白、CRLF、尾随垃圾；
 * - 元素内容：字符串内逗号/括号/转义/UTF-8、嵌套对象/数组、Decimal、
 *   null/bool、空字符串；
 * - 规模：10 万元素（~2 MB）全量流式 + 峰值 RSS 断言（< 输入 × 3），
 *   并与整份 `Parser::Parse` 的峰值 RSS 做对照；
 * - 非法输入：缺 `]`、元素缺失、尾逗号、括号不配对、顶层非数组、空输入、
 *   尾随垃圾、元素内语法错误；逐项断言 `JsonFailure` 与**整份输入**的
 *   offset/line/column；
 * - 配置：`allow_trailing_comma`、`allow_bom`、`max_input_bytes`、`timeout_ms`；
 * - 语义：回调提前停止、文档复用（不累积）、零拷贝借用、错误位置映射；
 * - 随机：2000 轮随机数组，与整份解析结果逐元素比较；
 * - 真实场景（6 个）：日志导出、订单列表、传感器采样、地理坐标、名单、
 *   分页 API 取前 N 条。
 *
 * 编译运行（Linux / WSL）：
 * ```
 * g++ -std=c++17 -O1 -Wall -Wextra -Wpedantic -Werror -I include \
 *     tests/test_stream.cpp src/go2cpp/json/parser.cpp \
 *     src/go2cpp/json/core/decimal.cpp src/go2cpp/json/core/conversion.cpp -o /tmp/t && /tmp/t
 * ```
 */

#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "go2cpp/json/core/decimal.hpp"
#include "go2cpp/tools/error.hpp"
#include "go2cpp/json/config.hpp"
#include "go2cpp/json/document.hpp"
#include "go2cpp/json/error.hpp"
#include "go2cpp/json/parser.hpp"
#include "go2cpp/json/stream.hpp"
#include "go2cpp/json/value.hpp"

#if defined(__linux__)
#include <sys/resource.h>
#include <unistd.h>
#endif

namespace {

std::size_t g_checks = 0;
std::size_t g_failures = 0;

/// @brief 断言；失败时打印位置并累计失败数，不中断后续用例。
#define CHECK(cond)                                                            \
  do {                                                                         \
    ++g_checks;                                                                \
    if (!(cond)) {                                                             \
      std::fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__, __LINE__,   \
                   #cond);                                                     \
      ++g_failures;                                                            \
    }                                                                          \
  } while (false)

using go2cpp::ErrorMessage;
using go2cpp::ErrorPtr;
using go2cpp::json::core::Decimal;
using go2cpp::json::Document;
using go2cpp::json::JsonError;
using go2cpp::json::JsonFailure;
using go2cpp::json::ParseArrayStream;
using go2cpp::json::Parser;
using go2cpp::json::ParserConfig;
using go2cpp::json::Value;

// ---------------------------------------------------------------------------
// 辅助
// ---------------------------------------------------------------------------

/// @brief 把 `Value` 递归序列化成稳定文本（只用于两次解析结果的比较）。
/// @param value [in] 待序列化的句柄；可为 null 句柄。
/// @param out [in,out] 追加写入的目标；调用方保证其生命周期覆盖本调用。
/// @note 深度由测试数据限制（≤ 8 层），不会爆栈。
void SerializeValue(const Value& value, std::string& out) {
  if (value.IsNull()) {
    out += "null";
    return;
  }
  if (value.IsBool()) {
    out += value.As<bool>() ? "true" : "false";
    return;
  }
  if (value.IsInt()) {
    out += std::to_string(value.As<std::int64_t>());
    return;
  }
  if (value.IsDouble()) {
    char buffer[40];
    const int length = std::snprintf(buffer, sizeof(buffer), "%.17g", value.As<double>());
    if (length > 0) {
      out.append(buffer, static_cast<std::size_t>(length));
    }
    return;
  }
  if (value.IsDecimal()) {
    out += value.As<Decimal>().ToString();
    return;
  }
  if (value.IsString()) {
    out += '"';
    out.append(value.AsText());
    out += '"';
    return;
  }
  if (value.IsArray()) {
    out += '[';
    for (std::size_t index = 0; index < value.Size(); ++index) {
      if (index != 0) {
        out += ',';
      }
      SerializeValue(value.At(index), out);
    }
    out += ']';
    return;
  }
  if (value.IsObject()) {
    out += '{';
    for (std::size_t index = 0; index < value.Size(); ++index) {
      if (index != 0) {
        out += ',';
      }
      out += '"';
      out.append(value.MemberName(index));
      out += "\":";
      SerializeValue(value.At(index), out);
    }
    out += '}';
    return;
  }
  out += "<unknown>";
}

/// @brief 流式解析并把每个元素序列化收集起来。
/// @param input [in] 整份输入；借用。
/// @param out [out] 先清空，再按顺序追加每个元素的序列化文本。
/// @param error [out] 失败时的错误。
/// @param config [in] 解析配置。
/// @return 已成功解析并回调的元素数。
std::size_t CollectElements(std::string_view input, std::vector<std::string>& out, ErrorPtr& error,
                            const ParserConfig& config = ParserConfig{}) {
  out.clear();
  return ParseArrayStream(
      input,
      [&out](Document& doc) {
        std::string text;
        SerializeValue(doc.Root(), text);
        out.push_back(std::move(text));
        return true;
      },
      error, config);
}

/**
 * @brief 断言流式解析在指定位置报出指定错误，并检查已处理元素数。
 * @param text [in] 整份输入。
 * @param expected_failure [in] 期望的失败原因。
 * @param expected_offset [in] 期望的 0 起绝对偏移。
 * @param expected_line [in] 期望的 1 起行号。
 * @param expected_column [in] 期望的 1 起列号。
 * @param expected_count [in] 期望的已成功元素数。
 * @param config [in] 解析配置。
 */
void CheckStreamError(std::string_view text, JsonFailure expected_failure,
                      std::size_t expected_offset, std::size_t expected_line,
                      std::size_t expected_column, std::size_t expected_count,
                      const ParserConfig& config = ParserConfig{}) {
  ++g_checks;
  ErrorPtr error;
  std::size_t calls = 0;
  const std::size_t count = ParseArrayStream(
      text, [&calls](Document&) { ++calls; return true; }, error, config);
  const auto* json_error = dynamic_cast<const JsonError*>(error.get());
  const bool count_ok = count == expected_count && calls == expected_count;
  const bool error_ok =
      json_error != nullptr && json_error->Failure() == expected_failure &&
      json_error->Offset() == expected_offset && json_error->Line() == expected_line &&
      json_error->Column() == expected_column;
  if (count_ok && error_ok) {
    return;
  }
  ++g_failures;
  if (json_error == nullptr) {
    std::fprintf(stderr, "CHECK failed: \"%.80s\" 期望 %s@%zu，实际无 JsonError（count=%zu）: %s\n",
                 std::string(text).c_str(), go2cpp::json::JsonFailureName(expected_failure),
                 expected_offset, count, ErrorMessage(error).c_str());
    return;
  }
  std::fprintf(stderr,
               "CHECK failed: \"%.80s\" 期望 %s@%zu(%zu,%zu)/count=%zu，"
               "实际 %s@%zu(%zu,%zu)/count=%zu\n",
               std::string(text).c_str(), go2cpp::json::JsonFailureName(expected_failure),
               expected_offset, expected_line, expected_column, expected_count,
               go2cpp::json::JsonFailureName(json_error->Failure()), json_error->Offset(),
               json_error->Line(), json_error->Column(), count);
}

/// @brief 生成 10 万元素规模的数组文本（对象元素，约 3 MB）。
/// @param count [in] 元素个数。
/// @return 完整 JSON 文本（`[` + 元素 + `]`）。
std::string MakeLargeArray(std::size_t count) {
  std::string out;
  out.reserve(count * 34 + 2);
  out += '[';
  for (std::size_t index = 0; index < count; ++index) {
    if (index != 0) {
      out += ',';
    }
    out += "{\"id\":";
    out += std::to_string(index);
    out += ",\"v\":";
    out += std::to_string(index * 7);
    out += ",\"tag\":\"x\"}";
  }
  out += ']';
  return out;
}

/// @brief 进程峰值 RSS（KiB）；非 Linux 或读取失败返回 0。
std::size_t PeakRssKb() {
#if defined(__linux__)
  struct rusage usage {};
  if (getrusage(RUSAGE_SELF, &usage) != 0) {
    return 0;
  }
  return static_cast<std::size_t>(usage.ru_maxrss);
#else
  return 0;
#endif
}

/// @brief 进程当前 RSS（KiB）；非 Linux 或读取失败返回 0。
std::size_t CurrentRssKb() {
#if defined(__linux__)
  const long page_size = sysconf(_SC_PAGESIZE);
  if (page_size <= 0) {
    return 0;
  }
  std::FILE* file = std::fopen("/proc/self/statm", "r");
  if (file == nullptr) {
    return 0;
  }
  unsigned long total_pages = 0;
  unsigned long resident_pages = 0;
  const int fields = std::fscanf(file, "%lu %lu", &total_pages, &resident_pages);
  std::fclose(file);
  (void)total_pages;
  if (fields != 2) {
    return 0;
  }
  return static_cast<std::size_t>(resident_pages) * static_cast<std::size_t>(page_size) / 1024u;
#else
  return 0;
#endif
}

// ---------------------------------------------------------------------------
// 1. 数组切分
// ---------------------------------------------------------------------------

void TestArraySplitting() {
  std::printf("\n[1] 数组切分（空/单/多/空白）...\n");

  // 空数组：不回调、无错误。
  {
    std::vector<std::string> elements;
    ErrorPtr error;
    CHECK(CollectElements("[]", elements, error) == 0);
    CHECK(elements.empty());
    CHECK(error == nullptr);
  }
  // 空数组 + 前导/尾随空白与 CRLF。
  {
    std::vector<std::string> elements;
    ErrorPtr error;
    CHECK(CollectElements("  \r\n[   \r\n  ]\r\n  ", elements, error) == 0);
    CHECK(elements.empty());
    CHECK(error == nullptr);
  }
  // 单元素。
  {
    std::vector<std::string> elements;
    ErrorPtr error;
    CHECK(CollectElements("[1]", elements, error) == 1);
    CHECK(elements.size() == 1);
    CHECK(elements[0] == "1");
  }
  // 多个标量元素，混合类型。
  {
    std::vector<std::string> elements;
    ErrorPtr error;
    CHECK(CollectElements(R"([1,-2,3.5,true,false,null,"x"])", elements, error) == 7);
    const std::vector<std::string> expected = {"1", "-2", "3.5", "true", "false", "null", "\"x\""};
    CHECK(elements == expected);
  }
  // 元素间的任意空白（空格/制表/换行）不影响切分。
  {
    std::vector<std::string> elements;
    ErrorPtr error;
    CHECK(CollectElements("[\r\n\t1 \t,\r\n\t2\n,\t3\r\n]", elements, error) == 3);
    CHECK(elements.size() == 3);
    CHECK(elements[0] == "1");
    CHECK(elements[1] == "2");
    CHECK(elements[2] == "3");
  }
  // 尾随空白（含 CRLF）允许。
  {
    std::vector<std::string> elements;
    ErrorPtr error;
    CHECK(CollectElements("[1,2]\r\n\t  ", elements, error) == 2);
    CHECK(error == nullptr);
  }
  // 空字符串元素与只含空白的字符串元素。
  {
    std::vector<std::string> elements;
    ErrorPtr error;
    CHECK(CollectElements(R"(["","   "])", elements, error) == 2);
    CHECK(elements.size() == 2);
    CHECK(elements[0] == "\"\"");
    CHECK(elements[1] == "\"   \"");
  }
}

// ---------------------------------------------------------------------------
// 2. 字符串与嵌套元素（切分器必须看懂字符串与括号）
// ---------------------------------------------------------------------------

void TestStringAndNesting() {
  std::printf("\n[2] 字符串/转义/嵌套元素...\n");

  // 字符串里的 `,` `]` `}` 不能当分隔符。
  {
    std::vector<std::string> elements;
    ErrorPtr error;
    CHECK(CollectElements(R"(["a,b]c}d","e"])", elements, error) == 2);
    CHECK(elements.size() == 2);
    CHECK(elements[0] == "\"a,b]c}d\"");
    CHECK(elements[1] == "\"e\"");
  }
  // 转义引号：`\"` 不是字符串结束。
  {
    std::vector<std::string> elements;
    ErrorPtr error;
    CHECK(CollectElements(R"(["a\"b","c"])", elements, error) == 2);
    CHECK(elements.size() == 2);
    CHECK(elements[0] == "\"a\"b\"");
    CHECK(elements[1] == "\"c\"");
  }
  // 反斜杠在字符串末尾（`"a\\"`）：两个反斜杠是一个转义，后面才是引号。
  {
    std::vector<std::string> elements;
    ErrorPtr error;
    CHECK(CollectElements(R"(["a\\","b"])", elements, error) == 2);
    CHECK(elements.size() == 2);
    CHECK(elements[0] == "\"a\\\"");
  }
  // `\u` 转义与 UTF-8 原字节。
  {
    std::vector<std::string> elements;
    ErrorPtr error;
    CHECK(CollectElements("[\"\\u4f60\\u597d\",\"世界\"]", elements, error) == 2);
    CHECK(elements.size() == 2);
    CHECK(elements[0] == "\"你好\"");
    CHECK(elements[1] == "\"世界\"");
  }
  // 嵌套对象元素：`}` 与 `,` 在深度内。
  {
    std::vector<std::string> elements;
    ErrorPtr error;
    CHECK(CollectElements(R"([{"a":1,"b":2},{"c":[1,2,3]}])", elements, error) == 2);
    CHECK(elements.size() == 2);
    CHECK(elements[0] == R"({"a":1,"b":2})");
    CHECK(elements[1] == R"({"c":[1,2,3]})");
  }
  // 元素本身是数组（数组套数组）。
  {
    std::vector<std::string> elements;
    ErrorPtr error;
    CHECK(CollectElements("[[1,2],[],[3]]", elements, error) == 3);
    CHECK(elements.size() == 3);
    CHECK(elements[0] == "[1,2]");
    CHECK(elements[1] == "[]");
    CHECK(elements[2] == "[3]");
  }
  // 深层嵌套（括号计数必须按层回退）。
  {
    std::vector<std::string> elements;
    ErrorPtr error;
    CHECK(CollectElements(R"([{"a":{"b":{"c":[[[1]]]}}},2])", elements, error) == 2);
    CHECK(elements.size() == 2);
    CHECK(elements[0] == R"({"a":{"b":{"c":[[[1]]]}}})");
    CHECK(elements[1] == "2");
  }
  // 转义出现在嵌套字符串里，且字符串内含括号。
  {
    std::vector<std::string> elements;
    ErrorPtr error;
    CHECK(CollectElements(R"([{"k":"a\"}]"},"tail"])", elements, error) == 2);
    CHECK(elements.size() == 2);
    CHECK(elements[0] == "{\"k\":\"a\"}]\"}");  // 序列化不转义：值为 a"}] 共 4 字节
    CHECK(elements[1] == "\"tail\"");
  }
}

// ---------------------------------------------------------------------------
// 3. 10 万元素 + 峰值 RSS
// ---------------------------------------------------------------------------

void TestLargeArrayAndRss() {
  std::printf("\n[3] 10 万元素流式解析 + 峰值 RSS...\n");

  constexpr std::size_t kElementCount = 100000;
  const std::string input = MakeLargeArray(kElementCount);
  const std::size_t input_bytes = input.size();

  // RSS 基线：输入字符串已生成完毕，之后的内存增长全部来自解析。
  const std::size_t peak_before_kb = PeakRssKb();
  const std::size_t rss_before_kb = CurrentRssKb();

  std::size_t seen = 0;
  std::int64_t id_sum = 0;
  std::int64_t value_sum = 0;
  ErrorPtr error;
  const auto begin_time = std::chrono::steady_clock::now();
  const std::size_t count = ParseArrayStream(
      input,
      [&seen, &id_sum, &value_sum](Document& doc) {
        ++seen;
        const Value& root = doc.Root();
        if (!root.IsObject() || root.Size() != 3) {
          ++g_failures;
          std::fprintf(stderr, "CHECK failed: 第 %zu 个元素不是 3 成员对象\n", seen);
          return true;
        }
        id_sum += root["id"].As<std::int64_t>();
        value_sum += root["v"].As<std::int64_t>();
        return true;
      },
      error);
  const auto end_time = std::chrono::steady_clock::now();

  const std::size_t peak_after_kb = PeakRssKb();
  const std::size_t rss_after_kb = CurrentRssKb();
  const std::size_t growth_kb = peak_after_kb > peak_before_kb ? peak_after_kb - peak_before_kb : 0;
  const auto elapsed_ms =
      std::chrono::duration_cast<std::chrono::milliseconds>(end_time - begin_time).count();

  CHECK(count == kElementCount);
  CHECK(seen == kElementCount);
  CHECK(error == nullptr);
  // sum(0..99999) = 99999*100000/2；v = i*7。
  CHECK(id_sum == 4999950000LL);
  CHECK(value_sum == 4999950000LL * 7);

  std::printf("    输入 %.2f MB（%zu 元素），耗时 %lld ms（%.2f 万元素/秒）\n",
              static_cast<double>(input_bytes) / (1024.0 * 1024.0), kElementCount,
              static_cast<long long>(elapsed_ms),
              elapsed_ms > 0 ? static_cast<double>(kElementCount) / 1000.0 /
                                   (static_cast<double>(elapsed_ms) / 1000.0)
                             : 0.0);
  std::printf("    峰值 RSS: 解析前 %zu KiB → 解析后 %zu KiB（增量 %zu KiB）；当前 %zu → %zu KiB\n",
              peak_before_kb, peak_after_kb, growth_kb, rss_before_kb, rss_after_kb);

#if defined(__linux__)
  // 硬断言（任务口径）：峰值 RSS < 输入大小 × 3。
  CHECK(peak_after_kb * 1024 < input_bytes * 3);
  // 判别力更强的断言：解析阶段新增的峰值 < 输入大小 × 1。
  // 整份 DOM 会按输入预分配节点池/成员池/元素池（≈ 3.5× 输入），必然超标。
  CHECK(growth_kb * 1024 < input_bytes);

  // 对照组：同一输入走整份 Parser::Parse，峰值必然远高于流式路径。
  {
    const std::size_t full_peak_before_kb = PeakRssKb();
    Document full_document;
    ErrorPtr full_error;
    Parser parser;
    const bool full_ok = parser.Parse(input, full_document, full_error);
    const std::size_t full_peak_after_kb = PeakRssKb();
    CHECK(full_ok);
    CHECK(full_document.Root().Size() == kElementCount);
    std::printf("    对照（整份 Parse）：峰值 RSS %zu KiB → %zu KiB（增量 %zu KiB）\n",
                full_peak_before_kb, full_peak_after_kb,
                full_peak_after_kb > full_peak_before_kb ? full_peak_after_kb - full_peak_before_kb
                                                         : 0);
    CHECK(full_peak_after_kb > peak_after_kb);
  }
#else
  std::printf("    （非 Linux：跳过 RSS 断言）\n");
#endif
}

// ---------------------------------------------------------------------------
// 4. 非法输入
// ---------------------------------------------------------------------------

void TestInvalidInput() {
  std::printf("\n[4] 非法输入...\n");

  // 空输入 / 只有空白：数组都没开始。
  CheckStreamError("", JsonFailure::kUnexpectedEnd, 0, 1, 1, 0);
  CheckStreamError("   \r\n", JsonFailure::kUnexpectedEnd, 5, 2, 1, 0);

  // 顶层不是数组（对象 / 标量）——不支持，位置指向第一个非空白字节。
  CheckStreamError(R"({"a":1})", JsonFailure::kUnexpectedChar, 0, 1, 1, 0);
  CheckStreamError("  \n 42", JsonFailure::kUnexpectedChar, 4, 2, 2, 0);
  CheckStreamError(R"("abc")", JsonFailure::kUnexpectedChar, 0, 1, 1, 0);

  // 缺 `]`：数组在输入末尾仍未闭合。
  CheckStreamError("[1,2", JsonFailure::kUnexpectedEnd, 4, 1, 5, 2);
  CheckStreamError("[", JsonFailure::kUnexpectedEnd, 1, 1, 2, 0);
  CheckStreamError("[1,", JsonFailure::kUnexpectedEnd, 3, 1, 4, 1);
  CheckStreamError("[{\"a\":1}", JsonFailure::kUnexpectedEnd, 8, 1, 9, 1);

  // 元素缺失：连续逗号 / 数组以逗号开头。
  CheckStreamError("[1,,2]", JsonFailure::kUnexpectedChar, 3, 1, 4, 1);
  CheckStreamError("[,1]", JsonFailure::kUnexpectedChar, 1, 1, 2, 0);
  CheckStreamError("[1,2,,]", JsonFailure::kUnexpectedChar, 5, 1, 6, 2);

  // 尾逗号：默认拒绝，位置指向 `]`。
  CheckStreamError("[1,]", JsonFailure::kTrailingComma, 3, 1, 4, 1);
  CheckStreamError("[\n  1,\n]", JsonFailure::kTrailingComma, 7, 3, 1, 1);

  // 括号不配对：切分器按深度回退，语法错误由 Parser 报（位置仍是绝对的）。
  CheckStreamError("[{\"a\":1]", JsonFailure::kUnexpectedChar, 7, 1, 8, 0);
  CheckStreamError("[1,2}", JsonFailure::kUnexpectedChar, 4, 1, 5, 2);
  // 元素位置出现 `}`：不是「输入提前结束」，而是语法错误。
  CheckStreamError("[}", JsonFailure::kUnexpectedChar, 1, 1, 2, 0);
  CheckStreamError("[1,}]", JsonFailure::kUnexpectedChar, 3, 1, 4, 1);

  // 数组闭合后还有垃圾。
  CheckStreamError("[1]]", JsonFailure::kTrailingGarbage, 3, 1, 4, 1);
  CheckStreamError("[1] x", JsonFailure::kTrailingGarbage, 4, 1, 5, 1);
  CheckStreamError("[1]{}", JsonFailure::kTrailingGarbage, 3, 1, 4, 1);

  // 元素内部语法错误：偏移必须换算成整份输入的绝对位置。
  CheckStreamError("[1, tru, 3]", JsonFailure::kUnexpectedEnd, 7, 1, 8, 1);
  CheckStreamError("[01]", JsonFailure::kInvalidNumber, 2, 1, 3, 0);
  CheckStreamError(R"(["a\q"])", JsonFailure::kInvalidEscape, 4, 1, 5, 0);
  CheckStreamError(R"(["abc)", JsonFailure::kUnclosedString, 5, 1, 6, 0);

  // 多行输入：错误位置的行列必须按整份输入重放（不是按元素切片）。
  CheckStreamError("[\n  {\"a\":1},\r\n  {\"b\":}\n]", JsonFailure::kUnexpectedChar, 21, 3, 8,
                   1);
}

// ---------------------------------------------------------------------------
// 5. 配置项
// ---------------------------------------------------------------------------

void TestConfig() {
  std::printf("\n[5] 配置（尾逗号/BOM/输入上限/超时）...\n");

  // allow_trailing_comma：允许时 `[1,]` 正常结束；不允许时位置指向 `]`。
  {
    ParserConfig config;
    config.allow_trailing_comma = true;
    std::vector<std::string> elements;
    ErrorPtr error;
    CHECK(CollectElements("[1,]", elements, error, config) == 1);
    CHECK(elements.size() == 1);
    CHECK(elements[0] == "1");
    CHECK(error == nullptr);
    // 连续两个逗号仍是非法的（尾逗号只允许「一个」）。
    CheckStreamError("[1,,]", JsonFailure::kUnexpectedChar, 3, 1, 4, 1, config);
  }

  // allow_bom：跳过 EF BB BF；偏移相对去 BOM 后的视图（与 Parser::Parse 一致）。
  {
    ParserConfig config;
    config.allow_bom = true;
    const std::string with_bom = std::string("\xEF\xBB\xBF") + "[1,2]";
    std::vector<std::string> elements;
    ErrorPtr error;
    CHECK(CollectElements(with_bom, elements, error, config) == 2);
    CHECK(elements.size() == 2);
    CHECK(elements[0] == "1");
    CHECK(elements[1] == "2");
    // 去 BOM 后 `[1,]` 的 `]` 在偏移 3。
    const std::string bad = std::string("\xEF\xBB\xBF") + "[1,]";
    CheckStreamError(bad, JsonFailure::kTrailingComma, 3, 1, 4, 1, config);
    // 默认不开 BOM：BOM 字节是非法字符。
    ParserConfig strict;
    CheckStreamError(with_bom, JsonFailure::kUnexpectedChar, 0, 1, 1, 0, strict);
  }

  // max_input_bytes：作用于**单个元素**，不拒绝整体输入。
  {
    ParserConfig config;
    config.max_input_bytes = 32;
    const std::string big_element = R"([{"key":")" + std::string(64, 'a') + R"("},1])";
    std::vector<std::string> elements;
    ErrorPtr error;
    const std::size_t count = CollectElements(big_element, elements, error, config);
    CHECK(count == 0);
    const auto* json_error = dynamic_cast<const JsonError*>(error.get());
    CHECK(json_error != nullptr);
    if (json_error != nullptr) {
      CHECK(json_error->Failure() == JsonFailure::kTooLarge);
      CHECK(json_error->Offset() == 1);  // 元素起点
    }
    // 整体输入（42 字节）远超 32 字节，但每个元素都很小 → 正常处理。
    std::vector<std::string> small_elements;
    ErrorPtr small_error;
    CHECK(CollectElements("[1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20]", small_elements,
                          small_error, config) == 20);
    CHECK(small_elements.size() == 20);
    CHECK(small_error == nullptr);
  }

  // timeout_ms：整次调用的总预算；10 万元素在 1 ms 内不可能跑完。
  {
    ParserConfig config;
    config.timeout_ms = 1;
    const std::string input = MakeLargeArray(100000);
    ErrorPtr error;
    std::size_t calls = 0;
    const std::size_t count = ParseArrayStream(
        input, [&calls](Document&) { ++calls; return true; }, error, config);
    const auto* json_error = dynamic_cast<const JsonError*>(error.get());
    CHECK(json_error != nullptr);
    if (json_error != nullptr) {
      CHECK(json_error->Failure() == JsonFailure::kTimeout);
    }
    CHECK(count < 100000);
    CHECK(calls == count);
  }
}

// ---------------------------------------------------------------------------
// 6. 回调语义：提前停止 / 文档复用 / 零拷贝 / 错误只写一次
// ---------------------------------------------------------------------------

void TestCallbackSemantics() {
  std::printf("\n[6] 回调语义（提前停止/复用/零拷贝）...\n");

  // 提前停止：返回值包含触发停止的那个元素，error 保持为空。
  {
    ErrorPtr error;
    std::size_t calls = 0;
    const std::size_t count = ParseArrayStream(
        "[1,2,3,4,5]",
        [&calls](Document&) {
          ++calls;
          return calls < 3;
        },
        error);
    CHECK(count == 3);
    CHECK(calls == 3);
    CHECK(error == nullptr);
  }
  // 第一个元素就停：返回 1。
  {
    ErrorPtr error;
    const std::size_t count =
        ParseArrayStream("[1,2,3]", [](Document&) { return false; }, error);
    CHECK(count == 1);
    CHECK(error == nullptr);
  }
  // 文档复用：每次回调看到的是「仅当前元素」的文档，不累积上一个元素。
  {
    std::vector<std::size_t> value_counts;
    std::vector<std::size_t> root_sizes;
    ErrorPtr error;
    const std::size_t count = ParseArrayStream(
        R"([{"a":1,"b":2,"c":3},{"d":4},[1,2],7])",
        [&value_counts, &root_sizes](Document& doc) {
          value_counts.push_back(doc.ValueCount());
          root_sizes.push_back(doc.Root().Size());
          return true;
        },
        error);
    CHECK(count == 4);
    CHECK(root_sizes.size() == 4);
    if (root_sizes.size() == 4) {
      CHECK(root_sizes[0] == 3);
      CHECK(root_sizes[1] == 1);  // 不是 3+1：文档已 Clear
      CHECK(root_sizes[2] == 2);
      CHECK(root_sizes[3] == 0);  // 标量没有子项
    }
    // 每个元素的节点数只与自身有关（对象 1 + 3 值 = 4 等）。
    CHECK(value_counts.size() == 4);
    if (value_counts.size() == 4) {
      CHECK(value_counts[0] == 4);
      CHECK(value_counts[1] == 2);
      CHECK(value_counts[2] == 3);
      CHECK(value_counts[3] == 1);
    }
  }
  // 零拷贝：无转义字符串直接借用输入缓冲（只看第一个元素，避免被后续元素覆盖）。
  {
    const std::string input = R"(["hello-world","second"])";
    ErrorPtr error;
    const char* borrowed = nullptr;
    std::size_t borrowed_size = 0;
    std::size_t index = 0;
    const std::size_t count = ParseArrayStream(
        input,
        [&borrowed, &borrowed_size, &index](Document& doc) {
          std::string_view text;
          if (index == 0 && doc.Root().As(text)) {
            borrowed = text.data();
            borrowed_size = text.size();
          }
          ++index;
          return true;
        },
        error);
    CHECK(count == 2);
    CHECK(borrowed_size == 11);
    CHECK(borrowed == input.data() + 2);  // 指向原始缓冲，不是新分配的副本
  }
  // 有转义时才落进文档字符串池：视图只在回调期间有效，必须在回调内拷走。
  {
    const std::string input = R"(["a\"b"])";
    ErrorPtr error;
    std::string decoded;
    std::size_t seen = 0;
    const std::size_t count = ParseArrayStream(
        input,
        [&decoded, &seen](Document& doc) {
          std::string_view text;
          if (doc.Root().As(text)) {
            decoded.assign(text);
          }
          ++seen;
          return true;
        },
        error);
    CHECK(count == 1);
    CHECK(seen == 1);
    CHECK(decoded == "a\"b");
  }
  // 失败时只写一次错误、且不回调任何元素。
  {
    ErrorPtr error;
    std::size_t calls = 0;
    const std::size_t count = ParseArrayStream(
        "[1,2,oops]", [&calls](Document&) { ++calls; return true; }, error);
    CHECK(count == 2);
    CHECK(calls == 2);
    CHECK(error != nullptr);
  }
}

// ---------------------------------------------------------------------------
// 7. 随机：与整份 Parse 逐元素比对
// ---------------------------------------------------------------------------

/// @brief 生成一段随机字符串（含需要转义的引号与反斜杠），按 JSON 转义后写入。
void AppendRandomString(std::mt19937& rng, std::string& out) {
  static const char kAlphabet[] = "abcXYZ019 ,.:[]{}\\\"\t\n/";
  std::uniform_int_distribution<int> length_dist(0, 12);
  std::uniform_int_distribution<std::size_t> char_dist(0, sizeof(kAlphabet) - 2);
  const int length = length_dist(rng);
  out += '"';
  for (int index = 0; index < length; ++index) {
    const char value = kAlphabet[char_dist(rng)];
    switch (value) {
      case '"':
        out += "\\\"";
        break;
      case '\\':
        out += "\\\\";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\t':
        out += "\\t";
        break;
      default:
        out += value;
        break;
    }
  }
  out += '"';
}

/// @brief 生成一个随机 JSON 值（深度受限，覆盖全部标量类型与容器）。
void AppendRandomValue(std::mt19937& rng, int depth, std::string& out) {
  std::uniform_int_distribution<int> kind_dist(0, depth >= 3 ? 4 : 6);
  const int kind = kind_dist(rng);
  switch (kind) {
    case 0:
      out += "null";
      return;
    case 1:
      out += (rng() & 1u) != 0 ? "true" : "false";
      return;
    case 2: {
      std::uniform_int_distribution<std::int64_t> int_dist(-1000000, 1000000);
      out += std::to_string(int_dist(rng));
      return;
    }
    case 3: {
      std::uniform_real_distribution<double> real_dist(-1000.0, 1000.0);
      char buffer[40];
      const int length = std::snprintf(buffer, sizeof(buffer), "%.6f", real_dist(rng));
      if (length > 0) {
        out.append(buffer, static_cast<std::size_t>(length));
      }
      return;
    }
    case 4:
      AppendRandomString(rng, out);
      return;
    case 5: {
      std::uniform_int_distribution<int> count_dist(0, 4);
      const int count = count_dist(rng);
      out += '[';
      for (int index = 0; index < count; ++index) {
        if (index != 0) {
          out += ',';
        }
        AppendRandomValue(rng, depth + 1, out);
      }
      out += ']';
      return;
    }
    default: {
      std::uniform_int_distribution<int> count_dist(0, 3);
      const int count = count_dist(rng);
      out += '{';
      for (int index = 0; index < count; ++index) {
        if (index != 0) {
          out += ',';
        }
        out += "\"k";
        out += std::to_string(index);
        out += "\":";
        AppendRandomValue(rng, depth + 1, out);
      }
      out += '}';
      return;
    }
  }
}

void TestRandomAgainstFullParse() {
  std::printf("\n[7] 随机数组 vs 整份 Parse（2000 轮）...\n");

  std::mt19937 rng(20240501u);
  std::size_t rounds = 0;
  std::size_t mismatches = 0;
  for (int round = 0; round < 2000; ++round) {
    std::uniform_int_distribution<int> count_dist(0, 12);
    const int element_count = count_dist(rng);
    std::string input = "[";
    for (int index = 0; index < element_count; ++index) {
      if (index != 0) {
        input += ',';
      }
      AppendRandomValue(rng, 0, input);
    }
    input += ']';

    // 整份解析的基准结果。
    Parser parser;
    Document full_document;
    ErrorPtr full_error;
    if (!parser.Parse(input, full_document, full_error)) {
      ++mismatches;
      std::fprintf(stderr, "随机输入整份解析失败: %s\n输入: %s\n", ErrorMessage(full_error).c_str(),
                   input.c_str());
      continue;
    }
    // 流式解析结果。
    std::vector<std::string> elements;
    ErrorPtr stream_error;
    const std::size_t count = CollectElements(input, elements, stream_error);
    if (stream_error != nullptr || count != full_document.Root().Size() ||
        elements.size() != full_document.Root().Size()) {
      ++mismatches;
      std::fprintf(stderr, "随机输入元素数不一致: 流式 %zu/%zu，整份 %zu\n输入: %s\n", count,
                   elements.size(), full_document.Root().Size(), input.c_str());
      continue;
    }
    for (std::size_t index = 0; index < elements.size(); ++index) {
      std::string expected;
      SerializeValue(full_document.Root().At(index), expected);
      if (elements[index] != expected) {
        ++mismatches;
        std::fprintf(stderr, "随机输入第 %zu 个元素不一致:\n  流式 %s\n  整份 %s\n输入: %s\n", index,
                     elements[index].c_str(), expected.c_str(), input.c_str());
        break;
      }
    }
    ++rounds;
  }
  CHECK(rounds == 2000);
  CHECK(mismatches == 0);
}

// ---------------------------------------------------------------------------
// 8. 真实场景（6 个）
// ---------------------------------------------------------------------------

/// @brief 场景 1：日志批量导出（对象数组 + 嵌套 tags + 转义引号）。
void ScenarioLogExport() {
  constexpr std::string_view kInput = R"([
    {"ts":1700000000123,"level":"info","msg":"user login","tags":["auth","web"]},
    {"ts":1700000000124,"level":"warn","msg":"slow query 1.2s","tags":[]},
    {"ts":1700000000125,"level":"error","msg":"disk \"full\"","tags":["io"]}
  ])";

  std::size_t count = 0;
  std::vector<std::string> levels;
  std::vector<std::size_t> tag_counts;
  ErrorPtr error;
  count = ParseArrayStream(
      kInput,
      [&levels, &tag_counts](Document& doc) {
        levels.emplace_back(doc.Root()["level"].AsText());
        tag_counts.push_back(doc.Root()["tags"].Size());
        return true;
      },
      error);
  CHECK(count == 3);
  CHECK(error == nullptr);
  CHECK(levels.size() == 3);
  if (levels.size() == 3) {
    CHECK(levels[0] == "info");
    CHECK(levels[1] == "warn");
    CHECK(levels[2] == "error");
  }
  CHECK(tag_counts.size() == 3);
  if (tag_counts.size() == 3) {
    CHECK(tag_counts[0] == 2);
    CHECK(tag_counts[1] == 0);
    CHECK(tag_counts[2] == 1);
  }
}

/// @brief 场景 2：订单列表（大整数 ID + 超长整数金额走 Decimal + 双精度金额）。
void ScenarioOrders() {
  constexpr std::string_view kInput = R"([
    {"order_id":9007199254740993,"amount":123456789012345678901234567890,"items":2},
    {"order_id":9007199254740995,"amount":0.01,"items":1}
  ])";

  std::vector<std::int64_t> ids;
  std::vector<std::string> amounts;
  ErrorPtr error;
  const std::size_t count = ParseArrayStream(
      kInput,
      [&ids, &amounts](Document& doc) {
        ids.push_back(doc.Root()["order_id"].As<std::int64_t>());
        amounts.push_back(doc.Root()["amount"].As<Decimal>().ToString());
        return true;
      },
      error);
  CHECK(count == 2);
  CHECK(error == nullptr);
  CHECK(ids.size() == 2);
  if (ids.size() == 2) {
    CHECK(ids[0] == 9007199254740993LL);
    CHECK(ids[1] == 9007199254740995LL);
  }
  CHECK(amounts.size() == 2);
  if (amounts.size() == 2) {
    CHECK(amounts[0] == "123456789012345678901234567890");  // 30 位整数：超出 int64 → Decimal
    CHECK(amounts[1] == "0.01");
  }
}

/// @brief 场景 3：传感器采样（纯 double 标量数组，科学计数法）。
void ScenarioSensorSamples() {
  constexpr std::string_view kInput = R"([0.1,0.25,-1.5,3.141592653589793,1e-7,1.5e300])";

  std::vector<double> samples;
  ErrorPtr error;
  const std::size_t count = ParseArrayStream(
      kInput, [&samples](Document& doc) { samples.push_back(doc.Root().As<double>()); return true; },
      error);
  CHECK(count == 6);
  CHECK(error == nullptr);
  CHECK(samples.size() == 6);
  if (samples.size() == 6) {
    CHECK(std::fabs(samples[0] - 0.1) < 1e-12);
    CHECK(std::fabs(samples[1] - 0.25) < 1e-12);
    CHECK(std::fabs(samples[2] + 1.5) < 1e-12);
    CHECK(std::fabs(samples[3] - 3.141592653589793) < 1e-12);
    CHECK(std::fabs(samples[4] - 1e-7) < 1e-18);
    CHECK(samples[5] == 1.5e300);
  }
}

/// @brief 场景 4：地理坐标批量（每个元素是 `[lon,lat]` 数组）。
void ScenarioGeoCoordinates() {
  constexpr std::string_view kInput =
      R"([[116.404,39.915],[121.4737,31.2304],[113.2644,23.1291],[]])";

  std::size_t count = 0;
  std::size_t pairs = 0;
  std::size_t empties = 0;
  ErrorPtr error;
  count = ParseArrayStream(
      kInput,
      [&pairs, &empties](Document& doc) {
        const Value& root = doc.Root();
        if (!root.IsArray()) {
          ++g_failures;
          std::fprintf(stderr, "CHECK failed: 坐标元素不是数组\n");
          return true;
        }
        if (root.Size() == 2) {
          ++pairs;
          if (!root.At(0).IsDouble() || !root.At(1).IsDouble()) {
            ++g_failures;
            std::fprintf(stderr, "CHECK failed: 坐标分量不是 double\n");
          }
        } else if (root.Size() == 0) {
          ++empties;
        } else {
          ++g_failures;
          std::fprintf(stderr, "CHECK failed: 坐标元素大小为 %zu\n", root.Size());
        }
        return true;
      },
      error);
  CHECK(count == 4);
  CHECK(error == nullptr);
  CHECK(pairs == 3);
  CHECK(empties == 1);
}

/// @brief 场景 5：名单（字符串数组：逗号、转义引号、中文、空串）。
void ScenarioAllowList() {
  constexpr std::string_view kInput = R"(["admin,ops","say \"hi\"","你好，世界","","a\\b"])";

  std::vector<std::string> names;
  ErrorPtr error;
  const std::size_t count = ParseArrayStream(
      kInput,
      [&names](Document& doc) {
        std::string_view text;
        doc.Root().As(text);
        names.emplace_back(text);
        return true;
      },
      error);
  CHECK(count == 5);
  CHECK(error == nullptr);
  CHECK(names.size() == 5);
  if (names.size() == 5) {
    CHECK(names[0] == "admin,ops");
    CHECK(names[1] == "say \"hi\"");
    CHECK(names[2] == "你好，世界");
    CHECK(names[3].empty());
    CHECK(names[4] == "a\\b");
  }
}

/// @brief 场景 6：分页 API 取前 N 条（回调提前停止，后面的元素不再解析）。
void ScenarioPagedApiTopN() {
  constexpr std::string_view kInput =
      R"([{"id":1},{"id":2},{"id":3},{"id":4},{"id":5},{"id":6}])";

  std::vector<std::int64_t> ids;
  ErrorPtr error;
  const std::size_t count = ParseArrayStream(
      kInput,
      [&ids](Document& doc) {
        ids.push_back(doc.Root()["id"].As<std::int64_t>());
        return ids.size() < 3;
      },
      error);
  CHECK(count == 3);
  CHECK(error == nullptr);
  CHECK(ids.size() == 3);
  if (ids.size() == 3) {
    CHECK(ids[0] == 1);
    CHECK(ids[1] == 2);
    CHECK(ids[2] == 3);
  }
}

}  // namespace

int main() {
  const auto begin = std::chrono::steady_clock::now();

  TestArraySplitting();
  TestStringAndNesting();
  TestLargeArrayAndRss();
  TestInvalidInput();
  TestConfig();
  TestCallbackSemantics();
  TestRandomAgainstFullParse();

  ScenarioLogExport();
  ScenarioOrders();
  ScenarioSensorSamples();
  ScenarioGeoCoordinates();
  ScenarioAllowList();
  ScenarioPagedApiTopN();

  const auto end = std::chrono::steady_clock::now();
  const auto elapsed_ms =
      std::chrono::duration_cast<std::chrono::milliseconds>(end - begin).count();

  if (g_failures != 0) {
    std::fprintf(stderr, "FAILED: %zu check(s) failed out of %zu\n", g_failures, g_checks);
    return 1;
  }
  std::printf("ALL TESTS PASSED (%zu checks, %lld ms)\n", g_checks,
              static_cast<long long>(elapsed_ms));
  return 0;
}
