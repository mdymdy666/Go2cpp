/**
 * @file notification.cpp
 * @brief 通知类工具：logging_test（发 notifications/message）与 progress_test（发进度）。
 *
 * @details
 * MCP 的进度通知要求客户端在 `params._meta.progressToken` 里给出令牌，服务端
 * 在工具执行过程中主动推送 `notifications/progress`。参考项目把令牌从原始请求
 * 字符串里抠出来，再通过全局回调发通知；这里由 `ToolContext` 直接携带。
 */

#include "mcp/tools/builtin_tools.hpp"

#include <chrono>
#include <string>
#include <thread>

namespace go2cpp::mcp::tools {

namespace {

/// 从 context 取进度令牌；缺失时返回 null。
json ProgressToken(const ToolContext& context) {
    return context.progress_token();
}

void SendProgress(const ToolContext& context, const json& token, int progress, int total,
                  const std::string& message) {
    if (token.is_null() || !context.notify) {
        return;
    }
    json params = json::object();
    params["progressToken"] = token;
    params["progress"] = progress;
    params["total"] = total;
    if (!message.empty()) {
        params["message"] = message;
    }
    context.notify("notifications/progress", std::move(params));
}

}  // namespace

void RegisterNotificationTools(Registry& registry) {
    // logging_test：发一条 logging 通知后返回
    {
        Tool tool;
        tool.name = "logging_test";
        tool.description = "发送一条 MCP logging 通知并返回确认信息";
        tool.input_schema = json{{"type", "object"}, {"properties", json::object()}};
        tool.handler = [](const ToolContext& context) {
            if (context.notify) {
                json params = json::object();
                params["level"] = "info";
                params["logger"] = "notification-tools";
                params["data"] = "logging_test executed";
                context.notify("notifications/message", std::move(params));
            }
            return ToolResult::Text("logging_test: 已发送 notifications/message");
        };
        registry.RegisterTool(std::move(tool));
    }

    // progress_test：分 4 步推送进度
    {
        Tool tool;
        tool.name = "progress_test";
        tool.description =
            "分阶段推送进度通知（需要 tools/call 的 params._meta.progressToken）";
        tool.input_schema = json{{"type", "object"}, {"properties", json::object()}};
        tool.handler = [](const ToolContext& context) {
            const json token = ProgressToken(context);
            if (token.is_null()) {
                return ToolResult::Text(
                    "progress_test 需要在 tools/call 的 params._meta.progressToken 中提供令牌",
                    true);
            }
            constexpr int kTotal = 4;
            for (int step = 1; step <= kTotal; ++step) {
                SendProgress(context, token, step, kTotal,
                             "step " + std::to_string(step) + "/" + std::to_string(kTotal));
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
            return ToolResult::Text("progress_test: 已推送 " + std::to_string(kTotal) + " 条进度通知");
        };
        registry.RegisterTool(std::move(tool));
    }
}

}  // namespace go2cpp::mcp::tools
