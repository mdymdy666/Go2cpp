/**
 * @file sleep.cpp
 * @brief 延时工具：用于模拟耗时操作，也是后续 GMP 压测的负载来源。
 */

#include "mcp/tools/builtin_tools.hpp"

#include <chrono>
#include <string>
#include <thread>

namespace go2cpp::mcp::tools {

void RegisterSleepTool(Registry& registry) {
    Tool tool;
    tool.name = "sleep";
    tool.description = "睡眠指定毫秒数后返回（用于模拟耗时操作）";
    tool.input_schema = json{{"type", "object"},
                             {"properties", {{"milliseconds", {{"type", "integer"},
                                                               {"minimum", 0},
                                                               {"maximum", 60000},
                                                               {"description", "睡眠毫秒数"}}}}},
                             {"required", json::array({"milliseconds"})}};
    tool.handler = [](const ToolContext& context) {
        const auto field = context.arguments.find("milliseconds");
        if (field == context.arguments.end() || !field->is_number_integer()) {
            return ToolResult::Text("缺少整数参数 milliseconds", true);
        }
        const long long milliseconds = field->get<long long>();
        if (milliseconds < 0) {
            return ToolResult::Text("milliseconds 不能为负数", true);
        }
        if (milliseconds > 60000) {
            return ToolResult::Text("milliseconds 超过上限 60000", true);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(milliseconds));
        return ToolResult::Text("slept " + std::to_string(milliseconds) + " ms");
    };
    registry.RegisterTool(std::move(tool));
}

}  // namespace go2cpp::mcp::tools
