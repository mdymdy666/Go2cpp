/**
 * @file plugin_loader.cpp
 * @brief dlopen 插件加载与 C ABI → Registry 适配，以及插件主动通知的宿主注入。
 */

#include "plugin_loader.hpp"

#include <dlfcn.h>

#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <utility>

namespace go2cpp::mcp::plugin {

namespace {

/// 单条插件通知的上限。超过直接拒绝：MCP 通知属于控制面消息，1 MiB 已足够
/// 容纳日志/进度，也避免插件把传输队列写满。
constexpr std::size_t kMaxNotificationBytes = 1024 * 1024;

/// 插件只能发通知；请求与响应由客户端/宿主发起，插件越权一律拒绝。
constexpr std::string_view kNotificationPrefix = "notifications/";

/**
 * @brief 插件名 → 投递目标。
 *
 * C ABI 回调 `(pluginName, notification)` 没有 userdata，宿主无法把 loader 的
 * `this` 编进函数指针，只能按插件名反查。表项用 weak_ptr 指向 Hub：loader
 * 析构后 Hub 即便被测试持有，表项也已删除，延迟调用只会落到「查不到 → 丢弃」。
 */
struct NotificationTarget {
    std::weak_ptr<NotificationHub> hub;
    std::string path;  ///< 加载路径，用于同名插件注销时避免误删后加载者
};

struct TargetTable {
    std::mutex mutex;
    std::map<std::string, NotificationTarget> targets;
};

TargetTable& Targets() {
    static TargetTable table;
    return table;
}

void RegisterTarget(const std::string& name, const std::string& path,
                    const std::shared_ptr<NotificationHub>& hub) {
    std::lock_guard<std::mutex> lock(Targets().mutex);
    Targets().targets[name] = NotificationTarget{hub, path};
}

void UnregisterTarget(const std::string& name, const std::string& path) {
    std::lock_guard<std::mutex> lock(Targets().mutex);
    const auto it = Targets().targets.find(name);
    if (it != Targets().targets.end() && it->second.path == path) {
        Targets().targets.erase(it);
    }
}

/**
 * @brief C ABI 回调入口：插件 `SendToClient` 指向这里。
 *
 * 这个函数会被插件在任意线程调用，且跨 DLL 边界：**不允许异常逃逸**，
 * 否则插件侧栈展开行为未定义（插件可能没开异常）。
 */
void PluginNotificationTrampoline(const char* plugin_name, const char* notification) noexcept {
    try {
        if (plugin_name == nullptr || notification == nullptr) {
            return;
        }
        std::shared_ptr<NotificationHub> hub;
        {
            std::lock_guard<std::mutex> lock(Targets().mutex);
            const auto it = Targets().targets.find(plugin_name);
            if (it == Targets().targets.end()) {
                return;  // 已卸载：丢弃（不缓存，见 NotificationHub 文档）
            }
            hub = it->second.hub.lock();
        }
        if (!hub) {
            return;  // loader 已析构：丢弃
        }
        hub->Deliver(plugin_name, notification);
    } catch (...) {
        // 宿主内部错误也不能回抛给插件。
    }
}

/// 调插件 HandleRequest 并把返回的 JSON 解析成 ToolResult。
ToolResult InvokeToolPlugin(PluginAPI* api, const std::string& name, const json& arguments) {
    json request = json::object();
    request["jsonrpc"] = "2.0";
    request["id"] = 1;
    request["method"] = "tools/call";
    request["params"] = json{{"name", name}, {"arguments", arguments}};

    char* raw = api->HandleRequest(request.dump().c_str());
    if (raw == nullptr) {
        return ToolResult::Text("plugin returned null", true);
    }
    const std::string text(raw);
    delete[] raw;

    json parsed;
    try {
        parsed = json::parse(text);
    } catch (const json::exception&) {
        return ToolResult::Text("plugin returned malformed JSON: " + text.substr(0, 120), true);
    }

    ToolResult result;
    result.is_error = parsed.value("isError", false);
    if (parsed.contains("content") && parsed["content"].is_array()) {
        for (const auto& block : parsed["content"]) {
            if (block.is_object() && block.value("type", "") == "text") {
                result.content.push_back(Content{"text", block.value("text", "")});
            }
        }
    }
    if (result.content.empty()) {
        result.content.push_back(Content{"text", text});
    }
    return result;
}

/// 调插件 HandleRequest 处理 prompts/get 或 resources/read，返回完整 result。
json InvokeJsonPlugin(PluginAPI* api, const std::string& method, const json& params) {
    json request = json::object();
    request["jsonrpc"] = "2.0";
    request["id"] = 1;
    request["method"] = method;
    request["params"] = params;

    char* raw = api->HandleRequest(request.dump().c_str());
    if (raw == nullptr) {
        throw std::runtime_error("plugin returned null");
    }
    const std::string text(raw);
    delete[] raw;
    try {
        return json::parse(text);
    } catch (const json::exception& error) {
        throw std::runtime_error(std::string("plugin returned malformed JSON: ") + error.what());
    }
}

}  // namespace

// ---------------------------------------------------------------------------
// NotificationHub
// ---------------------------------------------------------------------------

NotificationHub::NotificationHub(Sink sink) : m_sink(std::move(sink)) {}

void NotificationHub::SetSink(Sink sink) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_sink = std::move(sink);
}

void NotificationHub::Detach() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_sink = nullptr;
}

void NotificationHub::Deliver(const std::string& plugin, std::string_view payload) noexcept {
    try {
        if (payload.empty() || payload.size() > kMaxNotificationBytes) {
            m_rejected.fetch_add(1, std::memory_order_relaxed);
            return;
        }

        json document = json::parse(payload.begin(), payload.end());
        if (!document.is_object()) {
            m_rejected.fetch_add(1, std::memory_order_relaxed);
            return;
        }

        // 默认按 MCP logging 通知处理：插件往往只传日志参数，没有 method 字段。
        std::string method = "notifications/message";
        json params = json::object();

        const auto method_it = document.find("method");
        if (method_it != document.end()) {
            if (!method_it->is_string()) {
                m_rejected.fetch_add(1, std::memory_order_relaxed);
                return;
            }
            const std::string candidate = method_it->get<std::string>();
            if (candidate.rfind(kNotificationPrefix, 0) != 0) {
                // 插件想发请求/响应/未知方法：一律拒绝，避免污染协议状态机。
                m_rejected.fetch_add(1, std::memory_order_relaxed);
                return;
            }
            method = candidate;
            const auto params_it = document.find("params");
            if (params_it != document.end() && !params_it->is_null()) {
                if (!params_it->is_object()) {
                    m_rejected.fetch_add(1, std::memory_order_relaxed);
                    return;
                }
                params = *params_it;
            }
        } else {
            params = document;
        }

        // 先取快照再调用：sink 内部会拿 Server 的锁，不能在持本锁时调用，
        // 否则 Detach 与传输写会互相等待。
        Sink sink;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            sink = m_sink;
        }
        if (!sink) {
            m_dropped.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        sink(plugin, method, params);
        m_forwarded.fetch_add(1, std::memory_order_relaxed);
    } catch (...) {
        // 解析/编码/sink 抛出的异常都算拒绝：通知链路不能影响插件执行。
        m_rejected.fetch_add(1, std::memory_order_relaxed);
    }
}

// ---------------------------------------------------------------------------
// PluginLoader
// ---------------------------------------------------------------------------

PluginLoader::PluginLoader() : m_hub(std::make_shared<NotificationHub>()) {}

PluginLoader::~PluginLoader() { UnloadAll(); }

void PluginLoader::SetNotificationSink(NotificationHub::Sink sink) {
    m_hub->SetSink(std::move(sink));
}

bool PluginLoader::LoadDirectory(const std::string& directory, Registry& registry,
                                 std::string& error) {
    std::error_code code;
    if (!std::filesystem::is_directory(directory, code)) {
        error = "插件目录不存在: " + directory;
        return false;
    }

    std::size_t loaded = 0;
    // 参考项目的产物布局是 plugins/<name>/lib<name>.so，所以递归遍历；
    // 同名插件后者覆盖前者（注册表 upsert 语义）。
    for (const auto& entry : std::filesystem::recursive_directory_iterator(directory, code)) {
        if (!entry.is_regular_file()) {
            continue;
        }
        if (entry.path().extension() != ".so") {
            continue;
        }
        std::string load_error;
        if (LoadFile(entry.path().string(), registry, load_error)) {
            ++loaded;
        } else if (!load_error.empty()) {
            error += load_error + "; ";
        }
    }
    return loaded > 0;
}

bool PluginLoader::LoadFile(const std::string& path, Registry& registry, std::string& error) {
    void* library = ::dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (library == nullptr) {
        const char* message = ::dlerror();
        error = path + ": " + (message != nullptr ? message : "dlopen failed");
        return false;
    }

    auto create = reinterpret_cast<PluginAPI* (*)()>(::dlsym(library, "CreatePlugin"));
    auto destroy = reinterpret_cast<void (*)(PluginAPI*)>(::dlsym(library, "DestroyPlugin"));
    if (create == nullptr || destroy == nullptr) {
        error = path + ": 缺少 CreatePlugin/DestroyPlugin 符号";
        ::dlclose(library);
        return false;
    }

    PluginAPI* api = create();
    if (api == nullptr || api->Initialize == nullptr) {
        error = path + ": CreatePlugin 返回空或缺少 Initialize";
        if (api != nullptr) {
            destroy(api);
        }
        ::dlclose(library);
        return false;
    }

    auto handle = std::make_unique<Handle>();
    handle->library = library;
    handle->api = api;
    handle->path = path;
    const char* declared = api->GetName != nullptr ? api->GetName() : nullptr;
    handle->name = (declared != nullptr && *declared != '\0') ? declared : path;

    // 注入先于 Initialize：插件若在初始化阶段就发通知（例如宣告就绪），
    // 宿主已经能接住。C ABI 结构体地址由 unique_ptr 保证稳定。
    handle->notifications.SendToClient = &PluginNotificationTrampoline;
    api->notifications = &handle->notifications;
    RegisterTarget(handle->name, handle->path, m_hub);

    if (api->Initialize() != 1) {
        UnregisterTarget(handle->name, handle->path);
        error = path + ": Initialize 失败";
        destroy(api);
        ::dlclose(library);
        return false;
    }

    const PluginType type = api->GetType != nullptr ? api->GetType() : PLUGIN_TYPE_TOOLS;

    if (type == PLUGIN_TYPE_TOOLS && api->GetToolCount != nullptr) {
        for (int index = 0; index < api->GetToolCount(); ++index) {
            const PluginTool* described = api->GetTool(index);
            if (described == nullptr || described->name == nullptr) {
                continue;
            }
            Tool tool;
            tool.name = described->name;
            tool.description = described->description != nullptr ? described->description : "";
            try {
                tool.input_schema = described->inputSchema != nullptr
                                        ? json::parse(described->inputSchema)
                                        : json{{"type", "object"}};
            } catch (const json::exception&) {
                tool.input_schema = json{{"type", "object"}};
            }
            const std::string name = tool.name;
            tool.handler = [api, name](const ToolContext& context) {
                return InvokeToolPlugin(api, name, context.arguments);
            };
            registry.RegisterTool(std::move(tool));
        }
    } else if (type == PLUGIN_TYPE_PROMPTS && api->GetPromptCount != nullptr) {
        for (int index = 0; index < api->GetPromptCount(); ++index) {
            const PluginPrompt* described = api->GetPrompt(index);
            if (described == nullptr || described->name == nullptr) {
                continue;
            }
            Prompt prompt;
            prompt.name = described->name;
            prompt.description = described->description != nullptr ? described->description : "";
            const std::string name = prompt.name;
            prompt.handler = [api, name](const json& arguments) {
                return InvokeJsonPlugin(api, "prompts/get",
                                        json{{"name", name}, {"arguments", arguments}});
            };
            registry.RegisterPrompt(std::move(prompt));
        }
    } else if (type == PLUGIN_TYPE_RESOURCES && api->GetResourceCount != nullptr) {
        for (int index = 0; index < api->GetResourceCount(); ++index) {
            const PluginResource* described = api->GetResource(index);
            if (described == nullptr || described->uri == nullptr) {
                continue;
            }
            Resource resource;
            resource.uri = described->uri;
            resource.name = described->name != nullptr ? described->name : resource.uri;
            resource.description = described->description != nullptr ? described->description : "";
            resource.mime_type = described->mime != nullptr ? described->mime : "text/plain";
            const std::string uri = resource.uri;
            resource.reader = [api, uri](const json&) {
                return InvokeJsonPlugin(api, "resources/read", json{{"uri", uri}});
            };
            registry.RegisterResource(std::move(resource));
        }
    } else {
        UnregisterTarget(handle->name, handle->path);
        error = path + ": 未知插件类型";
        destroy(api);
        ::dlclose(library);
        return false;
    }

    m_handles.push_back(std::move(handle));
    return true;
}

void PluginLoader::UnloadAll() {
    // 1) 先断开出口：Shutdown 期间插件的通知只累加 dropped 计数，
    //    不会再写客户端（服务可能已经停止，信号量/会话状态不再可靠）。
    if (m_hub) {
        m_hub->Detach();
    }

    for (auto& handle : m_handles) {
        if (handle->api != nullptr) {
            if (handle->api->Shutdown != nullptr) {
                handle->api->Shutdown();
            }
            // 2) 先注销投递表，再 Destroy/dlclose：DestroyPlugin 里若还有通知，
            //    查表失败直接丢弃，不会访问已释放的 loader 状态。
            UnregisterTarget(handle->name, handle->path);
            auto destroy =
                reinterpret_cast<void (*)(PluginAPI*)>(::dlsym(handle->library, "DestroyPlugin"));
            if (destroy != nullptr) {
                destroy(handle->api);
            }
        }
        if (handle->library != nullptr) {
            ::dlclose(handle->library);
        }
    }
    m_handles.clear();
}

}  // namespace go2cpp::mcp::plugin
