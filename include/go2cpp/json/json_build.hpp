/**
 * @file json_build.hpp
 * @brief 极简 JSON 构建器——替代 nlohmann 的 `to_json` 写法。
 *
 * ## 为什么需要它
 * `go2cpp/json` 的 `Value` 是**只读句柄**（解析产物），没有 `j["k"] = v` 这种写入 API。
 * `os/monitor` 的 18 个 `to_json` 需要**从零构建** JSON，所以补一个最小 builder。
 *
 * ## 与 nlohmann 的差异（**刻意保持极简**）
 * | nlohmann | 本类 |
 * |---|---|
 * | `json j; j["k"] = v;` | `JsonBuilder b; b.Key("k").Int(v);` |
 * | 自动转义 | `String()` 内部转义（`"` `\` 控制字符 + 非 ASCII 原样） |
 * | 任意嵌套 | 支持（`BeginObject`/`EndObject`/`BeginArray`/`EndArray`） |
 * | 类型安全 | 编译期无检查，**调用方按顺序写**（写错 → 非法 JSON，靠测试兜） |
 *
 * ## 边界与陷阱
 * - **key 与 value 必须交替**（`Key()` 后必须跟一个值方法）；
 * - **逗号自动管理**（首元素不加，后续加）；
 * - 浮点用 `to_chars` 最短往返（与 `writer.hpp` 一致）；
 * - 不做 pretty（monitor 的输出是给程序读的）。
 *
 * ## 数值契约（`docs/json-number-contract.md` 有完整表）
 * | 场景 | 行为 |
 * |---|---|
 * | 整值 `double`（`1.0`） | 输出 `1.0`，**不**退化成 `1` |
 * | `-0.0` | 输出 `-0.0`（保留符号） |
 * | 任意有限值 | `to_chars(general)` 最短表示，`parse(build(x))` **位级相等** |
 * | `NaN` / `Inf` | **拒绝**：不写该值，`ok() == false`，`error()` 给出原因 |
 * | `Decimal` | 直接写 `Decimal::ToString()`，是合法 JSON 数字（不套引号） |
 *
 * ## 失败语义
 * 唯一会失败的是**非有限浮点**（JSON 语法不允许 `nan`/`inf`）与理论上不可达的
 * `to_chars` 溢出。失败后：
 * - 出错的那次写入**不产生任何字节**（不写 `null` 占位、不破坏逗号状态）；
 * - `ok()` 变 false、`error()` 非空；
 * - 之后所有写方法都变成 no-op，直到 `Take()`；
 * - 调用方**必须**在 `Take()` 前检查 `ok()`，否则可能拿到半截 JSON（例如
 *   `{"x":` 后面没有值）。
 */
#pragma once

#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "go2cpp/json/core/decimal.hpp"

namespace go2cpp::json {

class JsonBuilder final {
 public:
  JsonBuilder() = default;

  JsonBuilder& BeginObject() { return Open('{'); }
  JsonBuilder& EndObject() { return Close('}'); }
  JsonBuilder& BeginArray() { return Open('['); }
  JsonBuilder& EndArray() { return Close(']'); }

  /// @brief 写 key（自动加引号 + 冒号）；必须后跟一个值方法。
  JsonBuilder& Key(std::string_view key) {
    if (!m_ok) {
      return *this;
    }
    MaybeComma();
    AppendEscaped(key);
    m_out.push_back(':');
    m_after_key = true;
    return *this;
  }

  JsonBuilder& String(std::string_view value) {
    if (!m_ok) {
      return *this;
    }
    MaybeComma();
    AppendEscaped(value);
    return *this;
  }

  JsonBuilder& Int(std::int64_t value) {
    if (!m_ok) {
      return *this;
    }
    char buffer[32];
    const auto [ptr, ec] = std::to_chars(buffer, buffer + sizeof(buffer), value);
    if (ec != std::errc{}) {
      Fail("JsonBuilder::Int: to_chars failed");
      return *this;
    }
    MaybeComma();
    m_out.append(buffer, static_cast<std::size_t>(ptr - buffer));
    return *this;
  }

  JsonBuilder& Uint(std::uint64_t value) {
    if (!m_ok) {
      return *this;
    }
    char buffer[32];
    const auto [ptr, ec] = std::to_chars(buffer, buffer + sizeof(buffer), value);
    if (ec != std::errc{}) {
      Fail("JsonBuilder::Uint: to_chars failed");
      return *this;
    }
    MaybeComma();
    m_out.append(buffer, static_cast<std::size_t>(ptr - buffer));
    return *this;
  }

  /**
   * @brief 写一个 `double`；整数形态补 `.0`，非有限值报错。
   *
   * @param value [in] 任意 double。`NaN`/`±Inf` 不是合法 JSON 数字，会被拒绝。
   * @return 自身引用（链式调用）；失败时 `ok()` 为 false。
   *
   * @details 最短往返由 `std::to_chars` 的 `chars_format::general` 保证：
   *          `parse(build(x)) == x` 位级相等（含 `-0.0` 的符号）。
   *          `to_chars` 把 `1.0` 写成 `1`，这里补 `.0` 让重新解析仍是浮点节点；
   *          判定只在**本次 to_chars 输出缓冲**里找 `.`/`e`/`E`，绝不看 `m_out`
   *          ——否则前文写过的字符串（如 `"a.b"`）会让后续整值 double 漏掉 `.0`。
   */
  JsonBuilder& Double(double value) {
    if (!m_ok) {
      return *this;
    }
    if (!std::isfinite(value)) {
      Fail("JsonBuilder::Double: NaN/Infinity is not valid JSON");
      return *this;
    }
    char buffer[64];
    const auto [ptr, ec] = std::to_chars(buffer, buffer + sizeof(buffer), value,
                                         std::chars_format::general);
    if (ec != std::errc{}) {
      Fail("JsonBuilder::Double: to_chars failed");
      return *this;
    }
    MaybeComma();
    m_out.append(buffer, static_cast<std::size_t>(ptr - buffer));
    bool has_point_or_exponent = false;
    for (const char* cursor = buffer; cursor != ptr; ++cursor) {
      if (*cursor == '.' || *cursor == 'e' || *cursor == 'E') {
        has_point_or_exponent = true;
        break;
      }
    }
    // 最短往返把 1.0 写成 "1"；补 ".0" 保证重新解析仍是浮点节点。
    if (!has_point_or_exponent) {
      m_out.append(".0");
    }
    return *this;
  }

  /**
   * @brief 写一个 `Decimal`（定点数）：输出 `Decimal::ToString()`，不套引号。
   *
   * @param value [in] 任意 Decimal（零值写成 `0`）。
   * @return 自身引用；`ToString()` 永远产出合法 JSON 数字，不会失败。
   *
   * @note `Decimal` 的文本不含指数；整数形态（如 `18446744073709551616`）也是
   *       合法 JSON 数字。重新解析时若值落在 int64 范围内会成为 int 节点，
   *       这是 JSON 没有 Decimal 类型的固有限制（详见 `docs/json-number-contract.md`）。
   */
  JsonBuilder& Decimal(const core::Decimal& value) {
    if (!m_ok) {
      return *this;
    }
    const std::string text = value.ToString();
    if (text.empty()) {
      Fail("JsonBuilder::Decimal: empty decimal text");
      return *this;
    }
    MaybeComma();
    m_out.append(text);
    return *this;
  }

  JsonBuilder& Bool(bool value) {
    if (!m_ok) {
      return *this;
    }
    MaybeComma();
    m_out.append(value ? "true" : "false");
    return *this;
  }

  JsonBuilder& Null() {
    if (!m_ok) {
      return *this;
    }
    MaybeComma();
    m_out.append("null");
    return *this;
  }

  /// @brief 取出结果（`std::move` 后 builder 不可再用）；失败时是半截 JSON。
  std::string Take() { return std::move(m_out); }

  const std::string& str() const noexcept { return m_out; }

  /// @brief 是否所有写入都成功；`false` 时 `str()` / `Take()` 不可用。
  bool ok() const noexcept { return m_ok; }

  /// @brief 失败原因；`ok() == true` 时为空串。
  const std::string& error() const noexcept { return m_error; }

 private:
  JsonBuilder& Open(char ch) {
    if (!m_ok) {
      return *this;
    }
    MaybeComma();
    m_out.push_back(ch);
    m_stack.push_back(ch);
    m_first.push_back(true);
    m_after_key = false;
    return *this;
  }

  JsonBuilder& Close(char ch) {
    if (!m_ok) {
      return *this;
    }
    if (!m_stack.empty()) {
      m_stack.pop_back();
      m_first.pop_back();
    }
    m_out.push_back(ch);
    m_after_key = false;
    return *this;
  }

  void MaybeComma() {
    if (m_after_key) {
      m_after_key = false;
      return;  // key 后面直接跟值，不加逗号
    }
    if (m_first.empty()) {
      return;  // 顶层第一个值
    }
    if (m_first.back()) {
      m_first.back() = false;
    } else {
      m_out.push_back(',');
    }
  }

  /// @brief 进入失败态；只在第一次失败时记录原因，后续写入全部 no-op。
  void Fail(std::string_view message) {
    if (!m_ok) {
      return;
    }
    m_ok = false;
    m_error.assign(message);
  }

  void AppendEscaped(std::string_view s) {
    m_out.push_back('"');
    for (unsigned char ch : s) {
      switch (ch) {
        case '"': m_out.append("\\\""); break;
        case '\\': m_out.append("\\\\"); break;
        case '\n': m_out.append("\\n"); break;
        case '\r': m_out.append("\\r"); break;
        case '\t': m_out.append("\\t"); break;
        case '\b': m_out.append("\\b"); break;
        case '\f': m_out.append("\\f"); break;
        default:
          if (ch < 0x20) {
            char buf[8];
            std::snprintf(buf, sizeof(buf), "\\u%04x", ch);
            m_out.append(buf);
          } else {
            m_out.push_back(static_cast<char>(ch));
          }
      }
    }
    m_out.push_back('"');
  }

  std::string m_out;
  std::string m_stack;          ///< 未闭合的 `{`/`[`
  std::vector<bool> m_first;    ///< 每层是否还没写过元素
  bool m_after_key{false};
  bool m_ok{true};              ///< false = 已发生不可恢复的写入错误。
  std::string m_error;          ///< 首个错误文本；`m_ok` 为 true 时为空。
};

}  // namespace go2cpp::json
