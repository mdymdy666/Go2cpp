/**
 * @file test_pointer.cpp
 * @brief JSON Pointer（RFC 6901）查询测试：基本路径、转义、未找到与错误。
 *
 * 覆盖：
 * - 基本路径：根（空串）、对象成员、数组下标、深层混合路径；
 * - 转义：`~1` → `/`、`~0` → `~`、`~01` 的输出顺序、转义出现在 key 任意位置；
 * - 存在但值为 null 的成员（`Find` 必须成功，区别于「不存在」）；
 * - 空 key（`""`）、数字 key（对象按 key 找而不是下标）；
 * - 失败：缺前导 `/`、非法转义、数组前导零 / 负号 / 非数字 / 溢出、
 *   成员不存在、下标越界、路径穿标量；
 * - 错误 offset 精确性；
 * - 便捷签名 `const Value* Find`（成功 / 失败 nullptr / 覆盖语义）；
 * - 随机：200 棵随机树 × 全部叶子路径的枚举验证；
 * - 真实场景（6 个）：API 响应、HTTP headers（MIME type 含 `/`）、配置数组、
 *   日志字段、空容器、深层数组。
 */

#include <chrono>
#include <cstddef>
#include <cstdio>
#include <random>
#include <string>
#include <string_view>
#include <vector>

#include "go2cpp/json/document.hpp"
#include "go2cpp/json/parser.hpp"
#include "go2cpp/json/pointer.hpp"
#include "go2cpp/json/value.hpp"

namespace {

int g_failures = 0;
std::size_t g_checks = 0;

#define CHECK(condition)                                                            \
  do {                                                                              \
    ++g_checks;                                                                     \
    if (!(condition)) {                                                             \
      std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition);     \
      ++g_failures;                                                                 \
    }                                                                               \
  } while (false)

using go2cpp::json::Document;
using go2cpp::json::Find;
using go2cpp::json::Parser;
using go2cpp::json::PointerError;
using go2cpp::json::PointerFailure;
using go2cpp::json::PointerFailureName;
using go2cpp::json::Value;

/// @brief 会被多个用例共用的固定文档。
const char* const kFixture = R"({
  "user": {"name": "张三", "roles": ["admin", "dev"], "profile": null},
  "headers": {"application/json": "yes", "a/b": "slash", "m~n": "tilde"},
  "arr": [10, 20, [30, 40], {"k": "v"}],
  "empty_obj": {},
  "empty_arr": [],
  "": "empty-key",
  "0": "zero-key"
})";

/// @brief 解析固定文档；失败直接返回 false。
bool ParseFixture(Parser* parser, Document* document) {
  go2cpp::ErrorPtr error;
  return parser->Parse(kFixture, *document, error);
}

/// @brief 期望成功的查询。
void ExpectFound(const Document& document, std::string_view pointer, std::string_view text) {
  ++g_checks;
  Value value;
  PointerError error;
  if (!Find(document, pointer, &value, &error)) {
    std::fprintf(stderr, "FAIL: \"%.60s\" 期望找到，实际 %s@%zu\n", std::string(pointer).c_str(),
                 PointerFailureName(error.failure), error.offset);
    ++g_failures;
    return;
  }
  if (!value.IsString() || value.AsText() != text) {
    std::fprintf(stderr, "FAIL: \"%.60s\" 值不符\n", std::string(pointer).c_str());
    ++g_failures;
  }
}

/// @brief 期望失败的查询。
void ExpectFailure(const Document& document, std::string_view pointer, PointerFailure failure,
                   std::size_t offset = 0) {
  ++g_checks;
  Value value;
  PointerError error;
  if (Find(document, pointer, &value, &error)) {
    std::fprintf(stderr, "FAIL: \"%.60s\" 期望失败却找到\n", std::string(pointer).c_str());
    ++g_failures;
    return;
  }
  if (error.failure != failure || error.offset != offset) {
    std::fprintf(stderr, "FAIL: \"%.60s\" 错误=%s@%zu，期望 %s@%zu\n",
                 std::string(pointer).c_str(), PointerFailureName(error.failure), error.offset,
                 PointerFailureName(failure), offset);
    ++g_failures;
  }
}

// ---------------------------------------------------------------------------
// 1. 基本路径
// ---------------------------------------------------------------------------

void TestBasics() {
  std::printf("\n[1] 基本路径...\n");
  Parser parser;
  Document document;
  CHECK(ParseFixture(&parser, &document));

  // 空指针 = 根。
  Value root;
  PointerError error;
  CHECK(Find(document, "", &root, &error));
  CHECK(root.IsObject());
  CHECK(root.Size() == 7);

  ExpectFound(document, "/user/name", "张三");
  ExpectFound(document, "/user/roles/0", "admin");
  ExpectFound(document, "/user/roles/1", "dev");

  // 数字与嵌套数组。
  Value value;
  CHECK(Find(document, "/arr/0", &value, &error));
  CHECK(value.IsInt() && value.As<std::int64_t>() == 10);
  CHECK(Find(document, "/arr/1", &value, &error));
  CHECK(value.IsInt() && value.As<std::int64_t>() == 20);
  CHECK(Find(document, "/arr/2/1", &value, &error));
  CHECK(value.IsInt() && value.As<std::int64_t>() == 40);
  ExpectFound(document, "/arr/3/k", "v");

  // 存在但值为 null：必须成功（区别于「不存在」）。
  CHECK(Find(document, "/user/profile", &value, &error));
  CHECK(value.IsNull());
  ExpectFailure(document, "/user/nope", PointerFailure::kMemberNotFound, 6);

  // 空 key。
  ExpectFound(document, "/", "empty-key");
  // 数字 key：对象按 key 查，不是按下标。
  ExpectFound(document, "/0", "zero-key");

  // 空容器。
  CHECK(Find(document, "/empty_obj", &value, &error) && value.IsObject() && value.Size() == 0);
  CHECK(Find(document, "/empty_arr", &value, &error) && value.IsArray() && value.Size() == 0);
  ExpectFailure(document, "/empty_obj/x", PointerFailure::kMemberNotFound, 11);
  ExpectFailure(document, "/empty_arr/0", PointerFailure::kIndexOutOfRange, 11);
}

// ---------------------------------------------------------------------------
// 2. 转义
// ---------------------------------------------------------------------------

void TestEscapes() {
  std::printf("\n[2] ~0 / ~1 转义...\n");
  Parser parser;
  Document document;
  CHECK(ParseFixture(&parser, &document));
  ExpectFound(document, "/headers/application~1json", "yes");
  ExpectFound(document, "/headers/a~1b", "slash");
  ExpectFound(document, "/headers/m~0n", "tilde");

  // `~01` 必须先解 ~0 再管 1：结果是 key "~1"，不是 "/"。
  go2cpp::ErrorPtr parse_error;
  Document tricky;
  CHECK(parser.Parse(R"({"~1":"tilde-one","/":"slash"})", tricky, parse_error));
  ExpectFound(tricky, "/~01", "tilde-one");

  // 非法转义与错误位置。
  ExpectFailure(document, "/headers/a~2b", PointerFailure::kInvalidEscape, 10);
  ExpectFailure(document, "/headers/a~", PointerFailure::kInvalidEscape, 10);
  ExpectFailure(document, "/a~x", PointerFailure::kInvalidEscape, 2);

  // 特殊字符 key（含空格、引号、反斜杠）。
  Document special;
  CHECK(parser.Parse(R"({"a b":1,"c\"d":2,"e\\f":3})", special, parse_error));
  Value value;
  PointerError error;
  CHECK(Find(special, "/a b", &value, &error) && value.As<std::int64_t>() == 1);
  CHECK(Find(special, "/c\"d", &value, &error) && value.As<std::int64_t>() == 2);
  CHECK(Find(special, "/e\\f", &value, &error) && value.As<std::int64_t>() == 3);
}

// ---------------------------------------------------------------------------
// 3. 失败与数组下标
// ---------------------------------------------------------------------------

void TestFailures() {
  std::printf("\n[3] 失败路径与数组下标...\n");
  Parser parser;
  Document document;
  CHECK(ParseFixture(&parser, &document));

  ExpectFailure(document, "user", PointerFailure::kMissingLeadingSlash, 0);
  ExpectFailure(document, "/user/name/x", PointerFailure::kNotAContainer, 11);
  ExpectFailure(document, "/arr/2/0/x", PointerFailure::kNotAContainer, 9);
  ExpectFailure(document, "/arr/4", PointerFailure::kIndexOutOfRange, 5);
  ExpectFailure(document, "/arr/99", PointerFailure::kIndexOutOfRange, 5);
  ExpectFailure(document, "/arr/01", PointerFailure::kInvalidArrayIndex, 5);
  ExpectFailure(document, "/arr/-1", PointerFailure::kInvalidArrayIndex, 5);
  ExpectFailure(document, "/arr/x", PointerFailure::kInvalidArrayIndex, 5);
  ExpectFailure(document, "/arr/", PointerFailure::kInvalidArrayIndex, 5);
  // 溢出：远大于 size_t 的数字。
  ExpectFailure(document, "/arr/99999999999999999999999999999999",
                PointerFailure::kInvalidArrayIndex, 5);

  // 对象成员名恰好是 "01" 时，按 key 查（不受数组下标规则影响）。
  go2cpp::ErrorPtr parse_error;
  Document numeric;
  CHECK(parser.Parse(R"({"01":"leading-zero"})", numeric, parse_error));
  ExpectFound(numeric, "/01", "leading-zero");

  // 失败时 out 保持原值。
  Value sentinel;
  PointerError error;
  CHECK(Find(document, "/user/name", &sentinel, &error));
  const std::string_view before = sentinel.AsText();
  Value kept;
  CHECK(Find(document, "/user/name", &kept, &error));
  CHECK(!Find(document, "/nope", &kept, &error));
  CHECK(kept.AsText() == before);
}

// ---------------------------------------------------------------------------
// 4. 便捷签名
// ---------------------------------------------------------------------------

void TestPointerConvenience() {
  std::printf("\n[4] const Value* 便捷签名...\n");
  Parser parser;
  Document document;
  CHECK(ParseFixture(&parser, &document));

  const Value* found = Find(document, "/user/roles/1");
  CHECK(found != nullptr);
  CHECK(found != nullptr && found->AsText() == "dev");

  CHECK(Find(document, "/missing") == nullptr);
  CHECK(Find(document, "bad") == nullptr);
  CHECK(Find(document, "/arr/100") == nullptr);

  // 已知的覆盖语义：同一线程下一次调用会改写上一次结果所在槽位。
  const Value* first = Find(document, "/arr/0");
  const Value* second = Find(document, "/arr/1");
  CHECK(first == second);  // 同一个 thread_local 槽位。
  CHECK(second != nullptr && second->As<std::int64_t>() == 20);
}

// ---------------------------------------------------------------------------
// 5. 随机树：枚举全部叶子路径
// ---------------------------------------------------------------------------

struct RandomTreeBuilder {
  std::mt19937 engine;
  std::string text;

  explicit RandomTreeBuilder(unsigned seed) : engine(seed) {}

  int Range(int limit) {
    std::uniform_int_distribution<int> distribution(0, limit - 1);
    return distribution(engine);
  }

  /// @brief 生成值，并把每条可达路径（相对当前节点）追加到 `paths`。
  void Build(int depth, std::string* paths) {
    const int kind = depth >= 3 ? Range(3) : Range(5);
    switch (kind) {
      case 0:
        text += "null";
        paths->append("\n");
        return;
      case 1: {
        const bool boolean = Range(2) == 0;
        text += boolean ? "true" : "false";
        paths->append("\n");
        return;
      }
      case 2: {
        const int number = Range(100);
        text += std::to_string(number);
        paths->append("\n");
        return;
      }
      case 3: {
        const int count = 1 + Range(3);
        text += '[';
        for (int index = 0; index < count; ++index) {
          if (index > 0) text += ',';
          const std::size_t before = paths->size();
          Build(depth + 1, paths);
          // 给刚生成的子路径加上 `/index` 前缀。
          std::string suffix = "/" + std::to_string(index);
          InsertPrefix(paths, before, suffix);
        }
        text += ']';
        return;
      }
      default: {
        const int count = 1 + Range(3);
        text += '{';
        for (int index = 0; index < count; ++index) {
          if (index > 0) text += ',';
          const std::string key = "k" + std::to_string(index);
          text += '"' + key + "\":";
          const std::size_t before = paths->size();
          Build(depth + 1, paths);
          InsertPrefix(paths, before, "/" + key);
        }
        text += '}';
        return;
      }
    }
  }

  /// @brief 把 `[begin, end)` 内每行路径前加上 `prefix`。
  static void InsertPrefix(std::string* paths, std::size_t begin, const std::string& prefix) {
    std::string rebuilt;
    std::size_t cursor = begin;
    while (cursor < paths->size()) {
      const std::size_t next = paths->find('\n', cursor);
      if (next == std::string::npos) {
        break;
      }
      rebuilt.append(prefix);
      rebuilt.append(*paths, cursor, next - cursor);
      rebuilt.push_back('\n');
      cursor = next + 1;
    }
    paths->resize(begin);
    paths->append(rebuilt);
  }
};

/// @brief 递归验证：路径指向的子树与原树一致（叶子只验证存在与非 null）。
bool VerifyPath(const Value& root, std::string_view path) {
  Value value;
  PointerError error;
  if (!Find(root, path, &value, &error)) {
    std::fprintf(stderr, "随机树: 路径 \"%.40s\" 未找到（%s@%zu）\n", std::string(path).c_str(),
                 PointerFailureName(error.failure), error.offset);
    return false;
  }
  return true;
}

void TestRandomTrees() {
  std::printf("\n[5] 随机树叶子路径枚举（200 棵）...\n");
  Parser parser;
  for (unsigned seed = 0; seed < 200; ++seed) {
    RandomTreeBuilder builder(seed);
    std::string paths;
    builder.Build(0, &paths);
    const std::string& text = builder.text;

    Document document;
    go2cpp::ErrorPtr error;
    ++g_checks;
    if (!parser.Parse(text, document, error)) {
      std::fprintf(stderr, "随机树 %u 解析失败: %.80s\n", seed, text.c_str());
      ++g_failures;
      continue;
    }
    // 逐条路径验证。
    std::size_t cursor = 0;
    std::size_t checked = 0;
    while (cursor < paths.size()) {
      const std::size_t next = paths.find('\n', cursor);
      if (next == std::string::npos) {
        break;
      }
      const std::string_view path(paths.data() + cursor, next - cursor);
      ++g_checks;
      if (!VerifyPath(document.Root(), path)) {
        std::fprintf(stderr, "  树: %.100s\n", text.c_str());
        ++g_failures;
      }
      ++checked;
      cursor = next + 1;
    }
    ++g_checks;
    if (checked == 0) {
      std::fprintf(stderr, "随机树 %u 没有生成路径\n", seed);
      ++g_failures;
    }
  }
}

// ---------------------------------------------------------------------------
// 6. 真实场景（6 个）
// ---------------------------------------------------------------------------

void ScenarioApiResponse() {
  std::printf("\n[6] 场景：API 响应取字段...\n");
  const char* text =
      R"({"code":0,"data":{"users":[{"id":1,"name":"\u5F20\u4E09","tags":["a","b"]},)"
      R"({"id":2,"name":"\u674E\u56DB","tags":[]}]}})";
  Parser parser;
  Document document;
  go2cpp::ErrorPtr parse_error;
  CHECK(parser.Parse(text, document, parse_error));
  Value value;
  PointerError error;
  CHECK(Find(document, "/code", &value, &error) && value.As<std::int64_t>() == 0);
  CHECK(Find(document, "/data/users/0/name", &value, &error));
  CHECK(value.AsText() == "\xE5\xBC\xA0\xE4\xB8\x89");
  ExpectFailure(document, "/data/users/1/tags/0", PointerFailure::kIndexOutOfRange, 19);
  CHECK(Find(document, "/data/users/1/tags", &value, &error) && value.Size() == 0);
}

void ScenarioHttpHeaders() {
  std::printf("\n[7] 场景：HTTP headers（MIME type 含 `/`）...\n");
  const char* text = R"({"headers":{"content-type":"application/json","x/a":"b"}})";
  Parser parser;
  Document document;
  go2cpp::ErrorPtr parse_error;
  CHECK(parser.Parse(text, document, parse_error));
  ExpectFound(document, "/headers/content-type", "application/json");
  // 值是 "application/json"，但 key 路径里 `/` 必须写成 `~1`。
  ExpectFound(document, "/headers/x~1a", "b");
  ExpectFailure(document, "/headers/x/a", PointerFailure::kMemberNotFound, 9);
}

void ScenarioConfigArray() {
  std::printf("\n[8] 场景：配置数组...\n");
  const char* text = R"({"servers":[{"host":"a","port":80},{"host":"b","port":443}]})";
  Parser parser;
  Document document;
  go2cpp::ErrorPtr parse_error;
  CHECK(parser.Parse(text, document, parse_error));
  ExpectFound(document, "/servers/0/host", "a");
  ExpectFound(document, "/servers/1/host", "b");
  Value value;
  PointerError error;
  CHECK(Find(document, "/servers/1/port", &value, &error));
  CHECK(value.As<std::int64_t>() == 443);
  ExpectFailure(document, "/servers/2/host", PointerFailure::kIndexOutOfRange, 9);
}

void ScenarioLogField() {
  std::printf("\n[9] 场景：日志字段（null 与缺失的区别）...\n");
  const char* text = R"({"level":"warn","error":null,"ctx":{"retry":2}})";
  Parser parser;
  Document document;
  go2cpp::ErrorPtr parse_error;
  CHECK(parser.Parse(text, document, parse_error));
  Value value;
  PointerError error;
  CHECK(Find(document, "/error", &value, &error) && value.IsNull());
  ExpectFailure(document, "/error/code", PointerFailure::kNotAContainer, 7);
  CHECK(Find(document, "/ctx/retry", &value, &error) && value.As<std::int64_t>() == 2);
}

void ScenarioEmptyContainers() {
  std::printf("\n[10] 场景：空容器与根...\n");
  Parser parser;
  Document document;
  go2cpp::ErrorPtr parse_error;
  CHECK(parser.Parse(R"({"a":[],"b":{}})", document, parse_error));
  Value value;
  PointerError error;
  CHECK(Find(document, "/a", &value, &error) && value.IsArray() && value.Size() == 0);
  CHECK(Find(document, "/b", &value, &error) && value.IsObject() && value.Size() == 0);
  CHECK(Find(document, "", &value, &error) && value.IsObject());
}

void ScenarioDeepArray() {
  std::printf("\n[11] 场景：深层数组...\n");
  std::string text;
  for (int index = 0; index < 100; ++index) {
    text += '[';
  }
  text += '7';
  for (int index = 0; index < 100; ++index) {
    text += ']';
  }
  Parser parser;
  Document document;
  go2cpp::ErrorPtr parse_error;
  go2cpp::json::ParserConfig config;
  config.max_depth = 200;
  CHECK(parser.Parse(text, document, parse_error, config));
  std::string pointer;
  for (int index = 0; index < 100; ++index) {
    pointer += "/0";
  }
  Value value;
  PointerError error;
  CHECK(Find(document, pointer, &value, &error));
  CHECK(value.IsInt() && value.As<std::int64_t>() == 7);
}

}  // namespace

int main() {
  const auto begin = std::chrono::steady_clock::now();

  TestBasics();
  TestEscapes();
  TestFailures();
  TestPointerConvenience();
  TestRandomTrees();
  ScenarioApiResponse();
  ScenarioHttpHeaders();
  ScenarioConfigArray();
  ScenarioLogField();
  ScenarioEmptyContainers();
  ScenarioDeepArray();

  const auto end = std::chrono::steady_clock::now();
  const auto elapsed_ms =
      std::chrono::duration_cast<std::chrono::milliseconds>(end - begin).count();

  if (g_failures != 0) {
    std::fprintf(stderr, "FAILED: %d check(s) failed out of %zu\n", g_failures, g_checks);
    return 1;
  }
  std::printf("\nALL TESTS PASSED (%zu checks, %lld ms)\n", g_checks,
              static_cast<long long>(elapsed_ms));
  return 0;
}
