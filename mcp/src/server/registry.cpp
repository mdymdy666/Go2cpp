/**
 * @file registry.cpp
 * @brief 工具 / 提示 / 资源注册表实现。
 */

#include "mcp/server/registry.hpp"

#include <algorithm>
#include <utility>

namespace go2cpp::mcp {

Registry::Registry(Registry&& other) noexcept {
    std::lock_guard<std::mutex> lock(other.m_mutex);
    m_tools = std::move(other.m_tools);
    m_prompts = std::move(other.m_prompts);
    m_resources = std::move(other.m_resources);
    m_tool_listeners = std::move(other.m_tool_listeners);
    m_next_listener_id = other.m_next_listener_id;
}

Registry& Registry::operator=(Registry&& other) noexcept {
    if (this != &other) {
        std::lock_guard<std::mutex> lock(m_mutex);
        std::lock_guard<std::mutex> other_lock(other.m_mutex);
        m_tools = std::move(other.m_tools);
        m_prompts = std::move(other.m_prompts);
        m_resources = std::move(other.m_resources);
        m_tool_listeners = std::move(other.m_tool_listeners);
        m_next_listener_id = other.m_next_listener_id;
    }
    return *this;
}

namespace {

bool UpsertTool(std::vector<Tool>& items, Tool item) {    const auto it = std::find_if(items.begin(), items.end(),
                                 [&](const Tool& existing) { return existing.name == item.name; });
    if (it == items.end()) {
        items.push_back(std::move(item));
        return false;
    }
    *it = std::move(item);
    return true;
}

bool UpsertPrompt(std::vector<Prompt>& items, Prompt item) {
    const auto it = std::find_if(items.begin(), items.end(),
                                 [&](const Prompt& existing) { return existing.name == item.name; });
    if (it == items.end()) {
        items.push_back(std::move(item));
        return false;
    }
    *it = std::move(item);
    return true;
}

bool UpsertResource(std::vector<Resource>& items, Resource item) {
    const auto it = std::find_if(items.begin(), items.end(),
                                 [&](const Resource& existing) { return existing.uri == item.uri; });
    if (it == items.end()) {
        items.push_back(std::move(item));
        return false;
    }
    *it = std::move(item);
    return true;
}

}  // namespace

void Registry::RegisterTool(Tool tool) {
    // 先留一份用于通知，再 move 进注册表。拷贝 std::function 只是引用计数，
    // 不会复制 handler 的捕获状态。
    Tool notification = tool;
    std::vector<ToolChangeListener> listeners;
    ToolChangeKind kind = ToolChangeKind::kAdded;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        kind = UpsertTool(m_tools, std::move(tool)) ? ToolChangeKind::kUpdated
                                                    : ToolChangeKind::kAdded;
        listeners = ListenerSnapshotLocked();
    }
    // 锁外通知：监听器（检索层增量索引）可以安全地回头读注册表或再次注册。
    for (const auto& listener : listeners) {
        listener(kind, notification);
    }
}

void Registry::RegisterPrompt(Prompt prompt) {
    std::lock_guard<std::mutex> lock(m_mutex);
    UpsertPrompt(m_prompts, std::move(prompt));
}

void Registry::RegisterResource(Resource resource) {
    std::lock_guard<std::mutex> lock(m_mutex);
    UpsertResource(m_resources, std::move(resource));
}

bool Registry::UnregisterTool(const std::string& name) {
    std::optional<Tool> removed;
    std::vector<ToolChangeListener> listeners;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        const auto it = std::find_if(m_tools.begin(), m_tools.end(),
                                     [&](const Tool& tool) { return tool.name == name; });
        if (it == m_tools.end()) {
            return false;
        }
        removed = std::move(*it);
        m_tools.erase(it);
        listeners = ListenerSnapshotLocked();
    }
    // 与注册一致：锁外通知，监听器里可以安全地查询/修改注册表。
    for (const auto& listener : listeners) {
        listener(ToolChangeKind::kRemoved, *removed);
    }
    return true;
}

std::size_t Registry::AddToolListener(ToolChangeListener listener) {
    std::lock_guard<std::mutex> lock(m_mutex);
    const std::size_t id = m_next_listener_id++;
    m_tool_listeners.emplace(id, std::move(listener));
    return id;
}

bool Registry::RemoveToolListener(std::size_t id) {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_tool_listeners.erase(id) > 0;
}

std::size_t Registry::tool_listener_count() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_tool_listeners.size();
}

std::vector<ToolChangeListener> Registry::ListenerSnapshotLocked() const {
    std::vector<ToolChangeListener> snapshot;
    snapshot.reserve(m_tool_listeners.size());
    for (const auto& item : m_tool_listeners) {
        snapshot.push_back(item.second);
    }
    return snapshot;
}

json Registry::ToolList() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    json result = json::object();
    result["tools"] = json::array();
    for (const auto& tool : m_tools) {
        result["tools"].push_back(tool.ToJson());
    }
    return result;
}

json Registry::PromptList() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    json result = json::object();
    result["prompts"] = json::array();
    for (const auto& prompt : m_prompts) {
        result["prompts"].push_back(prompt.ToJson());
    }
    return result;
}

json Registry::ResourceList() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    json result = json::object();
    result["resources"] = json::array();
    for (const auto& resource : m_resources) {
        result["resources"].push_back(resource.ToJson());
    }
    return result;
}

std::optional<json> Registry::ToolSchema(const std::string& name) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    const auto it = std::find_if(m_tools.begin(), m_tools.end(),
                                 [&](const Tool& tool) { return tool.name == name; });
    if (it == m_tools.end()) {
        return std::nullopt;
    }
    return it->input_schema;
}

std::optional<ToolResult> Registry::InvokeTool(const std::string& name, const ToolContext& context) const {
    std::function<ToolResult(const ToolContext&)> handler;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        const auto it = std::find_if(m_tools.begin(), m_tools.end(),
                                     [&](const Tool& tool) { return tool.name == name; });
        if (it == m_tools.end()) {
            return std::nullopt;
        }
        handler = it->handler;
    }
    // 锁外执行：工具实现里可以安全地再次注册/查询。
    return handler(context);
}

std::optional<json> Registry::GetPrompt(const std::string& name, const json& arguments) const {
    std::function<json(const json&)> handler;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        const auto it = std::find_if(m_prompts.begin(), m_prompts.end(),
                                     [&](const Prompt& prompt) { return prompt.name == name; });
        if (it == m_prompts.end()) {
            return std::nullopt;
        }
        handler = it->handler;
    }
    return handler(arguments);
}

std::optional<json> Registry::ReadResource(const std::string& uri) const {
    std::function<json(const json&)> reader;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        const auto it = std::find_if(m_resources.begin(), m_resources.end(),
                                     [&](const Resource& resource) { return resource.uri == uri; });
        if (it == m_resources.end()) {
            return std::nullopt;
        }
        reader = it->reader;
    }
    return reader(json::object());
}

std::size_t Registry::tool_count() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_tools.size();
}

std::size_t Registry::prompt_count() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_prompts.size();
}

std::size_t Registry::resource_count() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_resources.size();
}

}  // namespace go2cpp::mcp
