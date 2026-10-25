/**
 * @file tool_retriever.hpp
 * @brief 检索层（一）：按语义从注册表里挑工具（RAG 选工具）。
 *
 * @details
 * 工具多了以后，「把 22 个工具的 schema 全塞给模型」既费 token 又容易选错。
 * `ToolRetriever` 把每个工具的「名字 + 描述」编码成向量，按用户查询取 top-k，
 * 再只把这几条工具描述交给模型。
 *
 * 与参考项目的差别：
 * - 参考项目把 embedding 调用、缓存、验证全塞进 `ToolRetriever`；
 *   这里检索层只依赖 `vector::IEmbeddingProvider` 与 `VectorIndex`，可替换；
 * - **增量更新**：构造时订阅注册表的工具变更事件（默认开启），注册 → 新增一条
 *   向量，覆盖 → 只重算发生变化的那一条，注销 → 删除条目。应用层注册工具后
 *   不需要再调用 `Refresh()`；`Refresh()` 只作为兜底（换 provider/维度、
 *   怀疑索引不一致时使用）；
 * - 覆盖时若检索文本（名字 + 描述）没变，只换 schema/handler，**不重算向量**；
 * - 检索失败不抛异常：`LastError()` 记录原因，调用方回退到全量工具列表。
 *
 * 线程安全：`m_mutex` 保护定义表与索引写入，`VectorIndex` 内部读写锁承担并发
 * 检索；锁顺序固定为 `m_mutex → VectorIndex::m_mutex`（所有路径一致，不存在
 * 反向等待）。注册表事件在注册表锁外投递，因此不会出现
 * 「注册表锁 → 检索锁」与「检索锁 → 注册表锁」互等的死锁。
 */

#pragma once

#include <cstddef>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "mcp/server/registry.hpp"
#include "mcp/vector/embedding.hpp"
#include "mcp/vector/vector_index.hpp"

namespace go2cpp::mcp::retrieval {

/// 检索参数。
struct RetrieveOptions {
    std::size_t top_k = 5;        ///< 最多返回几条
    float min_score = 0.05f;      ///< 低于该余弦分数不返回
};

/// 一条检索结果。
struct RetrievedItem {
    std::string id;    ///< 工具名
    float score = 0;   ///< 相似度
};

/// 增量维护统计（诊断与一致性测试用）。
struct ToolRetrieverStats {
    std::size_t indexed = 0;   ///< 当前索引条目数
    std::size_t embedded = 0;  ///< 因新增或描述变化而重算的向量数
    std::size_t reused = 0;    ///< 覆盖时文本未变、复用旧向量的次数
    std::size_t removed = 0;   ///< 增量删除次数
    std::size_t rebuilds = 0;  ///< 成功的全量重建次数
    std::size_t failures = 0;  ///< 增量更新失败次数（embedding 失败等）
};

/**
 * @brief 工具检索器。
 *
 * 生命周期：持有 `Registry&` 与 `IEmbeddingProvider&`，两者都必须比本对象活得久。
 * 构造时若 `subscribe=true` 会注册监听器，析构时自动退订；因此本对象不可拷贝、
 * 不可移动（mutex 与监听器身份都依赖地址稳定）。
 */
class ToolRetriever {
public:
    /**
     * @param registry 工具注册表（不持有所有权）
     * @param provider 向量编码器（不持有所有权）
     * @param options 检索参数
     * @param subscribe true = 订阅注册表事件做增量索引；false = 首次检索时懒构建，
     *                  之后只能手工调用 AddTool/UpdateTool/RemoveTool/Refresh
     */
    ToolRetriever(Registry& registry, vector::IEmbeddingProvider& provider,
                  RetrieveOptions options = {}, bool subscribe = true);
    ~ToolRetriever();

    ToolRetriever(const ToolRetriever&) = delete;
    ToolRetriever& operator=(const ToolRetriever&) = delete;
    ToolRetriever(ToolRetriever&&) = delete;
    ToolRetriever& operator=(ToolRetriever&&) = delete;

    /// 按查询取最相关的工具（使用构造时的 top_k）。
    std::vector<RetrievedItem> Retrieve(const std::string& query);

    /// 按查询取最相关的工具，本次调用覆盖 top_k。
    std::vector<RetrievedItem> Retrieve(const std::string& query, std::size_t top_k);

    /**
     * @brief 生成给模型看的工具摘要（JSON 数组），只含 top-k 条。
     * @details 元素形如 `{"name":..., "description":..., "inputSchema":...}`，
     *          与 OpenAI tools 的 `function` 字段结构一致，便于直接复用。
     */
    json ContextFor(const std::string& query);

    // ------------------------------------------------------------------
    // 增量维护：注册表事件会自动调用；也可由调用方手工驱动（subscribe=false）
    // ------------------------------------------------------------------

    /// 新增或覆盖一个工具：只对这一个工具计算向量。
    bool AddTool(const Tool& tool);

    /**
     * @brief 覆盖更新一个工具。
     * @details 检索文本（名字 + 描述）未变时复用旧向量，只刷新 schema；
     *          文本变化时退化为 `AddTool`（只重算这一条）。
     */
    bool UpdateTool(const Tool& tool);

    /// 从索引移除工具；返回是否真的删掉了条目。
    bool RemoveTool(const std::string& name);

    /// 全量重建（兜底）。会重新计算所有工具向量，返回是否成功。
    bool Refresh();

    std::size_t IndexedTools() const;

    /// 当前索引里的工具名（已排序，便于一致性断言）。
    std::vector<std::string> IndexedIds() const;

    ToolRetrieverStats Stats() const;

    /// 是否已订阅注册表事件。
    bool subscribed() const;

    /// 最近一次失败原因；成功时为 "ok"。
    std::string LastError() const;

    const vector::IEmbeddingProvider& provider() const { return m_provider; }

private:
    /// 统一的检索文本构造：增量与全量必须一致，否则两边结果会漂移。
    static std::string BuildDocument(const Tool& tool);

    /// 把注册表里的工具全量转成索引（调用方必须持有 m_mutex）。
    bool RebuildLocked();

    /// 注册表事件回调（在注册表锁外执行）。
    void OnToolChanged(ToolChangeKind kind, const Tool& tool);

    Registry& m_registry;
    vector::IEmbeddingProvider& m_provider;
    RetrieveOptions m_options;

    mutable std::mutex m_mutex;
    vector::VectorIndex m_index;
    std::unordered_map<std::string, std::string> m_documents;  ///< 工具名 -> 检索文本
    json m_tool_schemas = json::object();                      ///< 工具名 -> 完整定义
    bool m_built = false;
    bool m_subscribed = false;
    std::size_t m_listener_id = 0;
    std::string m_last_error = "ok";
    std::size_t m_embedded = 0;
    std::size_t m_reused = 0;
    std::size_t m_removed = 0;
    std::size_t m_rebuilds = 0;
    std::size_t m_failures = 0;
};

}  // namespace go2cpp::mcp::retrieval
