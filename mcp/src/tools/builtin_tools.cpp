/**
 * @file builtin_tools.cpp
 * @brief 汇总注册内置工具、提示与资源。
 */

#include "mcp/tools/builtin_tools.hpp"

#include "go2cpp_debug_tools.hpp"

namespace go2cpp::mcp::tools {

void RegisterBuiltinTools(Registry& registry) {
    RegisterEchoTool(registry);
    RegisterCalculatorTools(registry);
    RegisterSleepTool(registry);
    RegisterWeatherTool(registry);
    RegisterNotificationTools(registry);
    RegisterRobotControlTools(registry);
    // Go2Cpp 运行时探针（DebugServer HTTP，默认 127.0.0.1:39101）。
    RegisterGo2CppDebugTools(registry);

    RegisterBuiltinPrompts(registry);
    RegisterBuiltinResources(registry);
}

}  // namespace go2cpp::mcp::tools
