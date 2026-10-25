/**
 * @file validators.hpp
 * @brief 校验层（二）：面向 MCP 场景的校验入口（工具参数 / 资源 URI / 工具定义）。
 *
 * @details 这一层不引用 `mcp/protocol/types.hpp`：只接收 JSON，避免校验层
 * 反向依赖工具层。调用方（Registry / Server）负责从 Tool 里取出 `inputSchema`。
 */

#pragma once

#include <string>

#include "mcp/validation/schema.hpp"

namespace go2cpp::mcp::validation {

/**
 * @brief 按工具的 inputSchema 校验 `tools/call` 的参数。
 * @param input_schema 工具声明的 JSON Schema；null/空对象视为不限制。
 * @param arguments 请求里的 `params.arguments`。
 */
Result ValidateToolArguments(const json& input_schema, const json& arguments);

/**
 * @brief 校验工具定义本身是否可注册。
 * @details 规则：`type` 必须是 object（或缺失）；`properties` 必须是对象；
 *          `required` 必须是字符串数组且每项都在 `properties` 里声明过。
 */
Result ValidateToolDefinition(const json& input_schema);

/**
 * @brief 校验资源 URI。
 * @details MCP 资源 URI 形如 `scheme:///path` 或 `scheme://authority/path`；
 *          这里只要求：非空、含 `://`、scheme 由字母数字与 `+.-` 组成。
 */
Result ValidateResourceUri(const std::string& uri);

}  // namespace go2cpp::mcp::validation
