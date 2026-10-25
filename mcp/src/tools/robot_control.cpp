/**
 * @file robot_control.cpp
 * @brief ROS 2 只读/受控命令工具组（9 个工具），对应参考项目的 robot-control 插件。
 *
 * @details
 * 参考实现直接在 shell 里执行命令、超时靠 `alarm`，且没有区分「只读」与
 * 「会改变系统状态」的命令。这里：
 * - 命令执行统一委托 `util::RunProcess`（底层为 `go2cpp::os::Run`），捕获
 *   合并后的 stdout/stderr，超时后按 TERM→KILL 阶梯终止并回收；
 * - 子命令白名单集中在 `AllowedSubcommand()`；
 * - `ros2_command` 对 `run/launch/set` 等改变状态的子命令要求
 *   `allow_mutating=true`，否则直接拒绝；
 * - 所有结果以 JSON 文本返回（exitCode / timedOut / output），
 *   不做「命令不存在就假装成功」的兜底。
 */

#include "mcp/tools/builtin_tools.hpp"

#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <fcntl.h>
#include <poll.h>
#include <sys/wait.h>
#include <unistd.h>

#include "util/process.hpp"

namespace go2cpp::mcp::tools {

namespace {
bool AllowedSubcommand(const std::string& name) {
    static const char* kAllowed[] = {"node",   "topic", "service", "action", "param",
                                     "pkg",    "run",   "launch",  "interface", "bag"};
    for (const char* item : kAllowed) {
        if (name == item) {
            return true;
        }
    }
    return false;
}

/// 会改变系统状态的子命令（必须显式 allow_mutating）。
bool MutatingSubcommand(const std::string& name) {
    return name == "run" || name == "launch" || name == "set" || name == "record" ||
           name == "play";
}

int TimeoutArgument(const json& arguments) {
    const auto it = arguments.find("timeout_ms");
    if (it == arguments.end() || !it->is_number_integer()) {
        return 5000;
    }
    const long long value = it->get<long long>();
    if (value < 100) {
        return 100;
    }
    if (value > 60000) {
        return 60000;
    }
    return static_cast<int>(value);
}

std::string StringField(const json& arguments, const char* name) {
    const auto it = arguments.find(name);
    return (it != arguments.end() && it->is_string()) ? it->get<std::string>() : std::string();
}

/// 把执行结果编码成 JSON 文本。
ToolResult EncodeResult(const std::vector<std::string>& argv, const util::ProcessResult& result) {
    std::string command;
    for (std::size_t index = 0; index < argv.size(); ++index) {
        if (index != 0) {
            command += ' ';
        }
        command += argv[index];
    }
    json payload = json::object();
    payload["command"] = command;
    payload["exitCode"] = result.exit_code;
    payload["timedOut"] = result.timed_out;
    payload["output"] = result.output;
    const bool failed = result.timed_out || result.exit_code != 0;
    return ToolResult::Text(payload.dump(), failed);
}

/// ros2 查询类工具的通用实现。
ToolResult QueryCommand(const std::vector<std::string>& tail, int timeout_ms) {
    std::vector<std::string> argv{"ros2"};
    argv.insert(argv.end(), tail.begin(), tail.end());
    return EncodeResult(argv, util::RunProcess(argv, timeout_ms));
}

void AddTimeoutSchema(json& properties) {
    properties["timeout_ms"] = json{{"type", "integer"}, {"minimum", 100}, {"maximum", 60000}};
}

}  // namespace

void RegisterRobotControlTools(Registry& registry) {
    // 1. ros2_node_list
    {
        Tool tool;
        tool.name = "ros2_node_list";
        tool.description = "列出 ROS 2 节点";
        json properties = json::object();
        properties["include_hidden"] = json{{"type", "boolean"}, {"default", false}};
        AddTimeoutSchema(properties);
        tool.input_schema = json{{"type", "object"}, {"properties", properties}};
        tool.handler = [](const ToolContext& context) {
            std::vector<std::string> tail{"node", "list"};
            if (context.arguments.value("include_hidden", false)) {
                tail.push_back("--all");
            }
            return QueryCommand(tail, TimeoutArgument(context.arguments));
        };
        registry.RegisterTool(std::move(tool));
    }

    // 2. ros2_topic_list
    {
        Tool tool;
        tool.name = "ros2_topic_list";
        tool.description = "列出 ROS 2 话题";
        json properties = json::object();
        properties["include_hidden"] = json{{"type", "boolean"}, {"default", false}};
        AddTimeoutSchema(properties);
        tool.input_schema = json{{"type", "object"}, {"properties", properties}};
        tool.handler = [](const ToolContext& context) {
            std::vector<std::string> tail{"topic", "list"};
            if (context.arguments.value("include_hidden", false)) {
                tail.push_back("--include-hidden-topics");
            }
            return QueryCommand(tail, TimeoutArgument(context.arguments));
        };
        registry.RegisterTool(std::move(tool));
    }

    // 3. ros2_topic_info
    {
        Tool tool;
        tool.name = "ros2_topic_info";
        tool.description = "查看话题的类型与发布/订阅关系";
        json properties = json::object();
        properties["topic"] = json{{"type", "string"}, {"minLength", 1}};
        properties["verbose"] = json{{"type", "boolean"}, {"default", false}};
        AddTimeoutSchema(properties);
        tool.input_schema = json{{"type", "object"}, {"properties", properties}, {"required", json::array({"topic"})}};
        tool.handler = [](const ToolContext& context) {
            const std::string topic = StringField(context.arguments, "topic");
            if (topic.empty()) {
                return ToolResult::Text("缺少字符串参数 topic", true);
            }
            std::vector<std::string> tail{"topic", "info", topic};
            if (context.arguments.value("verbose", false)) {
                tail.push_back("--verbose");
            }
            return QueryCommand(tail, TimeoutArgument(context.arguments));
        };
        registry.RegisterTool(std::move(tool));
    }

    // 4. ros2_service_list
    {
        Tool tool;
        tool.name = "ros2_service_list";
        tool.description = "列出 ROS 2 服务";
        json properties = json::object();
        properties["include_hidden"] = json{{"type", "boolean"}, {"default", false}};
        AddTimeoutSchema(properties);
        tool.input_schema = json{{"type", "object"}, {"properties", properties}};
        tool.handler = [](const ToolContext& context) {
            std::vector<std::string> tail{"service", "list"};
            if (context.arguments.value("include_hidden", false)) {
                tail.push_back("--include-hidden-services");
            }
            return QueryCommand(tail, TimeoutArgument(context.arguments));
        };
        registry.RegisterTool(std::move(tool));
    }

    // 5. ros2_service_info
    {
        Tool tool;
        tool.name = "ros2_service_info";
        tool.description = "查看 ROS 2 服务信息";
        json properties = json::object();
        properties["service"] = json{{"type", "string"}, {"minLength", 1}};
        AddTimeoutSchema(properties);
        tool.input_schema = json{{"type", "object"}, {"properties", properties}, {"required", json::array({"service"})}};
        tool.handler = [](const ToolContext& context) {
            const std::string service = StringField(context.arguments, "service");
            if (service.empty()) {
                return ToolResult::Text("缺少字符串参数 service", true);
            }
            return QueryCommand({"service", "info", service}, TimeoutArgument(context.arguments));
        };
        registry.RegisterTool(std::move(tool));
    }

    // 6. ros2_action_list
    {
        Tool tool;
        tool.name = "ros2_action_list";
        tool.description = "列出 ROS 2 action";
        json properties = json::object();
        properties["include_hidden"] = json{{"type", "boolean"}, {"default", false}};
        AddTimeoutSchema(properties);
        tool.input_schema = json{{"type", "object"}, {"properties", properties}};
        tool.handler = [](const ToolContext& context) {
            std::vector<std::string> tail{"action", "list"};
            if (context.arguments.value("include_hidden", false)) {
                tail.push_back("--include-hidden-actions");
            }
            return QueryCommand(tail, TimeoutArgument(context.arguments));
        };
        registry.RegisterTool(std::move(tool));
    }

    // 7. ros2_param_list
    {
        Tool tool;
        tool.name = "ros2_param_list";
        tool.description = "列出可见参数（可指定 node）";
        json properties = json::object();
        properties["node"] = json{{"type", "string"}, {"minLength", 1}};
        AddTimeoutSchema(properties);
        tool.input_schema = json{{"type", "object"}, {"properties", properties}};
        tool.handler = [](const ToolContext& context) {
            std::vector<std::string> tail{"param", "list"};
            const std::string node = StringField(context.arguments, "node");
            if (!node.empty()) {
                tail.push_back(node);
            }
            return QueryCommand(tail, TimeoutArgument(context.arguments));
        };
        registry.RegisterTool(std::move(tool));
    }

    // 8. ros2_param_get
    {
        Tool tool;
        tool.name = "ros2_param_get";
        tool.description = "读取单个 ROS 2 参数";
        json properties = json::object();
        properties["node"] = json{{"type", "string"}, {"minLength", 1}};
        properties["parameter"] = json{{"type", "string"}, {"minLength", 1}};
        AddTimeoutSchema(properties);
        tool.input_schema = json{{"type", "object"}, {"properties", properties}, {"required", json::array({"node", "parameter"})}};
        tool.handler = [](const ToolContext& context) {
            const std::string node = StringField(context.arguments, "node");
            const std::string parameter = StringField(context.arguments, "parameter");
            if (node.empty() || parameter.empty()) {
                return ToolResult::Text("缺少字符串参数 node / parameter", true);
            }
            return QueryCommand({"param", "get", node, parameter},
                                TimeoutArgument(context.arguments));
        };
        registry.RegisterTool(std::move(tool));
    }

    // 9. ros2_command
    {
        Tool tool;
        tool.name = "ros2_command";
        tool.description = "执行白名单内的 ros2 子命令；改变状态的命令必须 allow_mutating=true";
        json properties = json::object();
        properties["args"] = json{{"type", "array"}, {"items", json{{"type", "string"}}},
                                  {"minItems", 1}};
        properties["allow_mutating"] = json{{"type", "boolean"}, {"default", false}};
        AddTimeoutSchema(properties);
        tool.input_schema = json{{"type", "object"}, {"properties", properties}, {"required", json::array({"args"})}};
        tool.handler = [](const ToolContext& context) {
            const auto args = context.arguments.find("args");
            if (args == context.arguments.end() || !args->is_array() || args->empty()) {
                return ToolResult::Text("缺少字符串数组参数 args", true);
            }
            std::vector<std::string> tail;
            for (const auto& item : *args) {
                if (!item.is_string()) {
                    return ToolResult::Text("args 必须全部是字符串", true);
                }
                tail.push_back(item.get<std::string>());
            }
            if (!AllowedSubcommand(tail.front())) {
                return ToolResult::Text("子命令不在白名单内: " + tail.front(), true);
            }
            const bool allow_mutating = context.arguments.value("allow_mutating", false);
            if (MutatingSubcommand(tail.front()) && !allow_mutating) {
                return ToolResult::Text("子命令 " + tail.front() +
                                            " 会改变系统状态，需要显式 allow_mutating=true",
                                        true);
            }
            return QueryCommand(tail, TimeoutArgument(context.arguments));
        };
        registry.RegisterTool(std::move(tool));
    }
}

}  // namespace go2cpp::mcp::tools
