/**
 * @file validators.cpp
 * @brief 面向 MCP 的校验入口实现。
 */

#include "mcp/validation/validators.hpp"

#include <cctype>
#include <string>

namespace go2cpp::mcp::validation {

namespace {

bool ValidScheme(const std::string& scheme) {
    if (scheme.empty()) {
        return false;
    }
    for (char character : scheme) {
        const bool alpha = (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z');
        const bool digit = character >= '0' && character <= '9';
        if (!alpha && !digit && character != '+' && character != '-' && character != '.') {
            return false;
        }
    }
    return true;
}

}  // namespace

Result ValidateToolArguments(const json& input_schema, const json& arguments) {
    Result result;
    if (!arguments.is_object()) {
        result.ok = false;
        result.issues.push_back(Issue{"$", "arguments 必须是 JSON 对象"});
        return result;
    }
    if (input_schema.is_null() || !input_schema.is_object() || input_schema.empty()) {
        return result;  // 工具没声明 schema：不限制
    }
    return Validate(input_schema, arguments);
}

Result ValidateToolDefinition(const json& input_schema) {
    Result result;
    if (input_schema.is_null()) {
        return result;
    }
    if (!input_schema.is_object()) {
        result.ok = false;
        result.issues.push_back(Issue{"$", "inputSchema 必须是对象"});
        return result;
    }
    const auto type = input_schema.find("type");
    if (type != input_schema.end() && (!type->is_string() || type->get<std::string>() != "object")) {
        result.ok = false;
        result.issues.push_back(Issue{"$.type", "工具 inputSchema.type 必须是 object"});
    }
    const auto properties = input_schema.find("properties");
    if (properties != input_schema.end() && !properties->is_object()) {
        result.ok = false;
        result.issues.push_back(Issue{"$.properties", "properties 必须是对象"});
    }
    const auto required = input_schema.find("required");
    if (required != input_schema.end()) {
        if (!required->is_array()) {
            result.ok = false;
            result.issues.push_back(Issue{"$.required", "required 必须是数组"});
        } else {
            for (const auto& name : *required) {
                if (!name.is_string()) {
                    result.ok = false;
                    result.issues.push_back(Issue{"$.required", "required 元素必须是字符串"});
                    continue;
                }
                if (properties != input_schema.end() && properties->is_object() &&
                    properties->find(name.get<std::string>()) == properties->end()) {
                    result.ok = false;
                    result.issues.push_back(
                        Issue{"$.required", "required 字段 " + name.get<std::string>() + " 未在 properties 中声明"});
                }
            }
        }
    }
    return result;
}

Result ValidateResourceUri(const std::string& uri) {
    Result result;
    if (uri.empty()) {
        result.ok = false;
        result.issues.push_back(Issue{"$", "资源 URI 不能为空"});
        return result;
    }
    const std::size_t separator = uri.find("://");
    if (separator == std::string::npos || separator == 0) {
        result.ok = false;
        result.issues.push_back(Issue{"$", "资源 URI 必须包含 scheme://"});
        return result;
    }
    if (!ValidScheme(uri.substr(0, separator))) {
        result.ok = false;
        result.issues.push_back(Issue{"$", "资源 URI 的 scheme 含非法字符"});
    }
    return result;
}

}  // namespace go2cpp::mcp::validation
