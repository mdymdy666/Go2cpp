/**
 * @file tool_retriever.cpp
 * @brief 工具检索实现：注册表事件驱动的增量索引 + top-k 查询。
 */

#include "mcp/retrieval/tool_retriever.hpp"

#include <algorithm>
#include <exception>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace go2cpp::mcp::retrieval {

ToolRetriever::ToolRetriever(Registry& registry, vector::IEmbeddingProvider& provider,
                             RetrieveOptions options, bool subscribe)
    : m_registry(registry), m_provider(provider), m_options(options) {
    if (subscribe) {
        // 先订阅、再做首次对齐：构造期间发生的注册不会漏掉。监听器在注册表锁外
        // 投递，与下方的首次对齐用同一把 m_mutex 串行，因此不会看到半套索引。
        m_listener_id = m_registry.AddToolListener(
            [this](ToolChangeKind kind, const Tool& tool) { OnToolChanged(kind, tool); });
        m_subscribed = true;
    }
    if (m_subscribed) {
        // 把构造前已注册的工具纳入索引（一次性对齐）；空注册表时是 O(0)。
        // 失败不阻断构造：m_built 保持 false，首次检索会重试全量构建。
        std::lock_guard<std::mutex> lock(m_mutex);
        (void)RebuildLocked();
    }
}

ToolRetriever::~ToolRetriever() {
    // 退订后不再收到事件；调用方需保证此后的注册不会并发引用本对象
    // （正常生命周期里检索器与注册表同属 Application，服务停止后才析构）。
    if (m_subscribed) {
        m_registry.RemoveToolListener(m_listener_id);
    }
}

std::string ToolRetriever::BuildDocument(const Tool& tool) {
    // 名字命中最直接，描述补充语义；增量与全量共用这一构造，保证结果一致。
    return tool.name + "\n" + tool.description;
}

bool ToolRetriever::RebuildLocked() {
    json document = m_registry.ToolList();
    const auto tools = document.find("tools");
    if (tools == document.end() || !tools->is_array()) {
        m_last_error = "tools/list 返回结构异常";
        return false;
    }

    // 先在局部容器里构建，全部成功后再整体替换，避免半套索引对外可见。
    // 不能直接 move 整个 VectorIndex：它含 shared_mutex，不可移动。
    std::vector<std::pair<std::string, vector::Embedding>> rebuilt;
    std::unordered_map<std::string, std::string> documents;
    json schemas = json::object();

    for (const auto& tool : *tools) {
        if (!tool.is_object()) {
            continue;
        }
        const std::string name = tool.value("name", "");
        if (name.empty()) {
            continue;
        }
        const std::string description = tool.value("description", "");
        const std::string text = name + "\n" + description;
        vector::Embedding embedding;
        std::string error;
        if (!m_provider.Embed(text, embedding, error)) {
            m_last_error = "工具 " + name + " 向量化失败: " + error;
            return false;
        }
        rebuilt.emplace_back(name, std::move(embedding));
        documents.emplace(name, text);
        schemas[name] = tool;
    }

    m_index.Clear();
    for (auto& [id, embedding] : rebuilt) {
        m_index.Add(id, std::move(embedding));
    }
    m_documents = std::move(documents);
    m_tool_schemas = std::move(schemas);
    m_built = true;
    m_last_error = "ok";
    ++m_rebuilds;
    return true;
}

bool ToolRetriever::AddTool(const Tool& tool) {
    if (tool.name.empty()) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_last_error = "工具名为空，无法写入索引";
        ++m_failures;
        return false;
    }

    // embedding 在锁外算：远程 provider 可能很慢，不能挡住并发检索。
    const std::string text = BuildDocument(tool);
    vector::Embedding embedding;
    std::string error;
    if (!m_provider.Embed(text, embedding, error)) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_last_error = "工具 " + tool.name + " 向量化失败: " + error;
        ++m_failures;
        return false;
    }

    const json schema = tool.ToJson();
    std::lock_guard<std::mutex> lock(m_mutex);
    m_index.Add(tool.name, std::move(embedding));
    m_documents[tool.name] = text;
    m_tool_schemas[tool.name] = schema;
    m_built = true;
    m_last_error = "ok";
    ++m_embedded;
    return true;
}

bool ToolRetriever::UpdateTool(const Tool& tool) {
    if (tool.name.empty()) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_last_error = "工具名为空，无法更新索引";
        ++m_failures;
        return false;
    }

    const std::string text = BuildDocument(tool);
    const json schema = tool.ToJson();
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        const auto found = m_documents.find(tool.name);
        if (found != m_documents.end() && found->second == text &&
            m_index.Contains(tool.name)) {
            // 检索文本没变（只换了 handler 或 schema）：复用旧向量，跳过 embedding。
            m_tool_schemas[tool.name] = schema;
            m_last_error = "ok";
            ++m_reused;
            return true;
        }
    }
    return AddTool(tool);
}

bool ToolRetriever::RemoveTool(const std::string& name) {
    if (name.empty()) {
        return false;
    }
    std::lock_guard<std::mutex> lock(m_mutex);
    const bool indexed = m_index.Remove(name);
    const bool documented = m_documents.erase(name) > 0;
    m_tool_schemas.erase(name);
    if (indexed || documented) {
        ++m_removed;
        m_last_error = "ok";
        return true;
    }
    return false;
}

bool ToolRetriever::Refresh() {
    std::lock_guard<std::mutex> lock(m_mutex);
    return RebuildLocked();
}

void ToolRetriever::OnToolChanged(ToolChangeKind kind, const Tool& tool) {
    // 监听器运行在注册表调用方的线程上：这里不能把异常抛回去，否则一次工具注册
    // 会因为向量化失败而破坏注册表状态。
    try {
        switch (kind) {
            case ToolChangeKind::kAdded:
                (void)AddTool(tool);
                break;
            case ToolChangeKind::kUpdated:
                (void)UpdateTool(tool);
                break;
            case ToolChangeKind::kRemoved:
                (void)RemoveTool(tool.name);
                break;
        }
    } catch (const std::exception& exception) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_last_error = std::string("增量索引异常: ") + exception.what();
        ++m_failures;
    } catch (...) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_last_error = "增量索引异常（未知类型）";
        ++m_failures;
    }
}

std::vector<RetrievedItem> ToolRetriever::Retrieve(const std::string& query) {
    return Retrieve(query, m_options.top_k);
}

std::vector<RetrievedItem> ToolRetriever::Retrieve(const std::string& query, std::size_t top_k) {
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_built && !RebuildLocked()) {
            return {};
        }
    }

    vector::Embedding embedding;
    std::string error;
    if (!m_provider.Embed(query, embedding, error)) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_last_error = "查询向量化失败: " + error;
        return {};
    }

    const auto hits = m_index.Search(embedding, top_k, m_options.min_score);
    std::vector<RetrievedItem> items;
    items.reserve(hits.size());
    for (const auto& hit : hits) {
        items.push_back(RetrievedItem{hit.id, hit.score});
    }
    return items;
}

json ToolRetriever::ContextFor(const std::string& query) {
    const auto items = Retrieve(query);
    json context = json::array();
    // 检索与组装之间可能有注销/覆盖：在锁内取最新定义，找不到就跳过，
    // 不把已失效的工具交给模型。
    std::lock_guard<std::mutex> lock(m_mutex);
    for (const auto& item : items) {
        const auto found = m_tool_schemas.find(item.id);
        if (found == m_tool_schemas.end()) {
            continue;
        }
        json entry = json::object();
        entry["name"] = item.id;
        entry["description"] = found->value("description", "");
        entry["inputSchema"] = found->value("inputSchema", json::object());
        entry["score"] = item.score;
        context.push_back(std::move(entry));
    }
    return context;
}

std::size_t ToolRetriever::IndexedTools() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_index.size();
}

std::vector<std::string> ToolRetriever::IndexedIds() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    std::vector<std::string> ids = m_index.Ids();
    std::sort(ids.begin(), ids.end());
    return ids;
}

ToolRetrieverStats ToolRetriever::Stats() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    ToolRetrieverStats stats;
    stats.indexed = m_index.size();
    stats.embedded = m_embedded;
    stats.reused = m_reused;
    stats.removed = m_removed;
    stats.rebuilds = m_rebuilds;
    stats.failures = m_failures;
    return stats;
}

bool ToolRetriever::subscribed() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_subscribed;
}

std::string ToolRetriever::LastError() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_last_error;
}

}  // namespace go2cpp::mcp::retrieval
