/**
 * @file types.hpp
 * @brief MCP 的领域类型：工具、提示、资源，以及它们到 JSON 的编码。
 *
 * @details
 * 参考项目把这些类型塞在插件 C ABI 结构体里（`PluginTool` / `PluginPrompt` /
 * `PluginResource`），导致每个字段都是裸 `const char*`、跨 DLL 边界传指针、
 * 生命周期完全靠约定。这里改成值语义 + `std::function` 回调：
 * - 注册时拷贝一份描述，调用方之后改原对象不影响已注册项；
 * - 处理函数直接返回值，不再有「谁 `delete[]` 返回值」的问题。
 *
 * 编码函数集中在这里，保证 `tools/list`、`prompts/list`、`resources/list`
 * 三个响应与规范字段名完全一致，不在 server 里散落字段名。
 */

#pragma once

#include <functional>
#include <string>
#include <vector>

#include "nlohmann/json.hpp"

namespace go2cpp::mcp {

using json = nlohmann::json;

/// MCP 内容块。目前只实现 text（覆盖原项目全部内置工具的输出形态）。
struct Content {
    std::string type = "text";  ///< 内容类型，规范当前要求 "text"
    std::string text;           ///< 文本正文

    json ToJson() const {
        json value = json::object();
        value["type"] = type;
        value["text"] = text;
        return value;
    }
};

/// 工具调用结果。对应 MCP `tools/call` 的 result 对象。
struct ToolResult {
    bool is_error = false;                ///< 工具自身是否报告了业务错误
    std::vector<Content> content;         ///< 返回内容块
    json structured = nullptr;            ///< 可选 structuredContent（未启用时为 null）

    json ToJson() const {
        json value = json::object();
        value["isError"] = is_error;
        value["content"] = json::array();
        for (const auto& item : content) {
            value["content"].push_back(item.ToJson());
        }
        if (!structured.is_null()) {
            value["structuredContent"] = structured;
        }
        return value;
    }

    /// 便捷构造：单个文本块。
    static ToolResult Text(std::string text, bool is_error = false) {
        ToolResult result;
        result.is_error = is_error;
        result.content.push_back(Content{"text", std::move(text)});
        return result;
    }
};

/// 工具执行上下文。
/**
 * @details
 * 参考项目的插件只能在 `HandleRequest` 里拿到原始请求字符串，想发通知必须
 * 通过全局回调指针（`notificationState` + 互斥量）绕回 server。这里把
 * 「参数 + meta + 通知通道」直接作为上下文传入，工具不需要知道 server 的存在。
 */
struct ToolContext {
    json arguments = json::object();  ///< tools/call 的 params.arguments
    json meta = json::object();       ///< tools/call 的 params._meta（progressToken 在这里）
    std::function<void(const std::string& method, json params)> notify;  ///< 主动推送通知

    /// 读取 `_meta.progressToken`；不存在时返回 null。
    json progress_token() const {
        const auto it = meta.find("progressToken");
        return it == meta.end() ? json(nullptr) : *it;
    }
};

/// 工具定义。`handler` 抛出的异常由 server 统一转成 isError 结果。
struct Tool {
    std::string name;
    std::string description;
    json input_schema = json{{"type", "object"}};
    std::function<ToolResult(const ToolContext& context)> handler;

    json ToJson() const {
        json value = json::object();
        value["name"] = name;
        value["description"] = description;
        value["inputSchema"] = input_schema;
        return value;
    }
};

/// 提示参数声明。
struct PromptArgument {
    std::string name;
    std::string description;
    bool required = false;

    json ToJson() const {
        json value = json::object();
        value["name"] = name;
        value["description"] = description;
        value["required"] = required;
        return value;
    }
};

/// 提示定义。`handler` 返回 MCP `prompts/get` 的完整 result（含 messages）。
struct Prompt {
    std::string name;
    std::string description;
    std::vector<PromptArgument> arguments;
    std::function<json(const json& arguments)> handler;

    json ToJson() const {
        json value = json::object();
        value["name"] = name;
        value["description"] = description;
        value["arguments"] = json::array();
        for (const auto& argument : arguments) {
            value["arguments"].push_back(argument.ToJson());
        }
        return value;
    }
};

/// 资源定义。`reader` 返回 `contents` 数组（含 uri/mimeType/text）。
struct Resource {
    std::string uri;
    std::string name;
    std::string description;
    std::string mime_type = "text/plain";
    std::function<json(const json& arguments)> reader;

    json ToJson() const {
        json value = json::object();
        value["uri"] = uri;
        value["name"] = name;
        value["description"] = description;
        value["mimeType"] = mime_type;
        return value;
    }
};

}  // namespace go2cpp::mcp
