/**
 * @file embedding.hpp
 * @brief 向量层（一）：文本到向量的编码接口与两个内置实现。
 *
 * @details
 * 参考项目把 embedding 调用直接写进 `ToolRetriever`（DashScope + LRU），
 * 导致「检索」和「向量化」糊在一起，离线跑不了、也换不了模型。这里拆开：
 *
 * - `IEmbeddingProvider`：只负责「文本 → 定长 float 向量」，不关心存取；
 * - `HashEmbeddingProvider`：**离线可用**的特征哈希 n-gram 编码，
 *   确定性、零网络依赖，作为默认实现与单测基线；
 * - `HttpEmbeddingProvider`：OpenAI 兼容 `POST {base_url}/embeddings`，
 *   走与 LLM 客户端同一条 HTTP 通路（http 用 Go2Cpp，https 用 curl）。
 *
 * 约定：所有 provider 输出的向量**必须同维度**；混用不同 provider 的索引
 * 没有意义，`VectorIndex::Search` 遇到维度不一致的条目会跳过并计数。
 */

#pragma once

#include <cstddef>
#include <list>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace go2cpp::mcp::vector {

/// 一个向量。用 float 而不是 double：余弦相似度对精度不敏感，省一半内存。
using Embedding = std::vector<float>;

/// 文本编码器接口。
class IEmbeddingProvider {
public:
    virtual ~IEmbeddingProvider() = default;

    /// 提供者名字，写进检索元数据与日志。
    virtual std::string Name() const = 0;

    /// 输出向量维度；同一实例恒定。
    virtual std::size_t Dimension() const = 0;

    /**
     * @brief 把一段文本编码为向量。
     * @param text 输入文本（UTF-8）。
     * @param out 输出向量；失败时内容未定义。
     * @param error 失败时写入中文诊断，可为空。
     */
    virtual bool Embed(std::string_view text, Embedding& out, std::string& error) = 0;

    /// 批量编码的默认实现：逐个调用 `Embed`，任一失败即整体失败。
    virtual bool EmbedBatch(const std::vector<std::string>& texts, std::vector<Embedding>& out,
                            std::string& error);
};

/**
 * @brief 特征哈希 n-gram 编码器（离线默认实现）。
 *
 * 算法：对文本按 UTF-8 码点切分，取 unigram + bigram（中英文都适用），
 * 每个特征用 FNV-1a 64 位哈希散列到 `dimension` 个桶之一，并按哈希最高位
 * 决定 +1/-1（signed hashing，降低碰撞偏置），最后做 L2 归一化。
 *
 * 这不是语义模型，**只保证「字面相近的文本向量相近」**：用于工具名/描述的
 * 近义词与关键词检索足够，也是 CI 里不依赖网络的确定性基线。
 */
class HashEmbeddingProvider final : public IEmbeddingProvider {
public:
    /// @param dimension 向量维度，建议 256/512/1024；小于 8 会被钳到 8。
    explicit HashEmbeddingProvider(std::size_t dimension = 512);

    std::string Name() const override { return "hash-ngram"; }
    std::size_t Dimension() const override { return m_dimension; }
    bool Embed(std::string_view text, Embedding& out, std::string& error) override;

private:
    std::size_t m_dimension;
};

/**
 * @brief OpenAI 兼容的 `/embeddings` 客户端。
 *
 * @details 端点、模型、key、超时都从调用方传入（应用层从配置层取），
 * 本类不读全局状态。响应解析只取 `data[0].embedding`，维度以服务端返回为准；
 * 首次调用后缓存维度，后续不一致直接报错。
 *
 * 注意：DeepSeek 当前不提供 embeddings 接口，默认实现是
 * `HashEmbeddingProvider`；本类用于接入其他兼容端点或本地网关。
 */
class HttpEmbeddingProvider final : public IEmbeddingProvider {
public:
    struct Options {
        std::string base_url = "https://api.openai.com/v1";  ///< 不含 /embeddings
        std::string model = "text-embedding-3-small";        ///< 模型名
        std::string api_key;                                 ///< Bearer token
        int timeout_ms = 30000;                              ///< 单次超时
        std::size_t dimension = 0;                           ///< 0 = 以服务端首次返回为准
    };

    explicit HttpEmbeddingProvider(Options options);

    std::string Name() const override { return "http:" + m_options.model; }
    std::size_t Dimension() const override { return m_dimension; }
    bool Embed(std::string_view text, Embedding& out, std::string& error) override;

private:
    Options m_options;
    std::size_t m_dimension = 0;  ///< 首次成功响应后确定
};

/// 余弦相似度；任一向量为零向量或维度不一致时返回 0。
float CosineSimilarity(const Embedding& left, const Embedding& right);

/// 原地 L2 归一化；零向量保持不变。
void Normalize(Embedding& value);

/// 欧氏范数。
float Norm(const Embedding& value);

/**
 * @brief 线程安全的 LRU 向量缓存。
 *
 * 为什么需要：工具清单不变、查询高频重复时，重复调用远程 embedding 既慢又花钱。
 * key 是原始文本，value 是编码结果；命中率通过 `Stats()` 暴露给健康检查。
 */
class EmbeddingCache {
public:
    struct Stats {
        std::size_t hits = 0;      ///< 命中次数
        std::size_t misses = 0;    ///< 未命中次数
        std::size_t entries = 0;   ///< 当前条目数
    };

    /// @param capacity 最大条目数，0 会被钳到 1。
    explicit EmbeddingCache(std::size_t capacity = 256);

    /// 查询；未命中返回 false（不创建条目）。
    bool Get(const std::string& key, Embedding& out);

    /// 写入（已存在则刷新为最新）。
    void Put(const std::string& key, Embedding value);

    void Clear();
    Stats Snapshot() const;
    std::size_t capacity() const { return m_capacity; }

private:
    struct Entry {
        Embedding value;
        std::list<std::string>::iterator position;
    };

    std::size_t m_capacity;
    mutable std::mutex m_mutex;
    std::unordered_map<std::string, Entry> m_entries;
    std::list<std::string> m_lru;  ///< 头部最新，尾部最旧
    std::size_t m_hits = 0;
    std::size_t m_misses = 0;
};

}  // namespace go2cpp::mcp::vector
