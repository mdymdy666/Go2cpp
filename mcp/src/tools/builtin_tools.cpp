/**
 * @file builtin_tools.cpp
 * @brief 汇总注册内置工具、提示与资源。
 */

#include "mcp/tools/builtin_tools.hpp"

namespace go2cpp::mcp::tools {

void RegisterBuiltinTools(Registry& registry) {
    RegisterEchoTool(registry);
    RegisterCalculatorTools(registry);
    RegisterSleepTool(registry);
    RegisterWeatherTool(registry);
    RegisterNotificationTools(registry);
    RegisterRobotControlTools(registry);

    RegisterBuiltinPrompts(registry);
    RegisterBuiltinResources(registry);
}

}  // namespace go2cpp::mcp::tools
