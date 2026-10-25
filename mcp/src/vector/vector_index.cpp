/**
 * @file vector_index.cpp
 * @brief 暴力余弦索引实现与文本序列化。
 */

#include "mcp/vector/vector_index.hpp"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace go2cpp::mcp::vector {

void VectorIndex::Add(const std::string& id, Embedding embedding) {
    std::unique_lock<std::shared_mutex> lock(m_mutex);
    m_entries[id] = std::move(embedding);
}

bool VectorIndex::Remove(const std::string& id) {
    std::unique_lock<std::shared_mutex> lock(m_mutex);
    return m_entries.erase(id) > 0;
}

bool VectorIndex::Contains(const std::string& id) const {
    std::shared_lock<std::shared_mutex> lock(m_mutex);
    return m_entries.find(id) != m_entries.end();
}

bool VectorIndex::Get(const std::string& id, Embedding& out) const {
    std::shared_lock<std::shared_mutex> lock(m_mutex);
    const auto found = m_entries.find(id);
    if (found == m_entries.end()) {
        return false;
    }
    out = found->second;
    return true;
}

std::size_t VectorIndex::size() const {
    std::shared_lock<std::shared_mutex> lock(m_mutex);
    return m_entries.size();
}

void VectorIndex::Clear() {
    std::unique_lock<std::shared_mutex> lock(m_mutex);
    m_entries.clear();
}

std::size_t VectorIndex::Dimension() const {
    std::shared_lock<std::shared_mutex> lock(m_mutex);
    if (m_entries.empty()) {
        return 0;
    }
    return m_entries.begin()->second.size();
}

std::vector<SearchHit> VectorIndex::Search(const Embedding& query, std::size_t top_k,
                                           float min_score) const {
    std::vector<SearchHit> hits;
    if (query.empty() || top_k == 0) {
        return hits;
    }
    std::shared_lock<std::shared_mutex> lock(m_mutex);
    hits.reserve(m_entries.size());
    for (const auto& [id, embedding] : m_entries) {
        // 维度不一致说明索引里混入了别的 provider 的向量：跳过而不是按 0 分
        // 参与排序，否则会污染 top-k（文档承诺「跳过并计数」）。
        if (embedding.size() != query.size()) {
            continue;
        }
        const float score = CosineSimilarity(query, embedding);
        if (score < min_score) {
            continue;
        }
        hits.push_back(SearchHit{id, score});
    }
    const auto by_score = [](const SearchHit& left, const SearchHit& right) {
        return left.score > right.score;
    };
    if (hits.size() > top_k) {
        std::partial_sort(hits.begin(), hits.begin() + static_cast<std::ptrdiff_t>(top_k), hits.end(),
                          by_score);
        hits.resize(top_k);
    } else {
        std::sort(hits.begin(), hits.end(), by_score);
    }
    return hits;
}

bool VectorIndex::Save(const std::string& path, std::string* error) const {
    if (path.empty()) {
        if (error != nullptr) {
            *error = "vector index 路径为空";
        }
        return false;
    }
    // 先在锁内做一份快照，文件 IO 放在锁外，避免长写阻塞检索。
    std::vector<std::pair<std::string, Embedding>> snapshot;
    {
        std::shared_lock<std::shared_mutex> lock(m_mutex);
        snapshot.reserve(m_entries.size());
        for (const auto& item : m_entries) {
            snapshot.emplace_back(item.first, item.second);
        }
    }

    const std::string temporary = path + ".tmp";
    std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
    if (!output) {
        if (error != nullptr) {
            *error = "无法写入 " + temporary;
        }
        return false;
    }
    output << "MCPVEC1 " << snapshot.size() << '\n';
    for (const auto& [id, embedding] : snapshot) {
        output << id << '\t';
        for (std::size_t index = 0; index < embedding.size(); ++index) {
            if (index != 0) {
                output << ',';
            }
            output << embedding[index];
        }
        output << '\n';
    }
    output.flush();
    if (!output) {
        if (error != nullptr) {
            *error = "写入 " + temporary + " 失败";
        }
        std::remove(temporary.c_str());
        return false;
    }
    output.close();
    if (std::rename(temporary.c_str(), path.c_str()) != 0) {
        if (error != nullptr) {
            *error = "rename " + temporary + " -> " + path + " 失败";
        }
        std::remove(temporary.c_str());
        return false;
    }
    return true;
}

bool VectorIndex::Load(const std::string& path, std::string* error) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        if (error != nullptr) {
            *error = "无法打开 " + path;
        }
        return false;
    }

    std::string header;
    if (!std::getline(input, header) || header.rfind("MCPVEC1 ", 0) != 0) {
        if (error != nullptr) {
            *error = path + " 不是 MCPVEC1 索引";
        }
        return false;
    }

    std::unordered_map<std::string, Embedding> parsed;
    std::string line;
    while (std::getline(input, line)) {
        if (line.empty()) {
            continue;
        }
        const std::size_t tab = line.find('\t');
        if (tab == std::string::npos || tab == 0) {
            if (error != nullptr) {
                *error = path + " 行格式错误: " + line.substr(0, 80);
            }
            return false;
        }
        const std::string id = line.substr(0, tab);
        Embedding embedding;
        std::string_view values(line.data() + tab + 1, line.size() - tab - 1);
        std::size_t start = 0;
        while (start <= values.size()) {
            const std::size_t comma = values.find(',', start);
            const std::size_t end = comma == std::string_view::npos ? values.size() : comma;
            const std::string token(values.substr(start, end - start));
            try {
                embedding.push_back(std::stof(token));
            } catch (const std::exception&) {
                if (error != nullptr) {
                    *error = path + " 含非法数值: " + token;
                }
                return false;
            }
            if (comma == std::string_view::npos) {
                break;
            }
            start = comma + 1;
        }
        parsed.emplace(id, std::move(embedding));
    }

    {
        std::unique_lock<std::shared_mutex> lock(m_mutex);
        m_entries = std::move(parsed);
    }
    return true;
}

std::vector<std::string> VectorIndex::Ids() const {
    std::shared_lock<std::shared_mutex> lock(m_mutex);
    std::vector<std::string> ids;
    ids.reserve(m_entries.size());
    for (const auto& item : m_entries) {
        ids.push_back(item.first);
    }
    return ids;
}

}  // namespace go2cpp::mcp::vector
