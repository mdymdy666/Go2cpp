/**
 * @file schema.cpp
 * @brief JSON Schema 子集校验实现。
 */

#include "mcp/validation/schema.hpp"

#include <regex>
#include <string>
#include <vector>

namespace go2cpp::mcp::validation {

namespace {

/// 把 JSON 值类型名转为 schema 用的类型名。
std::string TypeName(const json& value) {
    if (value.is_null()) return "null";
    if (value.is_boolean()) return "boolean";
    if (value.is_number_integer() || value.is_number_unsigned()) return "integer";
    if (value.is_number_float()) return "number";
    if (value.is_string()) return "string";
    if (value.is_array()) return "array";
    if (value.is_object()) return "object";
    return "unknown";
}

bool MatchesType(const std::string& type, const json& value) {
    if (type == "null") return value.is_null();
    if (type == "boolean") return value.is_boolean();
    if (type == "string") return value.is_string();
    if (type == "number") return value.is_number();
    // JSON Schema 中 1.0 也算 integer；这里只接受真正的整数，避免把 1.5 放过。
    if (type == "integer") return value.is_number_integer() || value.is_number_unsigned();
    if (type == "object") return value.is_object();
    if (type == "array") return value.is_array();
    return false;
}

/// 追加一条问题；路径已经由调用方拼好。
void AddIssue(Result& result, const std::string& path, std::string message) {
    result.ok = false;
    result.issues.push_back(Issue{path, std::move(message)});
}

void ValidateNode(const json& schema, const json& value, const std::string& path, Result& result);

/// 任意一个分支通过即可（oneOf 的「恰好一个」语义不实现，文档已声明）。
void ValidateAnyOf(const json& schema, const json& value, const std::string& path,
                   const char* keyword, Result& result) {
    const auto branches = schema.find(keyword);
    if (branches == schema.end() || !branches->is_array() || branches->empty()) {
        return;
    }
    for (const auto& branch : *branches) {
        Result attempt;
        ValidateNode(branch, value, path, attempt);
        if (attempt.ok) {
            return;
        }
    }
    AddIssue(result, path, std::string(keyword) + " 的所有分支都不匹配");
}

void ValidateNode(const json& schema, const json& value, const std::string& path, Result& result) {
    if (!schema.is_object()) {
        return;  // 宽松：schema 不是对象就当没有约束
    }

    // type
    const auto type_it = schema.find("type");
    if (type_it != schema.end()) {
        if (type_it->is_string()) {
            if (!MatchesType(type_it->get<std::string>(), value)) {
                AddIssue(result, path, "期望类型 " + type_it->get<std::string>() + "，实际 " +
                                           TypeName(value));
                return;  // 类型不符时后续关键字无意义
            }
        } else if (type_it->is_array()) {
            bool matched = false;
            for (const auto& candidate : *type_it) {
                if (candidate.is_string() && MatchesType(candidate.get<std::string>(), value)) {
                    matched = true;
                    break;
                }
            }
            if (!matched) {
                AddIssue(result, path, "值类型不在允许的 type 列表里（实际 " + TypeName(value) + "）");
                return;
            }
        }
    }

    // enum / const
    const auto enum_it = schema.find("enum");
    if (enum_it != schema.end() && enum_it->is_array()) {
        bool found = false;
        for (const auto& candidate : *enum_it) {
            if (candidate == value) {
                found = true;
                break;
            }
        }
        if (!found) {
            AddIssue(result, path, "取值不在 enum 白名单内");
        }
    }
    const auto const_it = schema.find("const");
    if (const_it != schema.end() && *const_it != value) {
        AddIssue(result, path, "取值与 const 不一致");
    }

    // 数值范围
    if (value.is_number()) {
        const auto minimum = schema.find("minimum");
        if (minimum != schema.end() && minimum->is_number() &&
            value.get<double>() < minimum->get<double>()) {
            AddIssue(result, path, "值小于 minimum");
        }
        const auto maximum = schema.find("maximum");
        if (maximum != schema.end() && maximum->is_number() &&
            value.get<double>() > maximum->get<double>()) {
            AddIssue(result, path, "值大于 maximum");
        }
    }

    // 字符串
    if (value.is_string()) {
        const std::string text = value.get<std::string>();
        const auto min_length = schema.find("minLength");
        if (min_length != schema.end() && min_length->is_number() && min_length->get<long long>() > 0 &&
            static_cast<long long>(text.size()) < min_length->get<long long>()) {
            AddIssue(result, path, "字符串长度小于 minLength");
        }
        const auto max_length = schema.find("maxLength");
        if (max_length != schema.end() && max_length->is_number() && max_length->get<long long>() >= 0 &&
            static_cast<long long>(text.size()) > max_length->get<long long>()) {
            AddIssue(result, path, "字符串长度大于 maxLength");
        }
        const auto pattern = schema.find("pattern");
        if (pattern != schema.end() && pattern->is_string()) {
            try {
                const std::regex expression(pattern->get<std::string>());
                if (!std::regex_search(text, expression)) {
                    AddIssue(result, path, "字符串不匹配 pattern");
                }
            } catch (const std::regex_error&) {
                // 非法正则属于工具定义错误，不是调用方参数错误：跳过而不是惩罚调用方。
            }
        }
    }

    // 数组
    if (value.is_array()) {
        const auto min_items = schema.find("minItems");
        if (min_items != schema.end() && min_items->is_number() && min_items->get<long long>() > 0 &&
            static_cast<long long>(value.size()) < min_items->get<long long>()) {
            AddIssue(result, path, "数组元素少于 minItems");
        }
        const auto max_items = schema.find("maxItems");
        if (max_items != schema.end() && max_items->is_number() && max_items->get<long long>() >= 0 &&
            static_cast<long long>(value.size()) > max_items->get<long long>()) {
            AddIssue(result, path, "数组元素多于 maxItems");
        }
        const auto items = schema.find("items");
        if (items != schema.end() && items->is_object()) {
            for (std::size_t index = 0; index < value.size(); ++index) {
                ValidateNode(*items, value[index], path + "[" + std::to_string(index) + "]", result);
            }
        }
    }

    // 对象
    if (value.is_object()) {
        const auto required = schema.find("required");
        if (required != schema.end() && required->is_array()) {
            for (const auto& name : *required) {
                if (!name.is_string()) {
                    continue;
                }
                if (value.find(name.get<std::string>()) == value.end()) {
                    AddIssue(result, path, "缺少必填字段 " + name.get<std::string>());
                }
            }
        }
        const auto properties = schema.find("properties");
        if (properties != schema.end() && properties->is_object()) {
            for (const auto& item : properties->items()) {
                const auto found = value.find(item.key());
                if (found != value.end()) {
                    ValidateNode(item.value(), *found, path + "." + item.key(), result);
                }
            }
            const auto additional = schema.find("additionalProperties");
            if (additional != schema.end() && additional->is_boolean() && !additional->get<bool>()) {
                for (const auto& item : value.items()) {
                    if (properties->find(item.key()) == properties->end()) {
                        AddIssue(result, path, "不允许的字段 " + item.key());
                    }
                }
            }
        }
    }

    ValidateAnyOf(schema, value, path, "anyOf", result);
    ValidateAnyOf(schema, value, path, "oneOf", result);
}

}  // namespace

std::string Result::Summary() const {
    if (ok) {
        return "ok";
    }
    std::string summary;
    const std::size_t limit = std::min<std::size_t>(issues.size(), 5);
    for (std::size_t index = 0; index < limit; ++index) {
        if (index != 0) {
            summary += "; ";
        }
        summary += issues[index].path + ": " + issues[index].message;
    }
    if (issues.size() > limit) {
        summary += "; ... 共 " + std::to_string(issues.size()) + " 条";
    }
    return summary;
}

Result Validate(const json& schema, const json& value, const std::string& root_path) {
    Result result;
    ValidateNode(schema, value, root_path, result);
    return result;
}

}  // namespace go2cpp::mcp::validation
