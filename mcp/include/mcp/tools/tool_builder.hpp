/**
 * @file tool_builder.hpp
 * @brief 工具层的简洁注册门面：一行注册一个工具/提示/资源。
 *
 * @details 旧代码注册工具要手写 `Tool` 结构体、填 schema、move handler：
 * ```cpp
 * Tool tool;
 * tool.name = "echo";
 * tool.description = "...";
 * tool.input_schema = json::parse(R"(...)");
 * tool.handler = [](const ToolContext&) { ... };
 * registry.RegisterTool(std::move(tool));
 * ```
 * 这里把样板收进 `ToolBuilder`，让工具定义读起来像声明：
 * ```cpp
 * tools::ToolBuilder builder(registry);
 * builder.AddTool("echo", "回显文本", schema, handler);
 * ```
 * 四个方法是链式的，注册顺序即调用顺序。名字重复仍按注册表语义「覆盖」。
 */

#pragma once

#include <functional>
#include <string>
#include <utility>

#include "mcp/server/registry.hpp"

namespace go2cpp::mcp::tools {

class ToolBuilder {
public:
    explicit ToolBuilder(Registry& registry) : m_registry(registry) {}

    /// 注册工具；`schema` 建议至少 `{{"type","object"}}`。
    ToolBuilder& AddTool(std::string name, std::string description, json schema,
                         std::function<ToolResult(const ToolContext&)> handler) {
        Tool tool;
        tool.name = std::move(name);
        tool.description = std::move(description);
        tool.input_schema = std::move(schema);
        tool.handler = std::move(handler);
        m_registry.RegisterTool(std::move(tool));
        return *this;
    }

    /// 注册提示。
    ToolBuilder& AddPrompt(std::string name, std::string description,
                           std::vector<PromptArgument> arguments,
                           std::function<json(const json&)> handler) {
        Prompt prompt;
        prompt.name = std::move(name);
        prompt.description = std::move(description);
        prompt.arguments = std::move(arguments);
        prompt.handler = std::move(handler);
        m_registry.RegisterPrompt(std::move(prompt));
        return *this;
    }

    /// 注册资源。
    ToolBuilder& AddResource(std::string uri, std::string name, std::string description,
                             std::string mime_type, std::function<json(const json&)> reader) {
        Resource resource;
        resource.uri = std::move(uri);
        resource.name = std::move(name);
        resource.description = std::move(description);
        resource.mime_type = std::move(mime_type);
        resource.reader = std::move(reader);
        m_registry.RegisterResource(std::move(resource));
        return *this;
    }

private:
    Registry& m_registry;
};

}  // namespace go2cpp::mcp::tools
