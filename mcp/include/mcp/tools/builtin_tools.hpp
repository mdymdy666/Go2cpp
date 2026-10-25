/**
 * @file builtin_tools.hpp
 * @brief 内置工具 / 提示 / 资源注册入口（公开头文件）。
 *
 * @details 每个类别一个编译单元，`RegisterBuiltinTools` 汇总；新增工具时只改
 * 汇总处，不影响协议层。嵌入式使用方只要链接 `mcp_core` 并调用本函数即可。
 */

#pragma once

#include "mcp/server/registry.hpp"

namespace go2cpp::mcp::tools {

/// 注册全部内置工具 / 提示 / 资源。
void RegisterBuiltinTools(Registry& registry);

/// calculator 工具组：calculator、add、subtract、multiply、divide、power、sqrt、factorial。
void RegisterCalculatorTools(Registry& registry);
void RegisterSleepTool(Registry& registry);
void RegisterEchoTool(Registry& registry);
void RegisterWeatherTool(Registry& registry);
/// notification 工具组：logging_test、progress_test。
void RegisterNotificationTools(Registry& registry);
/// ROS 2 工具组：9 个 ros2_* 工具。
void RegisterRobotControlTools(Registry& registry);

/// 资源：bacio:///quote、document:///life-choices.txt。
void RegisterBuiltinResources(Registry& registry);
/// 提示：code-review。
void RegisterBuiltinPrompts(Registry& registry);

}  // namespace go2cpp::mcp::tools
