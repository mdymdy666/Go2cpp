/**
 * @file echo.cpp
 * @brief 回显工具：协议联调与压测的基础负载，也用来验证 content 编码。
 */

#include "mcp/tools/builtin_tools.hpp"

#include <string>

namespace go2cpp::mcp::tools {

void RegisterEchoTool(Registry& registry) {
    Tool tool;
    tool.name = "echo";
    tool.description = "原样返回输入文本（用于联调与压测）";
    tool.input_schema = json{{"type", "object"},
                             {"properties", {{"text", {{"type", "string"},
                                                       {"description", "要回显的文本"}}}}},
                             {"required", json::array({"text"})}};
    tool.handler = [](const ToolContext& context) {
        const auto field = context.arguments.find("text");
        if (field == context.arguments.end() || !field->is_string()) {
            return ToolResult::Text("缺少字符串参数 text", true);
        }
        return ToolResult::Text(field->get<std::string>());
    };
    registry.RegisterTool(std::move(tool));
}

}  // namespace go2cpp::mcp::tools
