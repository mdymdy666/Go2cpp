/**
 * @file vector_index.hpp
 * @brief 向量层（二）：暴力最近邻索引与检索命中结构。
 *
 * @details
 * 规模预期是「几十到几千条」（工具数、文档块数），暴力余弦扫描比引入
 * HNSW/FAISS 更合适：零依赖、可预测、易调试。索引只做三件事：
 * 增/删、按余弦取 top-k、序列化到文本以便重启复用。
 *
 * 线程安全：内部 `shared_mutex`，读（Search）并发、写（Add/Remove）独占。
 * 向量本身按值拷贝，调用方改自己的副本不影响索引。
 */

#pragma once

#include <cstddef>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "mcp/vector/embedding.hpp"

namespace go2cpp::mcp::vector {

/// 一条检索命中。
struct SearchHit {
    std::string id;    ///< 条目 id（工具名 / 文档块 id）
    float score = 0;   ///< 余弦相似度，越大越相关
};

/**
 * @brief 暴力余弦索引。
 *
 * @details 序列化格式（UTF-8 文本，便于 diff 与人工检查）：
 * ```
 * MCPVEC1 <count>
 * <id>\t<v1>,<v2>,...
 * ```
 * id 中的 `\t`/换行会被拒绝，避免解析歧义。
 */
class VectorIndex {
public:
    /// 插入或覆盖同 id 的向量。
    void Add(const std::string& id, Embedding embedding);

    /// 删除；返回是否真的删掉了。
    bool Remove(const std::string& id);

    bool Contains(const std::string& id) const;

    /// 按 id 取向量副本；不存在返回 false（文档增量更新时复用未变化块的向量）。
    bool Get(const std::string& id, Embedding& out) const;
    std::size_t size() const;
    void Clear();

    /// 查询维度（第一条插入的向量维度）；空索引返回 0。
    std::size_t Dimension() const;

    /**
     * @brief 取与 query 最相似的前 top_k 条。
     * @param min_score 低于该分数的条目被丢弃（默认 -1，即不过滤）。
     * @return 按 score 降序排列；维度不一致的条目被跳过。
     */
    std::vector<SearchHit> Search(const Embedding& query, std::size_t top_k,
                                  float min_score = -1.0f) const;

    /// 导出到文件（临时文件 + rename，避免半截文件）。
    bool Save(const std::string& path, std::string* error = nullptr) const;

    /// 从 `Save` 的格式导入；失败时不修改现有内容。
    bool Load(const std::string& path, std::string* error = nullptr);

    /// 全部 id（调试/统计用）。
    std::vector<std::string> Ids() const;

private:
    mutable std::shared_mutex m_mutex;
    std::unordered_map<std::string, Embedding> m_entries;
};

}  // namespace go2cpp::mcp::vector
