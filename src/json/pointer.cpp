/**
 * @file pointer.cpp
 * @brief JSON Pointer（RFC 6901）求值实现。
 *
 * 逐段扫描、零正则：指针语法只有「`/` 分段 + `~0`/`~1` 转义 + 数组十进制
 * 下标」三条规则，用 `std::string_view::find` 与一次线性扫描足够；不引入
 * 任何解析器生成器或正则，避免把一个 O(len) 操作变成带分配的重活。
 */

#include "go2cpp/json/pointer.hpp"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>

#include "go2cpp/json/document.hpp"
#include "go2cpp/json/value.hpp"

namespace go2cpp::json {

namespace {

/// @brief 把一段原始 token 解码成对象 key（`~1` → `/`，`~0` → `~`）。
///
/// 逐字节扫描而不是先 replace：RFC 6901 要求 `~1` 先于 `~0` 替换，若先做
/// 字符串级 replace，`~01` 会被两步替换成 `/`（错误）。单次扫描天然满足顺序。
bool DecodeToken(std::string_view raw, std::size_t base_offset, std::string* token,
                 PointerError* error) {
  token->clear();
  token->reserve(raw.size());
  for (std::size_t index = 0; index < raw.size(); ++index) {
    if (raw[index] != '~') {
      token->push_back(raw[index]);
      continue;
    }
    if (index + 1 >= raw.size() || (raw[index + 1] != '0' && raw[index + 1] != '1')) {
      if (error != nullptr) {
        error->failure = PointerFailure::kInvalidEscape;
        error->offset = base_offset + index;
      }
      return false;
    }
    token->push_back(raw[index + 1] == '0' ? '~' : '/');
    ++index;  // 跳过被消费的转义字符。
  }
  return true;
}

/// @brief 解析 RFC 6901 数组下标：`0` 或 `[1-9][0-9]*`，不接受前导零。
bool ParseArrayIndex(std::string_view token, std::size_t* index) {
  if (token.empty()) {
    return false;
  }
  if (token.size() > 1 && token[0] == '0') {
    return false;
  }
  std::size_t value = 0;
  for (const char digit : token) {
    if (digit < '0' || digit > '9') {
      return false;
    }
    const std::size_t previous = value;
    value = value * 10 + static_cast<std::size_t>(digit - '0');
    if (value / 10 != previous) {
      return false;  // 溢出：不可能命中任何真实数组。
    }
  }
  *index = value;
  return true;
}

/// @brief 在对象里按 key 精确查找；找到返回 true 并写 `out`。
///
/// 不用 `Value::Find`：它查不到与「成员值恰好是 null」返回同一个空句柄，
/// Pointer 必须区分这两种情况（前者不存在、后者存在且为 null）。
bool FindMember(const Value& object, std::string_view key, Value* out) {
  const std::size_t count = object.Size();
  for (std::size_t index = 0; index < count; ++index) {
    if (object.MemberName(index) == key) {
      *out = object.At(index);
      return true;
    }
  }
  return false;
}

/// @brief `Find` 的公共实现；`error` 可为 nullptr。
bool FindImpl(const Value& root, std::string_view pointer, Value* out, PointerError* error) {
  if (error != nullptr) {
    *error = PointerError{};
  }
  if (pointer.empty()) {
    *out = root;  // RFC 6901：空指针指向整个文档。
    return true;
  }
  if (pointer.front() != '/') {
    if (error != nullptr) {
      error->failure = PointerFailure::kMissingLeadingSlash;
      error->offset = 0;
    }
    return false;
  }

  Value current = root;
  std::string token;
  std::size_t position = 1;  // 跳过首个 '/'。
  for (;;) {
    const std::size_t slash = pointer.find('/', position);
    const std::size_t segment_end = slash == std::string_view::npos ? pointer.size() : slash;
    const std::string_view raw = pointer.substr(position, segment_end - position);

    if (!DecodeToken(raw, position, &token, error)) {
      return false;
    }

    if (current.IsObject()) {
      Value next;
      if (!FindMember(current, token, &next)) {
        if (error != nullptr) {
          error->failure = PointerFailure::kMemberNotFound;
          error->offset = position;
        }
        return false;
      }
      current = next;
    } else if (current.IsArray()) {
      std::size_t index = 0;
      if (!ParseArrayIndex(token, &index)) {
        if (error != nullptr) {
          error->failure = PointerFailure::kInvalidArrayIndex;
          error->offset = position;
        }
        return false;
      }
      if (index >= current.Size()) {
        if (error != nullptr) {
          error->failure = PointerFailure::kIndexOutOfRange;
          error->offset = position;
        }
        return false;
      }
      current = current.At(index);
    } else {
      if (error != nullptr) {
        error->failure = PointerFailure::kNotAContainer;
        error->offset = position;
      }
      return false;
    }

    if (slash == std::string_view::npos) {
      break;
    }
    position = slash + 1;
  }
  *out = current;
  return true;
}

}  // namespace

bool Find(const Document& doc, std::string_view pointer, Value* out, PointerError* error) {
  return FindImpl(doc.Root(), pointer, out, error);
}

bool Find(const Value& root, std::string_view pointer, Value* out, PointerError* error) {
  return FindImpl(root, pointer, out, error);
}

const Value* Find(const Document& doc, std::string_view pointer) {
  // thread_local 槽位：返回指针必须指向稳定存储，而 Value 是 16 B 句柄，
  // 无法指向文档内部（文档没有 Value 数组）。同一线程连续调用会覆盖上一次
  // 结果，这是该便捷签名的固有限制，头文件已写明。
  thread_local Value result;
  PointerError error;
  if (!Find(doc, pointer, &result, &error)) {
    return nullptr;
  }
  return &result;
}

const char* PointerFailureName(PointerFailure failure) noexcept {
  switch (failure) {
    case PointerFailure::kNone:
      return "none";
    case PointerFailure::kMissingLeadingSlash:
      return "missing leading slash";
    case PointerFailure::kInvalidEscape:
      return "invalid escape";
    case PointerFailure::kInvalidArrayIndex:
      return "invalid array index";
    case PointerFailure::kIndexOutOfRange:
      return "index out of range";
    case PointerFailure::kMemberNotFound:
      return "member not found";
    case PointerFailure::kNotAContainer:
      return "not a container";
  }
  return "unknown";
}

}  // namespace go2cpp::json
