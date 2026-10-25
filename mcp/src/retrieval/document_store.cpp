/**
 * @file document_store.cpp
 * @brief 文档分块、增量向量化与检索实现。
 */

#include "mcp/retrieval/document_store.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <utility>

namespace go2cpp::mcp::retrieval {

namespace {

/// 把字节位置对齐到 UTF-8 字符起点：落在续字节(0x80..0xBF)上就向后找。
/// 不这样做会切出非法 UTF-8，nlohmann::json 编码时直接抛异常。
std::size_t AlignToCharacterBoundary(const std::string& text, std::size_t position) {
    if (position >= text.size()) {
        return text.size();
    }
    while (position < text.size() &&
           (static_cast<unsigned char>(text[position]) & 0xC0U) == 0x80U) {
        ++position;
    }
    return position;
}

/// 在 [target/2, target] 里找最后一个换行或空格，找不到就硬切。
std::size_t FindBreak(const std::string& text, std::size_t begin, std::size_t target) {
    if (target >= text.size()) {
        return text.size();
    }
    const std::size_t lower_bound = begin + (target - begin) / 2;
    for (std::size_t cursor = target; cursor > lower_bound; --cursor) {
        const char character = text[cursor - 1];
        if (character == '\n' || character == ' ' || character == '\t') {
            return cursor;
        }
    }
    return target;
}

}  // namespace

DocumentStore::DocumentStore(vector::IEmbeddingProvider& provider, ChunkOptions options)
    : m_provider(provider), m_options(options) {
    if (m_options.chunk_size < 16) {
        m_options.chunk_size = 16;
    }
    if (m_options.overlap >= m_options.chunk_size) {
        m_options.overlap = m_options.chunk_size / 4;
    }
}

void DocumentStore::EraseUriLocked(const std::string& uri) {
    for (auto it = m_chunks.begin(); it != m_chunks.end();) {
        if (it->second.uri == uri) {
            m_index.Remove(it->first);
            it = m_chunks.erase(it);
        } else {
            ++it;
        }
    }
}

bool DocumentStore::AddDocument(const std::string& uri, const std::string& text,
                                std::string& error) {
    if (uri.empty()) {
        error = "文档 uri 为空";
        return false;
    }
    if (text.empty()) {
        // 空文档等于「内容被清空」：删掉旧块，避免留下检索不到的陈旧片段。
        std::lock_guard<std::mutex> lock(m_mutex);
        EraseUriLocked(uri);
        m_uris.erase(uri);
        m_last_error = "ok";
        return true;
    }

    // 1) 快照旧块：文本 -> 向量。内容没变的块直接复用，不重复调用 embedding。
    //    快照只在锁内做一次，之后分块与向量化都在锁外，避免阻塞检索。
    std::unordered_map<std::string, vector::Embedding> reusable;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (const auto& item : m_chunks) {
            if (item.second.uri != uri) {
                continue;
            }
            vector::Embedding embedding;
            if (m_index.Get(item.first, embedding)) {
                reusable.emplace(item.second.text, std::move(embedding));
            }
        }
    }

    struct PendingChunk {
        std::string id;
        std::string text;
        vector::Embedding embedding;
    };
    std::vector<PendingChunk> pending;
    std::size_t computed = 0;
    std::size_t reused = 0;

    // 2) 分块：与全量构建使用同一套断点规则，保证增量与全量切出相同的块。
    std::size_t begin = 0;
    std::size_t index = 0;
    while (begin < text.size()) {
        const std::size_t start = begin;
        const std::size_t end = AlignToCharacterBoundary(
            text, FindBreak(text, begin, begin + m_options.chunk_size));
        if (end <= start) {
            break;  // 防御：chunk_size 过小或已到末尾
        }
        std::string piece = text.substr(begin, end - begin);
        // 去掉纯粹的空白块，避免污染索引。
        const bool blank = std::all_of(piece.begin(), piece.end(), [](char character) {
            return character == ' ' || character == '\n' || character == '\r' || character == '\t';
        });
        if (!blank) {
            PendingChunk chunk;
            chunk.id = uri + "#" + std::to_string(index);
            chunk.text = std::move(piece);
            const auto found = reusable.find(chunk.text);
            if (found != reusable.end()) {
                chunk.embedding = found->second;
                ++reused;
            } else if (!m_provider.Embed(chunk.text, chunk.embedding, error)) {
                // 失败时现有内容保持不变：所有新块都成功后才会提交。
                std::lock_guard<std::mutex> lock(m_mutex);
                m_last_error = error;
                return false;
            } else {
                ++computed;
            }
            pending.push_back(std::move(chunk));
        }
        ++index;
        if (end >= text.size()) {
            break;
        }
        // 回退 overlap 个字符，保证跨块语义不完全割裂；回退点同样要对齐，
        // 否则下一块的起点会落在汉字中间。对齐后若没有前进，就强制从 end 继续。
        if (end - start > m_options.overlap) {
            begin = AlignToCharacterBoundary(text, end - m_options.overlap);
        } else {
            begin = end;
        }
        if (begin <= start) {
            begin = end;
        }
    }

    // 3) 原子替换该 uri 的块与向量。
    std::lock_guard<std::mutex> lock(m_mutex);
    EraseUriLocked(uri);
    for (auto& chunk : pending) {
        m_index.Add(chunk.id, std::move(chunk.embedding));
        m_chunks.emplace(chunk.id, DocumentChunk{chunk.id, uri, chunk.text});
    }
    m_uris.insert(uri);
    m_embedded += computed;
    m_reused += reused;
    m_last_error = "ok";
    return true;
}

bool DocumentStore::RemoveDocument(const std::string& uri) {
    if (uri.empty()) {
        return false;
    }
    std::lock_guard<std::mutex> lock(m_mutex);
    const bool known = m_uris.erase(uri) > 0;
    EraseUriLocked(uri);
    if (!known) {
        return false;
    }
    m_last_error = "ok";
    return true;
}

std::size_t DocumentStore::AddDirectory(const std::string& directory, std::string& error) {
    namespace fs = std::filesystem;
    std::error_code code;
    if (!fs::is_directory(directory, code)) {
        error = directory + " 不是目录";
        return 0;
    }
    std::size_t loaded = 0;
    for (const auto& entry : fs::recursive_directory_iterator(directory, code)) {
        if (!entry.is_regular_file(code)) {
            continue;
        }
        const std::string extension = entry.path().extension().string();
        if (extension != ".txt" && extension != ".md" && extension != ".markdown") {
            continue;
        }
        std::ifstream input(entry.path(), std::ios::binary);
        if (!input) {
            continue;
        }
        std::ostringstream buffer;
        buffer << input.rdbuf();
        // 用相对路径构造稳定 URI（document:///life-choices.txt），而不是
        // 暴露宿主机绝对路径：检索结果里的 uri 应该能被 resources/read 复用。
        const std::filesystem::path relative = fs::relative(entry.path(), directory, code);
        const std::string uri = "document:///" + relative.generic_string();
        std::string local_error;
        if (!AddDocument(uri, buffer.str(), local_error)) {
            error = local_error;
            return loaded;
        }
        ++loaded;
    }
    return loaded;
}

std::vector<vector::SearchHit> DocumentStore::Search(const std::string& query, std::size_t top_k,
                                                     float min_score) {
    vector::Embedding embedding;
    std::string error;
    if (!m_provider.Embed(query, embedding, error)) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_last_error = "查询向量化失败: " + error;
        return {};
    }
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_index.Search(embedding, top_k, min_score);
}

std::optional<DocumentChunk> DocumentStore::Get(const std::string& id) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    const auto found = m_chunks.find(id);
    if (found == m_chunks.end()) {
        return std::nullopt;
    }
    return found->second;
}

std::size_t DocumentStore::size() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_chunks.size();
}

std::size_t DocumentStore::document_count() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_uris.size();
}

DocumentStore::Stats DocumentStore::Snapshot() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    Stats stats;
    stats.documents = m_uris.size();
    stats.chunks = m_chunks.size();
    stats.embedded = m_embedded;
    stats.reused = m_reused;
    return stats;
}

void DocumentStore::Clear() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_index.Clear();
    m_chunks.clear();
    m_uris.clear();
}

std::string DocumentStore::LastError() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_last_error;
}

}  // namespace go2cpp::mcp::retrieval
