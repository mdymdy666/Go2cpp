/**
 * @file document_store.hpp
 * @brief 检索层（二）：文本分块 + 向量化 + 片段召回（知识检索工具的数据底座）。
 *
 * @details
 * 资源文件（resources 目录下的 .txt/.md）与外部文档需要在「不引入数据库」的前提下做语义
 * 检索：这里按字符数切块（尽量在换行/空白处断开），每块一个向量，检索时返回
 * 原文片段。规模上限在几千块，暴力余弦足够。
 *
 * 增量语义：`AddDocument` 按 uri 整体替换旧块，但**按块文本复用未变化块的向量**，
 * 只对新增/变化的块调用 embedding——重复加载内容完全相同的文件时零向量化开销。
 * `RemoveDocument` 直接删除该 uri 的全部块与向量（容器 erase，没有墓碑/压缩步骤）。
 * 并发写同一 uri 的语义是 last-writer-wins：分块与向量化在锁外，提交时整体替换。
 */

#pragma once

#include <cstddef>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "mcp/vector/embedding.hpp"
#include "mcp/vector/vector_index.hpp"

namespace go2cpp::mcp::retrieval {

/// 一个文档片段。
struct DocumentChunk {
    std::string id;    ///< 全局唯一：`<uri>#<序号>`
    std::string uri;   ///< 来源 URI
    std::string text;  ///< 片段原文
};

/// 分块参数。
struct ChunkOptions {
    std::size_t chunk_size = 400;  ///< 每块目标字符数（UTF-8 字节数近似）
    std::size_t overlap = 80;      ///< 相邻块重叠字符数，避免切断上下文
};

/**
 * @brief 内存文档库。
 *
 * 线程安全：`AddDocument` / `RemoveDocument` / `Search` / `Get` / `Clear` 内部加锁；
 * 返回的片段是拷贝。分块与 embedding 在锁外完成，不阻塞并发检索。
 */
class DocumentStore {
public:
    /// 增量维护统计（诊断与测试用）。
    struct Stats {
        std::size_t documents = 0;  ///< 文档数（不同 uri）
        std::size_t chunks = 0;     ///< 当前块数
        std::size_t embedded = 0;   ///< 累计计算的块向量数
        std::size_t reused = 0;     ///< 累计复用的块向量数（块内容未变）
    };

    DocumentStore(vector::IEmbeddingProvider& provider, ChunkOptions options = {});

    /**
     * @brief 加入或更新一篇文档（会分块，只对变化块向量化）。
     * @details 同一 uri 再次调用等价于更新：旧块整体替换；内容未变的块复用旧向量。
     *          空文本表示清空该文档（删除旧块）。
     * @return 成功返回 true；embedding 失败返回 false 并写 error，现有内容不变。
     */
    bool AddDocument(const std::string& uri, const std::string& text, std::string& error);

    /// 删除某 uri 的全部块；返回是否真的删掉了内容。
    bool RemoveDocument(const std::string& uri);

    /// 递归加载目录下的 `*.txt` / `*.md`；返回成功入库的文件数。
    std::size_t AddDirectory(const std::string& directory, std::string& error);

    /// 检索最相关的片段。
    std::vector<vector::SearchHit> Search(const std::string& query, std::size_t top_k,
                                          float min_score = 0.05f);

    /// 按 id 取片段（检索结果 id 的二次查询）。
    std::optional<DocumentChunk> Get(const std::string& id) const;

    std::size_t size() const;
    std::size_t document_count() const;
    Stats Snapshot() const;
    void Clear();

    /// 最近一次失败原因；成功时为 "ok"。
    std::string LastError() const;

private:
    /// 删除某 uri 的全部块与向量（调用方必须持有 m_mutex）。
    void EraseUriLocked(const std::string& uri);

    vector::IEmbeddingProvider& m_provider;
    ChunkOptions m_options;

    mutable std::mutex m_mutex;
    vector::VectorIndex m_index;
    std::unordered_map<std::string, DocumentChunk> m_chunks;
    std::unordered_set<std::string> m_uris;
    std::size_t m_embedded = 0;
    std::size_t m_reused = 0;
    std::string m_last_error = "ok";
};

}  // namespace go2cpp::mcp::retrieval
