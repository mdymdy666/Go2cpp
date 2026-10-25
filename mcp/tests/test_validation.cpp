/**
 * @file test_validation.cpp
 * @brief 校验层单元测试：JSON Schema 子集、工具参数、资源 URI、工具定义。
 */

#include <string>

#include "mcp/validation/schema.hpp"
#include "mcp/validation/validators.hpp"
#include "test_util.hpp"

int main() {
    using namespace go2cpp::mcp;
    using namespace go2cpp::mcp::test;
    using go2cpp::mcp::validation::json;

    // 1. type 与 required。
    {
        const json schema = json{{"type", "object"},
                                 {"properties", json{{"name", json{{"type", "string"}}}}},
                                 {"required", json::array({"name"})}};
        const auto ok = validation::Validate(schema, json{{"name", "x"}});
        CHECK(ok.ok);
        const auto missing = validation::Validate(schema, json::object());
        CHECK(!missing.ok);
        const auto wrong_type = validation::Validate(schema, json{{"name", 42}});
        CHECK(!wrong_type.ok);
    }

    // 2. additionalProperties=false。
    {
        const json schema = json{{"type", "object"},
                                 {"properties", json{{"a", json{{"type", "integer"}}}}},
                                 {"additionalProperties", false}};
        CHECK(validation::Validate(schema, json{{"a", 1}}).ok);
        CHECK(!validation::Validate(schema, json{{"a", 1}, {"b", 2}}).ok);
    }

    // 3. 数值范围 / 字符串长度 / pattern / enum。
    {
        const json schema = json{{"type", "object"},
                                 {"properties", json{{"count", json{{"type", "integer"}, {"minimum", 1}, {"maximum", 10}}},
                                                     {"mode", json{{"type", "string"}, {"enum", json::array({"fast", "slow"})}}},
                                                     {"code", json{{"type", "string"}, {"minLength", 3}, {"maxLength", 5}, {"pattern", "^[a-z]+$"}}}}}};
        CHECK(validation::Validate(schema, json{{"count", 5}, {"mode", "fast"}, {"code", "abc"}}).ok);
        CHECK(!validation::Validate(schema, json{{"count", 0}}).ok);
        CHECK(!validation::Validate(schema, json{{"count", 11}}).ok);
        CHECK(!validation::Validate(schema, json{{"mode", "medium"}}).ok);
        CHECK(!validation::Validate(schema, json{{"code", "ab"}}).ok);
        CHECK(!validation::Validate(schema, json{{"code", "abcdef"}}).ok);
        CHECK(!validation::Validate(schema, json{{"code", "ABC"}}).ok);
    }

    // 4. 数组 items / minItems。
    {
        const json schema = json{{"type", "array"},
                                 {"items", json{{"type", "integer"}}},
                                 {"minItems", 2}};
        CHECK(validation::Validate(schema, json::array({1, 2})).ok);
        CHECK(!validation::Validate(schema, json::array({1})).ok);
        CHECK(!validation::Validate(schema, json::array({1, "x"})).ok);
        CHECK(!validation::Validate(schema, json::array({1, 2, "x"})).ok);
    }

    // 5. 嵌套路径写在诊断里，便于定位。
    {
        const json schema = json{{"type", "object"},
                                 {"properties", json{{"inner", json{{"type", "object"},
                                                                     {"properties", json{{"value", json{{"type", "string"}}}}}}}}}};
        const auto result = validation::Validate(schema, json{{"inner", json{{"value", 1}}}});
        CHECK(!result.ok);
        CHECK(result.Summary().find("inner") != std::string::npos);
    }

    // 6. 工具参数：非对象、schema 为空、有效参数。
    {
        CHECK(!validation::ValidateToolArguments(json::object(), json::array()).ok);
        CHECK(validation::ValidateToolArguments(json::object(), json{{"any", 1}}).ok);
        const json schema = json{{"type", "object"},
                                 {"properties", json{{"query", json{{"type", "string"}}}}},
                                 {"required", json::array({"query"})}};
        CHECK(validation::ValidateToolArguments(schema, json{{"query", "x"}}).ok);
        CHECK(!validation::ValidateToolArguments(schema, json::object()).ok);
    }

    // 7. 资源 URI。
    {
        CHECK(validation::ValidateResourceUri("bacio:///quote").ok);
        CHECK(validation::ValidateResourceUri("document:///a.txt").ok);
        CHECK(!validation::ValidateResourceUri("").ok);
        CHECK(!validation::ValidateResourceUri("no-scheme").ok);
        CHECK(!validation::ValidateResourceUri("bad scheme:///x").ok);
    }

    // 8. 工具定义：required 必须已声明。
    {
        CHECK(validation::ValidateToolDefinition(json{{"type", "object"}}).ok);
        CHECK(!validation::ValidateToolDefinition(json{{"type", "array"}}).ok);
        CHECK(!validation::ValidateToolDefinition(
                   json{{"type", "object"},
                        {"properties", json{{"a", json{{"type", "string"}}}}},
                        {"required", json::array({"b"})}})
                   .ok);
    }

    return Summary("validation");
}
