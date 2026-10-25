/**
 * @file embedding.cpp
 * @brief 向量层实现：哈希编码、HTTP 编码、余弦与 LRU 缓存。
 */

#include "mcp/vector/embedding.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <list>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "mcp/net/http_post.hpp"
#include "nlohmann/json.hpp"

namespace go2cpp::mcp::vector {

namespace {

/// FNV-1a 64 位。选它而不是 std::hash：跨平台/跨编译器的结果稳定，
/// 索引才能被持久化后原样复现。
std::uint64_t Fnv1a(std::string_view data, std::uint64_t seed) {
    std::uint64_t hash = 1469598103934665603ULL ^ seed;
    for (unsigned char byte : data) {
        hash ^= static_cast<std::uint64_t>(byte);
        hash *= 1099511628211ULL;
    }
    return hash;
}

/// 把 UTF-8 文本解码成码点序列；非法字节按单字节码点处理（不抛异常）。
std::vector<std::uint32_t> DecodeUtf8(std::string_view text) {
    std::vector<std::uint32_t> codepoints;
    codepoints.reserve(text.size());
    std::size_t index = 0;
    while (index < text.size()) {
        const unsigned char lead = static_cast<unsigned char>(text[index]);
        std::size_t length = 1;
        std::uint32_t value = lead;
        if ((lead & 0xE0) == 0xC0) {
            length = 2;
            value = lead & 0x1F;
        } else if ((lead & 0xF0) == 0xE0) {
            length = 3;
            value = lead & 0x0F;
        } else if ((lead & 0xF8) == 0xF0) {
            length = 4;
            value = lead & 0x07;
        }
        if (length > 1 && index + length <= text.size()) {
            bool valid = true;
            for (std::size_t offset = 1; offset < length; ++offset) {
                const unsigned char next = static_cast<unsigned char>(text[index + offset]);
                if ((next & 0xC0) != 0x80) {
                    valid = false;
                    break;
                }
                value = (value << 6) | (next & 0x3F);
            }
            if (valid) {
                index += length;
                codepoints.push_back(value);
                continue;
            }
        }
        codepoints.push_back(lead);
        ++index;
    }
    return codepoints;
}

/// 把一个特征累加进向量：signed hashing 降低碰撞带来的系统性偏置。
void AccumulateFeature(std::uint64_t hash, std::vector<float>& accumulator) {
    const std::size_t bucket = static_cast<std::size_t>(hash % accumulator.size());
    const float sign = (hash >> 63U) != 0 ? -1.0F : 1.0F;
    accumulator[bucket] += sign;
}

}  // namespace

bool IEmbeddingProvider::EmbedBatch(const std::vector<std::string>& texts,
                                    std::vector<Embedding>& out, std::string& error) {
    out.clear();
    out.reserve(texts.size());
    for (const auto& text : texts) {
        Embedding value;
        if (!Embed(text, value, error)) {
            out.clear();
            return false;
        }
        out.push_back(std::move(value));
    }
    return true;
}

HashEmbeddingProvider::HashEmbeddingProvider(std::size_t dimension)
    : m_dimension(std::max<std::size_t>(dimension, 8)) {}

bool HashEmbeddingProvider::Embed(std::string_view text, Embedding& out, std::string& error) {
    (void)error;
    out.assign(m_dimension, 0.0F);
    const auto codepoints = DecodeUtf8(text);
    if (codepoints.empty()) {
        return true;  // 空文本 → 零向量，检索分数为 0，不视为错误
    }

    constexpr std::uint64_t kSeedUnigram = 0x9E3779B97F4A7C15ULL;
    constexpr std::uint64_t kSeedBigram = 0xC2B2AE3D27D4EB4FULL;

    for (std::size_t index = 0; index < codepoints.size(); ++index) {
        // unigram：单字符特征，保证长文本与短查询都能命中。
        char buffer[8];
        const int written = std::snprintf(buffer, sizeof(buffer), "u%u",
                                          static_cast<unsigned>(codepoints[index]));
        AccumulateFeature(Fnv1a(std::string_view(buffer, static_cast<std::size_t>(written)),
                                kSeedUnigram),
                          out);
        // bigram：相邻两字符，弥补 unigram 丢失的语序信息。
        if (index + 1 < codepoints.size()) {
            const int pair_written =
                std::snprintf(buffer, sizeof(buffer), "b%u,%u",
                              static_cast<unsigned>(codepoints[index]),
                              static_cast<unsigned>(codepoints[index + 1]));
            AccumulateFeature(Fnv1a(std::string_view(buffer, static_cast<std::size_t>(pair_written)),
                                    kSeedBigram),
                              out);
        }
    }
    Normalize(out);
    return true;
}

HttpEmbeddingProvider::HttpEmbeddingProvider(Options options)
    : m_options(std::move(options)), m_dimension(options.dimension) {}

bool HttpEmbeddingProvider::Embed(std::string_view text, Embedding& out, std::string& error) {
    if (m_options.base_url.empty()) {
        error = "embedding base_url 为空";
        return false;
    }
    if (m_options.api_key.empty()) {
        error = "缺少 embedding API key";
        return false;
    }

    nlohmann::json body = nlohmann::json::object();
    body["model"] = m_options.model;
    body["input"] = std::string(text);

    net::HttpPostOptions post;
    post.api_key = m_options.api_key;
    post.timeout = std::chrono::milliseconds(m_options.timeout_ms);

    std::string response_body;
    std::string post_error;
    const std::string url = m_options.base_url + "/embeddings";
    if (!net::HttpPostJson(url, body.dump(), post, response_body, post_error)) {
        error = std::move(post_error);
        return false;
    }

    nlohmann::json document;
    try {
        document = nlohmann::json::parse(response_body);
    } catch (const nlohmann::json::exception& exception) {
        error = std::string("embedding 响应不是合法 JSON: ") + exception.what();
        return false;
    }

    const auto data = document.find("data");
    if (data == document.end() || !data->is_array() || data->empty()) {
        error = "embedding 响应缺少 data[0]: " + response_body.substr(0, 200);
        return false;
    }
    const auto values = data->front().find("embedding");
    if (values == data->front().end() || !values->is_array()) {
        error = "embedding 响应缺少 data[0].embedding";
        return false;
    }

    Embedding parsed;
    parsed.reserve(values->size());
    for (const auto& item : *values) {
        if (!item.is_number()) {
            error = "embedding 向量含非数值元素";
            return false;
        }
        parsed.push_back(item.get<float>());
    }
    if (m_dimension == 0) {
        m_dimension = parsed.size();
    } else if (parsed.size() != m_dimension) {
        error = "embedding 维度不稳定: 期望 " + std::to_string(m_dimension) + " 实际 " +
                std::to_string(parsed.size());
        return false;
    }
    Normalize(parsed);
    out = std::move(parsed);
    return true;
}

float CosineSimilarity(const Embedding& left, const Embedding& right) {
    if (left.size() != right.size() || left.empty()) {
        return 0.0F;
    }
    double dot = 0.0;
    double left_norm = 0.0;
    double right_norm = 0.0;
    for (std::size_t index = 0; index < left.size(); ++index) {
        dot += static_cast<double>(left[index]) * static_cast<double>(right[index]);
        left_norm += static_cast<double>(left[index]) * static_cast<double>(left[index]);
        right_norm += static_cast<double>(right[index]) * static_cast<double>(right[index]);
    }
    if (left_norm <= 0.0 || right_norm <= 0.0) {
        return 0.0F;
    }
    return static_cast<float>(dot / (std::sqrt(left_norm) * std::sqrt(right_norm)));
}

void Normalize(Embedding& value) {
    const float length = Norm(value);
    if (length <= 0.0F) {
        return;
    }
    for (float& element : value) {
        element /= length;
    }
}

float Norm(const Embedding& value) {
    double sum = 0.0;
    for (float element : value) {
        sum += static_cast<double>(element) * static_cast<double>(element);
    }
    return static_cast<float>(std::sqrt(sum));
}

EmbeddingCache::EmbeddingCache(std::size_t capacity) : m_capacity(std::max<std::size_t>(capacity, 1)) {}

bool EmbeddingCache::Get(const std::string& key, Embedding& out) {
    std::lock_guard<std::mutex> lock(m_mutex);
    const auto found = m_entries.find(key);
    if (found == m_entries.end()) {
        ++m_misses;
        return false;
    }
    ++m_hits;
    m_lru.splice(m_lru.begin(), m_lru, found->second.position);
    out = found->second.value;
    return true;
}

void EmbeddingCache::Put(const std::string& key, Embedding value) {
    std::lock_guard<std::mutex> lock(m_mutex);
    const auto found = m_entries.find(key);
    if (found != m_entries.end()) {
        found->second.value = std::move(value);
        m_lru.splice(m_lru.begin(), m_lru, found->second.position);
        return;
    }
    m_lru.push_front(key);
    m_entries.emplace(key, Entry{std::move(value), m_lru.begin()});
    while (m_entries.size() > m_capacity) {
        const std::string& victim = m_lru.back();
        m_entries.erase(victim);
        m_lru.pop_back();
    }
}

void EmbeddingCache::Clear() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_entries.clear();
    m_lru.clear();
    m_hits = 0;
    m_misses = 0;
}

EmbeddingCache::Stats EmbeddingCache::Snapshot() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    Stats stats;
    stats.hits = m_hits;
    stats.misses = m_misses;
    stats.entries = m_entries.size();
    return stats;
}

}  // namespace go2cpp::mcp::vector
