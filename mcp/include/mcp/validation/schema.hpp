/**
 * @file schema.hpp
 * @brief 校验层（一）：JSON Schema 子集校验器。
 *
 * @details
 * 参考项目对工具参数几乎不校验：插件拿到 `arguments` 直接取字段，缺字段就在
 * 插件里崩或返回莫名错误。这里提供一个**够用且可预期**的 JSON Schema 子集：
 *
 * | 关键字 | 语义 |
 * | --- | --- |
 * | `type` | string / number / integer / boolean / object / array / null |
 * | `enum` / `const` | 取值白名单 / 固定值 |
 * | `required` | 对象必填字段 |
 * | `properties` | 对象字段的子 schema |
 * | `additionalProperties` | `false` 时禁止未声明字段（对象形式暂不支持） |
 * | `items` | 数组元素的子 schema |
 * | `minimum` / `maximum` | 数值范围（含端点） |
 * | `minLength` / `maxLength` / `pattern` | 字符串长度与正则 |
 * | `minItems` / `maxItems` | 数组长度 |
 * | `anyOf` / `oneOf` | 任一分支通过即可（不做互斥计数） |
 *
 * 不支持的关键字（`$ref`、`allOf`、`format` 等）**被忽略而不是报错**：
 * 工具 schema 是声明式的，未知关键字不应让整个调用失败。这个取舍写在文档里，
 * 避免使用者误以为做了完整校验。
 */

#pragma once

#include <string>
#include <vector>

#include "nlohmann/json.hpp"

namespace go2cpp::mcp::validation {

using json = nlohmann::json;

/// 一条校验问题。
struct Issue {
    std::string path;     ///< JSON 路径，如 `$.properties.city`
    std::string message;  ///< 中文诊断
};

/// 校验结果。
struct Result {
    bool ok = true;
    std::vector<Issue> issues;

    /// 把所有问题拼成一行（最多前 5 条），用于日志与错误响应。
    std::string Summary() const;
};

/**
 * @brief 用 schema 校验 value。
 * @param schema 期望是 object；不是 object 时直接通过（宽松策略）。
 * @param value 待校验值。
 * @param root_path 起始路径，默认 `$`。
 */
Result Validate(const json& schema, const json& value, const std::string& root_path = "$");

}  // namespace go2cpp::mcp::validation
