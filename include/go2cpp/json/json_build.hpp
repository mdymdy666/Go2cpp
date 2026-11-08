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
 */
#pragma once

#include <charconv>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>
#include <cstdio>

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
    MaybeComma();
    AppendEscaped(key);
    m_out.push_back(':');
    m_after_key = true;
    return *this;
  }

  JsonBuilder& String(std::string_view value) {
    MaybeComma();
    AppendEscaped(value);
    return *this;
  }

  JsonBuilder& Int(std::int64_t value) {
    MaybeComma();
    char buf[32];
    auto [ptr, ec] = std::to_chars(buf, buf + sizeof(buf), value);
    if (ec == std::errc{}) {
      m_out.append(buf, static_cast<std::size_t>(ptr - buf));
    } else {
      m_out.append("0");
    }
    return *this;
  }

  JsonBuilder& Uint(std::uint64_t value) {
    MaybeComma();
    char buf[32];
    auto [ptr, ec] = std::to_chars(buf, buf + sizeof(buf), value);
    if (ec == std::errc{}) {
      m_out.append(buf, static_cast<std::size_t>(ptr - buf));
    } else {
      m_out.append("0");
    }
    return *this;
  }

  JsonBuilder& Double(double value) {
    MaybeComma();
    char buf[64];
    auto [ptr, ec] = std::to_chars(buf, buf + sizeof(buf), value,
                                   std::chars_format::general);
    if (ec == std::errc{}) {
      m_out.append(buf, static_cast<std::size_t>(ptr - buf));
      // 整数形态补 .0（与 writer.hpp 契约一致，保持类型）
      if (m_out.find('.') == std::string::npos &&
          m_out.find('e') == std::string::npos &&
          m_out.find('E') == std::string::npos) {
        m_out.append(".0");
      }
    } else {
      m_out.append("0.0");
    }
    return *this;
  }

  JsonBuilder& Bool(bool value) {
    MaybeComma();
    m_out.append(value ? "true" : "false");
    return *this;
  }

  JsonBuilder& Null() {
    MaybeComma();
    m_out.append("null");
    return *this;
  }

  /// @brief 取出结果（`std::move` 后 builder 不可再用）。
  std::string Take() { return std::move(m_out); }

  const std::string& str() const noexcept { return m_out; }

 private:
  JsonBuilder& Open(char ch) {
    MaybeComma();
    m_out.push_back(ch);
    m_stack.push_back(ch);
    m_first.push_back(true);
    m_after_key = false;
    return *this;
  }

  JsonBuilder& Close(char ch) {
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
};

}  // namespace go2cpp::json
