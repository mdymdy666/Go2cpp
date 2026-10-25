/**
 * @file notify_plugin.cpp
 * @brief 测试用 .so 插件：覆盖「插件主动通知」的正常与异常路径。
 *
 * @details
 * 编译成 MODULE 库，由 `test_plugin_notify.cpp`（C++ 单测）与
 * `plugin_notify_e2e.py`（stdio/HTTP 端到端）加载。行为：
 *
 * | 工具 | 行为 | 宿主期望 |
 * | --- | --- | --- |
 * | `notify_test` | 发 `notifications/message` + `notifications/progress` | 两条都转发 |
 * | `notify_bad_json` | 发非法 JSON | 拒绝（rejected++） |
 * | `notify_forbidden` | 用 `tools/list` 冒充请求 | 拒绝（rejected++） |
 * | `notify_worker` | 在 std::thread 里发一条 message | 转发（验证线程安全） |
 *
 * `Shutdown()` 里还会发一条消息：宿主卸载时先 Detach，这条应计入 dropped
 * 而不是写给客户端——这是「卸载后不回调」的可观测证据。
 *
 * 注意：这是**独立编译的插件**，只包含 `plugin_api.h` 与 nlohmann/json，
 * 不链接宿主库；与真实第三方插件的构建方式一致。
 */

#include <cstring>
#include <string>
#include <thread>

#include "mcp/plugin/plugin_api.h"
#include "nlohmann/json.hpp"

namespace {

using json = nlohmann::json;

PluginAPI* g_plugin = nullptr;

const char* PluginName() { return "fake-notify"; }

/// 通过宿主注入的通道发通知。插件必须自己判空：宿主可以不提供通知能力。
void SendRaw(const std::string& payload) {
    if (g_plugin == nullptr || g_plugin->notifications == nullptr ||
        g_plugin->notifications->SendToClient == nullptr) {
        return;
    }
    g_plugin->notifications->SendToClient(PluginName(), payload.c_str());
}

void Send(const json& payload) { SendRaw(payload.dump()); }

json TextResult(const std::string& text) {
    json result = json::object();
    result["content"] = json::array({json{{"type", "text"}, {"text", text}}});
    result["isError"] = false;
    return result;
}

/// 宿主按 `delete[]` 释放返回值，所以必须用 new[] 分配。
char* CopyText(const std::string& text) {
    char* buffer = new char[text.size() + 1];
    std::memcpy(buffer, text.data(), text.size());
    buffer[text.size()] = '\0';
    return buffer;
}

const char* GetNameImpl() { return PluginName(); }
const char* GetVersionImpl() { return "1.0.0"; }
PluginType GetTypeImpl() { return PLUGIN_TYPE_TOOLS; }
int InitializeImpl() { return 1; }

char* HandleRequestImpl(const char* request_text) {
    const json request = json::parse(request_text);
    const std::string tool = request.at("params").value("name", "");

    if (tool == "notify_test") {
        Send(json{{"jsonrpc", "2.0"},
                  {"method", "notifications/message"},
                  {"params",
                   {{"level", "notice"}, {"logger", "fake-notify"}, {"data", "plugin says hi"}}}});
        Send(json{{"jsonrpc", "2.0"},
                  {"method", "notifications/progress"},
                  {"params",
                   {{"progressToken", "tok-1"}, {"progress", 1}, {"total", 2}, {"message", "half"}}}});
        return CopyText(TextResult("notify_test ok: 2 notifications").dump());
    }
    if (tool == "notify_bad_json") {
        SendRaw("{not json");
        return CopyText(TextResult("notify_bad_json ok").dump());
    }
    if (tool == "notify_forbidden") {
        // 插件试图发一个请求（tools/list）：宿主必须拒绝，不能污染协议状态机。
        Send(json{{"jsonrpc", "2.0"}, {"method", "tools/list"}, {"params", json::object()}});
        return CopyText(TextResult("notify_forbidden ok").dump());
    }
    if (tool == "notify_worker") {
        std::thread worker([] {
            Send(json{{"jsonrpc", "2.0"},
                      {"method", "notifications/message"},
                      {"params", {{"level", "info"}, {"data", "from worker thread"}}}});
        });
        worker.join();
        return CopyText(TextResult("notify_worker ok: 1 notification").dump());
    }
    return CopyText(TextResult("unknown tool: " + tool).dump());
}

void ShutdownImpl() {
    // 卸载时宿主已 Detach：这条通知应被丢弃并计数，不得写给客户端。
    Send(json{{"jsonrpc", "2.0"},
              {"method", "notifications/message"},
              {"params", {{"level", "notice"}, {"data", "shutdown notification"}}}});
}

const PluginTool kTools[] = {
    {"notify_test", "发一条 message 与一条 progress 通知", R"({"type":"object","properties":{}})"},
    {"notify_bad_json", "发一段非法 JSON（宿主应拒绝）", R"({"type":"object","properties":{}})"},
    {"notify_forbidden", "尝试发 tools/list 请求（宿主应拒绝）",
     R"({"type":"object","properties":{}})"},
    {"notify_worker", "从工作线程发一条通知", R"({"type":"object","properties":{}})"},
};

int GetToolCountImpl() { return static_cast<int>(sizeof(kTools) / sizeof(kTools[0])); }

const PluginTool* GetToolImpl(int index) {
    if (index < 0 || index >= GetToolCountImpl()) {
        return nullptr;
    }
    return &kTools[index];
}

PluginAPI g_api = {
    GetNameImpl,
    GetVersionImpl,
    GetTypeImpl,
    InitializeImpl,
    HandleRequestImpl,
    ShutdownImpl,
    GetToolCountImpl,
    GetToolImpl,
    nullptr,  // GetPromptCount
    nullptr,  // GetPrompt
    nullptr,  // GetResourceCount
    nullptr,  // GetResource
    nullptr,  // notifications（宿主注入）
};

}  // namespace

extern "C" MCP_PLUGIN_API PluginAPI* CreatePlugin() {
    g_plugin = &g_api;
    return &g_api;
}

extern "C" MCP_PLUGIN_API void DestroyPlugin(PluginAPI*) {
    g_plugin = nullptr;
}
